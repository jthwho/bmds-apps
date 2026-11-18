/**
 * @file      dialogs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * IRC's dialogs (devplan/irc.md §6, §7a): Networks... (networks.ini edited: each network's
 * fields, networks added and removed; saved, written and applied), and Register this nick...
 * (NickServ's REGISTER and VERIFY; the password kept as the network's login once registered).
 */

#include "app.h"

#include "format.h"
#include "util.h"

#include <bmds/app.h>

#include <stdio.h>
#include <string.h>

namespace irc {

        namespace {

                // A field's text (a getter's answer) into @p out.
                void textOf(bmds_gui_t *gui, uint32_t id, String &out) {
                        out.clear();
                        bmds_gui_value_t v;
                        if (bmds_gui_get_named(gui, id, "text", &v) == 0) (void)out.assign(Text(v.text, v.length));
                        // Spaces at the ends are a typing slip, not a setting.
                        Text t = out;
                        while (!t.isEmpty() && t[0] == ' ') t = t.mid(1);
                        while (!t.isEmpty() && t[t.size() - 1] == ' ') t = t.left(t.size() - 1);
                        if (t.size() != out.size()) (void)out.assign(t);
                        return;
                }

                // A comma-separated list's items.
                void listOf(Text text, Vector<String> &out) {
                        out.clear();
                        size_t at = 0;
                        while (at <= text.size()) {
                                size_t end = text.find(',', at);
                                if (end == Text::NoPos) end = text.size();
                                Text item = text.mid(at, end - at);
                                while (!item.isEmpty() && item[0] == ' ') item = item.mid(1);
                                while (!item.isEmpty() && item[item.size() - 1] == ' ')
                                        item = item.left(item.size() - 1);
                                String s;
                                if (!item.isEmpty() && s.assign(item) == Error::Ok) (void)out.append(std::move(s));
                                at = end + 1;
                        }
                        return;
                }

                void joinList(const Vector<String> &items, String &out) {
                        out.clear();
                        for (size_t i = 0; i < items.size(); ++i) {
                                if (i > 0) (void)out.append(", ");
                                (void)out.append(items[i]);
                        }
                        return;
                }

                // A row of @p form labelled @p label holding a line edit (masked for a password).
                uint32_t field(bmds_gui_t *g, uint32_t form, const char *label, const char *help = nullptr,
                               bool masked = false) {
                        const uint32_t row = bmds_form_row_new(g, form);
                        bmds_form_row_set_label(g, row, label);
                        if (help != nullptr) bmds_form_row_set_help(g, row, help);
                        const uint32_t edit = bmds_line_edit_new(g, row);
                        if (masked) bmds_line_edit_set_masked(g, edit, 1);
                        return edit;
                }

                uint32_t button(bmds_gui_t *g, uint32_t parent, const char *text) {
                        const uint32_t b = bmds_button_new(g, parent);
                        bmds_button_set_text(g, b, text);
                        return b;
                }

        } // namespace

        // ---- Networks...

