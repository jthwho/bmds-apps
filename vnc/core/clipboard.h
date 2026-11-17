/**
 * @file      clipboard.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "bytes.h"

#include <cstddef>
#include <cstdint>

namespace vnc {

        /**
         * @brief The Extended Clipboard (rfbproto's pseudo-encoding 0xC0A1E5CE): cut text as UTF-8,
         *        in ServerCutText and ClientCutText messages with a negative length, a flags word
         *        first (devplan/vnc.md §23). Text only here: its lines end CR LF and a NUL follows
         *        it; a provide's data is a zlib stream of its own.
         */
        namespace ExtendedClipboard {

                // The flags: formats in the low 16 bits, actions in the top 8.
                inline constexpr uint32_t Text = 1u << 0;
                inline constexpr uint32_t Rtf = 1u << 1;
                inline constexpr uint32_t Html = 1u << 2;
                inline constexpr uint32_t Formats = 0xffffu;
                inline constexpr uint32_t Caps = 1u << 24;
                inline constexpr uint32_t Request = 1u << 25;
                inline constexpr uint32_t Peek = 1u << 26;
                inline constexpr uint32_t Notify = 1u << 27;
                inline constexpr uint32_t Provide = 1u << 28;

                /** @brief What a viewer that sends no caps is taken to have (the spec's defaults). */
                inline constexpr uint32_t DefaultFlags = Text | Rtf | Html | Request | Notify | Provide;
                inline constexpr uint32_t DefaultTextMax = 20u * 1024 * 1024;

                /**
                 * @brief A caps message's text size (the most text the sender takes unsolicited):
                 *        @p data, @p length the bytes after its flags.
                 * @return The size, or 0 if text isn't among its formats (or the message is short).
                 */
                uint32_t capsTextMax(uint32_t flags, const uint8_t *data, size_t length);

                /** @brief ServerCutText with caps: text alone, @p actions, its size @p textMax. */
                void writeCaps(Writer &out, uint32_t actions, uint32_t textMax);

                /** @brief ServerCutText with flags alone (a request, a peek, a notify). */
                void writeFlags(Writer &out, uint32_t flags);

                /**
                 * @brief ServerCutText providing @p length bytes of UTF-8 text at @p text (lines ending
                 *        LF, or CR LF): sent CR LF, a NUL after it, compressed.
                 * @return False if it didn't fit in @p out (nothing written) or zlib failed.
                 */
                bool writeText(Writer &out, const char *text, size_t length);

                /**
                 * @brief A provide's text: @p data, @p length the bytes after its flags (its zlib
                 *        stream), inflated into @p out as UTF-8 with lines ending LF, no NUL.
                 * @return The bytes written, or -1: no text in it, longer than @p capacity, or broken.
                 */
                long readText(uint32_t flags, const uint8_t *data, size_t length, char *out, size_t capacity);

        } // namespace ExtendedClipboard

} // namespace vnc
