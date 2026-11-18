/**
 * @file      commands.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What is typed (irc/core/commands.h) and Tab (irc/core/complete.h).
 */

#include "commands.h"
#include "complete.h"
#include "helpers.h"
#include "ignore.h"

using irc::Buffer;
using irc::Error;
using irc::Outcome;
using irc::Text;
using Lines = std::vector<std::string>;

namespace {

        // A session registered as jth in #bmds (alice, alan, bob and us), its buffers, a command line.
        struct World {
                        irc::Buffers   buffers;
                        test::Server   s{test::config("jth")};
                        Outcome        outcome;
                        uint32_t       network = 0, bmds = 0;
                        irc::Completer completer;

                        World() {
                                s.buffers = &buffers;
                                s.registerAs("jth");
                                s.say(":jth!u@h JOIN #bmds");
                                s.say(":srv 353 jth = #bmds :@alice alan bob jth");
                                s.say(":srv 366 jth #bmds :End");
                                s.say(":srv 332 jth #bmds :the topic");
                                network = buffers.network().id;
                                bmds = buffers.find("#bmds", irc::CaseMapping::Rfc1459)->id;
                                s.take();
                                s.takeLines();
                        }

                        // @p typed in buffer @p in: what was sent (paced out in full), the lines told.
                        Lines type(const char *typed, uint32_t in) {
                                irc::Vector<irc::Line> lines;
                                REQUIRE(irc::runCommand(typed, s.session, buffers, in, s.now, lines, outcome) ==
                                        Error::Ok);
                                s.keep(lines);
                                s.wait(3000); // paced lines out
                                Lines sent = s.take();
                                shown = s.takeLines();
                                // The server answers now and then, so the session's keepalive stays quiet.
                                s.say(":srv NOTICE jth :still here");
                                s.events.clear();
                                s.takeLines();
                                return sent;
                        }

                        // The last line the command made.
                        std::string told() { return shown.empty() ? std::string() : shown.back().view; }

                        std::vector<test::ShownLine> shown; // the last command's lines

                        // Tab on @p typed with the cursor at its end (or @p cursor).
                        std::string tab(std::string typed, size_t cursor = std::string::npos) {
                                irc::String line;
                                (void)line.assign(test::text(typed));
                                size_t at = cursor == std::string::npos ? typed.size() : cursor;
                                if (!completer.complete(line, at, s.session, buffers, bmds)) return "(none)";
                                last = test::str(line);
                                lastCursor = at;
                                return last;
                        }
                        // Tab again on what the last Tab left.
                        std::string again() { return tab(last, lastCursor); }

                        std::string last;
                        size_t      lastCursor = 0;
        };

} // namespace

TEST_CASE("Commands: text said where it is typed; //text; nowhere to say it") {
        World w;
        CHECK(w.type("hello", w.bmds) == Lines{"PRIVMSG #bmds :hello"});
        CHECK(w.type("//etc/passwd is a file", w.bmds) == Lines{"PRIVMSG #bmds :/etc/passwd is a file"});
        w.s.takeLines();
        CHECK(w.type("hello", w.network).empty());
        CHECK(w.told().find("Nothing to say it to here") != std::string::npos);
        CHECK(w.type("", w.bmds).empty());
}

TEST_CASE("Commands: /me /msg /notice /query /ns /cs") {
        World w;
        CHECK(w.type("/me waves", w.bmds) == Lines{"PRIVMSG #bmds :\x01"
                                                   "ACTION waves\x01"});
        CHECK(w.type("/msg alice hi there", w.bmds) == Lines{"PRIVMSG alice :hi there"});
        REQUIRE(w.buffers.find("alice", irc::CaseMapping::Rfc1459) != nullptr); // shown in a private chat
        CHECK(w.type("/notice bob heads up", w.bmds) == Lines{"NOTICE bob :heads up"});
        CHECK(w.type("/query carol", w.bmds).empty());
        CHECK(w.outcome.action == Outcome::Action::Show);
        const Buffer *carol = w.buffers.find("carol", irc::CaseMapping::Rfc1459);
        REQUIRE(carol != nullptr);
        CHECK(w.outcome.buffer == carol->id);
        CHECK(w.type("/query carol hello", w.bmds) == Lines{"PRIVMSG carol :hello"});
        w.s.takeLines();
        CHECK(w.type("/ns IDENTIFY jth secret", w.network) == Lines{"PRIVMSG NickServ :IDENTIFY jth secret"});
        REQUIRE_FALSE(w.shown.empty());
        CHECK(w.shown.back().log.find("secret") == std::string::npos);
        CHECK(w.type("/cs OP #bmds", w.network) == Lines{"PRIVMSG ChanServ :OP #bmds"});
        CHECK(w.type("/msg alice", w.bmds).empty());
        CHECK(w.told().find("Usage: /msg") != std::string::npos);
        CHECK(w.type("/query #bmds", w.bmds).empty());
        CHECK(w.told().find("Usage: /query") != std::string::npos);
}

