/**
 * @file      server.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The VNC server's BMDS side (devplan/vnc.md §5b-§5e): the socket, the GUI connection's SHELL
 * (the screen watched and captured, input injected), the hardware JPEG through image_encode, the
 * tray icon and the notifications. The protocol and the encodings are the core's (vnc/core).
 */

#include "server.h"

#include <bmds/guiclasses.h>
#include <bmds/sys.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <new>

namespace vnc {

        namespace {

                constexpr size_t      InBytes = 16 * 1024;
                constexpr size_t      OutBytes = 1024 * 1024;     // an update at most (bigger ones go in parts)
                constexpr size_t      CaptureBytes = 1024 * 1024; // a capture's pixels (a 2048-wide tile row at least)
                constexpr uint32_t    MaxRects = 64;
                constexpr uint16_t    BandRows = 16;            // a rectangle is encoded in bands of these
                constexpr uint32_t    MaxRectsAnUpdate = 60000; // under the header's 16 bits
                constexpr uint32_t    FailuresAllowed = 3;      // wrong passwords from one address
                constexpr uint64_t    FailureWindowMs = 60'000; // within this long
                constexpr uint64_t    RefusedMs = 60'000;       // then refused for this long
                constexpr const char *TrayIconName = "preferences-system-network";

                volatile sig_atomic_t stopAsked = 0;

                void onTerm(int) {
                        stopAsked = 1;
                        return;
                }

                uint64_t nowMs() {
                        struct timespec t;
                        clock_gettime(CLOCK_MONOTONIC, &t);
                        return uint64_t(t.tv_sec) * 1000u + uint64_t(t.tv_nsec) / 1000000u;
                }

                // RFB's buttons (1 left, 2 middle, 4 right) as BMDS's (1 left, 2 right, 4 middle).
                uint8_t bmdsButtons(uint8_t rfb) {
                        return uint8_t((rfb & 1u) | ((rfb & 4u) ? 2u : 0u) | ((rfb & 2u) ? 4u : 0u));
                }

        } // namespace

        struct Server::Viewer {
                        Viewer(const Session::Config &config, Handler &handler) : session(config, handler) {}

                        int         fd = -1;
                        uint32_t    address = 0; // IPv4, network order
                        char        name[32] = {};
                        uint64_t    sinceMs = 0;
                        Session     session;
                        Encoder     encoder;
                        KeyState    keys;
                        uint8_t     buttons = 0; // RFB's, as last sent
                        bool        watching = false;
                        bool        requested = false; // an update asked for and not sent
                        bool        full = false;      // the whole screen asked for
                        bool        changed = false;   // the screen changed since the last capture
                        bool        more = false;      // the last capture left changes behind
                        uint8_t     in[InBytes];
                        size_t      inSize = 0;
                        uint8_t    *out = nullptr;
                        size_t      outSize = 0;
                        size_t      outSent = 0;
                        uint8_t    *pixels = nullptr; // the last capture's rectangles, packed
                        bmds_rect_t rects[MaxRects];
                        size_t      offsets[MaxRects];
                        uint32_t    rectCount = 0;
                        uint32_t    rectAt = 0; // the next rectangle to send
                        uint16_t    bandAt = 0; // and its next row
                        uint16_t    capturedWidth = 0;
                        uint16_t    capturedHeight = 0;

                        // A writer appending to what is still to be sent.
                        Writer appender() { return Writer(out + outSize, OutBytes - outSize); }
        };

        Server::Server(const Settings &settings) : _settings(settings) {}

        Server::~Server() {
                if (_viewer != nullptr) drop("the server stopped");
                if (_listen >= 0) close(_listen);
                if (_gui != nullptr) bmds_gui_close(_gui);
        }

        // ---- Starting

        bool Server::connectGui() {
                // From autoexec.sh the desktop may come later: try again a while (devplan/vnc.md §5e).
                const uint64_t until = nowMs() + uint64_t(_settings.startWaitS) * 1000u;
                unsigned       waitMs = 250;
                for (;;) {
                        _gui = bmds_gui_connect(nullptr);
                        if (_gui != nullptr || nowMs() >= until || stopAsked) break;
                        usleep(waitMs * 1000u);
                        if (waitMs < 4000) waitMs *= 2;
                }
                return _gui != nullptr;
        }

