/**
 * @file      session.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * One network's protocol (irc/core/session.h) against a scripted server (devplan/irc.md §9):
 * registration with and without CAP, SASL and its failure, NickServ, a taken nick, channels and
 * members, messages and CTCP, sending (split, paced, echoed), PING, a lost connection.
 */

#include "helpers.h"
#include "session.h"

#include <algorithm>

using irc::Error;
using irc::Event;
using irc::Login;
using irc::Text;
using test::Server;
using Lines = std::vector<std::string>;
using Kind = irc::Event::Kind;

namespace {

        const test::Seen *find(const std::vector<test::Seen> &events, Kind kind, const std::string &target = "\x01") {
                for (const test::Seen &e : events)
                        if (e.kind == kind && (target == "\x01" || e.target == target)) return &e;
                return nullptr;
        }

        irc::Config withAutojoin(irc::Config c, std::initializer_list<const char *> channels) {
                for (const char *ch : channels) {
                        irc::String s;
                        (void)s.assign(ch);
                        (void)c.autojoin.append(std::move(s));
                }
                return c;
        }

} // namespace

TEST_CASE("Registration without CAP: NICK and USER, the welcome, the joins") {
        Server s(withAutojoin(test::config("jth"), {"#bmds", "#key secret"}));
        s.connect();
        CHECK(s.take() == Lines{"CAP LS 302", "NICK jth", "USER u 0 * :Real Name"});
        CHECK(s.session.state() == irc::Session::State::Registering);
        s.say(":srv 421 * CAP :Unknown command");
        s.say(":srv 001 jth :Welcome to the network, jth");
        CHECK(s.session.state() == irc::Session::State::Registered);
        CHECK(s.session.nick() == Text("jth"));
        CHECK(s.take() == Lines{"JOIN #bmds", "JOIN #key secret"});
        auto events = s.takeEvents();
        REQUIRE(find(events, Kind::Registered) != nullptr);
        CHECK(find(events, Kind::Registered)->text == "jth");
}

TEST_CASE("CAP LS over two lines, REQ of what is wanted, SASL PLAIN, CAP END") {
        Server s(withAutojoin(test::config("jth", Login::Sasl, "secret"), {"#bmds"}));
        s.connect();
        s.take();
        s.say(":srv CAP * LS * :multi-prefix sasl=PLAIN,EXTERNAL unknown-cap");
        CHECK(s.take().empty()); // more to come
        s.say(":srv CAP * LS :server-time echo-message");
        CHECK(s.take() == Lines{"CAP REQ :server-time multi-prefix echo-message sasl"});
        s.say(":srv CAP * ACK :server-time multi-prefix echo-message sasl");
        CHECK(s.session.hasCap("echo-message"));
        CHECK(s.session.hasCap("SASL"));
        CHECK_FALSE(s.session.hasCap("unknown-cap"));
        CHECK(s.take() == Lines{"AUTHENTICATE PLAIN"});
        s.say("AUTHENTICATE +");
        CHECK(s.take() == Lines{"AUTHENTICATE AGp0aABzZWNyZXQ="});
        s.say(":srv 900 jth jth!u@host jth :You are now logged in as jth");
        s.say(":srv 903 jth :SASL authentication successful");
        CHECK(s.take() == Lines{"CAP END"});
        CHECK(s.session.account() == Text("jth"));
        auto events = s.takeEvents();
        REQUIRE(find(events, Kind::LoggedIn) != nullptr);
        s.say(":srv 001 jth :Welcome");
        CHECK(s.take() == Lines{"JOIN #bmds"}); // logged in already: no waiting
}