TEST_CASE("Commands: /join /part /close /nick /topic /away /back") {
        World w;
        CHECK(w.type("/join bmds2", w.bmds) == Lines{"JOIN #bmds2"});
        CHECK(w.type("/j #a,#b k1", w.bmds) == Lines{"JOIN #a k1", "JOIN #b"});
        CHECK(w.type("/part", w.bmds) == Lines{"PART #bmds"});
        CHECK(w.type("/part #x bye now", w.bmds) == Lines{"PART #x :bye now"});
        CHECK(w.type("/part going", w.bmds) == Lines{"PART #bmds :going"});
        CHECK(w.type("/part", w.network).empty());
        CHECK(w.told().find("Usage: /part") != std::string::npos);
        CHECK(w.type("/close", w.bmds) == Lines{"PART #bmds"});
        CHECK(w.outcome.action == Outcome::Action::Close);
        CHECK(w.outcome.buffer == w.bmds);
        CHECK(w.type("/close", w.network).empty());
        CHECK(w.outcome.action == Outcome::Action::None);
        CHECK(w.type("/nick jth2", w.bmds) == Lines{"NICK jth2"});
        CHECK(w.type("/topic", w.bmds).empty());
        CHECK(w.told().find("--\x0f\tTopic: the topic") != std::string::npos);
        CHECK(w.type("/topic a new one", w.bmds) == Lines{"TOPIC #bmds :a new one"});
        CHECK(w.type("/topic #other theirs", w.bmds) == Lines{"TOPIC #other :theirs"});
        CHECK(w.type("/away lunch", w.bmds) == Lines{"AWAY :lunch"});
        CHECK(w.type("/away", w.bmds) == Lines{"AWAY"}); // alone: back, as mIRC's
        CHECK(w.type("/back", w.bmds) == Lines{"AWAY"});
}

TEST_CASE("Commands: /whois /names /mode /kick /invite /raw /quit /clear /help, any other") {
        World w;
        CHECK(w.type("/whois alice", w.bmds) == Lines{"WHOIS alice"});
        CHECK(w.type("/names", w.bmds) == Lines{"NAMES #bmds"});
        CHECK(w.type("/mode +o alice", w.bmds) == Lines{"MODE #bmds +o alice"});
        CHECK(w.type("/mode jth +i", w.bmds) == Lines{"MODE jth +i"});
        CHECK(w.type("/kick bob go away", w.bmds) == Lines{"KICK #bmds bob :go away"});
        CHECK(w.type("/kick bob", w.bmds) == Lines{"KICK #bmds bob"});
        CHECK(w.type("/invite carol", w.bmds) == Lines{"INVITE carol #bmds"});
        CHECK(w.type("/invite carol #other", w.bmds) == Lines{"INVITE carol #other"});
        CHECK(w.type("/quote PRIVMSG x :y", w.bmds) == Lines{"PRIVMSG x :y"});
        CHECK(w.type("/version", w.bmds) == Lines{"VERSION"});
        CHECK(w.type("/foo bar baz", w.bmds) == Lines{"FOO bar baz"});
        CHECK(w.type("/clear", w.bmds).empty());
        CHECK(w.outcome.action == Outcome::Action::Clear);
        w.s.takeLines();
        CHECK(w.type("/help", w.bmds).empty());
        CHECK(w.told().find("/join") != std::string::npos);
        CHECK(w.type("/raw", w.bmds).empty());
        CHECK(w.told().find("Usage: /raw") != std::string::npos);
        CHECK(w.type("/quit bye all", w.bmds) == Lines{"QUIT :bye all"});
        CHECK(w.outcome.action == Outcome::Action::Quit);
}