        bool Server::listen() {
                _listen = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
                if (_listen < 0) return false;
                const int on = 1;
                (void)setsockopt(_listen, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
                sockaddr_in a = {};
                a.sin_family = AF_INET;
                a.sin_port = htons(_settings.port);
                a.sin_addr.s_addr = htonl(_settings.lan ? INADDR_ANY : INADDR_LOOPBACK);
                if (bind(_listen, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || ::listen(_listen, 2) != 0)
                        return false;
                snprintf(_address, sizeof(_address), "%s:%u", _settings.lan ? "every interface" : "127.0.0.1",
                         unsigned(_settings.port));
                return true;
        }

        int Server::start(int ready) {
                auto say = [ready](bool ok, const char *text) {
                        if (ready >= 0) {
                                char      line[160];
                                const int n = snprintf(line, sizeof(line), "%c %s\n", ok ? '0' : '1', text);
                                (void)write(ready, line, size_t(n));
                                close(ready);
                        } else {
                                fprintf(ok ? stdout : stderr, "vncserver: %s\n", text);
                        }
                };
                struct sigaction sa = {};
                sa.sa_handler = onTerm;
                (void)sigaction(SIGTERM, &sa, nullptr);
                (void)sigaction(SIGINT, &sa, nullptr);
                signal(SIGPIPE, SIG_IGN);
                if (!connectGui()) {
                        say(false, "no desktop to share (the GUI service isn't running)");
                        return 1;
                }
                if (!bmds_gui_has_extension(_gui, "SHELL") || !bmds_sys_has(BMDS_SYS_GUI_SCREEN_CAPTURE)) {
                        say(false, "not allowed to share the screen: add vncserver to the setting gui/shell-clients"
                                   " (Settings > Desktop), on a BMDS with screen capture");
                        return 1;
                }
                bmds_gui_value_t screen;
                if (bmds_gui_display(_gui, "screen", &screen) == 0) {
                        _width = uint16_t(screen.width);
                        _height = uint16_t(screen.height);
                }
                if (!listen()) {
                        char why[96];
                        snprintf(why, sizeof(why), "can't listen on port %u: %s (running already?)",
                                 unsigned(_settings.port), strerror(errno));
                        say(false, why);
                        return 1;
                }
                _shell = bmds_shell_new(_gui, 0);
                _tray = bmds_tray_icon_new(_gui, 0);
                bmds_tray_icon_set_icon(_gui, _tray, TrayIconName);
                bmds_tray_icon_set_category(_gui, _tray, "application");
                _trayMenu = bmds_menu_new(_gui, _tray); // a tray icon's menu: its context menu
                _menuViewer = bmds_menu_item_new(_gui, _trayMenu);
                bmds_menu_item_set_enabled(_gui, _menuViewer, 0);
                _menuDisconnect = bmds_menu_item_new(_gui, _trayMenu);
                bmds_menu_item_set_text(_gui, _menuDisconnect, "&Disconnect");
                bmds_menu_add_separator(_gui, _trayMenu);
                _menuStop = bmds_menu_item_new(_gui, _trayMenu);
                bmds_menu_item_set_text(_gui, _menuStop, "&Stop Server");
                refreshTray();
                bmds_tray_icon_set_visible(_gui, _tray, 1);
                char body[128];
                if (_settings.lan) {
                        // On the network without TLS: said where it can't be missed.
                        snprintf(body, sizeof(body),
                                 "Listening on %s, not encrypted: anyone on the network can watch the session",
                                 _address);
                        notify("VNC server started", body, "normal", false);
                } else {
                        snprintf(body, sizeof(body), "Listening on %s%s", _address,
                                 _settings.password[0] != '\0' ? "" : " (no password)");
                        notify("VNC server started", body, "low", false);
                }
                (void)bmds_gui_flush(_gui);
                say(true, body);
                return 0;
        }

        // ---- The loop

        int Server::run() {
                while (!_stopping && !stopAsked) {
                        // Events the library holds already first: poll wouldn't see them.
                        guiEvents();
                        update();
                        (void)bmds_gui_flush(_gui);
                        pollfd fds[3];
                        nfds_t n = 0;
                        fds[n++] = pollfd{bmds_gui_fd(_gui), POLLIN, 0};
                        fds[n++] = pollfd{_listen, POLLIN, 0};
                        if (_viewer != nullptr) {
                                short want = POLLIN;
                                if (_viewer->outSent < _viewer->outSize) want = short(want | POLLOUT);
                                fds[n++] = pollfd{_viewer->fd, want, 0};
                        }
                        const int r = poll(fds, n, -1);
                        if (_settings.verbose)
                                fprintf(stderr, "poll %d: gui %x listen %x viewer %x\n", r, fds[0].revents,
                                        fds[1].revents, n == 3 ? fds[2].revents : 0);
                        if (r < 0) {
                                if (errno == EINTR) continue;
                                break;
                        }
                        if (fds[0].revents & (POLLHUP | POLLERR)) break; // the GUI went
                        if (fds[1].revents & POLLIN) accept();
                        if (n == 3 && _viewer != nullptr) {
                                if (fds[2].revents & (POLLIN | POLLHUP | POLLERR)) readViewer();
                                if (_viewer != nullptr && (fds[2].revents & POLLOUT)) (void)writeViewer();
                        }
                }
                return 0;
        }

        void Server::guiEvents() {
                bmds_gui_event_t e;
                int              r;
                while ((r = bmds_gui_next_event(_gui, &e, 0)) == 1) {
                        if (_settings.verbose) {
                                const char *name = bmds_gui_atom_name(_gui, e.name);
                                fprintf(stderr, "event %s from %u\n", name != nullptr ? name : "?", unsigned(e.source));
                        }
                        if (e.source == _shell && bmds_gui_event_is(_gui, &e, BMDS_GUI_SHELL_SCREEN_CHANGED)) {
                                if (_viewer != nullptr) _viewer->changed = true;
                        } else if (e.source == _menuDisconnect && _viewer != nullptr) {
                                drop("disconnected at the board");
                        } else if (e.source == _menuStop) {
                                _stopping = true;
                        } else if (e.source == _notification && _notification != 0 &&
                                   bmds_gui_event_is(_gui, &e, BMDS_GUI_NOTIFICATION_ACTIVATED) && e.count == 1 &&
                                   e.values[0].number == 0 && _viewer != nullptr) {
                                drop("disconnected at the board");
                        }
                }
                if (r < 0) _stopping = true; // the GUI service closed the connection
                return;
        }

        // ---- Viewers

        void Server::accept() {
                sockaddr_in from = {};
                socklen_t   len = sizeof(from);
                const int   fd =
                        ::accept4(_listen, reinterpret_cast<sockaddr *>(&from), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (_settings.verbose) fprintf(stderr, "accept: %d (%s)\n", fd, fd < 0 ? strerror(errno) : "ok");
                if (fd < 0) return;
                const uint32_t address = from.sin_addr.s_addr;
                if (_viewer != nullptr || isRefused(address)) {
                        close(fd); // one viewer at a time (devplan/vnc.md A6); a guesser waits
                        return;
                }
                const int on = 1;
                (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
                Session::Config c;
                c.name = "BMDS";
                c.width = _width;
                c.height = _height;
                c.password = _settings.password[0] != '\0' ? _settings.password : nullptr;
                c.random = random;
                c.randomArg = this;
                auto *v = new (std::nothrow) Viewer(c, _events);
                if (v == nullptr) {
                        close(fd);
                        return;
                }
                v->out = static_cast<uint8_t *>(malloc(OutBytes));
                v->pixels = static_cast<uint8_t *>(malloc(CaptureBytes));
                if (v->out == nullptr || v->pixels == nullptr || !v->encoder.init(Encoder::TightMaxPixels)) {
                        free(v->out);
                        free(v->pixels);
                        delete v;
                        close(fd);
                        return;
                }
                v->fd = fd;
                v->address = address;
                v->sinceMs = nowMs();
                inet_ntop(AF_INET, &from.sin_addr, v->name, sizeof(v->name));
                v->encoder.setJpeg(jpeg, this);
                v->encoder.setJpegCeiling(_settings.jpegCeiling);
                _viewer = v;
                Writer w = v->appender();
                v->session.start(w);
                v->outSize += w.size();
                (void)writeViewer();
                return;
        }

        void Server::drop(const char *why) {
                Viewer *v = _viewer;
                if (v == nullptr) return;
                _viewer = nullptr;
                if (v->watching) bmds_shell_watch_screen(_gui, _shell, 0); // the system's icon goes too
                if (v->session.failedAuthentication()) {
                        failedFrom(v->address);
                } else if (v->watching) {
                        char body[96];
                        snprintf(body, sizeof(body), "%s: %s", v->name, why);
                        notify("Viewer left", body, "low", false);
                }
                close(v->fd);
                free(v->out);
                free(v->pixels);
                delete v;
                refreshTray();
                return;
        }

        void Server::readViewer() {
                Viewer       *v = _viewer;
                const ssize_t n = recv(v->fd, v->in + v->inSize, InBytes - v->inSize, 0);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                        drop(n == 0 ? "the viewer closed" : "the connection failed");
                        return;
                }
                if (_settings.verbose) fprintf(stderr, "recv: %d\n", int(n));
                if (n < 0) return;
                v->inSize += size_t(n);
                Writer       w = v->appender();
                const size_t taken = v->session.feed(v->in, v->inSize, w);
                v->outSize += w.size();
                memmove(v->in, v->in + taken, v->inSize - taken);
                v->inSize -= taken;
                if (!writeViewer()) return;
                if (v->session.isClosed()) {
                        drop(v->session.closeReason() != nullptr ? v->session.closeReason() : "closed");
                } else if (v->inSize == InBytes) {
                        drop("a message too long"); // nothing was taken from a full buffer
                }
                return;
        }

        bool Server::writeViewer() {
                Viewer *v = _viewer;
                while (v->outSent < v->outSize) {
                        const ssize_t n = send(v->fd, v->out + v->outSent, v->outSize - v->outSent, MSG_NOSIGNAL);
                        if (_settings.verbose)
                                fprintf(stderr, "send %u: %d %s\n", unsigned(v->outSize - v->outSent), int(n),
                                        n < 0 ? strerror(errno) : "");
                        if (n < 0) {
                                if (errno == EAGAIN || errno == EINTR) return true;
                                drop("the connection failed");
                                return false;
                        }
                        v->outSent += size_t(n);
                }
                v->outSent = v->outSize = 0;
                return true;
        }

        // ---- Updates (devplan/vnc.md §5b)

        int Server::capture(bool all) {
                Viewer               *v = _viewer;
                bmds_screen_capture_t c = {};
                c.size = sizeof(c);
                c.format = BMDS_PIXEL_BGR24;
                c.flags = all ? BMDS_CAPTURE_ALL : 0;
                c.buffer = v->pixels;
                c.capacity = CaptureBytes;
                c.rects = v->rects;
                c.max_rects = MaxRects;
                const int32_t r = bmds_gui_screen_capture(bmds_gui_fd(_gui), &c);
                if (r == -BMDS_EACCES) return 0; // the watch isn't there yet: its screenChanged will come
                if (r < 0) return -1;
                v->rectCount = c.rect_count;
                v->rectAt = 0;
                v->bandAt = 0;
                v->more = c.more != 0;
                v->capturedWidth = uint16_t(c.width);
                v->capturedHeight = uint16_t(c.height);
                size_t at = 0;
                for (uint32_t i = 0; i < c.rect_count; ++i) {
                        v->offsets[i] = at;
                        at += size_t(v->rects[i].width) * v->rects[i].height * 3;
                }
                return 1;
        }

        void Server::update() {
                Viewer *v = _viewer;
                if (v == nullptr || v->session.state() != Session::State::Normal || !v->requested ||
                    v->outSent < v->outSize)
                        return;
                if (v->rectAt == v->rectCount) {
                        // Nothing left of the last capture: a new one if something is due.
                        if (!v->full && !v->changed && !v->more) return;
                        const bool all = v->full;
                        v->changed = false; // a change from here on tells it again
                        const int got = capture(all);
                        if (got < 0) {
                                drop("the screen can't be captured");
                                return;
                        }
                        if (got == 0) return; // not watched yet: the request (and a full one) waits
                        v->full = false;
                }
                Writer out(v->out, OutBytes);
                v->outSize = v->outSent = 0;
                Session::writeUpdateHeader(out, 0);
                uint32_t count = 0;
                // The picture's size changed (a program's full screen): the viewer told, or let go.
                if (v->capturedWidth != _width || v->capturedHeight != _height) {
                        if (!v->encoder.hasDesktopSize()) {
                                drop("the screen changed size and the viewer can't follow");
                                return;
                        }
                        _width = v->capturedWidth;
                        _height = v->capturedHeight;
                        v->session.setSize(_width, _height);
                        Session::writeDesktopSize(out, _width, _height);
                        ++count;
                }
                while (v->rectAt < v->rectCount && count < MaxRectsAnUpdate) {
                        const bmds_rect_t &r = v->rects[v->rectAt];
                        const uint16_t     w = uint16_t(r.width), left = uint16_t(r.height - v->bandAt);
                        // The most rows (in bands) the room left can take whatever they hold.
                        uint16_t rows = left;
                        while (rows > BandRows && v->encoder.worstCase(w, rows) > out.room())
                                rows = uint16_t((rows - 1) / BandRows * BandRows);
                        if (v->encoder.worstCase(w, rows) > out.room()) {
                                if (count > 0) break;                     // the rest in the next update
                                rows = rows > BandRows ? BandRows : rows; // an empty update: a band anyway
                        }
                        const Picture picture{v->pixels + v->offsets[v->rectAt] + size_t(v->bandAt) * w * 3,
                                              size_t(w) * 3, w, rows};
                        const size_t  n = v->encoder.encode(picture, uint16_t(r.x), uint16_t(r.y + v->bandAt), out);
                        if (n == 0) {
                                if (v->encoder.isBroken()) {
                                        drop("an update didn't fit (its stream can't be mended)");
                                        return;
                                }
                                if (count > 0) break;
                                drop("an update didn't fit");
                                return;
                        }
                        count += uint32_t(n);
                        v->bandAt = uint16_t(v->bandAt + rows);
                        if (v->bandAt == r.height) {
                                ++v->rectAt;
                                v->bandAt = 0;
                        }
                }
                if (count == 0) return; // nothing changed after all: the request waits
                out.patchU16(2, uint16_t(count));
                v->outSize = out.size();
                v->requested = false;
                (void)writeViewer();
                return;
        }

        // ---- The session's events

        void Server::Events::ready() {
                Viewer *v = _server._viewer;
                if (v == nullptr) return;
                // Watched only while a viewer is in: the system's "screen shared" icon says so.
                // Its first screenChanged (sent as the watch begins) lets updates start: a capture
                // before the service took the request would be refused.
                bmds_shell_watch_screen(_server._gui, _server._shell, 1);
                v->watching = true;
                char body[96];
                snprintf(body, sizeof(body), "%s is viewing the screen", v->name);
                _server.notify("Viewer connected", body, "normal", true);
                _server.refreshTray();
                return;
        }

        void Server::Events::setPixelFormat(const PixelFormat &format) {
                if (_server._viewer != nullptr) _server._viewer->encoder.setPixelFormat(format);
                return;
        }

        void Server::Events::setEncodings(const int32_t *encodings, size_t count) {
                if (_server._viewer != nullptr) _server._viewer->encoder.setEncodings(encodings, count);
                return;
        }

        void Server::Events::updateRequest(bool incremental, uint16_t, uint16_t, uint16_t, uint16_t) {
                Viewer *v = _server._viewer;
                if (v == nullptr) return;
                v->requested = true;
                if (!incremental) v->full = true;
                return;
        }

        void Server::Events::key(bool down, uint32_t keysym) {
                Viewer *v = _server._viewer;
                if (v == nullptr) return;
                const KeyState::Key k = v->keys.event(down, keysym);
                if (k.usage != 0)
                        bmds_shell_inject_key(_server._gui, _server._shell, k.usage, k.modifiers, down ? 1 : 0);
                return;
        }

        void Server::Events::pointer(uint8_t buttons, uint16_t x, uint16_t y) {
                Viewer *v = _server._viewer;
                if (v == nullptr) return;
                // The wheel: buttons 4 and 5 (away, towards), a notch at each press.
                const uint8_t pressed = uint8_t(buttons & ~v->buttons);
                const int     wheel = ((pressed & 0x08) ? 1 : 0) - ((pressed & 0x10) ? 1 : 0);
                v->buttons = buttons;
                const int32_t px = x < _server._width ? x : int32_t(_server._width) - 1;
                const int32_t py = y < _server._height ? y : int32_t(_server._height) - 1;
                bmds_shell_inject_pointer(_server._gui, _server._shell, px, py, bmdsButtons(buttons), wheel);
                return;
        }

        // ---- Telling the user (devplan/vnc.md §5d)

        void Server::notify(const char *title, const char *body, const char *urgency, bool withDisconnect) {
                const uint32_t n = bmds_notification_new(_gui, 0);
                bmds_notification_set_app(_gui, n, "VNC Server");
                bmds_notification_set_icon(_gui, n, TrayIconName);
                bmds_notification_set_title(_gui, n, title);
                bmds_notification_set_body(_gui, n, body);
                bmds_notification_set_urgency(_gui, n, urgency);
                if (withDisconnect) {
                        const char *actions[] = {"Disconnect"};
                        bmds_notification_set_actions(_gui, n, actions, 1);
                        _notification = n;
                } else {
                        bmds_notification_set_transient(_gui, n, 1);
                }
                bmds_notification_show(_gui, n);
                return;
        }

        void Server::refreshTray() {
                char tip[128], line[64];
                if (_viewer != nullptr && _viewer->watching) {
                        const unsigned minutes = unsigned((nowMs() - _viewer->sinceMs) / 60000u);
                        snprintf(tip, sizeof(tip), "VNC server on %s: %s watching", _address, _viewer->name);
                        snprintf(line, sizeof(line), "%s, %u min", _viewer->name, minutes);
                } else {
                        snprintf(tip, sizeof(tip), "VNC server on %s: no viewer", _address);
                        snprintf(line, sizeof(line), "No viewer");
                }
                bmds_tray_icon_set_tool_tip(_gui, _tray, tip);
                bmds_menu_item_set_text(_gui, _menuViewer, line);
                bmds_menu_item_set_enabled(_gui, _menuDisconnect, _viewer != nullptr ? 1 : 0);
                return;
        }

        bool Server::isRefused(uint32_t address) {
                const uint64_t now = nowMs();
                for (const Failure &f : _failures)
                        if (f.address == address && f.refusedUntilMs > now) return true;
                return false;
        }

        // A wrong password from @p address: told; the third within a minute refuses it for a minute.
        void Server::failedFrom(uint32_t address) {
                const uint64_t now = nowMs();
                Failure       *slot = &_failures[0];
                for (Failure &f : _failures) {
                        if (f.address == address) {
                                slot = &f;
                                break;
                        }
                        if (f.firstMs < slot->firstMs) slot = &f; // the oldest goes
                }
                if (slot->address != address || now - slot->firstMs > FailureWindowMs)
                        *slot = Failure{address, 0, now, 0};
                ++slot->count;
                char    name[32];
                in_addr a;
                a.s_addr = address;
                inet_ntop(AF_INET, &a, name, sizeof(name));
                char body[96];
                if (slot->count >= FailuresAllowed) {
                        slot->refusedUntilMs = now + RefusedMs;
                        snprintf(body, sizeof(body), "%s: %u wrong passwords; refused for a minute", name,
                                 unsigned(slot->count));
                        notify("VNC: someone is guessing", body, "critical", false);
                } else {
                        snprintf(body, sizeof(body), "%s gave a wrong password", name);
                        notify("VNC: wrong password", body, "normal", false);
                }
                return;
        }

        // ---- For the core

        long Server::jpeg(void *, const Picture &picture, int quality, uint8_t *out, size_t capacity) {
                bmds_image_encode_t e = {};
                e.size = sizeof(e);
                e.codec = BMDS_IMAGE_JPEG;
                e.format = BMDS_PIXEL_BGR24;
                e.width = picture.width;
                e.height = picture.height;
                e.stride = uint32_t(picture.stride);
                e.pixels = picture.pixels;
                e.quality = uint32_t(quality);
                e.sampling = quality >= 90 ? BMDS_JPEG_YUV444 : BMDS_JPEG_YUV420;
                e.out = out;
                e.out_capacity = uint32_t(capacity);
                const int32_t n = bmds_image_encode(-1, &e);
                return n > 0 ? long(n) : -1; // a failure: the core sends the pixels instead
        }

        void Server::random(void *, uint8_t *out, size_t n) {
                size_t got = 0;
                while (got < n) {
                        const ssize_t r = getrandom(out + got, n - got, 0);
                        if (r <= 0) break;
                        got += size_t(r);
                }
                return;
        }

} // namespace vnc
