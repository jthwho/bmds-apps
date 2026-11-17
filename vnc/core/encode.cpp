/**
 * @file      encode.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * RFB's encodings (devplan/vnc.md §5a): Raw (RFB 3.8 §7.7.1), ZRLE (§7.7.6) and Tight (TightVNC's,
 * as libvncserver and TigerVNC speak it), each rectangle given the cheapest kind that is exact,
 * and JPEG only for pictures.
 */

#include "encode.h"
#include "rfb.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <time.h>
#include <zlib.h>

namespace vnc {

        namespace {

                // Tight's control byte (its first): a fill, a JPEG, or basic compression on stream N.
                constexpr uint8_t TightFill = 0x80;
                constexpr uint8_t TightJpeg = 0x90;
                constexpr uint8_t TightExplicitFilter = 0x40;
                constexpr uint8_t TightFilterPalette = 1;
                constexpr size_t  TightMinToCompress = 12; // less is sent as it is

                // Tight's streams: one per kind of data, so each stays alike.
                constexpr int StreamBasic = 0;
                constexpr int StreamMono = 1;
                constexpr int StreamPalette = 2;

                // A palette's colours at most for Tight (a byte an index) and ZRLE (packed).
                constexpr size_t   TightMaxPalette = 16;
                constexpr size_t   ZrleMaxPalette = 16;
                constexpr uint16_t ZrleTile = 64;

                // zlib's window and memory (devplan/vnc.md A3): about 40 KB a stream.
                constexpr int WindowBits = 11;
                constexpr int MemLevel = 6;

                // Tight's compact length: 7 bits a byte, the top bit saying another follows.
                void compactLength(Writer &out, size_t n) {
                        out.u8(uint8_t((n & 0x7f) | (n > 0x7f ? 0x80 : 0)));
                        if (n > 0x7f) {
                                out.u8(uint8_t(((n >> 7) & 0x7f) | (n > 0x3fff ? 0x80 : 0)));
                                if (n > 0x3fff) out.u8(uint8_t(n >> 14));
                        }
                        return;
                }

                // QualityLevel 0-9 as a JPEG's quality, and its sampling: TigerVNC's levels, roughly.
                int jpegQuality(int level) {
                        static constexpr int Quality[10] = {15, 29, 41, 42, 62, 77, 79, 86, 92, 100};
                        return Quality[level < 0 ? 0 : level > 9 ? 9 : level];
                }

                uint64_t nowUs() {
                        struct timespec t;
                        clock_gettime(CLOCK_MONOTONIC, &t);
                        return uint64_t(t.tv_sec) * 1000000u + uint64_t(t.tv_nsec) / 1000u;
                }

        } // namespace

        // ---- ZStream

        bool ZStream::compress(const uint8_t *in, size_t n, int level, bool runs, Writer &out) {
                const int strategy = runs ? Z_RLE : Z_DEFAULT_STRATEGY;
                if (_z == nullptr) {
                        _z = static_cast<z_stream_s *>(std::calloc(1, sizeof(z_stream_s)));
                        if (_z == nullptr) return false;
                        if (deflateInit2(_z, level, Z_DEFLATED, WindowBits, MemLevel, strategy) != Z_OK) {
                                std::free(_z);
                                _z = nullptr;
                                return false;
                        }
                        _level = level;
                        _runs = runs;
                } else if (level != _level || runs != _runs) {
                        // The stream goes on (the viewer's inflate doesn't care about either).
                        if (deflateParams(_z, level, strategy) == Z_OK) {
                                _level = level;
                                _runs = runs;
                        }
                }
                _z->next_in = const_cast<Bytef *>(in);
                _z->avail_in = uInt(n);
                // Until the flush is whole: deflate leaves room in the output once it is.
                for (;;) {
                        const size_t room = out.room();
                        if (room == 0) return false;
                        _z->next_out = out.data() + out.size();
                        _z->avail_out = uInt(room);
                        if (deflate(_z, Z_SYNC_FLUSH) == Z_STREAM_ERROR) return false;
                        (void)out.reserve(room - _z->avail_out);
                        if (_z->avail_out != 0) break;
                }
                return _z->avail_in == 0;
        }