TEST_CASE("SASL PLAIN in 400-byte pieces, a + after an exact multiple") {
        // \0jth\0 and 295 bytes: 300 bytes, 400 in base64.
        std::string password(295, 'p');
        Server      s(test::config("jth", Login::Sasl, password.c_str()));
        s.connect();
        s.take();
        s.say(":srv CAP * LS :sasl");
        CHECK(s.take() == Lines{"CAP REQ :sasl"});
        s.say(":srv CAP * ACK :sasl");
        s.say("AUTHENTICATE +");
        Lines sent = s.take();
        REQUIRE(sent.size() == 3);
        CHECK(sent[0] == "AUTHENTICATE PLAIN");
        CHECK(sent[1].size() == std::string("AUTHENTICATE ").size() + 400);
        CHECK(sent[2] == "AUTHENTICATE +");
}

TEST_CASE("SASL refused: told, CAP END, then NickServ's IDENTIFY; the joins wait for its answer") {
        Server s(withAutojoin(test::config("jth", Login::Sasl, "secret"), {"#bmds"}));
        s.connect();
        s.take();
        s.say(":srv CAP * LS :sasl");
        s.say(":srv CAP * ACK :sasl");
        s.say("AUTHENTICATE +");
        s.take();
        s.say(":srv 904 jth :SASL authentication failed");
        CHECK(s.take() == Lines{"CAP END"});
        REQUIRE(find(s.takeEvents(), Kind::LoginFailed) != nullptr);
        s.say(":srv 001 jth :Welcome");
        CHECK(s.take() == Lines{"PRIVMSG NickServ :IDENTIFY jth secret"});
        s.wait(1000);
        CHECK(s.take().empty()); // the joins wait
        s.say(":NickServ!NickServ@services. NOTICE jth :You are now identified for jth.");
        CHECK(s.take() == Lines{"JOIN #bmds"});
        auto              events = s.takeEvents();
        const test::Seen *notice = find(events, Kind::Notice);
        REQUIRE(notice != nullptr);
        CHECK(notice->target.empty()); // the network's buffer
        CHECK((notice->flags & Event::Service) != 0);
}

TEST_CASE("NickServ that never answers: the joins after ten seconds") {
        Server s(withAutojoin(test::config("jth", Login::NickServ, "secret"), {"#bmds"}));
        s.connect();
        s.take();
        s.say(":srv 001 jth :Welcome");
        CHECK(s.take() == Lines{"PRIVMSG NickServ :IDENTIFY jth secret"});
        s.wait(9000);
        CHECK(s.take().empty());
        s.wait(1500);
        CHECK(s.take() == Lines{"JOIN #bmds"});
}

TEST_CASE("A server password: PASS first") {
        Server s(test::config("jth", Login::Password, "bouncerpass"));
        s.connect();
        CHECK(s.take() == Lines{"PASS bouncerpass", "CAP LS 302", "NICK jth", "USER u 0 * :Real Name"});
        s.say(":srv 001 jth :Welcome");
        CHECK(s.take().empty()); // no IDENTIFY for a server's password
}

TEST_CASE("A taken nick: the alternates, then _ added, then a number; ours ghosted and taken back") {
        irc::Config c = test::config("jth", Login::NickServ, "secret");
        irc::String alt;
        (void)alt.assign("jth2");
        (void)c.altNicks.append(std::move(alt));
        Server s(std::move(c));
        s.connect();
        s.take();
        s.say(":srv 433 * jth :Nickname is already in use");
        CHECK(s.take() == Lines{"NICK jth2"});
        s.say(":srv 433 * jth2 :Nickname is already in use");
        CHECK(s.take() == Lines{"NICK jth_"});
        s.say(":srv 433 * jth_ :Nickname is already in use");
        CHECK(s.take() == Lines{"NICK jth__"});
        s.say(":srv 433 * jth__ :Nickname is already in use");
        s.say(":srv 433 * jth___ :Nickname is already in use");
        Lines numbered = s.take();
        REQUIRE(numbered.size() == 2);
        CHECK(numbered[0] == "NICK jth___");
        CHECK(numbered[1].size() == std::string("NICK jth123").size());
        // Registered as jth__: our nick ghosted, IDENTIFY; NickServ's answer: the nick taken back.
        s.say(":srv 001 jth__ :Welcome");
        CHECK(s.session.nick() == Text("jth__"));
        CHECK(s.take() == Lines{"PRIVMSG NickServ :GHOST jth secret", "PRIVMSG NickServ :IDENTIFY jth secret"});
        s.say(":NickServ!NickServ@services. NOTICE jth__ :jth has been ghosted.");
        s.wait(1);
        CHECK(s.take() == Lines{"NICK jth"});
        s.say(":jth__!u@host NICK :jth");
        CHECK(s.session.nick() == Text("jth"));
        s.wait(5000);
        CHECK(s.take().empty()); // no second NICK
        // Taken once registered: told, nothing tried.
        s.say(":srv 433 jth other :Nickname is already in use");
        CHECK(s.take().empty());
}

