/**
 * @file      people.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * People (devplan/irc.md §7e, §7g): the member list's right-click menu as mIRC's (each item a
 * command run in the channel's tab; the operator's enabled when we are one), the reason a kick
 * asks for, and the notify list's dialog.
 */

#include "app.h"

#include "format.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

namespace irc {

        namespace {

                uint32_t menuItem(bmds_gui_t *g, uint32_t menu, const char *text) {
                        const uint32_t i = bmds_menu_item_new(g, menu);
                        bmds_menu_item_set_text(g, i, text);
                        return i;
                }

                uint32_t newButton(bmds_gui_t *g, uint32_t parent, const char *text) {
                        const uint32_t b = bmds_button_new(g, parent);
                        bmds_button_set_text(g, b, text);
                        return b;
                }

                // A dialog's window: owned by the main one, padded as the others.
                uint32_t dialogWindow(bmds_gui_t *g, uint32_t owner, const char *title, int32_t width, int32_t height) {
                        const uint32_t w = bmds_window_new(g, 0);
                        bmds_window_set_title(g, w, title);
                        bmds_window_set_owner(g, w, owner);
                        bmds_window_set_width(g, w, width);
                        bmds_window_set_height(g, w, height);
                        bmds_window_set_margin(g, w, 8);
                        bmds_window_set_spacing(g, w, 4);
                        return w;
                }

        } // namespace

        // ---- The member list's menu (§7e)

        void App::memberMenu(Page &p, int row, int x, int y) {
                if (p.network == NoNetwork || row < 0 || size_t(row) >= p.nicks.size()) return;
                bmds_gui_t   *g = _gui;
                Network      &net = *_networks[p.network];
                const Buffer *b = net.buffers.byId(p.buffer);
                if (b == nullptr) return;
                _menuNetwork = p.network;
                _menuBuffer = p.buffer;
                (void)_menuNick.assign(p.nicks[size_t(row)]);
                if (_memberMenuId == 0) {
                        _memberMenuId = bmds_menu_new(g, _window);
                        _mQuery = menuItem(g, _memberMenuId, "&Query");
                        _mWhois = menuItem(g, _memberMenuId, "&Whois");
                        _mCopy = menuItem(g, _memberMenuId, "&Copy nick");
                        bmds_menu_add_separator(g, _memberMenuId);
                        const uint32_t ctcp = bmds_menu_new(g, _memberMenuId);
                        bmds_menu_set_title(g, ctcp, "CTC&P");
                        _mPing = menuItem(g, ctcp, "&Ping");
                        _mVersion = menuItem(g, ctcp, "&Version");
                        _mTime = menuItem(g, ctcp, "&Time");
                        const uint32_t control = bmds_menu_new(g, _memberMenuId);
                        bmds_menu_set_title(g, control, "C&ontrol");
                        _mOp = menuItem(g, control, "&Op");
                        _mDeop = menuItem(g, control, "&Deop");
                        _mVoice = menuItem(g, control, "&Voice");
                        _mDevoice = menuItem(g, control, "Devoi&ce");
                        _mHop = menuItem(g, control, "&Halfop");
                        _mDehop = menuItem(g, control, "Dehal&fop");
                        bmds_menu_add_separator(g, control);
                        _mKick = menuItem(g, control, "&Kick");
                        _mKickWhy = menuItem(g, control, "Kick (&why)...");
                        _mBan = menuItem(g, control, "&Ban");
                        _mBanKick = menuItem(g, control, "Ban, k&ick (why)...");
                        bmds_menu_add_separator(g, _memberMenuId);
                        _mIgnore = menuItem(g, _memberMenuId, "&Ignore");
                        _mNotify = menuItem(g, _memberMenuId, "Add to &notify list");
                }
                // The operator's commands for an operator; kicks and voices for a half-operator too.
                const Isupport &is = net.session.isupport();
                const Channel  *c = net.session.channel(b->name);
                char            mine = 0;
                if (c != nullptr)
                        for (const Member &m : c->members)
                                if (sameName(m.nick, net.session.nick(), is.caseMapping())) mine = m.prefixes[0];
                const bool     hasOp = is.prefixChars().find('@') != Text::NoPos;
                const bool     hasHalf = is.prefixFor('h') != 0;
                const bool     op = mine != 0 && hasOp && is.rank(mine) <= is.rank('@');
                const bool     half = op || (mine != 0 && hasHalf && is.rank(mine) <= is.rank(is.prefixFor('h')));
                const bool     inChannel = b->type == Buffer::Type::Channel && b->joined;
                const uint32_t opItems[] = {_mOp, _mDeop, _mBan, _mBanKick};
                for (uint32_t item : opItems) bmds_menu_item_set_enabled(g, item, inChannel && op);
                const uint32_t halfItems[] = {_mVoice, _mDevoice, _mKick, _mKickWhy};
                for (uint32_t item : halfItems) bmds_menu_item_set_enabled(g, item, inChannel && half);
                bmds_menu_item_set_enabled(g, _mHop, inChannel && op && hasHalf);
                bmds_menu_item_set_enabled(g, _mDehop, inChannel && op && hasHalf);
                const bool ignored = _ignores.matches(_menuNick, "", "", net.settings.name, is.caseMapping());
                bmds_menu_item_set_text(g, _mIgnore, ignored ? "Un&ignore" : "&Ignore");
                bool listed = false;
                for (const String &nick : net.settings.notify)
                        listed = listed || sameName(nick, _menuNick, is.caseMapping());
                bmds_menu_item_set_text(g, _mNotify, listed ? "Remove from &notify list" : "Add to &notify list");
                bmds_menu_popup(g, _memberMenuId, x, y);
                return;
        }

