/**
 * @file      text.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>

namespace vnc {

        /**
         * @brief RFB's cut text is Latin-1 (RFB 3.8 §7.5.6, §7.6.4); BMDS's clipboard is UTF-8.
         *        Lines end with LF in both (a CR before an LF is dropped on the way in).
         */
        namespace Text {

                /**
                 * @brief @p length bytes of Latin-1 at @p in as UTF-8 into @p out (@p capacity bytes:
                 *        twice @p length always fits). CRs before LFs are dropped.
                 * @return The bytes written (stops before a character that doesn't fit).
                 */
                size_t utf8FromLatin1(const char *in, size_t length, char *out, size_t capacity);

                /**
                 * @brief @p length bytes of UTF-8 at @p in as Latin-1 into @p out (@p capacity bytes:
                 *        @p length always fits). A character Latin-1 hasn't, or a malformed sequence,
                 *        becomes '?'.
                 * @return The bytes written.
                 */
                size_t latin1FromUtf8(const char *in, size_t length, char *out, size_t capacity);

        } // namespace Text

} // namespace vnc
