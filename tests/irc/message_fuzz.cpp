/**
 * @file      message_fuzz.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * IRC's messages under libFuzzer (devplan/irc.md §9): bytes cut into lines in chunks the input
 * chooses, each line parsed; a parsed message built again and parsed back to the same; a line
 * split as a long message is. Faults, and any of these checks failing, abort.
 *
 *   CC=clang CXX=clang++ cmake -S tests -B build-fuzz -G Ninja
 *   (cd build-fuzz && build irc_message_fuzz)
 *   mkdir -p corpus && build-fuzz/irc_message_fuzz corpus
 */

#include "message.h"

#include <cstdint>
#include <cstdlib>

using irc::Text;

namespace {

        void check(bool ok) {
                if (!ok) abort();
                return;
        }

        bool inside(Text part, Text whole) {
                return part.isEmpty() || (part.data() >= whole.data() && part.end() <= whole.end());
        }

        void parsed(Text line) {
                irc::Message m;
                if (irc::parse(line, m) != irc::Error::Ok) return;
                check(!m.command.isEmpty());
                check(m.paramCount <= irc::Message::MaxParams);
                check(inside(m.tags, line) && inside(m.source, line) && inside(m.command, line));
                for (size_t i = 0; i < m.paramCount; ++i) check(inside(m.params[i], line));

                char   out[64];
                size_t n = irc::unescapeTag(m.tag("a"), out, sizeof out);
                check(n <= m.tag("a").size());
                irc::Source s = irc::parseSource(m.source);
                check(inside(s.nick, m.source) && inside(s.user, m.source) && inside(s.host, m.source));

                // Built again (every parameter but the last as a middle one, if it can be), the line
                // parses back to the same command and parameters.
                irc::LineBuilder b(m.command);
                for (size_t i = 0; i + 1 < m.paramCount; ++i) b.arg(m.params[i]);
                if (m.paramCount) b.last(m.last());
                Text again = b.line();
                if (b.error() != irc::Error::Ok) return;
                check(again.size() <= irc::LineBuilder::MaxLine);
                irc::Message back;
                check(irc::parse(again.left(again.size() - 2), back) == irc::Error::Ok);
                check(back.command == m.command);
                check(back.paramCount == m.paramCount);
                for (size_t i = 0; i < m.paramCount; ++i) check(back.params[i] == m.params[i]);
                return;
        }

        void split(Text line, size_t most) {
                Text rest = line;
                while (!rest.isEmpty()) {
                        size_t n = irc::splitAt(rest, most);
                        if (n == 0) {
                                // Only when the first character doesn't fit.
                                check(most < 4);
                                return;
                        }
                        check(n <= most && n <= rest.size());
                        rest = rest.mid(n);
                        if (!rest.isEmpty() && rest[0] == ' ') rest = rest.mid(1);
                }
                return;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        // The first byte: the chunk size bytes are fed in; the second: the split's room.
        if (size < 2) return 0;
        size_t chunk = data[0] % 64 + 1;
        size_t most = data[1];
        Text   bytes(reinterpret_cast<const char *>(data) + 2, size - 2);

        static irc::LineReader reader;
        reader.reset();
        for (size_t pos = 0; pos < bytes.size(); pos += chunk) {
                reader.feed(bytes.mid(pos, chunk), [&](Text line) {
                        check(!line.isEmpty() && line.size() <= irc::LineReader::MaxLine);
                        check(line.find('\n') == Text::NoPos);
                        parsed(line);
                        split(line, most);
                });
        }
        return 0;
}
