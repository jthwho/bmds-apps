/**
 * @file      actions.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What step 5 adds around the dialogs (devplan/irc.md §7f-§7l): commands run in a given
 * conversation and what they leave to the app (/list /find /ignore /notify /server), the
 * options for kicks and invitations, watching people, Find, a paste of many lines, and the
 * events routed to the dialog open for them.
 */

#include "app.h"

#include "format.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

namespace irc {

        void App::runIn(uint32_t n, uint32_t buffer, Text command) {
                if (n >= _networks.size()) return;
                Network     &net = *_networks[n];
                Vector<Line> lines;
                Outcome      o;
                (void)runCommand(command, net.session, net.buffers, buffer, now(), lines, o);
                show(n, lines);
                switch (o.action) {
                        case Outcome::Action::None: break;
                        case Outcome::Action::Show:
                                syncPages(n);
                                for (size_t i = 0; i < _pages.size(); ++i)
                                        if (_pages[i].network == n && _pages[i].buffer == o.buffer) setCurrent(i, true);
                                break;
                        case Outcome::Action::Clear:
                                if (Page *pg = page(n, o.buffer, false); pg != nullptr)
                                        bmds_chat_view_clear(_gui, pg->chat);
                                break;
                        case Outcome::Action::Close:
                                for (size_t i = 0; i < _pages.size(); ++i)
                                        if (_pages[i].network == n && _pages[i].buffer == o.buffer) {
                                                closePage(i);
                                                break;
                                        }
                                break;
                        case Outcome::Action::Quit: net.wanted = false; break;
                        default: fulfil(n, buffer, o); break;
                }
                pump(n);
                return;
        }

        void App::applyBehaviour(Network &net) {
                // The Channel menu's options (§7l) and the ignore list (§7f), each network's session's.
                net.settings.session.rejoin = _settings.rejoin;
                net.settings.session.joinInvited = _settings.joinInvited;
                NetworkSettings copy;
                if (copySettings(net.settings, copy) == Error::Ok) net.session.setConfig(std::move(copy.session));
                net.session.setIgnores(&_ignores, net.settings.name);
                return;
        }

        void App::watchAll(uint32_t n) {
                // The notify list's people, and those of the private chats open (§7g, §7b).
                Network &net = *_networks[n];
                for (const String &nick : net.settings.notify) (void)net.session.watch(nick);
                for (size_t i = 0; i < net.buffers.count(); ++i)
                        if (net.buffers.at(i).type == Buffer::Type::Query)
                                (void)net.session.watch(net.buffers.at(i).name);
                return;
        }

