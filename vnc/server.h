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

namespace vnc {

        /** @brief What `vncserver` runs with: its settings, read once at the start. */
        struct Settings {
                        uint16_t port = 5900;
                        bool     lan = false; ///< Every interface, not only the loopback.
                        int      jpegCeiling = 80;
                        int      startWaitS = 30;
                        char     password[64] = {}; ///< From the password file; empty: none.
                        bool     verbose = false;   ///< -v: what happens, on stderr (the foreground's)
        };

        /**
         * @brief The VNC server (devplan/vnc.md in this repository; BMDS's devplan/vnc.md for what
         *        the system gives it): one viewer at a time, the screen watched while it is
         *        connected, its updates captured, encoded and sent as it asks, its keys and pointer
         *        injected; a tray icon for the server's life and notifications for viewers.
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

                        // The Handler the session calls: its viewer's.
                        class Events final : public Handler {
                                public:
                                        explicit Events(Server &server) : _server(server) {}
                                        void ready() override;
                                        void setPixelFormat(const PixelFormat &format) override;
                                        void setEncodings(const int32_t *encodings, size_t count) override;
                                        void updateRequest(bool incremental, uint16_t x, uint16_t y, uint16_t w,
                                                           uint16_t h) override;
                                        void key(bool down, uint32_t keysym) override;
                                        void pointer(uint8_t buttons, uint16_t x, uint16_t y) override;

                                private:
                                        Server &_server;
                        };

                        bool         connectGui();
                        bool         listen();
                        void         accept();
                        void         drop(const char *why); // the viewer gone
                        void         readViewer();
                        bool         writeViewer();          // false: the connection failed
                        void         startUpdate();          // an update's job given to the worker, if one is due
                        void         updateDone(int result); // the worker's answer, on the loop
                        int          buildUpdate(bool all, bool fresh); // the worker's: capture and encode (Built...)
                        bool         startWorker(int core);             // the encoding thread, kept to @p core
                        void         stopWorker();
                        void         place(); // the worker moved to the less busy core, if it should
                        static void *workerEntry(void *arg) __attribute__((noinline));
                        void         workerLoop();
                        int          capture(bool all); // into the capture buffer: 1, 0 (not watched yet), -1
                        void         guiEvents();
                        void notify(const char *title, const char *body, const char *urgency, bool withDisconnect);
                        void refreshTray();
                        void readBlank();                 // the Blank Display item's check from display/output
                        void toggleBlank();               // display/output between remote and on, saved
                        bool isRefused(uint32_t address); // too many failed passwords lately
                        void failedFrom(uint32_t address);

                        static long jpeg(void *arg, const Picture &picture, int quality, uint8_t *out, size_t capacity);
                        static void random(void *arg, uint8_t *out, size_t n);

                        Settings    _settings;
                        bmds_gui_t *_gui = nullptr;
                        uint32_t    _screen = 0; // the ScreenShare object
                        uint32_t    _tray = 0;
                        uint32_t    _trayMenu = 0;
                        uint32_t    _menuViewer = 0; // the viewer's line (disabled)
                        uint32_t    _menuDisconnect = 0;
                        uint32_t    _menuStop = 0;
                        uint32_t    _menuBlank = 0;    // Blank Display While Connected (display/output)
                        int32_t     _outputId = -1;    // display/output's sysconf id
                        int         _outputWatch = -1; // its watch: the item follows changes made elsewhere
                        bool        _blank = false;    // display/output is remote
                        uint32_t    _notification = 0; // the last one with Disconnect
                        int         _listen = -1;
                        uint16_t    _width = 0; // the picture being sent
                        uint16_t    _height = 0;
                        char        _address[48] = {}; // where it listens, as said
                        Viewer     *_viewer = nullptr;
                        // The worker (devplan/vnc.md §17): captures and encodes on the core less busy,
                        // while this thread keeps the sockets and the GUI. A job at a time.
                        bool     _workerRunning = false;
                        void    *_workerThread = nullptr; // pthread_t, kept opaque here
                        int      _workerCore = -1;
                        int      _jobPipe[2] = {-1, -1};  // the loop to the worker: 'u' an update, 'q' quit
                        int      _donePipe[2] = {-1, -1}; // the worker to the loop: its result
                        bool     _busy = false;           // a job given and not answered
                        bool     _jobAll = false;         // the job's: the whole screen
                        bool     _jobFresh = false;       // and capture first (else the last capture's rest)
                        uint64_t _workUs = 0;             // the worker's time on jobs since the last placement
                        // Each core's load (cpu.coreN.busy_us), for place().
                        uint32_t _loadIds[2] = {};
                        bool     _loadOk = false;
                        int64_t  _loadBusy[2] = {};
                        int64_t  _loadNs = 0;
                        uint64_t _placedMs = 0;
                        Events   _events{*this};
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
                                        uint64_t zlibSeen = 0; // the encoder's totals at the last update
                                        uint64_t encodeSeen = 0;
                                        uint64_t pixelsSeen = 0;
                        };
                        Timing _timing;
        };

} // namespace vnc
