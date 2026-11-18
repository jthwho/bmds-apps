/**
 * @file      app.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "buffers.h"
#include "commands.h"
#include "complete.h"
#include "connection.h"
#include "logs.h"
#include "message.h"
#include "session.h"
#include "settings.h"

#include <bmds/guiclasses.h>

#include <stdint.h>

namespace irc {

        /** @brief One network: its settings, protocol, buffers and socket (devplan/irc.md §3). */
        struct Network {
                        NetworkSettings settings;
                        Session         session;
                        Buffers         buffers;
                        Connection      connection;
                        LineReader      reader;
                        bool            wanted = false; ///< The user wants it connected (reconnects).
                        unsigned        attempts = 0;   ///< Failed connections in a row.
                        int64_t         reconnectAt = -1;

                        explicit Network(NetworkSettings &&s) : session(std::move(s.session)) {
                                settings = std::move(s);
                        }
        };

        /** @brief A buffer's tab: its page and widgets, and what the tab shows as last set. */
        struct Page {
                        uint32_t network = 0; ///< Its index in the networks (`NoNetwork`: the app's own page).
                        uint32_t buffer = 0;
                        uint32_t tab = 0, chat = 0, topic = 0, members = 0;
                        String   title, badge;
                        uint8_t  attention = 0;
        };

        /**
         * @brief The IRC client (devplan/irc.md §6): one window, a tab a conversation, the input
         *        line below, the networks' sockets and the GUI in one `poll()`.
         */
        class App {
                public:
                        static constexpr uint32_t NoNetwork = UINT32_MAX;

                        App() = default;
                        ~App();

                        /** @brief Runs it until the window closes. @return The exit status. */
                        int run();

                private:
                        bool  checkSystem();
                        void  loadSettings();
                        void  buildWindow();
                        Now   now() const;
                        void  pollOnce();
                        void  gui(const bmds_gui_event_t &e);
                        void  connect(uint32_t n);
                        void  disconnect(uint32_t n, const char *why, bool again);
                        void  serviceNetwork(uint32_t n, short revents);
                        void  pump(uint32_t n);
                        void  react(uint32_t n, const Event &e);
                        void  show(uint32_t n, Vector<Line> &lines);
                        Page *page(uint32_t n, uint32_t buffer, bool make);
                        Page *pageOf(uint32_t id); // by its tab, chat view or member list
                        void  syncPages(uint32_t n);
                        void  closePage(size_t index);
                        void  setCurrent(size_t index, bool tell);
                        void  refreshMembers(uint32_t n, Text channel);
                        void  refreshTopic(uint32_t n, Text channel);
                        void  updateStatus();
                        void  typed(Text text);
                        void  complete();
                        void  history(int step);
                        void  note(Text text);
                        void  quit();
                        void  menu(uint32_t id);

                        bmds_gui_t *_gui = nullptr;
                        uint32_t    _window = 0, _tabs = 0, _entry = 0, _nick = 0, _status = 0, _clipboard = 0;
                        uint32_t    _connectItem = 0, _disconnectItem = 0, _quitItem = 0, _joinItem = 0, _partItem = 0,
                                 _closeItem = 0, _joinsItem = 0, _aboutItem = 0;
                        Vector<Network *> _networks;
                        Vector<Page>      _pages; // in the tabs' order
                        size_t            _current = 0;
                        bool              _focused = true;
                        bool              _done = false;
                        Logs              _logs;
                        AppSettings       _settings;
                        Vector<String>    _problems;
                        String            _dataDir;
                        Completer         _completer;
                        Vector<String>    _history;
                        size_t            _historyAt = 0;
                        int32_t           _utcOffset = 0;
                        int64_t           _flushAt = -1;
        };

} // namespace irc