        void App::fulfil(uint32_t n, uint32_t buffer, Outcome &o) {
                Network     &net = *_networks[n];
                Vector<Line> lines;
                const auto   say = [&](Text text) {
                        (void)net.buffers.note(buffer, text, now().wall, lines);
                        show(n, lines);
                };
                switch (o.action) {
                        case Outcome::Action::List: openChannelList(n, o.text); return;
                        case Outcome::Action::Find: openFind(o.text); return;
                        case Outcome::Action::Ignore: {
                                if (o.text.isEmpty()) {
                                        if (_ignores.size() == 0) say("No one is ignored.");
                                        for (size_t i = 0; i < _ignores.size(); ++i) {
                                                String line;
                                                (void)line.assign("Ignored: ");
                                                (void)line.append(_ignores.maskAt(i));
                                                if (!_ignores.networkAt(i).isEmpty()) {
                                                        (void)line.append(" (on ");
                                                        (void)line.append(_ignores.networkAt(i));
                                                        (void)line.append(')');
                                                }
                                                say(line);
                                        }
                                        return;
                                }
                                const bool done = o.on ? _ignores.add(o.text, o.extra) == Error::Ok
                                                       : _ignores.remove(o.text, o.extra);
                                String     line;
                                (void)line.assign(!done ? "Not ignored: " : o.on ? "Ignoring " : "No longer ignoring ");
                                (void)line.append(o.text);
                                say(line);
                                // Kept: settings.ini's list made from the one in use.
                                _settings.ignores.clear();
                                for (size_t i = 0; i < _ignores.size(); ++i) {
                                        String m;
                                        (void)m.assign(_ignores.maskAt(i));
                                        if (!_ignores.networkAt(i).isEmpty()) {
                                                (void)m.append('@');
                                                (void)m.append(_ignores.networkAt(i));
                                        }
                                        (void)_settings.ignores.append(std::move(m));
                                }
                                saveSettings();
                                return;
                        }
                        case Outcome::Action::Notify: {
                                const CaseMapping mapping = net.session.isupport().caseMapping();
                                if (o.text.isEmpty()) {
                                        if (net.settings.notify.isEmpty()) say("No one on the notify list here.");
                                        for (const String &nick : net.settings.notify) {
                                                const int on = net.session.online(nick);
                                                String    line;
                                                (void)line.assign(nick);
                                                (void)line.append(on == 1   ? ": online"
                                                                  : on == 0 ? ": offline"
                                                                            : ": not known yet");
                                                say(line);
                                        }
                                        return;
                                }
                                size_t at = net.settings.notify.size();
                                for (size_t i = 0; i < net.settings.notify.size(); ++i)
                                        if (sameName(net.settings.notify[i], o.text, mapping)) at = i;
                                if (o.on && at == net.settings.notify.size()) {
                                        String nick;
                                        (void)nick.assign(o.text);
                                        (void)net.settings.notify.append(std::move(nick));
                                        (void)net.session.watch(o.text);
                                } else if (!o.on && at < net.settings.notify.size()) {
                                        net.settings.notify.removeAt(at);
                                        if (net.buffers.find(o.text, mapping) == nullptr) net.session.unwatch(o.text);
                                }
                                String line;
                                (void)line.assign(o.on ? "On the notify list: " : "Off the notify list: ");
                                (void)line.append(o.text);
                                say(line);
                                saveNetworks();
                                if (_ntf.window != 0) listNotify();
                                pump(n);
                                return;
                        }
                        case Outcome::Action::Server: {
                                if (o.fresh) {
                                        // /server -m: a network of its own, with this one's nick.
                                        NetworkSettings s;
                                        (void)s.name.assign(o.text);
                                        for (int k = 2; k < 100; ++k) {
                                                bool taken = false;
                                                for (const Network *other : _networks)
                                                        taken = taken || (!other->removed &&
                                                                          other->settings.name == s.name.view());
                                                if (!taken) break;
                                                char name[96];
                                                snprintf(name, sizeof(name), "%.*s %d", int(o.text.size()),
                                                         o.text.data(), k);
                                                (void)s.name.assign(name);
                                        }
                                        (void)s.host.assign(o.text);
                                        s.port = o.port;
                                        s.tls = o.on;
                                        s.autoconnect = false;
                                        (void)s.session.nick.assign(net.settings.session.nick);
                                        (void)s.session.user.assign(net.settings.session.user);
                                        (void)s.session.realName.assign(net.settings.session.realName);
                                        (void)s.session.version.assign("BMDS IRC 0.1");
                                        auto *fresh = new (std::nothrow) Network(std::move(s));
                                        if (fresh == nullptr) return;
                                        Buffers::Settings &b = fresh->buffers.settings();
                                        b.showJoins = _settings.showJoins;
                                        b.utcOffsetMinutes = _utcOffset;
                                        for (const String &w : _settings.highlights) {
                                                String c;
                                                if (c.assign(w) == Error::Ok) (void)b.highlights.append(std::move(c));
                                        }
                                        if (_networks.append(std::move(fresh)) != Error::Ok) return;
                                        const uint32_t added = uint32_t(_networks.size() - 1);
                                        applyBehaviour(*_networks[added]);
                                        updateSecrets(added);
                                        syncPages(added);
                                        saveNetworks();
                                        connect(added);
                                        return;
                                }
                                // This network elsewhere: its server changed (and kept), connected again.
                                (void)net.settings.host.assign(o.text);
                                net.settings.port = o.port;
                                net.settings.tls = o.on;
                                net.settings.fingerprint.clear(); // another server's certificate
                                saveNetworks();
                                if (net.connection.state() == Connection::State::Up) {
                                        (void)net.session.quit("Changing server");
                                        pump(n);
                                }
                                net.connection.stop();
                                disconnect(n, "changing server", false);
                                connect(n);
                                return;
                        }
                        default: return;
                }
        }

