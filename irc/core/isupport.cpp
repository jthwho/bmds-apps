/**
 * @file      isupport.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "isupport.h"

#include <cstring>

namespace irc {

        bool sameName(Text a, Text b, CaseMapping mapping) {
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i)
                        if (foldCase(a[i], mapping) != foldCase(b[i], mapping)) return false;
                return true;
        }

        void Isupport::put(char *out, size_t room, Text value) {
                const size_t n = value.size() < room - 1 ? value.size() : room - 1;
                std::memcpy(out, value.data(), n);
                out[n] = '\0';
                return;
        }

        void Isupport::reset() {
                _caseMapping = CaseMapping::Rfc1459;
                put(_network, sizeof(_network), "");
                put(_prefixModes, sizeof(_prefixModes), "ov");
                put(_prefixChars, sizeof(_prefixChars), "@+");
                put(_chanTypes, sizeof(_chanTypes), "#&");
                put(_listModes, sizeof(_listModes), "beI");
                put(_alwaysModes, sizeof(_alwaysModes), "k");
                put(_setModes, sizeof(_setModes), "l");
                return;
        }

        void Isupport::apply(const Message &message) {
                // 005 NICK TOKEN TOKEN=VALUE ... :are supported by this server
                for (size_t i = 1; i + 1 < message.paramCount; ++i) {
                        Text         token = message.params[i];
                        const bool   negated = token.startsWith("-");
                        const size_t eq = token.find('=');
                        Text         key = negated ? token.mid(1) : eq == Text::NoPos ? token : token.left(eq);
                        Text         value = negated || eq == Text::NoPos ? Text() : token.mid(eq + 1);
                        if (key == Text("CASEMAPPING")) {
                                _caseMapping = value == Text("ascii")            ? CaseMapping::Ascii
                                               : value == Text("strict-rfc1459") ? CaseMapping::StrictRfc1459
                                                                                 : CaseMapping::Rfc1459;
                        } else if (key == Text("NETWORK")) {
                                put(_network, sizeof(_network), value);
                        } else if (key == Text("CHANTYPES")) {
                                put(_chanTypes, sizeof(_chanTypes), negated ? Text("#&") : value);
                        } else if (key == Text("PREFIX")) {
                                // (modes)prefixes, or nothing: no prefixes.
                                const size_t close = value.find(')');
                                if (negated) {
                                        put(_prefixModes, sizeof(_prefixModes), "ov");
                                        put(_prefixChars, sizeof(_prefixChars), "@+");
                                } else if (value.startsWith("(") && close != Text::NoPos &&
                                           close - 1 == value.size() - close - 1) {
                                        put(_prefixModes, sizeof(_prefixModes), value.mid(1, close - 1));
                                        put(_prefixChars, sizeof(_prefixChars), value.mid(close + 1));
                                } else if (value.isEmpty()) {
                                        put(_prefixModes, sizeof(_prefixModes), "");
                                        put(_prefixChars, sizeof(_prefixChars), "");
                                }
                        } else if (key == Text("CHANMODES")) {
                                if (negated) {
                                        put(_listModes, sizeof(_listModes), "beI");
                                        put(_alwaysModes, sizeof(_alwaysModes), "k");
                                        put(_setModes, sizeof(_setModes), "l");
                                        continue;
                                }
                                // A,B,C,D
                                char  *outs[3] = {_listModes, _alwaysModes, _setModes};
                                size_t from = 0;
                                for (int part = 0; part < 3; ++part) {
                                        size_t comma = value.find(',', from);
                                        if (comma == Text::NoPos) comma = value.size();
                                        put(outs[part], 48, value.mid(from, comma - from));
                                        from = comma + 1;
                                }
                        }
                }
                return;
        }

        bool Isupport::isChannel(Text name) const {
                return !name.isEmpty() && Text(_chanTypes).find(name[0]) != Text::NoPos;
        }

        char Isupport::prefixFor(char mode) const {
                const size_t at = Text(_prefixModes).find(mode);
                return at != Text::NoPos && at < std::strlen(_prefixChars) ? _prefixChars[at] : 0;
        }

        size_t Isupport::rank(char prefix) const {
                const size_t at = Text(_prefixChars).find(prefix);
                return at != Text::NoPos ? at : std::strlen(_prefixChars);
        }

        bool Isupport::takesParameter(char mode, bool setting) const {
                if (Text(_prefixModes).find(mode) != Text::NoPos) return true;
                if (Text(_listModes).find(mode) != Text::NoPos) return true;
                if (Text(_alwaysModes).find(mode) != Text::NoPos) return true;
                if (Text(_setModes).find(mode) != Text::NoPos) return setting;
                return false;
        }

        Text Isupport::withoutStatusPrefix(Text target) const {
                size_t i = 0;
                while (i < target.size() && Text(_prefixChars).find(target[i]) != Text::NoPos) ++i;
                return i > 0 && isChannel(target.mid(i)) ? target.mid(i) : target;
        }

} // namespace irc
