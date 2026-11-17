/**
 * @file      message.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * IRC's messages (irc/core/message.h): parsed, built, split and cut from bytes.
 */

#include "message.h"

#include <doctest.h>

#include <string>
#include <vector>

using irc::Error;
using irc::Text;

namespace doctest {
        template <> struct StringMaker<Text> {
                        static String convert(Text text) {
                                return String("\"") + String(text.data(), String::size_type(text.size())) + "\"";
                        }
        };
} // namespace doctest

namespace {

        Text text(const std::string &s) {
                return Text(s.data(), s.size());
        }

        // Every line @p reader makes of @p chunks, fed in that order.
        std::vector<std::string> lines(irc::LineReader &reader, const std::vector<std::string> &chunks) {
                std::vector<std::string> out;
                for (const std::string &chunk : chunks)
                        reader.feed(text(chunk), [&](Text line) { out.emplace_back(line.data(), line.size()); });
                return out;
        }

} // namespace

TEST_CASE("Text: views, finds, case") {
        Text t("PRIVMSG #bmds");
        CHECK(t.size() == 13);
        CHECK(t.find(' ') == 7);
        CHECK(t.find('x') == Text::NoPos);
        CHECK(t.mid(8) == Text("#bmds"));
        CHECK(t.mid(20).isEmpty());
        CHECK(t.left(3) == Text("PRI"));
        CHECK(t.startsWith("PRIV"));
        CHECK_FALSE(t.startsWith("PRIVMSG #bmds!"));
        CHECK(Text("privmsg").equalsIgnoringCase("PRIVMSG"));
        CHECK_FALSE(Text("privmsg").equalsIgnoringCase("PRIVMSGS"));
        CHECK(Text("a.b.c").findLast('.') == 3);
        CHECK(Text(nullptr).isEmpty());
}

TEST_CASE("parse: every part") {
        irc::Message m;
        REQUIRE(irc::parse("@time=2026-11-16T10:41:00.000Z;account=alice :alice!a@host PRIVMSG #bmds :hi there", m) ==
                Error::Ok);
        CHECK(m.tags == Text("time=2026-11-16T10:41:00.000Z;account=alice"));
        CHECK(m.source == Text("alice!a@host"));
        CHECK(m.command == Text("PRIVMSG"));
        CHECK(m.is("privmsg"));
        REQUIRE(m.paramCount == 2);
        CHECK(m.param(0) == Text("#bmds"));
        CHECK(m.param(1) == Text("hi there"));
        CHECK(m.last() == Text("hi there"));
        CHECK(m.param(2).isEmpty());
        CHECK(m.numeric() == -1);
        CHECK(m.tag("time") == Text("2026-11-16T10:41:00.000Z"));
        CHECK(m.tag("account") == Text("alice"));
        CHECK(m.hasTag("account"));
        CHECK_FALSE(m.hasTag("acc"));
        CHECK(m.tag("msgid").isEmpty());
}

TEST_CASE("parse: no tags, no source, no parameters") {
        irc::Message m;
        REQUIRE(irc::parse("PING", m) == Error::Ok);
        CHECK(m.command == Text("PING"));
        CHECK(m.paramCount == 0);
        CHECK(m.source.isEmpty());
        CHECK(m.tags.isEmpty());
        CHECK(m.last().isEmpty());

        REQUIRE(irc::parse("PING :irc.libera.chat", m) == Error::Ok);
        CHECK(m.last() == Text("irc.libera.chat"));
}

TEST_CASE("parse: numerics, middle parameters, an empty last one, extra spaces") {
        irc::Message m;
        REQUIRE(irc::parse(":irc.example.net 005 jth CHANTYPES=# PREFIX=(ov)@+ :are supported by this server", m) ==
                Error::Ok);
        CHECK(m.numeric() == 5);
        REQUIRE(m.paramCount == 4);
        CHECK(m.param(1) == Text("CHANTYPES=#"));
        CHECK(m.param(2) == Text("PREFIX=(ov)@+"));

        REQUIRE(irc::parse(":srv 332 jth #bmds :", m) == Error::Ok);
        CHECK(m.numeric() == 332);
        REQUIRE(m.paramCount == 3);
        CHECK(m.last().isEmpty());

        REQUIRE(irc::parse("  :srv   NOTICE   *   :a  b  ", m) == Error::Ok);
        CHECK(m.source == Text("srv"));
        REQUIRE(m.paramCount == 2);
        CHECK(m.param(0) == Text("*"));
        CHECK(m.param(1) == Text("a  b  "));

        // A trailing space after the middle parameters adds none.
        REQUIRE(irc::parse("MODE #bmds +o jth ", m) == Error::Ok);
        CHECK(m.paramCount == 3);

        // A colon inside a middle parameter is its own.
        REQUIRE(irc::parse("JOIN #a:b", m) == Error::Ok);
        CHECK(m.param(0) == Text("#a:b"));

        CHECK(irc::parse("1a1", m) == Error::Ok);
        CHECK(m.numeric() == -1);
        CHECK(irc::parse("0001", m) == Error::Ok);
        CHECK(m.numeric() == -1);
}