        void App::openNetworks() {
                bmds_gui_t *g = _gui;
                if (_nf.window != 0) {
                        bmds_window_raise(g, _nf.window);
                        return;
                }
                // The networks as they are, copied: edited here, applied on Save.
                _edit.clear();
                _editOrigin.clear();
                for (uint32_t n = 0; n < _networks.size(); ++n) {
                        if (_networks[n]->removed) continue;
                        NetworkSettings copy;
                        if (copySettings(_networks[n]->settings, copy) == Error::Ok &&
                            _edit.append(std::move(copy)) == Error::Ok)
                                (void)_editOrigin.append(int32_t(n));
                }
                _editAt = 0;

                NetworkFields &f = _nf;
                f.window = bmds_window_new(g, 0);
                bmds_window_set_title(g, f.window, "Networks");
                bmds_window_set_owner(g, f.window, _window);
                bmds_window_set_modal(g, f.window, 1);
                bmds_window_set_width(g, f.window, 640);
                bmds_window_set_height(g, f.window, 520);
                // A window's column is flush with its frame (for bars at the edges): a dialog of
                // plain widgets pads it, as the toolkit's own dialogs are.
                bmds_window_set_margin(g, f.window, 8);
                bmds_window_set_spacing(g, f.window, 4);
                const uint32_t body = bmds_splitter_new(g, f.window);
                bmds_splitter_set_direction(g, body, "across");
                bmds_widget_set_stretch(g, body, 1);

                // The networks, and Add and Remove under them.
                const uint32_t left = bmds_gui_new(g, "Box", body);
                bmds_box_set_direction(g, left, "down");
                f.list = bmds_list_view_new(g, left);
                bmds_widget_set_stretch(g, f.list, 1);
                const uint32_t listButtons = bmds_gui_new(g, "Box", left);
                bmds_box_set_direction(g, listButtons, "across");
                f.add = button(g, listButtons, "&Add");
                f.fromList = button(g, listButtons, "From &list...");
                f.remove = button(g, listButtons, "&Remove");

                // The chosen one's fields.
                const uint32_t form = bmds_form_new(g, body);
                bmds_widget_set_stretch(g, form, 1);
                f.name = field(g, form, "Name:");
                f.host = field(g, form, "Server:");
                uint32_t row = bmds_form_row_new(g, form);
                bmds_form_row_set_label(g, row, "Port:");
                f.port = bmds_number_field_new(g, row);
                bmds_number_field_set_minimum(g, f.port, 1);
                bmds_number_field_set_maximum(g, f.port, 65535);
                f.tls = bmds_check_box_new(g, row);
                bmds_check_box_set_text(g, f.tls, "&TLS");
                f.nick = field(g, form, "Nick:");
                f.altNicks = field(g, form, "Other nicks:", "Tried when the nick is taken: a, b");
                f.user = field(g, form, "User name:", "The nick if empty");
                f.realName = field(g, form, "Real name:", "The nick if empty");
                row = bmds_form_row_new(g, form);
                bmds_form_row_set_label(g, row, "Login:");
                f.login = bmds_combo_box_new(g, row);
                static const char *const logins[] = {"None", "SASL (account and password)", "SASL with a certificate",
                                                     "NickServ IDENTIFY", "Server password"};
                bmds_combo_box_set_items(g, f.login, logins, 5);
                f.account = field(g, form, "Account:", "The nick if empty");
                f.password = field(g, form, "Password:", nullptr, true);
                f.cert = field(g, form, "Certificate:", "SASL with a certificate: its PEM file, and its key's");
                f.key = field(g, form, "Key:");
                f.ca = field(g, form, "More roots:", "A PEM file of CA certificates this network's is signed by");
                f.fingerprint =
                        field(g, form, "Trusted certificate:", "Its SHA256: kept when you trust one; empty: none");
                f.channels = field(g, form, "Channels:", "Joined once connected: #a, #b key");
                // Perform in a window of its own, as mIRC's (a text editor here would take the page).
                row = bmds_form_row_new(g, form);
                bmds_form_row_set_label(g, row, "Perform:");
                bmds_form_row_set_help(g, row, "Commands run once connected, before the joins");
                f.perform = button(g, row, "&Edit...");
                f.autoconnect = bmds_check_box_new(g, form);
                bmds_check_box_set_text(g, f.autoconnect, "Connect when IRC &starts");

                f.status = bmds_label_new(g, f.window);
                const uint32_t buttons = bmds_gui_new(g, "Box", f.window);
                bmds_box_set_direction(g, buttons, "across");
                bmds_widget_set_stretch(g, bmds_gui_new(g, "Spacer", buttons), 1);
                f.save = button(g, buttons, "&Save");
                f.cancel = button(g, buttons, "Cancel");
                const uint32_t closeRequested = bmds_gui_atom(g, "closeRequested");
                bmds_gui_listen(g, f.window, &closeRequested, 1, 1);

                bmds_splitter_set_position(g, body, 250); // the list's three buttons in a row
                listNetworks();
                if (!_edit.isEmpty()) loadNetwork(0);
                bmds_window_show(g, f.window);
                return;
        }

        void App::listNetworks() {
                Vector<const char *> names;
                Vector<String>       texts;
                for (const NetworkSettings &n : _edit) {
                        String s;
                        (void)s.assign(n.name);
                        if (!n.isComplete()) (void)s.append(" (needs a nick)");
                        (void)s.append('\0');
                        (void)texts.append(std::move(s));
                }
                for (const String &s : texts) {
                        const char *c = s.data();
                        (void)names.append(std::move(c));
                }
                bmds_item_view_set_items(_gui, _nf.list, names.isEmpty() ? nullptr : names.begin(),
                                         uint32_t(names.size()));
                if (_editAt < _edit.size()) bmds_item_view_set_current(_gui, _nf.list, int32_t(_editAt));
                bmds_widget_set_enabled(_gui, _nf.remove, !_edit.isEmpty());
                return;
        }

