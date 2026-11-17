/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * vncserver (devplan/vnc.md §5e): starts the server in a session of its own and returns once it
 * listens (from autoexec.sh, or over SSH: the login can end), or stops it, or says how it is.
 *
 *     vncserver            start, in the background
 *     vncserver -f         start, in the foreground (what the background one runs)
 *     vncserver stop       stop the running one
 *     vncserver status     say whether one runs, and where it listens
 *     vncserver passwd     set the password viewers give (read from the terminal)
 */

#include "server.h"

#include <bmds/sys.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

        // The running server's process id (BMDS's /tmp: RAM, gone at a reset, as a pid should be).
        constexpr const char *PidFile = "/tmp/vncserver.pid";

        // The settings, declared by the program (devplan/vnc.md §5d).
        struct Param {
                        const char *key;
                        uint32_t    type;
                        const char *def;
                        const char *help;
                        const char *choices;
                        int32_t     min, max;
        };
        constexpr Param Params[] = {
                {"vnc/port", BMDS_SYSCONF_TYPE_INT, "5900", "The VNC server's TCP port", nullptr, 1, 65535},
                {"vnc/listen", BMDS_SYSCONF_TYPE_ENUM, "loopback",
                 "Where the VNC server listens: loopback (reach it through an SSH tunnel) or lan (every "
                 "interface, only with a password; not encrypted: the password can be guessed from the traffic)",
                 "loopback|lan", 0, 0},
                {"vnc/jpeg-quality", BMDS_SYSCONF_TYPE_INT, "80", "The VNC server's JPEG quality at most (1-100)",
                 nullptr, 1, 100},
                {"vnc/start-wait", BMDS_SYSCONF_TYPE_INT, "30",
                 "Seconds the VNC server waits for the desktop when started early (autoexec.sh)", nullptr, 0, 600},
        };

        // A declared setting's value into @p out (its default if it can't be read).
        void setting(const Param &p, char *out, size_t size) {
                bmds_sysconf_param_t d = {};
                d.size = sizeof(d);
                d.type = p.type;
                d.key = p.key;
                d.default_value = p.def;
                d.help = p.help;
                d.choices = p.choices;
                d.min = p.min;
                d.max = p.max;
                int32_t id = bmds_sysconf_declare(&d);
                if (id < 0) id = bmds_sysconf_find(p.key);
                if (id < 0 || bmds_sysconf_get(id, BMDS_SYSCONF_FIELD_VALUE, out, uint32_t(size)) < 0)
                        snprintf(out, size, "%s", p.def);
                return;
        }

        // The password file: the user's config folder (the VNC challenge needs the password itself,
        // so it can't be a system secret, which reads back as "(set)").
        void passwordPath(char *out, size_t size) {
                const char *home = getenv("HOME");
                snprintf(out, size, "%s/.config/vncserver/password", home != nullptr ? home : "/home");
                return;
        }

        bool readPassword(char *out, size_t size) {
                char path[256];
                passwordPath(path, sizeof(path));
                FILE *f = fopen(path, "r");
                if (f == nullptr) return false;
                const bool ok = fgets(out, int(size), f) != nullptr;
                fclose(f);
                if (!ok) return false;
                out[strcspn(out, "\r\n")] = '\0';
                return out[0] != '\0';
        }

        int setPassword() {
                char path[256], line[64];
                passwordPath(path, sizeof(path));
                printf("Password for VNC viewers (8 characters count; empty for none): ");
                fflush(stdout);
                if (fgets(line, sizeof(line), stdin) == nullptr) return 1;
                line[strcspn(line, "\r\n")] = '\0';
                // The folders, then the file, readable by its owner only.
                char dir[256];
                snprintf(dir, sizeof(dir), "%s", path);
                for (char *p = dir + 1; *p != '\0'; ++p) {
                        if (*p != '/') continue;
                        *p = '\0';
                        (void)mkdir(dir, 0700);
                        *p = '/';
                }
                if (line[0] == '\0') {
                        (void)unlink(path);
                        printf("vncserver: no password: viewers on the board's loopback get in without one\n");
                        return 0;
                }
                const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (fd < 0 || write(fd, line, strlen(line)) < 0 || write(fd, "\n", 1) != 1) {
                        perror(path);
                        if (fd >= 0) close(fd);
                        return 1;
                }
                close(fd);
                if (strlen(line) > 8) printf("vncserver: VNC uses the first 8 characters only\n");
                printf("vncserver: the new password counts from the next start\n");
                return 0;
        }

        pid_t runningPid() {
                FILE *f = fopen(PidFile, "r");
                if (f == nullptr) return -1;
                long pid = -1;
                if (fscanf(f, "%ld", &pid) != 1) pid = -1;
                fclose(f);
                if (pid <= 0 || kill(pid_t(pid), 0) != 0) return -1; // gone: the file is stale
                return pid_t(pid);
        }

        int stop() {
                const pid_t pid = runningPid();
                if (pid < 0) {
                        printf("vncserver: not running\n");
                        return 1;
                }
                if (kill(pid, SIGTERM) != 0) {
                        perror("vncserver: stop");
                        return 1;
                }
                for (int i = 0; i < 50 && kill(pid, 0) == 0; ++i) usleep(100 * 1000);
                printf("vncserver: stopped\n");
                return 0;
        }

        int status() {
                const pid_t pid = runningPid();
                if (pid < 0) {
                        printf("vncserver: not running\n");
                        return 1;
                }
                // Where it really listens: what it wrote at its start (the settings may have changed since).
                char  where[80] = "an address it didn't say";
                FILE *f = fopen(PidFile, "r");
                if (f != nullptr) {
                        char line[80];
                        if (fgets(line, sizeof(line), f) != nullptr && fgets(line, sizeof(line), f) != nullptr) {
                                line[strcspn(line, "\r\n")] = '\0';
                                snprintf(where, sizeof(where), "%s", line);
                        }
                        fclose(f);
                }
                printf("vncserver: running (pid %ld), listening on %s\n", long(pid), where);
                return 0;
        }

        // Started from a terminal: the server again, in a session of its own (a login ending doesn't
        // hang it up), told to say on a pipe when it listens; this one prints that and returns.
        int startInBackground(const char *self) {
                int pipeFds[2];
                if (pipe(pipeFds) != 0) {
                        perror("vncserver");
                        return 1;
                }
                char readyArg[16];
                snprintf(readyArg, sizeof(readyArg), "%d", pipeFds[1]);
                const pid_t pid = vfork();
                if (pid == 0) {
                        setsid();
                        char *const argv[] = {const_cast<char *>(self), const_cast<char *>("-f"),
                                              const_cast<char *>("--ready"), readyArg, nullptr};
                        execvp(self, argv);
                        _exit(127);
                }
                close(pipeFds[1]);
                if (pid < 0) {
                        perror("vncserver: start");
                        close(pipeFds[0]);
                        return 1;
                }
                char    line[192] = {};
                size_t  got = 0;
                ssize_t n;
                while (got < sizeof(line) - 1 && (n = read(pipeFds[0], line + got, sizeof(line) - 1 - got)) > 0)
                        got += size_t(n);
                close(pipeFds[0]);
                if (got < 2) {
                        fprintf(stderr, "vncserver: it ended before it was listening\n");
                        return 1;
                }
                fprintf(line[0] == '0' ? stdout : stderr, "vncserver: %s", line + 2);
                return line[0] == '0' ? 0 : 1;
        }

} // namespace