TEST_CASE("parse: the fifteenth parameter takes the rest") {
        irc::Message m;
        REQUIRE(irc::parse("CMD 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 :17", m) == Error::Ok);
        REQUIRE(m.paramCount == irc::Message::MaxParams);
        CHECK(m.param(13) == Text("14"));
        CHECK(m.param(14) == Text("15 16 :17"));
}

TEST_CASE("parse: no command") {
        irc::Message m;
        CHECK(irc::parse("", m) == Error::Malformed);
        CHECK(irc::parse("   ", m) == Error::Malformed);
        CHECK(irc::parse("@a=b", m) == Error::Malformed);
        CHECK(irc::parse(":source", m) == Error::Malformed);
        CHECK(irc::parse("@a=b :source ", m) == Error::Malformed);
}

TEST_CASE("tags: values, keys alone, client tags, escapes") {
        irc::Message m;
        REQUIRE(irc::parse("@a=1;flag;+example.com/typing=active;;b=;c=x\\sy\\:z\\\\w :s CMD", m) == Error::Ok);
        CHECK(m.tag("a") == Text("1"));
        CHECK(m.hasTag("flag"));
        CHECK(m.tag("flag").isEmpty());
        CHECK(m.tag("+example.com/typing") == Text("active"));
        CHECK(m.hasTag("b"));
        CHECK(m.tag("b").isEmpty());

        Text   raw = m.tag("c");
        char   out[32];
        size_t n = irc::unescapeTag(raw, out, sizeof out);
        CHECK(Text(out, n) == Text("x y;z\\w"));

        n = irc::unescapeTag("a\\rb\\nc\\qd\\", out, sizeof out);
        CHECK(Text(out, n) == Text("a\rb\ncqd"));

        // Too small: what it would take, and only what fits written.
        n = irc::unescapeTag("abcdef", out, 3);
        CHECK(n == 6);
        CHECK(Text(out, 3) == Text("abc"));
}

TEST_CASE("parseSource: a user, a server, parts missing") {
        irc::Source s = irc::parseSource("alice!~a@user/alice");
        CHECK(s.nick == Text("alice"));
        CHECK(s.user == Text("~a"));
        CHECK(s.host == Text("user/alice"));
        CHECK_FALSE(s.isServer());

        s = irc::parseSource("irc.libera.chat");
        CHECK(s.nick == Text("irc.libera.chat"));
        CHECK(s.isServer());

        s = irc::parseSource("bob@host");
        CHECK(s.nick == Text("bob"));
        CHECK(s.user.isEmpty());
        CHECK(s.host == Text("host"));

        s = irc::parseSource("carol");
        CHECK(s.nick == Text("carol"));
        CHECK_FALSE(s.isServer());
}

TEST_CASE("LineBuilder: parameters, the last one, CR LF") {
        irc::LineBuilder b("PRIVMSG");
        b.arg("#bmds").last("hello: world");
        CHECK(b.error() == Error::Ok);
        CHECK(b.line() == Text("PRIVMSG #bmds :hello: world\r\n"));
        CHECK(b.line() == Text("PRIVMSG #bmds :hello: world\r\n"));

        irc::LineBuilder empty("TOPIC");
        empty.arg("#bmds").last("");
        CHECK(empty.line() == Text("TOPIC #bmds :\r\n"));

        irc::LineBuilder bare("QUIT");
        CHECK(bare.line() == Text("QUIT\r\n"));
}

TEST_CASE("LineBuilder: refuses what would change the line") {
        auto refused = [](irc::LineBuilder &b) {
                return b.error() == Error::Malformed && b.line().isEmpty();
        };

        irc::LineBuilder injected("PRIVMSG");
        injected.arg("#bmds").last("hi\r\nQUIT :bye");
        CHECK(refused(injected));

        irc::LineBuilder lf("PRIVMSG");
        lf.arg("#bmds").last("a\nb");
        CHECK(refused(lf));

        irc::LineBuilder nul("PRIVMSG");
        nul.arg("#bmds").last(Text("a\0b", 3));
        CHECK(refused(nul));

        irc::LineBuilder space("JOIN");
        space.arg("#a b");
        CHECK(refused(space));

        irc::LineBuilder colon("PRIVMSG");
        colon.arg(":x").last("y");
        CHECK(refused(colon));

        irc::LineBuilder empty("MODE");
        empty.arg("");
        CHECK(refused(empty));

        irc::LineBuilder after("PRIVMSG");
        after.last("x").arg("y");
        CHECK(refused(after));

        irc::LineBuilder twice("PRIVMSG");
        twice.last("x").last("y");
        CHECK(refused(twice));

        irc::LineBuilder badCommand("PRIV MSG");
        CHECK(refused(badCommand));
        irc::LineBuilder noCommand("");
        CHECK(refused(noCommand));
        // A command parsed from "@a @b :x" is "@b": built again it would be tags (the fuzzer's).
        irc::LineBuilder tagLike("@b");
        CHECK(refused(tagLike));
        irc::LineBuilder sourceLike(":b");
        CHECK(refused(sourceLike));
}

