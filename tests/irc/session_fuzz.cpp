/**
 * @file      session_fuzz.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * One network's session under libFuzzer (devplan/irc.md §9): the input is a server's lines (one
 * a line), some of them turned into what the user does or time passing by their first byte.
 * Every line the session sends must be one line of at most 512 bytes; faults abort.
 *
 *   CC=clang CXX=clang++ cmake -S tests -B build-fuzz -G Ninja
 *   (cd build-fuzz && build irc_session_fuzz)
 *   mkdir -p corpus && build-fuzz/irc_session_fuzz corpus
 */

#include "format.h"
#include "session.h"

#include <cstdint>
#include <cstdlib>

using irc::Text;

namespace {

        void check(bool ok) {
                if (!ok) abort();
                return;
        }

        void drain(irc::Session &s, const irc::Now &now) {
                irc::String line;
                while (s.takeLine(now.mono, line)) {
                        const Text l = line;
                        check(l.size() >= 2 && l.size() <= irc::LineBuilder::MaxLine);
                        check(l[l.size() - 2] == '\r' && l[l.size() - 1] == '\n');
                        for (size_t i = 0; i + 2 < l.size(); ++i) check(l[i] != '\r' && l[i] != '\n' && l[i] != '\0');
                }
                irc::Event e;
                while (s.takeEvent(e)) check(irc::Format::isUtf8(e.text));
                return;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        irc::Config c;
        (void)c.nick.assign("jth");
        (void)c.password.assign("secret");
        c.login = size > 0 && (data[0] & 1) ? irc::Login::Sasl : irc::Login::NickServ;
        irc::String join;
        (void)join.assign("#bmds");
        (void)c.autojoin.append(std::move(join));
        irc::Session s(std::move(c));
        irc::Now     now{1000, 1000};
        s.connected(now);
        drain(s, now);

        const Text input(reinterpret_cast<const char *>(data), size);
        size_t     at = 0;
        while (at < input.size()) {
                size_t end = input.find('\n', at);
                if (end == Text::NoPos) end = input.size();
                const Text line = input.mid(at, end - at);
                at = end + 1;
                if (line.isEmpty()) continue;
                // The first byte: 0x01 the user says the rest, 0x02 time passes, 0x03 the connection
                // goes and comes back; anything else the server's line.
                const Text rest = line.mid(1);
                switch (line[0]) {
                        case '\x01': {
                                const size_t space = rest.find(' ');
                                const Text   target = space == Text::NoPos ? rest : rest.left(space);
                                const Text   text = space == Text::NoPos ? Text() : rest.mid(space + 1);
                                (void)s.privmsg(target, text, now);
                                (void)s.action(target, text, now);
                                (void)s.join(target, text);
                                (void)s.raw(rest);
                                break;
                        }
                        case '\x02':
                                now.mono += int64_t(uint8_t(rest.isEmpty() ? 0 : rest[0])) * 1000;
                                now.wall = now.mono;
                                s.tick(now);
                                break;
                        case '\x03':
                                s.disconnected();
                                s.connected(now);
                                break;
                        default: s.received(line, now); break;
                }
                drain(s, now);
        }
        // Everything typed goes out in time.
        now.mono += 3600 * 1000;
        s.tick(now);
        drain(s, now);
        return 0;
}
