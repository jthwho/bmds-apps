/**
 * @file      rfb.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The VNC server's protocol (vnc/core): DES and VNC's challenge, the handshake in each version,
 * the viewer's messages, pixel formats and keysyms.
 */

#include "des.h"
#include "keysym.h"
#include "rfb.h"
#include "text.h"

#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

using vnc::Session;
using vnc::Writer;

namespace {

        std::vector<uint8_t> hex(const char *text) {
                std::vector<uint8_t> out;
                for (size_t i = 0; text[i] != '\0' && text[i + 1] != '\0'; i += 2)
                        out.push_back(uint8_t(std::stoi(std::string(text + i, 2), nullptr, 16)));
                return out;
        }

        // What a session was told.
        struct Recorder : vnc::Handler {
                        int                                    readies = 0;
                        vnc::PixelFormat                       format;
                        int                                    formats = 0;
                        std::vector<int32_t>                   encodings;
                        std::vector<int>                       requests; // incremental, x, y, w, h each
                        std::vector<std::pair<bool, uint32_t>> keys;
                        std::vector<int>                       pointers; // buttons, x, y each
                        std::string                            cut;

                        void ready() override { ++readies; }
                        void setPixelFormat(const vnc::PixelFormat &f) override {
                                format = f;
                                ++formats;
                        }
                        void setEncodings(const int32_t *e, size_t n) override { encodings.assign(e, e + n); }
                        void updateRequest(bool inc, uint16_t x, uint16_t y, uint16_t w, uint16_t h) override {
                                requests.insert(requests.end(), {inc ? 1 : 0, x, y, w, h});
                        }
                        void key(bool down, uint32_t keysym) override { keys.emplace_back(down, keysym); }
                        void pointer(uint8_t b, uint16_t x, uint16_t y) override {
                                pointers.insert(pointers.end(), {b, x, y});
                        }
                        void cutText(const char *t, size_t n) override { cut.assign(t, n); }
        };

        void fixedChallenge(void *, uint8_t *out, size_t n) {
                for (size_t i = 0; i < n; ++i) out[i] = uint8_t(i);
                return;
        }

        struct Fixture {
                        Recorder             handler;
                        Session::Config      config;
                        std::vector<uint8_t> outBytes = std::vector<uint8_t>(4096);
                        Writer               out{outBytes.data(), outBytes.size()};

                        explicit Fixture(const char *password = nullptr) {
                                config.name = "board";
                                config.width = 1280;
                                config.height = 720;
                                config.password = password;
                                config.random = fixedChallenge;
                        }
                        // Feeds @p bytes whole, returns what the server wrote since the last call.
                        std::vector<uint8_t> feed(Session &s, const std::vector<uint8_t> &bytes) {
                                out.clear();
                                const size_t taken = s.feed(bytes.data(), bytes.size(), out);
                                CHECK(taken == bytes.size());
                                return std::vector<uint8_t>(out.data(), out.data() + out.size());
                        }
        };

        std::vector<uint8_t> text(const char *s) {
                return std::vector<uint8_t>(s, s + std::strlen(s));
        }

        // ServerInit for a 1280 × 720 "board": size, the default pixel format, the name.
        void checkServerInit(const std::vector<uint8_t> &w) {
                REQUIRE(w.size() == 4 + 16 + 4 + 5);
                CHECK(vnc::readU16(&w[0]) == 1280);
                CHECK(vnc::readU16(&w[2]) == 720);
                CHECK(w[4] == 32);
                CHECK(w[5] == 24);
                CHECK(w[7] == 1); // true colour
                CHECK(w[14] == 16);
                CHECK(vnc::readU32(&w[20]) == 5);
                CHECK(std::string(w.begin() + 24, w.end()) == "board");
        }

} // namespace

