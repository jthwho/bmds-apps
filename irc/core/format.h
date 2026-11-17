/**
 * @file      format.h
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
         * @brief A message's text as IRC has it (devplan/irc.md §3, §7): mIRC's codes kept as they
         *        came (BMDS's ChatView draws them); taken out for the logs' plain text, a
         *        notification and finding a mention; text that isn't UTF-8 read as Latin-1; CTCP
         *        (`\x01ACTION waves\x01`); IRCv3's `server-time`.
         *
         * @par Thread Safety
         * Reentrant.
         */
        namespace Format {

                /**
                 * @brief Writes @p text without mIRC's codes (`\x02` bold, `\x1d`, `\x1f`, `\x1e`,
                 *        `\x11`, `\x16`, `\x0f`, `\x03FG[,BG]`, `\x04RRGGBB[,RRGGBB]`) into @p out.
                 * @return `Ok` or `NoMem`.
                 */
                Error strip(Text text, String &out);

                /** @brief Returns true if @p text is UTF-8 (no overlong forms or surrogates). */
                bool isUtf8(Text text);

                /**
                 * @brief Writes @p text into @p out as UTF-8: as it is if it is UTF-8 already, else
                 *        read as Latin-1 (each byte past 0x7f its character). NULs are dropped.
                 * @return `Ok` or `NoMem`.
                 */
                Error toUtf8(Text text, String &out);

                /** @brief A CTCP request or reply: `\x01COMMAND args\x01`. */
                struct Ctcp {
                                Text command; ///< `ACTION`, `VERSION`, `PING`...
                                Text args;
                };

                /**
                 * @brief Returns true if @p text is a CTCP message (starts with `\x01`; the closing
                 *        one may be missing), its parts in @p out.
                 */
                bool parseCtcp(Text text, Ctcp &out);

                /**
                 * @brief Returns true if @p text mentions @p nick: as a word of its own (between
                 *        characters that can't be in a nick), compared under @p mapping, the codes
                 *        ignored.
                 */
                bool mentions(Text text, Text nick, CaseMapping mapping);

                /**
                 * @brief Reads IRCv3's `time` tag (`2026-11-16T10:41:00.123Z`) into @p ms since 1970
                 *        (UTC). @return True if it was one.
                 */
                bool parseServerTime(Text time, int64_t &ms);

                /** @brief Writes @p bytes as base64 (SASL's) into @p out. @return `Ok` or `NoMem`. */
                Error base64(Text bytes, String &out);

        } // namespace Format

} // namespace irc
