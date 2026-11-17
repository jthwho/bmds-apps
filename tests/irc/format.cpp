/**
 * @file      format.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * IRC's text (irc/core/format.h): codes taken out, Latin-1 read, CTCP, mentions, server-time.
 */

#include "format.h"
#include "helpers.h"

using irc::Error;
using irc::Text;
namespace Format = irc::Format;

namespace {

        std::string stripped(Text in) {
                irc::String out;
                REQUIRE(Format::strip(in, out) == Error::Ok);
                return test::str(out);
        }

        std::string utf8(Text in) {
                irc::String out;
                REQUIRE(Format::toUtf8(in, out) == Error::Ok);
                return test::str(out);
        }

} // namespace

TEST_CASE("Format::strip: every code, colours with and without their numbers") {
        CHECK(stripped("plain") == "plain");
        CHECK(stripped("\x02"
                       "bold\x02 \x1d"
                       "it\x1d \x1fu\x1f \x1es\x1e \x11m\x11 \x16r\x16 \x0f"
                       "end") == "bold it u s m r end");
        CHECK(stripped("\x03"
                       "4red\x03 \x03"
                       "04,12x\x03 \x03"
                       "1,2y") == "red x y");
        CHECK(stripped("\x03"
                       "123") == "3"); // two digits at most
        CHECK(stripped("\x03"
                       ",5x") == ",5x"); // no foreground: the comma is text
        CHECK(stripped("\x03"
                       "4,x") == ",x"); // a comma without a background digit stays
        CHECK(stripped("\x03"
                       "4,123") == "3"); // a background of two digits
        CHECK(stripped("\x04"
                       "ff8800orange\x04"
                       "ff8800,000000x\x04"
                       "y") == "orangexy");
        CHECK(stripped("\x04"
                       "ff88zz") == "ff88zz"); // not six hex digits: the code alone
        CHECK(stripped("") == "");
        CHECK(stripped("\x02\x03\x0f") == "");
}

TEST_CASE("Format::isUtf8 and toUtf8: UTF-8 kept, anything else read as Latin-1") {
        CHECK(Format::isUtf8("plain"));
        CHECK(Format::isUtf8("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"));
        CHECK_FALSE(Format::isUtf8("caf\xe9"));          // Latin-1
        CHECK_FALSE(Format::isUtf8("\xc0\xaf"));         // overlong
        CHECK_FALSE(Format::isUtf8("\xed\xa0\x80"));     // a surrogate
        CHECK_FALSE(Format::isUtf8("\xe2\x82"));         // cut short
        CHECK_FALSE(Format::isUtf8("\xf4\x90\x80\x80")); // past U+10FFFF
        CHECK(utf8("caf\xc3\xa9") == "caf\xc3\xa9");
        CHECK(utf8("caf\xe9") == "caf\xc3\xa9");
        CHECK(utf8("\xff") == "\xc3\xbf");
        CHECK(utf8(Text("a\0b", 3)) == "ab");
}

TEST_CASE("Format::parseCtcp: ACTION, VERSION, the closing byte missing, not one") {
        Format::Ctcp c;
        REQUIRE(Format::parseCtcp("\x01"
                                  "ACTION waves hello\x01",
                                  c));
        CHECK(c.command == Text("ACTION"));
        CHECK(c.args == Text("waves hello"));
        REQUIRE(Format::parseCtcp("\x01VERSION\x01", c));
        CHECK(c.command == Text("VERSION"));
        CHECK(c.args.isEmpty());
        REQUIRE(Format::parseCtcp("\x01PING 123", c));
        CHECK(c.args == Text("123"));
        CHECK_FALSE(Format::parseCtcp("hello", c));
        CHECK_FALSE(Format::parseCtcp("\x01\x01", c));
        CHECK_FALSE(Format::parseCtcp("", c));
}

TEST_CASE("Format::mentions: a word of its own, any case, codes ignored") {
        using irc::CaseMapping;
        CHECK(Format::mentions("jth: did it land?", "jth", CaseMapping::Rfc1459));
        CHECK(Format::mentions("ask JTH", "jth", CaseMapping::Rfc1459));
        CHECK(Format::mentions("hi \x02jth\x02!", "jth", CaseMapping::Rfc1459));
        CHECK(Format::mentions("x[y] there", "X{Y}", CaseMapping::Rfc1459));
        CHECK_FALSE(Format::mentions("jthx and xjth", "jth", CaseMapping::Rfc1459));
        CHECK_FALSE(Format::mentions("jth_ is someone else", "jth", CaseMapping::Rfc1459));
        CHECK(Format::mentions("<jth>", "jth", CaseMapping::Rfc1459));
        CHECK_FALSE(Format::mentions("anything", "", CaseMapping::Rfc1459));
}

TEST_CASE("Format::parseServerTime: milliseconds since 1970, or not a time") {
        int64_t ms = 0;
        REQUIRE(Format::parseServerTime("1970-01-01T00:00:00.000Z", ms));
        CHECK(ms == 0);
        REQUIRE(Format::parseServerTime("2026-11-16T10:41:00.123Z", ms));
        CHECK(ms == 1794825660123);
        REQUIRE(Format::parseServerTime("2000-02-29T23:59:59Z", ms));
        CHECK(ms == 951868799000);
        REQUIRE(Format::parseServerTime("2026-11-16T10:41:00.5Z", ms));
        CHECK(ms % 1000 == 500);
        CHECK_FALSE(Format::parseServerTime("2026-11-16 10:41:00Z", ms));
        CHECK_FALSE(Format::parseServerTime("2026-13-16T10:41:00Z", ms));
        CHECK_FALSE(Format::parseServerTime("2026-11-16T10:41:00", ms));
        CHECK_FALSE(Format::parseServerTime("garbage", ms));
}

TEST_CASE("Format::base64: SASL's, with padding") {
        irc::String out;
        REQUIRE(Format::base64(Text("\0jth\0secret", 11), out) == Error::Ok);
        CHECK(out == Text("AGp0aABzZWNyZXQ="));
        REQUIRE(Format::base64("", out) == Error::Ok);
        CHECK(out.isEmpty());
        REQUIRE(Format::base64("ab", out) == Error::Ok);
        CHECK(out == Text("YWI="));
        REQUIRE(Format::base64("abc", out) == Error::Ok);
        CHECK(out == Text("YWJj"));
}
