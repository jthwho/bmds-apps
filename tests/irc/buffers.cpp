/**
 * @file      buffers.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * One network's buffers (irc/core/buffers.h): events routed, lines made for the view and the
 * log, counts and attention, private chats opened and renamed, passwords hidden; and the
 * formatting they use (local times, nick colours, hiding).
 */

#include "buffers.h"
#include "format.h"
#include "helpers.h"
#include "ignore.h"

using irc::Buffer;
using irc::Error;
using irc::Text;
using test::Server;

namespace {

        // A session registered as jth, its events going through buffers.
        struct World {
                        irc::Buffers buffers;
                        Server       s{test::config("jth")};
                        World() {
                                s.buffers = &buffers;
                                s.registerAs("jth");
                                s.takeLines();
                        }
                        Buffer *find(const char *name) { return buffers.find(name, irc::CaseMapping::Rfc1459); }
        };

        std::string colour(const char *nick) {
                const uint8_t c = irc::Format::nickColour(nick, irc::CaseMapping::Rfc1459);
                return std::string("\x03") + char('0' + c / 10) + char('0' + c % 10) + nick + "\x03";
        }

} // namespace

TEST_CASE("Format: local times, nick colours, a password to NickServ hidden") {
        using irc::Format::localTime;
        irc::Format::LocalTime t = localTime(1763289660000, 0); // 2025-11-16 10:41:00 UTC
        CHECK(t.year == 2025);
        CHECK(t.month == 11);
        CHECK(t.day == 16);
        CHECK(t.hour == 10);
        CHECK(t.minute == 41);
        t = localTime(1763289660000, -480); // PST: the same day, 02:41
        CHECK(t.hour == 2);
        t = localTime(1763289660000 - 11 * 3600000LL, -480); // the day before
        CHECK(t.day == 15);
        CHECK(t.hour == 15);
        t = localTime(0, -60); // before 1970
        CHECK(t.year == 1969);
        CHECK(t.hour == 23);

        using irc::CaseMapping;
        CHECK(irc::Format::nickColour("alice", CaseMapping::Rfc1459) ==
              irc::Format::nickColour("ALICE", CaseMapping::Rfc1459));
        CHECK(irc::Format::nickColour("a[b]", CaseMapping::Rfc1459) ==
              irc::Format::nickColour("a{b}", CaseMapping::Rfc1459));
        for (const char *n : {"a", "b", "jth", "someone", "x"}) {
                const uint8_t c = irc::Format::nickColour(n, CaseMapping::Rfc1459);
                CHECK(c >= 2);
                CHECK(c <= 13);
                CHECK(c != 8);  // yellow
                CHECK(c != 0);  // white
                CHECK(c != 11); // light cyan
        }

        const auto hidden = [](Text target, Text text) {
                irc::String out;
                return irc::Format::hideSecret(target, text, out) ? test::str(out) : std::string("(as it is)");
        };
        CHECK(hidden("NickServ", "IDENTIFY secret") == "IDENTIFY ****");
        CHECK(hidden("nickserv", "identify jth secret") == "identify jth ****");
        CHECK(hidden("NickServ", "REGISTER secret me@example.com") == "REGISTER **** me@example.com");
        CHECK(hidden("NickServ", "GHOST jth secret") == "GHOST jth ****");
        CHECK(hidden("NickServ", "SET PASSWORD newone") == "SET PASSWORD ****");
        CHECK(hidden("NickServ", "INFO jth") == "(as it is)");
        CHECK(hidden("NickServ", "IDENTIFY") == "(as it is)");
        CHECK(hidden("alice", "IDENTIFY secret") == "(as it is)");
}

TEST_CASE("Format::registration: NickServ's answers to REGISTER and VERIFY") {
        using R = irc::Format::Registration;
        using irc::Format::registration;
        CHECK(registration("jth is now registered to jth@example.com, with the password secret.") == R::Registered);
        CHECK(registration("Nickname jth registered.") == R::Registered);
        CHECK(registration("jth has now been verified.") == R::Registered);
        CHECK(registration("An email containing nickname activation instructions has been sent to jth@example.com. "
                           "If you do not receive it, /msg NickServ VERIFY REGISTER jth <code>") == R::Verify);
        CHECK(registration("\x02jth\x02 is already registered.") == R::Refused);
        CHECK(registration("Invalid email address.") == R::Refused);
        CHECK(registration("You must have been using this nick for at least 30 seconds to register.") == R::Refused);
        CHECK(registration("You are now identified for jth.") == R::None);
        CHECK(registration("") == R::None);
}

