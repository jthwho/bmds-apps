/**
 * @file      pins.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The Pins tab's rows: which pads show, their order, the classes' counts; the columns' words.
 */

#include <doctest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "pins.h"

using ioman::Pin;
using ioman::PinClass;
using ioman::PinList;

namespace {

        Pin pin(uint8_t offset, PinClass c, char level, const char *owner, const char *use, const char *where,
                const char *direction = "-", const char *pull = "-") {
                Pin p;
                p.offset = offset;
                p.pinClass = c;
                p.level = level;
                std::snprintf(p.name, sizeof(p.name), "GPIO%u", unsigned(offset));
                std::snprintf(p.owner, sizeof(p.owner), "%s", owner);
                std::snprintf(p.use, sizeof(p.use), "%s", use);
                std::snprintf(p.where, sizeof(p.where), "%s", where);
                std::snprintf(p.direction, sizeof(p.direction), "%s", direction);
                std::snprintf(p.pull, sizeof(p.pull), "%s", pull);
                return p;
        }

        // The pads shown, in order.
        std::string order(const PinList &list) {
                std::string s;
                for (size_t i = 0; i < list.count(); ++i) {
                        if (!s.empty()) s += ",";
                        s += std::to_string(list.at(i).offset);
                }
                return s;
        }

        const Pin Pins[] = {
                pin(0, PinClass::Fixed, '-', "", "32 kHz xtal", ""),
                pin(2, PinClass::Free, '1', "", "", "EXT1.7"),
                pin(14, PinClass::Free, '0', "", "", "EXT1.8", "in", "up"),
                pin(20, PinClass::Free, '0', "", "", "EXT1.14"),
                pin(35, PinClass::System, '1', "eth0", "RMII TXD1", "", "periph"),
                pin(37, PinClass::User, '1', "uart0", "TX", "UEXT.3", "out"),
                pin(53, PinClass::System, '0', "i2c1", "SCL", "UEXT.8", "periph,od", "up"),
        };
        constexpr size_t PinCount = sizeof(Pins) / sizeof(Pins[0]);

} // namespace

TEST_CASE("PinList: the Pins tab's rows") {
        PinList list;

        SUBCASE("every pad by number, and the classes' counts") {
                list.update(Pins, PinCount);
                CHECK(order(list) == "0,2,14,20,35,37,53");
                CHECK(list.total() == 7);
                CHECK(list.classCount(PinClass::Free) == 3);
                CHECK(list.classCount(PinClass::System) == 2);
                CHECK(list.classCount(PinClass::User) == 1);
                CHECK(list.classCount(PinClass::Fixed) == 1);
                CHECK(list.rowOf(35) == 4);
                CHECK(list.rowOf(36) == -1);
        }

        SUBCASE("by class: the others counted, not shown") {
                list.setShow(PinList::Show::Free);
                list.update(Pins, PinCount);
                CHECK(order(list) == "2,14,20");
                CHECK(list.classCount(PinClass::System) == 2);
                list.setShow(PinList::Show::System);
                list.update(Pins, PinCount);
                CHECK(order(list) == "35,53");
        }

        SUBCASE("on a connector: its name, not a prefix of another's") {
                list.setShow(PinList::Show::Connector, "UEXT");
                list.update(Pins, PinCount);
                CHECK(order(list) == "37,53");
                list.setShow(PinList::Show::Connector, "EXT");
                list.update(Pins, PinCount);
                CHECK(order(list) == "");
                list.setShow(PinList::Show::Connector, "");
                list.update(Pins, PinCount);
                CHECK(order(list) == "");
        }

        SUBCASE("by connector pin: the name, then the pin's number; pads on none last") {
                list.setSort(PinList::Where, false);
                list.update(Pins, PinCount);
                CHECK(order(list) == "2,14,20,37,53,0,35");
                list.setSort(PinList::Where, true);
                list.update(Pins, PinCount);
                CHECK(order(list) == "53,37,20,14,2,0,35");
        }

        SUBCASE("by owner, the pads nobody holds or labels last") {
                list.setSort(PinList::Owner, false);
                list.update(Pins, PinCount);
                CHECK(order(list) == "0,35,53,37,2,14,20");
        }

        SUBCASE("by class and direction, ties by number") {
                list.setSort(PinList::Class, false);
                list.update(Pins, PinCount);
                CHECK(order(list) == "2,14,20,0,35,53,37");
                list.setSort(PinList::Direction, false);
                list.update(Pins, PinCount);
                CHECK(order(list) == "0,2,20,14,37,35,53");
        }

        SUBCASE("too many pads: the first MaxPins") {
                Pin many[PinList::MaxPins + 4];
                for (size_t i = 0; i < sizeof(many) / sizeof(many[0]); ++i)
                        many[i] = pin(uint8_t(i), PinClass::Free, '0', "", "", "");
                list.update(many, sizeof(many) / sizeof(many[0]));
                CHECK(list.total() == PinList::MaxPins);
                CHECK(list.count() == PinList::MaxPins);
        }
}

TEST_CASE("Pins: the columns' words, swatches and hashes") {
        char owner[ioman::OwnerBytes];
        ioman::ownerText(Pins[4], owner);
        CHECK(std::strcmp(owner, "eth0: RMII TXD1") == 0);
        ioman::ownerText(Pins[0], owner);
        CHECK(std::strcmp(owner, "32 kHz xtal") == 0);
        ioman::ownerText(Pins[1], owner);
        CHECK(owner[0] == '\0');

        char direction[ioman::DirectionBytes];
        ioman::directionText(Pins[2], direction);
        CHECK(std::strcmp(direction, "in up") == 0);
        ioman::directionText(Pins[5], direction);
        CHECK(std::strcmp(direction, "out") == 0);
        ioman::directionText(Pins[6], direction);
        CHECK(std::strcmp(direction, "periph,od up") == 0);

        CHECK(ioman::swatchOf(Pins[0]) == 1 * 3 + 2); // fixed, no level
        CHECK(ioman::swatchOf(Pins[1]) == 0);         // free, high
        CHECK(ioman::swatchOf(Pins[2]) == 1);         // free, low
        CHECK(ioman::swatchOf(Pins[5]) == 3 * 3 + 0); // user, high
        CHECK(ioman::swatchOf(Pins[6]) < ioman::SwatchCount);

        Pin            p = Pins[2];
        const uint64_t h = ioman::pinHash(p);
        CHECK(ioman::pinHash(p) == h);
        p.level = '1'; // the level is the swatch's, not the words'
        CHECK(ioman::pinHash(p) == h);
        CHECK(ioman::swatchOf(p) == 0);
        p.peripheral = true; // a peripheral's line: no level shown
        CHECK(ioman::swatchOf(p) == 2);
        p.level = '0';
        CHECK(ioman::swatchOf(p) == 2);
        CHECK(ioman::pinHash(p) != h);
        p = Pins[2];
        std::snprintf(p.owner, sizeof(p.owner), "io");
        CHECK(ioman::pinHash(p) != h);

        CHECK(std::strcmp(ioman::pinClassName(PinClass::System), "system") == 0);
}
