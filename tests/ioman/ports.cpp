/**
 * @file      ports.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The Ports tab's words: a port's value as fields and back, bytes in hex, i2cdetect's grid.
 */

#include <doctest.h>

#include <cstring>
#include <string>

#include "ports.h"

using ioman::PortKind;
using ioman::PortWords;
using ioman::ScanResult;

namespace {

        std::string join(PortKind kind, const PortWords &w) {
                char   out[256];
                size_t bad = 0;
                REQUIRE(ioman::joinWords(kind, w, out, sizeof(out), bad));
                CHECK(bad == ioman::MaxFields);
                return out;
        }

        std::string roundTrip(PortKind kind, const char *value) {
                PortWords w;
                ioman::splitWords(kind, value, w);
                return join(kind, w);
        }

} // namespace

TEST_CASE("Ports: a UART's value as fields, and back") {
        PortWords w;
        ioman::splitWords(PortKind::Uart, "tx=37 rx=38 baud=9600 format=7e2 flow=rtscts rts=16 cts=17 loopback force",
                          w);
        CHECK(std::string(w.field[0]) == "37");
        CHECK(std::string(w.field[1]) == "38");
        CHECK(std::string(w.field[2]) == "16");
        CHECK(std::string(w.field[3]) == "17");
        CHECK(std::string(w.field[4]) == "9600");
        CHECK(std::string(w.field[5]) == "7e2");
        CHECK(std::string(w.field[6]) == "rtscts");
        CHECK(std::string(w.field[7]) == "1");
        CHECK(std::string(w.other) == "force");
        // Fields in their order, the other words after; a default choice left out.
        CHECK(join(PortKind::Uart, w) == "tx=37 rx=38 rts=16 cts=17 baud=9600 format=7e2 flow=rtscts loopback force");
        CHECK(roundTrip(PortKind::Uart, "rx=38  tx=37 flow=none") == "tx=37 rx=38");
        CHECK(roundTrip(PortKind::Uart, "") == "");

        ioman::splitWords(PortKind::Uart, "", w);
        CHECK(std::string(w.field[6]) == "none"); // a choice's default shown
        CHECK(std::string(w.field[7]) == "");
}

TEST_CASE("Ports: an SPI port's value as fields, and back") {
        PortWords w;
        ioman::splitWords(PortKind::Spi, "sck=4 mosi=53 cs=5,33 mode=3 clock=2m bits=16 cs-high loopback", w);
        CHECK(std::string(w.field[0]) == "4");
        CHECK(std::string(w.field[2]) == "");
        CHECK(std::string(w.field[3]) == "5,33");
        CHECK(std::string(w.field[4]) == "3");
        CHECK(std::string(w.field[5]) == "2m");
        CHECK(std::string(w.field[6]) == "16");
        CHECK(std::string(w.field[7]) == "");
        CHECK(std::string(w.field[8]) == "1");
        CHECK(std::string(w.field[10]) == "1");
        CHECK(roundTrip(PortKind::Spi, "sck=4 mosi=53 mode=0 bits=8") == "sck=4 mosi=53");
}

TEST_CASE("Ports: words the fields don't know are kept as written") {
        PortWords w;
        // An unknown choice, a flag with a value, a word nobody knows: all in the other words.
        ioman::splitWords(PortKind::Spi, "sck=4 mode=7 lsb=1 speed=1m mosi=53", w);
        CHECK(std::string(w.field[4]) == "0");
        CHECK(std::string(w.field[7]) == "");
        CHECK(std::string(w.other) == "mode=7 lsb=1 speed=1m");
        CHECK(join(PortKind::Spi, w) == "sck=4 mosi=53 mode=7 lsb=1 speed=1m");
        // A space typed into a field: refused, which one said.
        snprintf(w.field[0], sizeof(w.field[0]), "4 5");
        char   out[128];
        size_t bad = 0;
        CHECK_FALSE(ioman::joinWords(PortKind::Spi, w, out, sizeof(out), bad));
        CHECK(bad == 0);
        // Too long for the room given.
        ioman::splitWords(PortKind::Spi, "sck=4 mosi=53", w);
        CHECK_FALSE(ioman::joinWords(PortKind::Spi, w, out, 8, bad));
        // A system I2C bus has no fields.
        size_t count = 99;
        CHECK(ioman::portFields(PortKind::I2c, count) == nullptr);
        CHECK(count == 0);
}

TEST_CASE("Ports: words found") {
        CHECK(ioman::hasWord("tx=37 force", "force"));
        CHECK_FALSE(ioman::hasWord("tx=37 forced", "force"));
        CHECK_FALSE(ioman::hasWord(nullptr, "force"));
}

TEST_CASE("Ports: numbers and bytes as people write them") {
        unsigned long v = 0;
        CHECK(ioman::parseNumber("0x50", 0x7f, v));
        CHECK(v == 0x50);
        CHECK(ioman::parseNumber(" 80 ", 0x7f, v));
        CHECK(v == 80);
        CHECK_FALSE(ioman::parseNumber("0x80", 0x7f, v));
        CHECK_FALSE(ioman::parseNumber("", 0x7f, v));
        CHECK_FALSE(ioman::parseNumber("-1", 0x7f, v));
        CHECK_FALSE(ioman::parseNumber("12z", 0x7f, v));

        uint8_t b[8];
        CHECK(ioman::parseBytes("de ad", b, 8) == 2);
        CHECK(b[0] == 0xde);
        CHECK(b[1] == 0xad);
        CHECK(ioman::parseBytes("deadBEEF", b, 8) == 4);
        CHECK(b[3] == 0xef);
        CHECK(ioman::parseBytes("0xde,0xad:0x1", b, 8) == 3);
        CHECK(b[2] == 0x01);
        CHECK(ioman::parseBytes("", b, 8) == 0);
        CHECK(ioman::parseBytes("dz", b, 8) == -1);
        CHECK(ioman::parseBytes("0102030405060708090a", b, 8) == -1); // more than room

        uint8_t many[20];
        for (uint8_t i = 0; i < 20; ++i) many[i] = i;
        char text[128];
        ioman::bytesText(many, 3, 0, text, sizeof(text));
        CHECK(std::string(text) == "00 01 02");
        ioman::bytesText(many, 18, 2, text, sizeof(text));
        CHECK(std::string(text) == "00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f\n  10 11");
}

TEST_CASE("Ports: i2cdetect's grid") {
        ScanResult r[128];
        for (unsigned a = 0; a < 128; ++a) r[a] = (a < 0x08 || a > 0x77) ? ScanResult::Skipped : ScanResult::None;
        r[0x48] = ScanResult::Driver;
        r[0x50] = ScanResult::Found;
        char grid[1024];
        ioman::scanGrid(r, grid, sizeof(grid));
        const std::string g(grid);
        CHECK(g.find("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n") == 0);
        CHECK(g.find("00:                         -- -- -- -- -- -- -- --\n") != std::string::npos);
        CHECK(g.find("40: -- -- -- -- -- -- -- -- UU -- -- -- -- -- -- --\n") != std::string::npos);
        CHECK(g.find("50: 50 --") != std::string::npos);
        CHECK(g.find("70: -- -- -- -- -- -- -- --                        \n") != std::string::npos);
}