TEST_CASE("Buffers: a channel's messages, its view's and log's lines, counts and attention") {
        World w;
        w.s.say(":jth!u@h JOIN #bmds");
        Buffer *bmds = w.find("#bmds");
        REQUIRE(bmds != nullptr);
        CHECK(bmds->type == Buffer::Type::Channel);
        CHECK(bmds->joined);
        auto lines = w.s.takeLines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].view == "10:41 -->\x0f\tYou joined #bmds");
        CHECK(lines[0].level == 0);
        CHECK(bmds->unread == 0); // not a message

        w.s.say(":alice!a@h PRIVMSG #bmds :hello there");
        lines = w.s.takeLines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].buffer == bmds->id);
        CHECK(lines[0].view == "10:41 <" + colour("alice") + ">\x0f\thello there");
        CHECK(lines[0].log == "10:41:00 <alice> hello there");
        CHECK(lines[0].level == 1);
        CHECK_FALSE(lines[0].notify);
        CHECK(bmds->unread == 1);
        CHECK(bmds->attention == 1);

        // A mention: reversed, counted, an alert, told.
        w.s.say(":bob!b@h PRIVMSG #bmds :jth: did the TLS land?");
        lines = w.s.takeLines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].view == "10:41 \x16<" + colour("bob") + ">\x16\x0f\tjth: did the TLS land?");
        CHECK(lines[0].level == 2);
        CHECK(lines[0].notify);
        CHECK(bmds->unread == 2);
        CHECK(bmds->mentions == 1);
        CHECK(bmds->attention == 2);
        CHECK(bmds->speakers.size() == 2);
        CHECK(bmds->speakers[0] == Text("bob")); // last first

        // A highlight word too.
        (void)w.buffers.settings().highlights.append([] {
                irc::String s;
                (void)s.assign("bmds");
                return s;
        }());
        w.s.say(":carol!c@h PRIVMSG #bmds :BMDS rocks");
        CHECK(w.s.takeLines()[0].level == 2);

        // In view in a focused window: read, nothing counted or told; unfocused: counted again.
        w.buffers.setActive(bmds->id, true);
        CHECK(bmds->unread == 0);
        CHECK(bmds->attention == 0);
        w.s.say(":bob!b@h PRIVMSG #bmds :jth: again");
        lines = w.s.takeLines();
        CHECK_FALSE(lines[0].notify);
        CHECK(bmds->unread == 0);
        w.buffers.setActive(bmds->id, false);
        w.s.say(":bob!b@h PRIVMSG #bmds :jth: and again");
        CHECK(w.s.takeLines()[0].notify);
        CHECK(bmds->mentions == 1);

        // /me, our own message (shown, not counted), a notice to the channel.
        w.s.say(":alice!a@h PRIVMSG #bmds :\x01"
                "ACTION waves\x01");
        CHECK(w.s.takeLines()[0].view == "10:41 *\x0f\t" + colour("alice") + " waves");
        REQUIRE(w.s.session.privmsg("#bmds", "mine", w.s.now) == Error::Ok);
        w.s.flush();
        lines = w.s.takeLines();
        CHECK(lines[0].view == "10:41 <jth>\x0f\tmine");
        CHECK(lines[0].log == "10:41:00 <jth> mine");
        CHECK(bmds->unread == 2); // alice's /me counted, ours not
        w.s.say(":alice!a@h NOTICE #bmds :a notice");
        CHECK(w.s.takeLines()[0].view == "10:41 -alice-\x0f\ta notice");
}