TEST_CASE("Channels: our join, NAMES with prefixes, modes, nick changes, parts, quits, kicks") {
        Server s(test::config("jth"));
        s.registerAs("jth");
        s.say(":jth!~u@user/jth JOIN #bmds");
        const irc::Channel *c = s.session.channel("#BMDS");
        REQUIRE(c != nullptr);
        CHECK(c->joined);
        s.say(":srv 332 jth #bmds :BMDS, an OS for the ESP32-P4");
        CHECK(c->topic == Text("BMDS, an OS for the ESP32-P4"));
        s.say(":srv 353 jth = #bmds :@alice +bob");
        s.say(":srv 353 jth = #bmds :@+Carol jth");
        s.say(":srv 366 jth #bmds :End of /NAMES list.");
        REQUIRE(c->members.size() == 4);
        CHECK(c->members[0].nick == Text("alice"));
        CHECK(Text(c->members[0].prefixes) == Text("@"));
        CHECK(Text(c->members[2].prefixes) == Text("@+"));
        auto events = s.takeEvents();
        CHECK(find(events, Kind::Topic, "#bmds") != nullptr);
        CHECK(find(events, Kind::Members, "#bmds") != nullptr);

        // Modes: prefixes in rank order; a list mode's parameter skipped; one for no member.
        s.say(":alice!a@h MODE #bmds +ob-v+b bob nobody bob *!*@spam");
        CHECK(Text(c->members[1].prefixes) == Text("@"));
        events = s.takeEvents();
        const test::Seen *mode = find(events, Kind::Mode, "#bmds");
        REQUIRE(mode != nullptr);
        CHECK(mode->nick == "alice");
        CHECK(mode->text == "+ob-v+b bob nobody bob *!*@spam");
        s.say(":alice!a@h MODE #bmds -o+v bob bob");
        CHECK(Text(c->members[1].prefixes) == Text("+"));

        // A nick change: in the channel, and for a private chat with them.
        s.say(":bob!b@h NICK :bobby");
        CHECK(c->members[1].nick == Text("bobby"));
        events = s.takeEvents();
        CHECK(find(events, Kind::Nick, "#bmds") != nullptr);
        const test::Seen *query = find(events, Kind::Nick, "bob");
        REQUIRE(query != nullptr);
        CHECK((query->flags & Event::Query) != 0);
        CHECK(query->text == "bobby");

        // Away (away-notify), then back.
        s.say(":carol!c@h AWAY :lunch");
        CHECK(c->members[2].away);
        s.say(":carol!c@h AWAY");
        CHECK_FALSE(c->members[2].away);

        s.say(":alice!a@h PART #bmds :bye");
        CHECK(c->members.size() == 3);
        s.say(":bobby!b@h QUIT :Quit: gone");
        CHECK(c->members.size() == 2);
        events = s.takeEvents();
        CHECK(find(events, Kind::Part, "#bmds")->text == "bye");
        CHECK(find(events, Kind::Quit, "#bmds") != nullptr);
        CHECK(find(events, Kind::Quit, "bobby") != nullptr);

        // Someone joining (extended-join), and us kicked: the channel gone.
        s.say(":dave!d@h JOIN #bmds dave_acct :Dave Real");
        events = s.takeEvents();
        const test::Seen *join = find(events, Kind::Join, "#bmds");
        REQUIRE(join != nullptr);
        CHECK(join->extra == "dave_acct");
        CHECK(join->text == "Dave Real");
        CHECK(c->members.size() == 3);
        s.say(":carol!c@h KICK #bmds jth :out");
        CHECK(s.session.channel("#bmds") == nullptr);
        events = s.takeEvents();
        const test::Seen *kick = find(events, Kind::Kick, "#bmds");
        REQUIRE(kick != nullptr);
        CHECK((kick->flags & Event::Self) != 0);
        CHECK(kick->extra == "carol");
}

