/**
 * @file      buffers.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "session.h"
#include "str.h"
#include "text.h"

#include <cstddef>
#include <cstdint>

namespace irc {

        /**
         * @brief A conversation of one network's: its own (notices, the server), a channel, or a
         *        private chat. Its lines are its `ChatView`'s and its log's, not kept here
         *        (devplan/irc.md §13).
         */
        struct Buffer {
                        enum class Type : uint8_t {
                                Network,
                                Channel,
                                Query,
                        };
                        uint32_t       id = 0; ///< Unique in its network's `Buffers`, never reused.
                        Type           type = Type::Network;
                        String         name;           ///< A channel's, a person's; the network's own: "".
                        uint32_t       unread = 0;     ///< Messages since it was last in view.
                        uint32_t       mentions = 0;   ///< Of them, mentions (and every one, in a private chat).
                        uint8_t        attention = 0;  ///< 0 quiet, 1 activity, 2 an alert (a tab's, devplan/tabs.md).
                        bool           joined = false; ///< A channel we are in now.
                        Vector<String> speakers;       ///< A channel's, who spoke last first (Tab's order).
        };

        /** @brief A line for a buffer: what its `ChatView` shows, and its log's text. */
        struct Line {
                        uint32_t buffer = 0;
                        String   view;           ///< `HH:MM marker` gutter, a tab, the text (mIRC's codes kept).
                        String   log;            ///< `HH:MM:SS marker text`, plain.
                        int64_t  time = 0;       ///< ms since 1970, UTC (the log's day from it).
                        uint8_t  level = 0;      ///< 0 an event (a join, a mode), 1 a message, 2 a mention or private.
                        bool     notify = false; ///< A mention or private message not in view: a notification.
        };

        /**
         * @brief One network's buffers (devplan/irc.md §3, §6, §7b): the session's events routed to
         *        them and turned into lines; the counts and attention of those not in view; who
         *        spoke last; a password typed to NickServ hidden.
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class Buffers {
                public:
                        struct Settings {
                                        Vector<String> highlights; ///< Words that count as mentions besides our nick.
                                        bool           showJoins = true;     ///< Joins, parts and quits shown.
                                        int32_t        utcOffsetMinutes = 0; ///< The zone the times are shown in.
                                        /// Passwords hidden (`****`) where a service or the server repeats them
                                        /// (Atheme's "registered ..., with the password secret.").
                                        Vector<String> secrets;
                        };

                        /** @brief Most speakers a channel remembers. */
                        static constexpr size_t MaxSpeakers = 32;

                        Buffers();

                        Settings       &settings() { return _settings; }
                        const Settings &settings() const { return _settings; }

                        /**
                         * @brief Takes in @p event (of @p session): its buffer found or made (a private
                         *        chat opened by a message), its lines added to @p out, the counts kept.
                         * @return `Ok` or `NoMem`.
                         */
                        Error apply(const Event &event, const Session &session, Vector<Line> &out);

                        /** @brief Adds a line of the client's own (`-- text`) to buffer @p id. */
                        Error note(uint32_t id, Text text, int64_t time, Vector<Line> &out);

                        size_t        count() const { return _buffers.size(); }
                        const Buffer &at(size_t i) const { return _buffers[i]; }
                        Buffer       &network() { return _buffers[0]; }
                        Buffer       *byId(uint32_t id);
                        const Buffer *byId(uint32_t id) const;
                        /** @brief Returns the channel's or private chat's named @p name, or null. */
                        Buffer *find(Text name, CaseMapping mapping);

                        /** @brief Returns the buffer named @p name of @p type, made if it isn't there. */
                        Buffer *open(Buffer::Type type, Text name, CaseMapping mapping);

                        /** @brief The connection went: no channel is joined now (the session rejoins them). */
                        void disconnected();

                        /** @brief Forgets buffer @p id (not the network's). */
                        void close(uint32_t id);

                        /**
                         * @brief Buffer @p id is in view (0: none of this network's), the window
                         *        @p focused or not: a buffer in view in a focused window is read.
                         */
                        void     setActive(uint32_t id, bool focused);
                        uint32_t active() const { return _active; }

                private:
                        Buffer *make(Buffer::Type type, Text name);
                        Error add(Buffer &b, const Event &e, uint8_t level, Text marker, Text text, Vector<Line> &out);
                        bool  mentioned(Text text, const Session &s) const;
                        void  spoke(Buffer &b, Text nick, CaseMapping mapping);
                        void  seen(Buffer &b);

                        Settings       _settings;
                        Vector<Buffer> _buffers;
                        uint32_t       _nextId = 1;
                        uint32_t       _active = 0;
                        bool           _focused = false;
        };

} // namespace irc
