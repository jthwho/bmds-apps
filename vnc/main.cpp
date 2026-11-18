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
 *     vncserver --desktop  the same, from the launcher (app.desktop): what isn't a start (running
 *                          already, or why it can't) told in a notification, as there is no terminal
 *     vncserver -f         start, in the foreground (what the background one runs)
 *     vncserver stop       stop the running one
 *     vncserver status     say whether one runs, and where it listens
 *     vncserver passwd     set the password viewers give (read from the terminal, or piped in):
 *                          sysconf's vnc/password, a secret, kept as VNC keeps them (devplan/vnc.md §28)
 */

#include "des.h"
#include "server.h"

#include <bmds/guiclasses.h>
#include <bmds/sys.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
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
                 "interface, only with a password; encrypted as vnc/encryption says)",
                 "loopback|lan", 0, 0},
                {"vnc/jpeg-quality", BMDS_SYSCONF_TYPE_INT, "80", "The VNC server's JPEG quality at most (1-100)",
                 nullptr, 1, 100},
                {"vnc/start-wait", BMDS_SYSCONF_TYPE_INT, "30",
                 "Seconds the VNC server waits for the desktop when started early (autoexec.sh)", nullptr, 0, 600},
                {"vnc/encryption", BMDS_SYSCONF_TYPE_ENUM, "tls",
                 "On the LAN: tls (VeNCrypt with the board's certificate; TigerVNC, TurboVNC, Remmina) or none (the "
                 "password alone, for viewers without VeNCrypt: it can be guessed from the traffic)",
                 "tls|none", 0, 0},
        };

        // The password viewers give: a secret (shown as "(set)"), VNC's way of storing them (DES
        // with VNC's fixed key, in hex; devplan/vnc.md §28): the challenge needs the password
        // itself, so it can't be a hash.
        constexpr Param PasswordParam = {"vnc/password",
                                         BMDS_SYSCONF_TYPE_SECRET,
                                         "",
                                         "The VNC server's password, as VNC stores them; set with 'vncserver passwd' "
                                         "(empty: none)",
                                         nullptr,
                                         0,
                                         0};

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

        // The password setting, declared: its id, or -1.
        int32_t passwordId() {
                const Param         &p = PasswordParam;
                bmds_sysconf_param_t d = {};
                d.size = sizeof(d);
                d.type = p.type;
                d.key = p.key;
                d.default_value = p.def;
                d.help = p.help;
                int32_t id = bmds_sysconf_declare(&d);
                if (id < 0) id = bmds_sysconf_find(p.key);
                return id;
        }

        bool readPassword(char *out, size_t size) {
                out[0] = '\0';
                const int32_t id = passwordId();
                char          stored[vnc::StoredChars + 2];
                // The value itself (a BMDS before BMDS_SYSCONF_FIELD_SECRET: none).
                if (id < 0 || bmds_sysconf_get(id, BMDS_SYSCONF_FIELD_SECRET, stored, sizeof(stored)) <= 0)
                        return false;
                char plain[9];
                if (!vnc::revealPassword(stored, plain)) {
                        fprintf(stderr, "vncserver: vnc/password isn't one 'vncserver passwd' set: set it again\n");
                        return false;
                }
                snprintf(out, size, "%s", plain);
                memset(plain, 0, sizeof(plain));
                return out[0] != '\0';
        }

        int setPassword() {
                char line[64];
                // Not echoed at a terminal.
                struct termios was;
                const bool     tty = isatty(0) && tcgetattr(0, &was) == 0;
                if (tty) {
                        struct termios quiet = was;
                        quiet.c_lflag &= ~tcflag_t(ECHO);
                        (void)tcsetattr(0, TCSANOW, &quiet);
                        printf("Password for VNC viewers (8 characters count; empty for none): ");
                        fflush(stdout);
                }
                const bool got = fgets(line, sizeof(line), stdin) != nullptr;
                if (tty) {
                        (void)tcsetattr(0, TCSANOW, &was);
                        printf("\n");
                }
                if (!got) return 1;
                line[strcspn(line, "\r\n")] = '\0';
                const int32_t id = passwordId();
                char          stored[vnc::StoredChars + 1] = "";
                if (line[0] != '\0') vnc::obfuscatePassword(line, stored);
                const size_t length = strlen(line);
                memset(line, 0, sizeof(line));
                if (id < 0 || bmds_sysconf_set(id, BMDS_SYSCONF_OP_SET, stored) < 0 ||
                    bmds_sysconf_set(id, BMDS_SYSCONF_OP_SAVE, nullptr) < 0) {
                        fprintf(stderr, "vncserver: vnc/password couldn't be saved\n");
                        return 1;
                }
                if (stored[0] == '\0') {
                        printf("vncserver: no password: viewers on the board's loopback get in without one\n");
                        return 0;
                }
                if (length > 8) printf("vncserver: VNC uses the first 8 characters only\n");
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

        // Where the running one really listens into @p where: what it wrote at its start (the
        // settings may have changed since).
        void runningAddress(char *where, size_t size) {
                snprintf(where, size, "an address it didn't say");
                FILE *f = fopen(PidFile, "r");
                if (f == nullptr) return;
                char line[80];
                if (fgets(line, sizeof(line), f) != nullptr && fgets(line, sizeof(line), f) != nullptr) {
                        line[strcspn(line, "\r\n")] = '\0';
                        snprintf(where, size, "%s", line);
                }
                fclose(f);
                return;
        }

        int status() {
                const pid_t pid = runningPid();
                if (pid < 0) {
                        printf("vncserver: not running\n");
                        return 1;
                }
                char where[80];
                runningAddress(where, sizeof(where));
                printf("vncserver: running (pid %ld), listening on %s\n", long(pid), where);
                return 0;
        }

        // Started from the launcher (--desktop): @p body in a notification, the terminal's message
        // there. Nothing if there's no desktop to show it.
        void tellDesktop(const char *title, const char *body, const char *urgency) {
                bmds_gui_t *gui = bmds_gui_connect(nullptr);
                if (gui == nullptr) return;
                const uint32_t n = bmds_notification_new(gui, 0);
                bmds_notification_set_app(gui, n, "VNC Server");
                bmds_notification_set_icon(gui, n, "preferences-system-network");
                bmds_notification_set_title(gui, n, title);
                bmds_notification_set_body(gui, n, body);
                bmds_notification_set_urgency(gui, n, urgency);
                bmds_notification_show(gui, n);
                (void)bmds_gui_flush(gui);
                bmds_gui_close(gui);
                return;
        }

        // Started from a terminal: the server again, in a session of its own (a login ending doesn't
        // hang it up), told to say on a pipe when it listens; this one prints that and returns.
        // @p desktop: from the launcher, a failure told in a notification too (the server's own
        // "VNC server started" tells a start).
        int startInBackground(const char *self, bool desktop) {
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
                        if (desktop)
                                tellDesktop("VNC server didn't start", "It ended before it was listening", "normal");
                        return 1;
                }
                fprintf(line[0] == '0' ? stdout : stderr, "vncserver: %s", line + 2);
                if (line[0] == '0') return 0;
                if (desktop) {
                        line[strcspn(line, "\r\n")] = '\0';
                        tellDesktop("VNC server didn't start", line + 2, "normal");
                }
                return 1;
        }

} // namespace