TEST_CASE("Messages: a channel's, a private one, a /me, CTCP answered, notices, server-time") {
        Server s(test::config("jth"));
        s.registerAs("jth");
        s.say(":jth!u@h JOIN #bmds");
        s.takeEvents();

        s.say("@time=2026-11-16T10:41:00.000Z :alice!a@h PRIVMSG #bmds :did the TLS land?");
        auto              events = s.takeEvents();
        const test::Seen *m = find(events, Kind::Message, "#bmds");
        REQUIRE(m != nullptr);
        CHECK(m->nick == "alice");
        CHECK(m->text == "did the TLS land?");
        CHECK(m->time == 1794825660000);

        s.say(":alice!a@h PRIVMSG jth :psst");
        events = s.takeEvents();
        m = find(events, Kind::Message, "alice");
        REQUIRE(m != nullptr);
        CHECK((m->flags & Event::Query) != 0);
        CHECK(m->time == s.now.wall); // no server-time: ours

        s.say(":alice!a@h PRIVMSG #bmds :\x01"
              "ACTION waves\x01");
        events = s.takeEvents();
        m = find(events, Kind::Message, "#bmds");
        REQUIRE(m != nullptr);
        CHECK((m->flags & Event::Action) != 0);
        CHECK(m->text == "waves");

        s.say(":alice!a@h PRIVMSG jth :\x01VERSION\x01");
        CHECK(s.take() == Lines{"NOTICE alice :\x01VERSION BMDS IRC\x01"});
        s.say(":alice!a@h PRIVMSG jth :\x01PING 12345\x01");
        CHECK(s.take() == Lines{"NOTICE alice :\x01PING 12345\x01"});
        s.say(":alice!a@h PRIVMSG jth :\x01"
              "FINGER\x01");
        CHECK(s.take().empty());
        CHECK(s.takeEvents().empty());

        // To the ops only (STATUSMSG): the channel's buffer.
        s.say(":alice!a@h NOTICE @#bmds :ops only");
        events = s.takeEvents();
        CHECK(find(events, Kind::Notice, "#bmds") != nullptr);
        s.say(":irc.example.net NOTICE jth :*** server says");
        events = s.takeEvents();
        REQUIRE(find(events, Kind::Notice, "") != nullptr);
        CHECK((find(events, Kind::Notice, "")->flags & Event::Server) != 0);
        s.say(":alice!a@h NOTICE jth :a notice to me");
        events = s.takeEvents();
        REQUIRE(find(events, Kind::Notice, "") != nullptr);
        CHECK(find(events, Kind::Notice, "")->flags == 0);

        // Latin-1 read as such.
        s.say(":alice!a@h PRIVMSG #bmds :caf\xe9");
        CHECK(find(s.takeEvents(), Kind::Message, "#bmds")->text == "caf\xc3\xa9");

        s.say(":alice!a@h INVITE jth #secret");
        CHECK(find(s.takeEvents(), Kind::Invite)->text == "#secret");
}