        void App::loadNetwork(size_t i) {
                if (i >= _edit.size()) return;
                bmds_gui_t            *g = _gui;
                const NetworkFields   &f = _nf;
                const NetworkSettings &n = _edit[i];
                String                 list;
                _editAt = i;
                bmds_line_edit_set_text(g, f.name, CString(n.name).c());
                bmds_line_edit_set_text(g, f.host, CString(n.host).c());
                bmds_number_field_set_value(g, f.port, n.port);
                bmds_check_box_set_checked(g, f.tls, n.tls);
                bmds_line_edit_set_text(g, f.nick, CString(n.session.nick).c());
                joinList(n.session.altNicks, list);
                bmds_line_edit_set_text(g, f.altNicks, CString(list).c());
                bmds_line_edit_set_text(g, f.user, CString(n.session.user).c());
                bmds_line_edit_set_text(g, f.realName, CString(n.session.realName).c());
                bmds_combo_box_set_current(g, f.login, int32_t(n.session.login));
                bmds_line_edit_set_text(g, f.account, CString(n.session.account).c());
                bmds_line_edit_set_text(g, f.password, CString(n.session.password).c());
                bmds_line_edit_set_text(g, f.cert, CString(n.cert).c());
                bmds_line_edit_set_text(g, f.key, CString(n.key).c());
                bmds_line_edit_set_text(g, f.ca, CString(n.ca).c());
                bmds_line_edit_set_text(g, f.fingerprint, CString(n.fingerprint).c());
                joinList(n.session.autojoin, list);
                bmds_line_edit_set_text(g, f.channels, CString(list).c());
                performLabel();
                bmds_check_box_set_checked(g, f.autoconnect, n.autoconnect);
                return;
        }

        void App::storeNetwork(size_t i) {
                if (i >= _edit.size()) return;
                bmds_gui_t          *g = _gui;
                const NetworkFields &f = _nf;
                NetworkSettings     &n = _edit[i];
                String               text;
                bmds_gui_value_t     v;
                textOf(g, f.name, n.name);
                textOf(g, f.host, n.host);
                if (bmds_number_field_get_value(g, f.port, &v) == 0 && v.number >= 1 && v.number <= 65535)
                        n.port = uint16_t(v.number);
                if (bmds_check_box_get_checked(g, f.tls, &v) == 0) n.tls = v.number != 0;
                textOf(g, f.nick, n.session.nick);
                textOf(g, f.altNicks, text);
                listOf(text, n.session.altNicks);
                textOf(g, f.user, n.session.user);
                textOf(g, f.realName, n.session.realName);
                if (bmds_combo_box_get_current(g, f.login, &v) == 0 && v.number >= 0 && v.number <= 4)
                        n.session.login = Login(v.number);
                textOf(g, f.account, n.session.account);
                if (bmds_gui_get_named(g, f.password, "text", &v) == 0) // a password's spaces are its own
                        (void)n.session.password.assign(Text(v.text, v.length));
                textOf(g, f.cert, n.cert);
                textOf(g, f.key, n.key);
                textOf(g, f.ca, n.ca);
                textOf(g, f.fingerprint, n.fingerprint);
                textOf(g, f.channels, text);
                listOf(text, n.session.autojoin);
                if (bmds_check_box_get_checked(g, f.autoconnect, &v) == 0) n.autoconnect = v.number != 0;
                return;
        }

        void App::performLabel() {
                if (_editAt >= _edit.size()) return;
                const size_t count = _edit[_editAt].session.perform.size();
                char         text[48];
                if (count == 0)
                        snprintf(text, sizeof(text), "&Edit... (none)");
                else
                        snprintf(text, sizeof(text), "&Edit... (%u command%s)", unsigned(count), count == 1 ? "" : "s");
                bmds_button_set_text(_gui, _nf.perform, text);
                return;
        }

        void App::openPerform() {
                bmds_gui_t    *g = _gui;
                NetworkFields &f = _nf;
                if (f.performWindow != 0 || _editAt >= _edit.size()) return;
                f.performWindow = bmds_window_new(g, 0);
                char title[96];
                snprintf(title, sizeof(title), "Perform on %.*s", int(_edit[_editAt].name.size()),
                         _edit[_editAt].name.data());
                bmds_window_set_title(g, f.performWindow, title);
                bmds_window_set_owner(g, f.performWindow, f.window);
                bmds_window_set_modal(g, f.performWindow, 1);
                bmds_window_set_width(g, f.performWindow, 480);
                bmds_window_set_height(g, f.performWindow, 300);
                bmds_window_set_margin(g, f.performWindow, 8);
                bmds_window_set_spacing(g, f.performWindow, 4);
                bmds_label_set_text(g, bmds_label_new(g, f.performWindow),
                                    "Commands run once connected, before the channels are joined:");
                bmds_label_set_text(g, bmds_label_new(g, f.performWindow), "a line each, in order; $me is your nick.");
                f.performText = bmds_text_edit_new(g, f.performWindow);
                bmds_widget_set_stretch(g, f.performText, 1);
                String text;
                for (const String &line : _edit[_editAt].session.perform) {
                        (void)text.append(line);
                        (void)text.append('\n');
                }
                bmds_text_edit_set_text(g, f.performText, CString(text).c());
                const uint32_t row = bmds_gui_new(g, "Box", f.performWindow);
                bmds_box_set_direction(g, row, "across");
                bmds_widget_set_stretch(g, row, 0);
                bmds_widget_set_stretch(g, bmds_gui_new(g, "Spacer", row), 1);
                f.performOk = button(g, row, "OK");
                f.performCancel = button(g, row, "Cancel");
                bmds_window_show(g, f.performWindow);
                bmds_widget_set_focus(g, f.performText);
                return;
        }