TEST_CASE("DES: FIPS and the usual vectors, from OpenSSL") {
        struct Vector {
                        const char *key, *in, *out;
        };
        const Vector vectors[] = {
                {"133457799BBCDFF1", "0123456789ABCDEF", "85e813540f0ab405"},
                {"0000000000000000", "0000000000000000", "8ca64de9c1b123a7"},
                {"FFFFFFFFFFFFFFFF", "FFFFFFFFFFFFFFFF", "7359b2163e4edc58"},
                {"3000000000000000", "1000000000000001", "958e6e627a05557b"},
                {"0123456789ABCDEF", "1111111111111111", "17668dfc7292532d"},
                {"FEDCBA9876543210", "0123456789ABCDEF", "ed39d950fa74bcc4"},
                {"7CA110454A1A6E57", "01A1D6D039776742", "690f5b0d9a26939b"},
                {"0131D9619DC1376E", "5CD54CA83DEF57DA", "7a389d10354bd271"},
                {"07A1133E4A0B2686", "0248D43806F67172", "868ebb51cab4599a"},
                {"3849674C2602319E", "51454B582DDF440A", "7178876e01f19b2a"},
                {"04B915BA43FEB5B6", "42FD443059577FA2", "af37fb421f8c4095"},
        };
        for (const Vector &v : vectors) {
                const std::vector<uint8_t> key = hex(v.key), in = hex(v.in), want = hex(v.out);
                uint8_t                    out[8];
                vnc::desEncrypt(key.data(), in.data(), out);
                CHECK(std::vector<uint8_t>(out, out + 8) == want);
        }
}

TEST_CASE("DES: VNC's response, its key's bits reversed (checked with OpenSSL)") {
        uint8_t challenge[16], response[16];
        for (int i = 0; i < 16; ++i) challenge[i] = uint8_t(i);
        vnc::vncResponse("secret!!x", challenge, response); // only the first 8 count
        CHECK(std::vector<uint8_t>(response, response + 16) == hex("a1ce096f55fcf9a3f8a6f834df6cc386"));
}

TEST_CASE("RFB 3.8 without a password") {
        Fixture f;
        Session s(f.config, f.handler);
        s.start(f.out);
        CHECK(std::string(reinterpret_cast<char *>(f.out.data()), f.out.size()) == "RFB 003.008\n");
        CHECK(f.feed(s, text("RFB 003.008\n")) == std::vector<uint8_t>({1, 1})); // one type: None
        CHECK(f.feed(s, {1}) == std::vector<uint8_t>({0, 0, 0, 0}));             // SecurityResult ok
        checkServerInit(f.feed(s, {1}));
        CHECK(s.state() == Session::State::Normal);
        CHECK(f.handler.readies == 1);
}

TEST_CASE("RFB 3.8 with VNC authentication: right, then wrong") {
        uint8_t challenge[16], response[16];
        for (int i = 0; i < 16; ++i) challenge[i] = uint8_t(i);
        {
                Fixture f("hunter2");
                Session s(f.config, f.handler);
                CHECK(f.feed(s, text("RFB 003.008\n")) == std::vector<uint8_t>({1, 2}));
                CHECK(f.feed(s, {2}) == std::vector<uint8_t>(challenge, challenge + 16));
                vnc::vncResponse("hunter2", challenge, response);
                CHECK(f.feed(s, std::vector<uint8_t>(response, response + 16)) == std::vector<uint8_t>({0, 0, 0, 0}));
                checkServerInit(f.feed(s, {0}));
                CHECK_FALSE(s.failedAuthentication());
        }
        {
                Fixture f("hunter2");
                Session s(f.config, f.handler);
                f.feed(s, text("RFB 003.008\n"));
                f.feed(s, {2});
                vnc::vncResponse("hunter3", challenge, response);
                const std::vector<uint8_t> w = f.feed(s, std::vector<uint8_t>(response, response + 16));
                REQUIRE(w.size() > 8);
                CHECK(vnc::readU32(&w[0]) == 1);                                // failed
                CHECK(std::string(w.begin() + 8, w.end()) == "wrong password"); // 3.8: a reason
                CHECK(s.isClosed());
                CHECK(s.failedAuthentication());
        }
}

