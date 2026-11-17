/**
 * @file      helpers.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What IRC's core tests share: doctest printing a Text, and a session driven by a scripted
 * server (what it sends collected, its events too, the clock moved by hand).
 */

#pragma once

#include "buffers.h"
#include "session.h"
#include "text.h"

#include <doctest.h>

#include <string>
#include <vector>

namespace doctest {
        template <> struct StringMaker<irc::Text> {
                        static String convert(irc::Text text) {
                                return String("\"") + String(text.data(), String::size_type(text.size())) + "\"";
                        }
        };
} // namespace doctest

namespace test {

        inline irc::Text text(const std::string &s) {
                return irc::Text(s.data(), s.size());
        }

        inline std::string str(irc::Text t) {
                return std::string(t.data(), t.size());
        }

        /** @brief An event, copied out of the session's (its strings as std::string). */
        struct Seen {
                        irc::Event::Kind kind;
                        uint8_t          flags;
                        std::string      target, nick, text, extra;
                        int64_t          time;
                        int64_t          number;
        };

        /** @brief A buffer's line, copied out. */
        struct ShownLine {
                        uint32_t    buffer;
                        std::string view, log;
                        uint8_t     level;
                        bool        notify;
        };

        /** @brief A session and a scripted server: lines in, what it sent and told out. */
        struct Server {
                        irc::Session             session;
                        irc::Now                 now{1000, 1763289660000}; // 2025-11-16 10:41 UTC
                        std::vector<std::string> sent;                     // without CR LF
                        std::vector<Seen>        events;
                        irc::Buffers            *buffers = nullptr; // when set, every event goes through it too
                        std::vector<ShownLine>   lines;

                        explicit Server(irc::Config &&config) : session(std::move(config)) {}

                        /** @brief Every line pacing lets out now, into `sent`. */
                        void flush() {
                                irc::String line;
                                while (session.takeLine(now.mono, line)) {
                                        std::string l = str(line);
                                        REQUIRE(l.size() >= 2);
                                        REQUIRE(l.substr(l.size() - 2) == "\r\n");
                                        sent.push_back(l.substr(0, l.size() - 2));
                                }
                                irc::Event e;
                                while (session.takeEvent(e)) {
                                        events.push_back(Seen{e.kind, e.flags, str(e.target), str(e.nick), str(e.text),
                                                              str(e.extra), e.time, e.number});
                                        if (buffers != nullptr) {
                                                irc::Vector<irc::Line> out;
                                                REQUIRE(buffers->apply(e, session, out) == irc::Error::Ok);
                                                keep(out);
                                        }
                                }
                                return;
                        }

                        /** @brief Lines made outside the session (a command's), kept with the rest. */
                        void keep(irc::Vector<irc::Line> &out) {
                                for (const irc::Line &l : out)
                                        lines.push_back(
                                                ShownLine{l.buffer, str(l.view), str(l.log), l.level, l.notify});
                                out.clear();
                                return;
                        }

                        void connect() {
                                session.connected(now);
                                flush();
                        }

                        /** @brief The server says @p line. */
                        void say(const std::string &line) {
                                session.received(text(line), now);
                                flush();
                        }

                        /** @brief Time passes (ticks as the session asks), pacing too. */
                        void wait(int64_t ms) {
                                const int64_t until = now.mono + ms;
                                for (;;) {
                                        flush(); // what can go now goes before the clock moves
                                        if (now.mono >= until) break;
                                        int64_t       step = until - now.mono;
                                        const int64_t tick = session.tickWaitMs(now.mono);
                                        const int64_t send = session.sendWaitMs(now.mono);
                                        if (tick >= 0 && tick < step) step = tick;
                                        if (send > 0 && send < step) step = send;
                                        if (step < 1) step = 1; // a tick due now: run after a ms
                                        now.mono += step;
                                        now.wall += step;
                                        session.tick(now);
                                }
                                return;
                        }

                        /** @brief Takes what was sent so far. */
                        std::vector<std::string> take() {
                                std::vector<std::string> out;
                                out.swap(sent);
                                return out;
                        }

                        /** @brief Takes the buffers' lines so far. */
                        std::vector<ShownLine> takeLines() {
                                std::vector<ShownLine> out;
                                out.swap(lines);
                                return out;
                        }

                        /** @brief Takes the events so far. */
                        std::vector<Seen> takeEvents() {
                                std::vector<Seen> out;
                                out.swap(events);
                                return out;
                        }

                        /** @brief Registered plainly as @p nick (no CAP), what that sent and told dropped. */
                        void registerAs(const std::string &nick) {
                                connect();
                                say(":srv 001 " + nick + " :Welcome");
                                say(":srv 005 " + nick + " CHANTYPES=# PREFIX=(ov)@+ NETWORK=Test :are supported");
                                take();
                                takeEvents();
                        }
        };

        inline irc::Config config(const char *nick, irc::Login login = irc::Login::None, const char *password = "") {
                irc::Config c;
                (void)c.nick.assign(nick);
                (void)c.user.assign("u");
                (void)c.realName.assign("Real Name");
                c.login = login;
                (void)c.password.assign(password);
                return c;
        }

} // namespace test