int main(int argc, char **argv) {
        bool foreground = false, verbose = false, desktop = false;
        int  ready = -1;
        for (int i = 1; i < argc; ++i) {
                if (strcmp(argv[i], "-f") == 0) {
                        foreground = true;
                } else if (strcmp(argv[i], "-v") == 0) {
                        verbose = true;
                } else if (strcmp(argv[i], "--desktop") == 0) {
                        desktop = true;
                } else if (strcmp(argv[i], "--ready") == 0 && i + 1 < argc) {
                        ready = atoi(argv[++i]);
                } else if (strcmp(argv[i], "stop") == 0) {
                        return stop();
                } else if (strcmp(argv[i], "status") == 0) {
                        return status();
                } else if (strcmp(argv[i], "passwd") == 0) {
                        return setPassword();
                } else {
                        fprintf(stderr, "usage: vncserver [--desktop | -f [-v]] | stop | status | passwd\n");
                        return 2;
                }
        }
        if (runningPid() >= 0) {
                fprintf(stderr, "vncserver: running already ('vncserver status')\n");
                if (desktop) {
                        char where[80], body[160];
                        runningAddress(where, sizeof(where));
                        snprintf(body, sizeof(body), "Listening on %s; Stop Server is in its tray icon's menu", where);
                        tellDesktop("VNC server is running already", body, "low");
                }
                return 1;
        }
        if (!foreground) return startInBackground(argv[0], desktop);

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
        setting(Params[4], value, sizeof(value));
        s.tls = s.lan && strcmp(value, "none") != 0;
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