        bool App::memberChosen(uint32_t item) {
                if (_memberMenuId == 0 || item == 0 || _menuNetwork >= _networks.size() || _menuNick.isEmpty())
                        return false;
                struct Command {
                                uint32_t    item;
                                const char *command; // the nick follows it
                };
                const Command commands[] = {{_mQuery, "/query "},  {_mWhois, "/quote WHOIS "}, {_mPing, "/ping "},
                                            {_mVersion, "/ctcp "}, {_mTime, "/ctcp "},         {_mOp, "/op "},
                                            {_mDeop, "/deop "},    {_mVoice, "/voice "},       {_mDevoice, "/devoice "},
                                            {_mHop, "/hop "},      {_mDehop, "/dehop "},       {_mKick, "/kick "},
                                            {_mBan, "/ban "}};
                for (const Command &c : commands) {
                        if (c.item != item) continue;
                        String line;
                        (void)line.assign(c.command);
                        (void)line.append(_menuNick);
                        if (item == _mWhois) { // twice: their idle time from their own server
                                (void)line.append(' ');
                                (void)line.append(_menuNick);
                        }
                        if (item == _mVersion) (void)line.append(" VERSION");
                        if (item == _mTime) (void)line.append(" TIME");
                        runIn(_menuNetwork, _menuBuffer, line);
                        return true;
                }
                if (item == _mCopy) {
                        bmds_clipboard_copy_text(_gui, _clipboard, "clipboard", CString(_menuNick).c());
                        return true;
                }
                if (item == _mKickWhy || item == _mBanKick) {
                        askReason(item == _mKickWhy ? 1 : 2);
                        return true;
                }
                if (item == _mIgnore || item == _mNotify) {
                        Network   &net = *_networks[_menuNetwork];
                        String     line;
                        const bool off = item == _mIgnore
                                                 ? _ignores.matches(_menuNick, "", "", net.settings.name,
                                                                    net.session.isupport().caseMapping())
                                                 : [&] {
                                                           for (const String &nick : net.settings.notify)
                                                                   if (sameName(nick, _menuNick,
                                                                                net.session.isupport().caseMapping()))
                                                                           return true;
                                                           return false;
                                                   }();
                        (void)line.assign(item == _mIgnore ? (off ? "/unignore " : "/ignore ")
                                                           : (off ? "/unnotify " : "/notify "));
                        (void)line.append(_menuNick);
                        runIn(_menuNetwork, _menuBuffer, line);
                        return true;
                }
                return false;
        }