        void App::storePerform() {
                // A command a line, the empty ones and the ends' spaces left out.
                NetworkFields &f = _nf;
                if (f.performWindow == 0 || _editAt >= _edit.size()) return;
                bmds_gui_value_t v;
                NetworkSettings &n = _edit[_editAt];
                n.session.perform.clear();
                if (bmds_text_edit_get_text(_gui, f.performText, &v) == 0) {
                        const Text all(v.text, v.length);
                        size_t     at = 0;
                        while (at < all.size()) {
                                size_t end = all.find('\n', at);
                                if (end == Text::NoPos) end = all.size();
                                Text line = all.mid(at, end - at);
                                at = end + 1;
                                while (!line.isEmpty() && (line[0] == ' ' || line[0] == '\t')) line = line.mid(1);
                                while (!line.isEmpty() &&
                                       (line[line.size() - 1] == ' ' || line[line.size() - 1] == '\r'))
                                        line = line.left(line.size() - 1);
                                String s;
                                if (!line.isEmpty() && s.assign(line) == Error::Ok)
                                        (void)n.session.perform.append(std::move(s));
                        }
                }
                return;
        }

        void App::closeNetworks() {
                if (_nf.window == 0) return;
                closeServerList();
                if (_nf.performWindow != 0) bmds_gui_destroy(_gui, _nf.performWindow);
                bmds_gui_destroy(_gui, _nf.window); // its widgets with it
                _nf = NetworkFields();
                _edit.clear();
                _editOrigin.clear();
                return;
        }

        void App::removeNetwork(uint32_t n) {
                Network &net = *_networks[n];
                net.wanted = false;
                net.reconnectAt = -1;
                if (net.connection.state() == Connection::State::Up) {
                        (void)net.session.quit("");
                        pump(n);
                        // The QUIT given a moment: the connection's buffer is written as poll allows.
                        char   buf[256];
                        size_t got = 0;
                        for (int i = 0; i < 5 && (net.connection.events() & POLLOUT) != 0; ++i) {
                                struct pollfd p = {net.connection.fd(), net.connection.events(), 0};
                                if (poll(&p, 1, 100) > 0)
                                        (void)net.connection.service(p.revents, now().mono, buf, sizeof(buf), got);
                        }
                }
                net.connection.stop();
                net.removed = true;
                for (size_t i = _pages.size(); i > 0; --i)
                        if (_pages[i - 1].network == n) closePage(i - 1);
                return;
        }

        void App::saveNetworkDialog() {
                storeNetwork(_editAt);
                // Each needs a name of its own.
                for (size_t i = 0; i < _edit.size(); ++i) {
                        if (_edit[i].name.isEmpty()) {
                                bmds_label_set_text(_gui, _nf.status, "Each network needs a name.");
                                loadNetwork(i);
                                listNetworks();
                                return;
                        }
                        for (size_t k = 0; k < i; ++k)
                                if (_edit[k].name.view().equalsIgnoringCase(_edit[i].name)) {
                                        bmds_label_set_text(_gui, _nf.status, "Two networks have the same name.");
                                        return;
                                }
                }
                // Those taken out: disconnected, their tabs closed.
                for (uint32_t n = 0; n < _networks.size(); ++n) {
                        if (_networks[n]->removed) continue;
                        bool kept = false;
                        for (int32_t origin : _editOrigin)
                                if (origin == int32_t(n)) kept = true;
                        if (!kept) removeNetwork(n);
                }
                // The others' settings taken (the session's from the next connection), the new ones made.
                bool renamed = false;
                for (size_t i = 0; i < _edit.size(); ++i) {
                        NetworkSettings copy;
                        if (copySettings(_edit[i], copy) != Error::Ok) continue;
                        if (copy.session.version.isEmpty()) (void)copy.session.version.assign("BMDS IRC 0.1");
                        resolvePaths(copy);
                        if (_editOrigin[i] >= 0) {
                                Network &net = *_networks[uint32_t(_editOrigin[i])];
                                renamed = renamed || !(net.settings.name == copy.name.view());
                                NetworkSettings forSession;
                                (void)copySettings(copy, forSession);
                                net.session.setConfig(std::move(forSession.session));
                                net.settings = std::move(copy);
                                updateSecrets(uint32_t(_editOrigin[i]));
                                applyBehaviour(net);
                                watchAll(uint32_t(_editOrigin[i]));
                        } else {
                                auto *net = new (std::nothrow) Network(std::move(copy));
                                if (net == nullptr) continue;
                                Buffers::Settings &b = net->buffers.settings();
                                b.showJoins = _settings.showJoins;
                                b.utcOffsetMinutes = _utcOffset;
                                for (const String &w : _settings.highlights) {
                                        String c;
                                        if (c.assign(w) == Error::Ok) (void)b.highlights.append(std::move(c));
                                }
                                (void)_networks.append(std::move(net));
                                updateSecrets(uint32_t(_networks.size() - 1));
                                applyBehaviour(*_networks[_networks.size() - 1]);
                                watchAll(uint32_t(_networks.size() - 1));
                                syncPages(uint32_t(_networks.size() - 1));
                        }
                }
                saveNetworks();
                closeNetworks();
                if (renamed) resetTree();
                for (uint32_t n = 0; n < _networks.size(); ++n) {
                        Network &net = *_networks[n];
                        if (net.removed) continue;
                        syncPages(n);
                        // Ready and wanted at the start: connected now (a first nick set, a network added).
                        if (net.settings.isComplete() && net.settings.autoconnect &&
                            net.connection.state() == Connection::State::Idle && net.reconnectAt < 0 && !net.wanted)
                                connect(n);
                }
                // The app's own page (no network had a nick): gone once one can connect.
                for (size_t i = 0; i < _pages.size(); ++i)
                        if (_pages[i].network == NoNetwork && _networks.size() > 0) {
                                closePage(i);
                                break;
                        }
                updateStatus();
                return;
        }

