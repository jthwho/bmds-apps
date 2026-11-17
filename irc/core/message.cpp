/**
 * @file      message.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "message.h"

#include <cstring>

namespace irc {

        namespace {

                // A byte that may not be in any parameter: it would end the line, or C strings.
                bool isBreak(char c) {
                        return c == '\r' || c == '\n' || c == '\0';
                }

                bool hasBreak(Text text) {
                        for (char c : text)
                                if (isBreak(c)) return true;
                        return false;
                }

                // The next word of @p line from @p pos (past spaces), and where it ends.
                size_t skipSpaces(Text line, size_t pos) {
                        while (pos < line.size() && line[pos] == ' ') ++pos;
                        return pos;
                }

                size_t wordEnd(Text line, size_t pos) {
                        size_t end = line.find(' ', pos);
                        return end == Text::NoPos ? line.size() : end;
                }

                // Calls @p fn(key, value, hasValue) for each tag of @p tags until it returns true.
                template <typename F> bool eachTag(Text tags, F &&fn) {
                        size_t pos = 0;
                        while (pos <= tags.size()) {
                                size_t end = tags.find(';', pos);
                                if (end == Text::NoPos) end = tags.size();
                                Text   tag = tags.mid(pos, end - pos);
                                size_t eq = tag.find('=');
                                if (!tag.isEmpty()) {
                                        Text key = eq == Text::NoPos ? tag : tag.left(eq);
                                        Text value = eq == Text::NoPos ? Text() : tag.mid(eq + 1);
                                        if (fn(key, value)) return true;
                                }
                                pos = end + 1;
                        }
                        return false;
                }

        } // namespace

        Source parseSource(Text source) {
                Source out;
                Text   rest = source;
                size_t at = rest.find('@');
                if (at != Text::NoPos) {
                        out.host = rest.mid(at + 1);
                        rest = rest.left(at);
                }
                size_t bang = rest.find('!');
                if (bang != Text::NoPos) {
                        out.user = rest.mid(bang + 1);
                        rest = rest.left(bang);
                }
                out.nick = rest;
                return out;
        }

        int Message::numeric() const {
                if (command.size() != 3) return -1;
                int n = 0;
                for (char c : command) {
                        if (c < '0' || c > '9') return -1;
                        n = n * 10 + (c - '0');
                }
                return n;
        }

        bool Message::hasTag(Text key) const {
                return eachTag(tags, [&](Text k, Text) { return k == key; });
        }

        Text Message::tag(Text key) const {
                Text found;
                eachTag(tags, [&](Text k, Text v) {
                        if (!(k == key)) return false;
                        found = v;
                        return true;
                });
                return found;
        }

        Error parse(Text line, Message &out) {
                out = Message();
                size_t pos = skipSpaces(line, 0);
                if (pos < line.size() && line[pos] == '@') {
                        size_t end = wordEnd(line, pos);
                        out.tags = line.mid(pos + 1, end - pos - 1);
                        pos = skipSpaces(line, end);
                }
                if (pos < line.size() && line[pos] == ':') {
                        size_t end = wordEnd(line, pos);
                        out.source = line.mid(pos + 1, end - pos - 1);
                        pos = skipSpaces(line, end);
                }
                size_t end = wordEnd(line, pos);
                out.command = line.mid(pos, end - pos);
                if (out.command.isEmpty()) return Error::Malformed;
                pos = skipSpaces(line, end);
                while (pos < line.size()) {
                        if (line[pos] == ':') {
                                out.params[out.paramCount++] = line.mid(pos + 1);
                                break;
                        }
                        if (out.paramCount == Message::MaxParams - 1) {
                                out.params[out.paramCount++] = line.mid(pos);
                                break;
                        }
                        end = wordEnd(line, pos);
                        out.params[out.paramCount++] = line.mid(pos, end - pos);
                        pos = skipSpaces(line, end);
                }
                return Error::Ok;
        }

        size_t unescapeTag(Text raw, char *out, size_t most) {
                size_t n = 0;
                auto   put = [&](char c) {
                        if (n < most) out[n] = c;
                        ++n;
                        return;
                };
                for (size_t i = 0; i < raw.size(); ++i) {
                        char c = raw[i];
                        if (c != '\\') {
                                put(c);
                                continue;
                        }
                        if (++i == raw.size()) break;
                        switch (raw[i]) {
                                case ':': put(';'); break;
                                case 's': put(' '); break;
                                case 'r': put('\r'); break;
                                case 'n': put('\n'); break;
                                default: put(raw[i]); break;
                        }
                }
                return n;
        }

        size_t splitAt(Text text, size_t most) {
                if (text.size() <= most) return text.size();
                // Back to the start of the character text[most] is in (at most three bytes: text
                // that isn't UTF-8 is cut anywhere).
                size_t cut = most;
                for (int i = 0; i < 3 && cut > 0 && (uint8_t(text[cut]) & 0xc0) == 0x80; ++i) --cut;
                if (cut == 0) return 0;
                size_t space = text.left(cut + 1).findLast(' ');
                if (space != Text::NoPos && space > 0 && space >= most / 2) return space;
                return cut;
        }

        LineBuilder::LineBuilder(Text command) {
                bool word = !command.isEmpty();
                for (char c : command)
                        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) word = false;
                if (!word)
                        _error = Error::Malformed;
                else
                        append(command);
                return;
        }

        LineBuilder &LineBuilder::arg(Text param) {
                if (_error != Error::Ok) return *this;
                if (_ended || param.isEmpty() || param[0] == ':' || param.find(' ') != Text::NoPos || hasBreak(param))
                        _error = Error::Malformed;
                else if (append(" "))
                        append(param);
                return *this;
        }

        LineBuilder &LineBuilder::last(Text param) {
                if (_error != Error::Ok) return *this;
                if (_ended || hasBreak(param))
                        _error = Error::Malformed;
                else if (append(" :"))
                        append(param);
                _ended = true;
                return *this;
        }

        Text LineBuilder::line() {
                if (_error != Error::Ok) return Text();
                // append() keeps two bytes for these.
                _buf[_size] = '\r';
                _buf[_size + 1] = '\n';
                return Text(_buf, _size + 2);
        }

        bool LineBuilder::append(Text bytes) {
                if (bytes.size() > MaxLine - 2 - _size) {
                        _error = Error::TooLong;
                        return false;
                }
                memcpy(_buf + _size, bytes.data(), bytes.size());
                _size += bytes.size();
                return true;
        }

        size_t LineReader::write(Text bytes) {
                if (_start > 0) {
                        memmove(_buf, _buf + _start, _end - _start);
                        _end -= _start;
                        _start = 0;
                }
                size_t room = sizeof(_buf) - _end;
                size_t n = bytes.size() < room ? bytes.size() : room;
                memcpy(_buf + _end, bytes.data(), n);
                _end += n;
                return n;
        }

        bool LineReader::take(Text &line) {
                for (;;) {
                        size_t from = _start + _scanned;
                        auto  *lf = static_cast<char *>(memchr(_buf + from, '\n', _end - from));
                        if (lf == nullptr) {
                                _scanned = _end - _start;
                                if (_end - _start == sizeof(_buf)) {
                                        // Full without an LF: too long. Dropped to its end.
                                        if (!_skipping) ++_dropped;
                                        _skipping = true;
                                        _start = _end = _scanned = 0;
                                }
                                return false;
                        }
                        size_t first = _start;
                        size_t past = size_t(lf - _buf);
                        _start = past + 1;
                        _scanned = 0;
                        if (_skipping) {
                                _skipping = false;
                                continue;
                        }
                        if (past > first && _buf[past - 1] == '\r') --past;
                        if (past == first) continue;
                        if (past - first > MaxLine) {
                                ++_dropped;
                                continue;
                        }
                        line = Text(_buf + first, past - first);
                        return true;
                }
        }

} // namespace irc
