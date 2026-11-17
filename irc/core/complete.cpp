/**
 * @file      complete.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "complete.h"

#include "commands.h"

namespace irc {

        namespace {

                // @p word starts @p name, under @p mapping.
                bool startsWith(Text name, Text word, CaseMapping mapping) {
                        return name.size() >= word.size() && sameName(name.left(word.size()), word, mapping);
                }

                // @p a before @p b, ignoring case.
                bool before(Text a, Text b, CaseMapping mapping) {
                        const size_t n = a.size() < b.size() ? a.size() : b.size();
                        for (size_t i = 0; i < n; ++i) {
                                const char x = foldCase(a[i], mapping), y = foldCase(b[i], mapping);
                                if (x != y) return x < y;
                        }
                        return a.size() < b.size();
                }

                bool has(const Vector<String> &list, Text name, CaseMapping mapping) {
                        for (const String &s : list)
                                if (sameName(s, name, mapping)) return true;
                        return false;
                }

                void add(Vector<String> &list, Text name) {
                        String s;
                        if (s.assign(name) == Error::Ok) (void)list.append(std::move(s));
                        return;
                }

        } // namespace

        void Completer::reset() {
                _matches.clear();
                _index = 0;
                _last.clear();
                return;
        }

        bool Completer::apply(String &line, size_t &cursor) {
                const Text match = _matches[_index];
                const bool nickFirst = _atStart && _nicks;
                String     out;
                (void)out.assign(_before);
                (void)out.append(match);
                // At the end: a separator after it (": " after a nick starting the line); in the
                // middle of the line only the colon, the space being there already.
                if (_after.isEmpty())
                        (void)out.append(nickFirst ? Text(": ") : Text(" "));
                else if (nickFirst && !_after.view().startsWith(":"))
                        (void)out.append(':');
                cursor = out.size();
                (void)out.append(_after);
                line = std::move(out);
                (void)_last.assign(line);
                _lastCursor = cursor;
                return true;
        }

        bool Completer::complete(String &line, size_t &cursor, const Session &session, const Buffers &buffers,
                                 uint32_t current) {
                // Again on the line we left: the next match.
                if (!_matches.isEmpty() && line.view() == _last.view() && cursor == _lastCursor) {
                        _index = (_index + 1) % _matches.size();
                        return apply(line, cursor);
                }
                reset();
                const Text all = line;
                if (cursor > all.size()) cursor = all.size();
                size_t start = cursor;
                while (start > 0 && all[start - 1] != ' ') --start;
                const Text word = all.mid(start, cursor - start);
                if (word.isEmpty()) return false;
                const CaseMapping mapping = session.isupport().caseMapping();
                _atStart = start == 0;
                _nicks = false;

                if (_atStart && word.startsWith("/")) {
                        // Commands, in their (alphabetical) order.
                        const Text name = word.mid(1);
                        for (size_t i = 0; i < CommandCount; ++i)
                                if (startsWith(CommandNames[i], name, CaseMapping::Ascii)) {
                                        String c;
                                        (void)c.assign("/");
                                        (void)c.append(CommandNames[i]);
                                        (void)_matches.append(std::move(c));
                                }
                } else if (session.isupport().isChannel(word)) {
                        for (size_t i = 0; i < session.channelCount(); ++i) {
                                const Text name = session.channelAt(i).name;
                                if (startsWith(name, word, mapping) && !has(_matches, name, mapping))
                                        add(_matches, name);
                        }
                } else {
                        _nicks = true;
                        const Buffer *b = buffers.byId(current);
                        if (b == nullptr) return false;
                        if (b->type == Buffer::Type::Query) {
                                if (startsWith(b->name, word, mapping)) add(_matches, b->name);
                        } else if (b->type == Buffer::Type::Channel) {
                                const Channel *c = session.channel(b->name);
                                const auto     member = [&](Text nick) {
                                        if (c == nullptr) return true;
                                        for (const Member &m : c->members)
                                                if (sameName(m.nick, nick, mapping)) return true;
                                        return false;
                                };
                                // Who spoke last first (still here), then the rest by name.
                                for (const String &s : b->speakers)
                                        if (startsWith(s, word, mapping) && member(s) &&
                                            !sameName(s, session.nick(), mapping))
                                                add(_matches, s);
                                const size_t spoken = _matches.size();
                                if (c != nullptr)
                                        for (const Member &m : c->members) {
                                                if (!startsWith(m.nick, word, mapping) ||
                                                    sameName(m.nick, session.nick(), mapping) ||
                                                    has(_matches, m.nick, mapping))
                                                        continue;
                                                // In order by name among the unspoken.
                                                size_t at = _matches.size();
                                                while (at > spoken && before(m.nick, _matches[at - 1], mapping)) --at;
                                                String s;
                                                if (s.assign(m.nick) == Error::Ok)
                                                        (void)_matches.insert(at, std::move(s));
                                        }
                        }
                }
                if (_matches.isEmpty()) return false;
                (void)_before.assign(all.left(start));
                (void)_after.assign(all.mid(cursor));
                _index = 0;
                return apply(line, cursor);
        }

} // namespace irc
