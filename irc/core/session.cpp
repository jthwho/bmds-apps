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
                const char *const WantedCaps[] = {"server-time",   "message-tags",     "away-notify", "account-notify",
                                                  "extended-join", "multi-prefix",     "cap-notify",  "chghost",
                                                  "echo-message",  "userhost-in-names"};

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

                // A decimal number's value (digits only), or -1.
                int64_t decimal(Text text) {
                        if (text.isEmpty() || text.size() > 18) return -1;
                        int64_t n = 0;
                        for (char c : text) {
                                if (c < '0' || c > '9') return -1;
                                n = n * 10 + (c - '0');
                        }
                        return n;
                }

                void appendDecimal(String &out, int64_t n) {
                        char   digits[20];
                        size_t k = 0;
                        if (n < 0) n = 0;
                        do {
                                digits[k++] = char('0' + n % 10);
                                n /= 10;
                        } while (n > 0 && k < sizeof(digits));
                        while (k > 0) (void)out.append(digits[--k]);
                        return;
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

        Text Session::hostOf(Text nick) const {
                for (const Channel &c : _channels)
                        for (const Member &m : c.members)
                                if (!m.host.isEmpty() && sameName(m.nick, nick, _isupport.caseMapping())) return m.host;
                return Text();
        }

        int Session::online(Text nick) const {
                for (const Watched &w : _watched)
                        if (sameName(w.nick, nick, _isupport.caseMapping())) return w.state;
                return -1;
        }

        void Session::setIgnores(const Ignores *ignores, Text network) {
                _ignores = ignores;
                (void)_networkName.assign(network);
                return;
        }

        // From someone ignored: counted, told a minute after the first (devplan/irc.md §7f).
        bool Session::ignored(const Source &src, const Now &now) {
                if (_ignores == nullptr || src.nick.isEmpty() || src.isServer() || isMe(src.nick)) return false;
                if (!_ignores->matches(src.nick, src.user, src.host, _networkName, _isupport.caseMapping()))
                        return false;
                ++_ignoredCount;
                if (_ignoredAt < 0) _ignoredAt = now.mono + IgnoredEveryMs;
                return true;
        }

        void Session::setChannelMode(Channel &c, char mode, bool on, Text param) {
                for (size_t i = 0; i < c.modes.size(); ++i)
                        if (c.modes[i].mode == mode) {
                                if (!on) {
                                        c.modes.removeAt(i);
                                } else {
                                        (void)c.modes[i].param.assign(param);
                                }
                                return;
                        }
                if (!on) return;
                ChannelMode m;
                m.mode = mode;
                if (m.param.assign(param) != Error::Ok) return;
                (void)c.modes.append(std::move(m));
                return;
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
                const Text   host = bang != Text::NoPos ? nick.mid(bang + 1) : Text();
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
                if (!host.isEmpty()) (void)m->host.assign(host);
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
                _performAsked = false;
                _performDone = false;
                _watching = false;
                _isonAt = -1;
                _isonAsked.clear();
                for (Watched &w : _watched) w.state = -1;
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
                _watching = false;
                _isonAt = -1;
                _isonAsked.clear();
                for (Watched &w : _watched) w.state = -1;
                for (Rejoin &r : _rejoins) r.at = -1;
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
                if (_isonAt >= 0 && now.mono >= _isonAt) sendIson(now);
                if (_ignoredAt >= 0 && now.mono >= _ignoredAt) {
                        Event e = make(Event::Kind::Ignored, Event::Server, "", "", "", now.wall);
                        e.number = _ignoredCount;
                        emit(std::move(e));
                        _ignoredCount = 0;
                        _ignoredAt = -1;
                }
                for (Rejoin &r : _rejoins) {
                        if (r.at < 0 || now.mono < r.at) continue;
                        r.at = -1;
                        r.last = now.mono;
                        if (_state != State::Registered) continue;
                        const Text p[] = {r.name, r.key};
                        send(Kind::Protocol, "JOIN", p, r.key.isEmpty() ? 1 : 2, false);
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
                consider(_isonAt);
                consider(_ignoredAt);
                for (const Rejoin &r : _rejoins) consider(r.at);
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
                                        if (_config.rejoin) scheduleRejoin(c, now);
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
                        if (Channel *c = findChannel(m.param(0)); c != nullptr) {
                                (void)Format::toUtf8(m.param(1), c->topic);
                                (void)c->topicBy.assign(src.nick);
                                c->topicAt = timeOf(m, now);
                        }
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
                        for (Channel &c : _channels)
                                if (Member *member = findMember(c, src.nick); member != nullptr) {
                                        (void)member->host.assign(m.param(0));
                                        (void)member->host.append('@');
                                        (void)member->host.append(m.param(1));
                                }
                } else if (m.is("INVITE") && m.paramCount >= 2) {
                        if (ignored(src, now)) return;
                        const Text channel = m.param(1);
                        Event      e = make(Event::Kind::Invite, 0, "", src.nick, channel, timeOf(m, now));
                        if (_config.joinInvited && isName(channel) && _isupport.isChannel(channel) &&
                            findChannel(channel) == nullptr) {
                                (void)e.extra.assign("joining");
                                const Text p[] = {channel};
                                send(Kind::Protocol, "JOIN", p, 1, false);
                        }
                        emit(std::move(e));
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
                _identifying = false;
                _joinAt = -1;
                // Perform first (devplan/irc.md §7f): the app runs its lines, then performed().
                if (!_config.perform.isEmpty() && !_performDone) {
                        if (!_performAsked) emit(make(Event::Kind::Perform, Event::Server, "", "", "", 0));
                        _performAsked = true;
                        return;
                }
                _joinsDone = true;
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

        void Session::performed() {
                _performDone = true;
                if (_performAsked && _state == State::Registered) joinAll();
                return;
        }

        // ---- Watching nicks (devplan/irc.md §7g): MONITOR, else ISON each minute

        void Session::monitor(char sign, const Vector<String> &nicks) {
                // MONITOR + a,b,c in lines of at most 400 bytes of nicks.
                String     list;
                const char s[2] = {sign, '\0'};
                for (size_t i = 0; i <= nicks.size(); ++i) {
                        const bool flush = i == nicks.size() || list.size() + nicks[i].size() + 1 > 400;
                        if (flush && !list.isEmpty()) {
                                const Text p[] = {s, list};
                                send(Kind::Protocol, "MONITOR", p, 2, false);
                                list.clear();
                        }
                        if (i == nicks.size()) break;
                        if (!list.isEmpty()) (void)list.append(',');
                        (void)list.append(nicks[i]);
                }
                return;
        }

        void Session::startWatching(const Now &now) {
                _watching = true;
                if (_watched.isEmpty()) return;
                if (_isupport.monitor() >= 0) {
                        Vector<String> nicks;
                        const size_t   most = _isupport.monitor() > 0 ? size_t(_isupport.monitor()) : SIZE_MAX;
                        for (const Watched &w : _watched) {
                                if (nicks.size() >= most) break; // past the server's limit: not watched
                                String n;
                                if (n.assign(w.nick) == Error::Ok) (void)nicks.append(std::move(n));
                        }
                        monitor('+', nicks);
                } else {
                        sendIson(now);
                }
                return;
        }

        void Session::sendIson(const Now &now) {
                _isonAt = -1;
                if (_watched.isEmpty() || _state != State::Registered) return;
                // One line: the nicks that fit in it (an ISON answer names only who is on).
                String list;
                _isonAsked.clear();
                for (const Watched &w : _watched) {
                        if (list.size() + w.nick.size() + 1 > 400) break;
                        if (!list.isEmpty()) (void)list.append(' ');
                        (void)list.append(w.nick);
                        String n;
                        if (n.assign(w.nick) == Error::Ok) (void)_isonAsked.append(std::move(n));
                }
                const Text p[] = {list};
                send(Kind::Protocol, "ISON", p, 1, true);
                _isonAt = now.mono + IsonEveryMs;
                return;
        }

        void Session::setOnline(Text nick, bool on, Text host, const Now &now) {
                for (Watched &w : _watched) {
                        if (!sameName(w.nick, nick, _isupport.caseMapping())) continue;
                        const int8_t was = w.state;
                        w.state = on ? 1 : 0;
                        // On: said when it wasn't; off: only after being seen on.
                        if (on && was != 1) {
                                Event e = make(Event::Kind::Online, Event::Server, "", w.nick, "", now.wall);
                                (void)e.extra.assign(host);
                                emit(std::move(e));
                        } else if (!on && was == 1) {
                                emit(make(Event::Kind::Offline, Event::Server, "", w.nick, "", now.wall));
                        }
                        return;
                }
                return;
        }

        Error Session::watch(Text nick) {
                if (!isName(nick) || nick[0] == ':' || nick.find('!') != Text::NoPos) return Error::Malformed;
                for (const Watched &w : _watched)
                        if (sameName(w.nick, nick, _isupport.caseMapping())) return Error::Ok;
                Watched w;
                if (w.nick.assign(nick) != Error::Ok || _watched.append(std::move(w)) != Error::Ok) return Error::NoMem;
                if (!_watching || _state != State::Registered) return Error::Ok;
                if (_isupport.monitor() >= 0) {
                        if (_isupport.monitor() > 0 && _watched.size() > size_t(_isupport.monitor()))
                                return Error::Ok; // past the limit: not watched here
                        Vector<String> one;
                        String         n;
                        if (n.assign(nick) != Error::Ok || one.append(std::move(n)) != Error::Ok) return Error::NoMem;
                        monitor('+', one);
                } else if (_isonAt < 0) {
                        _isonAt = 0; // the next tick asks
                }
                return Error::Ok;
        }

        void Session::unwatch(Text nick) {
                for (size_t i = 0; i < _watched.size(); ++i) {
                        if (!sameName(_watched[i].nick, nick, _isupport.caseMapping())) continue;
                        if (_watching && _state == State::Registered && _isupport.monitor() >= 0) {
                                Vector<String> one;
                                String         n;
                                if (n.assign(_watched[i].nick) == Error::Ok && one.append(std::move(n)) == Error::Ok)
                                        monitor('-', one);
                        }
                        _watched.removeAt(i);
                        if (_watched.isEmpty()) _isonAt = -1;
                        return;
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
                if (!service && ignored(src, now)) return; // CTCP unanswered too
                if (notice && src.nick.equalsIgnoringCase("NickServ")) {
                        // NickServ answered: the joins wait no longer; a ghosted nick is ours to take.
                        if (_identifying) joinAll();
                        if (_regainAt >= 0) _regainAt = now.mono;
                }

                uint8_t      flags = fromMe ? uint8_t(Event::Self) : uint8_t(0);
                Text         body = text;
                Format::Ctcp ctcp;
                if (Format::parseCtcp(text, ctcp)) {
                        if (notice) {
                                // An answer to our request: shown; a PING's with its delay (our clock sent).
                                if (fromMe) return;
                                String answer; // as any text: made UTF-8 (Latin-1 read as such)
                                (void)answer.assign(ctcp.command);
                                if (!ctcp.args.isEmpty()) {
                                        (void)answer.append(' ');
                                        (void)answer.append(ctcp.args);
                                }
                                Event         e = make(Event::Kind::CtcpReply, 0, "", src.nick, answer, t);
                                const int64_t sent = decimal(ctcp.args);
                                if (ctcp.command.equalsIgnoringCase("PING") && sent >= 0 && sent <= now.mono)
                                        e.number = now.mono - sent;
                                emit(std::move(e));
                                return;
                        }
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
                        c->topicBy.clear();
                        c->topicAt = 0;
                        c->modes.clear();
                        c->namesDone = true;
                        (void)_selfUser.assign(src.user);
                        (void)_selfHost.assign(src.host);
                }
                if (c != nullptr) {
                        addMember(*c, src.nick);
                        if (Member *member = findMember(*c, src.nick); member != nullptr && !src.host.isEmpty()) {
                                (void)member->host.assign(src.user);
                                (void)member->host.append('@');
                                (void)member->host.append(src.host);
                        }
                }
                // extended-join: JOIN #chan account :real name
                Event e = make(Event::Kind::Join, me ? Event::Self : 0, name, src.nick, m.param(2), timeOf(m, now));
                if (m.paramCount >= 3 && m.param(1) != Text("*")) (void)e.extra.assign(m.param(1));
                emit(std::move(e));
                if (c != nullptr && !me) emit(make(Event::Kind::Members, 0, name, "", "", now.wall));
                return;
        }

        // Kicked: back in after a moment, its key kept, once a minute at most (devplan/irc.md §7l).
        void Session::scheduleRejoin(const Channel &c, const Now &now) {
                Rejoin *r = nullptr;
                for (Rejoin &k : _rejoins)
                        if (sameName(k.name, c.name, _isupport.caseMapping())) r = &k;
                if (r != nullptr && r->last >= 0 && now.mono - r->last < RejoinOnceMs) return;
                if (r == nullptr) {
                        Rejoin fresh;
                        if (fresh.name.assign(c.name) != Error::Ok || _rejoins.append(std::move(fresh)) != Error::Ok)
                                return;
                        r = &_rejoins.last();
                }
                r->key.clear();
                for (const ChannelMode &mode : c.modes)
                        if (mode.mode == 'k') (void)r->key.assign(mode.param);
                if (r->key.isEmpty())
                        for (const String &entry : _config.autojoin) {
                                const Text   e = entry;
                                const size_t space = e.find(' ');
                                if (space != Text::NoPos && sameName(e.left(space), c.name, _isupport.caseMapping()))
                                        (void)r->key.assign(e.mid(space + 1));
                        }
                r->at = now.mono + RejoinAfterMs;
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
                        const bool takes = _isupport.takesParameter(mode, setting);
                        const Text param = takes ? m.param(next++) : Text();
                        const char prefix = _isupport.prefixFor(mode);
                        if (prefix == 0 && !_isupport.isListMode(mode)) {
                                setChannelMode(*c, mode, setting, param); // n, t, k key, l 20...
                                continue;
                        }
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
                        case 333: // RPL_TOPICWHOTIME nick #chan setter time
                                if (Channel *c = findChannel(m.param(1)); c != nullptr) {
                                        const Text   setter = m.param(2);
                                        const size_t bang = setter.find('!');
                                        (void)c->topicBy.assign(bang == Text::NoPos ? setter : setter.left(bang));
                                        const int64_t at = decimal(m.param(3));
                                        c->topicAt = at >= 0 ? at * 1000 : 0;
                                }
                                return;
                        case 324: // RPL_CHANNELMODEIS nick #chan +nt-... params
                                if (Channel *c = findChannel(m.param(1)); c != nullptr) {
                                        c->modes.clear();
                                        size_t next = 3;
                                        for (char mode : m.param(2)) {
                                                if (mode == '+' || mode == '-') continue;
                                                const bool takes = _isupport.takesParameter(mode, true);
                                                setChannelMode(*c, mode, true, takes ? m.param(next++) : Text());
                                        }
                                        emit(make(Event::Kind::ChannelModes, 0, c->name, "", "", now.wall));
                                }
                                return;
                        case 329: return; // RPL_CREATIONTIME
                        case 321: emit(make(Event::Kind::ListStart, Event::Server, "", "", "", now.wall)); return;
                        case 322: { // RPL_LIST nick #chan users :topic
                                Event e = make(Event::Kind::ListEntry, Event::Server, m.param(1), "", m.param(3),
                                               now.wall);
                                const int64_t users = decimal(m.param(2));
                                e.number = users >= 0 ? users : 0;
                                emit(std::move(e));
                                return;
                        }
                        case 323: emit(make(Event::Kind::ListEnd, Event::Server, "", "", "", now.wall)); return;
                        case 263: // RPL_TRYAGAIN nick LIST :Please wait a while and try again.
                        case 416: // ERR_TOOMANYMATCHES nick LIST :Output too large
                                if (m.param(1).equalsIgnoringCase("LIST")) {
                                        emit(make(Event::Kind::ListEnd, Event::Server, "", "", m.last(), now.wall));
                                        return;
                                }
                                break;
                        case 367:   // RPL_BANLIST nick #chan mask [setter time]
                        case 348:   // RPL_EXCEPTLIST
                        case 346: { // RPL_INVITELIST
                                Event e = make(Event::Kind::ModeListEntry, Event::Server, m.param(1), m.param(3),
                                               m.param(2), now.wall);
                                (void)e.extra.assign(numeric == 367 ? "b" : numeric == 348 ? "e" : "I");
                                const size_t bang = e.nick.view().find('!');
                                if (bang != Text::NoPos) {
                                        String nick;
                                        (void)nick.assign(e.nick.view().left(bang));
                                        e.nick = std::move(nick);
                                }
                                const int64_t at = decimal(m.param(4));
                                e.number = at >= 0 ? at : 0;
                                emit(std::move(e));
                                return;
                        }
                        case 368: // RPL_ENDOFBANLIST nick #chan :End
                        case 349:
                        case 347: {
                                Event e = make(Event::Kind::ModeListEnd, Event::Server, m.param(1), "", "", now.wall);
                                (void)e.extra.assign(numeric == 368 ? "b" : numeric == 349 ? "e" : "I");
                                emit(std::move(e));
                                return;
                        }
                        case 730:   // RPL_MONONLINE nick :a!u@h,b!u@h
                        case 731: { // RPL_MONOFFLINE nick :a,b
                                Text list = m.last();
                                while (!list.isEmpty()) {
                                        size_t comma = list.find(',');
                                        if (comma == Text::NoPos) comma = list.size();
                                        const Text   item = list.left(comma);
                                        const size_t bang = item.find('!');
                                        setOnline(bang == Text::NoPos ? item : item.left(bang), numeric == 730,
                                                  bang == Text::NoPos ? Text() : item.mid(bang + 1), now);
                                        list = list.mid(comma + 1);
                                }
                                return;
                        }
                        case 732: // RPL_MONLIST, RPL_ENDOFMONLIST
                        case 733: return;
                        case 303: {                              // RPL_ISON nick :a b
                                if (_isonAsked.isEmpty()) break; // a /ison typed: shown
                                Vector<String> asked = std::move(_isonAsked);
                                _isonAsked.clear();
                                for (const String &nick : asked) {
                                        bool on = false;
                                        eachWord(m.last(), [&](Text w) {
                                                if (sameName(w, nick, _isupport.caseMapping())) on = true;
                                        });
                                        setOnline(nick, on, Text(), now);
                                }
                                return;
                        }
                        case 376: // RPL_ENDOFMOTD, ERR_NOMOTD: registration's burst done, ISUPPORT known
                        case 422:
                                if (!_watching) startWatching(now);
                                break;
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
                send(Kind::User, "PART", p, reason.isEmpty() ? 1 : 2, !reason.isEmpty());
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

        Error Session::ctcp(Text target, Text command, Text args, const Now &now) {
                if (!isName(target) || !isName(command) || hasBreak(args)) return Error::Malformed;
                String body;
                (void)body.append('\x01');
                for (char c : command) (void)body.append(c >= 'a' && c <= 'z' ? char(c - 32) : c);
                if (!args.isEmpty() || command.equalsIgnoringCase("PING")) {
                        (void)body.append(' ');
                        if (args.isEmpty())
                                appendDecimal(body, now.mono); // its answer's delay (CtcpReply's number)
                        else
                                (void)body.append(args);
                }
                if (body.append('\x01') != Error::Ok) return Error::NoMem;
                if (body.size() > 400) return Error::TooLong;
                const Text p[] = {target, body};
                send(Kind::User, "PRIVMSG", p, 2, true);
                return Error::Ok;
        }

        Error Session::list(uint32_t minUsers, uint32_t maxUsers) {
                // ELIST U: >n (more than n users), <n (fewer); the rest the caller filters.
                String     conditions;
                const bool users =
                        _isupport.elist().find('U') != Text::NoPos || _isupport.elist().find('u') != Text::NoPos;
                if (users && minUsers > 1) {
                        (void)conditions.append('>');
                        appendDecimal(conditions, int64_t(minUsers) - 1);
                }
                if (users && maxUsers > 0) {
                        if (!conditions.isEmpty()) (void)conditions.append(',');
                        (void)conditions.append('<');
                        appendDecimal(conditions, int64_t(maxUsers) + 1);
                }
                const Text p[] = {conditions};
                send(Kind::User, "LIST", p, conditions.isEmpty() ? 0 : 1, false);
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