TEST_CASE("Sending: shown at once without echo-message, by the server's echo with it") {
        Server s(test::config("jth"));
        s.registerAs("jth");
        REQUIRE(s.session.privmsg("#bmds", "hello", s.now) == Error::Ok);
        REQUIRE(s.session.action("alice", "waves", s.now) == Error::Ok);
        s.flush();
        CHECK(s.take() == Lines{"PRIVMSG #bmds :hello", "PRIVMSG alice :\x01"
                                                        "ACTION waves\x01"});
        auto              events = s.takeEvents();
        const test::Seen *mine = find(events, Kind::Message, "#bmds");
        REQUIRE(mine != nullptr);
        CHECK((mine->flags & Event::Self) != 0);
        const test::Seen *act = find(events, Kind::Message, "alice");
        REQUIRE(act != nullptr);
        CHECK((act->flags & (Event::Self | Event::Action | Event::Query)) ==
              (Event::Self | Event::Action | Event::Query));

        Server e(test::config("jth"));
        e.connect();
        e.say(":srv CAP * LS :echo-message");
        e.say(":srv CAP * ACK :echo-message");
        e.say(":srv 001 jth :Welcome");
        e.take();
        e.takeEvents();
        REQUIRE(e.session.privmsg("alice", "hi", e.now) == Error::Ok);
        e.flush();
        CHECK(e.take() == Lines{"PRIVMSG alice :hi"});
        CHECK(e.takeEvents().empty()); // the echo shows it
        e.say(":jth!u@h PRIVMSG alice :hi");
        events = e.takeEvents();
        const test::Seen *echo = find(events, Kind::Message, "alice"); // for whom it was
        REQUIRE(echo != nullptr);
        CHECK((echo->flags & Event::Self) != 0);
}

TEST_CASE("Sending: a long message split at words, each line within 512 bytes") {
        Server s(test::config("jth"));
        s.registerAs("jth");
        std::string text;
        for (int i = 0; i < 150; ++i) text += "word" + std::to_string(i) + " ";
        text.pop_back();
        REQUIRE(s.session.privmsg("#bmds", test::text(text), s.now) == Error::Ok);
        s.wait(10000);
        Lines       sent = s.take();
        std::string joined;
        REQUIRE(sent.size() >= 2);
        for (const std::string &l : sent) {
                REQUIRE(l.rfind("PRIVMSG #bmds :", 0) == 0);
                // Room for the server's ":nick!user@host " too (a 63-byte host assumed).
                CHECK(l.size() + 2 + 1 + 3 + 1 + 10 + 1 + 63 + 1 <= 512);
                if (!joined.empty()) joined += ' ';
                joined += l.substr(std::string("PRIVMSG #bmds :").size());
        }
        CHECK(joined == text);
}

TEST_CASE("Sending: refused with a line break, a bad target, nothing to say") {
        Server s(test::config("jth"));
        s.registerAs("jth");
        CHECK(s.session.privmsg("#bmds", "hi\r\nQUIT :x", s.now) == Error::Malformed);
        CHECK(s.session.privmsg("#a b", "hi", s.now) == Error::Malformed);
        CHECK(s.session.privmsg("", "hi", s.now) == Error::Malformed);
        CHECK(s.session.privmsg("#bmds", "", s.now) == Error::Invalid);
        CHECK(s.session.join("#a,#b") == Error::Malformed);
        CHECK(s.session.raw("PRIVMSG #a :x\nQUIT") == Error::Malformed);
        CHECK(s.session.raw("") == Error::Malformed);
        s.flush();
        CHECK(s.take().empty());
        REQUIRE(s.session.raw("WHOIS alice") == Error::Ok);
        REQUIRE(s.session.join("#key", "pw") == Error::Ok);
        REQUIRE(s.session.part("#key", "later") == Error::Ok);
        REQUIRE(s.session.setTopic("#bmds", "a topic") == Error::Ok);
        REQUIRE(s.session.setAway("lunch") == Error::Ok);
        REQUIRE(s.session.setAway("") == Error::Ok);
        s.wait(5000);
        CHECK(s.take() ==
              Lines{"WHOIS alice", "JOIN #key pw", "PART #key :later", "TOPIC #bmds :a topic", "AWAY :lunch", "AWAY"});
}

