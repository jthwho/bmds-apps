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
                        struct Seen {
                                        String  nick;
                                        int64_t at = 0; ///< When last told on or off (ms since 1970).
                                        bool    on = false;
                        };
                        Vector<Seen> seen; ///< The notify list's people as last told (§7g).

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
                        Vector<String> nicks; ///< The member list's nicks, in its order.
        };

        /** @brief A channel of a list (§7d): its name and topic in the list's text. */
        struct ListedChannel {
                        uint32_t name = 0, topic = 0; ///< Offsets in the text.
                        uint16_t nameLength = 0, topicLength = 0;
                        uint32_t users = 0;
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

                        // The step 5 additions (devplan/irc.md §7c-§7l).
                        void runIn(uint32_t n, uint32_t buffer, Text command);
                        void applyBehaviour(Network &net);
                        void watchAll(uint32_t n);
                        void fulfil(uint32_t n, uint32_t buffer, Outcome &o); // what a command left to the app
                        void openFind(Text text);
                        void find(bool backward);
                        void pasted(Text text);
                        void sendPaste();
                        void closePaste();
                        void notifyOnline(uint32_t n, const Event &e);
                        void sawWatched(uint32_t n, const Event &e);
                        bool routeEvent(uint32_t n, const Event &e); // a dialog's: not shown in the buffers
                        bool actionsEvent(const bmds_gui_event_t &e);

                        // The member list's menu and its reasons (people.cpp).
                        void memberMenu(Page &p, int row, int x, int y);
                        bool memberChosen(uint32_t item);
                        void askReason(uint8_t kind);
                        void openNotifyList();
                        void listNotify();
                        void closeNotifyList();
                        bool peopleEvent(const bmds_gui_event_t &e);

                        // The channel list and Channel Central (channels.cpp).
                        void openChannelList(uint32_t n, Text filter);
                        void listEvent(const Event &e);
                        void showChannels();
                        void closeChannelList();
                        void openCentral();
                        void centralEvent(const Event &e);
                        void showModeList();
                        void applyCentral();
                        void closeCentral();
                        bool channelsEvent(const bmds_gui_event_t &e);

                        // The dialogs (dialogs.cpp).
                        void openPerform();
                        void storePerform();
                        void performLabel();
                        void openServerList();
                        void showServers();
                        void closeServerList();
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
                                        String   nick; ///< Someone watched came on: Open opens a private chat.
                        };
                        Vector<Note> _notes; // notifications shown
                        uint32_t     _networksItem = 0, _registerItem = 0;
                        uint32_t     _trustBox = 0, _trustNetwork = 0; // the question about a certificate

                        // Networks...: the window, its widgets, the networks as edited.
                        struct NetworkFields {
                                        uint32_t window = 0, list = 0, add = 0, remove = 0, save = 0, cancel = 0,
                                                 status = 0, fromList = 0, perform = 0;
                                        uint32_t performWindow = 0, performText = 0, performOk = 0, performCancel = 0;
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

                        // Add from list... (Networks...: servers.ini, §7c).
                        struct ServerListFields {
                                        uint32_t window = 0, filter = 0, view = 0, add = 0, close = 0, status = 0;
                        };
                        ServerListFields    _sf;
                        Vector<ServerEntry> _servers;
                        Vector<uint32_t>    _serverRows; // the view's rows' entries

                        // List channels... (§7d).
                        struct ListFields {
                                        uint32_t window = 0, text = 0, least = 0, most = 0, get = 0, join = 0,
                                                 close = 0, view = 0, status = 0, network = 0;
                                        bool     receiving = false;
                                        uint32_t dropped = 0; ///< Sent past the most kept.
                                        int      sortColumn = 1;
                                        bool     ascending = false;
                        };
                        static constexpr size_t MostListed = 20000;
                        static constexpr size_t MostShown = 2000;
                        static constexpr size_t TopicBytes = 200;
                        ListFields              _lf;
                        Vector<ListedChannel>   _listed;
                        String                  _listText;
                        Vector<uint32_t>        _listShown; // the view's rows' channels

                        // Channel Central (§7h).
                        struct CentralFields {
                                        uint32_t window = 0, topic = 0, topicBy = 0, flags[6] = {}, key = 0, limit = 0,
                                                 which = 0, view = 0, mask = 0, add = 0, remove = 0, ok = 0, cancel = 0,
                                                 status = 0, network = 0;
                                        String channel;
                                        char   mode = 'b'; ///< The list shown.
                        };
                        struct ModeEntry {
                                        char    mode = 'b';
                                        String  mask, by;
                                        int64_t at = 0;
                        };
                        CentralFields     _cf;
                        Vector<ModeEntry> _modeEntries;

                        // The member list's menu (§7e) and a reason asked for.
                        uint32_t _memberMenuId = 0, _mQuery = 0, _mWhois = 0, _mCopy = 0, _mPing = 0, _mVersion = 0,
                                 _mTime = 0, _mOp = 0, _mDeop = 0, _mVoice = 0, _mDevoice = 0, _mHop = 0, _mDehop = 0,
                                 _mKick = 0, _mKickWhy = 0, _mBan = 0, _mBanKick = 0, _mIgnore = 0, _mNotify = 0;
                        uint32_t _menuNetwork = 0, _menuBuffer = 0;
                        String   _menuNick;
                        struct ReasonFields {
                                        uint32_t window = 0, edit = 0, ok = 0, cancel = 0;
                                        uint8_t  kind = 0; ///< 1 kick, 2 ban and kick
                        };
                        ReasonFields _reason;

                        // The notify list (§7g).
                        struct NotifyFields {
                                        uint32_t window = 0, view = 0, nick = 0, add = 0, remove = 0, close = 0;
                        };
                        struct NotifyRow {
                                        uint32_t network = 0;
                                        String   nick;
                        };
                        NotifyFields      _ntf;
                        Vector<NotifyRow> _notifyRows;

                        // Find (§7j) and a paste of many lines (§7k).
                        uint32_t _findRow = 0, _findEdit = 0, _findPrev = 0, _findNext = 0, _findClose = 0;
                        struct PasteFields {
                                        uint32_t window = 0, label = 0, text = 0, send = 0, cancel = 0, network = 0,
                                                 buffer = 0;
                        };
                        PasteFields _pf;
                        Ignores     _ignores;
                        uint32_t    _listItem = 0, _centralItem = 0, _rejoinItem = 0, _inviteItem = 0, _notifyItem = 0,
                                 _findItem = 0;
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