        void ZStream::end() {
                if (_z == nullptr) return;
                deflateEnd(_z);
                std::free(_z);
                _z = nullptr;
                _level = -1;
                return;
        }

        // ---- Encoder

        Encoder::Encoder() = default;

        Encoder::~Encoder() {
                std::free(_work);
                std::free(_packed);
        }

        bool Encoder::init(uint32_t maxPixels) {
                // A rectangle's pixels as 4 bytes at most, and zlib's worst case over it.
                _workBytes = size_t(maxPixels) * 4;
                _packedBytes = _workBytes + _workBytes / 1000 + 64;
                _work = static_cast<uint8_t *>(std::malloc(_workBytes));
                _packed = static_cast<uint8_t *>(std::malloc(_packedBytes));
                return _work != nullptr && _packed != nullptr;
        }

        void Encoder::setEncodings(const int32_t *encodings, size_t count) {
                _encoding = Encoding::Raw;
                _quality = -1;
                _compress = 6;
                _lastRect = false;
                _desktopSize = false;
                bool chosen = false;
                for (size_t i = 0; i < count; ++i) {
                        const int32_t e = encodings[i];
                        if (!chosen && (e == Encoding::Tight || e == Encoding::Zrle || e == Encoding::Raw)) {
                                _encoding = e; // the viewer's first that this server has
                                chosen = true;
                        } else if (e >= Encoding::QualityLevel0 && e <= Encoding::QualityLevel0 + 9) {
                                _quality = int(e - Encoding::QualityLevel0);
                        } else if (e >= Encoding::CompressLevel0 && e <= Encoding::CompressLevel0 + 9) {
                                _compress = int(e - Encoding::CompressLevel0);
                        } else if (e == Encoding::LastRect) {
                                _lastRect = true;
                        } else if (e == Encoding::DesktopSize) {
                                _desktopSize = true;
                        }
                }
                return;
        }

        size_t Encoder::worstCase(uint16_t width, uint16_t height) const {
                const size_t pixels = size_t(width) * height;
                // Tight's pieces each cost a header, a control byte, a palette and zlib's overhead; a
                // JPEG is held to its pixels' size (jpegRoom()).
                // Solid areas cut a rectangle into at most four more each, and a fill each.
                const size_t pieces = pixels / (TightMaxPixels / 4) + size_t(width) / TightMaxWidth + 2 +
                                      5 * (pixels / SolidMinPixels + 1);
                return pieces * (12 + 1 + 3 + 256 * 4 + 64) + jpegRoom(pixels) + 64;
        }

        size_t Encoder::jpegRoom(size_t pixels) {
                return pixels * 4 + pixels / 100 + 1024;
        }

        size_t Encoder::encode(const Picture &picture, uint16_t x, uint16_t y, Writer &out) {
                if (picture.width == 0 || picture.height == 0 || _broken) return 0;
                const uint64_t started = nowUs();
                const size_t   before = out.size();
                size_t         rects = 0;
                if (_encoding == Encoding::Tight) {
                        rects = tightSolids(picture, x, y, out);
                } else if (_encoding == Encoding::Zrle) {
                        rects = zrle(picture, x, y, out) ? 1 : 0;
                } else {
                        rects = raw(picture, x, y, out) ? 1 : 0;
                }
                if (rects == 0 || out.overflowed()) {
                        out.truncate(before);
                        // What the streams took was never sent: Tight's start again (told with the
                        // next rectangle); ZRLE's can't.
                        if (_encoding == Encoding::Tight) {
                                for (int i = 0; i < 4; ++i) _tight[i].end();
                                _tightResets = 0x0f;
                        } else if (_encoding == Encoding::Zrle) {
                                _broken = true;
                        }
                        return 0;
                }
                _stats.pixels += uint64_t(picture.width) * picture.height;
                _stats.bytes += out.size() - before;
                _stats.encodeUs += nowUs() - started;
                return rects;
        }

