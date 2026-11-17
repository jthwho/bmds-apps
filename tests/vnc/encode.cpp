/**
 * @file      encode.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The VNC server's encodings (vnc/core/encode.h): each decoded again here, as a viewer would,
 * with zlib's inflate keeping each stream across rectangles, and compared pixel for pixel.
 */

#include "encode.h"
#include "rfb.h"

#include <doctest.h>

#include <cstring>
#include <vector>
#include <zlib.h>

using vnc::Encoder;
using vnc::Picture;
using vnc::Writer;

namespace {

        // A screen of B, G, R bytes.
        struct Screen {
                        uint16_t             width, height;
                        std::vector<uint8_t> bytes;
                        Screen(uint16_t w, uint16_t h) : width(w), height(h), bytes(size_t(w) * h * 3) {}
                        void set(int x, int y, uint32_t rgb) {
                                uint8_t *p = &bytes[(size_t(y) * width + size_t(x)) * 3];
                                p[0] = uint8_t(rgb);
                                p[1] = uint8_t(rgb >> 8);
                                p[2] = uint8_t(rgb >> 16);
                        }
                        uint32_t get(int x, int y) const {
                                const uint8_t *p = &bytes[(size_t(y) * width + size_t(x)) * 3];
                                return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
                        }
                        Picture picture() const { return Picture{bytes.data(), size_t(width) * 3, width, height}; }
        };

