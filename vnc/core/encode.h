/**
 * @file      encode.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "bytes.h"
#include "pixelformat.h"

#include <cstddef>
#include <cstdint>

struct z_stream_s;

namespace vnc {

        /**
         * @brief One zlib stream, kept for a session (RFB's ZRLE has one, Tight four: the viewer
         *        inflates each as one long stream). A small window (devplan/vnc.md A3): about 40 KB.
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class ZStream {
                public:
                        ZStream() = default;
                        ~ZStream() { end(); }
                        ZStream(const ZStream &) = delete;
                        ZStream &operator=(const ZStream &) = delete;

                        /** @brief Compresses @p n bytes at @p in, flushed to a byte boundary, onto @p out. @return False if it didn't fit. */
                        bool compress(const uint8_t *in, size_t n, int level, Writer &out);

                        /** @brief Forgets the stream (a viewer told to reset it starts a new one). */
                        void end();

                        bool isStarted() const { return _z != nullptr; }

                private:
                        z_stream_s *_z = nullptr;
                        int         _level = -1;
        };

        /** @brief Encodes a rectangle of pixels as JPEG, into @p out: the bytes, or -1 (the board's hardware codec). */
        using JpegFn = long (*)(void *arg, const Picture &picture, int quality, uint8_t *out, size_t capacity);

        /**
         * @brief What a viewer's updates are encoded with, and the encoding (devplan/vnc.md §5a):
         *        Tight first (a fill for one colour, a palette through zlib for few, JPEG for many
         *        when the viewer gave a quality, else its pixels through zlib), ZRLE for viewers
         *        without Tight, Raw for the rest.
         *
         * @par Thread Safety
         * Not thread-safe: a session's.
         */
        class Encoder {
                public:
                        /** @brief How each rectangle went. */
                        struct Stats {
                                        uint32_t fills = 0;
                                        uint32_t palettes = 0;
                                        uint32_t jpegs = 0;
                                        uint32_t basics = 0; ///< Tight's pixels through zlib.
                                        uint32_t zrle = 0;
                                        uint32_t raw = 0;
                                        uint64_t pixels = 0;
                                        uint64_t bytes = 0;
                        };

                        /** @brief A rectangle's pixels at most before Tight splits it (libvncserver's at its best levels). */
                        static constexpr uint32_t TightMaxPixels = 65536;
                        static constexpr uint16_t TightMaxWidth = 2048;

                        /** @brief Rectangles this narrow or short are never JPEGs (their headers outweigh them). */
                        static constexpr uint16_t JpegMinWidth = 64;
                        static constexpr uint16_t JpegMinHeight = 32;

                        Encoder();
                        ~Encoder();
                        Encoder(const Encoder &) = delete;
                        Encoder &operator=(const Encoder &) = delete;

                        /**
                         * @brief Makes its working memory for rectangles up to @p maxPixels: a palette's
                         *        indices and zlib's output. @return False without the memory.
                         */
                        bool init(uint32_t maxPixels);

                        /** @brief The viewer's pixel format. */
                        void               setPixelFormat(const PixelFormat &format) { _format = format; }
                        const PixelFormat &pixelFormat() const { return _format; }

                        /** @brief The viewer's encodings (its SetEncodings): the encoding and its options chosen. */
                        void setEncodings(const int32_t *encodings, size_t count);

                        /** @brief The encoding chosen: `Encoding::Tight`, `Zrle` or `Raw`. */
                        int32_t encoding() const { return _encoding; }

                        bool hasLastRect() const { return _lastRect; }
                        bool hasDesktopSize() const { return _desktopSize; }

                        /** @brief The viewer's quality level (0-9), or -1: none given (no JPEG). */
                        int qualityLevel() const { return _quality; }

                        /** @brief Caps the JPEG's quality (1-100: the settings' ceiling). */
                        void setJpegCeiling(int quality) {
                                _jpegCeiling = quality;
                                return;
                        }

                        /** @brief Sets the JPEG encoder (null: no JPEG). */
                        void setJpeg(JpegFn fn, void *arg) {
                                _jpeg = fn;
                                _jpegArg = arg;
                                return;
                        }

                        /**
                         * @brief Returns the most bytes `encode()` can write for a @p width × @p height
                         *        rectangle: with that much room it can't run out.
                         */
                        size_t worstCase(uint16_t width, uint16_t height) const;

                        /**
                         * @brief Writes @p picture, the screen's rectangle at @p x, @p y, as one or more
                         *        rectangles (header and data) onto @p out.
                         * @return The rectangles written (Tight splits big ones), or 0 if @p out was too
                         *         small: @p out as it was. A zlib stream may have taken the lost bytes:
                         *         Tight's are reset (the viewer told with the next rectangle); ZRLE's
                         *         can't be, so `isBroken()` (end the session). Give `worstCase()`'s room.
                         */
                        size_t encode(const Picture &picture, uint16_t x, uint16_t y, Writer &out);

                        /** @brief Returns true if ZRLE's stream lost bytes the viewer needed (end the session). */
                        bool isBroken() const { return _broken; }

                        const Stats &stats() const { return _stats; }

                private:
                        size_t encodeTight(const Picture &picture, uint16_t x, uint16_t y, Writer &out);
                        bool   tightRect(const Picture &picture, uint16_t x, uint16_t y, Writer &out);
                        bool   tightPalette(const Picture &picture, size_t colours, Writer &out);
                        bool   tightBasic(const Picture &picture, Writer &out);
                        bool   tightJpeg(const Picture &picture, Writer &out);
                        bool   tightData(int stream, const uint8_t *data, size_t n, Writer &out);
                        void   tightControl(Writer &out, uint8_t control); // with the streams to reset
                        bool   zrle(const Picture &picture, uint16_t x, uint16_t y, Writer &out);
                        bool   raw(const Picture &picture, uint16_t x, uint16_t y, Writer &out);
                        size_t countColours(const Picture &picture, size_t limit); // into _palette; limit + 1 if more
                        static size_t jpegRoom(size_t pixels);                     // a JPEG's bytes at most
                        uint8_t       indexOf(uint32_t rgb) const;

                        PixelFormat _format;
                        int32_t     _encoding = 0; // Encoding::Raw
                        int         _quality = -1;
                        int         _compress = 6;
                        int         _jpegCeiling = 90;
                        bool        _lastRect = false;
                        bool        _desktopSize = false;
                        JpegFn      _jpeg = nullptr;
                        void       *_jpegArg = nullptr;
                        ZStream     _tight[4];
                        uint8_t     _tightResets = 0; // the streams the viewer must reset (bits 0-3)
                        bool        _broken = false;
                        ZStream     _zrle;
                        uint32_t    _palette[256] = {};
                        size_t      _paletteCount = 0;
                        uint8_t    *_work = nullptr; // a rectangle's pixels or indices, before zlib
                        size_t      _workBytes = 0;
                        uint8_t    *_packed = nullptr; // zlib's output, before its length
                        size_t      _packedBytes = 0;
                        Stats       _stats;
        };

} // namespace vnc