TEST_CASE("RFB 3.8 with VeNCrypt X509Vnc: the subtype, TLS, then VNC authentication") {
        uint8_t challenge[16], response[16];
        for (int i = 0; i < 16; ++i) challenge[i] = uint8_t(i);
        {
                Fixture f("hunter2");
                f.config.vencrypt = true;
                Session s(f.config, f.handler);
                CHECK(f.feed(s, text("RFB 003.008\n")) == std::vector<uint8_t>({1, 19}));
                CHECK(f.feed(s, {19}) == std::vector<uint8_t>({0, 2}));               // VeNCrypt 0.2
                CHECK(f.feed(s, {0, 2}) == std::vector<uint8_t>({0, 1, 0, 0, 1, 5})); // ok; X509Vnc (261)
                CHECK(f.feed(s, {0, 0, 1, 5}) == std::vector<uint8_t>({1}));          // accepted
                CHECK(s.state() == Session::State::Tls);
                // Nothing is taken until TLS is up (the viewer's ClientHello is TLS's).
                std::vector<uint8_t> out(64);
                Writer               w(out.data(), out.size());
                const uint8_t        hello[] = {0x16, 3, 1};
                CHECK(s.feed(hello, sizeof(hello), w) == 0);
                CHECK(w.size() == 0);
                CHECK(s.state() == Session::State::Tls);
                s.tlsStarted(w);
                CHECK(std::vector<uint8_t>(out.begin(), out.begin() + long(w.size())) ==
                      std::vector<uint8_t>(challenge, challenge + 16));
                vnc::vncResponse("hunter2", challenge, response);
                CHECK(f.feed(s, std::vector<uint8_t>(response, response + 16)) == std::vector<uint8_t>({0, 0, 0, 0}));
                checkServerInit(f.feed(s, {0}));
        }
        {
                // Plain VNC authentication refused; another subtype refused.
                Fixture f("hunter2");
                f.config.vencrypt = true;
                Session s(f.config, f.handler);
                f.feed(s, text("RFB 003.008\n"));
                f.feed(s, {2});
                CHECK(s.isClosed());
                Session t(f.config, f.handler);
                f.feed(t, text("RFB 003.008\n"));
                f.feed(t, {19});
                f.feed(t, {0, 2});
                CHECK(f.feed(t, {0, 0, 1, 4}) == std::vector<uint8_t>({0}));
                CHECK(t.isClosed());
        }
        {
                // 3.3 can't choose: refused with a reason.
                Fixture f("hunter2");
                f.config.vencrypt = true;
                Session                    s(f.config, f.handler);
                const std::vector<uint8_t> w = f.feed(s, text("RFB 003.003\n"));
                REQUIRE(w.size() > 8);
                CHECK(vnc::readU32(&w[0]) == 0);
                CHECK(s.isClosed());
        }
}

TEST_CASE("RFB 3.3 and 3.7: the server chooses, and the results' differences") {
        {
                Fixture f;
                Session s(f.config, f.handler);
                CHECK(f.feed(s, text("RFB 003.003\n")) == std::vector<uint8_t>({0, 0, 0, 1})); // None, chosen
                checkServerInit(f.feed(s, {1}));
                CHECK(s.minorVersion() == 3);
        }
        {
                Fixture                    f("pw");
                Session                    s(f.config, f.handler);
                const std::vector<uint8_t> w = f.feed(s, text("RFB 003.003\n"));
                REQUIRE(w.size() == 20); // VNC, chosen, and the challenge at once
                CHECK(vnc::readU32(&w[0]) == 2);
        }
        {
                Fixture f;
                Session s(f.config, f.handler);
                CHECK(f.feed(s, text("RFB 003.007\n")) == std::vector<uint8_t>({1, 1}));
                CHECK(f.feed(s, {1}).empty()); // 3.7 sends no SecurityResult for None
                checkServerInit(f.feed(s, {1}));
        }
        {
                Fixture f;
                Session s(f.config, f.handler);
                f.feed(s, text("RFB 003.889\n")); // Apple's: spoken as 3.8
                CHECK(s.minorVersion() == 8);
        }
        {
                Fixture                    f;
                Session                    s(f.config, f.handler);
                const std::vector<uint8_t> http = text("GET / HTTP/1.1\n");
                (void)s.feed(http.data(), http.size(), f.out); // the rest isn't read: closed
                CHECK(s.isClosed());
        }
}

