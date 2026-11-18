/**
 * @file      ignore.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "ignore.h"

namespace irc {

        bool matchesMask(Text mask, Text text, CaseMapping mapping) {
                // The usual greedy match: a star remembered, and moved on by one when what follows fails.
                size_t m = 0, t = 0, star = Text::NoPos, after = 0;
                while (t < text.size()) {
                        if (m < mask.size() && mask[m] == '*') {
                                star = m++;
                                after = t;
                        } else if (m < mask.size() &&
                                   (mask[m] == '?' || foldCase(mask[m], mapping) == foldCase(text[t], mapping))) {
                                ++m;
                                ++t;
                        } else if (star != Text::NoPos) {
                                m = star + 1;
                                t = ++after;
                        } else {
                                return false;
                        }
                }
                while (m < mask.size() && mask[m] == '*') ++m;
                return m == mask.size();
        }

        Error Ignores::normalize(Text spec, String &out) {
                out.clear();
                if (spec.isEmpty()) return Error::Invalid;
                for (char c : spec)
                        if (c == ' ' || c == ',' || c == '\r' || c == '\n' || c == '\0') return Error::Invalid;
                const size_t bang = spec.find('!');
                const size_t at = spec.find('@');
                Error        err = Error::Ok;
                if (bang == Text::NoPos && at == Text::NoPos) { // nick
                        err = out.assign(spec);
                        if (err == Error::Ok) err = out.append("!*@*");
                } else if (bang == Text::NoPos) { // user@host
                        err = out.assign("*!");
                        if (err == Error::Ok) err = out.append(spec);
                } else if (at == Text::NoPos) { // nick!user
                        err = out.assign(spec);
                        if (err == Error::Ok) err = out.append("@*");
                } else {
                        err = out.assign(spec);
                }
                if (err != Error::Ok) return Error::NoMem;
                // Every part there: nick!user@host, none of them empty.
                const Text   m = out;
                const size_t b = m.find('!'), a = m.find('@');
                if (b == 0 || a == Text::NoPos || a < b || a == b + 1 || a + 1 == m.size()) {
                        out.clear();
                        return Error::Invalid;
                }
                return Error::Ok;
        }

        Error Ignores::add(Text spec, Text network) {
                String      mask;
                const Error err = normalize(spec, mask);
                if (err != Error::Ok) return err;
                for (const Entry &e : _masks)
                        if (e.mask.view().equalsIgnoringCase(mask) && e.network.view().equalsIgnoringCase(network))
                                return Error::Ok;
                Entry e;
                e.mask = std::move(mask);
                if (e.network.assign(network) != Error::Ok) return Error::NoMem;
                return _masks.append(std::move(e));
        }

        bool Ignores::remove(Text spec, Text network) {
                String mask;
                if (normalize(spec, mask) != Error::Ok) return false;
                for (size_t i = 0; i < _masks.size(); ++i)
                        if (_masks[i].mask.view().equalsIgnoringCase(mask) &&
                            _masks[i].network.view().equalsIgnoringCase(network)) {
                                _masks.removeAt(i);
                                return true;
                        }
                return false;
        }

        bool Ignores::matches(Text nick, Text user, Text host, Text network, CaseMapping mapping) const {
                if (_masks.isEmpty() || nick.isEmpty()) return false;
                for (const Entry &e : _masks) {
                        if (!e.network.isEmpty() && !e.network.view().equalsIgnoringCase(network)) continue;
                        const Text   m = e.mask;
                        const size_t b = m.find('!'), a = m.find('@', b == Text::NoPos ? 0 : b);
                        if (b == Text::NoPos || a == Text::NoPos) continue;
                        if (matchesMask(m.left(b), nick, mapping) &&
                            matchesMask(m.mid(b + 1, a - b - 1), user, mapping) &&
                            matchesMask(m.mid(a + 1), host, mapping))
                                return true;
                }
                return false;
        }

        Error Ignores::read(Text list) {
                _masks.clear();
                size_t at = 0;
                while (at < list.size()) {
                        size_t end = list.find(',', at);
                        if (end == Text::NoPos) end = list.size();
                        Text item = list.mid(at, end - at);
                        at = end + 1;
                        while (!item.isEmpty() && item[0] == ' ') item = item.mid(1);
                        while (!item.isEmpty() && item[item.size() - 1] == ' ') item = item.left(item.size() - 1);
                        if (item.isEmpty()) continue;
                        // nick!user@host@network: a second @ after the host's names the network.
                        const size_t first = item.find('@');
                        const size_t second = first == Text::NoPos ? Text::NoPos : item.find('@', first + 1);
                        const Text   spec = second == Text::NoPos ? item : item.left(second);
                        const Text   network = second == Text::NoPos ? Text() : item.mid(second + 1);
                        if (add(spec, network) == Error::NoMem) return Error::NoMem;
                }
                return Error::Ok;
        }

        Error Ignores::write(String &out) const {
                out.clear();
                for (size_t i = 0; i < _masks.size(); ++i) {
                        if (i > 0 && out.append(", ") != Error::Ok) return Error::NoMem;
                        if (out.append(_masks[i].mask) != Error::Ok) return Error::NoMem;
                        if (!_masks[i].network.isEmpty() &&
                            (out.append('@') != Error::Ok || out.append(_masks[i].network) != Error::Ok))
                                return Error::NoMem;
                }
                return Error::Ok;
        }

} // namespace irc