        // ---- Add from the list (servers.ini, §7c)

        void App::openServerList() {
                bmds_gui_t *g = _gui;
                if (_sf.window != 0) {
                        bmds_window_raise(g, _sf.window);
                        return;
                }
                // Read now, kept only while the dialog is open.
                _servers.clear();
                Vector<String> problems;
                String         text;
                const char    *dir = bmds_app_dir();
                bool           found = false;
                if (dir != nullptr) {
                        String path;
                        (void)path.assign(dir);
                        (void)path.append("/servers.ini");
                        found = readFile(CString(path).c(), text);
                        if (found) (void)readServers(text, _servers, problems);
                }
                ServerListFields &f = _sf;
                f.window = bmds_window_new(g, 0);
                bmds_window_set_title(g, f.window, "Add a network from the list");
                bmds_window_set_owner(g, f.window, _nf.window);
                bmds_window_set_modal(g, f.window, 1);
                bmds_window_set_width(g, f.window, 600);
                bmds_window_set_height(g, f.window, 420);
                bmds_window_set_margin(g, f.window, 8);
                bmds_window_set_spacing(g, f.window, 4);
                const uint32_t top = bmds_gui_new(g, "Box", f.window);
                bmds_box_set_direction(g, top, "across");
                bmds_widget_set_stretch(g, top, 0);
                bmds_label_set_text(g, bmds_label_new(g, top), "Find:");
                f.filter = bmds_line_edit_new(g, top);
                bmds_widget_set_stretch(g, f.filter, 1);
                const uint32_t edited = bmds_gui_atom(g, BMDS_GUI_LINE_EDIT_EDITED);
                bmds_gui_listen(g, f.filter, &edited, 1, 1);
                f.view = bmds_details_view_new(g, f.window);
                bmds_widget_set_stretch(g, f.view, 1);
                static const char *const columns[] = {"Network", "For", "Connection"};
                bmds_details_view_set_columns(g, f.view, columns, 3);
                bmds_details_view_set_column_width(g, f.view, 0, 120);
                bmds_details_view_set_column_width(g, f.view, 1, 320);
                bmds_details_view_set_column_width(g, f.view, 2, 120);
                const uint32_t row = bmds_gui_new(g, "Box", f.window);
                bmds_box_set_direction(g, row, "across");
                bmds_widget_set_stretch(g, row, 0);
                f.status = bmds_label_new(g, row);
                bmds_widget_set_stretch(g, f.status, 1);
                f.add = button(g, row, "&Add");
                f.close = button(g, row, "Close");
                showServers();
                if (!found)
                        bmds_label_set_text(g, f.status,
                                            "The list isn't installed (IRC isn't running from its app folder).");
                else if (!problems.isEmpty())
                        bmds_label_set_text(g, f.status, CString(problems[0]).c());
                bmds_window_show(g, f.window);
                bmds_widget_set_focus(g, f.filter);
                return;
        }

