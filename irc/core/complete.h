/**
 * @file      complete.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "buffers.h"
#include "session.h"
#include "str.h"
#include "text.h"

#include <cstddef>
#include <cstdint>

namespace irc {

        /**
         * @brief Tab in the input line (devplan/irc.md §3): the word before the cursor completed,
         *        and each Tab after it the next match. A command at the line's start (`/jo`), a
         *        channel (`#bm`: the session's), or a nick: in a channel those who spoke last
         *        first, then its other members by name (not ours); in a private chat the other's.
         *        A nick at the line's start gets `: ` after it, anything else a space.
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class Completer {
                public:
                        /**
                         * @brief Completes @p line at @p cursor (bytes) in buffer @p current: both
                         *        changed. @return True if it did (false: no match; nothing changed).
                         */
                        bool complete(String &line, size_t &cursor, const Session &session, const Buffers &buffers,
                                      uint32_t current);

                        /** @brief Forgets the matches (anything typed but Tab). */
                        void reset();

                private:
                        bool apply(String &line, size_t &cursor);

                        String         _before; // the line before the word
                        String         _after;  // the line after the cursor
                        Vector<String> _matches;
                        size_t         _index = 0;
                        bool           _atStart = false;
                        bool           _nicks = false; // completing nicks (": " after one at the start)
                        String         _last;          // the line as we left it: a Tab on it cycles
                        size_t         _lastCursor = 0;
        };

} // namespace irc
