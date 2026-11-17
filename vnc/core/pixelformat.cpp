/**
 * @file      pixelformat.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * RFB's pixel formats (RFB 3.8 §7.4; ZRLE's CPIXEL §7.7.6; Tight's TPIXEL): from the screen's B,
 * G, R bytes to what the viewer asked for.
 */

#include "pixelformat.h"

namespace vnc {

        namespace {

                // A channel's 8 bits scaled to @p max (a mask of the low bits, as RFB's are).
                uint32_t scale(uint32_t c, uint16_t max) {
                        return max == 255 ? c : (c * max + 127) / 255;
                }

                bool isMask(uint16_t v) {
                        return v != 0 && (v & (v + 1u)) == 0;
                }

        } // namespace

        bool PixelFormat::parse(const uint8_t w[WireBytes]) {
                PixelFormat f;
                f.bitsPerPixel = w[0];
                f.depth = w[1];
                f.bigEndian = w[2] != 0;
                f.trueColour = w[3] != 0;
                f.redMax = readU16(w + 4);
                f.greenMax = readU16(w + 6);
                f.blueMax = readU16(w + 8);
                f.redShift = w[10];
                f.greenShift = w[11];
                f.blueShift = w[12];
                if (f.bitsPerPixel != 8 && f.bitsPerPixel != 16 && f.bitsPerPixel != 32) return false;
                if (!f.trueColour || !isMask(f.redMax) || !isMask(f.greenMax) || !isMask(f.blueMax)) return false;
                if (f.redShift >= f.bitsPerPixel || f.greenShift >= f.bitsPerPixel || f.blueShift >= f.bitsPerPixel)
                        return false;
                *this = f;
                return true;
        }

        void PixelFormat::write(Writer &out) const {
                out.u8(bitsPerPixel);
                out.u8(depth);
                out.u8(bigEndian ? 1 : 0);
                out.u8(trueColour ? 1 : 0);
                out.u16(redMax);
                out.u16(greenMax);
                out.u16(blueMax);
                out.u8(redShift);
                out.u8(greenShift);
                out.u8(blueShift);
                out.zeros(3);
                return;
        }

        uint32_t PixelFormat::value(uint32_t rgb) const {
                return scale(rgb >> 16 & 0xffu, redMax) << redShift | scale(rgb >> 8 & 0xffu, greenMax) << greenShift |
                       scale(rgb & 0xffu, blueMax) << blueShift;
        }

        void PixelFormat::storePixel(uint32_t rgb, uint8_t *out) const {
                const uint32_t v = value(rgb);
                const size_t   n = bytesPerPixel();
                for (size_t i = 0; i < n; ++i) out[i] = uint8_t(v >> (8 * (bigEndian ? n - 1 - i : i)));
                return;
        }

        size_t PixelFormat::cpixelBytes() const {
                if (bitsPerPixel != 32 || depth > 24) return bytesPerPixel();
                const uint32_t used = uint32_t(redMax) << redShift | uint32_t(greenMax) << greenShift |
                                      uint32_t(blueMax) << blueShift;
                return (used & 0xff000000u) == 0 || (used & 0x000000ffu) == 0 ? 3 : 4;
        }

        void PixelFormat::storeCpixel(uint32_t rgb, uint8_t *out) const {
                if (cpixelBytes() != 3) {
                        storePixel(rgb, out);
                        return;
                }
                const uint32_t used = uint32_t(redMax) << redShift | uint32_t(greenMax) << greenShift |
                                      uint32_t(blueMax) << blueShift;
                // The three bytes holding the colours, in the PIXEL's own byte order.
                const uint32_t v = value(rgb);
                const bool     low = (used & 0xff000000u) == 0;
                const uint32_t three = low ? v : v >> 8;
                for (size_t i = 0; i < 3; ++i) out[i] = uint8_t(three >> (8 * (bigEndian ? 2 - i : i)));
                return;
        }

        bool PixelFormat::isTight24() const {
                return bitsPerPixel == 32 && depth == 24 && redMax == 255 && greenMax == 255 && blueMax == 255;
        }

        void PixelFormat::storeTpixel(uint32_t rgb, uint8_t *out) const {
                if (!isTight24()) {
                        storePixel(rgb, out);
                        return;
                }
                // Tight's 24-bit pixel: the channels in R, G, B order whatever the shifts.
                out[0] = uint8_t(rgb >> 16);
                out[1] = uint8_t(rgb >> 8);
                out[2] = uint8_t(rgb);
                return;
        }

        bool PixelFormat::operator==(const PixelFormat &o) const {
                return bitsPerPixel == o.bitsPerPixel && depth == o.depth && bigEndian == o.bigEndian &&
                       trueColour == o.trueColour && redMax == o.redMax && greenMax == o.greenMax &&
                       blueMax == o.blueMax && redShift == o.redShift && greenShift == o.greenShift &&
                       blueShift == o.blueShift;
        }

} // namespace vnc
