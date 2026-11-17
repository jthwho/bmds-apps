/**
 * @file      rfb.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * RFB's server side (RFB 3.8, the IETF's RFC 6143): the handshake, the viewer's messages and the
 * server's, as bytes in and bytes out.
 */

#include "rfb.h"

#include "text.h"

#include <cstring>

namespace vnc {

        namespace {

                constexpr size_t  VersionBytes = 12;
                constexpr uint8_t SecurityNone = 1;
                constexpr uint8_t SecurityVnc = 2;
                constexpr uint8_t  SecurityVencrypt = 19; // VeNCrypt: TLS, then a subtype's authentication
                constexpr uint32_t VencryptX509Vnc = 261; // TLS with the server's certificate, then VNC's

                // The viewer's messages (RFB 3.8 §7.5).
                constexpr uint8_t SetPixelFormat = 0;
                constexpr uint8_t SetEncodings = 2;
                constexpr uint8_t UpdateRequest = 3;
                constexpr uint8_t KeyEvent = 4;
                constexpr uint8_t PointerEvent = 5;
                constexpr uint8_t ClientCutText = 6;

                constexpr size_t MaxEncodings = 64; // a viewer's list, at most this many kept

        } // namespace

        Session::Session(const Config &config, Handler &handler) : _config(config), _handler(handler) {}

        void Session::start(Writer &out) {
                out.bytes("RFB 003.008\n", VersionBytes);
                return;
        }

        void Session::close(const char *reason) {
                _state = State::Closed;
                _reason = reason;
                return;
        }

        size_t Session::feed(const uint8_t *data, size_t length, Writer &out) {
                size_t taken = 0;
                while (taken < length && _state != State::Closed) {
                        size_t n = 0;
                        switch (_state) {
                                case State::Version: n = feedVersion(data + taken, length - taken, out); break;
                                case State::Security: n = feedSecurity(data + taken, length - taken, out); break;
                                case State::VencryptVersion:
                                        n = feedVencryptVersion(data + taken, length - taken, out);
                                        break;
                                case State::VencryptSubtype:
                                        n = feedVencryptSubtype(data + taken, length - taken, out);
                                        break;
                                case State::Tls: break; // nothing until TLS is up (the viewer sends its hello to it)
                                case State::Challenge: n = feedChallenge(data + taken, length - taken, out); break;
                                case State::Init: n = feedInit(data + taken, length - taken, out); break;
                                case State::Normal: n = feedMessage(data + taken, length - taken); break;
                                case State::Closed: break;
                        }
                        if (n == 0) break; // an unfinished message
                        taken += n;
                }
                return taken;
        }

        // "RFB 003.00x\n": 3.3 (and 3.5, as 3.3), 3.7, 3.8 (and anything later, as 3.8).
        size_t Session::feedVersion(const uint8_t *data, size_t length, Writer &out) {
                if (length < VersionBytes) return 0;
                if (std::memcmp(data, "RFB 003.", 8) != 0 || data[11] != '\n') {
                        close("not a VNC viewer");
                        return VersionBytes;
                }
                int minor = 0;
                for (int i = 8; i < 11; ++i) {
                        if (data[i] < '0' || data[i] > '9') {
                                close("not a VNC viewer");
                                return VersionBytes;
                        }
                        minor = minor * 10 + (data[i] - '0');
                }
                _minor = minor >= 8 ? 8 : minor == 7 ? 7 : 3;
                if (_config.vencrypt && _minor == 3) {
                        // 3.3 has the server choose a type: VeNCrypt needs the viewer's choice.
                        out.u32(0);
                        const char *why = "this server needs VeNCrypt (TLS): a viewer speaking RFB 3.7 or later";
                        out.u32(uint32_t(std::strlen(why)));
                        out.bytes(why, std::strlen(why));
                        close("an RFB 3.3 viewer (no VeNCrypt)");
                        return VersionBytes;
                }
                const uint8_t type = _config.vencrypt              ? SecurityVencrypt
                                     : _config.password != nullptr ? SecurityVnc
                                                                   : SecurityNone;
                if (_minor == 3) {
                        // 3.3: the server chooses.
                        out.u32(type);
                        if (type == SecurityVnc) {
                                sendChallenge(out);
                                _state = State::Challenge;
                        } else {
                                _state = State::Init;
                        }
                        return VersionBytes;
                }
                out.u8(1);
                out.u8(type);
                _state = State::Security;
                return VersionBytes;
        }

        size_t Session::feedSecurity(const uint8_t *data, size_t length, Writer &out) {
                if (length < 1) return 0;
                const uint8_t want = _config.vencrypt              ? SecurityVencrypt
                                     : _config.password != nullptr ? SecurityVnc
                                                                   : SecurityNone;
                if (data[0] != want) {
                        securityResult(out, false, "that security type isn't offered");
                        close("the viewer chose a security type not offered");
                        return 1;
                }
                if (want == SecurityVencrypt) {
                        out.u8(0); // VeNCrypt 0.2
                        out.u8(2);
                        _state = State::VencryptVersion;
                        return 1;
                }
                if (want == SecurityVnc) {
                        sendChallenge(out);
                        _state = State::Challenge;
                        return 1;
                }
                if (_minor >= 8) securityResult(out, true, nullptr); // 3.7 sends none for None
                _state = State::Init;
                return 1;
        }