TEST_CASE("RFB: the viewer's messages, whole or in pieces") {
        Fixture f;
        Session s(f.config, f.handler);
        f.feed(s, text("RFB 003.008\n"));
        f.feed(s, {1});
        f.feed(s, {1});
        std::
                vector<uint8_t>
                        m =
                                {
                                        0,  0,  0,    0,    16,   16,   0,    1,    0,    31,   0,    63,   0,   31,
                                        11, 5,  0,    0,    0,    0, // SetPixelFormat: RGB565
                                        2,  0,  0,    3,    0,    0,    0,    7,    0xff, 0xff, 0xff, 0xe0, 0,   0,
                                        0,  16, // SetEncodings: Tight, Q0, ZRLE
                                        3,  1,  0,    10,   0,    20,   1,    0,    0,    128, // an incremental request
                                        4,  1,  0,    0,    0,    0,    0xff, 0x0d,            // Return down
                                        5,  1,  0x02, 0x80, 0x01, 0x68, // the left button at 640, 360
                                        6,  0,  0,    0,    0,    0,    0,    5,    'h',  'e',  'l',  'l',  'o', // cut text
                                };
        // A byte at a time: each message waits until it is whole.
        size_t at = 0;
        for (size_t end = 1; end <= m.size(); ++end) {
                f.out.clear();
                at += s.feed(m.data() + at, end - at, f.out);
        }
        CHECK(at == m.size());
        CHECK(f.handler.formats == 1);
        CHECK(f.handler.format.bitsPerPixel == 16);
        CHECK(f.handler.format.redShift == 11);
        CHECK(f.handler.encodings == std::vector<int32_t>({7, -32, 16}));
        CHECK(f.handler.requests == std::vector<int>({1, 10, 20, 256, 128}));
        REQUIRE(f.handler.keys.size() == 1);
        CHECK(f.handler.keys[0].second == 0xff0d);
        CHECK(f.handler.pointers == std::vector<int>({1, 640, 360}));
        CHECK(f.handler.cut == "hello");
        CHECK_FALSE(s.isClosed());
        // A colour map isn't offered: asked for, the session ends.
        std::vector<uint8_t> colourMap = {0, 0, 0, 0, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        f.feed(s, colourMap);
        CHECK(s.isClosed());
}

TEST_CASE("Pixel formats: PIXEL, CPIXEL and TPIXEL") {
        vnc::PixelFormat f; // 32-bit little-endian, 0x00RRGGBB
        uint8_t          p[4];
        f.storePixel(0x123456, p);
        CHECK(std::vector<uint8_t>(p, p + 4) == std::vector<uint8_t>({0x56, 0x34, 0x12, 0}));
        CHECK(f.cpixelBytes() == 3);
        f.storeCpixel(0x123456, p);
        CHECK(std::vector<uint8_t>(p, p + 3) == std::vector<uint8_t>({0x56, 0x34, 0x12}));
        CHECK(f.isTight24());
        f.storeTpixel(0x123456, p);
        CHECK(std::vector<uint8_t>(p, p + 3) == std::vector<uint8_t>({0x12, 0x34, 0x56}));
        f.bigEndian = true;
        f.storePixel(0x123456, p);
        CHECK(std::vector<uint8_t>(p, p + 4) == std::vector<uint8_t>({0, 0x12, 0x34, 0x56}));
        f.storeCpixel(0x123456, p);
        CHECK(std::vector<uint8_t>(p, p + 3) == std::vector<uint8_t>({0x12, 0x34, 0x56}));
        // RGB565, as TigerVNC asks on a slow link.
        vnc::PixelFormat r;
        r.bitsPerPixel = 16;
        r.depth = 16;
        r.redMax = 31;
        r.greenMax = 63;
        r.blueMax = 31;
        r.redShift = 11;
        r.greenShift = 5;
        r.blueShift = 0;
        r.storePixel(0xffffff, p);
        CHECK(std::vector<uint8_t>(p, p + 2) == std::vector<uint8_t>({0xff, 0xff}));
        r.storePixel(0xff0000, p);
        CHECK(std::vector<uint8_t>(p, p + 2) == std::vector<uint8_t>({0x00, 0xf8}));
        CHECK(r.cpixelBytes() == 2);
        CHECK(r.tpixelBytes() == 2);
}

TEST_CASE("Keysyms: characters, Shift where the viewer didn't send it, modifiers held") {
        vnc::KeyState k;
        auto          a = k.event(true, 'a');
        CHECK(a.usage == 0x04);
        CHECK(a.modifiers == 0);
        auto bang = k.event(true, '!');
        CHECK(bang.usage == 0x1e);
        CHECK(bang.modifiers == vnc::Modifier::LeftShift);
        auto ctrl = k.event(true, 0xffe3);
        CHECK(ctrl.usage == 0xe0);
        CHECK(ctrl.modifiers == vnc::Modifier::LeftCtrl);
        auto c = k.event(true, 'c');
        CHECK(c.usage == 0x06);
        CHECK(c.modifiers == vnc::Modifier::LeftCtrl);
        k.event(false, 0xffe3);
        CHECK(k.modifiers() == 0);
        CHECK(vnc::mapKeysym(0xffbe).usage == 0x3a); // F1
        CHECK(vnc::mapKeysym(0xffc9).usage == 0x45); // F12
        CHECK(vnc::mapKeysym(0xff52).usage == 0x52); // Up
        CHECK(vnc::mapKeysym(0xffb0).usage == 0x62); // KP_0
        CHECK(vnc::mapKeysym(0xffb9).usage == 0x61); // KP_9
        CHECK(vnc::mapKeysym(0x00e9).usage == 0);    // é: no US key
        CHECK(vnc::mapKeysym('~').shift);
        CHECK(k.event(true, 0x20ac).usage == 0); // €: nothing injected
}

TEST_CASE("Cut text: Latin-1 and UTF-8 both ways") {
        using vnc::Text::latin1FromUtf8;
        using vnc::Text::utf8FromLatin1;
        char out[64];
        // Latin-1 in: é (0xe9) and ÿ (0xff) as two bytes each; CR LF as LF.
        const char latin[] = "caf\xe9\r\n\xff!";
        size_t     n = utf8FromLatin1(latin, sizeof(latin) - 1, out, sizeof(out));
        CHECK(std::string(out, n) == "caf\xc3\xa9\n\xc3\xbf!");
        // Room for half a character: stops before it.
        CHECK(utf8FromLatin1("\xe9", 1, out, 1) == 0);
        // UTF-8 out: Latin-1's own, '?' for the rest (the euro sign, an emoji), a broken sequence.
        const char utf8[] = "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80 \xc3";
        n = latin1FromUtf8(utf8, sizeof(utf8) - 1, out, sizeof(out));
        CHECK(std::string(out, n) == "caf\xe9 ? ? ?");
        // Overlong forms aren't Latin-1's characters.
        CHECK(std::string(out, latin1FromUtf8("\xc1\x81", 2, out, sizeof(out))) == "?");
}
