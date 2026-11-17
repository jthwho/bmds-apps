/**
 * @file      session.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "session.h"

#include "format.h"

#include <cstring>

namespace irc {

        namespace {

                // The capabilities asked for when offered (devplan/irc.md §1); SASL only with a login.
                const char *const WantedCaps[] = {"server-time",    "message-tags",  "away-notify",
                                                  "account-notify", "extended-join", "multi-prefix",
                                                  "cap-notify",     "chghost",       "echo-message"};

                bool hasBreak(Text text) {
                        for (char c : text)
                                if (c == '\r' || c == '\n' || c == '\0') return true;
                        return false;
                }

                // A name's word: no space, comma or break in it, and not empty.
                bool isName(Text name) {
                        if (name.isEmpty() || hasBreak(name)) return false;
                        return name.find(' ') == Text::NoPos && name.find(',') == Text::NoPos;
                }

                // The capability's name in an LS or ACK token (`sasl=PLAIN,EXTERNAL`, `-away-notify`).
                Text capName(Text token) {
                        const size_t eq = token.find('=');
                        return eq == Text::NoPos ? token : token.left(eq);
                }

                // Calls @p fn with each space-separated word of @p text.
                template <typename F> void eachWord(Text text, F &&fn) {
                        size_t at = 0;
                        while (at < text.size()) {
                                size_t end = text.find(' ', at);
                                if (end == Text::NoPos) end = text.size();
                                if (end > at) fn(text.mid(at, end - at));
                                at = end + 1;
                        }
                        return;
                }

                bool isService(Text nick) {
                        static const char *const services[] = {"NickServ", "ChanServ", "MemoServ", "OperServ",
                                                               "HostServ", "BotServ",  "Global",   "SaslServ"};
                        for (const char *s : services)
                                if (nick.equalsIgnoringCase(s)) return true;
                        return false;
                }

                bool contains(const Vector<String> &list, Text item) {
                        for (const String &s : list)
                                if (s.view().equalsIgnoringCase(item)) return true;
                        return false;
                }

        } // namespace

        // ---- Lines out

        void Session::sendLine(Kind kind, Text line) {
                Outgoing out;
                out.kind = kind;
                if (out.line.assign(line) != Error::Ok) return;
                size_t at = _out.size();
                if (kind == Kind::Priority) {
                        // After the other priority lines, before the rest.
                        at = 0;
                        while (at < _out.size() && _out[at].kind == Kind::Priority) ++at;
                }
                (void)_out.insert(at, std::move(out));
                return;
        }

        void Session::send(Kind kind, Text command, const Text *params, size_t count, bool lastIsText) {
                LineBuilder b(command);
                for (size_t i = 0; i < count; ++i) {
                        if (i + 1 == count && lastIsText)
                                b.last(params[i]);
                        else
                                b.arg(params[i]);
                }
                const Text line = b.line();
                if (b.error() == Error::Ok) sendLine(kind, line);
                return;
        }

        bool Session::takeLine(int64_t mono, String &line) {
                if (_out.isEmpty()) return false;
                const int64_t full = Burst * PaceMs;
                _credit += mono - _creditAt;
                if (_credit > full) _credit = full;
                _creditAt = mono;
                Outgoing &front = _out[0];
                if (front.kind != Kind::Priority) {
                        if (_state != State::Registered || _credit < PaceMs) return false;
                        _credit -= PaceMs;
                }
                line = std::move(front.line);
                _out.removeAt(0);
                return true;
        }

        int64_t Session::sendWaitMs(int64_t mono) const {
                if (_out.isEmpty()) return -1;
                if (_out[0].kind == Kind::Priority) return 0;
                if (_state != State::Registered) return -1;
                int64_t credit = _credit + (mono - _creditAt);
                if (credit > Burst * PaceMs) credit = Burst * PaceMs;
                return credit >= PaceMs ? 0 : PaceMs - credit;
        }

        // ---- Events out

        void Session::emit(Event &&event) {
                (void)_events.append(std::move(event));
                return;
        }

        Event Session::make(Event::Kind kind, uint8_t flags, Text target, Text nick, Text text, int64_t time) {
                Event e;
                e.kind = kind;
                e.flags = flags;
                (void)e.target.assign(target);
                (void)e.nick.assign(nick);
                (void)Format::toUtf8(text, e.text);
                e.time = time;
                return e;
        }

        bool Session::takeEvent(Event &out) {
                if (_eventHead >= _events.size()) {
                        _events.clear();
                        _eventHead = 0;
                        return false;
                }
                out = std::move(_events[_eventHead++]);
                if (_eventHead == _events.size()) {
                        _events.clear();
                        _eventHead = 0;
                }
                return true;
        }

        int64_t Session::timeOf(const Message &message, const Now &now) const {
                int64_t ms = 0;
                if (message.hasTag("time") && Format::parseServerTime(message.tag("time"), ms)) return ms;
                return now.wall;
        }

        bool Session::hasCap(Text cap) const {
                return contains(_caps, cap);
        }

        // ---- Channels and members

        Channel *Session::findChannel(Text name) {
                for (Channel &c : _channels)
                        if (sameName(c.name, name, _isupport.caseMapping())) return &c;
                return nullptr;
        }

        const Channel *Session::channel(Text name) const {
                return const_cast<Session *>(this)->findChannel(name);
        }

        Member *Session::findMember(Channel &channel, Text nick) {
                for (Member &m : channel.members)
                        if (sameName(m.nick, nick, _isupport.caseMapping())) return &m;
                return nullptr;
        }

        void Session::setPrefix(Member &member, char prefix, bool on) {
                // Kept in the server's order of rank, highest first.
                char       out[sizeof(member.prefixes)] = {};
                size_t     n = 0;
                const Text all = _isupport.prefixChars();
                const Text had(member.prefixes);
                for (char p : all) {
                        const bool keep = p == prefix ? on : had.find(p) != Text::NoPos;
                        if (keep && n + 1 < sizeof(out)) out[n++] = p;
                }
                std::memcpy(member.prefixes, out, sizeof(out));
                return;
        }

        void Session::addMember(Channel &channel, Text item) {
                // @+nick, or nick!user@host (userhost-in-names, not asked for but harmless).
                size_t     i = 0;
                const Text prefixChars = _isupport.prefixChars();
                while (i < item.size() && prefixChars.find(item[i]) != Text::NoPos) ++i;
                Text         nick = item.mid(i);
                const size_t bang = nick.find('!');
                if (bang != Text::NoPos) nick = nick.left(bang);
                if (nick.isEmpty()) return;
                Member *m = findMember(channel, nick);
                if (m == nullptr) {
                        Member fresh;
                        if (fresh.nick.assign(nick) != Error::Ok ||
                            channel.members.append(std::move(fresh)) != Error::Ok)
                                return;
                        m = &channel.members.last();
                }
                for (size_t k = 0; k < i; ++k) setPrefix(*m, item[k], true);
                return;
        }

        // ---- The connection

        void Session::connected(const Now &now) {
                _state = State::Registering;
                (void)_nick.assign(_config.nick);
                _account.clear();
                _selfUser.clear();
                _selfHost.clear();
                _isupport.reset();
                _caps.clear();
                _offered.clear();
                _capEnded = false;
                _saslFailed = false;
                _identifying = false;
                _joinsDone = false;
                _joinAt = -1;
                _regainAt = -1;
                _nickTry = 0;
                _lastReceived = now.mono;
                _pingSent = -1;
                _timedOut = false;
                _credit = Burst * PaceMs;
                _creditAt = now.mono;
                const Text user = _config.user.isEmpty() ? _config.nick.view() : _config.user.view();
                const Text real = _config.realName.isEmpty() ? _config.nick.view() : _config.realName.view();
                if (_config.login == Login::Password && !_config.password.isEmpty()) {
                        const Text p[] = {_config.password};
                        send(Kind::Priority, "PASS", p, 1, false);
                }
                const Text ls[] = {"LS", "302"};
                send(Kind::Priority, "CAP", ls, 2, false);
                const Text n[] = {_nick};
                send(Kind::Priority, "NICK", n, 1, false);
                const Text u[] = {user, "0", "*", real};
                send(Kind::Priority, "USER", u, 4, true);
                return;
        }

        void Session::disconnected() {
                _state = State::Disconnected;
                // What the user typed stays for the next connection; the rest was this one's.
                for (size_t i = _out.size(); i > 0; --i)
                        if (_out[i - 1].kind != Kind::User) _out.removeAt(i - 1);
                for (Channel &c : _channels) {
                        c.joined = false;
                        c.members.clear();
                        c.namesDone = true;
                }
                _caps.clear();
                _offered.clear();
                _identifying = false;
                _joinsDone = false;
                _joinAt = -1;
                _regainAt = -1;
                _pingSent = -1;
                return;
        }

        void Session::tick(const Now &now) {
                if (_state == State::Disconnected) return;
                const int64_t quiet = now.mono - _lastReceived;
                if (quiet >= TimeoutMs) {
                        if (!_timedOut) emit(make(Event::Kind::Timeout, 0, "", "", "", now.wall));
                        _timedOut = true;
                } else if (quiet >= PingAfterMs && _pingSent < 0 && _state == State::Registered) {
                        const Text p[] = {"bmds-irc"};
                        send(Kind::Priority, "PING", p, 1, true);
                        _pingSent = now.mono;
                }
                if (_joinAt >= 0 && now.mono >= _joinAt) joinAll();
                if (_regainAt >= 0 && now.mono >= _regainAt) {
                        _regainAt = -1;
                        if (!isMe(_config.nick)) {
                                const Text n[] = {_config.nick};
                                send(Kind::Protocol, "NICK", n, 1, false);
                        }
                }
                return;
        }

        int64_t Session::tickWaitMs(int64_t mono) const {
                if (_state == State::Disconnected) return -1;
                int64_t    soonest = -1;
                const auto consider = [&](int64_t at) {
                        if (at < 0) return;
                        const int64_t wait = at > mono ? at - mono : 0;
                        if (soonest < 0 || wait < soonest) soonest = wait;
                        return;
                };
                if (!_timedOut) consider(_lastReceived + TimeoutMs);
                if (_pingSent < 0 && _state == State::Registered) consider(_lastReceived + PingAfterMs);
                consider(_joinAt);
                consider(_regainAt);
                return soonest;
        }

        void Session::received(Text line, const Now &now) {
                _lastReceived = now.mono;
                _timedOut = false;
                Message m;
                if (parse(line, m) != Error::Ok) return;
                const int numeric = m.numeric();
                if (numeric >= 0) {
                        onNumeric(m, numeric, now);
                        return;
                }
                const Source src = parseSource(m.source);
                if (m.is("PING")) {
                        const Text p[] = {m.last()};
                        send(Kind::Priority, "PONG", p, 1, true);
                } else if (m.is("PONG")) {
                        if (_pingSent >= 0) {
                                _lagMs = now.mono - _pingSent;
                                _pingSent = -1;
                                Event e = make(Event::Kind::Lag, 0, "", "", "", now.wall);
                                e.number = _lagMs;
                                emit(std::move(e));
                        }
                } else if (m.is("CAP")) {
                        onCap(m);
                } else if (m.is("AUTHENTICATE")) {
                        onAuthenticate(m);
                } else if (m.is("PRIVMSG") || m.is("NOTICE")) {
                        onMessage(m, m.is("NOTICE"), now);
                } else if (m.is("JOIN")) {
                        onJoin(m, now);
                } else if (m.is("PART") && m.paramCount >= 1) {
                        const Text name = m.param(0);
                        const bool me = isMe(src.nick);
                        emit(make(Event::Kind::Part, me ? Event::Self : 0, name, src.nick, m.param(1), timeOf(m, now)));
                        for (size_t i = 0; i < _channels.size(); ++i) {
                                Channel &c = _channels[i];
                                if (!sameName(c.name, name, _isupport.caseMapping())) continue;
                                if (me) {
                                        _channels.removeAt(i);
                                } else {
                                        for (size_t k = 0; k < c.members.size(); ++k)
                                                if (sameName(c.members[k].nick, src.nick, _isupport.caseMapping()))
                                                        c.members.removeAt(k);
                                        emit(make(Event::Kind::Members, 0, name, "", "", now.wall));
                                }
                                break;
                        }
                } else if (m.is("KICK") && m.paramCount >= 2) {
                        const Text name = m.param(0), victim = m.param(1);
                        const bool me = isMe(victim);
                        Event      e =
                                make(Event::Kind::Kick, me ? Event::Self : 0, name, victim, m.param(2), timeOf(m, now));
                        (void)e.extra.assign(src.nick);
                        emit(std::move(e));
                        for (size_t i = 0; i < _channels.size(); ++i) {
                                Channel &c = _channels[i];
                                if (!sameName(c.name, name, _isupport.caseMapping())) continue;
                                if (me) {
                                        _channels.removeAt(i);
                                } else {
                                        for (size_t k = 0; k < c.members.size(); ++k)
                                                if (sameName(c.members[k].nick, victim, _isupport.caseMapping()))
                                                        c.members.removeAt(k);
                                        emit(make(Event::Kind::Members, 0, name, "", "", now.wall));
                                }
                                break;
                        }
                } else if (m.is("QUIT")) {
                        const int64_t t = timeOf(m, now);
                        for (Channel &c : _channels)
                                for (size_t k = 0; k < c.members.size(); ++k)
                                        if (sameName(c.members[k].nick, src.nick, _isupport.caseMapping())) {
                                                c.members.removeAt(k);
                                                emit(make(Event::Kind::Quit, 0, c.name, src.nick, m.param(0), t));
                                                emit(make(Event::Kind::Members, 0, c.name, "", "", now.wall));
                                                break;
                                        }
                        emit(make(Event::Kind::Quit, Event::Query, src.nick, src.nick, m.param(0), t));
                } else if (m.is("NICK") && m.paramCount >= 1) {
                        const Text    to = m.param(0);
                        const bool    me = isMe(src.nick);
                        const uint8_t self = me ? Event::Self : 0;
                        const int64_t t = timeOf(m, now);
                        for (Channel &c : _channels)
                                if (Member *member = findMember(c, src.nick); member != nullptr) {
                                        (void)member->nick.assign(to);
                                        emit(make(Event::Kind::Nick, self, c.name, src.nick, to, t));
                                        emit(make(Event::Kind::Members, 0, c.name, "", "", now.wall));
                                }
                        emit(make(Event::Kind::Nick, uint8_t(self | Event::Query), src.nick, src.nick, to, t));
                        if (me) {
                                (void)_nick.assign(to);
                                if (isMe(_config.nick)) _regainAt = -1;
                        }
                } else if (m.is("TOPIC") && m.paramCount >= 1) {
                        if (Channel *c = findChannel(m.param(0)); c != nullptr)
                                (void)Format::toUtf8(m.param(1), c->topic);
                        emit(make(Event::Kind::Topic, isMe(src.nick) ? Event::Self : 0, m.param(0), src.nick,
                                  m.param(1), timeOf(m, now)));
                } else if (m.is("MODE")) {
                        onMode(m, now);
                } else if (m.is("AWAY")) {
                        // away-notify: gone with a reason, or back.
                        const bool away = m.paramCount >= 1;
                        for (Channel &c : _channels)
                                if (Member *member = findMember(c, src.nick);
                                    member != nullptr && member->away != away) {
                                        member->away = away;
                                        emit(make(Event::Kind::Members, 0, c.name, "", "", now.wall));
                                }
                } else if (m.is("CHGHOST") && m.paramCount >= 2) {
                        if (isMe(src.nick)) {
                                (void)_selfUser.assign(m.param(0));
                                (void)_selfHost.assign(m.param(1));
                        }
                } else if (m.is("INVITE") && m.paramCount >= 2) {
                        emit(make(Event::Kind::Invite, 0, "", src.nick, m.param(1), timeOf(m, now)));
                } else if (m.is("ERROR")) {
                        emit(make(Event::Kind::Error, Event::Server, "", "", m.last(), now.wall));
                }
                return;
        }

        // ---- Registration

        void Session::endCap() {
                if (_capEnded) return;
                _capEnded = true;
                const Text p[] = {"END"};
                send(Kind::Priority, "CAP", p, 1, false);
                return;
        }

        void Session::onCap(const Message &m) {
                const Text sub = m.param(1);
                if (sub.equalsIgnoringCase("LS") || sub.equalsIgnoringCase("NEW")) {
                        const bool more = m.paramCount >= 4 && m.param(2) == Text("*");
                        const Text caps = more ? m.param(3) : m.param(2);
                        eachWord(caps, [&](Text token) {
                                String name;
                                if (name.assign(capName(token)) == Error::Ok) (void)_offered.append(std::move(name));
                        });
                        if (more) return;
                        // Ask for what we want of what is offered (SASL only while registering, with a login).
                        String     request;
                        const bool registering = _state == State::Registering && !_capEnded;
                        for (const char *want : WantedCaps)
                                if (contains(_offered, want) && !contains(_caps, want)) {
                                        if (!request.isEmpty()) (void)request.append(' ');
                                        (void)request.append(want);
                                }
                        const bool sasl = (_config.login == Login::Sasl && !_config.password.isEmpty()) ||
                                          _config.login == Login::SaslExternal;
                        if (registering && sasl && contains(_offered, "sasl")) {
                                if (!request.isEmpty()) (void)request.append(' ');
                                (void)request.append("sasl");
                        }
                        if (request.isEmpty()) {
                                if (registering) endCap();
                                return;
                        }
                        const Text p[] = {"REQ", request};
                        send(Kind::Priority, "CAP", p, 2, true);
                } else if (sub.equalsIgnoringCase("ACK")) {
                        bool sasl = false;
                        eachWord(m.last(), [&](Text token) {
                                if (token.startsWith("-")) {
                                        for (size_t i = 0; i < _caps.size(); ++i)
                                                if (_caps[i].view().equalsIgnoringCase(capName(token.mid(1))))
                                                        _caps.removeAt(i);
                                        return;
                                }
                                String name;
                                if (name.assign(capName(token)) == Error::Ok && !contains(_caps, name))
                                        (void)_caps.append(std::move(name));
                                if (capName(token).equalsIgnoringCase("sasl")) sasl = true;
                        });
                        if (_state != State::Registering || _capEnded) return;
                        if (sasl) {
                                const Text p[] = {_config.login == Login::SaslExternal ? "EXTERNAL" : "PLAIN"};
                                send(Kind::Priority, "AUTHENTICATE", p, 1, false);
                        } else {
                                endCap();
                        }
                } else if (sub.equalsIgnoringCase("NAK")) {
                        if (_state == State::Registering) endCap();
                } else if (sub.equalsIgnoringCase("DEL")) {
                        eachWord(m.last(), [&](Text token) {
                                for (size_t i = 0; i < _caps.size(); ++i)
                                        if (_caps[i].view().equalsIgnoringCase(capName(token))) _caps.removeAt(i);
                                for (size_t i = 0; i < _offered.size(); ++i)
                                        if (_offered[i].view().equalsIgnoringCase(capName(token))) _offered.removeAt(i);
                        });
                }
                return;
        }

        void Session::onAuthenticate(const Message &m) {
                if (m.param(0) != Text("+")) return;
                if (_config.login == Login::SaslExternal) {
                        const Text p[] = {"+"};
                        send(Kind::Priority, "AUTHENTICATE", p, 1, false);
                        return;
                }
                // PLAIN: authzid NUL authcid NUL password (no authzid), in 400-byte pieces.
                const Text account = _config.account.isEmpty() ? _config.nick.view() : _config.account.view();
                String     plain, encoded;
                (void)plain.append('\0');
                (void)plain.append(account);
                (void)plain.append('\0');
                (void)plain.append(_config.password);
                if (Format::base64(plain, encoded) != Error::Ok) return;
                const Text all = encoded;
                size_t     at = 0;
                do {
                        const Text p[] = {all.mid(at, 400)};
                        send(Kind::Priority, "AUTHENTICATE", p, 1, false);
                        at += 400;
                } while (at < all.size());
                if (all.size() % 400 == 0) {
                        const Text p[] = {"+"};
                        send(Kind::Priority, "AUTHENTICATE", p, 1, false);
                }
                return;
        }

        void Session::onNickTaken(const Message &m, const Now &now) {
                if (_state != State::Registering) {
                        emit(make(Event::Kind::ServerText, Event::Server, "", "", m.last(), now.wall));
                        return;
                }
                // The alternates, then the nick with _ added (up to three), then with a number.
                String       next;
                const size_t alts = _config.altNicks.size();
                if (_nickTry < alts) {
                        (void)next.assign(_config.altNicks[_nickTry]);
                } else if (_nickTry < alts + 3) {
                        (void)next.assign(_config.nick);
                        for (size_t i = 0; i <= _nickTry - alts; ++i) (void)next.append('_');
                } else {
                        (void)next.assign(_config.nick);
                        const int  n = int((now.mono + int64_t(_nickTry)) % 1000);
                        const char digits[3] = {char('0' + n / 100), char('0' + n / 10 % 10), char('0' + n % 10)};
                        (void)next.append(Text(digits, 3));
                }
                ++_nickTry;
                (void)_nick.assign(next);
                const Text p[] = {next};
                send(Kind::Priority, "NICK", p, 1, false);
                return;
        }

        void Session::identify() {
                const Text account = _config.account.isEmpty() ? _config.nick.view() : _config.account.view();
                String     text;
                (void)text.assign("IDENTIFY ");
                (void)text.append(account);
                (void)text.append(' ');
                (void)text.append(_config.password);
                const Text p[] = {"NickServ", text};
                send(Kind::Protocol, "PRIVMSG", p, 2, true);
                return;
        }

        void Session::onWelcome(const Message &m, const Now &now) {
                _state = State::Registered;
                _capEnded = true;
                (void)_nick.assign(m.param(0));
                emit(make(Event::Kind::Registered, Event::Server, "", "", _nick, now.wall));
                const bool password = !_config.password.isEmpty() && _config.login != Login::Password;
                // Our nick held by someone else (ourselves, gone stale): ghosted, then taken.
                if (!isMe(_config.nick) && password) {
                        String text;
                        (void)text.assign("GHOST ");
                        (void)text.append(_config.nick);
                        (void)text.append(' ');
                        (void)text.append(_config.password);
                        const Text p[] = {"NickServ", text};
                        send(Kind::Protocol, "PRIVMSG", p, 2, true);
                        _regainAt = now.mono + RegainWaitMs;
                }
                const bool nickserv = _config.login == Login::NickServ || (_config.login == Login::Sasl && _saslFailed);
                if (nickserv && password && _account.isEmpty()) {
                        identify();
                        _identifying = true;
                        _joinAt = now.mono + IdentifyWaitMs;
                } else {
                        joinAll();
                }
                return;
        }

        void Session::joinAll() {
                if (_joinsDone) return;
                _joinsDone = true;
                _identifying = false;
                _joinAt = -1;
                // The settings' channels ("#chan" or "#chan key"), then those of the last connection.
                Vector<String> names;
                for (const String &entry : _config.autojoin) {
                        const Text   e = entry;
                        const size_t space = e.find(' ');
                        const Text   name = space == Text::NoPos ? e : e.left(space);
                        const Text   key = space == Text::NoPos ? Text() : e.mid(space + 1);
                        if (!isName(name)) continue;
                        String n;
                        if (n.assign(name) != Error::Ok) continue;
                        (void)names.append(std::move(n));
                        Text p[] = {name, key};
                        send(Kind::Protocol, "JOIN", p, key.isEmpty() ? 1 : 2, false);
                }
                for (const Channel &c : _channels) {
                        bool already = false;
                        for (const String &n : names)
                                if (sameName(n, c.name, _isupport.caseMapping())) already = true;
                        if (already || c.joined) continue;
                        const Text p[] = {c.name};
                        send(Kind::Protocol, "JOIN", p, 1, false);
                }
                return;
        }

        // ---- Messages

        void Session::onMessage(const Message &m, bool notice, const Now &now) {
                if (m.paramCount < 2) return;
                const Source      src = parseSource(m.source);
                const Text        target = m.param(0);
                const Text        text = m.last();
                const int64_t     t = timeOf(m, now);
                const Event::Kind kind = notice ? Event::Kind::Notice : Event::Kind::Message;

                // The server's own (before registration too: "*** Looking up your hostname").
                if (m.source.isEmpty() || src.isServer()) {
                        emit(make(Event::Kind::Notice, Event::Server, "", src.nick, text, t));
                        return;
                }
                const bool fromMe = isMe(src.nick);
                const bool service = isService(src.nick);
                if (notice && src.nick.equalsIgnoringCase("NickServ")) {
                        // NickServ answered: the joins wait no longer; a ghosted nick is ours to take.
                        if (_identifying) joinAll();
                        if (_regainAt >= 0) _regainAt = now.mono;
                }

                uint8_t      flags = fromMe ? uint8_t(Event::Self) : uint8_t(0);
                Text         body = text;
                Format::Ctcp ctcp;
                if (Format::parseCtcp(text, ctcp)) {
                        if (notice) return; // replies to our requests: none sent but VERSION's own
                        if (ctcp.command.equalsIgnoringCase("ACTION")) {
                                flags |= Event::Action;
                                body = ctcp.args;
                        } else {
                                if (fromMe) return;
                                String reply;
                                (void)reply.append('\x01');
                                if (ctcp.command.equalsIgnoringCase("VERSION")) {
                                        (void)reply.append("VERSION ");
                                        (void)reply.append(_config.version.isEmpty() ? Text("BMDS IRC")
                                                                                     : _config.version.view());
                                } else if (ctcp.command.equalsIgnoringCase("PING")) {
                                        (void)reply.append("PING ");
                                        (void)reply.append(ctcp.args);
                                } else {
                                        return; // the rest ignored (devplan/irc.md §7)
                                }
                                (void)reply.append('\x01');
                                const Text p[] = {src.nick, reply};
                                send(Kind::Protocol, "NOTICE", p, 2, true);
                                return;
                        }
                }

                const Text channelName = _isupport.withoutStatusPrefix(target);
                if (_isupport.isChannel(channelName)) {
                        emit(make(kind, flags, channelName, src.nick, body, t));
                        return;
                }
                if (notice && !fromMe) {
                        // To us: the network's buffer decides where (devplan/irc.md §7b).
                        emit(make(kind, uint8_t(flags | (service ? Event::Service : 0)), "", src.nick, body, t));
                        return;
                }
                // A private chat: named for the other person (our own echoed: for whom it was).
                const Text other = fromMe ? target : src.nick;
                emit(make(kind, uint8_t(flags | Event::Query | (service ? Event::Service : 0)), other, src.nick, body,
                          t));
                return;
        }

        void Session::onJoin(const Message &m, const Now &now) {
                if (m.paramCount < 1) return;
                const Source src = parseSource(m.source);
                const Text   name = m.param(0);
                const bool   me = isMe(src.nick);
                Channel     *c = findChannel(name);
                if (me) {
                        if (c == nullptr) {
                                Channel fresh;
                                if (fresh.name.assign(name) != Error::Ok ||
                                    _channels.append(std::move(fresh)) != Error::Ok)
                                        return;
                                c = &_channels.last();
                        }
                        c->joined = true;
                        c->members.clear();
                        c->topic.clear();
                        c->namesDone = true;
                        (void)_selfUser.assign(src.user);
                        (void)_selfHost.assign(src.host);
                }
                if (c != nullptr) addMember(*c, src.nick);
                // extended-join: JOIN #chan account :real name
                Event e = make(Event::Kind::Join, me ? Event::Self : 0, name, src.nick, m.param(2), timeOf(m, now));
                if (m.paramCount >= 3 && m.param(1) != Text("*")) (void)e.extra.assign(m.param(1));
                emit(std::move(e));
                if (c != nullptr && !me) emit(make(Event::Kind::Members, 0, name, "", "", now.wall));
                return;
        }

        void Session::onMode(const Message &m, const Now &now) {
                if (m.paramCount < 2) return;
                const Source src = parseSource(m.source);
                const Text   target = m.param(0);
                String       text;
                for (size_t i = 1; i < m.paramCount; ++i) {
                        if (i > 1) (void)text.append(' ');
                        (void)text.append(m.params[i]);
                }
                Channel *c = _isupport.isChannel(target) ? findChannel(target) : nullptr;
                emit(make(Event::Kind::Mode, isMe(src.nick) ? Event::Self : 0, c != nullptr ? target : Text(), src.nick,
                          text, timeOf(m, now)));
                if (c == nullptr) return;
                // +o-v nick nick: each mode taking its parameter in turn.
                bool   setting = true, changed = false;
                size_t next = 2;
                for (char mode : m.param(1)) {
                        if (mode == '+' || mode == '-') {
                                setting = mode == '+';
                                continue;
                        }
                        if (!_isupport.takesParameter(mode, setting)) continue;
                        const Text param = m.param(next++);
                        const char prefix = _isupport.prefixFor(mode);
                        if (prefix == 0 || param.isEmpty()) continue;
                        if (Member *member = findMember(*c, param); member != nullptr) {
                                setPrefix(*member, prefix, setting);
                                changed = true;
                        }
                }
                if (changed) emit(make(Event::Kind::Members, 0, target, "", "", now.wall));
                return;
        }

        void Session::onNumeric(const Message &m, int numeric, const Now &now) {
                switch (numeric) {
                        case 1: onWelcome(m, now); return;
                        case 5: _isupport.apply(m); return;
                        case 432:
                        case 433:
                        case 437: onNickTaken(m, now); return;
                        case 900: // RPL_LOGGEDIN nick nick!user@host account :You are now logged in as account
                                (void)_account.assign(m.param(2));
                                emit(make(Event::Kind::LoggedIn, Event::Server, "", "", _account, now.wall));
                                if (_identifying) joinAll();
                                return;
                        case 901: _account.clear(); return; // logged out
                        case 903: endCap(); return;         // SASL succeeded
                        case 902:
                        case 904:
                        case 905:
                        case 906:
                                _saslFailed = true;
                                emit(make(Event::Kind::LoginFailed, Event::Server, "", "", m.last(), now.wall));
                                endCap();
                                return;
                        case 907:
                        case 908: return; // already authenticated; the mechanisms
                        case 332:         // RPL_TOPIC nick #chan :topic
                                if (Channel *c = findChannel(m.param(1)); c != nullptr)
                                        (void)Format::toUtf8(m.last(), c->topic);
                                emit(make(Event::Kind::Topic, 0, m.param(1), "", m.last(), now.wall));
                                return;
                        case 333: return; // who set it, when
                        case 353: {       // RPL_NAMREPLY nick = #chan :@a +b c
                                Channel *c = findChannel(m.param(2));
                                if (c == nullptr) return;
                                if (c->namesDone) {
                                        c->members.clear();
                                        c->namesDone = false;
                                }
                                eachWord(m.last(), [&](Text item) { addMember(*c, item); });
                                return;
                        }
                        case 366: // RPL_ENDOFNAMES nick #chan :End of /NAMES list.
                                if (Channel *c = findChannel(m.param(1)); c != nullptr) {
                                        c->namesDone = true;
                                        emit(make(Event::Kind::Members, 0, c->name, "", "", now.wall));
                                }
                                return;
                        case 396: (void)_selfHost.assign(m.param(1)); return; // RPL_VISIBLEHOST
                        default: break;
                }
                // The rest to the network's buffer: the parameters past our nick.
                String text;
                for (size_t i = 1; i < m.paramCount; ++i) {
                        if (i > 1) (void)text.append(' ');
                        (void)text.append(m.params[i]);
                }
                emit(make(Event::Kind::ServerText, Event::Server, "", "", text, timeOf(m, now)));
                return;
        }

        // ---- What the user does

        Error Session::say(Text command, Text target, Text text, bool action, const Now &now) {
                if (!isName(target) || hasBreak(text)) return Error::Malformed;
                if (text.isEmpty()) return Error::Invalid;
                // The server relays ":nick!user@host COMMAND target :text" in 510 bytes.
                const size_t user = _selfUser.isEmpty() ? 10 : _selfUser.size();
                const size_t host = _selfHost.isEmpty() ? 63 : _selfHost.size();
                const size_t used = 1 + _nick.size() + 1 + user + 1 + host + 1 + command.size() + 1 + target.size() +
                                    2 + (action ? 9 : 0);
                if (used >= 500) return Error::TooLong;
                const size_t room = 510 - used;
                const bool   echo = hasCap("echo-message");
                const bool   channel = _isupport.isChannel(_isupport.withoutStatusPrefix(target));
                Text         rest = text;
                while (!rest.isEmpty()) {
                        const size_t n = splitAt(rest, room);
                        if (n == 0) return Error::TooLong;
                        const Text piece = rest.left(n);
                        rest = rest.mid(n);
                        if (!rest.isEmpty() && rest[0] == ' ') rest = rest.mid(1);
                        String body;
                        if (action)
                                (void)body.append("\x01"
                                                  "ACTION ");
                        (void)body.append(piece);
                        if (action) (void)body.append('\x01');
                        const Text p[] = {target, body};
                        send(Kind::User, command, p, 2, true);
                        if (!echo) {
                                const Event::Kind kind =
                                        command == Text("NOTICE") ? Event::Kind::Notice : Event::Kind::Message;
                                uint8_t flags = Event::Self;
                                if (action) flags |= Event::Action;
                                if (!channel) flags |= Event::Query;
                                emit(make(kind, flags, target, _nick, piece, now.wall));
                        }
                }
                return Error::Ok;
        }

        Error Session::privmsg(Text target, Text text, const Now &now) {
                return say("PRIVMSG", target, text, false, now);
        }

        Error Session::action(Text target, Text text, const Now &now) {
                return say("PRIVMSG", target, text, true, now);
        }

        Error Session::notice(Text target, Text text, const Now &now) {
                return say("NOTICE", target, text, false, now);
        }

        Error Session::join(Text channel, Text key) {
                if (!isName(channel) || (!key.isEmpty() && !isName(key))) return Error::Malformed;
                const Text p[] = {channel, key};
                send(Kind::User, "JOIN", p, key.isEmpty() ? 1 : 2, false);
                return Error::Ok;
        }

        Error Session::part(Text channel, Text reason) {
                if (!isName(channel) || hasBreak(reason)) return Error::Malformed;
                const Text p[] = {channel, reason};
                send(Kind::User, "PART", p, reason.isEmpty() ? 1 : 2, true);
                return Error::Ok;
        }

        Error Session::changeNick(Text nick) {
                if (!isName(nick) || nick[0] == ':') return Error::Malformed;
                const Text p[] = {nick};
                send(_state == State::Registered ? Kind::User : Kind::Priority, "NICK", p, 1, false);
                if (_state != State::Registered) (void)_nick.assign(nick);
                return Error::Ok;
        }

        Error Session::setTopic(Text channel, Text topic) {
                if (!isName(channel) || hasBreak(topic)) return Error::Malformed;
                const Text p[] = {channel, topic};
                send(Kind::User, "TOPIC", p, 2, true);
                return Error::Ok;
        }

        Error Session::setAway(Text reason) {
                if (hasBreak(reason)) return Error::Malformed;
                const Text p[] = {reason};
                send(Kind::User, "AWAY", p, reason.isEmpty() ? 0 : 1, true);
                return Error::Ok;
        }

        Error Session::quit(Text reason) {
                if (hasBreak(reason)) return Error::Malformed;
                const Text p[] = {reason};
                send(Kind::Priority, "QUIT", p, reason.isEmpty() ? 0 : 1, true);
                return Error::Ok;
        }

        Error Session::raw(Text line) {
                if (line.isEmpty() || hasBreak(line)) return Error::Malformed;
                if (line.size() > LineBuilder::MaxLine - 2) return Error::TooLong;
                String l;
                if (l.assign(line) != Error::Ok || l.append("\r\n") != Error::Ok) return Error::NoMem;
                sendLine(Kind::User, l);
                return Error::Ok;
        }

        int64_t reconnectDelayMs(unsigned attempt, uint32_t random) {
                int64_t base = 2000;
                for (unsigned i = 0; i < attempt && base < 300000; ++i) base *= 2;
                if (base > 300000) base = 300000;
                return base * int64_t(80 + random % 41) / 100;
        }

} // namespace irc