TEST_CASE("Commands: a line break typed is refused and told, nothing sent") {
        World w;
        CHECK(w.type("/raw PRIVMSG #a :x\r\nQUIT", w.bmds).empty());
        CHECK(w.told().find("not something that can be sent") != std::string::npos);
        CHECK(w.type("/msg alice hi\nQUIT", w.bmds).empty());
        CHECK(w.told().find("not something that can be sent") != std::string::npos);
}

TEST_CASE("Tab: nicks (who spoke last first), each Tab the next; channels; commands; none") {
        World w;
        w.s.say(":bob!b@h PRIVMSG #bmds :first");
        w.s.say(":alan!a@h PRIVMSG #bmds :second");
        // alan spoke last, then the rest by name: alice. bob doesn't match.
        CHECK(w.tab("a") == "alan: ");
        CHECK(w.again() == "alice: ");
        CHECK(w.again() == "alan: ");
        CHECK(w.tab("hi A") == "hi alan ");
        CHECK(w.tab("b") == "bob: ");
        CHECK(w.tab("j").find("(none)") != std::string::npos); // ourselves not offered
        // In the middle of a line: the rest kept, the cursor after the nick.
        CHECK(w.tab("al how are you", 2) == "alan: how are you");
        CHECK(w.lastCursor == 5);
        CHECK(w.tab("al how are you", 2) == "alan: how are you"); // a different line: starts over
        CHECK(w.tab("#b") == "#bmds ");
        CHECK(w.tab("/jo") == "/join ");
        CHECK(w.tab("/q") == "/query ");
        CHECK(w.again() == "/quit ");
        CHECK(w.again() == "/quote ");
        CHECK(w.tab("zz") == "(none)");
        CHECK(w.tab("") == "(none)");
        CHECK(w.tab("hello ") == "(none)");
        // Someone who left isn't offered, though they spoke.
        w.s.say(":alan!a@h PART #bmds");
        CHECK(w.tab("a") == "alice: ");
}

TEST_CASE("Commands: the operator's in MODES-sized lines, bans from a host, kick and ban, half-ops") {
        World w;
        CHECK(w.type("/op alice bob alan jth", w.bmds) == Lines{"MODE #bmds +ooo alice bob alan", "MODE #bmds +o jth"});
        CHECK(w.type("/devoice #other a", w.bmds) == Lines{"MODE #other -v a"});
        CHECK(w.type("/deop", w.bmds).empty());
        CHECK(w.told().find("Usage: /op") != std::string::npos);
        CHECK(w.type("/op bob", w.network).empty()); // no channel in view, none named
        CHECK(w.type("/hop bob", w.bmds).empty());
        CHECK(w.told().find("no half-operators") != std::string::npos);
        w.s.say(":srv 005 jth MODES=4 PREFIX=(qaohv)~&@%+ :are supported");
        CHECK(w.type("/hop a b c d e", w.bmds) == Lines{"MODE #bmds +hhhh a b c d", "MODE #bmds +h e"});
        CHECK(w.type("/dehop a", w.bmds) == Lines{"MODE #bmds -h a"});

        // A ban by their host when it is known (their join), else by nick; a mask as given.
        w.s.say(":carol!c@host.example JOIN #bmds");
        CHECK(w.type("/ban carol", w.bmds) == Lines{"MODE #bmds +b *!*@host.example"});
        CHECK(w.type("/ban bob", w.bmds) == Lines{"MODE #bmds +b bob!*@*"});
        CHECK(w.type("/unban *!*@spam.example", w.bmds) == Lines{"MODE #bmds -b *!*@spam.example"});
        CHECK(w.type("/ban #other spammer!x", w.bmds) == Lines{"MODE #other +b spammer!x@*"});
        CHECK(w.type("/kb carol go away", w.bmds) ==
              Lines{"MODE #bmds +b *!*@host.example", "KICK #bmds carol :go away"});
        CHECK(w.type("/kb carol", w.bmds) == Lines{"MODE #bmds +b *!*@host.example", "KICK #bmds carol"});
        CHECK(w.type("/ban", w.bmds).empty());
        CHECK(w.type("/ban a*b", w.bmds).empty()); // a nick with a star: not one
        CHECK(w.told().find("Not a nick or a mask") != std::string::npos);
}

