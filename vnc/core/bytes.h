/**
 * @file      bytes.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vnc {

        /**
         * @brief Bytes appended to a buffer of the caller's, big-endian as RFB sends numbers
         *        (devplan/vnc.md §3). Past the buffer's end nothing more is written and
         *        `overflowed()` says so: a message is built whole or not at all.
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class Writer {
                public:
                        Writer(uint8_t *data, size_t capacity) : _data(data), _capacity(capacity) {}

                        void u8(uint8_t v) {
                                uint8_t *p = reserve(1);
                                if (p != nullptr) p[0] = v;
                                return;
                        }
                        void u16(uint16_t v) {
                                uint8_t *p = reserve(2);
                                if (p != nullptr) {
                                        p[0] = uint8_t(v >> 8);
                                        p[1] = uint8_t(v);
                                }
                                return;
                        }
                        void u32(uint32_t v) {
                                uint8_t *p = reserve(4);
                                if (p != nullptr) {
                                        p[0] = uint8_t(v >> 24);
                                        p[1] = uint8_t(v >> 16);
                                        p[2] = uint8_t(v >> 8);
                                        p[3] = uint8_t(v);
                                }
                                return;
                        }
                        void s32(int32_t v) {
                                u32(uint32_t(v));
                                return;
                        }
                        void bytes(const void *data, size_t n) {
                                uint8_t *p = reserve(n);
                                if (p != nullptr && n != 0) std::memcpy(p, data, n);
                                return;
                        }
                        void zeros(size_t n) {
                                uint8_t *p = reserve(n);
                                if (p != nullptr && n != 0) std::memset(p, 0, n);
                                return;
                        }

                        /** @brief Room for @p n bytes at the end, taken; null (and overflowed) if there isn't. */
                        uint8_t *reserve(size_t n) {
                                if (_overflowed || n > _capacity - _size) {
                                        _overflowed = true;
                                        return nullptr;
                                }
                                uint8_t *p = _data + _size;
                                _size += n;
                                return p;
                        }

                        /** @brief Writes @p v at @p at, inside what was written. */
                        void patchU16(size_t at, uint16_t v) {
                                if (at + 2 > _size) return;
                                _data[at] = uint8_t(v >> 8);
                                _data[at + 1] = uint8_t(v);
                                return;
                        }

                        /** @brief Goes back to @p size bytes written, overflowed or not (a part given up). */
                        void truncate(size_t size) {
                                if (size < _size) _size = size;
                                _overflowed = false;
                                return;
                        }

                        /** @brief Forgets the last @p n bytes written (a part that turned out not to be wanted). */
                        void drop(size_t n) {
                                _size = n > _size ? 0 : _size - n;
                                return;
                        }

                        uint8_t       *data() { return _data; }
                        const uint8_t *data() const { return _data; }
                        size_t         size() const { return _size; }
                        size_t         capacity() const { return _capacity; }
                        size_t         room() const { return _capacity - _size; }
                        bool           overflowed() const { return _overflowed; }

                        void clear() {
                                _size = 0;
                                _overflowed = false;
                                return;
                        }

                private:
                        uint8_t *_data;
                        size_t   _capacity;
                        size_t   _size = 0;
                        bool     _overflowed = false;
        };

        /** @brief Reads big-endian numbers at @p p (the caller has checked there are enough). */
        inline uint16_t readU16(const uint8_t *p) {
                return uint16_t(p[0] << 8 | p[1]);
        }
        inline uint32_t readU32(const uint8_t *p) {
                return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }

} // namespace vnc
