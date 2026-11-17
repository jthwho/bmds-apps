/**
 * @file      str.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "str.h"

namespace irc {

        Error String::reserve(size_t capacity) {
                if (capacity <= _capacity) return Error::Ok;
                size_t grown = _capacity < 16 ? 16 : _capacity;
                while (grown < capacity) {
                        if (grown > SIZE_MAX / 2) return Error::NoMem;
                        grown *= 2;
                }
                char *p = static_cast<char *>(std::realloc(_data, grown));
                if (p == nullptr) return Error::NoMem;
                _data = p;
                _capacity = grown;
                return Error::Ok;
        }

        Error String::assign(Text text) {
                // A part of itself survives a reserve() only if no reserve is needed (it fits already).
                if (reserve(text.size()) != Error::Ok) return Error::NoMem;
                if (text.size() > 0) std::memmove(_data, text.data(), text.size());
                _size = text.size();
                return Error::Ok;
        }

        Error String::append(Text text) {
                if (text.size() == 0) return Error::Ok;
                if (text.size() > SIZE_MAX - _size) return Error::NoMem;
                if (reserve(_size + text.size()) != Error::Ok) return Error::NoMem;
                std::memcpy(_data + _size, text.data(), text.size());
                _size += text.size();
                return Error::Ok;
        }

} // namespace irc