        void App::showServers() {
                bmds_gui_t      *g = _gui;
                bmds_gui_value_t v;
                String           want;
                if (bmds_line_edit_get_text(g, _sf.filter, &v) == 0) (void)want.assign(Text(v.text, v.length));
                const auto has = [&](Text hay) {
                        const Text n = want;
                        if (n.isEmpty()) return true;
                        for (size_t i = 0; i + n.size() <= hay.size(); ++i) {
                                size_t k = 0;
                                while (k < n.size() && Text::lower(hay[i + k]) == Text::lower(n[k])) ++k;
                                if (k == n.size()) return true;
                        }
                        return false;
                };
                bmds_item_view_clear(g, _sf.view);
                _serverRows.clear();
                for (uint32_t i = 0; i < _servers.size(); ++i) {
                        const ServerEntry &s = _servers[i];
                        if (!has(s.name) && !has(s.about) && !has(s.host)) continue;
                        char how[48];
                        snprintf(how, sizeof(how), s.tls ? "TLS, %u" : "%u, no TLS", unsigned(s.port));
                        const CString     name(s.name), about(s.about);
                        const char *const cells[] = {name.c(), about.c(), how};
                        bmds_details_view_add_row(g, _sf.view, cells, 3);
                        (void)_serverRows.append(std::move(i));
                }
                bmds_widget_set_enabled(g, _sf.add, !_serverRows.isEmpty());
                return;
        }

        void App::closeServerList() {
                if (_sf.window == 0) return;
                bmds_gui_destroy(_gui, _sf.window);
                _sf = ServerListFields();
                _servers.clear();
                _serverRows.clear();
                return;
        }

        // ---- Register this nick...

        void App::openRegister() {
                bmds_gui_t *g = _gui;
                if (_rf.window != 0) {
                        bmds_window_raise(g, _rf.window);
                        return;
                }
                const Page *p = _pages.isEmpty() || _current >= _pages.size() ? nullptr : &_pages[_current];
                if (p == nullptr || p->network == NoNetwork ||
                    _networks[p->network]->session.state() != Session::State::Registered) {
                        note("Register this nick: connect to the network first.");
                        return;
                }
                const Network  &net = *_networks[p->network];
                RegisterFields &f = _rf;
                f.network = p->network;
                f.window = bmds_window_new(g, 0);
                bmds_window_set_title(g, f.window, "Register this nick");
                bmds_window_set_owner(g, f.window, _window);
                bmds_window_set_width(g, f.window, 500);
                bmds_window_set_height(g, f.window, 380);
                bmds_window_set_margin(g, f.window, 8); // padded, as Networks...
                bmds_window_set_spacing(g, f.window, 4);
                char intro[160];
                snprintf(intro, sizeof(intro), "Register %.*s on %.*s with NickServ.", int(net.session.nick().size()),
                         net.session.nick().data(), int(net.settings.name.size()), net.settings.name.data());
                bmds_label_set_text(g, bmds_label_new(g, f.window), intro); // a label is one line: two of them
                bmds_label_set_text(g, bmds_label_new(g, f.window), "Its answers show here and in the network's tab.");
                const uint32_t form = bmds_form_new(g, f.window);
                bmds_widget_set_stretch(g, form, 0); // the slack goes above Close, not into the forms
                f.password = field(g, form, "Password:", nullptr, true);
                f.again = field(g, form, "Again:", nullptr, true);
                f.email = field(g, form, "Email:", "Where NickServ sends a code, if the network asks for one");
                const uint32_t sendRow = bmds_gui_new(g, "Box", f.window);
                bmds_box_set_direction(g, sendRow, "across");
                bmds_widget_set_stretch(g, bmds_gui_new(g, "Spacer", sendRow), 1);
                bmds_widget_set_stretch(g, sendRow, 0); // across only: no share of the height
                f.send = button(g, sendRow, "&Register");
                const uint32_t form2 = bmds_form_new(g, f.window);
                bmds_widget_set_stretch(g, form2, 0);
                f.code = field(g, form2, "Code:", "The code NickServ mailed, if it did");
                const uint32_t verifyRow = bmds_gui_new(g, "Box", f.window);
                bmds_box_set_direction(g, verifyRow, "across");
                bmds_widget_set_stretch(g, bmds_gui_new(g, "Spacer", verifyRow), 1);
                bmds_widget_set_stretch(g, verifyRow, 0);
                f.verify = button(g, verifyRow, "&Verify");
                f.status = bmds_label_new(g, f.window);
                bmds_widget_set_stretch(g, bmds_gui_new(g, "Spacer", f.window), 1);
                const uint32_t buttons = bmds_gui_new(g, "Box", f.window);
                bmds_box_set_direction(g, buttons, "across");
                bmds_widget_set_stretch(g, bmds_gui_new(g, "Spacer", buttons), 1);
                bmds_widget_set_stretch(g, buttons, 0);
                f.close = button(g, buttons, "Close");
                bmds_window_show(g, f.window);
                return;
        }

