/**
 * @file      clipboard.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The Extended Clipboard's messages (rfbproto's "Extended Clipboard Pseudo-Encoding"): text only.
 */

#include "clipboard.h"

#include <cstring>
#include <zlib.h>

namespace vnc {

        namespace ExtendedClipboard {

                namespace {

                        constexpr uint8_t ServerCutText = 3;
                        // A provide's deflate: a small window and memory (about 24 KB while it
                        // runs); any inflate takes it.
                        constexpr int WindowBits = 11;
                        constexpr int MemLevel = 6;
                        constexpr int Level = 6;

                        void writeHeader(Writer &out, uint32_t bytes) {
                                out.u8(ServerCutText);
                                out.zeros(3);
                                out.s32(-int32_t(bytes)); // negative: the extended form
                                return;
                        }

                        // Deflates @p n bytes at @p in into what is left of @p out.
                        bool deflateInto(z_stream &z, Writer &out, const uint8_t *in, size_t n, int flush) {
                                z.next_in = const_cast<uint8_t *>(in);
                                z.avail_in = uInt(n);
                                do {
                                        const size_t room = out.room();
                                        if (room == 0) return false;
                                        z.next_out = out.data() + out.size();
                                        z.avail_out = uInt(room);
                                        const int r = deflate(&z, flush);
                                        (void)out.reserve(room - z.avail_out);
                                        if (r == Z_STREAM_END) return true;
                                        if (r != Z_OK && r != Z_BUF_ERROR) return false;
                                        if (z.avail_out == 0) return false; // out of room before the end
                                } while (z.avail_in > 0 || flush == Z_FINISH);
                                return true;
                        }

                } // namespace

                uint32_t capsTextMax(uint32_t flags, const uint8_t *data, size_t length) {
                        // One size a format, in the formats' order: text is the first.
                        if ((flags & Text) == 0 || length < 4) return 0;
                        return readU32(data);
                }

                void writeCaps(Writer &out, uint32_t actions, uint32_t textMax) {
                        writeHeader(out, 8);
                        out.u32(Caps | actions | Text);
                        out.u32(textMax);
                        return;
                }

                void writeFlags(Writer &out, uint32_t flags) {
                        writeHeader(out, 4);
                        out.u32(flags);
                        return;
                }

                bool writeText(Writer &out, const char *text, size_t length) {
                        // Its size in the stream: CR LF for each lone LF, and the NUL.
                        size_t size = length + 1;
                        for (size_t i = 0; i < length; ++i)
                                if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) ++size;
                        const size_t start = out.size();
                        writeHeader(out, 0); // its length once known
                        out.u32(Provide | Text);
                        if (out.overflowed()) {
                                out.truncate(start);
                                return false;
                        }
                        z_stream z = {};
                        if (deflateInit2(&z, Level, Z_DEFLATED, WindowBits, MemLevel, Z_DEFAULT_STRATEGY) != Z_OK) {
                                out.truncate(start);
                                return false;
                        }
                        const uint8_t sizeBytes[4] = {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8),
                                                      uint8_t(size)};
                        bool          ok = deflateInto(z, out, sizeBytes, 4, Z_NO_FLUSH);
                        // The text through a small buffer, its LFs made CR LF.
                        uint8_t chunk[512];
                        size_t  n = 0;
                        for (size_t i = 0; i < length && ok; ++i) {
                                if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) chunk[n++] = '\r';
                                chunk[n++] = uint8_t(text[i]);
                                if (n >= sizeof(chunk) - 1) {
                                        ok = deflateInto(z, out, chunk, n, Z_NO_FLUSH);
                                        n = 0;
                                }
                        }
                        chunk[n++] = '\0';
                        ok = ok && deflateInto(z, out, chunk, n, Z_FINISH);
                        deflateEnd(&z);
                        if (!ok || out.overflowed()) {
                                out.truncate(start);
                                return false;
                        }
                        // The message's length: the flags and the stream.
                        const uint32_t bytes = uint32_t(out.size() - start - 8);
                        const uint32_t negative = uint32_t(-int32_t(bytes));
                        uint8_t       *p = out.data() + start + 4;
                        p[0] = uint8_t(negative >> 24);
                        p[1] = uint8_t(negative >> 16);
                        p[2] = uint8_t(negative >> 8);
                        p[3] = uint8_t(negative);
                        return true;
                }

                long readText(uint32_t flags, const uint8_t *data, size_t length, char *out, size_t capacity) {
                        if ((flags & Text) == 0) return -1;
                        z_stream z = {};
                        if (inflateInit(&z) != Z_OK) return -1;
                        z.next_in = const_cast<uint8_t *>(data);
                        z.avail_in = uInt(length);
                        // Text's pair first: its size, then its bytes. A viewer's flush may not end
                        // the stream: what was asked for is enough.
                        uint8_t sizeBytes[4];
                        z.next_out = sizeBytes;
                        z.avail_out = 4;
                        int  r = inflate(&z, Z_SYNC_FLUSH);
                        long result = -1;
                        if ((r == Z_OK || r == Z_STREAM_END) && z.avail_out == 0) {
                                const uint32_t size = readU32(sizeBytes);
                                if (size <= capacity) {
                                        z.next_out = reinterpret_cast<uint8_t *>(out);
                                        z.avail_out = uInt(size);
                                        r = size == 0 ? Z_OK : inflate(&z, Z_SYNC_FLUSH);
                                        if ((r == Z_OK || r == Z_STREAM_END) && z.avail_out == 0) result = long(size);
                                }
                        }
                        inflateEnd(&z);
                        if (result < 0) return -1;
                        // Up to the NUL; CRs before LFs dropped.
                        size_t at = 0;
                        for (size_t i = 0; i < size_t(result) && out[i] != '\0'; ++i) {
                                if (out[i] == '\r' && i + 1 < size_t(result) && out[i + 1] == '\n') continue;
                                out[at++] = out[i];
                        }
                        return long(at);
                }

        } // namespace ExtendedClipboard

} // namespace vnc
