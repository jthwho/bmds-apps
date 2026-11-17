/**
 * @file      str.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * IRC's core's containers (irc/core/str.h), casemapping and ISUPPORT (irc/core/isupport.h).
 */

#include "helpers.h"
#include "isupport.h"
#include "str.h"

using irc::Error;
using irc::Text;

namespace {

        // Counts its constructions and destructions: a Vector's moves leak or double nothing.
        struct Counted {
                        static int alive;
                        int        value = 0;
                        explicit Counted(int v) : value(v) { ++alive; }
                        Counted(Counted &&other) noexcept : value(other.value) { ++alive; }
                        Counted &operator=(Counted &&other) noexcept {
                                value = other.value;
                                return *this;
                        }
                        ~Counted() { --alive; }
        };
        int Counted::alive = 0;

} // namespace

TEST_CASE("String: assign, append, move, clear") {
        irc::String s;
        CHECK(s.isEmpty());
        CHECK(s.view() == Text(""));
        REQUIRE(s.assign("hello") == Error::Ok);
        REQUIRE(s.append(", ") == Error::Ok);
        REQUIRE(s.append('w') == Error::Ok);
        REQUIRE(s.append("orld, and on past sixteen bytes") == Error::Ok);
        CHECK(s == Text("hello, world, and on past sixteen bytes"));
        REQUIRE(s.assign(s.view().mid(7, 5)) == Error::Ok); // from itself
        CHECK(s == Text("world"));
        irc::String t(std::move(s));
        CHECK(t == Text("world"));
        CHECK(s.isEmpty());
        s = std::move(t);
        CHECK(s == Text("world"));
        s.clear();
        CHECK(s.isEmpty());
        REQUIRE(s.append(Text("a\0b", 3)) == Error::Ok);
        CHECK(s.size() == 3);
}

TEST_CASE("Vector: append, insert, remove, move; nothing leaked") {
        {
                irc::Vector<Counted> v;
                for (int i = 0; i < 10; ++i) REQUIRE(v.append(Counted(i)) == Error::Ok);
                CHECK(v.size() == 10);
                REQUIRE(v.insert(0, Counted(-1)) == Error::Ok);
                REQUIRE(v.insert(5, Counted(100)) == Error::Ok);
                REQUIRE(v.insert(99, Counted(200)) == Error::Ok); // past the end: at the end
                CHECK(v.size() == 13);
                CHECK(v[0].value == -1);
                CHECK(v[1].value == 0);
                CHECK(v[5].value == 100);
                CHECK(v[6].value == 4);
                CHECK(v.last().value == 200);
                v.removeAt(5);
                v.removeAt(0);
                v.removeAt(99); // none
                CHECK(v.size() == 11);
                for (int i = 0; i < 10; ++i) CHECK(v[size_t(i)].value == i);
                irc::Vector<Counted> w(std::move(v));
                CHECK(v.size() == 0);
                CHECK(w.size() == 11);
                CHECK(Counted::alive == 11);
                w.clear();
                CHECK(Counted::alive == 0);
                REQUIRE(w.append(Counted(7)) == Error::Ok);
        }
        CHECK(Counted::alive == 0);

        irc::Vector<irc::String> names;
        for (const char *n : {"a", "bb", "ccc"}) {
                irc::String s;
                REQUIRE(s.assign(n) == Error::Ok);
                REQUIRE(names.append(std::move(s)) == Error::Ok);
        }
        CHECK(names[2] == Text("ccc"));
}

TEST_CASE("Casemapping: rfc1459, strict, ascii") {
        using irc::CaseMapping;
        CHECK(irc::sameName("Nick", "nICK", CaseMapping::Ascii));
        CHECK_FALSE(irc::sameName("a[b]", "a{b}", CaseMapping::Ascii));
        CHECK(irc::sameName("a[b]\\", "A{B}|", CaseMapping::Rfc1459));
        CHECK(irc::sameName("x~", "X^", CaseMapping::Rfc1459));
        CHECK_FALSE(irc::sameName("x~", "X^", CaseMapping::StrictRfc1459));
        CHECK(irc::sameName("a[b]\\", "A{B}|", CaseMapping::StrictRfc1459));
        CHECK_FALSE(irc::sameName("abc", "abcd", CaseMapping::Rfc1459));
}

TEST_CASE("Isupport: defaults, tokens, negation, modes and prefixes") {
        irc::Isupport is;
        CHECK(is.caseMapping() == irc::CaseMapping::Rfc1459);
        CHECK(is.isChannel("#bmds"));
        CHECK(is.isChannel("&local"));
        CHECK_FALSE(is.isChannel("nick"));
        CHECK_FALSE(is.isChannel(""));
        CHECK(is.prefixFor('o') == '@');
        CHECK(is.prefixFor('v') == '+');
        CHECK(is.prefixFor('b') == 0);

        irc::Message m;
        REQUIRE(irc::parse(":srv 005 jth CASEMAPPING=ascii CHANTYPES=# NETWORK=Libera.Chat PREFIX=(qaohv)~&@%+ "
                           "CHANMODES=eIbq,k,flj,CFLMPQScgimnprstuz :are supported by this server",
                           m) == Error::Ok);
        is.apply(m);
        CHECK(is.caseMapping() == irc::CaseMapping::Ascii);
        CHECK(is.network() == Text("Libera.Chat"));
        CHECK_FALSE(is.isChannel("&local"));
        CHECK(is.prefixModes() == Text("qaohv"));
        CHECK(is.prefixChars() == Text("~&@%+"));
        CHECK(is.prefixFor('h') == '%');
        CHECK(is.rank('~') == 0);
        CHECK(is.rank('+') == 4);
        CHECK(is.rank('x') == 5);
        // Modes and their parameters: prefixes and lists always, k always, l and f only when set.
        CHECK(is.takesParameter('o', false));
        CHECK(is.takesParameter('q', true)); // a prefix here (and a list too)
        CHECK(is.takesParameter('b', false));
        CHECK(is.takesParameter('k', false));
        CHECK(is.takesParameter('l', true));
        CHECK_FALSE(is.takesParameter('l', false));
        CHECK_FALSE(is.takesParameter('n', true));
        CHECK(is.withoutStatusPrefix("@#bmds") == Text("#bmds"));
        CHECK(is.withoutStatusPrefix("%+#bmds") == Text("#bmds"));
        CHECK(is.withoutStatusPrefix("@nick") == Text("@nick"));
        CHECK(is.withoutStatusPrefix("#bmds") == Text("#bmds"));

        REQUIRE(irc::parse(":srv 005 jth -CHANTYPES -PREFIX :are supported", m) == Error::Ok);
        is.apply(m);
        CHECK(is.isChannel("&local"));
        CHECK(is.prefixChars() == Text("@+"));
        REQUIRE(irc::parse(":srv 005 jth PREFIX= :are supported", m) == Error::Ok);
        is.apply(m);
        CHECK(is.prefixChars() == Text(""));
        CHECK(is.prefixFor('o') == 0);
        REQUIRE(irc::parse(":srv 005 jth PREFIX=(ov)@ :bad: counts differ, ignored", m) == Error::Ok);
        is.apply(m);
        CHECK(is.prefixChars() == Text(""));
        is.reset();
        CHECK(is.prefixChars() == Text("@+"));
        CHECK(is.network() == Text(""));
}