        void App::closeRegister() {
                if (_rf.window == 0) return;
                bmds_gui_destroy(_gui, _rf.window);
                _rf = RegisterFields();
                return;
        }

        void App::registration(uint32_t n, const Event &e) {
                Network &net = *_networks[n];
                if (net.registerPassword.isEmpty() || !e.nick.view().equalsIgnoringCase("NickServ")) return;
                const Format::Registration r = Format::registration(e.text);
                if (r == Format::Registration::None) return;
                String plain;
                (void)Format::strip(e.text, plain);
                String     status;
                const bool saved = net.settings.session.login == Login::Sasl &&
                                   net.settings.session.password == net.registerPassword.view() &&
                                   net.settings.session.account == net.registerNick.view();
                if (r == Format::Registration::Registered && saved) {
                        // Registered already (now verified, say): the login was kept then.
                        (void)status.assign("NickServ: ");
                        (void)status.append(plain);
                } else if (r == Format::Registration::Registered) {
                        // Kept as the network's login: SASL from the next connection on (§7a).
                        net.settings.session.login = Login::Sasl;
                        (void)net.settings.session.account.assign(net.registerNick);
                        (void)net.settings.session.password.assign(net.registerPassword);
                        NetworkSettings forSession;
                        if (copySettings(net.settings, forSession) == Error::Ok)
                                net.session.setConfig(std::move(forSession.session));
                        saveNetworks();
                        updateSecrets(n);
                        (void)status.assign("Registered: IRC logs in as ");
                        (void)status.append(net.registerNick);
                        (void)status.append(" with it from now on (SASL).");
                        Vector<Line> lines;
                        (void)net.buffers.note(net.buffers.network().id, status, now().wall, lines);
                        show(n, lines);
                } else if (r == Format::Registration::Verify) {
                        (void)status.assign("NickServ mailed a code: type it above, then Verify.");
                } else {
                        (void)status.assign("NickServ: ");
                        (void)status.append(plain);
                        net.registerPassword.clear();
                }
                if (_rf.window != 0 && _rf.network == n) bmds_label_set_text(_gui, _rf.status, CString(status).c());
                return;
        }

        // ---- Their events