        void App::askReason(uint8_t kind) {
                bmds_gui_t *g = _gui;
                if (_reason.window != 0) bmds_gui_destroy(g, _reason.window);
                _reason = ReasonFields();
                _reason.kind = kind;
                _reason.window = dialogWindow(g, _window, kind == 1 ? "Kick" : "Ban and kick", 420, 150);
                bmds_window_set_modal(g, _reason.window, 1);
                char text[160];
                snprintf(text, sizeof(text), "%s %.*s: why? (shown to them and the channel)",
                         kind == 1 ? "Kick" : "Ban and kick", int(_menuNick.size()), _menuNick.data());
                bmds_label_set_text(g, bmds_label_new(g, _reason.window), text);
                _reason.edit = bmds_line_edit_new(g, _reason.window);
                bmds_widget_set_stretch(g, bmds_gui_new(g, "Spacer", _reason.window), 1);
                const uint32_t row = bmds_gui_new(g, "Box", _reason.window);
                bmds_box_set_direction(g, row, "across");
                bmds_widget_set_stretch(g, row, 0);
                bmds_widget_set_stretch(g, bmds_gui_new(g, "Spacer", row), 1);
                _reason.ok = newButton(g, row, kind == 1 ? "&Kick" : "&Ban and kick");
                _reason.cancel = newButton(g, row, "Cancel");
                bmds_window_show(g, _reason.window);
                bmds_widget_set_focus(g, _reason.edit);
                return;
        }

        // ---- The notify list (§7g)

        void App::openNotifyList() {
                bmds_gui_t *g = _gui;
                if (_ntf.window != 0) {
                        bmds_window_raise(g, _ntf.window);
                        return;
                }
                NotifyFields &f = _ntf;
                f.window = dialogWindow(g, _window, "Notify list", 520, 360);
                bmds_label_set_text(g, bmds_label_new(g, f.window),
                                    "Told when they come online. A double click: a private chat.");
                f.view = bmds_details_view_new(g, f.window);
                bmds_widget_set_stretch(g, f.view, 1);
                static const char *const columns[] = {"Nick", "Network", "Now"};
                bmds_details_view_set_columns(g, f.view, columns, 3);
                bmds_details_view_set_column_width(g, f.view, 0, 140);
                bmds_details_view_set_column_width(g, f.view, 1, 140);
                bmds_details_view_set_column_width(g, f.view, 2, 200);
                const uint32_t row = bmds_gui_new(g, "Box", f.window);
                bmds_box_set_direction(g, row, "across");
                bmds_widget_set_stretch(g, row, 0);
                f.nick = bmds_line_edit_new(g, row);
                bmds_widget_set_stretch(g, f.nick, 1);
                f.add = newButton(g, row, "&Add");
                f.remove = newButton(g, row, "&Remove");
                f.close = newButton(g, row, "Close");
                listNotify();
                bmds_window_show(g, f.window);
                bmds_widget_set_focus(g, f.nick);
                return;
        }

        void App::listNotify() {
                bmds_gui_t *g = _gui;
                bmds_item_view_clear(g, _ntf.view);
                _notifyRows.clear();
                for (uint32_t n = 0; n < _networks.size(); ++n) {
                        const Network &net = *_networks[n];
                        if (net.removed) continue;
                        for (const String &nick : net.settings.notify) {
                                // Now: on since, off (last seen), or not known (not connected).
                                const int on = net.session.online(nick);
                                int64_t   at = 0;
                                for (const Network::Seen &s : net.seen)
                                        if (sameName(s.nick, nick, net.session.isupport().caseMapping())) at = s.at;
                                char                    state[64];
                                const Format::LocalTime t = Format::localTime(at, _utcOffset);
                                if (on == 1)
                                        snprintf(state, sizeof(state), at > 0 ? "Online since %02d:%02d" : "Online",
                                                 t.hour, t.minute);
                                else if (on == 0 && at > 0)
                                        snprintf(state, sizeof(state), "Offline (seen till %02d:%02d)", t.hour,
                                                 t.minute);
                                else if (on == 0)
                                        snprintf(state, sizeof(state), "Offline");
                                else if (net.session.state() == Session::State::Registered)
                                        snprintf(state, sizeof(state), "Not seen online yet");
                                else
                                        snprintf(state, sizeof(state), "Not known (not connected)");
                                const CString     who(nick), where(net.settings.name);
                                const char *const cells[] = {who.c(), where.c(), state};
                                bmds_details_view_add_row(g, _ntf.view, cells, 3);
                                NotifyRow r;
                                r.network = n;
                                (void)r.nick.assign(nick);
                                (void)_notifyRows.append(std::move(r));
                        }
                }
                bmds_widget_set_enabled(g, _ntf.remove, !_notifyRows.isEmpty());
                return;
        }

