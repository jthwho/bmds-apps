/**
 * @file      session_fuzz.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * One network's session under libFuzzer (devplan/irc.md §9): the input is a server's lines (one
 * a line), some of them turned into what the user types (a command), a Tab, or time passing by
 * their first byte; every event goes through the network's buffers. Every line the session
 * sends must be one line of at most 512 bytes, every line a buffer makes a buffer's; faults
 * abort.
 *
 *   CC=clang CXX=clang++ cmake -S tests -B build-fuzz -G Ninja
 *   (cd build-fuzz && build irc_session_fuzz)
 *   mkdir -p corpus && build-fuzz/irc_session_fuzz corpus
 */

#include "buffers.h"
#include "commands.h"
#include "complete.h"
#include "format.h"
#include "ignore.h"
#include "session.h"

#include <cstdint>
#include <cstdlib>

using irc::Text;

namespace {

        void check(bool ok) {
                if (!ok) abort();
                return;
        }

        void drain(irc::Session &s, irc::Buffers &buffers, const irc::Now &now) {
                bool        perform = false;
                irc::String line;
                while (s.takeLine(now.mono, line)) {
                        const Text l = line;
                        check(l.size() >= 2 && l.size() <= irc::LineBuilder::MaxLine);
                        check(l[l.size() - 2] == '\r' && l[l.size() - 1] == '\n');
                        for (size_t i = 0; i + 2 < l.size(); ++i) check(l[i] != '\r' && l[i] != '\n' && l[i] != '\0');
                }
                irc::Event e;
                while (s.takeEvent(e)) {
                        check(irc::Format::isUtf8(e.text));
                        irc::Vector<irc::Line> lines;
                        (void)buffers.apply(e, s, lines);
                        for (const irc::Line &l : lines) check(buffers.byId(l.buffer) != nullptr);
                        if (e.kind == irc::Event::Kind::Perform) perform = true;
                }
                if (perform) {
                        // As the app: each line made a command and run, then the joins.
                        for (const irc::String &p : s.config().perform) {
                                irc::String            command;
                                irc::Vector<irc::Line> lines;
                                irc::Outcome           outcome;
                                if (irc::performLine(p, s.nick(), command) == irc::Error::Ok)
                                        (void)irc::runCommand(command, s, buffers, buffers.network().id, now, lines,
                                                              outcome);
                        }
                        s.performed();
                        drain(s, buffers, now);
                }
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
        if (size > 0 && (data[0] & 2)) {
                irc::String p;
                (void)p.assign("mode $me +x");
                (void)c.perform.append(std::move(p));
        }
        c.rejoin = size > 0 && (data[0] & 4);
        c.joinInvited = size > 0 && (data[0] & 8);
        irc::Ignores ignores;
        (void)ignores.read("spam!*@*, *!*@bad.*@TestNet");
        irc::Session   s(std::move(c));
        s.setIgnores(&ignores, "TestNet");
        (void)s.watch("alice");
        irc::Buffers   buffers;
        irc::Completer completer;
        irc::Now       now{1000, 1000};
        uint32_t       current = buffers.network().id;
        s.connected(now);
        drain(s, buffers, now);

        const Text input(reinterpret_cast<const char *>(data), size);
        size_t     at = 0;
        while (at < input.size()) {
                size_t end = input.find('\n', at);
                if (end == Text::NoPos) end = input.size();
                const Text line = input.mid(at, end - at);
                at = end + 1;
                if (line.isEmpty()) continue;
                // The first byte: 0x01 the user types the rest (in the buffer its next byte picks),
                // 0x02 time passes, 0x03 the connection goes and comes back, 0x04 Tab on the rest,
                // 0x05 the rest the ignore list, 0x06 the rest watched (or no longer, if it was);
                // anything else the server's line.
                const Text rest = line.mid(1);
                switch (line[0]) {
                        case '\x01': {
                                // Typed in a buffer the input's next byte picks.
                                if (buffers.count() > 0 && !rest.isEmpty())
                                        current = buffers.at(uint8_t(rest[0]) % buffers.count()).id;
                                irc::Vector<irc::Line> lines;
                                irc::Outcome           outcome;
                                (void)irc::runCommand(rest.mid(1), s, buffers, current, now, lines, outcome);
                                for (const irc::Line &l : lines) check(buffers.byId(l.buffer) != nullptr);
                                if (outcome.action == irc::Outcome::Action::Close) {
                                        buffers.close(outcome.buffer);
                                        current = buffers.network().id;
                                } else if (outcome.action == irc::Outcome::Action::Ignore && !outcome.text.isEmpty()) {
                                        if (outcome.on)
                                                (void)ignores.add(outcome.text, outcome.extra);
                                        else
                                                (void)ignores.remove(outcome.text, outcome.extra);
                                } else if (outcome.action == irc::Outcome::Action::Notify && !outcome.text.isEmpty()) {
                                        if (outcome.on)
                                                (void)s.watch(outcome.text);
                                        else
                                                s.unwatch(outcome.text);
                                }
                                break;
                        }
                        case '\x05': {
                                (void)ignores.read(rest);
                                irc::String out;
                                (void)ignores.write(out);
                                break;
                        }
                        case '\x06':
                                if (s.online(rest) != -1 || s.watch(rest) != irc::Error::Ok) s.unwatch(rest);
                                break;
                        case '\x04': {
                                irc::String text;
                                (void)text.assign(rest);
                                size_t cursor = rest.size();
                                for (int i = 0; i < 3; ++i)
                                        if (completer.complete(text, cursor, s, buffers, current))
                                                check(cursor <= text.size());
                                completer.reset();
                                break;
                        }
                        case '\x02':
                                now.mono += int64_t(uint8_t(rest.isEmpty() ? 0 : rest[0])) * 1000;
                                now.wall = now.mono;
                                s.tick(now);
                                break;
                        case '\x03':
                                s.disconnected();
                                buffers.disconnected();
                                s.connected(now);
                                break;
                        default: s.received(line, now); break;
                }
                drain(s, buffers, now);
        }
        // Everything typed goes out in time.
        now.mono += 3600 * 1000;
        s.tick(now);
        drain(s, buffers, now);
        return 0;
}
