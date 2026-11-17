/**
 * @file      text.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "text.h"

#include <cstdint>

namespace vnc {

        namespace Text {

                size_t utf8FromLatin1(const char *in, size_t length, char *out, size_t capacity) {
                        size_t at = 0;
                        for (size_t i = 0; i < length; ++i) {
                                const uint8_t c = uint8_t(in[i]);
                                if (c == '\r' && i + 1 < length && in[i + 1] == '\n') continue;
                                if (c < 0x80) {
                                        if (at + 1 > capacity) break;
                                        out[at++] = char(c);
                                } else {
                                        if (at + 2 > capacity) break;
                                        out[at++] = char(0xc0 | (c >> 6));
                                        out[at++] = char(0x80 | (c & 0x3f));
                                }
                        }
                        return at;
                }

                size_t latin1FromUtf8(const char *in, size_t length, char *out, size_t capacity) {
                        size_t at = 0;
                        for (size_t i = 0; i < length && at < capacity;) {
                                const uint8_t c = uint8_t(in[i]);
                                // The sequence's length from its lead byte (a stray continuation: 1).
                                const size_t n = c < 0x80             ? 1
                                                 : (c & 0xe0) == 0xc0 ? 2
                                                 : (c & 0xf0) == 0xe0 ? 3
                                                 : (c & 0xf8) == 0xf0 ? 4
                                                                      : 1;
                                bool         whole = i + n <= length;
                                for (size_t k = 1; k < n && whole; ++k) whole = (uint8_t(in[i + k]) & 0xc0) == 0x80;
                                if (!whole) {
                                        out[at++] = '?';
                                        ++i;
                                        continue;
                                }
                                uint32_t cp = n == 1 ? c : c & (0x7f >> n);
                                for (size_t k = 1; k < n; ++k) cp = cp << 6 | (uint8_t(in[i + k]) & 0x3f);
                                const bool ok = n == 1 ? c < 0x80 : cp >= 0x80 && cp <= 0xff && n == 2;
                                out[at++] = ok ? char(cp) : '?';
                                i += n;
                        }
                        return at;
                }

        } // namespace Text

} // namespace vnc
