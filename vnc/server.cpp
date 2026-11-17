/**
 * @file      server.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The VNC server's BMDS side (devplan/vnc.md §5b-§5e): the socket, the GUI connection's
 * ScreenShare (the screen watched and captured, input injected), the hardware JPEG through image_encode, the
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
#include <pthread.h>

namespace vnc {

        namespace {

                constexpr size_t InBytes = 16 * 1024;
                // A viewer's memory (devplan/vnc.md §18): about 1 MB in all with the encoder's, so a game
                // and a viewer fit together. Bigger changes go in more updates.
                constexpr size_t      OutBytes = 384 * 1024;     // an update at most (a 2048-wide band's worst case)
                constexpr size_t      CaptureBytes = 768 * 1024; // a capture's pixels (a 2048-wide tile row at least)
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

                uint64_t nowUs() {
                        struct timespec t;
                        clock_gettime(CLOCK_MONOTONIC, &t);
                        return uint64_t(t.tv_sec) * 1000000u + uint64_t(t.tv_nsec) / 1000u;
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

                        int      fd = -1;
                        uint32_t address = 0; // IPv4, network order
                        char     name[32] = {};
                        uint64_t sinceMs = 0;
                        Session  session;
                        Encoder  encoder;
                        KeyState keys;
                        uint8_t  buttons = 0; // RFB's, as last sent
                        bool     watching = false;
                        bool     requested = false; // an update asked for and not sent
                        bool     full = false;      // the whole screen asked for
                        bool     changed = false;   // the screen changed since the last capture
                        bool     more = false;      // the last capture left changes behind
                        // The viewer's SetPixelFormat and SetEncodings while the worker encodes: kept,
                        // and given to the encoder before the next job.
                        bool        pendingFormat = false;
                        PixelFormat format;
                        bool        pendingEncodings = false;
                        int32_t     encodings[64];
                        size_t      encodingCount = 0;
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
                stopWorker();
                const int pipes[] = {_jobPipe[0], _jobPipe[1], _donePipe[0], _donePipe[1]};
                for (int fd : pipes)
                        if (fd >= 0) close(fd);
                if (_listen >= 0) close(_listen);
                if (_outputWatch >= 0) close(_outputWatch);
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
                if (!bmds_sys_has(BMDS_SYS_GUI_SCREEN_CAPTURE)) {
                        say(false, "this BMDS can't share its screen (it has no screen capture: update it)");
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
                _screen = bmds_screen_share_new(_gui, 0);
                // The cores' load counters (place()), and the worker on the less busy core now.
                bmds_counter_t info;
                for (uint32_t i = 0;; ++i) {
                        memset(&info, 0, sizeof(info));
                        info.size = sizeof(info);
                        if (bmds_stats_list(i, &info) < 0) break;
                        if (strcmp(info.name, "cpu.core0.busy_us") == 0) _loadIds[0] = info.id;
                        if (strcmp(info.name, "cpu.core1.busy_us") == 0) _loadIds[1] = info.id;
                }
                int     core = 0;
                int64_t a[2], b[2], ns0 = 0, ns1 = 0;
                _loadOk = _loadIds[0] != 0 && _loadIds[1] != 0 && bmds_sys_has(BMDS_SYS_STATS_READ) &&
                          bmds_stats_read(_loadIds, 2, a, nullptr, &ns0) == 2 && a[0] >= 0 && a[1] >= 0;
                if (_loadOk) {
                        usleep(200 * 1000);
                        if (bmds_stats_read(_loadIds, 2, b, nullptr, &ns1) == 2 && b[1] - a[1] < b[0] - a[0]) core = 1;
                }
                if (!startWorker(core)) {
                        say(false, "can't start its encoding thread");
                        return 1;
                }
                _tray = bmds_tray_icon_new(_gui, 0);
                bmds_tray_icon_set_icon(_gui, _tray, TrayIconName);
                bmds_tray_icon_set_category(_gui, _tray, "application");
                _trayMenu = bmds_menu_new(_gui, _tray); // a tray icon's menu: its context menu
                _menuViewer = bmds_menu_item_new(_gui, _trayMenu);
                bmds_menu_item_set_enabled(_gui, _menuViewer, 0);
                _menuDisconnect = bmds_menu_item_new(_gui, _trayMenu);
                bmds_menu_item_set_text(_gui, _menuDisconnect, "&Disconnect");
                // The display blanked while a viewer is in: display/output = remote, the system's own
                // (devplan/vnc.md §14), so one setting says it however it is changed.
                _outputId = bmds_sysconf_find("display/output");
                if (_outputId >= 0) {
                        bmds_menu_add_separator(_gui, _trayMenu);
                        _menuBlank = bmds_menu_item_new(_gui, _trayMenu);
                        bmds_menu_item_set_text(_gui, _menuBlank, "&Blank Display While Connected");
                        _outputWatch = int(bmds_sysconf_watch("display/output", BMDS_O_CLOEXEC));
                        readBlank();
                }
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
                        if (_viewer != nullptr) place();
                        startUpdate();
                        (void)bmds_gui_flush(_gui);
                        // The GUI, the listener, display/output's watch (if any), the viewer (if any).
                        pollfd fds[5];
                        nfds_t n = 0;
                        fds[n++] = pollfd{bmds_gui_fd(_gui), POLLIN, 0};
                        fds[n++] = pollfd{_listen, POLLIN, 0};
                        const nfds_t doneAt = _donePipe[0] >= 0 ? n : nfds_t(-1);
                        if (_donePipe[0] >= 0) fds[n++] = pollfd{_donePipe[0], POLLIN, 0};
                        const nfds_t watchAt = _outputWatch >= 0 ? n : nfds_t(-1);
                        if (_outputWatch >= 0) fds[n++] = pollfd{_outputWatch, POLLIN, 0};
                        const nfds_t viewerAt = _viewer != nullptr ? n : nfds_t(-1);
                        if (_viewer != nullptr) {
                                short want = POLLIN;
                                if (_viewer->outSent < _viewer->outSize) want = short(want | POLLOUT);
                                fds[n++] = pollfd{_viewer->fd, want, 0};
                        }
                        // With a viewer in, a look at the cores now and then (place()).
                        const int r = poll(fds, n, _viewer != nullptr ? 1000 : -1);
                        if (_settings.verbose)
                                fprintf(stderr, "poll %d: gui %x listen %x\n", r, fds[0].revents, fds[1].revents);
                        if (r < 0) {
                                if (errno == EINTR) continue;
                                break;
                        }
                        if (fds[0].revents & (POLLHUP | POLLERR)) break; // the GUI went
                        if (fds[1].revents & POLLIN) accept();
                        if (doneAt != nfds_t(-1) && (fds[doneAt].revents & POLLIN)) {
                                char result = 0;
                                if (read(_donePipe[0], &result, 1) == 1) updateDone(int(result) - 8);
                        }
                        if (watchAt != nfds_t(-1) && (fds[watchAt].revents & POLLIN)) {
                                uint32_t records[16];
                                (void)read(_outputWatch, records, sizeof(records)); // only one is watched
                                readBlank();
                        }
                        if (viewerAt != nfds_t(-1) && _viewer != nullptr) {
                                const short got = fds[viewerAt].revents;
                                if (got & (POLLIN | POLLHUP | POLLERR)) readViewer();
                                if (_viewer != nullptr && (got & POLLOUT)) (void)writeViewer();
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
                        if (e.source == _screen && bmds_gui_event_is(_gui, &e, BMDS_GUI_SCREEN_SHARE_CHANGED)) {
                                if (_viewer != nullptr) _viewer->changed = true;
                        } else if (e.source == _menuDisconnect && _viewer != nullptr) {
                                drop("disconnected at the board");
                        } else if (e.source == _menuStop) {
                                _stopping = true;
                        } else if (e.source == _menuBlank && _menuBlank != 0) {
                                toggleBlank();
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
                // On BMDS_CAPTURE_ALIGN: the kernel copies a capture by the 2D-DMA into it.
                void *pixels = nullptr;
                if (posix_memalign(&pixels, BMDS_CAPTURE_ALIGN, CaptureBytes) == 0)
                        v->pixels = static_cast<uint8_t *>(pixels);
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
                _timing = Timing(); // its encoder's totals start at 0
                Writer w = v->appender();
                v->session.start(w);
                v->outSize += w.size();
                (void)writeViewer();
                return;
        }

        void Server::drop(const char *why) {
                Viewer *v = _viewer;
                if (v == nullptr) return;
                if (_busy) {
                        // The worker is using it: its answer waited for first.
                        char result = 0;
                        while (read(_donePipe[0], &result, 1) < 0 && errno == EINTR) continue;
                        _busy = false;
                }
                _viewer = nullptr;
                if (v->watching) bmds_screen_share_watch(_gui, _screen, 0); // the tray's mark goes too
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
                if (r == -BMDS_EACCES) return 0; // the watch isn't there yet: its changed will come
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

        // Results of buildUpdate().
        enum : int {
                Built = 1,      // an update in the out buffer
                Nothing = 0,    // no change after all: the request waits
                NotWatched = 2, // the watch isn't there yet: the request (a full one too) waits
                CaptureFailed = -1,
                SizeUnfollowable = -2,
                Broken = -3, // ZRLE's stream lost bytes
                NoFit = -4,
        };

        // On the loop: a job for the worker if an update is asked for, the last one sent and
        // something is due; the viewer's encoder settings given to the encoder first (it is idle).
        void Server::startUpdate() {
                Viewer *v = _viewer;
                if (v == nullptr || _busy || !_workerRunning || v->session.state() != Session::State::Normal ||
                    !v->requested || v->outSent < v->outSize)
                        return;
                if (v->pendingFormat) v->encoder.setPixelFormat(v->format);
                if (v->pendingEncodings) v->encoder.setEncodings(v->encodings, v->encodingCount);
                v->pendingFormat = v->pendingEncodings = false;
                const bool leftover = v->rectAt < v->rectCount;
                if (!leftover && !v->full && !v->changed && !v->more) return;
                _jobFresh = !leftover;
                _jobAll = !leftover && v->full;
                if (_jobFresh) v->changed = false; // a change from here on tells it again
                _busy = true;
                const char job = 'u';
                if (write(_jobPipe[1], &job, 1) != 1) _busy = false;
                return;
        }

        // On the loop: the worker's answer.
        void Server::updateDone(int result) {
                _busy = false;
                Viewer *v = _viewer;
                if (v == nullptr) return;
                switch (result) {
                        case Built:
                                if (_jobAll) v->full = false;
                                v->requested = false;
                                (void)writeViewer();
                                return;
                        case Nothing:
                                if (_jobAll) v->full = false;
                                return;
                        case NotWatched: return; // the request (and a full one) waits for the watch's first change
                        case CaptureFailed: drop("the screen can't be captured"); return;
                        case SizeUnfollowable: drop("the screen changed size and the viewer can't follow"); return;
                        case Broken: drop("an update didn't fit (its stream can't be mended)"); return;
                        default: drop("an update didn't fit"); return;
                }
        }

        // On the worker: the capture (or the last one's rest) encoded into the out buffer.
        int Server::buildUpdate(bool all, bool fresh) {
                const uint64_t t0 = nowUs();
                Viewer        *v = _viewer;
                if (fresh) {
                        const uint64_t c0 = nowUs();
                        const int      got = capture(all);
                        _timing.captureUs += nowUs() - c0;
                        if (got < 0) return CaptureFailed;
                        if (got == 0) return NotWatched;
                }
                Writer out(v->out, OutBytes);
                v->outSize = v->outSent = 0;
                Session::writeUpdateHeader(out, 0);
                uint32_t count = 0;
                // The picture's size changed (a program's full screen): the viewer told, or let go.
                if (v->capturedWidth != _width || v->capturedHeight != _height) {
                        if (!v->encoder.hasDesktopSize()) return SizeUnfollowable;
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
                                if (v->encoder.isBroken()) return Broken;
                                if (count > 0) break;
                                return NoFit;
                        }
                        count += uint32_t(n);
                        v->bandAt = uint16_t(v->bandAt + rows);
                        if (v->bandAt == r.height) {
                                ++v->rectAt;
                                v->bandAt = 0;
                        }
                }
                if (count == 0) return Nothing;
                out.patchU16(2, uint16_t(count));
                if (_settings.verbose) {
                        const Encoder::Stats &st = v->encoder.stats();
                        fprintf(stderr,
                                "update: %u rects %llu pixels %u bytes in %llu us on core %d (capture %llu, jpeg %llu, "
                                "zlib %llu, "
                                "encode %llu); so far fill %u palette %u jpeg %u basic %u zrle %u raw %u\n",
                                unsigned(count), (unsigned long long)(st.pixels - _timing.pixelsSeen),
                                unsigned(out.size()), (unsigned long long)(nowUs() - t0), _workerCore,
                                (unsigned long long)_timing.captureUs, (unsigned long long)_timing.jpegUs,
                                (unsigned long long)(st.zlibUs - _timing.zlibSeen),
                                (unsigned long long)(st.encodeUs - _timing.encodeSeen), unsigned(st.fills),
                                unsigned(st.palettes), unsigned(st.jpegs), unsigned(st.basics), unsigned(st.zrle),
                                unsigned(st.raw));
                }
                _timing = Timing();
                _timing.zlibSeen = v->encoder.stats().zlibUs;
                _timing.encodeSeen = v->encoder.stats().encodeUs;
                _timing.pixelsSeen = v->encoder.stats().pixels;
                v->outSize = out.size();
                return Built;
        }

        // ---- The worker and its core (devplan/vnc.md §17)

        namespace {
                struct WorkerStart {
                                Server *server;
                                int     core;
                };
        } // namespace

        // First thing, in a function that uses no floating point or PIE (either fixes a thread's core
        // for good): kept to its core; then the loop.
        void *Server::workerEntry(void *arg) {
                const WorkerStart start = *static_cast<WorkerStart *>(arg);
                free(arg);
                bmds_sched_t s;
                memset(&s, 0, sizeof(s));
                s.size = sizeof(s);
                s.cores = 1u << start.core;
                (void)bmds_sched(BMDS_SCHED_PIN, 0, &s);
                start.server->workerLoop();
                return nullptr;
        }

        void Server::workerLoop() {
                for (;;) {
                        char          job = 0;
                        const ssize_t n = read(_jobPipe[0], &job, 1);
                        if (n < 0 && errno == EINTR) continue;
                        if (n <= 0 || job == 'q') return;
                        const uint64_t t0 = nowUs();
                        const char     result = char(buildUpdate(_jobAll, _jobFresh) + 8); // 0..15 on the pipe
                        _workUs += nowUs() - t0;
                        (void)write(_donePipe[1], &result, 1);
                }
        }

        bool Server::startWorker(int core) {
                if (_jobPipe[0] < 0 && (pipe(_jobPipe) != 0 || pipe(_donePipe) != 0)) return false;
                auto *start = static_cast<WorkerStart *>(malloc(sizeof(WorkerStart)));
                if (start == nullptr) return false;
                *start = WorkerStart{this, core};
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setstacksize(&attr, 32 * 1024); // zlib's deflate and the encoder
                auto      *thread = static_cast<pthread_t *>(malloc(sizeof(pthread_t)));
                const bool ok = thread != nullptr && pthread_create(thread, &attr, workerEntry, start) == 0;
                pthread_attr_destroy(&attr);
                if (!ok) {
                        free(thread);
                        free(start);
                        return false;
                }
                _workerThread = thread;
                _workerCore = core;
                _workerRunning = true;
                _workUs = 0;
                _placedMs = nowMs();
                if (_settings.verbose) fprintf(stderr, "worker: on core %d\n", core);
                return true;
        }

        void Server::stopWorker() {
                if (!_workerRunning) return;
                if (_busy) { // its answer first
                        char result = 0;
                        while (read(_donePipe[0], &result, 1) < 0 && errno == EINTR) continue;
                        updateDone(int(result) - 8);
                }
                const char quit = 'q';
                (void)write(_jobPipe[1], &quit, 1);
                auto *thread = static_cast<pthread_t *>(_workerThread);
                pthread_join(*thread, nullptr);
                free(thread);
                _workerThread = nullptr;
                _workerRunning = false;
                return;
        }

        // Each core's load over the last few seconds, from the kernel's counters; the worker moved to
        // the other core when that one, the worker's own work aside, is clearly less busy.
        void Server::place() {
                if (!_loadOk || _busy) return;
                const uint64_t now = nowMs();
                if (now - _placedMs < 3000) return; // a few seconds between looks (and after a move)
                int64_t values[2], ns = 0;
                if (bmds_stats_read(_loadIds, 2, values, nullptr, &ns) != 2 || values[0] < 0 || values[1] < 0) return;
                if (_loadNs != 0 && ns > _loadNs) {
                        const int64_t span = (ns - _loadNs) / 1000;
                        int           percent[2];
                        for (int c = 0; c < 2; ++c) percent[c] = int((values[c] - _loadBusy[c]) * 100 / span);
                        const int mine = int(int64_t(_workUs) * 100 / span); // the worker's share of its core
                        const int other = 1 - _workerCore;
                        if (_settings.verbose)
                                fprintf(stderr, "load: core0 %d%% core1 %d%%, the worker %d%% of core %d\n", percent[0],
                                        percent[1], mine, _workerCore);
                        // Moved if the other core is busier with everything else by 20 points less.
                        if (_workerCore >= 0 && percent[other] + 20 < percent[_workerCore] - mine) {
                                stopWorker();
                                if (!startWorker(other)) (void)startWorker(1 - other);
                        }
                }
                _loadBusy[0] = values[0];
                _loadBusy[1] = values[1];
                _loadNs = ns;
                _placedMs = now;
                _workUs = 0;
                return;
        }

        // ---- The session's events

        void Server::Events::ready() {
                Viewer *v = _server._viewer;
                if (v == nullptr) return;
                // Watched only while a viewer is in: the tray's mark (a red dot on our icon) says so.
                // Its first changed (sent as the watch begins) lets updates start: a capture before
                // the service took the request would be refused.
                bmds_screen_share_watch(_server._gui, _server._screen, 1);
                v->watching = true;
                char body[96];
                snprintf(body, sizeof(body), "%s is viewing the screen", v->name);
                _server.notify("Viewer connected", body, "normal", true);
                _server.refreshTray();
                return;
        }

        // The encoder is the worker's while a job runs: these wait for startUpdate().
        void Server::Events::setPixelFormat(const PixelFormat &format) {
                Viewer *v = _server._viewer;
                if (v == nullptr) return;
                v->format = format;
                v->pendingFormat = true;
                return;
        }

        void Server::Events::setEncodings(const int32_t *encodings, size_t count) {
                Viewer *v = _server._viewer;
                if (v == nullptr) return;
                v->encodingCount = count < 64 ? count : 64;
                for (size_t i = 0; i < v->encodingCount; ++i) v->encodings[i] = encodings[i];
                v->pendingEncodings = true;
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
                        bmds_screen_share_inject_key(_server._gui, _server._screen, k.usage, k.modifiers, down ? 1 : 0);
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
                bmds_screen_share_inject_pointer(_server._gui, _server._screen, px, py, bmdsButtons(buttons), wheel);
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

        void Server::readBlank() {
                char value[16] = {};
                if (_outputId < 0 || bmds_sysconf_get(_outputId, BMDS_SYSCONF_FIELD_VALUE, value, sizeof(value)) < 0)
                        return;
                _blank = strcmp(value, "remote") == 0;
                bmds_menu_item_set_checked(_gui, _menuBlank, _blank ? 1 : 0);
                return;
        }

        // Checked: the display off while a viewer is in (and on again as it leaves); unchecked: on.
        // Saved, as Settings saves it: the user's choice outlives the server.
        void Server::toggleBlank() {
                const char *want = _blank ? "on" : "remote";
                if (bmds_sysconf_set(_outputId, BMDS_SYSCONF_OP_SET, want) < 0) {
                        notify("VNC server", "The display setting can't be changed", "normal", false);
                        readBlank();
                        return;
                }
                (void)bmds_sysconf_set(_outputId, BMDS_SYSCONF_OP_SAVE, nullptr);
                readBlank(); // the watch tells it too
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

        long Server::jpeg(void *arg, const Picture &picture, int quality, uint8_t *out, size_t capacity) {
                auto               *self = static_cast<Server *>(arg);
                const uint64_t      j0 = nowUs();
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
                self->_timing.jpegUs += nowUs() - j0;
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