        bool App::dialogEvent(const bmds_gui_event_t &e) {
                bmds_gui_t    *g = _gui;
                ServerListFields &sf = _sf;
                if (sf.window != 0) {
                        if (e.source == sf.close ||
                            (e.source == sf.window && bmds_gui_event_is(g, &e, BMDS_GUI_WINDOW_CLOSED))) {
                                closeServerList();
                                return true;
                        }
                        if (e.source == sf.filter) {
                                showServers();
                                return true;
                        }
                        if (e.source == sf.add ||
                            (e.source == sf.view && bmds_gui_event_is(g, &e, BMDS_GUI_ITEM_VIEW_ACTIVATED))) {
                                bmds_gui_value_t v;
                                const int32_t    row = e.source == sf.view && e.count == 1 ? int32_t(e.values[0].number)
                                                       : bmds_item_view_get_current(g, sf.view, &v) == 0
                                                               ? int32_t(v.number)
                                                               : -1;
                                if (row < 0 || size_t(row) >= _serverRows.size()) return true;
                                const ServerEntry &s = _servers[_serverRows[size_t(row)]];
                                storeNetwork(_editAt);
                                // There already: chosen; else added, its nick to fill in.
                                size_t at = _edit.size();
                                for (size_t i = 0; i < _edit.size(); ++i)
                                        if (_edit[i].name.view().equalsIgnoringCase(s.name)) at = i;
                                if (at == _edit.size()) {
                                        NetworkSettings n;
                                        if (networkFrom(s, n) != Error::Ok || _edit.append(std::move(n)) != Error::Ok)
                                                return true;
                                        (void)_editOrigin.append(-1);
                                }
                                closeServerList();
                                _editAt = at;
                                listNetworks();
                                loadNetwork(_editAt);
                                bmds_widget_set_focus(g, _nf.nick);
                                return true;
                        }
                }
                NetworkFields &f = _nf;
                if (f.performWindow != 0) {
                        const bool ok = e.source == f.performOk;
                        if (ok || e.source == f.performCancel ||
                            (e.source == f.performWindow && bmds_gui_event_is(g, &e, BMDS_GUI_WINDOW_CLOSED))) {
                                if (ok) storePerform();
                                bmds_gui_destroy(g, f.performWindow);
                                f.performWindow = f.performText = f.performOk = f.performCancel = 0;
                                performLabel();
                                return true;
                        }
                }
                if (f.window != 0) {
                        if (e.source == f.perform) {
                                openPerform();
                                return true;
                        }
                        if (e.source == f.fromList) {
                                openServerList();
                                return true;
                        }
                        if (e.source == f.window || e.source == f.cancel) {
                                if (e.source == f.cancel || bmds_gui_event_is(g, &e, "closeRequested") ||
                                    bmds_gui_event_is(g, &e, BMDS_GUI_WINDOW_CLOSED))
                                        closeNetworks();
                                return true;
                        }
                        if (e.source == f.list && bmds_gui_event_is(g, &e, BMDS_GUI_ITEM_VIEW_CURRENT_CHANGED) &&
                            e.count == 1) {
                                const int32_t i = int32_t(e.values[0].number);
                                if (i >= 0 && size_t(i) < _edit.size() && size_t(i) != _editAt) {
                                        storeNetwork(_editAt);
                                        loadNetwork(size_t(i));
                                        listNetworks(); // a name or nick changed shows
                                }
                                return true;
                        }
                        if (e.source == f.add) {
                                storeNetwork(_editAt);
                                NetworkSettings n;
                                char            name[32];
                                for (int k = 1;; ++k) {
                                        snprintf(name, sizeof(name), k == 1 ? "New network" : "New network %d", k);
                                        bool taken = false;
                                        for (const NetworkSettings &o : _edit)
                                                if (o.name == Text(name)) taken = true;
                                        if (!taken) break;
                                }
                                (void)n.name.assign(name);
                                if (_edit.append(std::move(n)) == Error::Ok) (void)_editOrigin.append(-1);
                                _editAt = _edit.size() - 1;
                                listNetworks();
                                loadNetwork(_editAt);
                                bmds_widget_set_focus(g, f.name);
                                return true;
                        }
                        if (e.source == f.remove && _editAt < _edit.size()) {
                                _edit.removeAt(_editAt);
                                _editOrigin.removeAt(_editAt);
                                if (_editAt > 0) --_editAt;
                                listNetworks();
                                loadNetwork(_editAt);
                                return true;
                        }
                        if (e.source == f.save) {
                                saveNetworkDialog();
                                return true;
                        }
                }
                RegisterFields &r = _rf;
                if (r.window != 0) {
                        if (e.source == r.close ||
                            (e.source == r.window && bmds_gui_event_is(g, &e, BMDS_GUI_WINDOW_CLOSED))) {
                                closeRegister();
                                return true;
                        }
                        if (r.network >= _networks.size()) return false;
                        Network &net = *_networks[r.network];
                        if (e.source == r.send) {
                                String           password, again, email, text;
                                bmds_gui_value_t v;
                                if (bmds_gui_get_named(g, r.password, "text", &v) == 0)
                                        (void)password.assign(Text(v.text, v.length));
                                if (bmds_gui_get_named(g, r.again, "text", &v) == 0)
                                        (void)again.assign(Text(v.text, v.length));
                                textOf(g, r.email, email);
                                const char *problem = nullptr;
                                if (password.size() < 6 || password.view().find(' ') != Text::NoPos)
                                        problem = "The password: six characters at least, no spaces.";
                                else if (!(password == again.view()))
                                        problem = "The two passwords differ.";
                                else if (email.view().find('@') == Text::NoPos || email.view().find(' ') != Text::NoPos)
                                        problem = "The email address isn't one.";
                                if (problem != nullptr) {
                                        bmds_label_set_text(g, r.status, problem);
                                        return true;
                                }
                                (void)net.registerNick.assign(net.session.nick());
                                (void)net.registerPassword.assign(password);
                                updateSecrets(r.network); // NickServ repeats it
                                (void)text.assign("REGISTER ");
                                (void)text.append(password);
                                (void)text.append(' ');
                                (void)text.append(email);
                                (void)net.session.privmsg("NickServ", text, now());
                                pump(r.network);
                                bmds_label_set_text(g, r.status, "Sent: waiting for NickServ...");
                                return true;
                        }
                        if (e.source == r.verify) {
                                String code, text;
                                textOf(g, r.code, code);
                                if (code.isEmpty() || code.view().find(' ') != Text::NoPos) {
                                        bmds_label_set_text(g, r.status, "Type the code NickServ mailed first.");
                                        return true;
                                }
                                (void)text.assign("VERIFY REGISTER ");
                                (void)text.append(net.registerNick.isEmpty() ? net.session.nick()
                                                                             : net.registerNick.view());
                                (void)text.append(' ');
                                (void)text.append(code);
                                (void)net.session.privmsg("NickServ", text, now());
                                pump(r.network);
                                bmds_label_set_text(g, r.status, "Sent: waiting for NickServ...");
                                return true;
                        }
                }
                return false;
        }

} // namespace irc
