/**
 * @file      commands.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "commands.h"

namespace irc {

        const char *const CommandNames[] = {"away", "back",  "clear", "close", "cs",    "help",  "invite", "join",
                                            "kick", "me",    "mode",  "msg",   "names", "nick",  "notice", "ns",
                                            "part", "query", "quit",  "quote", "raw",   "topic", "whois"};
        const size_t      CommandCount = sizeof(CommandNames) / sizeof(CommandNames[0]);

        namespace {

                // The first word of @p text, and what follows it (its spaces skipped).
                Text firstWord(Text text, Text &rest) {
                        size_t at = 0;
                        while (at < text.size() && text[at] == ' ') ++at;
                        size_t end = text.find(' ', at);
                        if (end == Text::NoPos) end = text.size();
                        const Text word = text.mid(at, end - at);
                        size_t     next = end;
                        while (next < text.size() && text[next] == ' ') ++next;
                        rest = text.mid(next);
                        return word;
                }

                struct Run {
                                Session      &session;
                                Buffers      &buffers;
                                uint32_t      current;
                                const Now    &now;
                                Vector<Line> &lines;
                                Outcome      &outcome;

                                const Buffer *here() const { return buffers.byId(current); }

                                // The channel or person in view, or empty in the network's buffer.
                                Text target() const {
                                        const Buffer *b = here();
                                        return b != nullptr && b->type != Buffer::Type::Network ? b->name.view()
                                                                                                : Text();
                                }

                                Error tell(Text text) { return buffers.note(current, text, now.wall, lines); }

                                Error tell2(Text a, Text b) {
                                        String t;
                                        (void)t.assign(a);
                                        (void)t.append(b);
                                        return tell(t);
                                }

                                // A session's refusal told.
                                Error check(Error err, Text what) {
                                        if (err == Error::Ok || err == Error::NoMem) return err;
                                        return tell2(what, err == Error::TooLong ? Text(": too long")
                                                           : err == Error::Invalid
                                                                   ? Text(": nothing to send")
                                                                   : Text(": not something that can be sent"));
                                }

                                Error raw(Text a, Text b = Text(), Text c = Text(), Text d = Text()) {
                                        String     line;
                                        const Text parts[] = {a, b, c, d};
                                        for (Text part : parts) {
                                                if (part.isEmpty()) continue;
                                                if (!line.isEmpty()) (void)line.append(' ');
                                                (void)line.append(part);
                                        }
                                        return check(session.raw(line), "That command");
                                }

                                Error say(Text text, bool action) {
                                        const Text to = target();
                                        if (to.isEmpty())
                                                return tell(
                                                        "Nothing to say it to here: /join a channel or /msg someone");
                                        if (text.isEmpty()) return Error::Ok;
                                        return check(action ? session.action(to, text, now)
                                                            : session.privmsg(to, text, now),
                                                     "The message");
                                }

                                // A channel's name as typed, # added if it has no channel prefix.
                                Error withPrefix(Text name, String &out) {
                                        if (session.isupport().isChannel(name)) return out.assign(name);
                                        if (out.assign("#") != Error::Ok) return Error::NoMem;
                                        return out.append(name);
                                }
                };

        } // namespace

        Error runCommand(Text typed, Session &session, Buffers &buffers, uint32_t current, const Now &now,
                         Vector<Line> &lines, Outcome &outcome) {
                outcome = Outcome();
                Run r{session, buffers, current, now, lines, outcome};
                if (typed.isEmpty()) return Error::Ok;
                if (typed[0] != '/') return r.say(typed, false);
                if (typed.startsWith("//")) return r.say(typed.mid(1), false);

                Text       args;
                const Text cmd = firstWord(typed.mid(1), args);
                Text       rest;
                const Text first = firstWord(args, rest);
                const auto is = [&](const char *name) {
                        return cmd.equalsIgnoringCase(name);
                };

                if (is("me")) return r.say(args, true);
                if (is("msg") || is("m") || is("notice")) {
                        if (first.isEmpty() || rest.isEmpty())
                                return r.tell2("Usage: /",
                                               is("notice") ? Text("notice target text") : Text("msg target text"));
                        return r.check(is("notice") ? session.notice(first, rest, now)
                                                    : session.privmsg(first, rest, now),
                                       "The message");
                }
                if (is("ns") || is("cs")) {
                        if (args.isEmpty())
                                return r.tell2("Usage: /", is("ns") ? Text("ns command") : Text("cs command"));
                        return r.check(session.privmsg(is("ns") ? Text("NickServ") : Text("ChanServ"), args, now),
                                       "The message");
                }
                if (is("query") || is("q")) {
                        if (first.isEmpty() || session.isupport().isChannel(first))
                                return r.tell("Usage: /query nick [text]");
                        Buffer *b = buffers.open(Buffer::Type::Query, first, session.isupport().caseMapping());
                        if (b == nullptr) return Error::NoMem;
                        outcome.action = Outcome::Action::Show;
                        outcome.buffer = b->id;
                        if (!rest.isEmpty()) return r.check(session.privmsg(first, rest, now), "The message");
                        return Error::Ok;
                }
                if (is("join") || is("j")) {
                        if (first.isEmpty()) return r.tell("Usage: /join #channel [key]");
                        // #a,#b: each joined (their keys, if any, in the same order).
                        Text names = first, keys = rest;
                        while (!names.isEmpty()) {
                                size_t comma = names.find(',');
                                if (comma == Text::NoPos) comma = names.size();
                                size_t keyComma = keys.find(',');
                                if (keyComma == Text::NoPos) keyComma = keys.size();
                                String name;
                                if (r.withPrefix(names.left(comma), name) != Error::Ok) return Error::NoMem;
                                const Error err = r.check(session.join(name, keys.left(keyComma)), "The join");
                                if (err != Error::Ok) return err;
                                names = names.mid(comma + 1);
                                keys = keys.mid(keyComma + 1);
                        }
                        return Error::Ok;
                }
                if (is("part") || is("leave")) {
                        Text channel = r.target(), reason = args;
                        if (session.isupport().isChannel(first)) {
                                channel = first;
                                reason = rest;
                        }
                        if (!session.isupport().isChannel(channel)) return r.tell("Usage: /part [#channel] [reason]");
                        return r.check(session.part(channel, reason), "The part");
                }
                if (is("close")) {
                        const Buffer *b = r.here();
                        if (b == nullptr || b->type == Buffer::Type::Network)
                                return r.tell("The network's own buffer stays: /quit to disconnect");
                        if (b->type == Buffer::Type::Channel && b->joined) {
                                const Error err = r.check(session.part(b->name, args), "The part");
                                if (err != Error::Ok) return err;
                        }
                        outcome.action = Outcome::Action::Close;
                        outcome.buffer = b->id;
                        return Error::Ok;
                }
                if (is("nick")) {
                        if (first.isEmpty()) return r.tell("Usage: /nick newnick");
                        return r.check(session.changeNick(first), "The nick");
                }
                if (is("topic")) {
                        Text channel = r.target(), topic = args;
                        if (session.isupport().isChannel(first)) {
                                channel = first;
                                topic = rest;
                        }
                        if (!session.isupport().isChannel(channel))
                                return r.tell("Usage: /topic [#channel] [new topic]");
                        if (topic.isEmpty()) {
                                const Channel *c = session.channel(channel);
                                return r.tell2("Topic: ",
                                               c != nullptr && !c->topic.isEmpty() ? c->topic.view() : Text("(none)"));
                        }
                        return r.check(session.setTopic(channel, topic), "The topic");
                }
                if (is("away")) return r.check(session.setAway(args.isEmpty() ? Text("Away") : args), "Away");
                if (is("back")) return r.check(session.setAway(""), "Back");
                if (is("whois")) {
                        if (first.isEmpty()) return r.tell("Usage: /whois nick");
                        return r.raw("WHOIS", first);
                }
                if (is("names")) {
                        const Text channel = first.isEmpty() ? r.target() : first;
                        if (!session.isupport().isChannel(channel)) return r.tell("Usage: /names [#channel]");
                        return r.raw("NAMES", channel);
                }
                if (is("mode")) {
                        // +o nick: the channel in view's; #chan +o nick, or nick +i.
                        if (first.startsWith("+") || first.startsWith("-")) {
                                if (r.target().isEmpty()) return r.tell("Usage: /mode [target] modes");
                                return r.raw("MODE", r.target(), args);
                        }
                        if (first.isEmpty()) return r.tell("Usage: /mode [target] modes");
                        return r.raw("MODE", first, rest);
                }
                if (is("kick")) {
                        const Text channel = r.target();
                        if (first.isEmpty() || !session.isupport().isChannel(channel))
                                return r.tell("Usage: /kick nick [reason] (in a channel)");
                        String reason;
                        if (!rest.isEmpty()) {
                                (void)reason.assign(":");
                                (void)reason.append(rest);
                        }
                        return r.raw("KICK", channel, first, reason);
                }
                if (is("invite")) {
                        const Text channel = rest.isEmpty() ? r.target() : firstWord(rest, rest);
                        if (first.isEmpty() || !session.isupport().isChannel(channel))
                                return r.tell("Usage: /invite nick [#channel]");
                        return r.raw("INVITE", first, channel);
                }
                if (is("quit")) {
                        outcome.action = Outcome::Action::Quit;
                        return r.check(session.quit(args), "The quit");
                }
                if (is("raw") || is("quote")) {
                        if (args.isEmpty()) return r.tell("Usage: /raw LINE");
                        return r.check(session.raw(args), "The line");
                }
                if (is("clear")) {
                        outcome.action = Outcome::Action::Clear;
                        outcome.buffer = current;
                        return Error::Ok;
                }
                if (is("help")) {
                        String list;
                        (void)list.assign("Commands:");
                        for (size_t i = 0; i < CommandCount; ++i) {
                                (void)list.append(" /");
                                (void)list.append(CommandNames[i]);
                        }
                        (void)list.append(". Others are sent as they are; //text says text starting with /.");
                        return r.tell(list);
                }
                // Any other: sent as typed, the command in capitals.
                if (cmd.isEmpty()) return r.tell("Usage: /command [arguments] (/help)");
                String upper;
                for (char c : cmd) (void)upper.append(c >= 'a' && c <= 'z' ? char(c - 32) : c);
                return r.raw(upper, args);
        }

} // namespace irc
