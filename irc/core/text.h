/**
 * @file      text.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace irc {

        /**
         * @brief Bytes someone else keeps: a view of a line, a parameter, a name (no terminating
         *        NUL). The core's string type, as the board has no libstdc++ (no `std::string_view`).
         *
         * @par Thread Safety
         * Reentrant.
         */
        class Text {
                public:
                        /** @brief No position (`find()`), or to the end (`mid()`). */
                        static constexpr size_t NoPos = SIZE_MAX;

                        constexpr Text() = default;
                        constexpr Text(const char *data, size_t size) : _data(data), _size(size) {}
                        /** @brief A C string's bytes, without its NUL. */
                        constexpr Text(const char *str) : _data(str), _size(str ? __builtin_strlen(str) : 0) {}

                        constexpr const char *data() const { return _data; }
                        constexpr size_t      size() const { return _size; }
                        constexpr bool        isEmpty() const { return _size == 0; }
                        constexpr char        operator[](size_t i) const { return _data[i]; }
                        constexpr const char *begin() const { return _data; }
                        constexpr const char *end() const { return _data + _size; }

                        /** @brief Returns @p len bytes from @p pos (fewer past the end, none past it). */
                        constexpr Text mid(size_t pos, size_t len = NoPos) const {
                                if (pos >= _size) return Text(_data + _size, 0);
                                size_t left = _size - pos;
                                return Text(_data + pos, len < left ? len : left);
                        }

                        /** @brief Returns the first @p len bytes. */
                        constexpr Text left(size_t len) const { return mid(0, len); }

                        /** @brief Returns the index of @p c at or past @p from, or `NoPos`. */
                        constexpr size_t find(char c, size_t from = 0) const {
                                for (size_t i = from; i < _size; ++i)
                                        if (_data[i] == c) return i;
                                return NoPos;
                        }

                        /** @brief Returns the index of the last @p c, or `NoPos`. */
                        constexpr size_t findLast(char c) const {
                                for (size_t i = _size; i > 0; --i)
                                        if (_data[i - 1] == c) return i - 1;
                                return NoPos;
                        }

                        constexpr bool startsWith(Text other) const {
                                return other._size <= _size && equalBytes(_data, other._data, other._size);
                        }

                        constexpr bool operator==(Text other) const {
                                return _size == other._size && equalBytes(_data, other._data, _size);
                        }

                        /** @brief Equal, ASCII letters in either case (a command's name, a CAP's). */
                        constexpr bool equalsIgnoringCase(Text other) const {
                                if (_size != other._size) return false;
                                for (size_t i = 0; i < _size; ++i)
                                        if (lower(_data[i]) != lower(other._data[i])) return false;
                                return true;
                        }

                        /** @brief Returns @p c as lower case if it is an ASCII capital. */
                        static constexpr char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }

                private:
                        static constexpr bool equalBytes(const char *a, const char *b, size_t n) {
                                for (size_t i = 0; i < n; ++i)
                                        if (a[i] != b[i]) return false;
                                return true;
                        }

                        const char *_data = "";
                        size_t      _size = 0;
        };

} // namespace irc