        // The colours of @p picture into the palette, at most @p limit (limit + 1: more than that).
        // The hot loops below read pixels off row pointers (B, G, R) rather than `Picture::rgb()`'s
        // multiply a pixel, and keep the last colour's answer: a desktop's runs are long.
        size_t Encoder::countColours(const Picture &picture, size_t limit, uint8_t *indices) {
                _paletteCount = 0;
                uint32_t last = 0xffffffffu;
                uint8_t  index = 0;
                for (uint16_t row = 0; row < picture.height; ++row) {
                        const uint8_t *p = picture.pixels + size_t(row) * picture.stride;
                        for (uint16_t col = 0; col < picture.width; ++col, p += 3) {
                                const uint32_t c = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
                                if (c != last) { // runs are common: the search skipped
                                        last = c;
                                        size_t i = 0;
                                        while (i < _paletteCount && _palette[i] != c) ++i;
                                        if (i == _paletteCount) {
                                                if (_paletteCount == limit) return limit + 1;
                                                _palette[_paletteCount++] = c;
                                        }
                                        index = uint8_t(i);
                                }
                                if (indices != nullptr) *indices++ = index;
                        }
                }
                return _paletteCount;
        }

        uint8_t Encoder::indexOf(uint32_t rgb) const {
                for (size_t i = 0; i < _paletteCount; ++i)
                        if (_palette[i] == rgb) return uint8_t(i);
                return 0;
        }

        void Encoder::indexRow(const Picture &picture, uint16_t row, uint8_t *out) const {
                const uint8_t *p = picture.pixels + size_t(row) * picture.stride;
                uint32_t       last = 0xffffffffu;
                uint8_t        index = 0;
                for (uint16_t col = 0; col < picture.width; ++col, p += 3) {
                        const uint32_t c = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
                        if (c != last) {
                                last = c;
                                index = indexOf(c);
                        }
                        out[col] = index;
                }
                return;
        }

        // ---- Raw

        bool Encoder::raw(const Picture &picture, uint16_t x, uint16_t y, Writer &out) {
                Session::writeRectHeader(out, x, y, picture.width, picture.height, Encoding::Raw);
                const size_t bpp = _format.bytesPerPixel();
                uint8_t     *p = out.reserve(size_t(picture.width) * picture.height * bpp);
                if (p == nullptr) return false;
                const bool bgrx = _format.isBgrx();
                for (uint16_t row = 0; row < picture.height; ++row) {
                        const uint8_t *src = picture.pixels + size_t(row) * picture.stride;
                        if (bgrx) { // the usual: B, G, R as they are, and a zero
                                for (uint16_t col = 0; col < picture.width; ++col, src += 3, p += 4) {
                                        p[0] = src[0];
                                        p[1] = src[1];
                                        p[2] = src[2];
                                        p[3] = 0;
                                }
                                continue;
                        }
                        for (uint16_t col = 0; col < picture.width; ++col, p += bpp)
                                _format.storePixel(picture.rgb(col, row), p);
                }
                ++_stats.raw;
                return true;
        }

        // ---- ZRLE: 64 × 64 tiles, each solid, a packed palette, or raw CPIXELs; all through one stream.