TEST_CASE("Commands: CTCP and /ping, /who /whowas, /mode alone, /away alone") {
        World w;
        CHECK(w.type("/ctcp alice version", w.bmds) == Lines{"PRIVMSG alice :\x01VERSION\x01"});
        CHECK(w.type("/ctcp alice clientinfo now", w.bmds) == Lines{"PRIVMSG alice :\x01"
                                                                    "CLIENTINFO now\x01"});
        const Lines ping = w.type("/ping alice", w.bmds);
        REQUIRE(ping.size() == 1);
        CHECK(ping[0].rfind("PRIVMSG alice :\x01PING ", 0) == 0);
        CHECK(w.type("/ping", w.bmds).empty());
        CHECK(w.type("/who #bmds", w.bmds) == Lines{"WHO #bmds"});
        CHECK(w.type("/whowas bob", w.bmds) == Lines{"WHOWAS bob"});
        CHECK(w.type("/mode", w.bmds) == Lines{"MODE #bmds"});
        CHECK(w.type("/mode", w.network) == Lines{"MODE jth"});
}

TEST_CASE("Commands: what the window does: /list /find /ignore /notify /server") {
        World w;
        CHECK(w.type("/list linux", w.bmds).empty());
        CHECK(w.outcome.action == Outcome::Action::List);
        CHECK(w.outcome.text == Text("linux"));
        CHECK(w.type("/find needle", w.bmds).empty());
        CHECK(w.outcome.action == Outcome::Action::Find);
        CHECK(w.outcome.buffer == w.bmds);
        CHECK(w.outcome.text == Text("needle"));

        CHECK(w.type("/ignore bob", w.bmds).empty());
        CHECK(w.outcome.action == Outcome::Action::Ignore);
        CHECK(w.outcome.on);
        CHECK(w.outcome.text == Text("bob!*@*"));
        CHECK(w.outcome.extra.isEmpty());
        w.type("/ignore *@spam.example Libera.Chat", w.bmds);
        CHECK(w.outcome.text == Text("*!*@spam.example"));
        CHECK(w.outcome.extra == Text("Libera.Chat"));
        w.type("/ignore", w.bmds); // the list
        CHECK(w.outcome.action == Outcome::Action::Ignore);
        CHECK(w.outcome.text.isEmpty());
        w.type("/unignore bob", w.bmds);
        CHECK(w.outcome.action == Outcome::Action::Ignore);
        CHECK(!w.outcome.on);
        w.type("/unignore", w.bmds);
        CHECK(w.outcome.action == Outcome::Action::None);
        w.type("/ignore !x@y", w.bmds);
        CHECK(w.outcome.action == Outcome::Action::None);

        w.type("/notify alice", w.bmds);
        CHECK(w.outcome.action == Outcome::Action::Notify);
        CHECK(w.outcome.on);
        CHECK(w.outcome.text == Text("alice"));
        w.type("/unnotify alice", w.bmds);
        CHECK(!w.outcome.on);
        w.type("/notify #bmds", w.bmds);
        CHECK(w.outcome.action == Outcome::Action::None);

        const struct {
                        const char *typed, *host;
                        uint16_t    port;
                        bool        tls, fresh;
        } servers[] = {{"/server irc.example.net", "irc.example.net", 6697, true, false},
                       {"/server -m irc.example.net 6667", "irc.example.net", 6667, false, true},
                       {"/server irc.example.net +7000", "irc.example.net", 7000, true, false},
                       {"/server irc.example.net:6697", "irc.example.net", 6697, true, false},
                       {"/server irc.example.net:6667", "irc.example.net", 6667, false, false}};
        for (const auto &s : servers) {
                CAPTURE(s.typed);
                CHECK(w.type(s.typed, w.bmds).empty());
                CHECK(w.outcome.action == Outcome::Action::Server);
                CHECK(w.outcome.text == Text(s.host));
                CHECK(w.outcome.port == s.port);
                CHECK(w.outcome.on == s.tls);
                CHECK(w.outcome.fresh == s.fresh);
        }
        w.type("/server", w.bmds);
        CHECK(w.outcome.action == Outcome::Action::None);
        w.type("/server host 70000", w.bmds);
        CHECK(w.outcome.action == Outcome::Action::None);
}