TEST_CASE("Buffers: private chats opened, alerts, renamed with the nick; notices routed") {
        World w;
        w.s.say(":jth!u@h JOIN #bmds");
        w.s.takeLines();
        w.s.say(":alice!a@h PRIVMSG jth :psst");
        Buffer *alice = w.find("alice");
        REQUIRE(alice != nullptr);
        CHECK(alice->type == Buffer::Type::Query);
        auto lines = w.s.takeLines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].buffer == alice->id);
        CHECK(lines[0].level == 2); // every private message an alert
        CHECK(lines[0].notify);
        const uint32_t id = alice->id;

        // Their nick change: the chat follows them.
        w.s.say(":alice!a@h NICK :alicia");
        CHECK(w.find("alice") == nullptr);
        REQUIRE(w.find("alicia") != nullptr);
        CHECK(w.find("alicia")->id == id);
        lines = w.s.takeLines();
        bool renamed = false;
        for (const auto &l : lines)
                if (l.buffer == id && l.view.find("is now known as") != std::string::npos) renamed = true;
        CHECK(renamed);
        // Their quit, shown in the chat.
        w.s.say(":alicia!a@h QUIT :bye");
        lines = w.s.takeLines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].buffer == id);
        CHECK(lines[0].view.find("quit (bye)") != std::string::npos);

        // Notices: a service's to the network's buffer, a person's to the one in view.
        w.s.say(":NickServ!N@services. NOTICE jth :You are now identified for jth.");
        lines = w.s.takeLines();
        CHECK(lines[0].buffer == w.buffers.network().id);
        CHECK(lines[0].level == 1); // naming us: not a mention, from a service
        w.buffers.setActive(w.find("#bmds")->id, true);
        w.s.say(":bob!b@h NOTICE jth :psst, a notice");
        lines = w.s.takeLines();
        CHECK(lines[0].buffer == w.find("#bmds")->id);
        CHECK(lines[0].level == 2);
        w.buffers.setActive(0, true); // another network's in view
        w.s.say(":bob!b@h NOTICE jth :another");
        CHECK(w.s.takeLines()[0].buffer == w.buffers.network().id);
}

TEST_CASE("Buffers: joins shown or not, parts and kicks, the topic, server lines, a password hidden") {
        World w;
        w.s.say(":jth!u@h JOIN #bmds");
        w.s.say(":dave!d@h JOIN #bmds dave_acct :Dave");
        auto lines = w.s.takeLines();
        REQUIRE(lines.size() == 2);
        CHECK(lines[1].view == "10:41 -->\x0f\t" + colour("dave") + " (dave_acct) joined");
        CHECK(lines[1].log == "10:41:00 --> dave (dave_acct) joined");
        w.buffers.settings().showJoins = false;
        w.s.say(":erin!e@h JOIN #bmds");
        w.s.say(":erin!e@h PART #bmds :later");
        CHECK(w.s.takeLines().empty());
        w.buffers.settings().showJoins = true;
        w.s.say(":dave!d@h PART #bmds :later");
        CHECK(w.s.takeLines()[0].view == "10:41 <--\x0f\t" + colour("dave") + " left (later)");

        w.s.say(":srv 332 jth #bmds :BMDS on the P4");
        CHECK(w.s.takeLines()[0].view == "10:41 --\x0f\tTopic: BMDS on the P4");
        // The server's notice: "--", not its name (which would widen every line's gutter).
        w.s.say(":tungsten.libera.chat NOTICE jth :*** Checking Ident");
        lines = w.s.takeLines();
        CHECK(lines[0].buffer == w.buffers.network().id);
        CHECK(lines[0].view == "10:41 --\x0f\t*** Checking Ident");
        w.s.say(":srv 372 jth :- the message of the day");
        lines = w.s.takeLines();
        CHECK(lines[0].buffer == w.buffers.network().id);
        CHECK(lines[0].view == "10:41 --\x0f\t- the message of the day");

        // Our message to NickServ: its password hidden in both lines.
        REQUIRE(w.s.session.privmsg("NickServ", "IDENTIFY jth secret", w.s.now) == Error::Ok);
        w.s.flush();
        lines = w.s.takeLines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].view.find("secret") == std::string::npos);
        CHECK(lines[0].log == "10:41:00 <jth> IDENTIFY jth ****");

        // A service repeating a password it was given: hidden too (Atheme's REGISTER answer).
        irc::String secret;
        (void)secret.assign("hunter22");
        (void)w.buffers.settings().secrets.append(std::move(secret));
        w.s.say(":NickServ!N@services. NOTICE jth :\x02jth\x02 is now registered to \x02"
                "a@b.c\x02, with the password "
                "\x02hunter22\x02.");
        lines = w.s.takeLines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].view.find("hunter22") == std::string::npos);
        CHECK(lines[0].log == "10:41:00 -NickServ- jth is now registered to a@b.c, with the password ****.");
        w.s.say(":alice!a@h PRIVMSG #bmds :hunter22 is my word"); // a person's: as they said it
        CHECK(w.s.takeLines()[0].log.find("hunter22") != std::string::npos);

        // Kicked: the channel's buffer stays, not joined.
        w.s.say(":carol!c@h KICK #bmds jth :out");
        lines = w.s.takeLines();
        REQUIRE(w.find("#bmds") != nullptr);
        CHECK_FALSE(w.find("#bmds")->joined);
        CHECK(lines[0].view == "10:41 <--\x0f\tjth was kicked by carol (out)");

        // The zone the times are shown in; a line of the client's own.
        w.buffers.settings().utcOffsetMinutes = -480;
        irc::Vector<irc::Line> out;
        REQUIRE(w.buffers.note(w.find("#bmds")->id, "a note", w.s.now.wall, out) == Error::Ok);
        REQUIRE(out.size() == 1);
        CHECK(test::str(out[0].view) == "02:41 --\x0f\ta note");
        // Closing; the network's buffer can't be.
        const uint32_t id = w.find("#bmds")->id;
        w.buffers.close(id);
        CHECK(w.buffers.byId(id) == nullptr);
        w.buffers.close(w.buffers.network().id);
        CHECK(w.buffers.count() >= 1);
        CHECK(w.buffers.at(0).type == Buffer::Type::Network);
}

