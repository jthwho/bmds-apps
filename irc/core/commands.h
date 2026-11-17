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
                        };
                        Action   action = Action::None;
                        uint32_t buffer = 0;
        };

        /** @brief The commands (without their `/`), as /help lists them and Tab completes them. */
        extern const char *const CommandNames[];
        extern const size_t      CommandCount;

        /**
         * @brief Does what the user typed in buffer @p current (devplan/irc.md §3): text said to
         *        the channel or person in view (`//text`: text starting with `/`), or a command:
         *        `/me /msg /query /notice /join /part /close /nick /topic /away /back /whois /mode
         *        /kick /invite /names /quit /raw /quote /clear /ns /cs /help`; any other `/cmd args`
         *        sent as it is (`CMD args`). A mistake (no target, a line break) is told in
         *        @p current as a line of @p lines, and isn't an error.
         * @return `Ok` or `NoMem`.
         */
        Error runCommand(Text typed, Session &session, Buffers &buffers, uint32_t current, const Now &now,
                         Vector<Line> &lines, Outcome &outcome);

} // namespace irc