        bool Encoder::zrle(const Picture &picture, uint16_t x, uint16_t y, Writer &out) {
                Session::writeRectHeader(out, x, y, picture.width, picture.height, Encoding::Zrle);
                const size_t cpx = _format.cpixelBytes();
                // The tiles' bytes into the work buffer a band of tiles at a time, compressed as it fills.
                const size_t lengthAt = out.size();
                out.u32(0); // the compressed length, written once known
                if (out.overflowed()) return false;
                const size_t dataStart = out.size();
                Writer       tiles(_work, _workBytes);
                for (uint16_t ty = 0; ty < picture.height; ty = uint16_t(ty + ZrleTile)) {
                        const uint16_t th = uint16_t(picture.height - ty < ZrleTile ? picture.height - ty : ZrleTile);
                        for (uint16_t tx = 0; tx < picture.width; tx = uint16_t(tx + ZrleTile)) {
                                const uint16_t tw =
                                        uint16_t(picture.width - tx < ZrleTile ? picture.width - tx : ZrleTile);
                                const Picture tile = picture.part(tx, ty, tw, th);
                                // A tile's worst case: raw CPIXELs and its byte.
                                if (tiles.room() < size_t(tw) * th * cpx + 1 + ZrleMaxPalette * cpx) {
                                        const uint64_t z0 = nowUs();
                                        const bool     ok = _zrle.compress(tiles.data(), tiles.size(), _compress, out);
                                        _stats.zlibUs += nowUs() - z0;
                                        if (!ok) return false;
                                        tiles.clear();
                                }
                                const size_t colours = countColours(tile, ZrleMaxPalette);
                                if (colours == 1) {
                                        tiles.u8(1);
                                        _format.storeCpixel(_palette[0], tiles.reserve(cpx));
                                } else if (colours <= ZrleMaxPalette) {
                                        tiles.u8(uint8_t(colours));
                                        for (size_t i = 0; i < colours; ++i)
                                                _format.storeCpixel(_palette[i], tiles.reserve(cpx));
                                        const unsigned bits = colours <= 2 ? 1 : colours <= 4 ? 2 : 4;
                                        const size_t   rowBytes = (size_t(tw) * bits + 7) / 8;
                                        uint8_t       *q = tiles.reserve(rowBytes * th); // room checked above
                                        uint8_t        indices[ZrleTile];
                                        for (uint16_t row = 0; row < th; ++row, q += rowBytes) {
                                                indexRow(tile, row, indices);
                                                unsigned byte = 0, filled = 0;
                                                uint8_t *b = q;
                                                for (uint16_t col = 0; col < tw; ++col) {
                                                        byte = byte << bits | indices[col];
                                                        filled += bits;
                                                        if (filled == 8) {
                                                                *b++ = uint8_t(byte);
                                                                byte = 0;
                                                                filled = 0;
                                                        }
                                                }
                                                if (filled != 0) *b = uint8_t(byte << (8 - filled));
                                        }
                                } else {
                                        tiles.u8(0);
                                        uint8_t   *p = tiles.reserve(size_t(tw) * th * cpx);
                                        const bool bgr = cpx == 3 && _format.isBgrx(); // a CPIXEL is B, G, R
                                        for (uint16_t row = 0; row < th; ++row) {
                                                if (bgr) {
                                                        std::memcpy(p, tile.pixels + size_t(row) * tile.stride,
                                                                    size_t(tw) * 3);
                                                        p += size_t(tw) * 3;
                                                        continue;
                                                }
                                                for (uint16_t col = 0; col < tw; ++col, p += cpx)
                                                        _format.storeCpixel(tile.rgb(col, row), p);
                                        }
                                }
                        }
                }
                const uint64_t z0 = nowUs();
                const bool     zok = tiles.size() == 0 || _zrle.compress(tiles.data(), tiles.size(), _compress, out);
                _stats.zlibUs += nowUs() - z0;
                if (!zok) return false;
                const size_t n = out.size() - dataStart;
                uint8_t     *len = out.data() + lengthAt;
                len[0] = uint8_t(n >> 24);
                len[1] = uint8_t(n >> 16);
                len[2] = uint8_t(n >> 8);
                len[3] = uint8_t(n);
                ++_stats.zrle;
                return true;
        }

        // ---- Tight

