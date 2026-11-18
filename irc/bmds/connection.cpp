/**
 * @file      connection.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "connection.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <new>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <tls.h>
#include <unistd.h>

namespace irc {

        // The lookup's job: the thread and the connection each hold a reference; the last frees it.
        // The thread owns the pipe's write end (closed when it is done), so a connection that went
        // away meanwhile only makes its write fail (SIGPIPE is ignored by the program).
        struct Job {
                        int         refs = 2;
                        char        host[256] = {};
                        char        port[8] = {};
                        int         pipeWrite = -1;
                        int         error = 0;
                        ::addrinfo *result = nullptr;
        };

        namespace {

                void release(Job *job) {
                        if (__atomic_sub_fetch(&job->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
                        if (job->result != nullptr) freeaddrinfo(job->result);
                        free(job);
                        return;
                }

                void *lookup(void *arg) {
                        auto      *job = static_cast<Job *>(arg);
                        ::addrinfo hints = {};
                        hints.ai_family = AF_UNSPEC;
                        hints.ai_socktype = SOCK_STREAM;
                        job->error = getaddrinfo(job->host, job->port, &hints, &job->result);
                        const char done = 1;
                        (void)!write(job->pipeWrite, &done, 1);
                        close(job->pipeWrite);
                        release(job);
                        return nullptr;
                }

                // The list of addresses being tried (the job's result, taken).
                ::addrinfo *&listOf(Job *job) {
                        return job->result;
                }

        } // namespace

        Connection::~Connection() {
                stop();
        }

        Connection::Result Connection::fail(const char *what, const char *detail) {
                (void)_why.assign(what);
                if (detail != nullptr && detail[0] != '\0') {
                        (void)_why.append(": ");
                        (void)_why.append(detail);
                }
                String why = std::move(_why);
                stop();
                _why = std::move(why);
                return Result::Failed;
        }

        bool Connection::start(Text host, uint16_t port, bool tls, Text ca, Text cert, Text key, int64_t mono) {
                stop();
                _why.clear();
                _security.clear();
                _fingerprint.clear();
                _out.clear();
                _wantTls = tls;
                (void)_host.assign(host);
                if (host.isEmpty() || host.size() >= sizeof(Job::host)) {
                        (void)_why.assign("no host to connect to");
                        return false;
                }
                // TLS's settings now, so a bad file is told before anything is sent.
                if (tls) {
                        _config = tls_config_new();
                        String path;
                        if (_config == nullptr) {
                                (void)_why.assign("no memory for TLS");
                                return false;
                        }
                        if (!ca.isEmpty() && (path.assign(ca) != Error::Ok || path.append('\0') != Error::Ok ||
                                              tls_config_set_ca_file(_config, path.data()) != 0)) {
                                (void)fail("the network's ca", tls_config_error(_config));
                                return false;
                        }
                        if (!cert.isEmpty()) {
                                String k;
                                if (path.assign(cert) != Error::Ok || path.append('\0') != Error::Ok ||
                                    k.assign(key) != Error::Ok || k.append('\0') != Error::Ok ||
                                    tls_config_set_keypair_file(_config, path.data(), k.data()) != 0) {
                                        (void)fail("the client certificate", tls_config_error(_config));
                                        return false;
                                }
                        }
                }
                Job *job = static_cast<Job *>(calloc(1, sizeof(Job)));
                if (job == nullptr || pipe(_pipe) != 0) {
                        free(job);
                        (void)_why.assign("no memory to connect");
                        return false;
                }
                ::new (job) Job();
                memcpy(job->host, host.data(), host.size());
                snprintf(job->port, sizeof(job->port), "%u", unsigned(port));
                job->pipeWrite = _pipe[1];
                _pipe[1] = -1; // the thread's now
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setstacksize(&attr, 24 * 1024);
                pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                pthread_t  thread;
                const bool ok = pthread_create(&thread, &attr, lookup, job) == 0;
                pthread_attr_destroy(&attr);
                if (!ok) {
                        close(job->pipeWrite);
                        free(job);
                        close(_pipe[0]);
                        _pipe[0] = -1;
                        (void)_why.assign("couldn't start the lookup");
                        return false;
                }
                _job = job;
                _state = State::Resolving;
                _deadline = mono + ConnectTimeoutMs;
                return true;
        }

        void Connection::stop() {
                if (_tls != nullptr) {
                        if (_state == State::Up) (void)tls_close(_tls);
                        tls_free(_tls);
                        _tls = nullptr;
                }
                if (_config != nullptr) {
                        tls_config_free(_config);
                        _config = nullptr;
                }
                if (_fd >= 0) close(_fd);
                _fd = -1;
                if (_pipe[0] >= 0) close(_pipe[0]);
                _pipe[0] = -1;
                if (_job != nullptr) release(_job);
                _job = nullptr;
                _next = nullptr;
                _state = State::Idle;
                _wantWrite = false;
                _deadline = -1;
                return;
        }

        int Connection::fd() const {
                switch (_state) {
                        case State::Resolving: return _pipe[0];
                        case State::Connecting:
                        case State::Handshaking:
                        case State::Up: return _fd;
                        case State::Idle: break;
                }
                return -1;
        }

        short Connection::events() const {
                switch (_state) {
                        case State::Resolving: return POLLIN;
                        case State::Connecting: return POLLOUT;
                        case State::Handshaking: return _wantWrite ? POLLOUT : POLLIN;
                        case State::Up: return short(POLLIN | (!_out.isEmpty() || _wantWrite ? POLLOUT : 0));
                        case State::Idle: break;
                }
                return 0;
        }

        Connection::Result Connection::tryNext(int64_t mono) {
                int last = 0;
                while (_next != nullptr) {
                        ::addrinfo *ai = _next;
                        _next = ai->ai_next;
                        if (_fd >= 0) close(_fd);
                        _fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                        if (_fd < 0) {
                                last = errno;
                                continue;
                        }
                        (void)fcntl(_fd, F_SETFL, fcntl(_fd, F_GETFL) | O_NONBLOCK);
                        if (connect(_fd, ai->ai_addr, ai->ai_addrlen) == 0) {
                                return _wantTls ? startTls(mono)
                                                : (_state = State::Up, _deadline = -1, Result::Connected);
                        }
                        if (errno == EINPROGRESS) {
                                _state = State::Connecting;
                                _deadline = mono + ConnectTimeoutMs;
                                return Result::Nothing;
                        }
                        last = errno;
                }
                return fail("couldn't connect", last != 0 ? strerror(last) : "no address answered");
        }

        Connection::Result Connection::startTls(int64_t mono) {
                (void)_security.assign("no TLS");
                _tls = tls_client();
                String host;
                if (_tls == nullptr || host.assign(_host) != Error::Ok || host.append('\0') != Error::Ok)
                        return fail("no memory for TLS");
                if (tls_configure(_tls, _config) != 0) return fail("TLS", tls_error(_tls));
                if (tls_connect_socket(_tls, _fd, host.data()) != 0) return fail("TLS", tls_error(_tls));
                _state = State::Handshaking;
                _deadline = mono + ConnectTimeoutMs;
                _wantWrite = true;
                return Result::Nothing;
        }

        Connection::Result Connection::flush() {
                while (!_out.isEmpty()) {
                        const ssize_t n = _tls != nullptr ? tls_write(_tls, _out.data(), _out.size())
                                                          : write(_fd, _out.data(), _out.size());
                        if (n == TLS_WANT_POLLOUT || n == TLS_WANT_POLLIN) {
                                _wantWrite = n == TLS_WANT_POLLOUT;
                                return Result::Nothing;
                        }
                        if (n < 0) {
                                if (_tls == nullptr && (errno == EAGAIN || errno == EWOULDBLOCK))
                                        return Result::Nothing;
                                return fail("the connection broke",
                                            _tls != nullptr ? tls_error(_tls) : strerror(errno));
                        }
                        (void)_out.assign(_out.view().mid(size_t(n)));
                }
                _wantWrite = false;
                return Result::Nothing;
        }

        bool Connection::send(Text bytes) {
                return _out.append(bytes) == Error::Ok;
        }

        Connection::Result Connection::service(short revents, int64_t mono, char *buf, size_t size, size_t &got) {
                got = 0;
                // Taking too long to connect: the next address, or given up.
                if (_deadline >= 0 && mono >= _deadline && revents == 0) {
                        if (_state == State::Connecting) return tryNext(mono);
                        return fail(_state == State::Resolving ? "the name lookup took too long"
                                                               : "the TLS handshake took too long");
                }
                switch (_state) {
                        case State::Idle: return Result::Nothing;

                        case State::Resolving: {
                                if ((revents & (POLLIN | POLLHUP)) == 0) return Result::Nothing;
                                char done = 0;
                                (void)!read(_pipe[0], &done, 1);
                                close(_pipe[0]);
                                _pipe[0] = -1;
                                if (_job->error != 0) {
                                        const char *why = gai_strerror(_job->error);
                                        return fail("couldn't find the server", why);
                                }
                                _next = listOf(_job);
                                return tryNext(mono);
                        }

                        case State::Connecting: {
                                if ((revents & (POLLOUT | POLLERR | POLLHUP)) == 0) return Result::Nothing;
                                int       error = 0;
                                socklen_t len = sizeof(error);
                                if (getsockopt(_fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0) error = errno;
                                if (error != 0) {
                                        if (_next == nullptr) return fail("couldn't connect", strerror(error));
                                        return tryNext(mono);
                                }
                                if (_wantTls) return startTls(mono);
                                _state = State::Up;
                                _deadline = -1;
                                (void)_security.assign("no TLS");
                                return Result::Connected;
                        }

                        case State::Handshaking: {
                                if (revents == 0) return Result::Nothing;
                                const int r = tls_handshake(_tls);
                                if (r == TLS_WANT_POLLIN || r == TLS_WANT_POLLOUT) {
                                        _wantWrite = r == TLS_WANT_POLLOUT;
                                        return Result::Nothing;
                                }
                                if (r != 0) return fail("TLS", tls_error(_tls));
                                _state = State::Up;
                                _deadline = -1;
                                _wantWrite = false;
                                const char *version = tls_conn_version(_tls);
                                const char *cipher = tls_conn_cipher(_tls);
                                const char *hash = tls_peer_cert_hash(_tls);
                                (void)_security.assign(version != nullptr ? version : "TLS");
                                (void)_cipher.assign(cipher != nullptr ? cipher : "");
                                if (hash != nullptr) (void)_fingerprint.assign(hash);
                                return Result::Connected;
                        }

                        case State::Up: {
                                if ((revents & POLLOUT) != 0 || _wantWrite) {
                                        const Result r = flush();
                                        if (r != Result::Nothing) return r;
                                }
                                if ((revents & (POLLIN | POLLHUP | POLLERR)) == 0) return Result::Nothing;
                                const ssize_t n = _tls != nullptr ? tls_read(_tls, buf, size) : read(_fd, buf, size);
                                if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) {
                                        if (n == TLS_WANT_POLLOUT) _wantWrite = true;
                                        return Result::Nothing;
                                }
                                if (n == 0) {
                                        stop();
                                        (void)_why.assign("the server closed the connection");
                                        return Result::Closed;
                                }
                                if (n < 0) {
                                        if (_tls == nullptr && (errno == EAGAIN || errno == EWOULDBLOCK))
                                                return Result::Nothing;
                                        return fail("the connection broke",
                                                    _tls != nullptr ? tls_error(_tls) : strerror(errno));
                                }
                                got = size_t(n);
                                return Result::Data;
                        }
                }
                return Result::Nothing;
        }

} // namespace irc