        // A desktop-like picture: a flat background, text-like strokes in two colours, and a
        // photo-like gradient in one corner.
        Screen desktop(uint16_t w, uint16_t h) {
                Screen   s(w, h);
                uint32_t seed = 1;
                for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x) {
                                uint32_t c = 0x3a6ea5;
                                if (y > 20 && y < 60 && x % 7 < 2 && (x / 7 + y / 9) % 3 != 0) c = 0x000000;
                                if (y > 80 && y < 120 && x > 10 && x < 300) c = (x + y) % 5 == 0 ? 0xffffff : 0xc0c0c0;
                                if (x >= w / 2 && y >= h / 2) {
                                        seed = seed * 1103515245u + 12345u;
                                        c = uint32_t((x * 255 / w) << 16 | (y * 255 / h) << 8 | ((seed >> 16) & 0xff));
                                }
                                s.set(x, y, c);
                        }
                return s;
        }

        // A viewer's side: reads updates' rectangles back into its own copy of the screen.
        struct Viewer {
                        vnc::PixelFormat format;
                        Screen           screen;
                        z_stream         zrle{}, tight[4]{};
                        bool             zrleOpen = false, tightOpen[4] = {};
                        int              jpegs = 0;

                        Viewer(uint16_t w, uint16_t h) : screen(w, h) {}
                        ~Viewer() {
                                if (zrleOpen) inflateEnd(&zrle);
                                for (int i = 0; i < 4; ++i)
                                        if (tightOpen[i]) inflateEnd(&tight[i]);
                        }

                        static std::vector<uint8_t> inflateSome(z_stream &z, bool &open, const uint8_t *in, size_t n,
                                                                size_t want) {
                                if (!open) {
                                        REQUIRE(inflateInit(&z) == Z_OK);
                                        open = true;
                                }
                                std::vector<uint8_t> out(want);
                                z.next_in = const_cast<uint8_t *>(in);
                                z.avail_in = uInt(n);
                                z.next_out = out.data();
                                z.avail_out = uInt(want);
                                const int r = inflate(&z, Z_SYNC_FLUSH);
                                CHECK((r == Z_OK || r == Z_BUF_ERROR));
                                CHECK(z.avail_in == 0);
                                CHECK(z.avail_out == 0);
                                return out;
                        }

                        uint32_t pixelAt(const uint8_t *p, size_t bytes) const {
                                uint32_t v = 0;
                                for (size_t i = 0; i < bytes; ++i)
                                        v |= uint32_t(p[i]) << (8 * (format.bigEndian ? bytes - 1 - i : i));
                                return v;
                        }
                        // A value back to 0xRRGGBB at 8 bits (exact for 8-bit channels).
                        uint32_t rgbOf(uint32_t v) const {
                                auto up = [](uint32_t c, uint16_t max) {
                                        return max == 255 ? c : c * 255 / max;
                                };
                                return up(v >> format.redShift & format.redMax, format.redMax) << 16 |
                                       up(v >> format.greenShift & format.greenMax, format.greenMax) << 8 |
                                       up(v >> format.blueShift & format.blueMax, format.blueMax);
                        }
                        uint32_t cpixel(const uint8_t *p) const {
                                if (format.cpixelBytes() == 4 || format.bytesPerPixel() != 4)
                                        return rgbOf(pixelAt(p, format.bytesPerPixel()));
                                // Three bytes: the low ones (as this server's formats have them).
                                return rgbOf(pixelAt(p, 3));
                        }
                        uint32_t tpixel(const uint8_t *p) const {
                                if (format.isTight24()) return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
                                return rgbOf(pixelAt(p, format.bytesPerPixel()));
                        }

                        size_t compactLength(const uint8_t *&p) {
                                size_t n = p[0] & 0x7f;
                                if (p[0] & 0x80) {
                                        n |= size_t(p[1] & 0x7f) << 7;
                                        if (p[1] & 0x80) {
                                                n |= size_t(p[2]) << 14;
                                                p += 3;
                                                return n;
                                        }
                                        p += 2;
                                        return n;
                                }
                                p += 1;
                                return n;
                        }

                        // One rectangle at @p p; returns the bytes after it.
                        const uint8_t *rect(const uint8_t *p) {
                                const int     x = vnc::readU16(p), y = vnc::readU16(p + 2);
                                const int     w = vnc::readU16(p + 4), h = vnc::readU16(p + 6);
                                const int32_t enc = int32_t(vnc::readU32(p + 8));
                                p += 12;
                                if (enc == vnc::Encoding::Raw) {
                                        const size_t bpp = format.bytesPerPixel();
                                        for (int r = 0; r < h; ++r)
                                                for (int c = 0; c < w; ++c, p += bpp)
                                                        screen.set(x + c, y + r, rgbOf(pixelAt(p, bpp)));
                                        return p;
                                }
                                if (enc == vnc::Encoding::Zrle) {
                                        const size_t n = vnc::readU32(p);
                                        p += 4;
                                        // Inflate everything the tiles could hold, then walk them.
                                        z_stream &z = zrle;
                                        if (!zrleOpen) {
                                                REQUIRE(inflateInit(&z) == Z_OK);
                                                zrleOpen = true;
                                        }
                                        std::vector<uint8_t> data(size_t(w) * h * 4 + 4096);
                                        z.next_in = const_cast<uint8_t *>(p);
                                        z.avail_in = uInt(n);
                                        z.next_out = data.data();
                                        z.avail_out = uInt(data.size());
                                        const int r = inflate(&z, Z_SYNC_FLUSH);
                                        CHECK((r == Z_OK || r == Z_BUF_ERROR));
                                        CHECK(z.avail_in == 0);
                                        const uint8_t *t = data.data();
                                        const size_t   cpx = format.cpixelBytes();
                                        for (int ty = 0; ty < h; ty += 64)
                                                for (int tx = 0; tx < w; tx += 64) {
                                                        const int tw = w - tx < 64 ? w - tx : 64,
                                                                  th = h - ty < 64 ? h - ty : 64;
                                                        const int sub = *t++;
                                                        if (sub == 1) {
                                                                const uint32_t c = cpixel(t);
                                                                t += cpx;
                                                                for (int r2 = 0; r2 < th; ++r2)
                                                                        for (int c2 = 0; c2 < tw; ++c2)
                                                                                screen.set(x + tx + c2, y + ty + r2, c);
                                                        } else if (sub == 0) {
                                                                for (int r2 = 0; r2 < th; ++r2)
                                                                        for (int c2 = 0; c2 < tw; ++c2, t += cpx)
                                                                                screen.set(x + tx + c2, y + ty + r2,
                                                                                           cpixel(t));
                                                        } else {
                                                                REQUIRE(sub <= 16);
                                                                uint32_t pal[16];
                                                                for (int i = 0; i < sub; ++i, t += cpx)
                                                                        pal[i] = cpixel(t);
                                                                const int bits = sub <= 2 ? 1 : sub <= 4 ? 2 : 4;
                                                                for (int r2 = 0; r2 < th; ++r2) {
                                                                        int bit = 0;
                                                                        for (int c2 = 0; c2 < tw; ++c2) {
                                                                                const int index =
                                                                                        (t[bit / 8] >>
                                                                                         (8 - bits - bit % 8)) &
                                                                                        ((1 << bits) - 1);
                                                                                screen.set(x + tx + c2, y + ty + r2,
                                                                                           pal[index]);
                                                                                bit += bits;
                                                                        }
                                                                        t += (bit + 7) / 8;
                                                                }
                                                        }
                                                }
                                        return p + n;
                                }
                                REQUIRE(enc == vnc::Encoding::Tight);
                                uint8_t control = *p++;
                                for (int i = 0; i < 4; ++i)
                                        if ((control >> i) & 1) { // the server says: start stream i again
                                                if (tightOpen[i]) inflateEnd(&tight[i]);
                                                tightOpen[i] = false;
                                        }
                                control &= 0xf0;
                                const size_t tpx = format.tpixelBytes();
                                if (control == 0x80) {
                                        const uint32_t c = tpixel(p);
                                        for (int r = 0; r < h; ++r)
                                                for (int c2 = 0; c2 < w; ++c2) screen.set(x + c2, y + r, c);
                                        return p + tpx;
                                }
                                if (control == 0x90) {
                                        const size_t n = compactLength(p);
                                        // The stub JPEG below: a fill colour in its 3 bytes.
                                        REQUIRE(n == 3);
                                        const uint32_t c = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
                                        for (int r = 0; r < h; ++r)
                                                for (int c2 = 0; c2 < w; ++c2) screen.set(x + c2, y + r, c);
                                        ++jpegs;
                                        return p + n;
                                }
                                const int stream = (control >> 4) & 3;
                                uint32_t  pal[256];
                                int       colours = 0;
                                if (control & 0x40) {
                                        REQUIRE(*p++ == 1); // the palette filter
                                        colours = *p++ + 1;
                                        for (int i = 0; i < colours; ++i, p += tpx) pal[i] = tpixel(p);
                                }
                                const size_t         rowBytes = colours == 2  ? size_t(w + 7) / 8
                                                                : colours > 0 ? size_t(w)
                                                                              : size_t(w) * tpx;
                                const size_t         want = rowBytes * size_t(h);
                                std::vector<uint8_t> data;
                                if (want < 12) {
                                        data.assign(p, p + want);
                                        p += want;
                                } else {
                                        const size_t n = compactLength(p);
                                        data = inflateSome(tight[stream], tightOpen[stream], p, n, want);
                                        p += n;
                                }
                                for (int r = 0; r < h; ++r)
                                        for (int c2 = 0; c2 < w; ++c2) {
                                                const uint8_t *row = data.data() + size_t(r) * rowBytes;
                                                uint32_t       c;
                                                if (colours == 2) {
                                                        c = pal[(row[c2 / 8] >> (7 - c2 % 8)) & 1];
                                                } else if (colours > 0) {
                                                        c = pal[row[c2]];
                                                } else {
                                                        c = tpixel(row + size_t(c2) * tpx);
                                                }
                                                screen.set(x + c2, y + r, c);
                                        }
                                return p;
                        }

                        // A whole update: its header and rectangles.
                        void update(const Writer &w) {
                                const uint8_t *p = w.data(), *end = w.data() + w.size();
                                REQUIRE(p[0] == 0);
                                const int count = vnc::readU16(p + 2);
                                p += 4;
                                for (int i = 0; i < count && p < end; ++i) {
                                        if (int32_t(vnc::readU32(p + 8)) == vnc::Encoding::LastRect) {
                                                p += 12;
                                                break;
                                        }
                                        p = rect(p);
                                }
                                CHECK(p == end);
                        }
        };

        // A stub JPEG: the picture's first pixel's colour, 3 bytes (decoded above as a fill).
        long stubJpeg(void *, const Picture &picture, int quality, uint8_t *out, size_t capacity) {
                CHECK(quality >= 1);
                CHECK(quality <= 100);
                if (capacity < 3) return -1;
                const uint32_t c = picture.rgb(0, 0);
                out[0] = uint8_t(c >> 16);
                out[1] = uint8_t(c >> 8);
                out[2] = uint8_t(c);
                return 3;
        }

        // Encodes @p screen's whole area in pieces (as captures come) and checks the viewer's copy.
        void roundTrip(const Screen &screen, Encoder &e, Viewer &v, bool expectExact) {
                std::vector<uint8_t> buffer(8 * 1024 * 1024);
                Writer               out(buffer.data(), buffer.size());
                vnc::Session::writeUpdateHeader(out, 0xffff);
                const Picture whole = screen.picture();
                // Three updates' worth of pieces: a band, a block, the rest.
                const uint16_t     w = screen.width, h = screen.height;
                const vnc::Picture parts[] = {whole.part(0, 0, w, 64), whole.part(0, 64, w / 2, uint16_t(h - 64)),
                                              whole.part(w / 2, 64, uint16_t(w - w / 2), uint16_t(h - 64))};
                const uint16_t     at[][2] = {{0, 0}, {0, 64}, {uint16_t(w / 2), 64}};
                for (int i = 0; i < 3; ++i) REQUIRE(e.encode(parts[i], at[i][0], at[i][1], out) > 0);
                vnc::Session::writeLastRect(out);
                REQUIRE_FALSE(out.overflowed());
                v.update(out);
                if (!expectExact) return;
                size_t differ = 0;
                for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x) differ += v.screen.get(x, y) != screen.get(x, y);
                CHECK(differ == 0);
        }

} // namespace