        // ---- Tight's solid areas (libvncserver's findBestSolidArea, simplified): a big rectangle is
        // searched on a 16-pixel grid for a solid block; the first is grown right, then down, while it
        // stays solid; big enough, it goes as one fill and the four rectangles around it (above,
        // left, right, below) are encoded the same way. A flat area then costs a few bytes rather than
        // a byte a pixel through zlib.

        bool Encoder::isSolid(const Picture &picture, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              uint32_t rgb) const {
                const uint8_t b = uint8_t(rgb), g = uint8_t(rgb >> 8), r = uint8_t(rgb >> 16);
                for (uint16_t row = 0; row < h; ++row) {
                        const uint8_t *p = picture.pixels + size_t(y + row) * picture.stride + size_t(x) * 3;
                        for (uint16_t col = 0; col < w; ++col, p += 3)
                                if (p[0] != b || p[1] != g || p[2] != r) return false;
                }
                return true;
        }

        bool Encoder::tightFill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t rgb, Writer &out) {
                Session::writeRectHeader(out, x, y, w, h, Encoding::Tight);
                tightControl(out, TightFill);
                uint8_t *p = out.reserve(_format.tpixelBytes());
                if (p == nullptr) return false;
                _format.storeTpixel(rgb, p);
                ++_stats.fills;
                return true;
        }

        size_t Encoder::tightSolids(const Picture &picture, uint16_t x, uint16_t y, Writer &out) {
                const uint16_t w = picture.width, h = picture.height;
                if (w == 0 || h == 0) return 0;
                if (uint32_t(w) * h < SolidSearchPixels) return encodeTight(picture, x, y, out);
                constexpr uint16_t Block = 16;
                for (uint16_t by = 0; by < h; by = uint16_t(by + Block)) {
                        const uint16_t bh = uint16_t(h - by < Block ? h - by : Block);
                        for (uint16_t bx = 0; bx < w; bx = uint16_t(bx + Block)) {
                                const uint16_t bw = uint16_t(w - bx < Block ? w - bx : Block);
                                const uint32_t c = picture.rgb(bx, by);
                                if (!isSolid(picture, bx, by, bw, bh, c)) continue;
                                // Grown right a block at a time, then down while the whole width holds.
                                uint16_t x1 = uint16_t(bx + bw);
                                while (x1 < w) {
                                        const uint16_t step = uint16_t(w - x1 < Block ? w - x1 : Block);
                                        if (!isSolid(picture, x1, by, step, bh, c)) break;
                                        x1 = uint16_t(x1 + step);
                                }
                                uint16_t y1 = uint16_t(by + bh);
                                while (y1 < h) {
                                        const uint16_t step = uint16_t(h - y1 < Block ? h - y1 : Block);
                                        if (!isSolid(picture, bx, y1, uint16_t(x1 - bx), step, c)) break;
                                        y1 = uint16_t(y1 + step);
                                }
                                if (uint32_t(x1 - bx) * (y1 - by) < SolidMinPixels) continue;
                                // Above, left, the fill, right, below.
                                size_t n = 0;
                                if (by > 0) {
                                        const size_t k = tightSolids(picture.part(0, 0, w, by), x, y, out);
                                        if (k == 0) return 0;
                                        n += k;
                                }
                                if (bx > 0) {
                                        const size_t k = tightSolids(picture.part(0, by, bx, uint16_t(y1 - by)), x,
                                                                     uint16_t(y + by), out);
                                        if (k == 0) return 0;
                                        n += k;
                                }
                                if (!tightFill(uint16_t(x + bx), uint16_t(y + by), uint16_t(x1 - bx), uint16_t(y1 - by),
                                               c, out))
                                        return 0;
                                ++n;
                                if (x1 < w) {
                                        const size_t k =
                                                tightSolids(picture.part(x1, by, uint16_t(w - x1), uint16_t(y1 - by)),
                                                            uint16_t(x + x1), uint16_t(y + by), out);
                                        if (k == 0) return 0;
                                        n += k;
                                }
                                if (y1 < h) {
                                        const size_t k = tightSolids(picture.part(0, y1, w, uint16_t(h - y1)), x,
                                                                     uint16_t(y + y1), out);
                                        if (k == 0) return 0;
                                        n += k;
                                }
                                return n;
                        }
                }
                return encodeTight(picture, x, y, out);
        }

        // Split into rectangles Tight takes (TightMaxWidth wide, TightMaxPixels at most; heights in
        // 16s where it can, so a JPEG's blocks stay whole).
        size_t Encoder::encodeTight(const Picture &picture, uint16_t x, uint16_t y, Writer &out) {
                size_t rects = 0;
                for (uint16_t cx = 0; cx < picture.width; cx = uint16_t(cx + TightMaxWidth)) {
                        const uint16_t w =
                                uint16_t(picture.width - cx < TightMaxWidth ? picture.width - cx : TightMaxWidth);
                        uint32_t rows = TightMaxPixels / w;
                        if (rows >= 16) rows -= rows % 16;
                        if (rows == 0) rows = 1;
                        for (uint16_t cy = 0; cy < picture.height; cy = uint16_t(cy + rows)) {
                                const uint32_t left = uint32_t(picture.height - cy);
                                const uint16_t h = uint16_t(left < rows ? left : rows);
                                if (!tightRect(picture.part(cx, cy, w, h), uint16_t(x + cx), uint16_t(y + cy), out))
                                        return 0;
                                ++rects;
                        }
                }
                return rects;
        }

        bool Encoder::tightRect(const Picture &picture, uint16_t x, uint16_t y, Writer &out) {
                Session::writeRectHeader(out, x, y, picture.width, picture.height, Encoding::Tight);
                // The indices made as the colours are counted (one pass over the pixels): what a
                // palette sends.
                const size_t colours = countColours(picture, TightMaxPalette, _work);
                if (colours == 1) {
                        tightControl(out, TightFill);
                        _format.storeTpixel(_palette[0], out.reserve(_format.tpixelBytes()));
                        ++_stats.fills;
                        return !out.overflowed();
                }
                if (colours <= TightMaxPalette) return tightPalette(picture, colours, out);
                const bool jpegAllowed = _jpeg != nullptr && _quality >= 0 && picture.width >= JpegMinWidth &&
                                         picture.height >= JpegMinHeight;
                if (jpegAllowed) {
                        const size_t  mark = out.size();
                        const uint8_t resets = _tightResets;
                        if (tightJpeg(picture, out)) return true;
                        out.truncate(mark); // the codec failed: its pixels instead
                        _tightResets = resets;
                }
                return tightBasic(picture, out);
        }

        // A palette of 2-16 colours: a bit a pixel for two, a byte for more, on their own streams.
        bool Encoder::tightPalette(const Picture &picture, size_t colours, Writer &out) {
                const int stream = colours == 2 ? StreamMono : StreamPalette;
                tightControl(out, uint8_t(stream << 4 | TightExplicitFilter));
                out.u8(TightFilterPalette);
                out.u8(uint8_t(colours - 1));
                const size_t tpx = _format.tpixelBytes();
                for (size_t i = 0; i < colours; ++i) _format.storeTpixel(_palette[i], out.reserve(tpx));
                if (out.overflowed()) return false;
                // The indices are in the work buffer already (countColours()), a byte a pixel: sent so
                // for more than two colours; for two, packed to a bit a pixel in place (the packed bytes
                // are never ahead of the ones read).
                const size_t pixels = size_t(picture.width) * picture.height;
                size_t       bytes = pixels;
                if (colours == 2) {
                        const size_t   rowBytes = (size_t(picture.width) + 7) / 8;
                        const uint8_t *in = _work;
                        for (uint16_t row = 0; row < picture.height; ++row) {
                                uint8_t *b = _work + size_t(row) * rowBytes;
                                unsigned byte = 0, filled = 0;
                                for (uint16_t col = 0; col < picture.width; ++col) {
                                        byte = byte << 1 | *in++;
                                        if (++filled == 8) {
                                                *b++ = uint8_t(byte);
                                                byte = 0;
                                                filled = 0;
                                        }
                                }
                                if (filled != 0) *b = uint8_t(byte << (8 - filled));
                        }
                        bytes = rowBytes * picture.height;
                }
                Writer data(_work, _workBytes);
                (void)data.reserve(bytes);
                if (data.overflowed() || !tightData(stream, data.data(), data.size(), out)) return false;
                ++_stats.palettes;
                return true;
        }

        // Many colours: the TPIXELs through stream 0.
        bool Encoder::tightBasic(const Picture &picture, Writer &out) {
                tightControl(out, uint8_t(StreamBasic << 4));
                const size_t tpx = _format.tpixelBytes();
                Writer       data(_work, _workBytes);
                uint8_t     *p = data.reserve(size_t(picture.width) * picture.height * tpx);
                if (p == nullptr) return false;
                const bool tight24 = _format.isTight24();
                for (uint16_t row = 0; row < picture.height; ++row) {
                        const uint8_t *src = picture.pixels + size_t(row) * picture.stride;
                        if (tight24) { // R, G, B: the screen's bytes the other way round
                                for (uint16_t col = 0; col < picture.width; ++col, src += 3, p += 3) {
                                        p[0] = src[2];
                                        p[1] = src[1];
                                        p[2] = src[0];
                                }
                                continue;
                        }
                        for (uint16_t col = 0; col < picture.width; ++col, p += tpx)
                                _format.storeTpixel(picture.rgb(col, row), p);
                }
                if (!tightData(StreamBasic, data.data(), data.size(), out)) return false;
                ++_stats.basics;
                return true;
        }

        // A picture's many colours as a JPEG (the board's hardware codec).
        bool Encoder::tightJpeg(const Picture &picture, Writer &out) {
                int quality = jpegQuality(_quality);
                if (quality > _jpegCeiling) quality = _jpegCeiling;
                // Never more than the pixels would take (else they go as they are), nor than is left.
                size_t room = jpegRoom(size_t(picture.width) * picture.height);
                if (room > _packedBytes) room = _packedBytes;
                if (room + 8 > out.room()) room = out.room() > 8 ? out.room() - 8 : 0;
                if (room == 0) return false;
                const long n = _jpeg(_jpegArg, picture, quality, _packed, room);
                if (n <= 0) return false;
                tightControl(out, TightJpeg);
                compactLength(out, size_t(n));
                out.bytes(_packed, size_t(n));
                if (out.overflowed()) return false;
                ++_stats.jpegs;
                return true;
        }

        // A rectangle's control byte, telling the viewer which streams to start again.
        void Encoder::tightControl(Writer &out, uint8_t control) {
                out.u8(uint8_t(control | _tightResets));
                _tightResets = 0;
                return;
        }

        // Tight's data: under 12 bytes as it is, else compressed on @p stream with its length first.
        bool Encoder::tightData(int stream, const uint8_t *data, size_t n, Writer &out) {
                if (n < TightMinToCompress) {
                        out.bytes(data, n);
                        return !out.overflowed();
                }
                Writer         packed(_packed, _packedBytes);
                const uint64_t z0 = nowUs();
                // Indices (the palettes' streams) by runs alone: fast, and as good on them.
                const bool ok = _tight[stream].compress(data, n, _compress, stream != StreamBasic, packed);
                _stats.zlibUs += nowUs() - z0;
                if (!ok) return false;
                compactLength(out, packed.size());
                out.bytes(packed.data(), packed.size());
                return !out.overflowed();
        }

} // namespace vnc
