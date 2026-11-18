/**
 * @file      buffers.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "buffers.h"

#include "format.h"

#include <cstdio>

namespace irc {

        namespace {

                using Kind = Event::Kind;

                // @p n as two digits.
                void two(String &out, int n) {
                        const char d[2] = {char('0' + n / 10 % 10), char('0' + n % 10)};
                        (void)out.append(Text(d, 2));
                        return;
                }

                // A nick in its colour, the colour ended after it; ours plain (not bold: ChatView starts a
                // run in another font on a 4-pixel column, which leaves gaps round a short nick).
                void colouredNick(String &out, Text nick, bool self, CaseMapping mapping) {
                        if (self) {
                                (void)out.append(nick);
                                return;
                        }
                        (void)out.append('\x03');
                        two(out, Format::nickColour(nick, mapping));
                        (void)out.append(nick);
                        (void)out.append('\x03');
                        return;
                }

        } // namespace

        Buffers::Buffers() {
                (void)make(Buffer::Type::Network, "");
        }

        Buffer *Buffers::make(Buffer::Type type, Text name) {
                Buffer b;
                b.id = _nextId++;
                b.type = type;
                if (b.name.assign(name) != Error::Ok || _buffers.append(std::move(b)) != Error::Ok) return nullptr;
                return &_buffers.last();
        }

        Buffer *Buffers::byId(uint32_t id) {
                for (Buffer &b : _buffers)
                        if (b.id == id) return &b;
                return nullptr;
        }

        const Buffer *Buffers::byId(uint32_t id) const {
                return const_cast<Buffers *>(this)->byId(id);
        }

        Buffer *Buffers::find(Text name, CaseMapping mapping) {
                if (name.isEmpty()) return nullptr;
                for (Buffer &b : _buffers)
                        if (b.type != Buffer::Type::Network && sameName(b.name, name, mapping)) return &b;
                return nullptr;
        }

        Buffer *Buffers::open(Buffer::Type type, Text name, CaseMapping mapping) {
                if (type == Buffer::Type::Network) return &_buffers[0];
                if (Buffer *b = find(name, mapping); b != nullptr) return b;
                return make(type, name);
        }

        void Buffers::close(uint32_t id) {
                for (size_t i = 1; i < _buffers.size(); ++i)
                        if (_buffers[i].id == id) {
                                _buffers.removeAt(i);
                                if (_active == id) _active = 0;
                                return;
                        }
                return;
        }

        void Buffers::disconnected() {
                for (Buffer &b : _buffers) b.joined = false;
                return;
        }

        void Buffers::seen(Buffer &b) {
                b.unread = 0;
                b.mentions = 0;
                b.attention = 0;
                return;
        }

        void Buffers::setActive(uint32_t id, bool focused) {
                _active = id;
                _focused = focused;
                if (Buffer *b = byId(id); b != nullptr && focused) seen(*b);
                return;
        }

        bool Buffers::mentioned(Text text, const Session &s) const {
                const CaseMapping mapping = s.isupport().caseMapping();
                if (Format::mentions(text, s.nick(), mapping)) return true;
                for (const String &word : _settings.highlights)
                        if (Format::mentions(text, word, CaseMapping::Ascii)) return true;
                return false;
        }

        void Buffers::spoke(Buffer &b, Text nick, CaseMapping mapping) {
                for (size_t i = 0; i < b.speakers.size(); ++i)
                        if (sameName(b.speakers[i], nick, mapping)) {
                                b.speakers.removeAt(i);
                                break;
                        }
                String s;
                if (s.assign(nick) != Error::Ok) return;
                (void)b.speakers.insert(0, std::move(s));
                if (b.speakers.size() > MaxSpeakers) b.speakers.removeAt(b.speakers.size() - 1);
                return;
        }

        Error Buffers::add(Buffer &b, const Event &e, uint8_t level, Text marker, Text text, Vector<Line> &out) {
                const bool              self = (e.flags & Event::Self) != 0;
                const bool              inView = b.id == _active && _focused;
                const Format::LocalTime t = Format::localTime(e.time, _settings.utcOffsetMinutes);
                Line                    line;
                line.buffer = b.id;
                line.time = e.time;
                line.level = level;
                // The view's: HH:MM marker, a tab, the text (the marker's codes ended before it).
                two(line.view, t.hour);
                (void)line.view.append(':');
                two(line.view, t.minute);
                (void)line.view.append(' ');
                (void)line.view.append(marker);
                (void)line.view.append("\x0f\t");
                if (line.view.append(text) != Error::Ok) return Error::NoMem;
                // The log's: HH:MM:SS marker text, plain.
                String plain;
                two(line.log, t.hour);
                (void)line.log.append(':');
                two(line.log, t.minute);
                (void)line.log.append(':');
                two(line.log, t.second);
                (void)line.log.append(' ');
                if (Format::strip(marker, plain) != Error::Ok || line.log.append(plain) != Error::Ok)
                        return Error::NoMem;
                (void)line.log.append(' ');
                if (Format::strip(text, plain) != Error::Ok || line.log.append(plain) != Error::Ok) return Error::NoMem;
                // Not in view: counted, marked; a mention or private message told.
                if (level >= 1 && !self && !inView) {
                        ++b.unread;
                        if (level == 2) ++b.mentions;
                        const uint8_t attention = level == 2 ? 2 : 1;
                        if (b.attention < attention) b.attention = attention;
                        line.notify = level == 2;
                }
                return out.append(std::move(line));
        }

        Error Buffers::note(uint32_t id, Text text, int64_t time, Vector<Line> &out) {
                Buffer *b = byId(id);
                if (b == nullptr) b = &network();
                Event e;
                e.time = time;
                e.flags = Event::Self;
                return add(*b, e, 0, "--", text, out);
        }

        Error Buffers::apply(const Event &e, const Session &s, Vector<Line> &out) {
                const CaseMapping mapping = s.isupport().caseMapping();
                const bool        self = (e.flags & Event::Self) != 0;
                const bool        query = (e.flags & Event::Query) != 0;
                String            marker, text;
                Buffer           *b = nullptr;
                uint8_t           level = 0;
                switch (e.kind) {
                        case Kind::Registered:
                                (void)text.assign("Connected as ");
                                (void)text.append(e.text);
                                return add(network(), e, 0, "--", text, out);

                        case Kind::Message:
                        case Kind::Notice: {
                                const bool service = (e.flags & (Event::Service | Event::Server)) != 0;
                                if (e.target.isEmpty()) {
                                        // A notice to us: a service's or the server's to the network's buffer,
                                        // a person's to the one in view (devplan/irc.md §7b).
                                        b = service ? &network() : byId(_active);
                                        if (b == nullptr) b = &network();
                                } else {
                                        b = open(query ? Buffer::Type::Query : Buffer::Type::Channel, e.target,
                                                 mapping);
                                }
                                if (b == nullptr) return Error::NoMem;
                                // A password typed to NickServ, or repeated by a service, hidden in the view and
                                // the log.
                                String hidden;
                                Text   body = self && Format::hideSecret(e.target, e.text, hidden) ? hidden.view()
                                                                                                   : e.text.view();
                                String repeated;
                                if (service && !_settings.secrets.isEmpty()) {
                                        (void)repeated.assign(body);
                                        for (const String &secret : _settings.secrets) {
                                                if (secret.size() < 4) continue; // too short to look for
                                                String     out;
                                                const Text t = repeated;
                                                size_t     at = 0;
                                                for (size_t i = 0; i + secret.size() <= t.size();) {
                                                        if (t.mid(i, secret.size()) == secret.view()) {
                                                                (void)out.append(t.mid(at, i - at));
                                                                (void)out.append("****");
                                                                i += secret.size();
                                                                at = i;
                                                        } else {
                                                                ++i;
                                                        }
                                                }
                                                (void)out.append(t.mid(at));
                                                repeated = std::move(out);
                                        }
                                        body = repeated;
                                }
                                // A service's or the server's text naming us ("identified for jth") isn't a mention.
                                const bool mention = !self && !service && mentioned(body, s);
                                // Private (a chat's, or a person's notice to us) or a mention: an alert.
                                const bool personal = !service && (query || e.target.isEmpty());
                                level = self ? 1 : (mention || personal) ? 2 : 1;
                                if (e.kind == Kind::Notice && (e.flags & Event::Server) != 0) {
                                        // The server's: "--", not its name (a long one widens the gutter
                                        // every line lines up at: ChatView's widest).
                                        (void)marker.assign("--");
                                        (void)text.assign(body);
                                } else if (e.kind == Kind::Notice) {
                                        (void)marker.append('-');
                                        (void)marker.append(e.nick.isEmpty() ? Text("server") : e.nick.view());
                                        (void)marker.append('-');
                                        (void)text.assign(body);
                                } else if ((e.flags & Event::Action) != 0) {
                                        (void)marker.assign(mention ? "\x16*\x16" : "*");
                                        colouredNick(text, e.nick, self, mapping);
                                        (void)text.append(' ');
                                        (void)text.append(body);
                                } else {
                                        if (mention) (void)marker.append('\x16');
                                        (void)marker.append('<');
                                        colouredNick(marker, e.nick, self, mapping);
                                        (void)marker.append('>');
                                        if (mention) (void)marker.append('\x16');
                                        (void)text.assign(body);
                                }
                                if (b->type == Buffer::Type::Channel && !self && e.kind == Kind::Message)
                                        spoke(*b, e.nick, mapping);
                                return add(*b, e, level, marker, text, out);
                        }

                        case Kind::Join:
                                b = open(Buffer::Type::Channel, e.target, mapping);
                                if (b == nullptr) return Error::NoMem;
                                if (self) {
                                        b->joined = true;
                                        (void)text.assign("You joined ");
                                        (void)text.append(e.target);
                                } else {
                                        if (!_settings.showJoins) return Error::Ok;
                                        colouredNick(text, e.nick, false, mapping);
                                        if (!e.extra.isEmpty()) {
                                                (void)text.append(" (");
                                                (void)text.append(e.extra);
                                                (void)text.append(')');
                                        }
                                        (void)text.append(" joined");
                                }
                                return add(*b, e, 0, "-->", text, out);

                        case Kind::Part:
                        case Kind::Kick:
                                b = find(e.target, mapping);
                                if (b == nullptr) return Error::Ok;
                                if (self) b->joined = false;
                                if (!self && !_settings.showJoins && e.kind == Kind::Part) return Error::Ok;
                                if (self && e.kind == Kind::Part) {
                                        (void)text.assign("You left");
                                } else {
                                        colouredNick(text, e.nick, self, mapping);
                                        if (e.kind == Kind::Part) {
                                                (void)text.append(" left");
                                        } else {
                                                (void)text.append(" was kicked by ");
                                                (void)text.append(e.extra);
                                        }
                                }
                                if (!e.text.isEmpty()) {
                                        (void)text.append(" (");
                                        (void)text.append(e.text);
                                        (void)text.append(')');
                                }
                                return add(*b, e, 0, "<--", text, out);

                        case Kind::Quit:
                                b = find(e.target, mapping);
                                if (b == nullptr || (query && b->type != Buffer::Type::Query)) return Error::Ok;
                                if (!_settings.showJoins && !query) return Error::Ok;
                                colouredNick(text, e.nick, false, mapping);
                                (void)text.append(" quit");
                                if (!e.text.isEmpty()) {
                                        (void)text.append(" (");
                                        (void)text.append(e.text);
                                        (void)text.append(')');
                                }
                                return add(*b, e, 0, "<--", text, out);

                        case Kind::Nick: {
                                b = find(e.target, mapping);
                                if (b == nullptr || (query && b->type != Buffer::Type::Query)) return Error::Ok;
                                // A private chat follows the person to their new nick.
                                if (query) (void)b->name.assign(e.text);
                                for (String &speaker : b->speakers)
                                        if (sameName(speaker, e.nick, mapping)) (void)speaker.assign(e.text);
                                colouredNick(text, e.nick, self, mapping);
                                (void)text.append(" is now known as ");
                                colouredNick(text, e.text, self, mapping);
                                return add(*b, e, 0, "--", text, out);
                        }

                        case Kind::Topic:
                                b = open(Buffer::Type::Channel, e.target, mapping);
                                if (b == nullptr) return Error::NoMem;
                                if (e.nick.isEmpty()) {
                                        (void)text.assign("Topic: ");
                                } else {
                                        colouredNick(text, e.nick, self, mapping);
                                        (void)text.append(" set the topic: ");
                                }
                                (void)text.append(e.text);
                                return add(*b, e, 0, "--", text, out);

                        case Kind::Mode:
                                b = e.target.isEmpty() ? &network() : find(e.target, mapping);
                                if (b == nullptr) b = &network();
                                colouredNick(text, e.nick.isEmpty() ? Text("server") : e.nick.view(), self, mapping);
                                (void)text.append(" sets mode ");
                                (void)text.append(e.text);
                                return add(*b, e, 0, "--", text, out);

                        case Kind::ServerText: return add(network(), e, 0, "--", e.text, out);

                        case Kind::LoggedIn:
                                (void)text.assign("Logged in as ");
                                (void)text.append(e.text);
                                return add(network(), e, 0, "--", text, out);

                        case Kind::LoginFailed:
                                (void)text.assign("Login failed: ");
                                (void)text.append(e.text);
                                return add(network(), e, 0, "--", text, out);

                        case Kind::Error:
                                (void)text.assign("Disconnected: ");
                                (void)text.append(e.text);
                                return add(network(), e, 0, "--", text, out);

                        case Kind::Timeout:
                                return add(network(), e, 0, "--", "No answer from the server: reconnecting", out);

                        case Kind::Invite:
                                colouredNick(text, e.nick, false, mapping);
                                (void)text.append(" invited you to ");
                                (void)text.append(e.text);
                                if (!e.extra.isEmpty()) (void)text.append(" (joining)");
                                return add(network(), e, 1, "--", text, out);

                        case Kind::Online:
                        case Kind::Offline: {
                                // The network's buffer, and their private chat if one is open.
                                colouredNick(text, e.nick, false, mapping);
                                (void)text.append(e.kind == Kind::Online ? " is online" : " is offline");
                                if (!e.extra.isEmpty()) {
                                        (void)text.append(" (");
                                        (void)text.append(e.extra);
                                        (void)text.append(')');
                                }
                                Buffer *chat = find(e.nick, mapping);
                                if (chat != nullptr && chat->type == Buffer::Type::Query) {
                                        const Error err = add(*chat, e, 0, "--", text, out);
                                        if (err != Error::Ok) return err;
                                }
                                return add(network(), e, 0, "--", text, out);
                        }

                        case Kind::CtcpReply: {
                                // Where it was asked from (the buffer in view): "alice's VERSION: ...".
                                b = byId(_active);
                                if (b == nullptr) b = &network();
                                colouredNick(text, e.nick, false, mapping);
                                (void)text.append("'s ");
                                const Text   t = e.text;
                                const size_t space = t.find(' ');
                                (void)text.append(space == Text::NoPos ? t : t.left(space));
                                if (e.number > 0 || t.left(space).equalsIgnoringCase("PING")) {
                                        char ms[32];
                                        snprintf(ms, sizeof(ms), ": %lld ms", (long long)e.number);
                                        (void)text.append(ms);
                                } else if (space != Text::NoPos) {
                                        (void)text.append(": ");
                                        (void)text.append(t.mid(space + 1));
                                }
                                return add(*b, e, 0, "--", text, out);
                        }

                        case Kind::Ignored: {
                                char said[64];
                                snprintf(said, sizeof(said), "%lld message%s from people ignored, not shown",
                                         (long long)e.number, e.number == 1 ? "" : "s");
                                return add(network(), e, 0, "--", said, out);
                        }

                        case Kind::ListEnd:
                                // The list itself is the dialog's; a refusal said here too.
                                if (e.text.isEmpty()) return Error::Ok;
                                (void)text.assign("The channel list: ");
                                (void)text.append(e.text);
                                return add(network(), e, 0, "--", text, out);

                        case Kind::ModeListEntry: {
                                // A list asked for by hand (/mode #c b); Channel Central takes them itself.
                                b = find(e.target, mapping);
                                if (b == nullptr) b = &network();
                                const char mode = e.extra.isEmpty() ? 'b' : e.extra.view()[0];
                                (void)text.append(e.target);
                                (void)text.append(mode == 'e' ? " exception: " : mode == 'I' ? " invite: " : " ban: ");
                                (void)text.append(e.text);
                                if (!e.nick.isEmpty()) {
                                        (void)text.append(" (by ");
                                        (void)text.append(e.nick);
                                        (void)text.append(')');
                                }
                                return add(*b, e, 0, "--", text, out);
                        }

                        case Kind::Members:
                        case Kind::Lag:
                        case Kind::Perform:
                        case Kind::ListStart:
                        case Kind::ListEntry:
                        case Kind::ModeListEnd:
                        case Kind::ChannelModes: return Error::Ok;
                }
                return Error::Ok;
        }

} // namespace irc