TEST_CASE("LineBuilder: 512 bytes at most, its CR LF counted") {
        // "PRIVMSG #c :" is 12 bytes: 498 more fill 510, and CR LF 512.
        std::string      fits(498, 'x');
        irc::LineBuilder ok("PRIVMSG");
        ok.arg("#c").last(text(fits));
        CHECK(ok.error() == Error::Ok);
        CHECK(ok.line().size() == 512);

        std::string      over(499, 'x');
        irc::LineBuilder tooLong("PRIVMSG");
        tooLong.arg("#c").last(text(over));
        CHECK(tooLong.error() == Error::TooLong);
        CHECK(tooLong.line().isEmpty());
}

TEST_CASE("splitAt: at a space, whole characters, never nothing") {
        CHECK(irc::splitAt("short", 10) == 5);
        CHECK(irc::splitAt("exactly", 7) == 7);
        CHECK(irc::splitAt("", 0) == 0);

        // At the last space in reach; the space itself starts the rest.
        CHECK(irc::splitAt("one two three four", 12) == 7);
        // A space right at the limit: the first piece is exactly the limit.
        CHECK(irc::splitAt("one two three", 7) == 7);
        // Only a space in the first half: cut where it reaches the limit instead.
        CHECK(irc::splitAt("a bcdefghijklmnop", 10) == 10);
        CHECK(irc::splitAt("abcdefghijklmnop", 10) == 10);

        // "é" is 2 bytes and "€" 3: never cut inside one.
        CHECK(irc::splitAt("ééééé", 5) == 4);
        CHECK(irc::splitAt("a€€", 3) == 1);
        CHECK(irc::splitAt("a€€", 4) == 4);
        CHECK(irc::splitAt("€", 2) == 0);

        // Not UTF-8: continuation bytes alone, cut after at most three steps back.
        CHECK(irc::splitAt("\x80\x80\x80\x80\x80\x80", 5) == 2);

        // Split again and again, every piece fits and the text comes back.
        std::string long_ = "the quick brown fox jumps over the lazy dog, then the tired fox sleeps; ";
        for (int i = 0; i < 4; ++i) long_ += long_;
        Text        rest = text(long_);
        std::string joined;
        int         pieces = 0;
        while (!rest.isEmpty()) {
                size_t n = irc::splitAt(rest, 100);
                REQUIRE(n > 0);
                REQUIRE(n <= 100);
                if (!joined.empty()) joined += ' ';
                joined.append(rest.data(), n);
                rest = rest.mid(n);
                if (!rest.isEmpty() && rest[0] == ' ') rest = rest.mid(1);
                ++pieces;
        }
        CHECK(pieces > 1);
        // The last piece keeps the text's trailing space; a break's space is put back.
        CHECK(joined == long_);
}

TEST_CASE("LineReader: LF or CR LF, any chunks, empty lines skipped") {
        irc::LineReader reader;
        auto            got = lines(reader, {"PING :a\r\n:s 001 jth :hi\n\r\n\nPI", "NG", " :b\r", "\n"});
        REQUIRE(got.size() == 3);
        CHECK(got[0] == "PING :a");
        CHECK(got[1] == ":s 001 jth :hi");
        CHECK(got[2] == "PING :b");

        // A byte at a time.
        std::string              all = "A\r\nB\r\nC\r\n";
        std::vector<std::string> bytes;
        for (char c : all) bytes.emplace_back(1, c);
        got = lines(reader, bytes);
        CHECK(got == std::vector<std::string>{"A", "B", "C"});

        // A part line forgotten.
        lines(reader, {"PART"});
        reader.reset();
        got = lines(reader, {"X\n"});
        CHECK(got == std::vector<std::string>{"X"});
        CHECK(reader.dropped() == 0);
}

TEST_CASE("LineReader: the longest line kept, a longer one dropped whole") {
        irc::LineReader reader;
        const size_t    most = irc::LineReader::MaxLine;

        std::string longest(most, 'x');
        auto        got = lines(reader, {longest + "\r\n", "after\r\n"});
        REQUIRE(got.size() == 2);
        CHECK(got[0].size() == most);
        CHECK(got[1] == "after");

        // One byte more, with and without its CR; then far longer, in pieces: each dropped, the
        // line after it read.
        got = lines(reader, {std::string(most + 1, 'y') + "\n", "one\n"});
        CHECK(got == std::vector<std::string>{"one"});
        CHECK(reader.dropped() == 1);

        got = lines(reader, {std::string(most + 1, 'y') + "\r\n", "two\n"});
        CHECK(got == std::vector<std::string>{"two"});
        CHECK(reader.dropped() == 2);

        std::string huge(5 * most, 'z');
        got = lines(reader, {huge.substr(0, 3 * most), huge.substr(3 * most), "\nthree\n"});
        CHECK(got == std::vector<std::string>{"three"});
        CHECK(reader.dropped() == 3);
}