        void App::closeNotifyList() {
                if (_ntf.window == 0) return;
                bmds_gui_destroy(_gui, _ntf.window);
                _ntf = NotifyFields();
                _notifyRows.clear();
                return;
        }

        bool App::peopleEvent(const bmds_gui_event_t &e) {
                bmds_gui_t *g = _gui;
                if (_reason.window != 0) {
                        const bool ok =
                                e.source == _reason.ok ||
                                (e.source == _reason.edit && bmds_gui_event_is(g, &e, BMDS_GUI_LINE_EDIT_ACCEPTED));
                        if (ok || e.source == _reason.cancel ||
                            (e.source == _reason.window && bmds_gui_event_is(g, &e, BMDS_GUI_WINDOW_CLOSED))) {
                                String           why;
                                bmds_gui_value_t v;
                                if (ok && bmds_line_edit_get_text(g, _reason.edit, &v) == 0)
                                        (void)why.assign(Text(v.text, v.length));
                                const uint8_t kind = _reason.kind;
                                bmds_gui_destroy(g, _reason.window);
                                _reason = ReasonFields();
                                if (ok) {
                                        String line;
                                        (void)line.assign(kind == 1 ? "/kick " : "/kb ");
                                        (void)line.append(_menuNick);
                                        if (!why.isEmpty()) {
                                                (void)line.append(' ');
                                                (void)line.append(why);
                                        }
                                        runIn(_menuNetwork, _menuBuffer, line);
                                }
                                return true;
                        }
                }
                NotifyFields &f = _ntf;
                if (f.window == 0) return false;
                if (e.source == f.close || (e.source == f.window && bmds_gui_event_is(g, &e, BMDS_GUI_WINDOW_CLOSED))) {
                        closeNotifyList();
                        return true;
                }
                if (e.source == f.add ||
                    (e.source == f.nick && bmds_gui_event_is(g, &e, BMDS_GUI_LINE_EDIT_ACCEPTED))) {
                        // Added on the network in view.
                        bmds_gui_value_t v;
                        if (bmds_line_edit_get_text(g, f.nick, &v) != 0 || v.length == 0) return true;
                        const Page *p = _current < _pages.size() ? &_pages[_current] : nullptr;
                        if (p == nullptr || p->network == NoNetwork) return true;
                        String line;
                        (void)line.assign("/notify ");
                        (void)line.append(Text(v.text, v.length));
                        bmds_line_edit_set_text(g, f.nick, "");
                        runIn(p->network, _networks[p->network]->buffers.network().id, line);
                        return true;
                }
                if (e.source == f.remove ||
                    (e.source == f.view && bmds_gui_event_is(g, &e, BMDS_GUI_ITEM_VIEW_ACTIVATED))) {
                        bmds_gui_value_t v;
                        const int32_t    row = e.source == f.view && e.count == 1 ? int32_t(e.values[0].number)
                                               : bmds_item_view_get_current(g, f.view, &v) == 0 ? int32_t(v.number)
                                                                                                : -1;
                        if (row < 0 || size_t(row) >= _notifyRows.size()) return true;
                        const NotifyRow r = [&] {
                                NotifyRow copy;
                                copy.network = _notifyRows[size_t(row)].network;
                                (void)copy.nick.assign(_notifyRows[size_t(row)].nick);
                                return copy;
                        }();
                        String line;
                        (void)line.assign(e.source == f.remove ? "/unnotify " : "/query ");
                        (void)line.append(r.nick);
                        runIn(r.network, _networks[r.network]->buffers.network().id, line);
                        if (e.source != f.remove) showWindow();
                        return true;
                }
                return false;
        }

} // namespace irc
