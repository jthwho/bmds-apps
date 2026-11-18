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
                        bool            removed = false;  ///< Taken out in Networks... (kept, idle, until the end).
                        bool            probing = false;  ///< A connection only to read a certificate not trusted.
                        String          registerNick;     ///< Register this nick...: the nick and password
                        String          registerPassword; ///< sent to NickServ, saved once it says registered.
                        bool            treeRoot = false; ///< Its row in the tree made.
                        String          treeKids;         ///< The tree's children as last given (names, LF-ended).

                        // The session gets a copy of the settings' config: they stay whole (saved, edited).
                        explicit Network(NetworkSettings &&s) : session(Config()) {
                                settings = std::move(s);
                                NetworkSettings copy;
                                if (copySettings(settings, copy) == Error::Ok)
                                        session.setConfig(std::move(copy.session));
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
                        void  show(uint32_t n, Vector<Line> &lines, const Event *event = nullptr);
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
                        void  notify(uint32_t n, const Event &e, const Line &line);
                        void  showWindow();
                        void  updateTray();
                        void  setTree(bool tree);
                        void  saveSettings();
                        void  treePath(const Page &p, String &out) const;
                        void  resetTree();
                        void  updateSecrets(uint32_t n);
                        void  resolvePaths(NetworkSettings &s) const;
                        void  saveNetworks();
                        void  askTrust(uint32_t n);
                        void  registration(uint32_t n, const Event &e);

                        // The dialogs (dialogs.cpp).
                        void openNetworks();
                        void loadNetwork(size_t i);
                        void storeNetwork(size_t i);
                        void listNetworks();
                        void saveNetworkDialog();
                        void closeNetworks();
                        void removeNetwork(uint32_t n);
                        void openRegister();
                        void closeRegister();
                        bool dialogEvent(const bmds_gui_event_t &e);

                        bmds_gui_t *_gui = nullptr;
                        uint32_t    _window = 0, _tabs = 0, _entry = 0, _nick = 0, _status = 0, _clipboard = 0;
                        uint32_t    _split = 0, _tree = 0, _tray = 0, _trayShow = 0, _trayQuit = 0, _treeItem = 0;
                        int         _trayMentions = -1; // the tray as last set
                        struct Note {
                                        uint32_t id = 0, network = 0, buffer = 0;
                        };
                        Vector<Note> _notes; // notifications shown
                        uint32_t     _networksItem = 0, _registerItem = 0;
                        uint32_t     _trustBox = 0, _trustNetwork = 0; // the question about a certificate

                        // Networks...: the window, its widgets, the networks as edited.
                        struct NetworkFields {
                                        uint32_t window = 0, list = 0, add = 0, remove = 0, save = 0, cancel = 0,
                                                 status = 0;
                                        uint32_t name = 0, host = 0, port = 0, tls = 0, nick = 0, altNicks = 0,
                                                 user = 0, realName = 0, login = 0, account = 0, password = 0,
                                                 channels = 0, cert = 0, key = 0, ca = 0, fingerprint = 0,
                                                 autoconnect = 0;
                        };
                        NetworkFields           _nf;
                        Vector<NetworkSettings> _edit;
                        Vector<int32_t>         _editOrigin; // each one's network (-1: new)
                        size_t                  _editAt = 0;

                        // Register this nick...
                        struct RegisterFields {
                                        uint32_t window = 0, password = 0, again = 0, email = 0, send = 0, code = 0,
                                                 verify = 0, status = 0, close = 0, network = 0;
                        };
                        RegisterFields _rf;
                        uint32_t _connectItem = 0, _disconnectItem = 0, _quitItem = 0, _joinItem = 0, _partItem = 0,
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
