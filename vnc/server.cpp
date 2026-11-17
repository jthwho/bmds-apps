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

#include "core/clipboard.h"
#include "core/text.h"

#include <bmds/guiclasses.h>
#include <bmds/sys.h>

#include <arpa/inet.h>
#include <stdarg.h>
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
                // Memory (devplan/vnc.md §18, §21): a viewer's update buffer and encoder, about 650 KB;
                // one capture buffer for all, as much as one update always carries (no leftovers).
                constexpr size_t      OutBytes = 384 * 1024;     // an update at most (a 2048-wide band's worst case)
                constexpr size_t      CaptureBytes = 256 * 1024; // a capture's pixels (a 2048-wide tile row at least)
                constexpr uint32_t    MaxRects = 64;
                constexpr uint64_t    PointerStillMs = 500;     // a viewer's mouse still this long: PointerPos again
                constexpr uint16_t    BandRows = 16;            // a rectangle is encoded in bands of these
                constexpr uint32_t    MaxRectsAnUpdate = 60000; // under the header's 16 bits
                constexpr uint32_t    FailuresAllowed = 3;      // wrong passwords from one address
                constexpr uint64_t    FailureWindowMs = 60'000; // within this long
                constexpr uint64_t    RefusedMs = 60'000;       // then refused for this long
                constexpr const char *TrayIconName = "preferences-system-network";
                constexpr const char *LogPath = "/tmp/vncserver.log"; // beside the pid file; RAM: kept small
                constexpr long        LogBytes = 32 * 1024;           // started afresh past this

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
                        Viewer(Server &server, const Session::Config &config)
                            : events(server, this), session(config, events) {}

                        Events   events; // before the session: it calls them
                        int      fd = -1;
                        uint32_t address = 0; // IPv4, network order
                        char     name[40] = {}; // "Viewer N (ADDRESS)", as the user sees it
                        uint64_t sinceMs = 0;
                        Session  session;
                        Encoder  encoder;
                        KeyState keys;
                        uint8_t  buttons = 0;       // RFB's, as last sent
                        uint32_t screen = 0;        // its ScreenShare: its own changes (its watch)
                        uint32_t notification = 0;  // "Viewer connected", with Disconnect
                        bool     watching = false;  // authenticated and watching
                        bool     requested = false; // an update asked for and not sent
                        bool     full = false;      // the whole screen asked for
                        bool     changed = false;   // the screen changed since its last capture
                        bool     more = false;      // its last capture left changes behind
                        bool     cutPending = false; // the board's copy to send it (or say there is one)
                        // The Extended Clipboard (devplan/vnc.md §23): asked for in its SetEncodings,
                        // its caps (the spec's defaults until it sends its own), messages to send it.
                        bool     extended = false;
                        uint32_t clipFlags = ExtendedClipboard::DefaultFlags;
                        uint32_t clipTextMax = ExtendedClipboard::DefaultTextMax;
                        bool     capsPending = false;    // our caps (each SetEncodings asking for it)
                        bool     requestPending = false; // ask for its copy (it said it has one)
                        bool     providePending = false; // it asked for the board's copy
                        bool     notifyPending = false;  // it peeked: say what the board has
                        // The Cursor pseudo-encoding (devplan/vnc.md §24): asked for, in use (its
                        // captures without the pointer), and the picture it has.
                        bool     wantsCursor = false;
                        bool     wantsAlpha = false; // Cursor With Alpha (TigerVNC): sent instead
                        bool     cursor = false;
                        uint32_t cursorSent = 0;
                        // PointerPos (devplan/vnc.md §25): where the board's pointer is, for a viewer
                        // drawing it (Cursor), so it follows the board's own mouse.
                        int32_t  wantsPointerPos = 0; // the encoding it listed for it (0: none)
                        bool     pointerPos = false;
                        int32_t  boardX = -1; // in its last capture (-1: none drawn)
                        int32_t  boardY = -1;
                        int32_t  posSentX = -1;
                        int32_t  posSentY = -1;
                        uint64_t movedMs = 0;        // its own last pointer event
                        unsigned refused = 0;        // injections the system refused (the first few logged)
                        // The viewer's SetPixelFormat and SetEncodings while the worker encodes: kept,
                        // and given to the encoder before the next job.
                        bool        pendingFormat = false;
                        PixelFormat format;
                        size_t      bytesPerPixel = 4; // its format's, as the encoder has it
                        bool        pendingEncodings = false;
                        int32_t     encodings[64];
                        size_t      encodingCount = 0;
                        uint8_t     in[InBytes];
                        size_t      inSize = 0;
                        uint8_t    *out = nullptr;
                        size_t      outSize = 0;
                        size_t      outSent = 0;
                        // Its last capture's rectangles (their pixels in the shared buffer while it owns it).
                        bmds_rect_t rects[MaxRects];
                        size_t      offsets[MaxRects];
                        uint32_t    rectCount = 0;
                        uint32_t    rectAt = 0; // the next rectangle to send
                        uint16_t    bandAt = 0; // and its next row
                        uint16_t    capturedWidth = 0;
                        uint16_t    capturedHeight = 0;
                        uint16_t    width = 0; // the picture it has (DesktopSize when it changes)
                        uint16_t    height = 0;
                        // -v: its encoder's totals at its last update.
                        uint64_t zlibSeen = 0;
                        uint64_t encodeSeen = 0;
                        uint64_t pixelsSeen = 0;

                        // A writer appending to what is still to be sent.
                        Writer appender() { return Writer(out + outSize, OutBytes - outSize); }
                        bool   isFlushed() const { return outSent >= outSize; }
                        bool   hasLeftover() const { return rectAt < rectCount; }
        };

        Server::Server(const Settings &settings) : _settings(settings) {}

        Server::~Server() {
                while (_viewerCount > 0) drop(_viewers[_viewerCount - 1], "the server stopped");
                stopWorker();
                free(_cut);
                free(_cursor);
                free(_pixels);
                const int pipes[] = {_jobPipe[0], _jobPipe[1], _donePipe[0], _donePipe[1]};
                for (int fd : pipes)
                        if (fd >= 0) close(fd);
                if (_listen >= 0) close(_listen);
                if (_outputWatch >= 0) close(_outputWatch);
                if (_gui != nullptr) bmds_gui_close(_gui);
                if (_log != nullptr) fclose(_log);
        }

        void Server::event(const char *format, ...) {
                char    text[200];
                va_list ap;
                va_start(ap, format);
                vsnprintf(text, sizeof(text), format, ap);
                va_end(ap);
                if (_settings.verbose) fprintf(stderr, "%s\n", text);
                if (_log == nullptr) return;
                if (ftell(_log) > LogBytes) {
                        fclose(_log);
                        _log = fopen(LogPath, "w");
                        if (_log == nullptr) return;
                }
                time_t    now = time(nullptr);
                struct tm t;
                localtime_r(&now, &t);
                fprintf(_log, "%04d-%02d-%02d %02d:%02d:%02d %s\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                        t.tm_hour, t.tm_min, t.tm_sec, text);
                fflush(_log);
                return;
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
                _log = fopen(LogPath, "a");
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
                _inputShare = bmds_screen_share_new(_gui, 0); // the viewers' input goes through it
                {
                        // A BMDS before the pointer's picture (devplan/vnc.md §24): no viewer gets Cursor.
                        bmds_gui_value_t shape = {};
                        _cursorAble =
                                _inputShare != 0 && bmds_screen_share_pointer_shape(_gui, _inputShare, &shape) == 0;
                }
                // One capture buffer for all viewers, on BMDS_CAPTURE_ALIGN: the kernel's 2D-DMA fills it.
                void *pixels = nullptr;
                if (posix_memalign(&pixels, BMDS_CAPTURE_ALIGN, CaptureBytes) != 0) {
                        say(false, "out of memory");
                        return 1;
                }
                _pixels = static_cast<uint8_t *>(pixels);
                _clipboard = bmds_clipboard_new(_gui, 0); // 0: no clipboard sharing, the rest works
                // Every name an event is compared with, known now: a name looked up later is a call on
                // the connection, and an event's strings last only until the next one (gui.h): the
                // first board copy came through with bytes overwritten, or not at all.
                const char *names[] = {BMDS_GUI_CLIPBOARD_CHANGED,
                                       BMDS_GUI_CLIPBOARD_PASTED,
                                       BMDS_GUI_SCREEN_SHARE_CHANGED,
                                       BMDS_GUI_NOTIFICATION_ACTIVATED,
                                       BMDS_GUI_SCREEN_SHARE_POINTER_CHANGED,
                                       "clipboard",
                                       "primary"};
                uint32_t    atoms[sizeof(names) / sizeof(names[0])] = {};
                (void)bmds_gui_atoms(_gui, names, sizeof(names) / sizeof(names[0]), atoms);
                _cut = static_cast<char *>(malloc(Session::MaxCutText));
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
                // The display blanked while a viewer is in: display/output = remote, the system's own
                // (devplan/vnc.md §14), so one setting says it however it is changed.
                _outputId = bmds_sysconf_find("display/output");
                if (_outputId >= 0) _outputWatch = int(bmds_sysconf_watch("display/output", BMDS_O_CLOEXEC));
                refreshTray();
                bmds_tray_icon_set_visible(_gui, _tray, 1);
                char body[128];
                if (_settings.tls) {
                        snprintf(body, sizeof(body),
                                 "Listening on %s with TLS (VeNCrypt): 'tls cert' shows the certificate viewers see",
                                 _address);
                        notify("VNC server started", body, "low", nullptr);
                } else if (_settings.lan) {
                        // On the network without TLS: said where it can't be missed.
                        snprintf(body, sizeof(body),
                                 "Listening on %s, not encrypted: anyone on the network can watch the session",
                                 _address);
                        notify("VNC server started", body, "normal", nullptr);
                } else {
                        snprintf(body, sizeof(body), "Listening on %s%s", _address,
                                 _settings.password[0] != '\0' ? "" : " (no password)");
                        notify("VNC server started", body, "low", nullptr);
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
                        if (_viewerCount > 0) place();
                        for (size_t i = 0; i < _viewerCount; ++i) startTls(_viewers[i]);
                        for (size_t i = 0; i < _viewerCount; ++i) flushClipboard(_viewers[i]);
                        refreshCursor();
                        startUpdate();
                        (void)bmds_gui_flush(_gui);
                        // The GUI, the listener, the worker's answers, display/output's watch, the viewers.
                        pollfd  fds[4 + MaxViewers];
                        Viewer *polled[MaxViewers] = {};
                        nfds_t  n = 0;
                        fds[n++] = pollfd{bmds_gui_fd(_gui), POLLIN, 0};
                        fds[n++] = pollfd{_listen, POLLIN, 0};
                        const nfds_t doneAt = _donePipe[0] >= 0 ? n : nfds_t(-1);
                        if (_donePipe[0] >= 0) fds[n++] = pollfd{_donePipe[0], POLLIN, 0};
                        const nfds_t watchAt = _outputWatch >= 0 ? n : nfds_t(-1);
                        if (_outputWatch >= 0) fds[n++] = pollfd{_outputWatch, POLLIN, 0};
                        const nfds_t viewersAt = n;
                        const size_t viewers = _viewerCount;
                        for (size_t i = 0; i < viewers; ++i) {
                                Viewer *v = _viewers[i];
                                polled[i] = v;
                                fds[n++] = pollfd{v->fd, short(v->isFlushed() ? POLLIN : POLLIN | POLLOUT), 0};
                        }
                        // With a viewer in, a look at the cores now and then (place()).
                        const int r = poll(fds, n, viewers > 0 ? 1000 : -1);
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
                        for (size_t i = 0; i < viewers; ++i) {
                                Viewer     *v = polled[i];
                                const short got = fds[viewersAt + i].revents;
                                if (isAlive(v) && (got & (POLLIN | POLLHUP | POLLERR))) readViewer(v);
                                if (isAlive(v) && (got & POLLOUT)) (void)writeViewer(v);
                        }
                }
                return 0;
        }

        void Server::guiEvents() {
                bmds_gui_event_t e;
                int              r;
                while ((r = bmds_gui_next_event(_gui, &e, 0)) == 1) {
                        if (_settings.verbose) {
                                // A name not known yet is looked up: this event's own strings are
                                // gone then, but the ones read below are all known (start()).
                                const char *name = bmds_gui_atom_name(_gui, e.name);
                                fprintf(stderr, "event %s from %u\n", name != nullptr ? name : "?", unsigned(e.source));
                        }
                        if (e.source == 0) continue;
                        if (e.source == _clipboard) {
                                clipboardEvent(e);
                                continue;
                        }
                        if (e.source == _menuStop) {
                                _stopping = true;
                                continue;
                        }
                        if (e.source == _menuBlank) {
                                toggleBlank();
                                continue;
                        }
                        if (e.source == _menuDisconnect) {
                                while (_viewerCount > 0) drop(_viewers[_viewerCount - 1], "disconnected at the board");
                                continue;
                        }
                        if (e.source == _menuInputNone) {
                                giveInput(nullptr, true);
                                continue;
                        }
                        for (size_t i = 0; i < MaxViewers; ++i)
                                if (e.source == _menuInputTo[i] && i < _viewerCount) giveInput(_viewers[i], true);
                        for (size_t i = 0; i < _viewerCount; ++i) {
                                Viewer *v = _viewers[i];
                                if (e.source == v->screen &&
                                    bmds_gui_event_is(_gui, &e, BMDS_GUI_SCREEN_SHARE_CHANGED)) {
                                        v->changed = true;
                                } else if (e.source == v->screen &&
                                           bmds_gui_event_is(_gui, &e, BMDS_GUI_SCREEN_SHARE_POINTER_CHANGED)) {
                                        _cursorStale = true;
                                } else if (e.source == v->notification &&
                                           bmds_gui_event_is(_gui, &e, BMDS_GUI_NOTIFICATION_ACTIVATED) &&
                                           e.count == 1 && e.values[0].number == 0) {
                                        drop(v, "disconnected at the board");
                                        break;
                                }
                        }
                }
                if (r < 0) _stopping = true; // the GUI service closed the connection
                return;
        }

        // ---- The clipboard (devplan/vnc.md §19): RFB's cut text, Latin-1, the clipboard selection

        void Server::clipboardEvent(const bmds_gui_event_t &e) {
                if (bmds_gui_event_is(_gui, &e, BMDS_GUI_CLIPBOARD_CHANGED)) {
                        // Someone copied on the board (not our own copy of a viewer's): read it.
                        const char *which =
                                e.count == 1 ? bmds_gui_atom_name(_gui, uint32_t(e.values[0].number)) : nullptr;
                        if (_viewerCount == 0 || which == nullptr || strcmp(which, "clipboard") != 0) return;
                        bmds_gui_value_t owns = {};
                        if (bmds_clipboard_owns(_gui, _clipboard, "clipboard", &owns) == 0 && owns.number != 0) return;
                        (void)bmds_clipboard_paste(_gui, _clipboard, "clipboard", "text/plain;charset=utf-8");
                } else if (bmds_gui_event_is(_gui, &e, BMDS_GUI_CLIPBOARD_PASTED) && e.count == 3 && _cut != nullptr) {
                        // Kept as UTF-8 (cut at a character's start if too long): each viewer's form
                        // made as it is sent.
                        const bmds_gui_value_t &bytes = e.values[2];
                        size_t n = bytes.length < Session::MaxCutText ? bytes.length : Session::MaxCutText;
                        if (n < bytes.length)
                                while (n > 0 && (uint8_t(bytes.text[n]) & 0xc0) == 0x80) --n;
                        memcpy(_cut, bytes.text, n);
                        _cutSize = n;
                        if (_settings.verbose)
                                fprintf(stderr, "clipboard: %u bytes to the viewers\n", unsigned(_cutSize));
                        // To every viewer (output, as the screen is), each between its updates.
                        for (size_t i = 0; i < _viewerCount; ++i) {
                                _viewers[i]->cutPending = _cutSize > 0;
                                flushClipboard(_viewers[i]);
                        }
                }
                return;
        }

        // What waits for a viewer, written after what is being sent (never into a job's update): our
        // caps, a request for its copy, what the board has, the board's copy (as it takes it), a
        // provide it asked for. One that doesn't fit waits for the buffer to empty; one too big even
        // then is given up.
        void Server::flushClipboard(Viewer *v) {
                if ((_busy && v == _jobViewer) || v->session.state() != Session::State::Normal) return;
                struct Item {
                                bool *pending;
                                Clip  what;
                };
                const Item   items[] = {{&v->capsPending, Clip::Caps},
                                        {&v->requestPending, Clip::Request},
                                        {&v->notifyPending, Clip::Notify},
                                        {&v->cutPending, Clip::Copy},
                                        {&v->providePending, Clip::Provide}};
                const size_t before = v->outSize;
                for (const Item &item : items) {
                        if (!*item.pending) continue;
                        Writer w = v->appender();
                        if (!writeClipboard(v, item.what, w)) {
                                if (v->outSize > 0) break; // room once what is there is sent
                        } else {
                                v->outSize += w.size();
                        }
                        *item.pending = false;
                }
                if (v->outSize > before) (void)writeViewer(v);
                return;
        }

        bool Server::writeClipboard(const Viewer *v, Clip what, Writer &w) {
                namespace X = ExtendedClipboard;
                const bool canProvide = (v->clipFlags & X::Provide) != 0 && (v->clipFlags & X::Text) != 0;
                switch (what) {
                        case Clip::Caps:
                                X::writeCaps(w, X::Request | X::Peek | X::Notify | X::Provide, 0); // 0: notify first
                                break;
                        case Clip::Request: X::writeFlags(w, X::Request | X::Text); break;
                        case Clip::Notify: X::writeFlags(w, X::Notify | (_cutSize > 0 ? X::Text : 0)); break;
                        case Clip::Copy:
                                // Said, if it takes notifies; given, if it takes it unasked; else Latin-1.
                                if (v->extended && (v->clipFlags & X::Notify) != 0) {
                                        X::writeFlags(w, X::Notify | X::Text);
                                        break;
                                }
                                if (v->extended && canProvide && _cutSize + 1 <= v->clipTextMax)
                                        return X::writeText(w, _cut, _cutSize);
                                return Session::writeCutTextFromUtf8(w, _cut, _cutSize);
                        case Clip::Provide: return canProvide && X::writeText(w, _cut, _cutSize);
                }
                return !w.overflowed();
        }

        // ---- Viewers (devplan/vnc.md §21: several, one with the input)

        bool Server::isAlive(const Viewer *v) const {
                for (size_t i = 0; i < _viewerCount; ++i)
                        if (_viewers[i] == v) return true;
                return false;
        }

        void Server::accept() {
                sockaddr_in from = {};
                socklen_t   len = sizeof(from);
                const int   fd =
                        ::accept4(_listen, reinterpret_cast<sockaddr *>(&from), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (_settings.verbose) fprintf(stderr, "accept: %d (%s)\n", fd, fd < 0 ? strerror(errno) : "ok");
                if (fd < 0) return;
                const uint32_t address = from.sin_addr.s_addr;
                if (_viewerCount == MaxViewers || isRefused(address)) {
                        close(fd); // as many as BMDS's watches; a guesser waits
                        return;
                }
                const int on = 1;
                (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
                Session::Config c;
                c.name = "BMDS";
                c.width = _width;
                c.height = _height;
                c.password = _settings.password[0] != '\0' ? _settings.password : nullptr;
                c.vencrypt = _settings.tls;
                c.maxCutText = InBytes - 8; // a longer copy can't be held whole: skipped
                c.random = random;
                c.randomArg = this;
                auto *v = new (std::nothrow) Viewer(*this, c);
                if (v == nullptr) {
                        close(fd);
                        return;
                }
                v->out = static_cast<uint8_t *>(malloc(OutBytes));
                if (v->out == nullptr || !v->encoder.init(Encoder::TightMaxPixels)) {
                        free(v->out);
                        delete v;
                        close(fd);
                        return;
                }
                v->fd = fd;
                v->address = address;
                v->sinceMs = nowMs();
                v->width = _width;
                v->height = _height;
                char ip[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                snprintf(v->name, sizeof(v->name), "Viewer %u (%s)", unsigned(++_viewersSeen), ip);
                v->encoder.setJpeg(jpeg, this);
                v->encoder.setJpegCeiling(_settings.jpegCeiling);
                _viewers[_viewerCount++] = v;
                event("%s: connected (%u in)", v->name, unsigned(_viewerCount));
                Writer w = v->appender();
                v->session.start(w);
                v->outSize += w.size();
                (void)writeViewer(v);
                return;
        }

        void Server::drop(Viewer *v, const char *why) {
                if (!isAlive(v)) return;
                event("%s: dropped: %s%s", v->name, why, v == _input ? " (it had the input)" : "");
                if (_busy && _jobViewer == v) {
                        // The worker is using it: its answer waited for first (and left unused).
                        char result = 0;
                        while (read(_donePipe[0], &result, 1) < 0 && errno == EINTR) continue;
                        _busy = false;
                        _jobViewer = nullptr;
                }
                size_t at = 0;
                while (_viewers[at] != v) ++at;
                for (size_t i = at; i + 1 < _viewerCount; ++i) _viewers[i] = _viewers[i + 1]; // the order kept
                _viewers[--_viewerCount] = nullptr;
                if (_pixelsOwner == v) _pixelsOwner = nullptr;
                // Its ScreenShare destroyed: its watch ends, and the tray's mark with the last one.
                if (v->screen != 0) bmds_gui_destroy(_gui, v->screen);
                if (v->session.failedAuthentication()) {
                        failedFrom(v->address);
                } else if (v->watching) {
                        char body[96];
                        snprintf(body, sizeof(body), "%s: %s", v->name, why);
                        notify("Viewer left", body, "low", nullptr);
                }
                if (_input == v) {
                        // The next in the order they came, watching already (devplan/vnc.md §21).
                        Viewer *next = nullptr;
                        for (size_t i = 0; i < _viewerCount && next == nullptr; ++i)
                                if (_viewers[i]->watching) next = _viewers[i];
                        giveInput(next, false); // its keys and buttons let go first
                }
                if (_viewerCount == 0) _inputNone = false; // the next first viewer gets it again
                close(v->fd);
                free(v->out);
                delete v;
                refreshTray();
                return;
        }

        // @p v has the keys and the pointer (null: nobody; @p chosen: the tray's None, kept until
        // all have left). Injected keys and buttons are let go first: the ScreenShare that injected
        // them is made again (BMDS releases what an object pressed when it goes).
        void Server::giveInput(Viewer *v, bool chosen) {
                if (v == _input) {
                        _inputNone = v == nullptr && (chosen || _inputNone);
                        refreshTray();
                        return;
                }
                if (_input != nullptr) {
                        _input->keys = KeyState();
                        _input->buttons = 0;
                        bmds_gui_destroy(_gui, _inputShare);
                        _inputShare = bmds_screen_share_new(_gui, 0);
                }
                _input = v;
                _inputNone = v == nullptr && chosen;
                event("input: %s%s", v != nullptr ? v->name : "nobody", chosen ? " (the tray's choice)" : "");
                if (v != nullptr && _viewerCount > 1) {
                        char body[96];
                        snprintf(body, sizeof(body), "%s has the keyboard and the mouse", v->name);
                        notify("VNC input", body, "low", nullptr);
                }
                refreshTray();
                return;
        }

        void Server::readViewer(Viewer *v) {
                const ssize_t n = recv(v->fd, v->in + v->inSize, InBytes - v->inSize, 0);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                        drop(v, n == 0 ? "the viewer closed"
                                : errno == EKEYREJECTED || errno == EPROTO || errno == EBADMSG
                                        ? "TLS failed"
                                        : "the connection failed");
                        return;
                }
                if (_settings.verbose) fprintf(stderr, "recv %s: %d\n", v->name, int(n));
                if (n < 0) return;
                v->inSize += size_t(n);
                Writer       w = v->appender();
                const size_t taken = v->session.feed(v->in, v->inSize, w);
                if (!isAlive(v)) return; // a session event dropped it
                v->outSize += w.size();
                memmove(v->in, v->in + taken, v->inSize - taken);
                v->inSize -= taken;
                if (!writeViewer(v)) return;
                if (v->session.isClosed()) {
                        drop(v, v->session.closeReason() != nullptr ? v->session.closeReason() : "closed");
                } else if (v->inSize == InBytes) {
                        drop(v, "a message too long"); // nothing was taken from a full buffer
                }
                return;
        }

        // VeNCrypt (devplan/vnc.md §20): once the subtype's acknowledgement is all sent, the kernel's
        // TLS on the socket, the server's side with the board's certificate; the challenge follows
        // inside it. The viewer sends nothing more until it has that byte, so nothing is read in
        // the clear meanwhile.
        void Server::startTls(Viewer *v) {
                if (v->session.state() != Session::State::Tls || !v->isFlushed()) return;
                bmds_tls_config_t t = {};
                t.size = sizeof(t);
                t.flags = BMDS_TLS_SERVER;
                const int r = bmds_tls_ctl(v->fd, BMDS_TLS_START, &t);
                event("%s: TLS %s", v->name, r < 0 ? strerror(-r) : "started");
                if (r < 0) {
                        drop(v, "TLS couldn't start");
                        return;
                }
                Writer w = v->appender();
                v->session.tlsStarted(w);
                v->outSize += w.size();
                (void)writeViewer(v); // waits for the handshake: POLLOUT once it is done
                return;
        }

        bool Server::writeViewer(Viewer *v) {
                while (v->outSent < v->outSize) {
                        const ssize_t n = send(v->fd, v->out + v->outSent, v->outSize - v->outSent, MSG_NOSIGNAL);
                        if (_settings.verbose)
                                fprintf(stderr, "send %s %u: %d %s\n", v->name, unsigned(v->outSize - v->outSent),
                                        int(n), n < 0 ? strerror(errno) : "");
                        if (n < 0) {
                                if (errno == EAGAIN || errno == EINTR) return true;
                                drop(v, "the connection failed");
                                return false;
                        }
                        v->outSent += size_t(n);
                }
                v->outSent = v->outSize = 0;
                return true;
        }

        // ---- Updates (devplan/vnc.md §5b)

        // As many pixels as an update always carries in @p v's format (raw, the worst, and a margin
        // for headers), so a capture never leaves rectangles behind for another viewer to overwrite.
        size_t Server::captureBytes(const Viewer *v) const {
                const size_t pixels = (OutBytes - 8192) / (v->bytesPerPixel + 1);
                size_t       bytes = pixels * 3;
                if (bytes > CaptureBytes) bytes = CaptureBytes;
                return bytes & ~size_t(BMDS_CAPTURE_ALIGN - 1);
        }

        int Server::capture(Viewer *v, bool all) {
                bmds_screen_capture_t c = {};
                c.size = sizeof(c);
                c.format = BMDS_PIXEL_BGR24;
                c.flags = (all ? BMDS_CAPTURE_ALL : 0) | (v->cursor ? BMDS_CAPTURE_NO_POINTER : 0);
                c.buffer = _pixels;
                c.capacity = uint32_t(captureBytes(v));
                c.rects = v->rects;
                c.max_rects = MaxRects;
                c.share = v->screen;
                c.pointer_x = c.pointer_y = -2; // a BMDS before them leaves these
                int32_t r = bmds_gui_screen_capture(bmds_gui_fd(_gui), &c);
                if (r == -BMDS_EINVAL && v->cursor) {
                        // A BMDS without captures leaving the pointer out (start() said it had them):
                        // the screen's pointer it is, for this viewer.
                        v->cursor = false;
                        c.flags &= ~BMDS_CAPTURE_NO_POINTER;
                        r = bmds_gui_screen_capture(bmds_gui_fd(_gui), &c);
                }
                if (r == -BMDS_EACCES) return 0; // the watch isn't there yet: its changed will come
                if (r < 0) return -1;
                v->rectCount = c.rect_count;
                v->rectAt = 0;
                v->bandAt = 0;
                v->more = c.more != 0;
                v->boardX = c.pointer_x >= 0 ? c.pointer_x : -1;
                v->boardY = c.pointer_x >= 0 ? c.pointer_y : -1;
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

        // On the loop: a job for a viewer that asked for an update, has had its last one, and has
        // something due, in turns; the viewer's encoder settings given to its encoder first (idle).
        // The capture buffer's owner with rectangles left goes first; anyone else's capture would
        // overwrite them (rare: captures fit an update), so then it starts again with everything.
        void Server::startUpdate() {
                if (_busy || !_workerRunning || _viewerCount == 0) return;
                auto ready = [](Viewer *v) {
                        return v->session.state() == Session::State::Normal && v->requested && v->isFlushed();
                };
                Viewer *pick = nullptr;
                if (_pixelsOwner != nullptr && _pixelsOwner->hasLeftover() && ready(_pixelsOwner)) pick = _pixelsOwner;
                for (size_t k = 0; k < _viewerCount && pick == nullptr; ++k) {
                        Viewer *v = _viewers[(_nextServed + k) % _viewerCount];
                        if (!ready(v)) continue;
                        if (v->pendingFormat) {
                                v->encoder.setPixelFormat(v->format);
                                v->bytesPerPixel = v->format.bytesPerPixel();
                        }
                        if (v->pendingEncodings) applyEncodings(v);
                        v->pendingFormat = v->pendingEncodings = false;
                        const bool cursorDue = v->cursor && _cursorSerial != 0 && v->cursorSent != _cursorSerial;
                        if (v->full || v->changed || v->more || cursorDue || (v == _pixelsOwner && v->hasLeftover())) {
                                pick = v;
                                _nextServed = (_nextServed + k + 1) % _viewerCount;
                        }
                }
                if (pick == nullptr) return;
                if (pick->pendingFormat) {
                        pick->encoder.setPixelFormat(pick->format);
                        pick->bytesPerPixel = pick->format.bytesPerPixel();
                }
                if (pick->pendingEncodings) applyEncodings(pick);
                pick->pendingFormat = pick->pendingEncodings = false;
                const bool leftover = pick == _pixelsOwner && pick->hasLeftover();
                if (!leftover && _pixelsOwner != nullptr && _pixelsOwner->hasLeftover()) {
                        _pixelsOwner->full = true; // its rest is lost with the buffer: everything again
                        _pixelsOwner->rectAt = _pixelsOwner->rectCount = 0;
                }
                _jobViewer = pick;
                _jobFresh = !leftover;
                _jobAll = !leftover && pick->full;
                if (_jobFresh) {
                        pick->changed = false; // a change from here on tells it again
                        _pixelsOwner = pick;
                }
                _busy = true;
                const char job = 'u';
                if (write(_jobPipe[1], &job, 1) != 1) _busy = false;
                return;
        }

        // A viewer's SetEncodings given to its encoder (the worker idle), and the Cursor taken up
        // or let go: its picture then changes (the pointer drawn by the viewer, or in the pixels), so
        // everything is sent again.
        void Server::applyEncodings(Viewer *v) {
                v->encoder.setEncodings(v->encodings, v->encodingCount);
                const bool cursor = v->wantsCursor && _cursorAble;
                v->pointerPos = cursor && v->wantsPointerPos != 0;
                if (cursor != v->cursor) {
                        v->cursor = cursor;
                        v->cursorSent = 0;
                        v->full = true;
                }
                return;
        }

        // The pointer's picture fetched after pointerChanged, while no job reads it (devplan/vnc.md
        // §24): little-endian width, height, hot spot, then 0xAARRGGBB pixels.
        void Server::refreshCursor() {
                if (!_cursorStale || _busy || !_cursorAble) return;
                bool wanted = false;
                for (size_t i = 0; i < _viewerCount; ++i) wanted = wanted || _viewers[i]->wantsCursor;
                if (!wanted) return;
                _cursorStale = false;
                bmds_gui_value_t v = {};
                if (bmds_screen_share_pointer_shape(_gui, _inputShare, &v) != 0 || v.length < 8)
                        return; // the next change
                const auto *b = reinterpret_cast<const uint8_t *>(v.text);
                const auto  u16 = [b](size_t at) {
                        return uint16_t(b[at] | b[at + 1] << 8);
                };
                const uint16_t w = u16(0), h = u16(2);
                const size_t   pixels = size_t(w) * h;
                if (v.length != 8 + pixels * 4) return;
                if (pixels > size_t(_cursorWidth) * _cursorHeight || _cursor == nullptr) {
                        free(_cursor);
                        _cursor = static_cast<uint32_t *>(malloc(pixels > 0 ? pixels * 4 : 4));
                        if (_cursor == nullptr) return;
                }
                for (size_t i = 0; i < pixels; ++i) {
                        const uint8_t *p = b + 8 + i * 4;
                        _cursor[i] = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
                }
                _cursorWidth = w;
                _cursorHeight = h;
                _cursorHotX = u16(4);
                _cursorHotY = u16(6);
                if (++_cursorSerial == 0) _cursorSerial = 1;
                return;
        }

        // On the loop: the worker's answer.
        void Server::updateDone(int result) {
                _busy = false;
                Viewer *v = _jobViewer;
                _jobViewer = nullptr;
                if (v == nullptr || !isAlive(v)) return;
                switch (result) {
                        case Built:
                                if (_jobAll) v->full = false;
                                v->requested = false;
                                (void)writeViewer(v);
                                return;
                        case Nothing:
                                if (_jobAll) v->full = false;
                                return;
                        case NotWatched: return; // the request (and a full one) waits for the watch's first change
                        case CaptureFailed: drop(v, "the screen can't be captured"); return;
                        case SizeUnfollowable: drop(v, "the screen changed size and the viewer can't follow"); return;
                        case Broken: drop(v, "an update didn't fit (its stream can't be mended)"); return;
                        default: drop(v, "an update didn't fit"); return;
                }
        }

        // On the worker: @p v's capture (or its last one's rest) encoded into its out buffer.
        int Server::buildUpdate(Viewer *v, bool all, bool fresh) {
                const uint64_t t0 = nowUs();
                if (fresh) {
                        const uint64_t c0 = nowUs();
                        const int      got = capture(v, all);
                        _timing.captureUs += nowUs() - c0;
                        if (got < 0) return CaptureFailed;
                        if (got == 0) return NotWatched;
                }
                Writer out(v->out, OutBytes);
                v->outSize = v->outSent = 0;
                Session::writeUpdateHeader(out, 0);
                uint32_t count = 0;
                // The picture's size changed (a program's full screen): the viewer told, or let go.
                if (v->capturedWidth != v->width || v->capturedHeight != v->height) {
                        if (!v->encoder.hasDesktopSize()) return SizeUnfollowable;
                        v->width = v->capturedWidth;
                        v->height = v->capturedHeight;
                        v->session.setSize(v->width, v->height);
                        Session::writeDesktopSize(out, v->width, v->height);
                        ++count;
                }
                // The pointer's picture, if it changed since the viewer's last (read while the loop
                // leaves it be: a job runs).
                if (v->cursor && _cursorSerial != 0 && v->cursorSent != _cursorSerial &&
                    (v->wantsAlpha ? Session::writeCursorWithAlpha(out, _cursorWidth, _cursorHeight, _cursorHotX,
                                                                   _cursorHotY, _cursor)
                                   : Session::writeCursor(out, v->encoder.pixelFormat(), _cursorWidth, _cursorHeight,
                                                          _cursorHotX, _cursorHotY, _cursor))) {
                        v->cursorSent = _cursorSerial;
                        ++count;
                }
                // Where the board's pointer went, if it moved. Not to a viewer moving its mouse now
                // (its own place, a little behind, would pull its pointer back; a watching one's
                // would be taken from it): once its mouse is still.
                const bool moving = nowMs() - v->movedMs < PointerStillMs;
                if (v->pointerPos && v->boardX >= 0 && (v->boardX != v->posSentX || v->boardY != v->posSentY) &&
                    !moving) {
                        Session::writeRectHeader(out, uint16_t(v->boardX), uint16_t(v->boardY), 0, 0,
                                                 v->wantsPointerPos);
                        if (!out.overflowed()) {
                                v->posSentX = v->boardX;
                                v->posSentY = v->boardY;
                                ++count;
                        }
                }
                const uint8_t *pixels = _pixels;
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
                        const Picture picture{pixels + v->offsets[v->rectAt] + size_t(v->bandAt) * w * 3, size_t(w) * 3,
                                              w, rows};
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
                if (v->capturedWidth != 0) {
                        _width = v->capturedWidth; // what a new viewer starts with
                        _height = v->capturedHeight;
                }
                if (_settings.verbose) {
                        const Encoder::Stats &st = v->encoder.stats();
                        fprintf(stderr,
                                "update %s: %u rects %llu pixels %u bytes in %llu us on core %d (capture %llu, jpeg "
                                "%llu, zlib %llu, encode %llu)%s\n",
                                v->name, unsigned(count), (unsigned long long)(st.pixels - v->pixelsSeen),
                                unsigned(out.size()), (unsigned long long)(nowUs() - t0), _workerCore,
                                (unsigned long long)_timing.captureUs, (unsigned long long)_timing.jpegUs,
                                (unsigned long long)(st.zlibUs - v->zlibSeen),
                                (unsigned long long)(st.encodeUs - v->encodeSeen),
                                v->hasLeftover() ? ", more left" : "");
                }
                _timing = Timing();
                v->zlibSeen = v->encoder.stats().zlibUs;
                v->encodeSeen = v->encoder.stats().encodeUs;
                v->pixelsSeen = v->encoder.stats().pixels;
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
                        const char result = char(buildUpdate(_jobViewer, _jobAll, _jobFresh) + 8); // 0..15 on the pipe
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
                Viewer *v = _viewer;
                // Watched only while a viewer is in, each through its own ScreenShare (its own changes):
                // the tray's mark (a red dot on our icon) says so. Its first changed (sent as the watch
                // begins) lets its updates start: a capture before the service took it would be refused.
                v->screen = bmds_screen_share_new(_server._gui, 0);
                bmds_screen_share_watch(_server._gui, v->screen, 1);
                v->watching = true;
                _server.event("%s: watching (input %s)", v->name,
                              _server._input != nullptr ? _server._input->name
                              : _server._inputNone      ? "none, chosen"
                                                        : "free");
                if (_server._input == nullptr && !_server._inputNone) _server.giveInput(v, false);
                char body[96];
                snprintf(body, sizeof(body), "%s is viewing the screen%s", v->name,
                         _server._input == v ? "" : " (watching only)");
                _server.notify("Viewer connected", body, "normal", &v->notification);
                _server.refreshTray();
                return;
        }

        // The encoder is the worker's while a job runs: these wait for startUpdate().
        void Server::Events::setPixelFormat(const PixelFormat &format) {
                _viewer->format = format;
                _viewer->pendingFormat = true;
                return;
        }

        void Server::Events::setEncodings(const int32_t *encodings, size_t count) {
                Viewer *v = _viewer;
                v->encodingCount = count < 64 ? count : 64;
                for (size_t i = 0; i < v->encodingCount; ++i) v->encodings[i] = encodings[i];
                v->pendingEncodings = true;
                v->wantsCursor = v->wantsAlpha = false;
                v->wantsPointerPos = 0;
                if (_server._settings.verbose) {
                        // What a viewer asks for, as it lists it (a real viewer's choices, -v).
                        fprintf(stderr, "%s: encodings", v->name);
                        for (size_t i = 0; i < count; ++i) fprintf(stderr, " %ld", long(encodings[i]));
                        fprintf(stderr, "\n");
                }
                for (size_t i = 0; i < count; ++i) {
                        const int32_t e = encodings[i];
                        if (e == Encoding::Cursor) v->wantsCursor = true;
                        if (e == Encoding::CursorWithAlpha) v->wantsCursor = v->wantsAlpha = true;
                        if ((e == Encoding::PointerPos || e == Encoding::VmwareCursorPosition) &&
                            v->wantsPointerPos == 0)
                                v->wantsPointerPos = e;
                }
                // The Extended Clipboard: our caps after each SetEncodings asking for it (the spec).
                for (size_t i = 0; i < count; ++i)
                        if (encodings[i] == Encoding::ExtendedClipboard) {
                                v->extended = true;
                                v->capsPending = true;
                        }
                return;
        }

        void Server::Events::updateRequest(bool incremental, uint16_t, uint16_t, uint16_t, uint16_t) {
                _viewer->requested = true;
                if (!incremental) _viewer->full = true;
                return;
        }

        // Keys and the pointer only from the viewer with the input; the others watch.
        void Server::Events::key(bool down, uint32_t keysym) {
                Viewer *v = _viewer;
                if (v != _server._input) return;
                const KeyState::Key k = v->keys.event(down, keysym);
                if (k.usage != 0) {
                        const int r = bmds_screen_share_inject_key(_server._gui, _server._inputShare, k.usage,
                                                                   k.modifiers, down ? 1 : 0);
                        if (r != 0 && v->refused++ < 5)
                                _server.event("%s: key 0x%x (usage %u) refused: %d", v->name, unsigned(keysym),
                                              unsigned(k.usage), r);
                }
                return;
        }

        void Server::Events::pointer(uint8_t buttons, uint16_t x, uint16_t y) {
                Viewer *v = _viewer;
                if (v != _server._input) return;
                // The wheel: buttons 4 and 5 (away, towards), a notch at each press.
                const uint8_t pressed = uint8_t(buttons & ~v->buttons);
                const int     wheel = ((pressed & 0x08) ? 1 : 0) - ((pressed & 0x10) ? 1 : 0);
                v->buttons = buttons;
                v->movedMs = nowMs();
                v->posSentX = x; // where its pointer is (its own drawing): no need to tell it
                v->posSentY = y;
                const int32_t px = x < v->width ? x : int32_t(v->width) - 1;
                const int32_t py = y < v->height ? y : int32_t(v->height) - 1;
                const int     r = bmds_screen_share_inject_pointer(_server._gui, _server._inputShare, px, py,
                                                                   bmdsButtons(buttons), wheel);
                if (r != 0 && v->refused++ < 5)
                        _server.event("%s: pointer %d,%d buttons %u refused: %d", v->name, int(px), int(py),
                                      unsigned(buttons), r);
                return;
        }

        // The viewer with the input copied: onto the board's clipboard as UTF-8 (our copy: its changed
        // isn't sent back). A watching viewer's copies are input too, and left.
        void Server::Events::cutText(const char *text, size_t length) {
                if (_viewer != _server._input || _server._clipboard == 0) return;
                char *utf8 = static_cast<char *>(malloc(length * 2 + 1));
                if (utf8 == nullptr) return;
                const size_t n = vnc::Text::utf8FromLatin1(text, length, utf8, length * 2);
                utf8[n] = '\0';
                _server.pasteFromViewer(_viewer, utf8);
                free(utf8);
                return;
        }

        // The Extended Clipboard (devplan/vnc.md §23): its caps kept; a copy it says it has asked
        // for (from the viewer with the input), its text when it comes; the board's when it asks.
        void Server::Events::extendedClipboard(uint32_t flags, const uint8_t *data, size_t length) {
                namespace X = ExtendedClipboard;
                Viewer *v = _viewer;
                if ((flags & X::Caps) != 0) {
                        v->clipFlags = flags;
                        v->clipTextMax = X::capsTextMax(flags, data, length);
                } else if ((flags & X::Request) != 0) {
                        if ((flags & X::Text) != 0 && _server._cutSize > 0) v->providePending = true;
                } else if ((flags & X::Peek) != 0) {
                        v->notifyPending = true;
                } else if ((flags & X::Notify) != 0) {
                        if ((flags & X::Text) != 0 && v == _server._input) v->requestPending = true;
                } else if ((flags & X::Provide) != 0) {
                        if (v != _server._input || _server._clipboard == 0) return;
                        char *text = static_cast<char *>(malloc(Session::MaxCutText + 1));
                        if (text == nullptr) return;
                        const long n = X::readText(flags, data, length, text, Session::MaxCutText);
                        if (n >= 0) {
                                text[n] = '\0';
                                _server.pasteFromViewer(v, text);
                        } else {
                                _server.event("%s: its copy not taken (past %u KB, or unreadable)", v->name,
                                              unsigned(Session::MaxCutText / 1024));
                        }
                        free(text);
                }
                return; // what it asked for is sent by the loop (flushClipboard())
        }

        // A viewer's copy onto the board's clipboard selection (our copy: its changed isn't sent back).
        void Server::pasteFromViewer(Viewer *v, const char *utf8) {
                (void)bmds_clipboard_copy_text(_gui, _clipboard, "clipboard", utf8);
                if (_settings.verbose)
                        fprintf(stderr, "clipboard: %u bytes from %s\n", unsigned(strlen(utf8)), v->name);
                return;
        }

        // ---- Telling the user (devplan/vnc.md §5d)

        // @p withDisconnect: the notification has Disconnect, its id kept there (its viewer's).
        void Server::notify(const char *title, const char *body, const char *urgency, uint32_t *withDisconnect) {
                const uint32_t n = bmds_notification_new(_gui, 0);
                bmds_notification_set_app(_gui, n, "VNC Server");
                bmds_notification_set_icon(_gui, n, TrayIconName);
                bmds_notification_set_title(_gui, n, title);
                bmds_notification_set_body(_gui, n, body);
                bmds_notification_set_urgency(_gui, n, urgency);
                if (withDisconnect != nullptr) {
                        const char *actions[] = {"Disconnect"};
                        bmds_notification_set_actions(_gui, n, actions, 1);
                        *withDisconnect = n;
                } else {
                        bmds_notification_set_transient(_gui, n, 1);
                }
                bmds_notification_show(_gui, n);
                return;
        }

        // The tip, and the menu made again (its entries keep the order they are made in): the
        // viewers' line, Give Input To (each viewer, None), Disconnect All, Blank Display, Stop.
        void Server::refreshTray() {
                if (_tray == 0) return;
                char   tip[160], line[64];
                size_t watching = 0;
                for (size_t i = 0; i < _viewerCount; ++i) watching += _viewers[i]->watching ? 1 : 0;
                if (watching == 0) {
                        snprintf(tip, sizeof(tip), "VNC server on %s: no viewer", _address);
                        snprintf(line, sizeof(line), "No viewer");
                } else {
                        snprintf(tip, sizeof(tip), "VNC server on %s: %u watching%s%s%s", _address, unsigned(watching),
                                 _input != nullptr ? ", input " : "", _input != nullptr ? "from " : "",
                                 _input != nullptr ? _input->name : "");
                        snprintf(line, sizeof(line), "%u viewer%s", unsigned(watching), watching == 1 ? "" : "s");
                }
                bmds_tray_icon_set_tool_tip(_gui, _tray, tip);
                if (_trayMenu != 0) bmds_gui_destroy(_gui, _trayMenu);
                _trayMenu = bmds_menu_new(_gui, _tray); // a tray icon's menu: its context menu
                _menuViewers = bmds_menu_item_new(_gui, _trayMenu);
                bmds_menu_item_set_text(_gui, _menuViewers, line);
                bmds_menu_item_set_enabled(_gui, _menuViewers, 0);
                _menuInput = bmds_menu_new(_gui, _trayMenu);
                bmds_menu_set_title(_gui, _menuInput, "Give &Input To");
                for (size_t i = 0; i < MaxViewers; ++i) _menuInputTo[i] = 0;
                for (size_t i = 0; i < _viewerCount; ++i) {
                        const Viewer *v = _viewers[i];
                        if (!v->watching) continue;
                        const unsigned minutes = unsigned((nowMs() - v->sinceMs) / 60000u);
                        char           text[64];
                        snprintf(text, sizeof(text), "%s, %u min", v->name, minutes);
                        _menuInputTo[i] = bmds_menu_item_new(_gui, _menuInput);
                        bmds_menu_item_set_text(_gui, _menuInputTo[i], text);
                        bmds_menu_item_set_checked(_gui, _menuInputTo[i], v == _input ? 1 : 0);
                }
                bmds_menu_add_separator(_gui, _menuInput);
                _menuInputNone = bmds_menu_item_new(_gui, _menuInput);
                bmds_menu_item_set_text(_gui, _menuInputNone, "&None (everyone watches)");
                bmds_menu_item_set_checked(_gui, _menuInputNone, _input == nullptr ? 1 : 0);
                _menuDisconnect = bmds_menu_item_new(_gui, _trayMenu);
                bmds_menu_item_set_text(_gui, _menuDisconnect, watching > 1 ? "&Disconnect All" : "&Disconnect");
                bmds_menu_item_set_enabled(_gui, _menuDisconnect, _viewerCount > 0 ? 1 : 0);
                _menuBlank = 0;
                if (_outputId >= 0) {
                        bmds_menu_add_separator(_gui, _trayMenu);
                        _menuBlank = bmds_menu_item_new(_gui, _trayMenu);
                        bmds_menu_item_set_text(_gui, _menuBlank, "&Blank Display While Connected");
                        readBlank();
                }
                bmds_menu_add_separator(_gui, _trayMenu);
                _menuStop = bmds_menu_item_new(_gui, _trayMenu);
                bmds_menu_item_set_text(_gui, _menuStop, "&Stop Server");
                return;
        }

        void Server::readBlank() {
                char value[16] = {};
                if (_outputId < 0 || bmds_sysconf_get(_outputId, BMDS_SYSCONF_FIELD_VALUE, value, sizeof(value)) < 0)
                        return;
                _blank = strcmp(value, "remote") == 0;
                if (_menuBlank != 0) bmds_menu_item_set_checked(_gui, _menuBlank, _blank ? 1 : 0);
                return;
        }

        // Checked: the display off while a viewer is in (and on again as it leaves); unchecked: on.
        // Saved, as Settings saves it: the user's choice outlives the server.
        void Server::toggleBlank() {
                const char *want = _blank ? "on" : "remote";
                if (bmds_sysconf_set(_outputId, BMDS_SYSCONF_OP_SET, want) < 0) {
                        notify("VNC server", "The display setting can't be changed", "normal", nullptr);
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
                        notify("VNC: someone is guessing", body, "critical", nullptr);
                } else {
                        snprintf(body, sizeof(body), "%s gave a wrong password", name);
                        notify("VNC: wrong password", body, "normal", nullptr);
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
