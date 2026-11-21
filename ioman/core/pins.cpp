/**
 * @file      pins.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The I/O Manager's pins (devplan/ioman.md): the Pins tab's rows, the columns' words.
 */

#include "pins.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ioman {

        const char *pinClassName(PinClass c) {
                switch (c) {
                        case PinClass::Free: return "free";
                        case PinClass::Fixed: return "fixed";
                        case PinClass::System: return "system";
                        case PinClass::User: return "user";
                }
                return "?";
        }

        void ownerText(const Pin &pin, char (&out)[OwnerBytes]) {
                if (pin.owner[0] != '\0' && pin.use[0] != '\0')
                        std::snprintf(out, sizeof(out), "%.15s: %.31s", pin.owner, pin.use);
                else
                        std::snprintf(out, sizeof(out), "%.31s", pin.owner[0] != '\0' ? pin.owner : pin.use);
                return;
        }

        void directionText(const Pin &pin, char (&out)[DirectionBytes]) {
                const bool pulled = pin.pull[0] != '\0' && std::strcmp(pin.pull, "-") != 0;
                std::snprintf(out, sizeof(out), "%.11s%s%.7s", pin.direction, pulled ? " " : "",
                              pulled ? pin.pull : "");
                return;
        }

        unsigned swatchOf(const Pin &pin) {
                const unsigned c = unsigned(pin.pinClass) < PinClassCount ? unsigned(pin.pinClass) : 0;
                const unsigned level = pin.peripheral ? 2 : pin.level == '1' ? 0 : pin.level == '0' ? 1 : 2;
                return c * 3 + level;
        }

        uint64_t pinHash(const Pin &pin) {
                // FNV-1a over what is shown.
                uint64_t   h = 0xcbf29ce484222325ull;
                const auto mix = [&h](const void *data, size_t size) {
                        for (size_t i = 0; i < size; ++i) {
                                h ^= static_cast<const uint8_t *>(data)[i];
                                h *= 0x100000001b3ull;
                        }
                };
                const uint8_t head[] = {pin.offset, uint8_t(pin.pinClass), uint8_t(pin.peripheral)};
                mix(head, sizeof(head));
                const char *const texts[] = {pin.name,      pin.owner, pin.use,  pin.route,
                                             pin.direction, pin.pull,  pin.where};
                for (const char *text : texts) mix(text, std::strlen(text) + 1);
                return h;
        }

        void PinList::setShow(Show show, const char *connector) {
                _show = show;
                _connector = connector != nullptr ? connector : "";
                return;
        }

        void PinList::setSort(size_t column, bool descending) {
                _sortColumn = column < ColumnCount ? column : Gpio;
                _descending = descending;
                return;
        }

        bool PinList::shows(const Pin &pin) const {
                switch (_show) {
                        case Show::All: return true;
                        case Show::Free: return pin.pinClass == PinClass::Free;
                        case Show::User: return pin.pinClass == PinClass::User;
                        case Show::System: return pin.pinClass == PinClass::System;
                        case Show::Fixed: return pin.pinClass == PinClass::Fixed;
                        case Show::Connector: {
                                // `UEXT.3`: the connector's name, then its pin.
                                const size_t n = std::strlen(_connector);
                                return n > 0 && std::strncmp(pin.where, _connector, n) == 0 && pin.where[n] == '.';
                        }
                }
                return true;
        }

        bool PinList::less(const Pin &a, const Pin &b) const {
                int order = 0;
                switch (_sortColumn) {
                        case Class: order = int(a.pinClass) - int(b.pinClass); break;
                        case Owner: {
                                char x[OwnerBytes], y[OwnerBytes];
                                ownerText(a, x);
                                ownerText(b, y);
                                // Empty last either way round: the pads nobody holds aren't the interesting ones.
                                if ((x[0] == '\0') != (y[0] == '\0')) return x[0] != '\0';
                                order = std::strcmp(x, y);
                                break;
                        }
                        case Route: order = std::strcmp(a.route, b.route); break;
                        case Direction: {
                                char x[DirectionBytes], y[DirectionBytes];
                                directionText(a, x);
                                directionText(b, y);
                                order = std::strcmp(x, y);
                                break;
                        }
                        case Where: {
                                if ((a.where[0] == '\0') != (b.where[0] == '\0')) return a.where[0] != '\0';
                                // `EXT1.9` before `EXT1.10`: the name, then the pin's number.
                                const char  *da = std::strchr(a.where, '.');
                                const char  *db = std::strchr(b.where, '.');
                                const size_t la = da != nullptr ? size_t(da - a.where) : std::strlen(a.where);
                                const size_t lb = db != nullptr ? size_t(db - b.where) : std::strlen(b.where);
                                order = std::strncmp(a.where, b.where, la < lb ? la : lb);
                                if (order == 0) order = int(la) - int(lb);
                                if (order == 0 && da != nullptr && db != nullptr)
                                        order = std::atoi(da + 1) - std::atoi(db + 1);
                                break;
                        }
                        default: break;
                }
                // Ties by number, always up.
                if (order == 0) return a.offset < b.offset;
                return _descending ? order > 0 : order < 0;
        }

        void PinList::update(const Pin *pins, size_t count) {
                _pins = pins;
                _total = count < MaxPins ? count : MaxPins;
                _count = 0;
                for (size_t &c : _classes) c = 0;
                for (size_t i = 0; i < _total; ++i) {
                        const size_t c = size_t(pins[i].pinClass);
                        if (c < PinClassCount) ++_classes[c];
                        if (shows(pins[i])) _order[_count++] = uint8_t(i);
                }
                // Insertion sort: a chip's pads are few (55 on the P4).
                for (size_t i = 1; i < _count; ++i) {
                        for (size_t j = i; j > 0 && less(_pins[_order[j]], _pins[_order[j - 1]]); --j) {
                                const uint8_t t = _order[j];
                                _order[j] = _order[j - 1];
                                _order[j - 1] = t;
                        }
                }
                return;
        }

        int PinList::rowOf(unsigned offset) const {
                for (size_t i = 0; i < _count; ++i)
                        if (_pins[_order[i]].offset == offset) return int(i);
                return -1;
        }

} // namespace ioman
