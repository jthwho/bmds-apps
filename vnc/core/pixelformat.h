/**
 * @file      pixelformat.h
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
         * @brief Pixels as the screen gives them: B, G, R bytes (BMDS's surfaces, a capture's
         *        `BMDS_PIXEL_BGR24`), @ref stride bytes a row.
         */
        struct Picture {
                        const uint8_t *pixels = nullptr;
                        size_t         stride = 0;
                        uint16_t       width = 0;
                        uint16_t       height = 0;

                        /** @brief Pixel (@p x, @p y) as 0xRRGGBB. */
                        uint32_t rgb(uint16_t x, uint16_t y) const {
                                const uint8_t *p = pixels + size_t(y) * stride + size_t(x) * 3;
                                return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
                        }

                        /** @brief The part @p x, @p y, @p w × @p h of it (inside it). */
                        Picture part(uint16_t x, uint16_t y, uint16_t w, uint16_t h) const {
                                return Picture{pixels + size_t(y) * stride + size_t(x) * 3, stride, w, h};
                        }
        };

        /**
         * @brief An RFB PIXEL_FORMAT (RFB 3.8 §7.4): how the viewer wants pixels. Only true colour
         *        is taken (a colour map isn't offered).
         *
         * @par Thread Safety
         * A plain value.
         */
        struct PixelFormat {
                        uint8_t  bitsPerPixel = 32;
                        uint8_t  depth = 24;
                        bool     bigEndian = false;
                        bool     trueColour = true;
                        uint16_t redMax = 255;
                        uint16_t greenMax = 255;
                        uint16_t blueMax = 255;
                        uint8_t  redShift = 16;
                        uint8_t  greenShift = 8;
                        uint8_t  blueShift = 0;

                        /** @brief The bytes of a PIXEL_FORMAT on the wire. */
                        static constexpr size_t WireBytes = 16;

                        /** @brief Reads one from the wire. @return False if it isn't one this server sends. */
                        bool parse(const uint8_t wire[WireBytes]);

                        /** @brief Writes it as the wire has it. */
                        void write(Writer &out) const;

                        /** @brief Returns a PIXEL's bytes. */
                        size_t bytesPerPixel() const { return bitsPerPixel / 8u; }

                        /** @brief Returns @p rgb (0xRRGGBB) as this format's value. */
                        uint32_t value(uint32_t rgb) const;

                        /** @brief Writes @p rgb as a PIXEL (`bytesPerPixel()` bytes at @p out). */
                        void storePixel(uint32_t rgb, uint8_t *out) const;

                        /** @brief Returns ZRLE's CPIXEL's bytes: 3 where RFB allows it, else a PIXEL's. */
                        size_t cpixelBytes() const;

                        /** @brief Writes @p rgb as a CPIXEL. */
                        void storeCpixel(uint32_t rgb, uint8_t *out) const;

                        /** @brief Returns true if Tight's TPIXEL is three bytes, R, G, B (32 bits, depth 24, 8 a channel). */
                        bool isTight24() const;

                        /** @brief Returns Tight's TPIXEL's bytes. */
                        size_t tpixelBytes() const { return isTight24() ? 3 : bytesPerPixel(); }

                        /** @brief Writes @p rgb as a TPIXEL. */
                        void storeTpixel(uint32_t rgb, uint8_t *out) const;

                        /**
                         * @brief Returns true for the usual 32-bit format (little-endian, 8 bits a channel,
                         *        red at 16): a PIXEL is the screen's B, G, R and a zero, a CPIXEL its B, G, R.
                         */
                        bool isBgrx() const {
                                return bitsPerPixel == 32 && !bigEndian && redMax == 255 && greenMax == 255 &&
                                       blueMax == 255 && redShift == 16 && greenShift == 8 && blueShift == 0;
                        }

                        /** @brief Returns true if @p other is the same format. */
                        bool operator==(const PixelFormat &other) const;
        };

} // namespace vnc