TEST_CASE("Buffers: watched people, CTCP answers, the ignored count, a refused list, ban lists, invites") {
        World w;
        REQUIRE(w.s.session.watch("alice") == Error::Ok);
        w.s.say(":srv 005 jth MONITOR=10 :are supported");
        w.s.say(":srv 376 jth :End of /MOTD");
        w.s.takeLines();
        Buffer *chat = w.buffers.open(Buffer::Type::Query, "alice", irc::CaseMapping::Rfc1459);
        REQUIRE(chat != nullptr);
        const uint32_t chatId = chat->id, network = w.buffers.network().id;
        w.s.say(":srv 730 jth :alice!a@alice.example");
        auto lines = w.s.takeLines();
        REQUIRE(lines.size() == 2); // their chat, and the network's
        CHECK(lines[0].buffer == chatId);
        CHECK(lines[1].buffer == network);
        CHECK(lines[1].view.find(colour("alice") + " is online (a@alice.example)") != std::string::npos);

        // An answer goes where the user is; a PING's with its delay.
        w.buffers.setActive(chatId, true);
        w.s.say(":alice!a@alice.example NOTICE jth :\x01VERSION mIRC v7\x01");
        lines = w.s.takeLines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].buffer == chatId);
        CHECK(lines[0].view.find("'s VERSION: mIRC v7") != std::string::npos);

        irc::Ignores ignores;
        REQUIRE(ignores.read("spam") == Error::Ok);
        w.s.session.setIgnores(&ignores, "TestNet");
        w.s.say(":spam!s@x PRIVMSG jth :buy");
        CHECK(w.s.takeLines().empty());
        w.s.wait(irc::Session::IgnoredEveryMs);
        lines = w.s.takeLines();
        REQUIRE(!lines.empty());
        CHECK(lines.back().view.find("1 message from people ignored") != std::string::npos);

        w.s.say(":srv 322 jth #a 3 :topic");
        w.s.say(":srv 323 jth :End");
        CHECK(w.s.takeLines().empty()); // the list is the dialog's
        w.s.say(":srv 263 jth LIST :Please wait a while");
        lines = w.s.takeLines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].view.find("The channel list: Please wait a while") != std::string::npos);

        w.s.say(":jth!u@h JOIN #bmds");
        w.s.takeLines();
        w.s.say(":srv 367 jth #bmds *!*@spam alice 1763289600");
        lines = w.s.takeLines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].buffer == w.find("#bmds")->id);
        CHECK(lines[0].view.find("#bmds ban: *!*@spam (by alice)") != std::string::npos);

        w.s.say(":bob!b@h INVITE jth #fun");
        lines = w.s.takeLines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].view.find(" invited you to #fun") != std::string::npos);
        CHECK(lines[0].view.find("(joining)") == std::string::npos); // not set to join
}
