/**
 * @file      isupport.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "message.h"
#include "text.h"

#include <cstddef>
#include <cstdint>

namespace irc {

        /** @brief How a network compares names (ISUPPORT's `CASEMAPPING`). */
        enum class CaseMapping : uint8_t {
                Ascii,         ///< A-Z as a-z.
                Rfc1459,       ///< And []\~ as {}|^ (the default: what a server that says nothing means).
                StrictRfc1459, ///< And []\ as {}|.
        };

        /** @brief Returns @p c folded to its lower case under @p mapping. */
        constexpr char foldCase(char c, CaseMapping mapping) {
                if (c >= 'A' && c <= 'Z') return char(c + 32);
                if (mapping == CaseMapping::Ascii) return c;
                if (c == '[') return '{';
                if (c == ']') return '}';
                if (c == '\\') return '|';
                if (c == '~' && mapping == CaseMapping::Rfc1459) return '^';
                return c;
        }

        /** @brief Returns true if @p a and @p b are the same name under @p mapping. */
        bool sameName(Text a, Text b, CaseMapping mapping);

        /**
         * @brief What a server says it supports (`005 RPL_ISUPPORT`), as the session needs it: the
         *        casemapping, the network's name, which names are channels, the member prefixes and
         *        which channel modes take a parameter. Fixed sizes (no allocation): a token longer
         *        than its room is cut.
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class Isupport {
                public:
                        Isupport() { reset(); }

                        /** @brief Back to what a server that says nothing means. */
                        void reset();

                        /** @brief Takes in a `005`'s tokens (`-TOKEN` back to the default). */
                        void apply(const Message &message);

                        CaseMapping caseMapping() const { return _caseMapping; }
                        Text        network() const { return Text(_network); }
                        Text        prefixModes() const { return Text(_prefixModes); }
                        Text        prefixChars() const { return Text(_prefixChars); }
                        Text        chanTypes() const { return Text(_chanTypes); }
                        /** @brief `ELIST`'s letters: what `LIST` filters at the server (`U`: by users). */
                        Text elist() const { return Text(_elist); }
                        /** @brief `MONITOR`'s limit: -1 without it, 0 without a limit. */
                        int32_t monitor() const { return _monitor; }
                        /** @brief `MODES`: the parameter modes one `MODE` may carry (3 by default). */
                        uint32_t modes() const { return _modes; }

                        /** @brief Returns true if @p mode is a list mode (`CHANMODES` A: bans...). */
                        bool isListMode(char mode) const;

                        /** @brief Returns true if @p name is a channel's (its first character a `CHANTYPES`). */
                        bool isChannel(Text name) const;

                        /** @brief Returns the prefix character mode @p mode gives (`o`: `@`), or 0. */
                        char prefixFor(char mode) const;

                        /** @brief Returns @p prefix's rank: 0 the highest, past the prefixes for a non-prefix. */
                        size_t rank(char prefix) const;

                        /** @brief Returns true if channel mode @p mode takes a parameter when set or unset. */
                        bool takesParameter(char mode, bool setting) const;

                        /**
                         * @brief Returns @p target without a `STATUSMSG` prefix (`@#chan`: `#chan`) when the
                         *        rest is a channel.
                         */
                        Text withoutStatusPrefix(Text target) const;

                private:
                        static void put(char *out, size_t room, Text value);

                        CaseMapping _caseMapping = CaseMapping::Rfc1459;
                        char        _network[64];
                        char        _prefixModes[16];
                        char        _prefixChars[16];
                        char        _chanTypes[16];
                        char        _listModes[48];   ///< CHANMODES A: lists, always a parameter.
                        char        _alwaysModes[48]; ///< B: always a parameter.
                        char        _setModes[48];    ///< C: a parameter when set.
                        char        _elist[16];
                        int32_t     _monitor = -1;
                        uint32_t    _modes = 3;
        };

} // namespace irc
