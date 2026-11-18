/**
 * @file      ignore.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "isupport.h"
#include "str.h"
#include "text.h"

#include <cstddef>
#include <cstdint>

namespace irc {

        /**
         * @brief Returns true if @p text matches the mask @p mask (`*` any run, `?` any one
         *        character), names compared under @p mapping. Without recursion: at worst the
         *        product of their sizes (a fuzzed mask can't stall it or overflow the stack).
         */
        bool matchesMask(Text mask, Text text, CaseMapping mapping);

        /**
         * @brief The people ignored (devplan/irc.md §7f): masks `nick!user@host`, each for every
         *        network or one (`settings.ini`'s `[ignore] masks`: `a!*@*, *!*@host@Libera.Chat`, a
         *        second `@` naming the network).
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class Ignores {
                public:
                        /**
                         * @brief Makes @p spec a full mask in @p out: `nick` is `nick!*@*`, `nick!user`
                         *        gains `@*`, `user@host` gains `*!`.
                         * @return `Ok`, `Invalid` (empty, or a space, comma or line break in it) or `NoMem`.
                         */
                        static Error normalize(Text spec, String &out);

                        /**
                         * @brief Ignores @p spec (normalized) on @p network (empty: every network).
                         * @return `Ok` (already there too), `Invalid` or `NoMem`.
                         */
                        Error add(Text spec, Text network);

                        /** @brief Stops ignoring @p spec on @p network. @return True if it was there. */
                        bool remove(Text spec, Text network);

                        /**
                         * @brief Returns true if `nick!user@host` is ignored on @p network (an empty
                         *        user or host, not known, matches only `*`).
                         */
                        bool matches(Text nick, Text user, Text host, Text network, CaseMapping mapping) const;

                        size_t size() const { return _masks.size(); }
                        Text   maskAt(size_t i) const { return _masks[i].mask; }
                        Text   networkAt(size_t i) const { return _masks[i].network; }

                        /** @brief Reads `settings.ini`'s list (a comma between masks). @return `Ok` or `NoMem`. */
                        Error read(Text list);

                        /** @brief Writes the list as `read()` takes it. @return `Ok` or `NoMem`. */
                        Error write(String &out) const;

                private:
                        struct Entry {
                                        String mask;
                                        String network;
                        };
                        Vector<Entry> _masks;
        };

} // namespace irc