int main(int argc, char **argv) {
        bool foreground = false, verbose = false;
        int  ready = -1;
        for (int i = 1; i < argc; ++i) {
                if (strcmp(argv[i], "-f") == 0) {
                        foreground = true;
                } else if (strcmp(argv[i], "-v") == 0) {
                        verbose = true;
                } else if (strcmp(argv[i], "--ready") == 0 && i + 1 < argc) {
                        ready = atoi(argv[++i]);
                } else if (strcmp(argv[i], "stop") == 0) {
                        return stop();
                } else if (strcmp(argv[i], "status") == 0) {
                        return status();
                } else if (strcmp(argv[i], "passwd") == 0) {
                        return setPassword();
                } else {
                        fprintf(stderr, "usage: vncserver [-f [-v]] | stop | status | passwd\n");
                        return 2;
                }
        }
        if (runningPid() >= 0) {
                fprintf(stderr, "vncserver: running already ('vncserver status')\n");
                return 1;
        }
        if (!foreground) return startInBackground(argv[0]);

        vnc::Settings s;
        char          value[32];
        setting(Params[0], value, sizeof(value));
        s.port = uint16_t(atoi(value));
        setting(Params[1], value, sizeof(value));
        s.lan = strcmp(value, "lan") == 0;
        setting(Params[2], value, sizeof(value));
        s.jpegCeiling = atoi(value);
        setting(Params[3], value, sizeof(value));
        s.startWaitS = atoi(value);
        (void)readPassword(s.password, sizeof(s.password));
        s.verbose = verbose;
        if (s.lan && s.password[0] == '\0') {
                // The network only with a password (devplan/vnc.md A5, as changed 2026-11-16): not
                // encrypted even then, so no one on the network gets in for nothing.
                const char *why = "vnc/listen is lan: set a password first ('vncserver passwd')";
                if (ready >= 0) {
                        char      line[160];
                        const int n = snprintf(line, sizeof(line), "1 %s\n", why);
                        (void)write(ready, line, size_t(n));
                        close(ready);
                } else {
                        fprintf(stderr, "vncserver: %s\n", why);
                }
                return 1;
        }

        vnc::Server server(s);
        if (server.start(ready) != 0) return 1;
        FILE *pid = fopen(PidFile, "w");
        if (pid != nullptr) {
                fprintf(pid, "%ld\n%s\n", long(getpid()), server.address()); // the second line: status's
                fclose(pid);
        }
        if (ready >= 0) {
                // In the background: nothing on the terminal it started from.
                const int null = open("/dev/null", O_RDWR);
                if (null >= 0) {
                        dup2(null, 0);
                        dup2(null, 1);
                        dup2(null, 2);
                        if (null > 2) close(null);
                }
        }
        const int status = server.run();
        (void)unlink(PidFile);
        return status;
}
