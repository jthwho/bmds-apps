/**
 * @file      server.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "encode.h"
#include "keysym.h"
#include "rfb.h"

#include <bmds/gui.h>

#include <cstddef>
#include <cstdint>
#include <stdio.h>

namespace vnc {

        /** @brief What `vncserver` runs with: its settings, read once at the start. */
        struct Settings {
                        uint16_t port = 5900;
                        bool     lan = false; ///< Every interface, not only the loopback.
                        bool     tls = false; ///< VeNCrypt only (X509Vnc: the board's certificate), on the LAN.
                        int      jpegCeiling = 80;
                        int      startWaitS = 30;
                        char     password[64] = {}; ///< From the password file; empty: none.
                        bool     verbose = false;   ///< -v: what happens, on stderr (the foreground's)
        };

        /**
         * @brief The VNC server (devplan/vnc.md in this repository; BMDS's devplan/vnc.md for what
         *        the system gives it): up to four viewers, each watching the screen with its own
         *        changes, its updates captured, encoded and sent as it asks; one of them (the first
         *        in, or the tray's choice) has the keys and the pointer, the rest watch; a tray icon
         *        for the server's life and notifications for viewers.
         *
         * @par Thread Safety
         * One thread.
         */
        class Server {
                public:
                        explicit Server(const Settings &settings);
                        ~Server();
                        Server(const Server &) = delete;
                        Server &operator=(const Server &) = delete;

                        /**
                         * @brief Connects to the GUI (waiting up to `startWaitS` for it), listens, shows
                         *        the tray icon. @p ready, if not -1, is written "0 ADDRESS\n" or "1
                         *        REASON\n" and closed (a starting `vncserver`'s pipe).
                         * @return 0, or 1 with the reason said.
                         */
                        int start(int ready);

                        /** @brief Runs until stopped (`SIGTERM`, the tray's Stop Server). @return The exit status. */
                        int run();

                        /** @brief Where it listens, as said ("127.0.0.1:5900", "every interface:5900"). */
                        const char *address() const { return _address; }

                private:
                        // One viewer.
                        struct Viewer;

                        /** @brief The most viewers at once: BMDS's screen watches (devplan/vnc.md §21). */
                        static constexpr size_t MaxViewers = 4;

                        // The Handler a viewer's session calls.
                        class Events final : public Handler {
                                public:
                                        Events(Server &server, Viewer *viewer) : _server(server), _viewer(viewer) {}
                                        void ready() override;
                                        void setPixelFormat(const PixelFormat &format) override;
                                        void setEncodings(const int32_t *encodings, size_t count) override;
                                        void updateRequest(bool incremental, uint16_t x, uint16_t y, uint16_t w,
                                                           uint16_t h) override;
                                        void key(bool down, uint32_t keysym) override;
                                        void pointer(uint8_t buttons, uint16_t x, uint16_t y) override;
                                        void cutText(const char *text, size_t length) override;

                                private:
                                        Server &_server;
                                        Viewer *_viewer;
                        };

                        bool   connectGui();
                        bool   listen();
                        void   accept();
                        bool   isAlive(const Viewer *v) const;   // still connected (not dropped meanwhile)
                        void   drop(Viewer *v, const char *why); // the viewer gone
                        void   readViewer(Viewer *v);
                        bool   writeViewer(Viewer *v); // false: the connection failed (dropped)
                        void   startTls(Viewer *v);    // VeNCrypt: TLS on its socket once its answer is sent
                        void   startUpdate(); // an update's job for a viewer given to the worker, if one is due
                        void   updateDone(int result);                       // the worker's answer, on the loop
                        int    buildUpdate(Viewer *v, bool all, bool fresh); // the worker's: capture and encode
                        int    capture(Viewer *v, bool all); // into the capture buffer: 1, 0 (not watched yet), -1
                        size_t captureBytes(const Viewer *v) const; // what one update can always carry
                        bool   startWorker(int core);               // the encoding thread, kept to @p core
                        void   stopWorker();
                        void   place(); // the worker moved to the less busy core, if it should
                        static void *workerEntry(void *arg) __attribute__((noinline));
                        void         workerLoop();
                        void         giveInput(Viewer *v, bool chosen); // null: nobody's (None when @p chosen)
                        void         guiEvents();
                        void         clipboardEvent(const bmds_gui_event_t &e); // the board's copies
                        void         flushCut(Viewer *v); // a board copy waiting, sent between updates
                        void notify(const char *title, const char *body, const char *urgency, uint32_t *withDisconnect);
                        // What happened, in /tmp/vncserver.log (and on stderr with -v): connections, TLS,
                        // the input, drops and why, refused injections.
                        void event(const char *format, ...) __attribute__((format(printf, 2, 3)));
                        void refreshTray();               // the tip and the menu (rebuilt: the viewers and the input)
                        void readBlank();                 // the Blank Display item's check from display/output
                        void toggleBlank();               // display/output between remote and on, saved
                        bool isRefused(uint32_t address); // too many failed passwords lately
                        void failedFrom(uint32_t address);

                        static long jpeg(void *arg, const Picture &picture, int quality, uint8_t *out, size_t capacity);
                        static void random(void *arg, uint8_t *out, size_t n);

                        Settings    _settings;
                        FILE       *_log = nullptr; // /tmp/vncserver.log
                        bmds_gui_t *_gui = nullptr;
                        uint32_t    _inputShare = 0; // the ScreenShare that injects (made again as input moves)
                        uint32_t    _clipboard = 0;  // the Clipboard object (devplan/vnc.md §19)
                        char       *_cut = nullptr;  // the board's last copy as Latin-1, to send
                        size_t      _cutSize = 0;
                        uint32_t    _tray = 0;
                        uint32_t    _trayMenu = 0;
                        uint32_t    _menuViewers = 0; // the viewers' line (disabled)
                        uint32_t    _menuInput = 0;   // Give Input To, a submenu
                        uint32_t    _menuInputTo[MaxViewers] = {};
                        uint32_t    _menuInputNone = 0;
                        uint32_t    _menuDisconnect = 0;
                        uint32_t    _menuStop = 0;
                        uint32_t    _menuBlank = 0;    // Blank Display While Connected (display/output)
                        int32_t     _outputId = -1;    // display/output's sysconf id
                        int         _outputWatch = -1; // its watch: the item follows changes made elsewhere
                        bool        _blank = false;    // display/output is remote
                        int         _listen = -1;
                        uint16_t    _width = 0; // the screen, as last captured (a new viewer's ServerInit)
                        uint16_t    _height = 0;
                        char        _address[48] = {}; // where it listens, as said
                        // The viewers, in the order they came; the one with the input (devplan/vnc.md §21).
                        Viewer  *_viewers[MaxViewers] = {};
                        size_t   _viewerCount = 0;
                        Viewer  *_input = nullptr;
                        bool     _inputNone = false; // None chosen in the tray: nobody gets it until all leave
                        size_t   _nextServed = 0;    // where the next update's search starts (turns)
                        uint32_t _viewersSeen = 0;   // viewers numbered as they come (two from one address)
                        // The capture buffer, one for all (a job at a time): its rectangles are the owner's.
                        uint8_t *_pixels = nullptr;
                        Viewer  *_pixelsOwner = nullptr;
                        // The worker (devplan/vnc.md §17): captures and encodes on the core less busy,
                        // while this thread keeps the sockets and the GUI. A job at a time.
                        bool     _workerRunning = false;
                        void    *_workerThread = nullptr; // pthread_t, kept opaque here
                        int      _workerCore = -1;
                        int      _jobPipe[2] = {-1, -1};  // the loop to the worker: 'u' an update, 'q' quit
                        int      _donePipe[2] = {-1, -1}; // the worker to the loop: its result
                        bool     _busy = false;           // a job given and not answered
                        Viewer  *_jobViewer = nullptr;    // the job's viewer
                        bool     _jobAll = false;         // the job's: the whole screen
                        bool     _jobFresh = false;       // and capture first (else the last capture's rest)
                        uint64_t _workUs = 0;             // the worker's time on jobs since the last placement
                        // Each core's load (cpu.coreN.busy_us), for place().
                        uint32_t _loadIds[2] = {};
                        bool     _loadOk = false;
                        int64_t  _loadBusy[2] = {};
                        int64_t  _loadNs = 0;
                        uint64_t _placedMs = 0;
                        bool     _stopping = false;
                        // Failed passwords, by address (the last few).
                        struct Failure {
                                        uint32_t address = 0;
                                        uint32_t count = 0;
                                        uint64_t firstMs = 0;
                                        uint64_t refusedUntilMs = 0;
                        };
                        Failure _failures[8];
                        // -v: where an update's time went.
                        struct Timing {
                                        uint64_t captureUs = 0;
                                        uint64_t jpegUs = 0;
                        };
                        Timing _timing;
        };

} // namespace vnc
