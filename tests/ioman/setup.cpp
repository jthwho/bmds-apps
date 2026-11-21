/**
 * @file      setup.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A pin's entry in io/gpio, read and written back; the level strip.
 */

#include <doctest.h>

#include <cstring>
#include <string>

#include "setup.h"

using ioman::PinSetup;

namespace {

        std::string with(const char *value, unsigned pad, const PinSetup *s) {
                char out[512];
                REQUIRE(ioman::withEntry(value, pad, s, out, sizeof(out)));
                return out;
        }

        std::string text(unsigned pad, const PinSetup &s) {
                char out[ioman::EntryBytes];
                ioman::entryText(pad, s, out);
                return out;
        }

} // namespace

TEST_CASE("PinSetup: an entry read, its words as fields and the rest kept") {
        PinSetup s;
        CHECK_FALSE(ioman::findEntry("", 14, s));
        CHECK(s.mode == PinSetup::Mode::None);
        REQUIRE(ioman::findEntry("2=out 14=od,high,pull-up,edges,debounce=2000,func=1,drive=3 15=in", 14, s));
        CHECK(s.mode == PinSetup::Mode::OpenDrain);
        CHECK(s.high);
        CHECK(s.pull == PinSetup::Pull::Up);
        CHECK(s.edges == PinSetup::Edges::Both);
        CHECK(s.debounceUs == 2000);
        CHECK(std::strcmp(s.extra, "func=1,drive=3") == 0);
        CHECK_FALSE(s.force);
        REQUIRE(ioman::findEntry("GPIO26=in:UART1_RXD,rising,falling,force", 26, s));
        CHECK(s.mode == PinSetup::Mode::In); // the default
        CHECK(s.edges == PinSetup::Edges::Both);
        CHECK(s.force);
        CHECK(std::strcmp(s.extra, "in:UART1_RXD") == 0);
        CHECK_FALSE(ioman::findEntry("140=out", 14, s)); // not a prefix
        CHECK_FALSE(ioman::findEntry("x=out", 14, s));
}

TEST_CASE("PinSetup: entries written") {
        PinSetup s;
        s.mode = PinSetup::Mode::Out;
        s.high = true;
        CHECK(text(14, s) == "14=out,high");
        s.pull = PinSetup::Pull::Down;
        s.force = true;
        CHECK(text(14, s) == "14=out,high,pull-down,force");
        PinSetup in;
        in.mode = PinSetup::Mode::In;
        in.high = true; // not an output: no level
        in.edges = PinSetup::Edges::Rising;
        in.debounceUs = 500;
        std::snprintf(in.extra, sizeof(in.extra), "in:UART1_RXD");
        CHECK(text(15, in) == "15=in,rising,debounce=500,in:UART1_RXD");
        PinSetup noEdges = in;
        noEdges.edges = PinSetup::Edges::None; // debounce only with edges
        CHECK(text(15, noEdges) == "15=in,in:UART1_RXD");
}

TEST_CASE("PinSetup: one entry replaced, added or taken out, the rest as they were") {
        PinSetup s;
        s.mode = PinSetup::Mode::Out;
        CHECK(with("", 14, &s) == "14=out,low");
        CHECK(with("2=out  GPIO14=in 15=in,force", 14, &s) == "2=out 14=out,low 15=in,force");
        CHECK(with("2=out 15=in", 14, &s) == "2=out 15=in 14=out,low");
        CHECK(with("2=out 14=in 15=in", 14, nullptr) == "2=out 15=in");
        PinSetup none;
        CHECK(with("14=in", 14, &none) == "");
        char small[8];
        CHECK_FALSE(ioman::withEntry("2=out 15=in", 14, &s, small, sizeof(small)));
}

TEST_CASE("levelStrip: the level over a span, from the edges") {
        bool                out[10];
        const ioman::EdgeAt edges[] = {{300, true}, {700, false}};
        ioman::levelStrip(false, edges, 2, 1000, 1000, out, 10); // slices end at 100, 200 ... 1000
        const bool expect[] = {false, false, true, true, true, true, false, false, false, false};
        for (size_t i = 0; i < 10; ++i) CHECK(out[i] == expect[i]);
        ioman::levelStrip(true, nullptr, 0, 1000, 1000, out, 10);
        for (size_t i = 0; i < 10; ++i) CHECK(out[i]);
}