TEST_CASE("Encodings: Raw, ZRLE and Tight decode to the same picture") {
        const Screen screen = desktop(640, 400);
        for (int32_t which : {vnc::Encoding::Raw, vnc::Encoding::Zrle, vnc::Encoding::Tight}) {
                CAPTURE(which);
                Encoder e;
                REQUIRE(e.init(Encoder::TightMaxPixels));
                const int32_t list[] = {which, vnc::Encoding::LastRect};
                e.setEncodings(list, 2);
                CHECK(e.encoding() == which);
                Viewer v(640, 400);
                roundTrip(screen, e, v, true);
                // The streams go on across updates: a second round decodes too.
                Screen changed = screen;
                for (int x = 0; x < 640; ++x) changed.set(x, 3, 0x00ff00);
                roundTrip(changed, e, v, true);
                if (which == vnc::Encoding::Tight) {
                        CHECK(e.stats().fills > 0);
                        CHECK(e.stats().palettes > 0);
                        CHECK(e.stats().basics > 0);
                        CHECK(e.stats().jpegs == 0); // no quality level: no JPEG
                }
        }
}

TEST_CASE("Encodings: a 16-bit viewer gets 16-bit pixels from each") {
        Screen s(96, 48);
        for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 96; ++x)
                        s.set(x, y, uint32_t((x % 32) * 8) << 16 | uint32_t((y % 64) * 4) << 8 | 0xf8);
        vnc::PixelFormat f;
        f.bitsPerPixel = 16;
        f.depth = 16;
        f.redMax = 31;
        f.greenMax = 63;
        f.blueMax = 31;
        f.redShift = 11;
        f.greenShift = 5;
        f.blueShift = 0;
        for (int32_t which : {vnc::Encoding::Raw, vnc::Encoding::Zrle, vnc::Encoding::Tight}) {
                CAPTURE(which);
                Encoder e;
                REQUIRE(e.init(Encoder::TightMaxPixels));
                e.setPixelFormat(f);
                e.setEncodings(&which, 1);
                Viewer v(96, 48);
                v.format = f;
                std::vector<uint8_t> buffer(256 * 1024);
                Writer               out(buffer.data(), buffer.size());
                vnc::Session::writeUpdateHeader(out, 1);
                REQUIRE(e.encode(s.picture(), 0, 0, out) == 1);
                v.update(out);
                // Each pixel as the format makes it, read back as the viewer reads it.
                size_t differ = 0;
                for (int y = 0; y < 48; ++y)
                        for (int x = 0; x < 96; ++x) differ += v.screen.get(x, y) != v.rgbOf(f.value(s.get(x, y)));
                CHECK(differ == 0);
        }
}