TEST_CASE("Ban masks and ignore masks: normalized, matched with wildcards and casemapping") {
        irc::String m;
        CHECK(irc::banMask("bob", "b@host.example", m) == Error::Ok);
        CHECK(m == Text("*!*@host.example"));
        CHECK(irc::banMask("bob", "", m) == Error::Ok);
        CHECK(m == Text("bob!*@*"));
        CHECK(irc::banMask("x!y@z", "", m) == Error::Ok);
        CHECK(m == Text("x!y@z"));
        CHECK(irc::banMask("", "", m) == Error::Invalid);

        CHECK(irc::Ignores::normalize("u@h", m) == Error::Ok);
        CHECK(m == Text("*!u@h"));
        CHECK(irc::Ignores::normalize("n!u", m) == Error::Ok);
        CHECK(m == Text("n!u@*"));
        CHECK(irc::Ignores::normalize("a b", m) == Error::Invalid);
        CHECK(irc::Ignores::normalize("n!@h", m) == Error::Invalid);
        CHECK(irc::Ignores::normalize("n!u@", m) == Error::Invalid);

        const auto r = irc::CaseMapping::Rfc1459;
        CHECK(irc::matchesMask("*", "", r));
        CHECK(irc::matchesMask("a*b?d", "aXXbcd", r));
        CHECK(!irc::matchesMask("a*b?d", "aXXbd", r));
        CHECK(irc::matchesMask("[x]*", "{X}yz", r)); // rfc1459: [ is {
        CHECK(!irc::matchesMask("[x]*", "{X}yz", irc::CaseMapping::Ascii));
        CHECK(!irc::matchesMask("*a*a*a*a*a*a*a*a*b", std::string(200, 'a').c_str(), r)); // no blow-up

        irc::Ignores ig;
        REQUIRE(ig.read("bob, *!*@spam.example@Libera.Chat, , carol!*@*") == Error::Ok);
        REQUIRE(ig.size() == 3);
        CHECK(ig.networkAt(1) == Text("Libera.Chat"));
        CHECK(ig.matches("BOB", "u", "h", "OFTC", r));
        CHECK(ig.matches("x", "y", "spam.example", "libera.chat", r));
        CHECK(!ig.matches("x", "y", "spam.example", "OFTC", r));
        CHECK(!ig.matches("alice", "", "", "OFTC", r));
        CHECK(ig.add("bob", "") == Error::Ok); // already there
        CHECK(ig.size() == 3);
        CHECK(ig.remove("carol", ""));
        CHECK(!ig.remove("carol", ""));
        irc::String out;
        REQUIRE(ig.write(out) == Error::Ok);
        CHECK(out == Text("bob!*@*, *!*@spam.example@Libera.Chat"));
}

TEST_CASE("Perform's lines: $me our nick, a / added") {
        irc::String out;
        CHECK(irc::performLine("/mode $me +x", "jth", out) == Error::Ok);
        CHECK(out == Text("/mode jth +x"));
        CHECK(irc::performLine("  msg Q@CServe.quakenet.org AUTH $me secret", "jth", out) == Error::Ok);
        CHECK(out == Text("/msg Q@CServe.quakenet.org AUTH jth secret"));
        CHECK(irc::performLine("/echo $meal $me", "jth", out) == Error::Ok);
        CHECK(out == Text("/echo $meal jth"));
        CHECK(irc::performLine("   ", "jth", out) == Error::Invalid);
}