        // VeNCrypt (its 0.2): the viewer's version, then the subtypes (X509Vnc alone), its choice
        // acknowledged, and TLS: the caller's (`tlsStarted()`).
        size_t Session::feedVencryptVersion(const uint8_t *data, size_t length, Writer &out) {
                if (length < 2) return 0;
                if (data[0] != 0 || data[1] != 2) {
                        out.u8(1); // not supported
                        close("a VeNCrypt version other than 0.2");
                        return 2;
                }
                out.u8(0);
                out.u8(1);
                out.u32(VencryptX509Vnc);
                _state = State::VencryptSubtype;
                return 2;
        }

        size_t Session::feedVencryptSubtype(const uint8_t *data, size_t length, Writer &out) {
                if (length < 4) return 0;
                if (readU32(data) != VencryptX509Vnc) {
                        out.u8(0);
                        close("the viewer chose a VeNCrypt subtype not offered");
                        return 4;
                }
                out.u8(1);
                _state = State::Tls;
                return 4;
        }

        void Session::tlsStarted(Writer &out) {
                if (_state != State::Tls) return;
                sendChallenge(out);
                _state = State::Challenge;
                return;
        }

        void Session::sendChallenge(Writer &out) {
                if (_config.random != nullptr) _config.random(_config.randomArg, _challenge, ChallengeBytes);
                out.bytes(_challenge, ChallengeBytes);
                return;
        }

        size_t Session::feedChallenge(const uint8_t *data, size_t length, Writer &out) {
                if (length < ChallengeBytes) return 0;
                uint8_t want[ChallengeBytes];
                vncResponse(_config.password, _challenge, want);
                const bool ok = sameBytes(want, data, ChallengeBytes);
                std::memset(want, 0, sizeof(want));
                securityResult(out, ok, "wrong password");
                if (!ok) {
                        _authFailed = true;
                        close("wrong password");
                        return ChallengeBytes;
                }
                _state = State::Init;
                return ChallengeBytes;
        }

        // SecurityResult: 0 ok, 1 failed (3.8: with a reason).
        void Session::securityResult(Writer &out, bool ok, const char *reason) {
                out.u32(ok ? 0 : 1);
                if (!ok && _minor >= 8) {
                        const size_t n = std::strlen(reason);
                        out.u32(uint32_t(n));
                        out.bytes(reason, n);
                }
                return;
        }

        // ClientInit (its shared flag: a second viewer is welcome either way), then ServerInit.
        size_t Session::feedInit(const uint8_t *data, size_t length, Writer &out) {
                if (length < 1) return 0;
                (void)data;
                out.u16(_config.width);
                out.u16(_config.height);
                PixelFormat().write(out);
                const size_t n = std::strlen(_config.name);
                out.u32(uint32_t(n));
                out.bytes(_config.name, n);
                _state = State::Normal;
                _handler.ready();
                return 1;
        }

        size_t Session::feedMessage(const uint8_t *data, size_t length) {
                if (_skip > 0) {
                        const size_t n = length < _skip ? length : _skip;
                        _skip -= uint32_t(n);
                        return n;
                }
                switch (data[0]) {
                        case SetPixelFormat: {
                                if (length < 20) return 0;
                                PixelFormat f;
                                if (f.parse(data + 4)) {
                                        _handler.setPixelFormat(f);
                                } else {
                                        close("a pixel format this server doesn't send (a colour map?)");
                                }
                                return 20;
                        }
                        case SetEncodings: {
                                if (length < 4) return 0;
                                const size_t count = readU16(data + 2);
                                if (length < 4 + 4 * count) return 0;
                                int32_t      list[MaxEncodings];
                                const size_t kept = count < MaxEncodings ? count : MaxEncodings;
                                for (size_t i = 0; i < kept; ++i) list[i] = int32_t(readU32(data + 4 + 4 * i));
                                _handler.setEncodings(list, kept);
                                return 4 + 4 * count;
                        }
                        case UpdateRequest:
                                if (length < 10) return 0;
                                _handler.updateRequest(data[1] != 0, readU16(data + 2), readU16(data + 4),
                                                       readU16(data + 6), readU16(data + 8));
                                return 10;
                        case KeyEvent:
                                if (length < 8) return 0;
                                _handler.key(data[1] != 0, readU32(data + 4));
                                return 8;
                        case PointerEvent:
                                if (length < 6) return 0;
                                _handler.pointer(data[1], readU16(data + 2), readU16(data + 4));
                                return 6;
                        case ClientCutText: {
                                if (length < 8) return 0;
                                const int32_t n = int32_t(readU32(data + 4));
                                if (n < 0) {
                                        // The Extended Clipboard's form (a negative length): its flags,
                                        // then its data; one too long to hold whole is skipped.
                                        const size_t m = size_t(-int64_t(n));
                                        if (m < 4 || m > MaxCutText || m > _config.maxCutText) {
                                                _skip = uint32_t(m);
                                                return 8;
                                        }
                                        if (length < 8 + m) return 0;
                                        _handler.extendedClipboard(readU32(data + 8), data + 12, m - 4);
                                        return 8 + m;
                                }
                                if (size_t(n) > MaxCutText || size_t(n) > _config.maxCutText) {
                                        _skip = uint32_t(n);
                                        return 8;
                                }
                                if (length < 8 + size_t(n)) return 0;
                                _handler.cutText(reinterpret_cast<const char *>(data + 8), size_t(n));
                                return 8 + size_t(n);
                        }
                        default:
                                // A message whose length isn't known: nothing after it can be read.
                                close("a message this server doesn't know");
                                return length;
                }
        }

