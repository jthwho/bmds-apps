/**
 * @file      channels.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Channels (devplan/irc.md §7d, §7h): the channel list as mIRC's (LIST filtered at the server as
 * it allows, kept here and filtered as typed, a column sorted by its header, joined with a double
 * click), and Channel Central (a channel's topic, modes, key, limit and its ban, exception and
 * invite lists).
 */

#include "app.h"

#include "format.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

namespace irc {

        namespace {

                uint32_t newButton(bmds_gui_t *g, uint32_t parent, const char *text) {
                        const uint32_t b = bmds_button_new(g, parent);
                        bmds_button_set_text(g, b, text);
                        return b;
                }

                uint32_t across(bmds_gui_t *g, uint32_t parent) {
                        const uint32_t row = bmds_gui_new(g, "Box", parent);
                        bmds_box_set_direction(g, row, "across");
                        bmds_widget_set_stretch(g, row, 0);
                        return row;
                }

                void label(bmds_gui_t *g, uint32_t parent, const char *text) {
                        bmds_label_set_text(g, bmds_label_new(g, parent), text);
                        return;
                }

                // ASCII letters' case ignored: @p needle in @p hay?
                bool containsText(Text hay, Text needle) {
                        if (needle.isEmpty()) return true;
                        for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                                size_t k = 0;
                                while (k < needle.size() && Text::lower(hay[i + k]) == Text::lower(needle[k])) ++k;
                                if (k == needle.size()) return true;
                        }
                        return false;
                }

                int compareText(Text a, Text b) {
                        const size_t n = a.size() < b.size() ? a.size() : b.size();
                        for (size_t i = 0; i < n; ++i) {
                                const char x = Text::lower(a[i]), y = Text::lower(b[i]);
                                if (x != y) return x < y ? -1 : 1;
                        }
                        return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
                }

                // A heap sort of @p rows by @p before (no libstdc++ on the board; 20,000 at most).
                template <typename F> void sortRows(Vector<uint32_t> &rows, F &&before) {
                        const size_t n = rows.size();
                        const auto   sift = [&](size_t start, size_t end) {
                                size_t root = start;
                                while (2 * root + 1 < end) {
                                        size_t child = 2 * root + 1;
                                        if (child + 1 < end && before(rows[child], rows[child + 1])) ++child;
                                        if (!before(rows[root], rows[child])) return;
                                        const uint32_t t = rows[root];
                                        rows[root] = rows[child];
                                        rows[child] = t;
                                        root = child;
                                }
                        };
                        for (size_t i = n / 2; i-- > 0;) sift(i, n);
                        for (size_t end = n; end > 1; --end) {
                                const uint32_t t = rows[0];
                                rows[0] = rows[end - 1];
                                rows[end - 1] = t;
                                sift(0, end - 1);
                        }
                        return;
                }