        // ---- Find (§7j)

        void App::openFind(Text text) {
                bmds_widget_set_visible(_gui, _findRow, 1);
                if (!text.isEmpty()) bmds_line_edit_set_text(_gui, _findEdit, CString(text).c());
                bmds_line_edit_select_all(_gui, _findEdit);
                bmds_widget_set_focus(_gui, _findEdit);
                if (!text.isEmpty()) find(true);
                return;
        }

        void App::find(bool backward) {
                if (_pages.isEmpty() || _current >= _pages.size()) return;
                bmds_gui_value_t v;
                if (bmds_line_edit_get_text(_gui, _findEdit, &v) != 0 || v.length == 0) return;
                const String what = [&] {
                        String s;
                        (void)s.assign(Text(v.text, v.length));
                        return s;
                }();
                bmds_gui_value_t found;
                const bool       ok =
                        bmds_chat_view_find(_gui, _pages[_current].chat, CString(what).c(), backward, &found) == 0 &&
                        found.number != 0;
                char text[160];
                snprintf(text, sizeof(text), ok ? "Found \"%.*s\"" : "\"%.*s\" isn't in this conversation",
                         int(what.size() > 100 ? 100 : what.size()), what.data());
                bmds_status_bar_set_field(_gui, _status, 0, text);
                return;
        }

        // ---- A paste of many lines (§7k)

        void App::pasted(Text text) {
                if (_pages.isEmpty() || _current >= _pages.size()) return;
                const Page &p = _pages[_current];
                if (p.network == NoNetwork) {
                        note("No network to send it to");
                        return;
                }
                const Buffer *b = _networks[p.network]->buffers.byId(p.buffer);
                size_t        count = 0, at = 0;
                while (at < text.size()) {
                        size_t end = at;
                        while (end < text.size() && text[end] != '\n' && text[end] != '\r') ++end;
                        if (end > at) ++count;
                        at = end + 1;
                }
                bmds_gui_t *g = _gui;
                closePaste();
                _pf.network = p.network;
                _pf.buffer = p.buffer;
                _pf.window = bmds_window_new(g, 0);
                bmds_window_set_title(g, _pf.window, "Paste");
                bmds_window_set_owner(g, _pf.window, _window);
                bmds_window_set_modal(g, _pf.window, 1);
                bmds_window_set_width(g, _pf.window, 560);
                bmds_window_set_height(g, _pf.window, 360);
                bmds_window_set_margin(g, _pf.window, 8);
                bmds_window_set_spacing(g, _pf.window, 4);
                char       question[200];
                const Text to = b != nullptr && b->type != Buffer::Type::Network ? b->name.view() : Text("the network");
                snprintf(question, sizeof(question), "Send %u line%s to %.*s? Change them first if you like.",
                         unsigned(count), count == 1 ? "" : "s", int(to.size()), to.data());
                _pf.label = bmds_label_new(g, _pf.window);
                bmds_label_set_text(g, _pf.label, question);
                _pf.text = bmds_text_edit_new(g, _pf.window);
                bmds_widget_set_stretch(g, _pf.text, 1);
                bmds_text_edit_set_text(g, _pf.text, CString(text).c());
                const uint32_t row = bmds_gui_new(g, "Box", _pf.window);
                bmds_box_set_direction(g, row, "across");
                bmds_widget_set_stretch(g, row, 0);
                bmds_widget_set_stretch(g, bmds_gui_new(g, "Spacer", row), 1);
                _pf.send = bmds_button_new(g, row);
                bmds_button_set_text(g, _pf.send, "&Send");
                _pf.cancel = bmds_button_new(g, row);
                bmds_button_set_text(g, _pf.cancel, "Cancel");
                bmds_window_show(g, _pf.window);
                bmds_widget_set_focus(g, _pf.send);
                return;
        }

