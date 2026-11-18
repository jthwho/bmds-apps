/**
 * @file      app.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "app.h"

#include "format.h"

#include <bmds/app.h>
#include <bmds/sys.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace irc {

        namespace {

                // NUL-ended, for C's calls.
                struct CString {
                                String s;
                                explicit CString(Text t) {
                                        (void)s.assign(t);
                                        (void)s.append('\0');
                                }
                                const char *c() const { return s.data(); }
                };

                // A whole file's bytes into @p out. False: it isn't there (or can't be read).
                bool readFile(const char *path, String &out) {
                        const int fd = open(path, O_RDONLY);
                        if (fd < 0) return false;
                        char buf[1024];
                        out.clear();
                        for (;;) {
                                const ssize_t n = read(fd, buf, sizeof(buf));
                                if (n <= 0) break;
                                (void)out.append(Text(buf, size_t(n)));
                        }
                        close(fd);
                        return true;
                }

                bool writeFile(const char *path, Text bytes) {
                        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                        if (fd < 0) return false;
                        const bool ok = write(fd, bytes.data(), bytes.size()) == ssize_t(bytes.size());
                        close(fd);
                        return ok;
                }

                // A class @p cls described with entry @p entry (a property, method or event)?
                // Its atom among the description's words: what an older firmware lacks.
                bool hasEntry(bmds_gui_t *gui, const char *cls, const char *entry) {
                        const uint8_t *body = nullptr;
                        uint32_t       length = 0;
                        if (bmds_gui_describe(gui, bmds_gui_atom(gui, cls), &body, &length) != 0) return false;
                        if (entry == nullptr) return true;
                        const uint32_t want = bmds_gui_atom(gui, entry);
                        for (uint32_t at = 0; at + 4 <= length; at += 4) {
                                const uint32_t v = uint32_t(body[at]) | uint32_t(body[at + 1]) << 8 |
                                                   uint32_t(body[at + 2]) << 16 | uint32_t(body[at + 3]) << 24;
                                if (v == want) return true;
                        }
                        return false;
                }

                // A path in the data folder, unless it is absolute.
                void inData(String &out, Text dataDir, Text path) {
                        out.clear();
                        if (!path.startsWith("/")) {
                                (void)out.append(dataDir);
                                (void)out.append('/');
                        }
                        (void)out.append(path);
                        return;
                }

                // A member's line in the list: its highest prefix, its nick, "(away)".
                bool memberBefore(const Member &a, const Member &b, const Isupport &is) {
                        const size_t ra = is.rank(a.prefixes[0]), rb = is.rank(b.prefixes[0]);
                        if (ra != rb) return ra < rb;
                        const Text   x = a.nick, y = b.nick;
                        const size_t n = x.size() < y.size() ? x.size() : y.size();
                        for (size_t i = 0; i < n; ++i) {
                                const char p = foldCase(x[i], is.caseMapping()), q = foldCase(y[i], is.caseMapping());
                                if (p != q) return p < q;
                        }
                        return x.size() < y.size();
                }

        } // namespace

        App::~App() {
                for (Network *n : _networks) delete n;
                if (_gui != nullptr) bmds_gui_close(_gui);
        }

        Now App::now() const {
                struct timespec m, w;
                clock_gettime(CLOCK_MONOTONIC, &m);
                clock_gettime(CLOCK_REALTIME, &w);
                Now n;
                n.mono = int64_t(m.tv_sec) * 1000 + m.tv_nsec / 1000000;
                n.wall = int64_t(w.tv_sec) * 1000 + w.tv_nsec / 1000000;
                return n;
        }

        // ---- Starting

        bool App::checkSystem() {
                // TLS sockets (network.md §35), ChatView, the tabs' marks and the input line's keys:
                // all from the same BMDS release (devplan/irc.md §2).
                const bool tls = bmds_sys_has(BMDS_SYS_TLS_CTL) != 0;
                const bool gui = hasEntry(_gui, "ChatView", nullptr) && hasEntry(_gui, "TabPage", "closable") &&
                                 hasEntry(_gui, "LineEdit", "keys");
                if (tls && gui) return true;
                const uint32_t box = bmds_message_box_new(_gui, 0);
                bmds_message_box_set_kind(_gui, box, "error");
                bmds_message_box_set_title(_gui, box, "IRC");
                bmds_message_box_set_text(_gui, box,
                                          "IRC needs a newer BMDS (TLS sockets, ChatView and its tabs): update the "
                                          "firmware.");
                bmds_message_box_open(_gui, box);
                bmds_gui_flush(_gui);
                bmds_gui_event_t e;
                while (bmds_gui_next_event(_gui, &e, -1) == 1)
                        if (e.source == box) break;
                return false;
        }

        void App::loadSettings() {
                char dir[512];
                if (bmds_app_data_dir(dir, sizeof(dir)) != 0) {
                        // Not started as the app (from a shell, say): the same folder by hand.
                        const char *home = getenv("HOME");
                        snprintf(dir, sizeof(dir), "%s/.local", home != nullptr && home[0] != '\0' ? home : "/home");
                        (void)mkdir(dir, 0755);
                        strncat(dir, "/share", sizeof(dir) - strlen(dir) - 1);
                        (void)mkdir(dir, 0755);
                        strncat(dir, "/irc", sizeof(dir) - strlen(dir) - 1);
                        (void)mkdir(dir, 0755);
                }
                (void)_dataDir.assign(dir);
                String path, text;
                inData(path, _dataDir, "networks.ini");
                if (!readFile(CString(path).c(), text)) {
                        (void)text.assign(FirstNetworks);
                        (void)writeFile(CString(path).c(), text);
                }
                Vector<NetworkSettings> nets;
                (void)readNetworks(text, nets, _problems);
                for (NetworkSettings &s : nets) {
                        // Files named in the data folder, unless absolute.
                        String p;
                        if (!s.ca.isEmpty()) inData(p, _dataDir, s.ca), (void)s.ca.assign(p);
                        if (!s.cert.isEmpty()) inData(p, _dataDir, s.cert), (void)s.cert.assign(p);
                        if (!s.key.isEmpty()) inData(p, _dataDir, s.key), (void)s.key.assign(p);
                        if (s.session.version.isEmpty()) (void)s.session.version.assign("BMDS IRC 0.1");
                        auto *n = new (std::nothrow) Network(std::move(s));
                        if (n != nullptr) (void)_networks.append(std::move(n));
                }
                inData(path, _dataDir, "settings.ini");
                if (!readFile(CString(path).c(), text)) text.clear();
                (void)readSettings(text, _settings, _problems);
                if (_settings.logs) {
                        inData(path, _dataDir, _settings.logFolder);
                        _logs.setFolder(path);
                }
                // The zone the times are shown in.
                const time_t t = time(nullptr);
                struct tm    local;
                if (localtime_r(&t, &local) != nullptr) _utcOffset = int32_t(local.tm_gmtoff / 60);
                for (Network *n : _networks) {
                        Buffers::Settings &b = n->buffers.settings();
                        b.showJoins = _settings.showJoins;
                        b.utcOffsetMinutes = _utcOffset;
                        for (const String &w : _settings.highlights) {
                                String copy;
                                if (copy.assign(w) == Error::Ok) (void)b.highlights.append(std::move(copy));
                        }
                }
                return;
        }

        void App::buildWindow() {
                bmds_gui_t *g = _gui;
                _window = bmds_window_new(g, 0);
                bmds_window_set_title(g, _window, "IRC");
                bmds_window_set_icon(g, _window, "mail-send");
                bmds_window_set_width(g, _window, 860);
                bmds_window_set_height(g, _window, 520);
                bmds_window_set_min_width(g, _window, 360);
                bmds_window_set_min_height(g, _window, 240);
                const uint32_t activated = bmds_gui_atom(g, BMDS_GUI_WINDOW_ACTIVATED);
                bmds_gui_listen(g, _window, &activated, 1, 1);

                // Menus: Network, Channel, View, Help (devplan/irc.md §6).
                const uint32_t bar = bmds_menu_bar_new(g, _window);
                const auto     item = [&](uint32_t menu, const char *text, const char *shortcut) {
                        const uint32_t i = bmds_menu_item_new(g, menu);
                        bmds_menu_item_set_text(g, i, text);
                        if (shortcut != nullptr) bmds_menu_item_set_shortcut(g, i, shortcut);
                        return i;
                };
                const uint32_t network = bmds_menu_new(g, bar);
                bmds_menu_set_title(g, network, "&Network");
                _connectItem = item(network, "&Connect", nullptr);
                _disconnectItem = item(network, "&Disconnect", nullptr);
                _quitItem = item(network, "&Quit", "Ctrl+Q");
                const uint32_t channel = bmds_menu_new(g, bar);
                bmds_menu_set_title(g, channel, "&Channel");
                _joinItem = item(channel, "&Join...", "Ctrl+J");
                _partItem = item(channel, "&Part", nullptr);
                _closeItem = item(channel, "&Close", "Ctrl+W");
                const uint32_t view = bmds_menu_new(g, bar);
                bmds_menu_set_title(g, view, "&View");
                _joinsItem = item(view, "&Joins and parts", nullptr);
                bmds_menu_item_set_checked(g, _joinsItem, _settings.showJoins);
                const uint32_t help = bmds_menu_new(g, bar);
                bmds_menu_set_title(g, help, "&Help");
                _aboutItem = item(help, "&About", nullptr);

                _tabs = bmds_tabs_new(g, _window);
                bmds_widget_set_stretch(g, _tabs, 1);

                // The input line: the nick (a click: /nick), the line taking Tab, Up and Down.
                const uint32_t row = bmds_gui_new(g, "Box", _window);
                bmds_box_set_direction(g, row, "across");
                _nick = bmds_button_new(g, row);
                bmds_button_set_text(g, _nick, "(no nick)");
                _entry = bmds_line_edit_new(g, row);
                bmds_widget_set_stretch(g, _entry, 1);
                static const char *const keys[] = {"Tab", "Up", "Down"};
                bmds_line_edit_set_keys(g, _entry, keys, 3);
                const uint32_t edited = bmds_gui_atom(g, BMDS_GUI_LINE_EDIT_EDITED);
                bmds_gui_listen(g, _entry, &edited, 1, 1);

                _status = bmds_status_bar_new(g, _window);
                bmds_status_bar_add_field(g, _status, 0, nullptr);
                _clipboard = bmds_clipboard_new(g, 0);

                // Each network's own tab first; the settings' problems in the first.
                if (_networks.isEmpty()) {
                        Page *p = page(NoNetwork, 0, true);
                        if (p != nullptr) {
                                bmds_chat_view_append(g, p->chat, "--\x0f\tNo network can connect yet.");
                                String path;
                                inData(path, _dataDir, "networks.ini");
                                String line;
                                (void)line.assign("--\x0f\tSet your nick in ");
                                (void)line.append(path);
                                (void)line.append(", and autoconnect = yes, then start IRC again.");
                                bmds_chat_view_append(g, p->chat, CString(line).c());
                        }
                }
                for (uint32_t n = 0; n < _networks.size(); ++n) syncPages(n);
                for (const String &problem : _problems) note(problem);
                bmds_window_show(g, _window);
                bmds_widget_set_focus(g, _entry);
                return;
        }

        int App::run() {
                signal(SIGPIPE, SIG_IGN); // a socket's peer gone: an error, not the end
                srand(unsigned(time(nullptr)));
                _gui = bmds_gui_connect(nullptr);
                if (_gui == nullptr) {
                        fprintf(stderr, "irc: no display (%s)\n", strerror(errno));
                        return 1;
                }
                if (!checkSystem()) return 1;
                loadSettings();
                buildWindow();
                for (uint32_t n = 0; n < _networks.size(); ++n)
                        if (_networks[n]->settings.autoconnect) connect(n);
                updateStatus();
                while (!_done) pollOnce();
                _logs.flush();
                return 0;
        }

        // ---- The loop

        void App::pollOnce() {
                // The GUI's events read already (a getter's answer brings them along) first.
                bmds_gui_event_t e;
                while (!_done && bmds_gui_next_event(_gui, &e, 0) == 1) gui(e);
                if (_done) return;
                bmds_gui_flush(_gui);

                struct pollfd fds[1 + 8];
                uint32_t      owner[1 + 8];
                size_t        count = 0;
                fds[count] = {bmds_gui_fd(_gui), POLLIN, 0};
                owner[count++] = NoNetwork;
                Now        t = now();
                int64_t    wait = -1;
                const auto sooner = [&](int64_t ms) {
                        if (ms >= 0 && (wait < 0 || ms < wait)) wait = ms;
                };
                for (uint32_t n = 0; n < _networks.size() && count < 9; ++n) {
                        Network &net = *_networks[n];
                        if (net.connection.fd() >= 0) {
                                fds[count] = {net.connection.fd(), net.connection.events(), 0};
                                owner[count++] = n;
                        }
                        if (net.connection.deadline() >= 0) sooner(net.connection.deadline() - t.mono);
                        if (net.connection.state() == Connection::State::Up) sooner(net.session.sendWaitMs(t.mono));
                        sooner(net.session.tickWaitMs(t.mono));
                        if (net.reconnectAt >= 0) sooner(net.reconnectAt - t.mono);
                }
                if (_flushAt >= 0) sooner(_flushAt - t.mono);
                if (wait > 600000) wait = 600000;
                const int ready = poll(fds, nfds_t(count), wait < 0 ? -1 : int(wait < 0 ? 0 : wait));
                if (ready < 0 && errno != EINTR) {
                        _done = true;
                        return;
                }
                t = now();
                for (size_t i = 1; i < count; ++i) serviceNetwork(owner[i], fds[i].revents);
                for (uint32_t n = 0; n < _networks.size(); ++n) {
                        Network &net = *_networks[n];
                        // Timeouts (a connect taking too long) and reconnecting.
                        if (net.connection.deadline() >= 0 && t.mono >= net.connection.deadline()) serviceNetwork(n, 0);
                        if (net.reconnectAt >= 0 && t.mono >= net.reconnectAt) {
                                net.reconnectAt = -1;
                                connect(n);
                        }
                        if (net.session.state() != Session::State::Disconnected) net.session.tick(t);
                        pump(n);
                }
                if (_flushAt >= 0 && t.mono >= _flushAt) {
                        _logs.flush();
                        _flushAt = -1;
                }
                return;
        }

        void App::connect(uint32_t n) {
                Network &net = *_networks[n];
                net.wanted = true;
                net.reconnectAt = -1;
                if (net.connection.state() != Connection::State::Idle) return;
                const NetworkSettings &s = net.settings;
                char                   what[160];
                snprintf(what, sizeof(what), "Connecting to %.*s:%u%s", int(s.host.size()), s.host.data(),
                         unsigned(s.port), s.tls ? " (TLS)" : " (no TLS)");
                Vector<Line> lines;
                (void)net.buffers.note(net.buffers.network().id, what, now().wall, lines);
                show(n, lines);
                const bool external = s.session.login == Login::SaslExternal;
                if (!net.connection.start(s.host, s.port, s.tls, s.ca, external ? s.cert.view() : Text(),
                                          external ? s.key.view() : Text(), now().mono))
                        disconnect(n, CString(net.connection.why()).c(), true);
                updateStatus();
                return;
        }

        void App::disconnect(uint32_t n, const char *why, bool again) {
                Network &net = *_networks[n];
                net.connection.stop();
                if (net.session.state() != Session::State::Disconnected) {
                        net.session.disconnected();
                        net.buffers.disconnected();
                }
                net.reader.reset();
                Vector<Line> lines;
                char         text[256];
                const Now    t = now();
                if (again && net.wanted) {
                        const int64_t delay = reconnectDelayMs(net.attempts++, uint32_t(rand()));
                        net.reconnectAt = t.mono + delay;
                        snprintf(text, sizeof(text), "Disconnected: %s. Connecting again in %d s.", why,
                                 int((delay + 500) / 1000));
                } else {
                        net.wanted = false;
                        snprintf(text, sizeof(text), "Disconnected: %s.", why);
                }
                (void)net.buffers.note(net.buffers.network().id, text, t.wall, lines);
                show(n, lines);
                syncPages(n);
                for (size_t i = 0; i < net.buffers.count(); ++i)
                        if (net.buffers.at(i).type == Buffer::Type::Channel) refreshMembers(n, net.buffers.at(i).name);
                updateStatus();
                return;
        }

        void App::serviceNetwork(uint32_t n, short revents) {
                Network  &net = *_networks[n];
                char      buf[2048];
                size_t    got = 0;
                const Now t = now();
                switch (net.connection.service(revents, t.mono, buf, sizeof(buf), got)) {
                        case Connection::Result::Nothing: break;
                        case Connection::Result::Connected: {
                                Vector<Line> lines;
                                String       text;
                                (void)text.assign("Connected (");
                                (void)text.append(net.connection.security());
                                if (!net.connection.cipher().isEmpty()) {
                                        (void)text.append(", ");
                                        (void)text.append(net.connection.cipher());
                                }
                                (void)text.append("); registering");
                                (void)net.buffers.note(net.buffers.network().id, text, t.wall, lines);
                                show(n, lines);
                                net.reader.reset();
                                net.session.connected(t);
                                break;
                        }
                        case Connection::Result::Data:
                                net.reader.feed(Text(buf, got), [&](Text line) { net.session.received(line, t); });
                                break;
                        case Connection::Result::Closed:
                        case Connection::Result::Failed: disconnect(n, CString(net.connection.why()).c(), true); break;
                }
                pump(n);
                updateStatus();
                return;
        }

        void App::pump(uint32_t n) {
                Network  &net = *_networks[n];
                const Now t = now();
                if (net.connection.state() == Connection::State::Up) {
                        String line;
                        while (net.session.takeLine(t.mono, line)) (void)net.connection.send(line);
                }
                Event e;
                bool  any = false;
                while (net.session.takeEvent(e)) {
                        Vector<Line> lines;
                        (void)net.buffers.apply(e, net.session, lines);
                        show(n, lines);
                        react(n, e);
                        any = true;
                }
                if (any) syncPages(n);
                return;
        }

        void App::react(uint32_t n, const Event &e) {
                Network &net = *_networks[n];
                switch (e.kind) {
                        case Event::Kind::Registered:
                                net.attempts = 0;
                                updateStatus();
                                break;
                        case Event::Kind::Nick:
                                if ((e.flags & Event::Self) != 0) updateStatus();
                                break;
                        case Event::Kind::Members: refreshMembers(n, e.target); break;
                        case Event::Kind::Topic: refreshTopic(n, e.target); break;
                        case Event::Kind::Join:
                                if ((e.flags & Event::Self) != 0) {
                                        // Our join: its tab shown (one we asked for).
                                        if (const Buffer *b =
                                                    net.buffers.find(e.target, net.session.isupport().caseMapping());
                                            b != nullptr) {
                                                syncPages(n);
                                                for (size_t i = 0; i < _pages.size(); ++i)
                                                        if (_pages[i].network == n && _pages[i].buffer == b->id)
                                                                setCurrent(i, true);
                                        }
                                }
                                break;
                        case Event::Kind::Timeout: disconnect(n, "no answer from the server", true); break;
                        case Event::Kind::Lag: updateStatus(); break;
                        default: break;
                }
                return;
        }

        // ---- Pages

        Page *App::page(uint32_t n, uint32_t buffer, bool make) {
                for (Page &p : _pages)
                        if (p.network == n && p.buffer == buffer) return &p;
                if (!make) return nullptr;
                bmds_gui_t   *g = _gui;
                const Buffer *b = n == NoNetwork ? nullptr : _networks[n]->buffers.byId(buffer);
                Page          p;
                p.network = n;
                p.buffer = buffer;
                p.tab = bmds_tab_page_new(g, _tabs);
                if (b != nullptr && b->type == Buffer::Type::Channel) {
                        p.topic = bmds_label_new(g, p.tab);
                        const uint32_t split = bmds_splitter_new(g, p.tab);
                        bmds_splitter_set_direction(g, split, "across");
                        bmds_widget_set_stretch(g, split, 1);
                        p.chat = bmds_chat_view_new(g, split);
                        p.members = bmds_list_view_new(g, split);
                        bmds_splitter_set_position(g, split, 640);
                } else {
                        p.chat = bmds_chat_view_new(g, p.tab);
                        bmds_widget_set_stretch(g, p.chat, 1);
                }
                bmds_tab_page_set_closable(g, p.tab, b != nullptr && b->type != Buffer::Type::Network);
                if (_pages.append(std::move(p)) != Error::Ok) return nullptr;
                Page &made = _pages.last();
                if (b != nullptr && b->type == Buffer::Type::Channel) {
                        refreshTopic(n, b->name);
                        refreshMembers(n, b->name);
                }
                return &made;
        }

        Page *App::pageOf(uint32_t id) {
                for (Page &p : _pages)
                        if (p.tab == id || p.chat == id || p.members == id) return &p;
                return nullptr;
        }

        void App::show(uint32_t n, Vector<Line> &lines) {
                Network &net = *_networks[n];
                for (const Line &l : lines) {
                        Page *p = page(n, l.buffer, true);
                        if (p == nullptr) continue;
                        bmds_chat_view_append(_gui, p->chat, CString(l.view).c());
                        const Buffer *b = net.buffers.byId(l.buffer);
                        _logs.write(net.settings.name, l.buffer, b != nullptr ? b->name.view() : Text(), l, _utcOffset);
                        if (_flushAt < 0) _flushAt = now().mono + Logs::FlushMs;
                }
                lines.clear();
                return;
        }

        void App::syncPages(uint32_t n) {
                Network &net = *_networks[n];
                for (size_t i = 0; i < net.buffers.count(); ++i) {
                        const Buffer &b = net.buffers.at(i);
                        Page         *p = page(n, b.id, true);
                        if (p == nullptr) continue;
                        // The tab: its name (the network's own: the network's), the unread count, the attention.
                        const Text title = b.type == Buffer::Type::Network ? net.settings.name.view() : b.name.view();
                        if (!(p->title == title)) {
                                (void)p->title.assign(title);
                                bmds_tab_page_set_title(_gui, p->tab, CString(title).c());
                        }
                        char badge[8] = {};
                        if (b.unread > 0)
                                snprintf(badge, sizeof(badge), "%u", b.unread > 999 ? 999u : unsigned(b.unread));
                        if (!(p->badge == Text(badge))) {
                                (void)p->badge.assign(badge);
                                bmds_tab_page_set_badge(_gui, p->tab, badge);
                        }
                        if (p->attention != b.attention) {
                                p->attention = b.attention;
                                bmds_tab_page_set_attention(_gui, p->tab, b.attention);
                        }
                }
                return;
        }

        void App::closePage(size_t index) {
                if (index >= _pages.size()) return;
                Page &p = _pages[index];
                if (p.network != NoNetwork) {
                        Network &net = *_networks[p.network];
                        _logs.close(net.settings.name, p.buffer);
                        net.buffers.close(p.buffer);
                }
                bmds_gui_destroy(_gui, p.tab); // its widgets with it
                _pages.removeAt(index);
                // The tab the widget made current (the next, or the one before at the end).
                bmds_gui_value_t v;
                _current = bmds_tabs_get_current(_gui, _tabs, &v) == 0 ? size_t(v.number) : 0;
                setCurrent(_current, false);
                return;
        }

        void App::setCurrent(size_t index, bool tell) {
                if (index >= _pages.size()) return;
                _current = index;
                if (tell) bmds_tabs_set_current(_gui, _tabs, int32_t(index));
                // In view in this network (read if the window has the focus); none in the others.
                const Page &p = _pages[index];
                for (uint32_t n = 0; n < _networks.size(); ++n) {
                        _networks[n]->buffers.setActive(n == p.network ? p.buffer : 0, _focused);
                        syncPages(n);
                }
                _completer.reset();
                updateStatus();
                return;
        }

        void App::refreshMembers(uint32_t n, Text name) {
                Network       &net = *_networks[n];
                const Buffer  *b = net.buffers.find(name, net.session.isupport().caseMapping());
                Page          *p = b != nullptr ? page(n, b->id, false) : nullptr;
                const Channel *c = net.session.channel(name);
                if (p == nullptr || p->members == 0) return;
                // By rank, then name; "(away)" after the away.
                const Isupport        &is = net.session.isupport();
                Vector<const Member *> order;
                if (c != nullptr)
                        for (const Member &m : c->members) {
                                size_t at = order.size();
                                while (at > 0 && memberBefore(m, *order[at - 1], is)) --at;
                                const Member *mp = &m;
                                (void)order.insert(at, std::move(mp));
                        }
                Vector<String>       texts;
                Vector<const char *> items;
                for (const Member *m : order) {
                        String s;
                        if (m->prefixes[0] != '\0') (void)s.append(m->prefixes[0]);
                        (void)s.append(m->nick);
                        if (m->away) (void)s.append(" (away)");
                        (void)s.append('\0');
                        (void)texts.append(std::move(s));
                }
                for (const String &s : texts) {
                        const char *c2 = s.data();
                        (void)items.append(std::move(c2));
                }
                bmds_item_view_set_items(_gui, p->members, items.isEmpty() ? nullptr : items.begin(),
                                         uint32_t(items.size()));
                return;
        }

        void App::refreshTopic(uint32_t n, Text name) {
                Network       &net = *_networks[n];
                const Buffer  *b = net.buffers.find(name, net.session.isupport().caseMapping());
                Page          *p = b != nullptr ? page(n, b->id, false) : nullptr;
                const Channel *c = net.session.channel(name);
                if (p == nullptr || p->topic == 0) return;
                String plain;
                if (c != nullptr) (void)Format::strip(c->topic, plain);
                bmds_label_set_text(_gui, p->topic, CString(plain).c());
                return;
        }

        void App::updateStatus() {
                if (_pages.isEmpty() || _current >= _pages.size()) return;
                const Page &p = _pages[_current];
                if (p.network == NoNetwork) {
                        bmds_status_bar_set_field(_gui, _status, 0, "No network");
                        return;
                }
                const Network &net = *_networks[p.network];
                char           text[200];
                const char    *state = "disconnected";
                switch (net.connection.state()) {
                        case Connection::State::Idle:
                                state = net.reconnectAt >= 0 ? "waiting to connect again" : "disconnected";
                                break;
                        case Connection::State::Resolving: state = "looking up the server"; break;
                        case Connection::State::Connecting: state = "connecting"; break;
                        case Connection::State::Handshaking: state = "TLS handshake"; break;
                        case Connection::State::Up:
                                state = net.session.state() == Session::State::Registered ? "connected" : "registering";
                                break;
                }
                int len = snprintf(text, sizeof(text), "%.*s: %s", int(net.settings.name.size()),
                                   net.settings.name.data(), state);
                if (net.connection.state() == Connection::State::Up && len > 0 && size_t(len) < sizeof(text)) {
                        len += snprintf(text + len, sizeof(text) - size_t(len), " (%.*s",
                                        int(net.connection.security().size()), net.connection.security().data());
                        if (!net.session.account().isEmpty() && size_t(len) < sizeof(text))
                                len += snprintf(text + len, sizeof(text) - size_t(len), ", logged in as %.*s",
                                                int(net.session.account().size()), net.session.account().data());
                        if (size_t(len) < sizeof(text)) len += snprintf(text + len, sizeof(text) - size_t(len), ")");
                        if (net.session.lagMs() >= 0 && size_t(len) < sizeof(text))
                                snprintf(text + len, sizeof(text) - size_t(len), ", lag %d ms",
                                         int(net.session.lagMs()));
                }
                bmds_status_bar_set_field(_gui, _status, 0, text);
                const Text nick = net.session.nick().isEmpty() ? net.settings.session.nick.view() : net.session.nick();
                bmds_button_set_text(_gui, _nick, nick.isEmpty() ? "(no nick)" : CString(nick).c());
                return;
        }

        // ---- What the user does

        void App::note(Text text) {
                if (_pages.isEmpty()) return;
                const Page &p = _pages[_current < _pages.size() ? _current : 0];
                String      line;
                (void)line.assign("--\x0f\t");
                (void)line.append(text);
                bmds_chat_view_append(_gui, p.chat, CString(line).c());
                return;
        }

        void App::typed(Text text) {
                if (!text.isEmpty()) {
                        String h;
                        if (h.assign(text) == Error::Ok) (void)_history.append(std::move(h));
                        if (_history.size() > 100) _history.removeAt(0);
                }
                _historyAt = _history.size();
                _completer.reset();
                if (_pages.isEmpty() || _current >= _pages.size()) return;
                // Copied: a command may close or add tabs.
                const struct {
                                uint32_t network, buffer;
                } p = {_pages[_current].network, _pages[_current].buffer};
                if (p.network == NoNetwork) {
                        note("No network to send it to: set one up in networks.ini");
                        return;
                }
                Network     &net = *_networks[p.network];
                Vector<Line> lines;
                Outcome      outcome;
                (void)runCommand(text, net.session, net.buffers, p.buffer, now(), lines, outcome);
                show(p.network, lines);
                switch (outcome.action) {
                        case Outcome::Action::None: break;
                        case Outcome::Action::Show:
                                syncPages(p.network);
                                for (size_t i = 0; i < _pages.size(); ++i)
                                        if (_pages[i].network == p.network && _pages[i].buffer == outcome.buffer)
                                                setCurrent(i, true);
                                break;
                        case Outcome::Action::Clear:
                                if (Page *pg = page(p.network, outcome.buffer, false); pg != nullptr)
                                        bmds_chat_view_clear(_gui, pg->chat);
                                break;
                        case Outcome::Action::Close:
                                for (size_t i = 0; i < _pages.size(); ++i)
                                        if (_pages[i].network == p.network && _pages[i].buffer == outcome.buffer) {
                                                closePage(i);
                                                break;
                                        }
                                break;
                        case Outcome::Action::Quit: net.wanted = false; break;
                }
                pump(p.network);
                return;
        }

        void App::complete() {
                if (_pages.isEmpty() || _current >= _pages.size() || _pages[_current].network == NoNetwork) return;
                const Page      &p = _pages[_current];
                Network         &net = *_networks[p.network];
                bmds_gui_value_t text, cursor;
                if (bmds_line_edit_get_text(_gui, _entry, &text) != 0 ||
                    bmds_line_edit_get_cursor(_gui, _entry, &cursor) != 0)
                        return;
                String line;
                (void)line.assign(Text(text.text, text.length));
                size_t at = size_t(cursor.number);
                if (!_completer.complete(line, at, net.session, net.buffers, p.buffer)) return;
                bmds_line_edit_set_text(_gui, _entry, CString(line).c());
                bmds_line_edit_set_cursor(_gui, _entry, int32_t(at));
                return;
        }

        void App::history(int step) {
                if (_history.isEmpty()) return;
                if (step < 0 && _historyAt > 0) --_historyAt;
                if (step > 0 && _historyAt < _history.size()) ++_historyAt;
                const Text line = _historyAt < _history.size() ? _history[_historyAt].view() : Text();
                bmds_line_edit_set_text(_gui, _entry, CString(line).c());
                _completer.reset();
                return;
        }

        void App::quit() {
                // A QUIT to each network up, given a moment to go out.
                bool any = false;
                for (uint32_t n = 0; n < _networks.size(); ++n) {
                        Network &net = *_networks[n];
                        net.wanted = false;
                        if (net.connection.state() != Connection::State::Up) continue;
                        (void)net.session.quit("");
                        pump(n);
                        any = true;
                }
                for (int i = 0; any && i < 10; ++i) {
                        any = false;
                        for (uint32_t n = 0; n < _networks.size(); ++n) {
                                Network &net = *_networks[n];
                                if (net.connection.state() != Connection::State::Up) continue;
                                struct pollfd f = {net.connection.fd(), net.connection.events(), 0};
                                if ((f.events & POLLOUT) == 0) continue;
                                any = true;
                                if (poll(&f, 1, 100) > 0) {
                                        char   buf[512];
                                        size_t got = 0;
                                        (void)net.connection.service(f.revents, now().mono, buf, sizeof(buf), got);
                                }
                        }
                }
                for (Network *n : _networks) n->connection.stop();
                _done = true;
                return;
        }

        void App::menu(uint32_t id) {
                const Page    *p = _pages.isEmpty() || _current >= _pages.size() ? nullptr : &_pages[_current];
                const uint32_t n = p != nullptr ? p->network : NoNetwork;
                if (id == _connectItem) {
                        if (n != NoNetwork) connect(n);
                } else if (id == _disconnectItem) {
                        if (n != NoNetwork) {
                                Network &net = *_networks[n];
                                net.wanted = false;
                                net.reconnectAt = -1;
                                if (net.connection.state() == Connection::State::Up) {
                                        (void)net.session.quit("");
                                        pump(n);
                                } else {
                                        disconnect(n, "as asked", false);
                                }
                        }
                } else if (id == _quitItem) {
                        quit();
                } else if (id == _joinItem) {
                        bmds_line_edit_set_text(_gui, _entry, "/join #");
                        bmds_widget_set_focus(_gui, _entry);
                } else if (id == _partItem) {
                        typed("/part");
                } else if (id == _closeItem) {
                        typed("/close");
                } else if (id == _joinsItem) {
                        _settings.showJoins = !_settings.showJoins;
                        bmds_menu_item_set_checked(_gui, _joinsItem, _settings.showJoins);
                        for (Network *net : _networks) net->buffers.settings().showJoins = _settings.showJoins;
                } else if (id == _aboutItem) {
                        const uint32_t box = bmds_message_box_new(_gui, _window);
                        bmds_message_box_set_kind(_gui, box, "information");
                        bmds_message_box_set_title(_gui, box, "About IRC");
                        bmds_message_box_set_text(_gui, box,
                                                  "IRC 0.1 for BMDS: chat on IRC networks, over TLS.\n"
                                                  "Settings: networks.ini and settings.ini in the app's data folder.\n"
                                                  "MIT licence. https://github.com/jthwho/bmds-apps");
                        bmds_message_box_open(_gui, box);
                }
                return;
        }

        void App::gui(const bmds_gui_event_t &e) {
                bmds_gui_t *g = _gui;
                if (e.source == _window) {
                        if (bmds_gui_event_is(g, &e, BMDS_GUI_WINDOW_CLOSED)) {
                                quit();
                        } else if (bmds_gui_event_is(g, &e, BMDS_GUI_WINDOW_ACTIVATED) && e.count == 1) {
                                _focused = e.values[0].number != 0;
                                setCurrent(_current, false);
                        }
                        return;
                }
                if (e.source == _entry) {
                        if (bmds_gui_event_is(g, &e, BMDS_GUI_LINE_EDIT_ACCEPTED) && e.count == 1) {
                                const String text = [&] {
                                        String s;
                                        (void)s.assign(Text(e.values[0].text, e.values[0].length));
                                        return s;
                                }();
                                bmds_line_edit_set_text(g, _entry, "");
                                typed(text);
                        } else if (bmds_gui_event_is(g, &e, BMDS_GUI_LINE_EDIT_KEY_PRESSED) && e.count == 2) {
                                const char *key = bmds_gui_atom_name(g, uint32_t(e.values[0].number));
                                if (key != nullptr && strcmp(key, "Tab") == 0) complete();
                                if (key != nullptr && strcmp(key, "Up") == 0) history(-1);
                                if (key != nullptr && strcmp(key, "Down") == 0) history(1);
                        } else if (bmds_gui_event_is(g, &e, BMDS_GUI_LINE_EDIT_EDITED)) {
                                _completer.reset();
                        }
                        return;
                }
                if (e.source == _nick) {
                        bmds_line_edit_set_text(g, _entry, "/nick ");
                        bmds_widget_set_focus(g, _entry);
                        return;
                }
                if (e.source == _tabs && bmds_gui_event_is(g, &e, BMDS_GUI_TABS_CHANGED) && e.count == 1) {
                        setCurrent(size_t(e.values[0].number), false);
                        return;
                }
                if (bmds_gui_event_is(g, &e, BMDS_GUI_MENU_ITEM_CHOSEN)) {
                        menu(e.source);
                        return;
                }
                Page *p = pageOf(e.source);
                if (p == nullptr) return;
                if (e.source == p->tab && bmds_gui_event_is(g, &e, BMDS_GUI_TAB_PAGE_CLOSE_REQUESTED)) {
                        // As /close typed in it: a channel left, then its tab gone.
                        for (size_t i = 0; i < _pages.size(); ++i)
                                if (&_pages[i] == p) {
                                        setCurrent(i, true);
                                        typed("/close");
                                        break;
                                }
                        return;
                }
                if (e.source == p->chat && bmds_gui_event_is(g, &e, BMDS_GUI_CHAT_VIEW_LINK_ACTIVATED) &&
                    e.count == 1) {
                        const Text url(e.values[0].text, e.values[0].length);
                        if ((url.startsWith("irc://") || url.startsWith("ircs://")) && p->network != NoNetwork) {
                                // irc://host/channel: joined on this network.
                                const size_t slash = url.find('/', url.find(':') + 3);
                                Text         channel = slash != Text::NoPos ? url.mid(slash + 1) : Text();
                                const size_t end = channel.find('?');
                                if (end != Text::NoPos) channel = channel.left(end);
                                if (!channel.isEmpty()) {
                                        String cmd;
                                        (void)cmd.assign("/join ");
                                        if (!channel.startsWith("#")) (void)cmd.append('#');
                                        (void)cmd.append(channel);
                                        typed(cmd);
                                        return;
                                }
                        }
                        // No browser on BMDS: the link copied, to paste where it is wanted.
                        bmds_clipboard_copy_text(g, _clipboard, "clipboard", CString(url).c());
                        String line;
                        (void)line.assign("Link copied: ");
                        (void)line.append(url);
                        note(line);
                        return;
                }
                return;
        }

} // namespace irc
