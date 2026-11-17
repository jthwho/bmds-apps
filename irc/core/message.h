/**
 * @file      message.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "text.h"

#include <cstddef>
#include <cstdint>

namespace irc {

        /** @brief What the core's functions return (devplan/irc.md §3). */
        enum class Error : uint8_t {
                Ok,
                Malformed, ///< Not a message, or a parameter that can't be sent (a CR, LF, NUL...).
                TooLong,   ///< Past a line's limit.
        };

        /** @brief A message's source, `nick!user@host` (a server's: its name, in `nick`). */
        struct Source {
                        Text nick;
                        Text user;
                        Text host;

                        /** @brief Returns true if it is a server's: no user or host, a dot in the name. */
                        bool isServer() const {
                                return user.isEmpty() && host.isEmpty() && nick.find('.') != Text::NoPos;
                        }
        };

        /** @brief Splits @p source (a message's, without its `:`) into its parts. */
        Source parseSource(Text source);

        /**
         * @brief One message received: views into its line (valid while the line is).
         *
         * @par Thread Safety
         * Reentrant.
         */
        struct Message {
                        /** @brief The most parameters (RFC 1459's 15: the last takes the rest of the line). */
                        static constexpr size_t MaxParams = 15;

                        Text   tags;    ///< IRCv3's, raw (`a=b;c`), without the `@`.
                        Text   source;  ///< Without the `:`; empty for none.
                        Text   command; ///< As sent: a word, or three digits.
                        Text   params[MaxParams];
                        size_t paramCount = 0;

                        /** @brief Returns parameter @p i, or an empty one past the last. */
                        Text param(size_t i) const { return i < paramCount ? params[i] : Text(); }

                        /** @brief Returns the last parameter (a message's text), or an empty one. */
                        Text last() const { return paramCount ? params[paramCount - 1] : Text(); }

                        /** @brief Returns true if the command is @p name (in either case). */
                        bool is(Text name) const { return command.equalsIgnoringCase(name); }

                        /** @brief Returns the numeric reply's number, or -1 for a command. */
                        int numeric() const;

                        /** @brief Returns true if it has tag @p key (with a value or not). */
                        bool hasTag(Text key) const;

                        /** @brief Returns tag @p key's value, escaped as sent (`unescapeTag()`); empty for none. */
                        Text tag(Text key) const;
        };

        /**
         * @brief Parses @p line (without its CR LF): `[@tags] [:source] COMMAND [params] [:last]`,
         *        runs of spaces taken as one.
         * @return `Ok`, or `Malformed` (no command).
         */
        Error parse(Text line, Message &out);

        /**
         * @brief Writes tag value @p raw unescaped (`\:` `;`, `\s` a space, `\\`, `\r`, `\n`; any
         *        other `\x` is `x`, a lone `\` at the end dropped) into @p out.
         * @return The bytes it takes, written or not (@p most at most are written).
         */
        size_t unescapeTag(Text raw, char *out, size_t most);

        /**
         * @brief How much of @p text to send in a line with room for @p most bytes of it: all if it
         *        fits, else up to the last space past half of @p most, else as much as fits; never
         *        part of a UTF-8 character. The rest follows its leading space dropped.
         * @return The bytes for this line: 0 only if @p most is less than the first character.
         */
        size_t splitAt(Text text, size_t most);

        /**
         * @brief One line to send, built a parameter at a time: `COMMAND p1 p2 :last` and its CR LF,
         *        at most 512 bytes. Refuses a command that isn't letters and digits, and a parameter
         *        that would change what is sent (a CR, LF or NUL anywhere; a space, a leading `:` or
         *        nothing in one but the last), so typed text can't add a command of its own.
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class LineBuilder {
                public:
                        /** @brief The most bytes a line takes, its CR LF too. */
                        static constexpr size_t MaxLine = 512;

                        /** @brief Starts a line of @p command. */
                        explicit LineBuilder(Text command);

                        /** @brief Adds a parameter that isn't the last. */
                        LineBuilder &arg(Text param);

                        /** @brief Adds the last parameter (it may hold spaces; written after a `:`). */
                        LineBuilder &last(Text param);

                        /** @brief Returns `Ok`, or the first parameter's error (the line is then not to be sent). */
                        Error error() const { return _error; }

                        /** @brief Returns the line, its CR LF ended (empty after an error). */
                        Text line();

                private:
                        bool append(Text bytes);

                        char   _buf[MaxLine];
                        size_t _size = 0;
                        Error  _error = Error::Ok;
                        bool   _ended = false; ///< The last parameter added: no more.
        };

        /**
         * @brief Bytes received cut into lines (at LF, a CR before it dropped; empty lines
         *        skipped). A line longer than IRCv3's limit (8191 bytes of tags and 512 of the
         *        rest) is dropped whole and counted.
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class LineReader {
                public:
                        /** @brief The longest line kept, without its CR LF. */
                        static constexpr size_t MaxLine = 8191 + 512;

                        /**
                         * @brief Reads @p bytes, calling @p onLine with each line completed (a `Text`
                         *        valid during the call).
                         */
                        template <typename F> void feed(Text bytes, F &&onLine) {
                                size_t used = 0;
                                while (used < bytes.size()) {
                                        used += write(bytes.mid(used));
                                        Text line;
                                        while (take(line)) onLine(line);
                                }
                                return;
                        }

                        /** @brief Returns the lines dropped as too long. */
                        size_t dropped() const { return _dropped; }

                        /** @brief Forgets a part line (a new connection). */
                        void reset() {
                                _start = _end = _scanned = 0;
                                _skipping = false;
                                return;
                        }

                private:
                        size_t write(Text bytes);
                        bool   take(Text &line);

                        char   _buf[MaxLine + 2];
                        size_t _start = 0;   ///< The first byte not yet a line's.
                        size_t _end = 0;     ///< Past the last byte written.
                        size_t _scanned = 0; ///< Bytes from `_start` known to hold no LF.
                        size_t _dropped = 0;
                        bool   _skipping = false; ///< In a line too long: dropped to its LF.
        };

} // namespace irc