        void Session::writeUpdateHeader(Writer &out, uint16_t rects) {
                out.u8(0); // FramebufferUpdate
                out.u8(0);
                out.u16(rects);
                return;
        }

        void Session::writeRectHeader(Writer &out, uint16_t x, uint16_t y, uint16_t w, uint16_t h, int32_t encoding) {
                out.u16(x);
                out.u16(y);
                out.u16(w);
                out.u16(h);
                out.s32(encoding);
                return;
        }

        void Session::writeDesktopSize(Writer &out, uint16_t width, uint16_t height) {
                writeRectHeader(out, 0, 0, width, height, Encoding::DesktopSize);
                return;
        }

        void Session::writeCopyRect(Writer &out, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t fromX,
                                    uint16_t fromY) {
                writeRectHeader(out, x, y, w, h, Encoding::CopyRect);
                out.u16(fromX);
                out.u16(fromY);
                return;
        }

        void Session::writeLastRect(Writer &out) {
                writeRectHeader(out, 0, 0, 0, 0, Encoding::LastRect);
                return;
        }

        bool Session::writeCursor(Writer &out, const PixelFormat &format, uint16_t width, uint16_t height,
                                  uint16_t hotX, uint16_t hotY, const uint32_t *argb) {
                const size_t start = out.size();
                const size_t bpp = format.bytesPerPixel();
                const size_t maskRow = (size_t(width) + 7) / 8;
                writeRectHeader(out, hotX, hotY, width, height, Encoding::Cursor);
                uint8_t *pixels = out.reserve(size_t(width) * height * bpp);
                uint8_t *mask = out.reserve(maskRow * height);
                if (out.overflowed()) {
                        out.truncate(start);
                        return false;
                }
                std::memset(mask, 0, maskRow * height);
                for (uint16_t y = 0; y < height; ++y)
                        for (uint16_t x = 0; x < width; ++x) {
                                const uint32_t p = argb[size_t(y) * width + x];
                                format.storePixel(p & 0xffffffu, pixels + (size_t(y) * width + x) * bpp);
                                if ((p >> 24) >= 0x80) mask[size_t(y) * maskRow + x / 8] |= uint8_t(0x80u >> (x % 8));
                        }
                return true;
        }

        bool Session::writeCursorWithAlpha(Writer &out, uint16_t width, uint16_t height, uint16_t hotX, uint16_t hotY,
                                           const uint32_t *argb) {
                const size_t start = out.size();
                writeRectHeader(out, hotX, hotY, width, height, Encoding::CursorWithAlpha);
                out.s32(Encoding::Raw);
                uint8_t *p = out.reserve(size_t(width) * height * 4);
                if (out.overflowed()) {
                        out.truncate(start);
                        return false;
                }
                for (size_t i = 0; i < size_t(width) * height; ++i, p += 4) {
                        const uint32_t v = argb[i], a = v >> 24;
                        p[0] = uint8_t(((v >> 16) & 0xff) * a / 255);
                        p[1] = uint8_t(((v >> 8) & 0xff) * a / 255);
                        p[2] = uint8_t((v & 0xff) * a / 255);
                        p[3] = uint8_t(a);
                }
                return true;
        }

        void Session::writeBell(Writer &out) {
                out.u8(2);
                return;
        }

        void Session::writeCutText(Writer &out, const char *text, size_t length) {
                out.u8(3);
                out.zeros(3);
                out.u32(uint32_t(length));
                out.bytes(text, length);
                return;
        }

        bool Session::writeCutTextFromUtf8(Writer &out, const char *text, size_t length) {
                // Latin-1 is never longer: made in place, the unused end given back.
                const size_t start = out.size();
                out.u8(3);
                out.zeros(3);
                out.u32(0);
                uint8_t *p = out.reserve(length);
                if (p == nullptr) {
                        out.truncate(start);
                        return false;
                }
                const size_t n = Text::latin1FromUtf8(text, length, reinterpret_cast<char *>(p), length);
                out.drop(length - n);
                uint8_t *len = out.data() + start + 4;
                len[0] = uint8_t(n >> 24);
                len[1] = uint8_t(n >> 16);
                len[2] = uint8_t(n >> 8);
                len[3] = uint8_t(n);
                return true;
        }

} // namespace vnc
