/**
 * @file      session.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "isupport.h"
#include "message.h"
#include "str.h"
#include "text.h"

#include <cstddef>
#include <cstdint>

namespace irc {

        /** @brief The time, twice: a monotonic clock for timers, and the wall's for what is shown. */
        struct Now {
                        int64_t mono = 0; ///< ms, never stepping back.
                        int64_t wall = 0; ///< ms since 1970 (UTC).
        };

        /** @brief How a network is logged in to (devplan/irc.md §7a). */
        enum class Login : uint8_t {
                None,
                Sasl,         ///< SASL PLAIN while registering (NickServ's IDENTIFY if it fails).
                SaslExternal, ///< SASL EXTERNAL: the TLS client certificate is the login.
                NickServ,     ///< `IDENTIFY` once registered.
                Password,     ///< `PASS` before registering (a bouncer, a private server).
        };

        /** @brief One network's settings, as the session needs them. */
        struct Config {
                        String         nick;
                        Vector<String> altNicks; ///< Tried in order when the nick is taken.
                        String         user;     ///< Empty: the nick.
                        String         realName; ///< Empty: the nick.
                        Login          login = Login::None;
                        String         account;  ///< SASL's and IDENTIFY's account; empty: the nick.
                        String         password; ///< SASL PLAIN's, NickServ's, or the server's.
                        Vector<String> autojoin; ///< Channels joined once registered (and logged in).
                        String         version;  ///< CTCP VERSION's answer.
        };

        /** @brief A member of a channel: a nick and its prefixes (`@+`, highest first). */
        struct Member {
                        String nick;
                        char   prefixes[8] = {};
                        bool   away = false;
        };

        /** @brief A channel the session is in (or was in, when the connection went: rejoined). */
        struct Channel {
                        String         name;
                        String         topic;
                        Vector<Member> members;
                        bool           joined = false;
                        bool           namesDone = true; ///< The last NAMES ended (the next 353 starts afresh).
        };

        /** @brief Something that happened, for the buffers and the window (devplan/irc.md §3). */
        struct Event {
                        enum class Kind : uint8_t {
                                Registered, ///< text: our nick.
                                Message,    ///< A PRIVMSG: target, nick, text (Action: a /me).
                                Notice,     ///< A NOTICE: target ("" if to us), nick, text.
                                Join,       ///< target, nick; text the real name, extra the account (extended-join).
                                Part,       ///< target, nick, text the reason.
                                Kick,       ///< target, nick kicked, extra by whom, text the reason.
                                Quit,       ///< target (each channel they were in, then their nick: Query), nick, text.
                                Nick,  ///< target (each channel, then the old nick: Query), nick the old, text the new.
                                Topic, ///< target, nick (empty: as it was on joining), text.
                                Members,     ///< target: its member list changed (names, modes, away).
                                Mode,        ///< target, nick, text the modes and parameters.
                                ServerText,  ///< text: a numeric's or the server's (to the network's buffer).
                                LoggedIn,    ///< text: the account.
                                LoginFailed, ///< text: why.
                                Error,       ///< text: the server's ERROR (the connection ends).
                                Timeout,     ///< Nothing heard for too long: the connection is to be dropped.
                                Invite,      ///< nick invited us to text.
                                Lag,         ///< number: the ms a PING took.
                        };
                        enum Flag : uint8_t {
                                Self = 1,    ///< We did it (our own message, join, nick change).
                                Action = 2,  ///< A CTCP ACTION (/me).
                                Server = 4,  ///< From the server.
                                Service = 8, ///< From a service (NickServ, ChanServ...).
                                Query = 16,  ///< target is a person (a private chat), not a channel.
                        };

                        Kind    kind = Kind::ServerText;
                        uint8_t flags = 0;
                        String  target;
                        String  nick;
                        String  text;
                        String  extra;
                        int64_t time = 0;   ///< When (ms since 1970): the server's (server-time) or ours.
                        int64_t number = 0; ///< Lag's.
        };

        /**
         * @brief One network's protocol (devplan/irc.md §3, §7): registration with CAP and SASL, the
         *        nick taken and retaken, ISUPPORT, PING, the channels and their members, messages
         *        and CTCP; what the user does turned into lines. No sockets: lines come in through
         *        `received()`, lines to send go out through `takeLine()` (paced: 5 at once, then one
         *        a second), and what happened through `takeEvent()`.
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class Session {
                public:
                        enum class State : uint8_t {
                                Disconnected,
                                Registering,
                                Registered,
                        };

                        /** @brief Quiet this long: we PING. */
                        static constexpr int64_t PingAfterMs = 90000;
                        /** @brief Quiet this long: given up (`Timeout`). */
                        static constexpr int64_t TimeoutMs = PingAfterMs + 120000;
                        /** @brief Lines at once before pacing; then one each `PaceMs`. */
                        static constexpr int64_t Burst = 5;
                        static constexpr int64_t PaceMs = 1000;
                        /** @brief How long the joins wait for NickServ after an IDENTIFY. */
                        static constexpr int64_t IdentifyWaitMs = 10000;
                        /** @brief How long after a GHOST before the nick is taken again. */
                        static constexpr int64_t RegainWaitMs = 2000;

                        explicit Session(Config &&config) : _config(std::move(config)) {}

                        const Config &config() const { return _config; }

                        /** @brief Takes new settings: the nick, login, channels... from the next connection on. */
                        void setConfig(Config &&config) { _config = std::move(config); }

                        /** @brief A connection is up: registration starts. */
                        void connected(const Now &now);

                        /** @brief A line came (without its CR LF). */
                        void received(Text line, const Now &now);

                        /** @brief Time passed: PINGs, the joins' wait, giving up. Call by `tickWaitMs()`. */
                        void tick(const Now &now);

                        /**
                         * @brief The connection went: lines not sent are dropped but what the user
                         *        typed, the channels are kept for joining again.
                         */
                        void disconnected();

                        /** @brief Takes the next line to send if pacing allows it (CR LF ended). */
                        bool takeLine(int64_t mono, String &line);

                        /** @brief Returns how long until `takeLine()` has one (0: now; -1: nothing to send). */
                        int64_t sendWaitMs(int64_t mono) const;

                        /** @brief Returns how long until `tick()` has something to do (-1: nothing). */
                        int64_t tickWaitMs(int64_t mono) const;

                        /** @brief Takes the next event. @return False if there is none. */
                        bool takeEvent(Event &out);

                        State           state() const { return _state; }
                        Text            nick() const { return _nick.view(); }
                        Text            account() const { return _account.view(); }
                        const Isupport &isupport() const { return _isupport; }
                        bool            hasCap(Text cap) const;
                        int64_t         lagMs() const { return _lagMs; }

                        size_t         channelCount() const { return _channels.size(); }
                        const Channel &channelAt(size_t i) const { return _channels[i]; }
                        /** @brief Returns the channel named @p name (casemapped), or null. */
                        const Channel *channel(Text name) const;

                        /** @name What the user does (`Malformed`: a CR, LF or NUL in it, or no target) */
                        ///@{
                        /** @brief A message, split at words if it is long; shown at once unless the server echoes. */
                        Error privmsg(Text target, Text text, const Now &now);
                        /** @brief A /me. */
                        Error action(Text target, Text text, const Now &now);
                        Error notice(Text target, Text text, const Now &now);
                        Error join(Text channel, Text key = Text());
                        Error part(Text channel, Text reason = Text());
                        Error changeNick(Text nick);
                        Error setTopic(Text channel, Text topic);
                        /** @brief Away with @p reason, or back (empty). */
                        Error setAway(Text reason);
                        Error quit(Text reason);
                        /** @brief A line as typed (`/raw`, `/quote`): checked only for line breaks. */
                        Error raw(Text line);
                        ///@}

                private:
                        enum class Kind : uint8_t {
                                Priority, ///< Registration, PONG: first and unpaced.
                                Protocol, ///< Joins, IDENTIFY: paced, dropped with the connection.
                                User,     ///< What the user did: paced, kept for the next connection.
                        };
                        struct Outgoing {
                                        String line;
                                        Kind   kind = Kind::Protocol;
                        };

                        void     send(Kind kind, Text command, const Text *params, size_t count, bool lastIsText);
                        void     sendLine(Kind kind, Text line); // a line built already, CR LF ended
                        void     emit(Event &&event);
                        Event    make(Event::Kind kind, uint8_t flags, Text target, Text nick, Text text, int64_t time);
                        bool     isMe(Text nick) const { return sameName(nick, _nick, _isupport.caseMapping()); }
                        Channel *findChannel(Text name);
                        Member  *findMember(Channel &channel, Text nick);
                        void     addMember(Channel &channel, Text nickWithPrefixes);
                        void     setPrefix(Member &member, char prefix, bool on);
                        int64_t  timeOf(const Message &message, const Now &now) const;

                        void  onCap(const Message &m);
                        void  onAuthenticate(const Message &m);
                        void  onWelcome(const Message &m, const Now &now);
                        void  onNickTaken(const Message &m, const Now &now);
                        void  onMessage(const Message &m, bool notice, const Now &now);
                        void  onJoin(const Message &m, const Now &now);
                        void  onMode(const Message &m, const Now &now);
                        void  onNumeric(const Message &m, int numeric, const Now &now);
                        void  endCap();
                        void  joinAll();
                        void  identify();
                        Error say(Text command, Text target, Text text, bool action, const Now &now);

                        Config           _config;
                        State            _state = State::Disconnected;
                        String           _nick;
                        String           _account;
                        String           _selfUser, _selfHost; // as the server shows us (a split's room)
                        Isupport         _isupport;
                        Vector<String>   _caps;    // enabled
                        Vector<String>   _offered; // CAP LS so far
                        bool             _capEnded = false;
                        bool             _saslFailed = false;
                        bool             _identifying = false; // IDENTIFY sent: the joins wait
                        bool             _joinsDone = false;
                        int64_t          _joinAt = -1;   // the joins at the latest (IDENTIFY's wait)
                        int64_t          _regainAt = -1; // NICK back to ours (after a GHOST)
                        size_t           _nickTry = 0;   // the nicks tried while registering
                        Vector<Channel>  _channels;
                        Vector<Outgoing> _out;
                        Vector<Event>    _events;
                        size_t           _eventHead = 0;
                        int64_t          _credit = Burst * PaceMs; // the pacing's
                        int64_t          _creditAt = 0;
                        int64_t          _lastReceived = 0;
                        int64_t          _pingSent = -1;
                        int64_t          _lagMs = -1;
                        bool             _timedOut = false;
        };

        /**
         * @brief How long to wait before connecting again after @p attempt failures in a row
         *        (devplan/irc.md §7): 2, 4, 8 ... 300 s, give or take a fifth (@p random: any number).
         */
        int64_t reconnectDelayMs(unsigned attempt, uint32_t random);

} // namespace irc
