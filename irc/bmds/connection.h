/**
 * @file      connection.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "str.h"

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

struct tls;
struct tls_config;
struct addrinfo;

namespace irc {

        /**
         * @brief One network's socket (devplan/irc.md §3, the platform's): the name looked up on
         *        a thread of its own (getaddrinfo blocks), each address tried with a non-blocking
         *        connect, then TLS through libtls (the kernel's TLS sockets: its handshake on the
         *        kernel's thread). Everything else waits in the program's one `poll()`: `events()`
         *        says what to wait for, `service()` does what came.
         *
         * @par Thread Safety
         * Not thread-safe (the program's loop); the lookup's thread shares only its job.
         */
        class Connection {
                public:
                        enum class State : uint8_t {
                                Idle,
                                Resolving,
                                Connecting,
                                Handshaking,
                                Up,
                        };

                        /** @brief What a `service()` brought. */
                        enum class Result : uint8_t {
                                Nothing,
                                Connected, ///< Up (TLS's handshake done): registration can start.
                                Data,      ///< Bytes came (in the caller's buffer).
                                Closed,    ///< The server closed it.
                                Failed,    ///< It couldn't connect, or broke: `why()` says.
                        };

                        /** @brief How long a connect or handshake may take. */
                        static constexpr int64_t ConnectTimeoutMs = 30000;

                        Connection() = default;
                        ~Connection();
                        Connection(const Connection &) = delete;
                        Connection &operator=(const Connection &) = delete;

                        /**
                         * @brief Starts connecting to @p host:@p port, with TLS if @p tls: checked
                         *        against the system's roots and @p ca's (none: empty), presenting
                         *        @p cert and @p key if given (SASL EXTERNAL).
                         * @return False if it couldn't start (`why()`).
                         */
                        bool start(Text host, uint16_t port, bool tls, Text ca, Text cert, Text key, int64_t mono);

                        /** @brief Closes it (TLS's close_notify sent if up), back to `Idle`. */
                        void stop();

                        State state() const { return _state; }
                        Text  why() const { return _why; }

                        /** @brief Returns "TLSv1.2" once up with TLS, or "no TLS". */
                        Text security() const { return _security; }

                        /** @brief Returns the TLS cipher suite, or empty. */
                        Text cipher() const { return _cipher; }

                        /** @brief Returns the server certificate's SHA-256 ("SHA256:hex"), or empty. */
                        Text fingerprint() const { return _fingerprint; }

                        /** @brief Its fd and the events to wait for (-1: none now). */
                        int   fd() const;
                        short events() const;

                        /** @brief Returns when the connect or handshake gives up (-1: none). */
                        int64_t deadline() const { return _deadline; }

                        /**
                         * @brief Does what @p revents (its fd's, or 0 on a timeout) allows: the next
                         *        step of connecting, bytes read into @p buf (@p size: @p got), the
                         *        queued bytes written.
                         */
                        Result service(short revents, int64_t mono, char *buf, size_t size, size_t &got);

                        /** @brief Queues @p bytes to send (sent as the socket takes them). */
                        bool send(Text bytes);

                private:
                        Result fail(const char *what, const char *detail = nullptr);
                        Result tryNext(int64_t mono); // the next address's connect
                        Result startTls(int64_t mono);
                        Result flush();

                        State              _state = State::Idle;
                        int                _fd = -1;
                        int                _pipe[2] = {-1, -1}; // the lookup's: a byte when done
                        struct Job        *_job = nullptr;      // shared with the lookup's thread
                        ::addrinfo        *_next = nullptr;     // the address to try next
                        struct tls        *_tls = nullptr;
                        struct tls_config *_config = nullptr;
                        bool               _wantTls = false;
                        bool               _wantWrite = false; // TLS asked for POLLOUT
                        String             _host;
                        String             _why;
                        String             _security;
                        String             _cipher;
                        String             _fingerprint;
                        String             _out; // queued to send
                        int64_t            _deadline = -1;
        };

} // namespace irc
