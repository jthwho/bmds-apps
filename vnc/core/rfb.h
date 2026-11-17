/**
 * @file      rfb.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "bytes.h"
#include "des.h"
#include "pixelformat.h"

#include <cstddef>
#include <cstdint>

namespace vnc {

        /** @brief RFB's encodings and pseudo-encodings this server knows (RFB 3.8 §7.7, §7.8). */
        namespace Encoding {
                inline constexpr int32_t Raw = 0;
                inline constexpr int32_t CopyRect = 1;
                inline constexpr int32_t Tight = 7;
                inline constexpr int32_t Zrle = 16;
                inline constexpr int32_t DesktopSize = -223;
                inline constexpr int32_t LastRect = -224;
                inline constexpr int32_t Cursor = -239;
                inline constexpr int32_t QualityLevel0 = -32;   // to -23: 0-9
                inline constexpr int32_t CompressLevel0 = -256; // to -247: 0-9
        } // namespace Encoding

        /** @brief What a viewer's messages ask (`Session`'s events); each default ignores it. */
        class Handler {
                public:
                        virtual ~Handler() = default;

                        /** @brief The handshake is done: updates may be sent. */
                        virtual void ready() {}
                        /** @brief The pixels from now on. */
                        virtual void setPixelFormat(const PixelFormat &format) { (void)format; }
                        /** @brief The encodings the viewer takes, best first. */
                        virtual void setEncodings(const int32_t *encodings, size_t count) {
                                (void)encodings;
                                (void)count;
                        }
                        /** @brief An update asked for: changes only (@p incremental) or the whole area. */
                        virtual void updateRequest(bool incremental, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
                                (void)incremental;
                                (void)x;
                                (void)y;
                                (void)w;
                                (void)h;
                        }
                        /** @brief A key down or up, an X keysym. */
                        virtual void key(bool down, uint32_t keysym) {
                                (void)down;
                                (void)keysym;
                        }
                        /** @brief The pointer at @p x, @p y with @p buttons (bit 0 the left, 3 and 4 the wheel). */
                        virtual void pointer(uint8_t buttons, uint16_t x, uint16_t y) {
                                (void)buttons;
                                (void)x;
                                (void)y;
                        }
                        /** @brief Text the viewer copied (Latin-1, as RFB has it). */
                        virtual void cutText(const char *text, size_t length) {
                                (void)text;
                                (void)length;
                        }
        };

        /**
         * @brief One viewer's RFB session, the server's side (RFB 3.8, and 3.3 and 3.7 as viewers
         *        ask): the handshake, VNC authentication or none, the viewer's messages parsed into
         *        `Handler` calls, and the server's messages written. No I/O: the caller feeds what
         *        it reads and sends what is written (devplan/vnc.md §3).
         *
         * @par Thread Safety
         * Not thread-safe: one task a session.
         */
        class Session {
                public:
                        /** @brief Where the handshake is. */
                        enum class State : uint8_t {
                                Version,   ///< Waiting for the viewer's ProtocolVersion.
                                Security,  ///< Waiting for its choice of security (3.7, 3.8).
                                VencryptVersion, ///< VeNCrypt: waiting for the viewer's version (0.2).
                                VencryptSubtype, ///< VeNCrypt: waiting for its subtype (X509Vnc).
                                Tls,             ///< VeNCrypt: waiting for the caller to start TLS (`tlsStarted()`).
                                Challenge, ///< Waiting for its answer to VNC's challenge.
                                Init,      ///< Waiting for its ClientInit.
                                Normal,    ///< Messages.
                                Closed,    ///< Ended (`closeReason()`).
                        };

                        /** @brief The viewer's messages' largest whole size kept for parsing (text past it is dropped). */
                        static constexpr size_t MaxCutText = 64 * 1024;

                        struct Config {
                                        const char *name = "BMDS"; ///< The desktop's name.
                                        uint16_t    width = 0;     ///< The screen's.
                                        uint16_t    height = 0;
                                        const char *password = nullptr; ///< VNC authentication's; null: none asked.
                                        /**
                                         * Only VeNCrypt's X509Vnc offered (with a password): TLS (the
                                         * caller's, at `State::Tls`), then VNC authentication inside it.
                                         */
                                        bool vencrypt = false;
                                        /** The longest cut text kept (the caller's input buffer's room); longer ones are skipped. */
                                        size_t maxCutText = MaxCutText;
                                        /** Fills @p n bytes with randomness (VNC's challenge). */
                                        void (*random)(void *arg, uint8_t *out, size_t n) = nullptr;
                                        void *randomArg = nullptr;
                        };

                        Session(const Config &config, Handler &handler);

                        /** @brief Writes the server's ProtocolVersion: the session's first bytes. */
                        void start(Writer &out);

                        /**
                         * @brief Takes what the viewer sent: every whole message in @p data handled
                         *        (answers written to @p out), what is left over for the next call.
                         * @return The bytes taken (the rest is an unfinished message: give it again
                         *         with what follows). A broken protocol closes the session.
                         */
                        size_t feed(const uint8_t *data, size_t length, Writer &out);

                        State       state() const { return _state; }
                        bool        isClosed() const { return _state == State::Closed; }
                        const char *closeReason() const { return _reason; }

                        /**
                         * @brief VeNCrypt: the caller has started TLS on the connection (everything
                         *        written before it sent in the clear first): the challenge follows.
                         */
                        void tlsStarted(Writer &out);

                        /** @brief The version the viewer speaks: 3, 7 or 8 (RFB 3.x). */
                        int minorVersion() const { return _minor; }

                        /** @brief Changes the screen's size (sent to the viewer by the caller: `writeDesktopSize()`). */
                        void setSize(uint16_t width, uint16_t height) {
                                _config.width = width;
                                _config.height = height;
                                return;
                        }

                        /** @brief Returns true if the handshake failed for a wrong password. */
                        bool failedAuthentication() const { return _authFailed; }

                        // ---- The server's messages (RFB 3.8 §7.6)

                        /** @brief FramebufferUpdate's header: @p rects rectangles follow (0xffff: `LastRect` ends them). */
                        static void writeUpdateHeader(Writer &out, uint16_t rects);

                        /** @brief A rectangle's header. */
                        static void writeRectHeader(Writer &out, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                                    int32_t encoding);

                        /** @brief The pseudo-rectangle saying the screen is now @p width × @p height. */
                        static void writeDesktopSize(Writer &out, uint16_t width, uint16_t height);

                        /** @brief The pseudo-rectangle ending an update sent with 0xffff rectangles. */
                        static void writeLastRect(Writer &out);

                        /** @brief Bell. */
                        static void writeBell(Writer &out);

                        /** @brief ServerCutText: text copied on the board (Latin-1). */
                        static void writeCutText(Writer &out, const char *text, size_t length);

                private:
                        size_t feedVersion(const uint8_t *data, size_t length, Writer &out);
                        size_t feedSecurity(const uint8_t *data, size_t length, Writer &out);
                        size_t feedVencryptVersion(const uint8_t *data, size_t length, Writer &out);
                        size_t feedVencryptSubtype(const uint8_t *data, size_t length, Writer &out);
                        size_t feedChallenge(const uint8_t *data, size_t length, Writer &out);
                        size_t feedInit(const uint8_t *data, size_t length, Writer &out);
                        size_t feedMessage(const uint8_t *data, size_t length);
                        void   sendChallenge(Writer &out);
                        void   securityResult(Writer &out, bool ok, const char *reason);
                        void   close(const char *reason);

                        Config      _config;
                        Handler    &_handler;
                        State       _state = State::Version;
                        int         _minor = 8;
                        uint8_t     _challenge[ChallengeBytes] = {};
                        bool        _authFailed = false;
                        uint32_t    _skip = 0; // the rest of a cut text too long to keep
                        const char *_reason = nullptr;
        };

} // namespace vnc