TEST_CASE("Tight: JPEG only for many colours, big enough, with a quality given") {
        const Screen screen = desktop(640, 400);
        Encoder      e;
        REQUIRE(e.init(Encoder::TightMaxPixels));
        const int32_t list[] = {vnc::Encoding::Tight, vnc::Encoding::QualityLevel0 + 6, vnc::Encoding::LastRect};
        e.setEncodings(list, 3);
        CHECK(e.qualityLevel() == 6);
        e.setJpeg(stubJpeg, nullptr);
        Viewer v(640, 400);
        roundTrip(screen, e, v, false); // the stub's JPEGs are fills: not exact
        CHECK(v.jpegs > 0);
        CHECK(e.stats().jpegs == uint32_t(v.jpegs));
        // The flat and two-colour parts stayed exact: the band of strokes at the top.
        size_t differ = 0;
        for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 320; ++x) differ += v.screen.get(x, y) != screen.get(x, y);
        CHECK(differ == 0);
}

TEST_CASE("Tight: big rectangles are split within its limits") {
        Screen s(2400, 64); // wider than Tight's 2048
        for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 2400; ++x) s.set(x, y, uint32_t(x * 7 + y * 13) & 0xffffff);
        Encoder e;
        REQUIRE(e.init(Encoder::TightMaxPixels));
        const int32_t t = vnc::Encoding::Tight;
        e.setEncodings(&t, 1);
        std::vector<uint8_t> buffer(4 * 1024 * 1024);
        Writer               out(buffer.data(), buffer.size());
        vnc::Session::writeUpdateHeader(out, 0); // the count patched below
        const size_t rects = e.encode(s.picture(), 0, 0, out);
        CHECK(rects >= 3);
        out.patchU16(2, uint16_t(rects));
        Viewer v(2400, 64);
        v.update(out);
        size_t differ = 0;
        for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 2400; ++x) differ += v.screen.get(x, y) != s.get(x, y);
        CHECK(differ == 0);
}

