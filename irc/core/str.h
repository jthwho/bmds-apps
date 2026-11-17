/**
 * @file      str.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "text.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace irc {

        /**
         * @brief Bytes of its own (the core's `std::string`, which the board hasn't): grown on the
         *        heap, never copied by accident (move only; `assign()` copies and says if it ran
         *        out of memory, as nothing here throws).
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class String {
                public:
                        String() = default;
                        String(const String &) = delete;
                        String &operator=(const String &) = delete;
                        String(String &&other) noexcept
                            : _data(other._data), _size(other._size), _capacity(other._capacity) {
                                other._data = nullptr;
                                other._size = other._capacity = 0;
                        }
                        String &operator=(String &&other) noexcept {
                                if (this != &other) {
                                        std::free(_data);
                                        _data = other._data;
                                        _size = other._size;
                                        _capacity = other._capacity;
                                        other._data = nullptr;
                                        other._size = other._capacity = 0;
                                }
                                return *this;
                        }
                        ~String() { std::free(_data); }

                        /** @brief Makes it @p text (which may be a part of it). @return `Ok` or `NoMem` (then unchanged). */
                        Error assign(Text text);

                        /** @brief Adds @p text at the end. @return `Ok` or `NoMem` (then unchanged). */
                        Error append(Text text);

                        /** @brief Adds @p c at the end. */
                        Error append(char c) { return append(Text(&c, 1)); }

                        /** @brief Empties it (keeping its room). */
                        void clear() { _size = 0; }

                        Text view() const { return Text(_data != nullptr ? _data : "", _size); }
                        operator Text() const { return view(); }
                        const char *data() const { return _data != nullptr ? _data : ""; }
                        size_t      size() const { return _size; }
                        bool        isEmpty() const { return _size == 0; }
                        bool        operator==(Text other) const { return view() == other; }

                private:
                        Error reserve(size_t capacity);

                        char  *_data = nullptr;
                        size_t _size = 0;
                        size_t _capacity = 0;
        };

        /**
         * @brief Things in order, on the heap (the core's `std::vector`): move only, its growth
         *        reported (`NoMem`) rather than thrown.
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        template <typename T> class Vector {
                public:
                        Vector() = default;
                        Vector(const Vector &) = delete;
                        Vector &operator=(const Vector &) = delete;
                        Vector(Vector &&other) noexcept
                            : _data(other._data), _size(other._size), _capacity(other._capacity) {
                                other._data = nullptr;
                                other._size = other._capacity = 0;
                        }
                        Vector &operator=(Vector &&other) noexcept {
                                if (this != &other) {
                                        clear();
                                        std::free(_data);
                                        _data = other._data;
                                        _size = other._size;
                                        _capacity = other._capacity;
                                        other._data = nullptr;
                                        other._size = other._capacity = 0;
                                }
                                return *this;
                        }
                        ~Vector() {
                                clear();
                                std::free(_data);
                        }

                        size_t   size() const { return _size; }
                        bool     isEmpty() const { return _size == 0; }
                        T       &operator[](size_t i) { return _data[i]; }
                        const T &operator[](size_t i) const { return _data[i]; }
                        T       *begin() { return _data; }
                        T       *end() { return _data + _size; }
                        const T *begin() const { return _data; }
                        const T *end() const { return _data + _size; }
                        T       &last() { return _data[_size - 1]; }

                        /** @brief Adds @p value at the end. @return `Ok` or `NoMem` (then unchanged). */
                        Error append(T &&value) { return insert(_size, std::move(value)); }

                        /** @brief Puts @p value at @p at (the rest after it). @return `Ok` or `NoMem`. */
                        Error insert(size_t at, T &&value) {
                                if (at > _size) at = _size;
                                if (_size == _capacity && grow() != Error::Ok) return Error::NoMem;
                                if (at == _size) {
                                        ::new (&_data[_size]) T(std::move(value));
                                } else {
                                        ::new (&_data[_size]) T(std::move(_data[_size - 1]));
                                        for (size_t i = _size - 1; i > at; --i) _data[i] = std::move(_data[i - 1]);
                                        _data[at] = std::move(value);
                                }
                                ++_size;
                                return Error::Ok;
                        }

                        /** @brief Takes away the one at @p at. */
                        void removeAt(size_t at) {
                                if (at >= _size) return;
                                for (size_t i = at; i + 1 < _size; ++i) _data[i] = std::move(_data[i + 1]);
                                _data[--_size].~T();
                                return;
                        }

                        /** @brief Takes them all away (keeping the room). */
                        void clear() {
                                while (_size > 0) _data[--_size].~T();
                                return;
                        }

                private:
                        Error grow() {
                                const size_t cap = _capacity == 0 ? 4 : _capacity * 2;
                                T           *p = static_cast<T *>(std::malloc(cap * sizeof(T)));
                                if (p == nullptr) return Error::NoMem;
                                for (size_t i = 0; i < _size; ++i) {
                                        ::new (&p[i]) T(std::move(_data[i]));
                                        _data[i].~T();
                                }
                                std::free(_data);
                                _data = p;
                                _capacity = cap;
                                return Error::Ok;
                        }

                        T     *_data = nullptr;
                        size_t _size = 0;
                        size_t _capacity = 0;
        };

} // namespace irc
