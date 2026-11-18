/**
 * @file      commands.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "buffers.h"
#include "session.h"
#include "text.h"

#include <cstddef>
#include <cstdint>

namespace irc {

        /** @brief What the window does after a command, past what the session sends. */
        struct Outcome {
                        enum class Action : uint8_t {
                                None,
                                Show,  ///< Show `buffer` (a private chat opened by /query).
                                Clear, ///< Empty `buffer`'s view.
                                Close, ///< Close `buffer` (its channel left first).
                                Quit,  ///< The network quit: no reconnecting.
                                List,  ///< Open the channel list, `text` its filter (/list).
                                Find,  ///< Find `text` in `buffer`'s view (/find).
                                Ignore, ///< `on`: ignore `text` (a mask) on network `extra` (empty: all); not `on`: stop; `text` empty: list them.
                                Notify, ///< `on`: watch `text`; not `on`: stop; `text` empty: list them.
                                Server, ///< Connect to `text`, port `port`, TLS `on`; `fresh`: as a new network (/server -m).
                        };
                        Action   action = Action::None;
                        uint32_t buffer = 0;
                        String   text;
                        String   extra;
                        uint16_t port = 0;
                        bool     on = false;
                        bool     fresh = false;
        };

        /**
         * @brief A ban mask for @p nick (devplan/irc.md §7e): `*!*@host` when their `user@host`
         *        (@p host) is known, else `nick!*@*`; a mask given (a `!` or `@` in it) as it is.
         */
        Error banMask(Text nick, Text host, String &out);

        /**
         * @brief Makes a Perform line (devplan/irc.md §7f) a command to run: `$me` our nick
         *        (@p nick), a `/` before it if it has none (a line said is a command too).
         * @return `Ok`, `Invalid` (empty) or `NoMem`.
         */
        Error performLine(Text line, Text nick, String &out);

        /** @brief The commands (without their `/`), as /help lists them and Tab completes them. */
        extern const char *const CommandNames[];
        extern const size_t      CommandCount;

        /**
         * @brief Does what the user typed in buffer @p current (devplan/irc.md §3): text said to
         *        the channel or person in view (`//text`: text starting with `/`), or a command:
         *        `CommandNames`' (§7i: the operator's, CTCP, /ignore, /notify, /list, /find,
         *        /server among them); any other `/cmd args` sent as it is (`CMD args`). A mistake (no target, a line break) is told in
         *        @p current as a line of @p lines, and isn't an error.
         * @return `Ok` or `NoMem`.
         */
        Error runCommand(Text typed, Session &session, Buffers &buffers, uint32_t current, const Now &now,
                         Vector<Line> &lines, Outcome &outcome);

} // namespace irc