TEST_CASE("Pacing: five at once, then one a second; PONG first, unpaced") {
        Server s(test::config("jth"));
        s.registerAs("jth");
        for (int i = 0; i < 10; ++i)
                REQUIRE(s.session.privmsg("#bmds", test::text(std::to_string(i)), s.now) == Error::Ok);
        s.flush();
        CHECK(s.take().size() == 5);
        CHECK(s.session.sendWaitMs(s.now.mono) == 1000);
        s.say("PING :irc.example.net");
        CHECK(s.take() == Lines{"PONG :irc.example.net"});
        s.wait(999);
        CHECK(s.take().empty());
        s.wait(1);
        CHECK(s.take() == Lines{"PRIVMSG #bmds :5"});
        s.wait(4000);
        CHECK(s.take().size() == 4);
        CHECK(s.session.sendWaitMs(s.now.mono) == -1);
}

TEST_CASE("Keeping it up: our PING after 90 s quiet, the lag; given up after 210 s") {
        Server s(test::config("jth"));
        s.registerAs("jth");
        CHECK(s.session.tickWaitMs(s.now.mono) == irc::Session::PingAfterMs);
        s.wait(89000);
        CHECK(s.take().empty());
        s.wait(1000);
        CHECK(s.take() == Lines{"PING :bmds-irc"});
        s.wait(42);
        s.say(":srv PONG srv :bmds-irc");
        CHECK(s.session.lagMs() == 42);
        const auto        events = s.takeEvents();
        const test::Seen *lag = find(events, Kind::Lag);
        REQUIRE(lag != nullptr);
        CHECK(lag->number == 42);
        // Quiet again: a PING at 90 s, nothing back; given up at 210 s, once.
        s.wait(90000);
        CHECK(s.take() == Lines{"PING :bmds-irc"});
        s.wait(119000);
        CHECK(find(s.takeEvents(), Kind::Timeout) == nullptr);
        s.wait(1000);
        CHECK(find(s.takeEvents(), Kind::Timeout) != nullptr);
        s.wait(60000);
        CHECK(find(s.takeEvents(), Kind::Timeout) == nullptr);
}

TEST_CASE("A lost connection: typed lines kept, the channels joined again") {
        Server s(test::config("jth"));
        s.registerAs("jth");
        s.say(":jth!u@h JOIN #bmds");
        s.say(":jth!u@h JOIN #c");
        s.say(":jth!u@h PART #c");
        s.session.disconnected();
        CHECK(s.session.state() == irc::Session::State::Disconnected);
        CHECK_FALSE(s.session.channel("#bmds")->joined);
        REQUIRE(s.session.privmsg("#bmds", "typed while away", s.now) == Error::Ok);
        s.flush();
        CHECK(s.take().empty());
        s.connect();
        CHECK(s.take() == Lines{"CAP LS 302", "NICK jth", "USER u 0 * :Real Name"});
        s.say(":srv 001 jth :Welcome");
        // The typed line was queued first: it goes first, then the rejoin (#c was left).
        CHECK(s.take() == Lines{"PRIVMSG #bmds :typed while away", "JOIN #bmds"});
}

TEST_CASE("ERROR ends it; reconnecting waits 2, 4 ... 300 s, a fifth either way") {
        Server s(test::config("jth"));
        s.registerAs("jth");
        s.say("ERROR :Closing Link: (Ping timeout)");
        CHECK(find(s.takeEvents(), Kind::Error)->text == "Closing Link: (Ping timeout)");
        for (unsigned attempt = 0; attempt < 12; ++attempt)
                for (uint32_t r : {0u, 20u, 40u, 12345u}) {
                        const int64_t base = attempt >= 8 ? 300000 : std::min<int64_t>(300000, 2000LL << attempt);
                        const int64_t d = irc::reconnectDelayMs(attempt, r);
                        CHECK(d >= base * 80 / 100);
                        CHECK(d <= base * 120 / 100);
                }
        CHECK(irc::reconnectDelayMs(0, 20) == 2000);
}