                const struct {
                                char        mode;
                                const char *text;
                } Flags[6] = {{'n', "No messages from outside (n)"},
                              {'t', "Only operators set the topic (t)"},
                              {'i', "Invited only (i)"},
                              {'m', "Moderated: voiced speak (m)"},
                              {'s', "Secret (s)"},
                              {'p', "Private (p)"}};

        } // namespace

        // ---- List channels... (§7d)

        void App::openChannelList(uint32_t n, Text filter) {
                bmds_gui_t *g = _gui;
                if (n >= _networks.size()) return;
                if (_lf.window != 0 && _lf.network != n) closeChannelList(); // one network's list at a time
                Network &net = *_networks[n];
                if (_lf.window == 0) {
                        ListFields &f = _lf;
                        f.network = n;
                        f.window = bmds_window_new(g, 0);
                        char title[96];
                        snprintf(title, sizeof(title), "Channels on %.*s", int(net.settings.name.size()),
                                 net.settings.name.data());
                        bmds_window_set_title(g, f.window, title);
                        bmds_window_set_owner(g, f.window, _window);
                        bmds_window_set_width(g, f.window, 720);
                        bmds_window_set_height(g, f.window, 480);
                        bmds_window_set_margin(g, f.window, 8);
                        bmds_window_set_spacing(g, f.window, 4);
                        uint32_t row = across(g, f.window);
                        label(g, row, "Text:");
                        f.text = bmds_line_edit_new(g, row);
                        bmds_widget_set_stretch(g, f.text, 1);
                        const uint32_t edited = bmds_gui_atom(g, BMDS_GUI_LINE_EDIT_EDITED);
                        bmds_gui_listen(g, f.text, &edited, 1, 1);
                        label(g, row, "Users from");
                        f.least = bmds_number_field_new(g, row);
                        bmds_number_field_set_minimum(g, f.least, 0);
                        bmds_number_field_set_maximum(g, f.least, 100000);
                        bmds_number_field_set_value(g, f.least, 3); // the one- and two-person long tail left out
                        label(g, row, "to");
                        f.most = bmds_number_field_new(g, row);
                        bmds_number_field_set_minimum(g, f.most, 0);
                        bmds_number_field_set_maximum(g, f.most, 100000);
                        bmds_number_field_set_value(g, f.most, 0);
                        label(g, row, "(0: any)");
                        f.view = bmds_details_view_new(g, f.window);
                        bmds_widget_set_stretch(g, f.view, 1);
                        static const char *const columns[] = {"Channel", "Users", "Topic"};
                        bmds_details_view_set_columns(g, f.view, columns, 3);
                        bmds_details_view_set_column_width(g, f.view, 0, 180);
                        bmds_details_view_set_column_width(g, f.view, 1, 72);
                        bmds_details_view_set_column_width(g, f.view, 2, 440);
                        row = across(g, f.window);
                        f.status = bmds_label_new(g, row);
                        bmds_widget_set_stretch(g, f.status, 1);
                        f.get = newButton(g, row, "&Get list");
                        f.join = newButton(g, row, "&Join");
                        f.close = newButton(g, row, "Close");
                        bmds_window_show(g, f.window);
                }
                if (!filter.isEmpty()) bmds_line_edit_set_text(g, _lf.text, CString(filter).c());
                bmds_window_raise(g, _lf.window);
                bmds_widget_set_focus(g, _lf.text);
                if (net.session.state() != Session::State::Registered) {
                        bmds_label_set_text(g, _lf.status, "Connect to the network first.");
                        return;
                }
                if (_listed.isEmpty() && !_lf.receiving) {
                        // Nothing kept yet: asked for at once (refilter what is kept otherwise).
                        bmds_gui_value_t v;
                        const uint32_t   least =
                                bmds_number_field_get_value(g, _lf.least, &v) == 0 ? uint32_t(v.number) : 0;
                        const uint32_t most =
                                bmds_number_field_get_value(g, _lf.most, &v) == 0 ? uint32_t(v.number) : 0;
                        (void)net.session.list(least, most);
                        pump(n);
                        _lf.receiving = true;
                        bmds_button_set_text(g, _lf.get, "&Stop");
                        bmds_label_set_text(g, _lf.status, "Asking the server...");
                } else {
                        showChannels();
                }
                return;
        }

        void App::listEvent(const Event &e) {
                bmds_gui_t *g = _gui;
                switch (e.kind) {
                        case Event::Kind::ListStart:
                                _listed.clear();
                                _listText.clear();
                                _lf.dropped = 0;
                                _lf.receiving = true;
                                bmds_button_set_text(g, _lf.get, "&Stop");
                                return;
                        case Event::Kind::ListEntry: {
                                if (!_lf.receiving) return; // stopped: the rest ignored
                                if (_listed.size() >= MostListed) {
                                        ++_lf.dropped;
                                        return;
                                }
                                // Its name and topic kept, the topic's codes gone and cut at 200 bytes.
                                String topic;
                                (void)Format::strip(e.text, topic);
                                size_t length = topic.size() > TopicBytes ? TopicBytes : topic.size();
                                while (length > 0 && length < topic.size() &&
                                       (uint8_t(topic.view()[length]) & 0xc0) == 0x80)
                                        --length; // not mid-character
                                ListedChannel c;
                                c.name = uint32_t(_listText.size());
                                c.nameLength = uint16_t(e.target.size() > 200 ? 200 : e.target.size());
                                if (_listText.append(e.target.view().left(c.nameLength)) != Error::Ok) return;
                                c.topic = uint32_t(_listText.size());
                                c.topicLength = uint16_t(length);
                                if (_listText.append(topic.view().left(length)) != Error::Ok) return;
                                c.users = uint32_t(e.number < 0 ? 0 : e.number > 10000000 ? 10000000 : e.number);
                                if (_listed.append(std::move(c)) != Error::Ok) return;
                                if (_listed.size() % 500 == 0) {
                                        char text[64];
                                        snprintf(text, sizeof(text), "Receiving: %u channels...",
                                                 unsigned(_listed.size()));
                                        bmds_label_set_text(g, _lf.status, text);
                                }
                                return;
                        }
                        case Event::Kind::ListEnd:
                                _lf.receiving = false;
                                bmds_button_set_text(g, _lf.get, "&Get list");
                                if (!e.text.isEmpty()) {
                                        String why;
                                        (void)why.assign("The server refused: ");
                                        (void)why.append(e.text);
                                        bmds_label_set_text(g, _lf.status, CString(why).c());
                                        return;
                                }
                                showChannels();
                                return;
                        default: return;
                }
        }

        void App::showChannels() {
                bmds_gui_t      *g = _gui;
                bmds_gui_value_t v;
                String           text;
                if (bmds_line_edit_get_text(g, _lf.text, &v) == 0) (void)text.assign(Text(v.text, v.length));
                const uint32_t least = bmds_number_field_get_value(g, _lf.least, &v) == 0 ? uint32_t(v.number) : 0;
                const uint32_t most = bmds_number_field_get_value(g, _lf.most, &v) == 0 ? uint32_t(v.number) : 0;
                const Text     all = _listText;
                const auto     name = [&](uint32_t i) {
                        return all.mid(_listed[i].name, _listed[i].nameLength);
                };
                const auto topic = [&](uint32_t i) {
                        return all.mid(_listed[i].topic, _listed[i].topicLength);
                };
                _listShown.clear();
                for (uint32_t i = 0; i < _listed.size(); ++i) {
                        const ListedChannel &c = _listed[i];
                        if (c.users < least || (most > 0 && c.users > most)) continue;
                        if (!containsText(name(i), text) && !containsText(topic(i), text)) continue;
                        (void)_listShown.append(std::move(i));
                }
                const int  column = _lf.sortColumn;
                const bool up = _lf.ascending;
                sortRows(_listShown, [&](uint32_t a, uint32_t b) {
                        int c = 0;
                        if (column == 1)
                                c = _listed[a].users < _listed[b].users   ? -1
                                    : _listed[a].users > _listed[b].users ? 1
                                                                          : 0;
                        else
                                c = compareText(column == 0 ? name(a) : topic(a), column == 0 ? name(b) : topic(b));
                        if (c == 0) c = compareText(name(a), name(b)); // a steady order
                        return up ? c < 0 : c > 0;
                });
                bmds_item_view_clear(g, _lf.view);
                const size_t shown = _listShown.size() < MostShown ? _listShown.size() : MostShown;
                for (size_t r = 0; r < shown; ++r) {
                        const uint32_t i = _listShown[r];
                        char           users[16];
                        snprintf(users, sizeof(users), "%u", unsigned(_listed[i].users));
                        const CString     n(name(i)), t(topic(i));
                        const char *const cells[] = {n.c(), users, t.c()};
                        bmds_details_view_add_row(g, _lf.view, cells, 3);
                }
                bmds_details_view_set_sort_indicator(g, _lf.view, column, !up); // its flag: descending
                char status[160];
                if (_listShown.size() > MostShown)
                        snprintf(status, sizeof(status), "%u of %u match: type more to narrow them",
                                 unsigned(MostShown), unsigned(_listShown.size()));
                else
                        snprintf(status, sizeof(status), "%u of %u channel%s%s", unsigned(_listShown.size()),
                                 unsigned(_listed.size()), _listed.size() == 1 ? "" : "s",
                                 _lf.dropped > 0 ? " (the server sent more than are kept)" : "");
                bmds_label_set_text(g, _lf.status, status);
                return;
        }

        void App::closeChannelList() {
                if (_lf.window == 0) return;
                bmds_gui_destroy(_gui, _lf.window);
                _lf = ListFields();
                _listed.clear();
                _listText.clear();
                _listShown.clear();
                return;
        }

        // ---- Channel Central (§7h)

        void App::openCentral() {
                bmds_gui_t   *g = _gui;
                const Page   *p = _current < _pages.size() ? &_pages[_current] : nullptr;
                const Buffer *b = p != nullptr && p->network != NoNetwork
                                          ? _networks[p->network]->buffers.byId(p->buffer)
                                          : nullptr;
                if (b == nullptr || b->type != Buffer::Type::Channel || !b->joined) {
                        note("Channel Central: show a channel you are in first.");
                        return;
                }
                closeCentral();
                Network        &net = *_networks[p->network];
                CentralFields  &f = _cf;
                const Isupport &is = net.session.isupport();
                f.network = p->network;
                (void)f.channel.assign(b->name);
                f.window = bmds_window_new(g, 0);
                char title[96];
                snprintf(title, sizeof(title), "Channel Central: %.*s", int(b->name.size()), b->name.data());
                bmds_window_set_title(g, f.window, title);
                bmds_window_set_owner(g, f.window, _window);
                bmds_window_set_width(g, f.window, 600);
                bmds_window_set_height(g, f.window, 560);
                bmds_window_set_margin(g, f.window, 8);
                bmds_window_set_spacing(g, f.window, 4);
                const uint32_t form = bmds_form_new(g, f.window);
                bmds_widget_set_stretch(g, form, 0);
                uint32_t row = bmds_form_row_new(g, form);
                bmds_form_row_set_label(g, row, "Topic:");
                f.topic = bmds_line_edit_new(g, row);
                bmds_widget_set_stretch(g, f.topic, 1); // a topic is long: the row's width
                f.topicBy = row;                        // its help: who set it, when
                const uint32_t flags = bmds_form_row_new(g, form);
                bmds_form_row_set_label(g, flags, "Modes:");
                const uint32_t grid = bmds_gui_new(g, "Box", flags);
                bmds_box_set_direction(g, grid, "down");
                for (size_t i = 0; i < 6; ++i) {
                        f.flags[i] = bmds_check_box_new(g, grid);
                        bmds_check_box_set_text(g, f.flags[i], Flags[i].text);
                }
                row = bmds_form_row_new(g, form);
                bmds_form_row_set_label(g, row, "Key (k):");
                f.key = bmds_line_edit_new(g, row);
                row = bmds_form_row_new(g, form);
                bmds_form_row_set_label(g, row, "Limit (l):");
                bmds_form_row_set_help(g, row, "The most people in it; 0: none");
                f.limit = bmds_number_field_new(g, row);
                bmds_number_field_set_minimum(g, f.limit, 0);
                bmds_number_field_set_maximum(g, f.limit, 100000);

                // The lists the network has: bans always, exceptions and invites if CHANMODES says.
                row = across(g, f.window);
                label(g, row, "List:");
                f.which = bmds_combo_box_new(g, row);
                bmds_combo_box_add_item(g, f.which, "Bans (b)");
                if (is.isListMode('e')) bmds_combo_box_add_item(g, f.which, "Exceptions (e)");
                if (is.isListMode('I')) bmds_combo_box_add_item(g, f.which, "Invites (I)");
                f.view = bmds_details_view_new(g, f.window);
                bmds_widget_set_stretch(g, f.view, 1);
                static const char *const columns[] = {"Mask", "Set by", "When"};
                bmds_details_view_set_columns(g, f.view, columns, 3);
                bmds_details_view_set_column_width(g, f.view, 0, 240);
                bmds_details_view_set_column_width(g, f.view, 1, 140);
                bmds_details_view_set_column_width(g, f.view, 2, 140);
                row = across(g, f.window);
                f.mask = bmds_line_edit_new(g, row);
                bmds_widget_set_stretch(g, f.mask, 1);
                f.add = newButton(g, row, "&Add");
                f.remove = newButton(g, row, "&Remove");
                row = across(g, f.window);
                f.status = bmds_label_new(g, row);
                bmds_widget_set_stretch(g, f.status, 1);
                f.ok = newButton(g, row, "OK");
                f.cancel = newButton(g, row, "Cancel");

                // Filled as the session knows the channel; then asked again, the lists too.
                _modeEntries.clear();
                f.mode = 'b';
                centralEvent([&] {
                        Event e;
                        e.kind = Event::Kind::ChannelModes;
                        return e;
                }());
                const Channel *c = net.session.channel(b->name);
                if (c != nullptr) bmds_line_edit_set_text(g, f.topic, CString(c->topic).c());
                String line;
                (void)line.assign("MODE ");
                (void)line.append(b->name);
                (void)net.session.raw(line);
                const char lists[] = {'b', 'e', 'I'};
                for (char m : lists) {
                        if (m != 'b' && !is.isListMode(m)) continue;
                        (void)line.assign("MODE ");
                        (void)line.append(b->name);
                        (void)line.append(' ');
                        (void)line.append(m);
                        (void)net.session.raw(line);
                }
                pump(p->network);
                bmds_window_show(g, f.window);
                return;
        }

        void App::centralEvent(const Event &e) {
                bmds_gui_t    *g = _gui;
                CentralFields &f = _cf;
                if (f.network >= _networks.size()) return;
                Network       &net = *_networks[f.network];
                const Channel *c = net.session.channel(f.channel);
                if (e.kind == Event::Kind::ChannelModes) {
                        // The modes as known; the controls an operator's (the topic too under +t).
                        bool    has[6] = {};
                        String  key;
                        int32_t limit = 0;
                        if (c != nullptr)
                                for (const ChannelMode &m : c->modes) {
                                        for (size_t i = 0; i < 6; ++i)
                                                if (m.mode == Flags[i].mode) has[i] = true;
                                        if (m.mode == 'k') (void)key.assign(m.param);
                                        if (m.mode == 'l') {
                                                limit = 0;
                                                for (char d : m.param.view())
                                                        if (d >= '0' && d <= '9' && limit < 1000000)
                                                                limit = limit * 10 + (d - '0');
                                        }
                                }
                        for (size_t i = 0; i < 6; ++i) bmds_check_box_set_checked(g, f.flags[i], has[i]);
                        bmds_line_edit_set_text(g, f.key, CString(key).c());
                        bmds_number_field_set_value(g, f.limit, limit);
                        const Isupport &is = net.session.isupport();
                        char            mine = 0;
                        if (c != nullptr)
                                for (const Member &m : c->members)
                                        if (sameName(m.nick, net.session.nick(), is.caseMapping()))
                                                mine = m.prefixes[0];
                        const bool half = is.prefixFor('h') != 0;
                        const bool op = mine != 0 && (is.rank(mine) <= is.rank('@') ||
                                                      (half && is.rank(mine) <= is.rank(is.prefixFor('h'))));
                        for (size_t i = 0; i < 6; ++i) bmds_widget_set_enabled(g, f.flags[i], op);
                        const uint32_t opOnly[] = {f.key, f.limit, f.mask, f.add, f.remove};
                        for (uint32_t w : opOnly) bmds_widget_set_enabled(g, w, op);
                        bmds_widget_set_enabled(g, f.topic, op || !has[1]);
                        bmds_label_set_text(g, f.status, op ? "" : "Not an operator here: shown, not changed.");
                        // Who set the topic, when.
                        if (c != nullptr && !c->topicBy.isEmpty()) {
                                char                    by[128];
                                const Format::LocalTime t = Format::localTime(c->topicAt, _utcOffset);
                                if (c->topicAt > 0)
                                        snprintf(by, sizeof(by), "Set by %.*s on %04d-%02d-%02d %02d:%02d",
                                                 int(c->topicBy.size()), c->topicBy.data(), t.year, t.month, t.day,
                                                 t.hour, t.minute);
                                else
                                        snprintf(by, sizeof(by), "Set by %.*s", int(c->topicBy.size()),
                                                 c->topicBy.data());
                                bmds_form_row_set_help(g, f.topicBy, by);
                        }
                        return;
                }
                if (e.kind == Event::Kind::ModeListEntry) {
                        ModeEntry m;
                        m.mode = e.extra.isEmpty() ? 'b' : e.extra.view()[0];
                        for (const ModeEntry &o : _modeEntries)
                                if (o.mode == m.mode && o.mask == e.text.view()) return;
                        (void)m.mask.assign(e.text);
                        (void)m.by.assign(e.nick);
                        m.at = e.number * 1000;
                        (void)_modeEntries.append(std::move(m));
                        return;
                }
                if (e.kind == Event::Kind::ModeListEnd && !e.extra.isEmpty() && e.extra.view()[0] == f.mode)
                        showModeList();
                return;
        }

        void App::showModeList() {
                bmds_gui_t *g = _gui;
                bmds_item_view_clear(g, _cf.view);
                for (const ModeEntry &m : _modeEntries) {
                        if (m.mode != _cf.mode) continue;
                        char when[32] = "";
                        if (m.at > 0) {
                                const Format::LocalTime t = Format::localTime(m.at, _utcOffset);
                                snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d", t.year, t.month, t.day, t.hour,
                                         t.minute);
                        }
                        const CString     mask(m.mask), by(m.by);
                        const char *const cells[] = {mask.c(), by.c(), when};
                        bmds_details_view_add_row(g, _cf.view, cells, 3);
                }
                return;
        }

        void App::applyCentral() {
                bmds_gui_t    *g = _gui;
                CentralFields &f = _cf;
                if (f.network >= _networks.size()) return;
                Network       &net = *_networks[f.network];
                const Channel *c = net.session.channel(f.channel);
                if (c == nullptr) return;
                bmds_gui_value_t v;
                // The topic, if changed.
                String topic;
                if (bmds_line_edit_get_text(g, f.topic, &v) == 0) (void)topic.assign(Text(v.text, v.length));
                if (!(topic == c->topic.view()) && !topic.isEmpty()) (void)net.session.setTopic(f.channel, topic);
                // The modes that changed, in one MODE: flags, then the key and the limit with theirs.
                bool    had[6] = {};
                String  oldKey;
                int32_t oldLimit = 0;
                for (const ChannelMode &m : c->modes) {
                        for (size_t i = 0; i < 6; ++i)
                                if (m.mode == Flags[i].mode) had[i] = true;
                        if (m.mode == 'k') (void)oldKey.assign(m.param);
                        if (m.mode == 'l')
                                for (char d : m.param.view())
                                        if (d >= '0' && d <= '9' && oldLimit < 1000000)
                                                oldLimit = oldLimit * 10 + (d - '0');
                }
                String plus, minus, params;
                for (size_t i = 0; i < 6; ++i) {
                        const bool on = bmds_check_box_get_checked(g, f.flags[i], &v) == 0 && v.number != 0;
                        if (on != had[i]) (void)(on ? plus : minus).append(Flags[i].mode);
                }
                String key;
                if (bmds_line_edit_get_text(g, f.key, &v) == 0) (void)key.assign(Text(v.text, v.length));
                if (!(key == oldKey.view())) {
                        if (!oldKey.isEmpty()) {
                                (void)minus.append('k');
                                (void)params.append(' ');
                                (void)params.append(oldKey);
                        }
                        if (!key.isEmpty() && key.view().find(' ') == Text::NoPos) {
                                (void)plus.append('k');
                                (void)params.append(' ');
                                (void)params.append(key);
                        }
                }
                const int32_t limit = bmds_number_field_get_value(g, f.limit, &v) == 0 ? int32_t(v.number) : oldLimit;
                if (limit != oldLimit) {
                        if (limit == 0) {
                                (void)minus.append('l');
                        } else {
                                char digits[16];
                                snprintf(digits, sizeof(digits), " %d", int(limit));
                                (void)plus.append('l');
                                (void)params.append(digits);
                        }
                }
                if (!plus.isEmpty() || !minus.isEmpty()) {
                        // -k's key goes before +k's: the minus part first.
                        String line;
                        (void)line.assign("MODE ");
                        (void)line.append(f.channel);
                        (void)line.append(' ');
                        if (!minus.isEmpty()) {
                                (void)line.append('-');
                                (void)line.append(minus);
                        }
                        if (!plus.isEmpty()) {
                                (void)line.append('+');
                                (void)line.append(plus);
                        }
                        (void)line.append(params);
                        (void)net.session.raw(line);
                }
                pump(f.network);
                return;
        }

        void App::closeCentral() {
                if (_cf.window == 0) return;
                bmds_gui_destroy(_gui, _cf.window);
                _cf = CentralFields();
                _modeEntries.clear();
                return;
        }

        bool App::channelsEvent(const bmds_gui_event_t &e) {
                bmds_gui_t      *g = _gui;
                bmds_gui_value_t v;
                ListFields      &f = _lf;
                if (f.window != 0) {
                        if (e.source == f.close ||
                            (e.source == f.window && bmds_gui_event_is(g, &e, BMDS_GUI_WINDOW_CLOSED))) {
                                closeChannelList();
                                return true;
                        }
                        if (e.source == f.get) {
                                if (f.receiving) { // Stop: what came is kept; IRC can't stop a list
                                        f.receiving = false;
                                        bmds_button_set_text(g, f.get, "&Get list");
                                        showChannels();
                                        return true;
                                }
                                _listed.clear();
                                _listText.clear();
                                openChannelList(f.network, Text());
                                return true;
                        }
                        if (e.source == f.text || e.source == f.least || e.source == f.most) {
                                if (!f.receiving) showChannels();
                                return true;
                        }
                        if (e.source == f.view && bmds_gui_event_is(g, &e, BMDS_GUI_DETAILS_VIEW_SORT_ASKED) &&
                            e.count == 1) {
                                const int column = int(e.values[0].number);
                                if (column == f.sortColumn)
                                        f.ascending = !f.ascending;
                                else
                                        f.ascending = column != 1; // users: the most first
                                f.sortColumn = column;
                                showChannels();
                                return true;
                        }
                        if (e.source == f.join ||
                            (e.source == f.view && bmds_gui_event_is(g, &e, BMDS_GUI_ITEM_VIEW_ACTIVATED))) {
                                const int32_t row = e.source == f.view && e.count == 1 ? int32_t(e.values[0].number)
                                                    : bmds_item_view_get_current(g, f.view, &v) == 0 ? int32_t(v.number)
                                                                                                     : -1;
                                if (row < 0 || size_t(row) >= _listShown.size() || f.network >= _networks.size())
                                        return true;
                                const ListedChannel &c = _listed[_listShown[size_t(row)]];
                                String               line;
                                (void)line.assign("/join ");
                                (void)line.append(_listText.view().mid(c.name, c.nameLength));
                                runIn(f.network, _networks[f.network]->buffers.network().id, line);
                                showWindow();
                                return true;
                        }
                }
                CentralFields &cf = _cf;
                if (cf.window == 0) return false;
                if (e.source == cf.cancel ||
                    (e.source == cf.window && bmds_gui_event_is(g, &e, BMDS_GUI_WINDOW_CLOSED))) {
                        closeCentral();
                        return true;
                }
                if (e.source == cf.ok) {
                        applyCentral();
                        closeCentral();
                        return true;
                }
                if (e.source == cf.which && bmds_gui_event_is(g, &e, BMDS_GUI_COMBO_BOX_CHANGED) && e.count == 1) {
                        // The combo's items: bans, then those the network has.
                        const Isupport &is = _networks[cf.network]->session.isupport();
                        char            modes[3];
                        size_t          count = 0;
                        modes[count++] = 'b';
                        if (is.isListMode('e')) modes[count++] = 'e';
                        if (is.isListMode('I')) modes[count++] = 'I';
                        const size_t i = size_t(e.values[0].number);
                        if (i < count) cf.mode = modes[i];
                        showModeList();
                        return true;
                }
                if (e.source == cf.add || e.source == cf.remove) {
                        if (cf.network >= _networks.size()) return true;
                        String mask;
                        if (e.source == cf.add) {
                                if (bmds_line_edit_get_text(g, cf.mask, &v) == 0)
                                        (void)mask.assign(Text(v.text, v.length));
                                String full;
                                if (Ignores::normalize(mask, full) != Error::Ok) {
                                        bmds_label_set_text(g, cf.status,
                                                            "A mask is nick!user@host (a * for any part).");
                                        return true;
                                }
                                mask = std::move(full);
                                bmds_line_edit_set_text(g, cf.mask, "");
                        } else {
                                // The chosen row: the nth of the list shown.
                                const int32_t row =
                                        bmds_item_view_get_current(g, cf.view, &v) == 0 ? int32_t(v.number) : -1;
                                int32_t k = 0;
                                for (const ModeEntry &m : _modeEntries)
                                        if (m.mode == cf.mode && k++ == row) (void)mask.assign(m.mask);
                                if (mask.isEmpty()) return true;
                        }
                        String line;
                        (void)line.assign("MODE ");
                        (void)line.append(cf.channel);
                        (void)line.append(e.source == cf.add ? " +" : " -");
                        (void)line.append(cf.mode);
                        (void)line.append(' ');
                        (void)line.append(mask);
                        Network &net = *_networks[cf.network];
                        (void)net.session.raw(line);
                        pump(cf.network);
                        // Shown at once (the server's MODE confirms it in the tab).
                        if (e.source == cf.add) {
                                ModeEntry m;
                                m.mode = cf.mode;
                                m.mask = std::move(mask);
                                (void)m.by.assign(net.session.nick());
                                m.at = now().wall;
                                (void)_modeEntries.append(std::move(m));
                        } else {
                                for (size_t i = 0; i < _modeEntries.size(); ++i)
                                        if (_modeEntries[i].mode == cf.mode && _modeEntries[i].mask == mask.view()) {
                                                _modeEntries.removeAt(i);
                                                break;
                                        }
                        }
                        showModeList();
                        return true;
                }
                return false;
        }

} // namespace irc