TEST_CASE("Encoding into too small a buffer writes nothing") {
        const Screen screen = desktop(256, 128);
        for (int32_t which : {vnc::Encoding::Raw, vnc::Encoding::Zrle, vnc::Encoding::Tight}) {
                CAPTURE(which);
                Encoder e;
                REQUIRE(e.init(Encoder::TightMaxPixels));
                e.setEncodings(&which, 1);
                std::vector<uint8_t> small(200);
                Writer               out(small.data(), small.size());
                out.u8(42);
                CHECK(e.encode(screen.picture(), 0, 0, out) == 0);
                CHECK(out.size() == 1);
                CHECK_FALSE(out.overflowed());
                CHECK(e.isBroken() == (which == vnc::Encoding::Zrle)); // ZRLE's stream can't be reset
                CHECK(e.worstCase(256, 128) > 256 * 128 * 4);
        }
}

TEST_CASE("Tight: a rectangle that didn't fit resets the streams, and the viewer keeps up") {
        const Screen screen = desktop(320, 200);
        Encoder      e;
        REQUIRE(e.init(Encoder::TightMaxPixels));
        const int32_t list[] = {vnc::Encoding::Tight, vnc::Encoding::LastRect};
        e.setEncodings(list, 2);
        Viewer v(320, 200);
        roundTrip(screen, e, v, true); // the streams under way
        // An update that runs out of room after the streams took some of it.
        std::vector<uint8_t> small(1500);
        Writer               cut(small.data(), small.size());
        vnc::Session::writeUpdateHeader(cut, 0xffff);
        CHECK(e.encode(screen.picture(), 0, 0, cut) == 0);
        CHECK_FALSE(e.isBroken());
        // The next update, whole: its first rectangle tells the viewer to start again.
        Screen changed = screen;
        for (int y = 0; y < 200; ++y) changed.set(y, y, 0xff00ff);
        roundTrip(changed, e, v, true);
}
