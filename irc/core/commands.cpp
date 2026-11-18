/**
 * @file      commands.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "commands.h"

#include "ignore.h"

namespace irc {

        const char *const CommandNames[] = {
                "away",    "back",     "ban",      "clear", "close",  "cs",     "ctcp",   "dehop",  "deop",
                "devoice", "find",     "help",     "hop",   "ignore", "invite", "join",   "kb",     "kick",
                "list",    "me",       "mode",     "msg",   "names",  "nick",   "notice", "notify", "ns",
                "op",      "part",     "ping",     "query", "quit",   "quote",  "raw",    "server", "topic",
                "unban",   "unignore", "unnotify", "voice", "who",    "whois",  "whowas"};
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

                                // The channel a command acts on: the first word if it is one (taken from
                                // @p args), else the one in view; empty if neither.
                                Text channelFrom(Text &args) {
                                        Text       rest;
                                        const Text first = firstWord(args, rest);
                                        if (session.isupport().isChannel(first)) {
                                                args = rest;
                                                return first;
                                        }
                                        const Text here = target();
                                        return session.isupport().isChannel(here) ? here : Text();
                                }

                                // MODE #c +ooo a b c: each nick of @p nicks, MODES of them a line.
                                Error modes(Text channel, char sign, char mode, Text nicks) {
                                        const uint32_t per = session.isupport().modes();
                                        String         letters, params;
                                        uint32_t       n = 0;
                                        Text           rest = nicks;
                                        for (;;) {
                                                Text       after;
                                                const Text nick = firstWord(rest, after);
                                                rest = after;
                                                if ((nick.isEmpty() || n == per) && n > 0) {
                                                        const Error err = raw("MODE", channel, letters, params);
                                                        if (err != Error::Ok) return err;
                                                        letters.clear();
                                                        params.clear();
                                                        n = 0;
                                                }
                                                if (nick.isEmpty()) return Error::Ok;
                                                if (letters.isEmpty()) (void)letters.append(sign);
                                                (void)letters.append(mode);
                                                if (!params.isEmpty()) (void)params.append(' ');
                                                (void)params.append(nick);
                                                ++n;
                                        }
                                }
                };

                // /server's [-m] host[:port] [[+]port]: + (or 6697) is TLS; no port, 6697 with TLS.
                bool parseServer(Text args, Outcome &outcome) {
                        Text rest;
                        Text first = firstWord(args, rest);
                        if (first == Text("-m")) {
                                outcome.fresh = true;
                                first = firstWord(rest, rest);
                        }
                        if (first.isEmpty()) return false;
                        Text         host = first, port = firstWord(rest, rest);
                        const size_t colon = first.findLast(':');
                        if (colon != Text::NoPos && first.find(':') == colon) { // host:port (not an IPv6 address)
                                host = first.left(colon);
                                if (port.isEmpty()) port = first.mid(colon + 1);
                        }
                        bool tls = port.isEmpty();
                        if (port.startsWith("+")) {
                                tls = true;
                                port = port.mid(1);
                        }
                        uint32_t number = 6697;
                        if (!port.isEmpty()) {
                                if (port.size() > 5) return false;
                                number = 0;
                                for (char c : port) {
                                        if (c < '0' || c > '9') return false;
                                        number = number * 10 + uint32_t(c - '0');
                                }
                                if (number == 0 || number > 65535) return false;
                                if (number == 6697) tls = true;
                        }
                        if (host.isEmpty() || outcome.text.assign(host) != Error::Ok) return false;
                        outcome.port = uint16_t(number);
                        outcome.on = tls;
                        outcome.action = Outcome::Action::Server;
                        return true;
                }

        } // namespace

        Error banMask(Text nick, Text host, String &out) {
                out.clear();
                if (nick.isEmpty()) return Error::Invalid;
                if (nick.find('!') != Text::NoPos || nick.find('@') != Text::NoPos)
                        return Ignores::normalize(nick, out);
                for (char c : nick)
                        if (c == ' ' || c == ',' || c == '\r' || c == '\n' || c == '\0' || c == '*')
                                return Error::Invalid;
                const size_t at = host.find('@');
                Error        err = Error::Ok;
                if (at != Text::NoPos && at + 1 < host.size()) {
                        err = out.assign("*!*@");
                        if (err == Error::Ok) err = out.append(host.mid(at + 1));
                } else {
                        err = out.assign(nick);
                        if (err == Error::Ok) err = out.append("!*@*");
                }
                return err == Error::Ok ? Error::Ok : Error::NoMem;
        }

        Error performLine(Text line, Text nick, String &out) {
                out.clear();
                while (!line.isEmpty() && line[0] == ' ') line = line.mid(1);
                if (line.isEmpty()) return Error::Invalid;
                if (line[0] != '/' && out.append('/') != Error::Ok) return Error::NoMem;
                for (size_t i = 0; i < line.size(); ++i) {
                        // $me, not $meal: a name's character after it keeps it as typed.
                        if (line.mid(i).startsWith("$me")) {
                                const char next = i + 3 < line.size() ? line[i + 3] : ' ';
                                const bool word = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                                                  (next >= '0' && next <= '9') || next == '_';
                                if (!word) {
                                        if (out.append(nick) != Error::Ok) return Error::NoMem;
                                        i += 2;
                                        continue;
                                }
                        }
                        if (out.append(line[i]) != Error::Ok) return Error::NoMem;
                }
                return Error::Ok;
        }

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
                // /away alone is back, as mIRC's.
                if (is("away")) return r.check(session.setAway(args), args.isEmpty() ? "Back" : "Away");
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
                        // +o nick: the channel in view's; #chan +o nick, or nick +i; alone, the
                        // modes of the channel in view (or ours).
                        if (first.startsWith("+") || first.startsWith("-")) {
                                if (r.target().isEmpty()) return r.tell("Usage: /mode [target] modes");
                                return r.raw("MODE", r.target(), args);
                        }
                        if (first.isEmpty()) {
                                const Text here = r.target();
                                return r.raw("MODE", session.isupport().isChannel(here) ? here : session.nick());
                        }
                        return r.raw("MODE", first, rest);
                }
                if (is("op") || is("deop") || is("voice") || is("devoice") || is("hop") || is("dehop")) {
                        Text       nicks = args;
                        const Text channel = r.channelFrom(nicks);
                        const char mode = is("op") || is("deop") ? 'o' : is("voice") || is("devoice") ? 'v' : 'h';
                        if (channel.isEmpty() || nicks.isEmpty()) return r.tell2("Usage: /", "op [#channel] nick...");
                        if (mode == 'h' && session.isupport().prefixFor('h') == 0)
                                return r.tell("This network has no half-operators");
                        return r.modes(channel, cmd.startsWith("de") ? '-' : '+', mode, nicks);
                }
                if (is("ban") || is("unban") || is("kb")) {
                        Text       who = args;
                        const Text channel = r.channelFrom(who);
                        Text       reason;
                        const Text nick = firstWord(who, reason);
                        if (channel.isEmpty() || nick.isEmpty())
                                return r.tell(is("kb") ? "Usage: /kb [#channel] nick [reason]"
                                                       : "Usage: /ban [#channel] nick|mask");
                        String mask;
                        if (banMask(nick, session.hostOf(nick), mask) != Error::Ok)
                                return r.tell("Not a nick or a mask");
                        const Error err = r.raw("MODE", channel, is("unban") ? "-b" : "+b", mask);
                        if (err != Error::Ok || !is("kb")) return err;
                        String text;
                        if (!reason.isEmpty()) {
                                (void)text.assign(":");
                                (void)text.append(reason);
                        }
                        return r.raw("KICK", channel, nick, text);
                }
                if (is("ctcp") || is("ping")) {
                        Text       what = rest;
                        const Text command = is("ping") ? Text("PING") : firstWord(rest, what);
                        if (first.isEmpty() || command.isEmpty())
                                return r.tell(is("ping") ? "Usage: /ping nick" : "Usage: /ctcp nick command [text]");
                        return r.check(session.ctcp(first, command, is("ping") ? Text() : what, now), "The CTCP");
                }
                if (is("whowas") || is("who")) {
                        if (first.isEmpty()) return r.tell(is("who") ? "Usage: /who mask" : "Usage: /whowas nick");
                        return r.raw(is("who") ? "WHO" : "WHOWAS", first);
                }
                if (is("list")) {
                        outcome.action = Outcome::Action::List;
                        return outcome.text.assign(args);
                }
                if (is("find")) {
                        outcome.action = Outcome::Action::Find;
                        outcome.buffer = current;
                        return outcome.text.assign(args);
                }
                if (is("ignore") || is("unignore")) {
                        outcome.action = Outcome::Action::Ignore;
                        outcome.on = is("ignore");
                        if (first.isEmpty()) {
                                if (outcome.on) return Error::Ok; // the list
                                outcome.action = Outcome::Action::None;
                                return r.tell("Usage: /unignore nick|mask [network]");
                        }
                        const Error err = Ignores::normalize(first, outcome.text);
                        if (err == Error::NoMem) return err;
                        if (err != Error::Ok) {
                                outcome.action = Outcome::Action::None;
                                return r.tell("Usage: /ignore nick|mask [network] (a mask: nick!user@host)");
                        }
                        Text after;
                        return outcome.extra.assign(firstWord(rest, after));
                }
                if (is("notify") || is("unnotify")) {
                        outcome.action = Outcome::Action::Notify;
                        outcome.on = is("notify");
                        if (first.isEmpty() && !outcome.on) {
                                outcome.action = Outcome::Action::None;
                                return r.tell("Usage: /unnotify nick");
                        }
                        if (session.isupport().isChannel(first)) {
                                outcome.action = Outcome::Action::None;
                                return r.tell("Usage: /notify nick (a person, not a channel)");
                        }
                        return outcome.text.assign(first);
                }
                if (is("server")) {
                        if (!parseServer(args, outcome)) {
                                outcome = Outcome();
                                return r.tell("Usage: /server [-m] host [[+]port] (+: TLS; none: 6697 with TLS)");
                        }
                        return Error::Ok;
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