        void App::sendPaste() {
                bmds_gui_value_t v;
                String           text;
                if (bmds_text_edit_get_text(_gui, _pf.text, &v) == 0) (void)text.assign(Text(v.text, v.length));
                const uint32_t n = _pf.network, buffer = _pf.buffer;
                closePaste();
                if (n >= _networks.size()) return;
                // Each line said, paced by the session; one starting with / is said too, never run.
                const Text t = text;
                size_t     at = 0;
                while (at < t.size()) {
                        size_t end = at;
                        while (end < t.size() && t[end] != '\n' && t[end] != '\r') ++end;
                        const Text line = t.mid(at, end - at);
                        at = end + 1;
                        if (line.isEmpty()) continue;
                        String said;
                        if (line[0] == '/') (void)said.append('/');
                        (void)said.append(line);
                        runIn(n, buffer, said);
                }
                return;
        }

        void App::closePaste() {
                if (_pf.window == 0) return;
                bmds_gui_destroy(_gui, _pf.window);
                _pf = PasteFields();
                return;
        }

        bool App::actionsEvent(const bmds_gui_event_t &e) {
                if (_pf.window == 0) return false;
                if (e.source == _pf.send) {
                        sendPaste();
                        return true;
                }
                if (e.source == _pf.cancel ||
                    (e.source == _pf.window && bmds_gui_event_is(_gui, &e, BMDS_GUI_WINDOW_CLOSED))) {
                        closePaste();
                        return true;
                }
                return false;
        }

        // ---- People watched (§7g)

        void App::sawWatched(uint32_t n, const Event &e) {
                Network &net = *_networks[n];
                for (Network::Seen &s : net.seen)
                        if (sameName(s.nick, e.nick, net.session.isupport().caseMapping())) {
                                s.at = now().wall;
                                s.on = e.kind == Event::Kind::Online;
                                if (_ntf.window != 0) listNotify();
                                return;
                        }
                Network::Seen s;
                (void)s.nick.assign(e.nick);
                s.at = now().wall;
                s.on = e.kind == Event::Kind::Online;
                (void)net.seen.append(std::move(s));
                if (_ntf.window != 0) listNotify();
                return;
        }

        void App::notifyOnline(uint32_t n, const Event &e) {
                // Only the notify list's people (a private chat's partner: the line in it is enough).
                Network &net = *_networks[n];
                bool     listed = false;
                for (const String &nick : net.settings.notify)
                        listed = listed || sameName(nick, e.nick, net.session.isupport().caseMapping());
                if (!listed) return;
                for (size_t i = 0; i < _notes.size(); ++i)
                        if (_notes[i].network == n && _notes[i].nick == e.nick.view()) {
                                bmds_gui_destroy(_gui, _notes[i].id);
                                _notes.removeAt(i);
                                break;
                        }
                char title[160];
                snprintf(title, sizeof(title), "%.*s is on %.*s", int(e.nick.size()), e.nick.data(),
                         int(net.settings.name.size()), net.settings.name.data());
                const uint32_t id = bmds_notification_new(_gui, 0);
                bmds_notification_set_title(_gui, id, title);
                bmds_notification_set_body(_gui, id, "From your notify list");
                bmds_notification_set_app(_gui, id, "IRC");
                bmds_notification_set_icon(_gui, id, "mail-send");
                static const char *const actions[] = {"Open"};
                bmds_notification_set_actions(_gui, id, actions, 1);
                bmds_notification_show(_gui, id);
                Note note;
                note.id = id;
                note.network = n;
                note.buffer = net.buffers.network().id;
                (void)note.nick.assign(e.nick);
                (void)_notes.append(std::move(note));
                return;
        }

        // ---- Events a dialog takes

        bool App::routeEvent(uint32_t n, const Event &e) {
                switch (e.kind) {
                        case Event::Kind::ListStart:
                        case Event::Kind::ListEntry:
                        case Event::Kind::ListEnd:
                                if (_lf.window != 0 && _lf.network == n) listEvent(e);
                                // A refusal is said in the network's tab too; the rest is the dialog's.
                                return e.kind != Event::Kind::ListEnd || e.text.isEmpty();
                        case Event::Kind::ModeListEntry:
                        case Event::Kind::ModeListEnd:
                        case Event::Kind::ChannelModes: {
                                if (_cf.window == 0 || _cf.network != n ||
                                    !sameName(e.target, _cf.channel, _networks[n]->session.isupport().caseMapping()))
                                        return false;
                                centralEvent(e);
                                return true;
                        }
                        default: return false;
                }
        }

} // namespace irc
