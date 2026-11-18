/**
 * @file      settings.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "settings.h"

#include <cstdio>

namespace irc {

        const char *const FirstNetworks =
                "# IRC's networks (devplan/irc.md §4 I3): a section a network. IRC's Network > Networks...\n"
                "# edits this file (and writes it again); it can be edited by hand too.\n"
                "#\n"
                "#   host, port, tls (yes/no)  where it is (TLS on 6697 by default)\n"
                "#   nick, altnicks            your nick, and others to try (a, b)\n"
                "#   user, realname            as the network shows you (the nick if empty)\n"
                "#   login                     none, sasl, sasl-external, nickserv or password\n"
                "#   account, password         the login's (the account is the nick if empty)\n"
                "#   cert, key                 sasl-external's client certificate and key (PEM files)\n"
                "#   ca                        more roots to trust for this network (PEM file)\n"
                "#   channels                  joined once connected: #a, #b key\n"
                "#   autoconnect               connect at the start (yes/no)\n"
                "\n"
                "[Libera.Chat]\n"
                "host = irc.libera.chat\n"
                "port = 6697\n"
                "tls = yes\n"
                "nick =\n"
                "login = none\n"
                "channels =\n"
                "autoconnect = no\n"
                "\n"
                "[OFTC]\n"
                "host = irc.oftc.net\n"
                "port = 6697\n"
                "tls = yes\n"
                "nick =\n"
                "login = none\n"
                "channels =\n"
                "autoconnect = no\n";

        namespace {

                bool yes(Text value, bool &out) {
                        if (value.equalsIgnoringCase("yes") || value.equalsIgnoringCase("true") || value == Text("1") ||
                            value.equalsIgnoringCase("on")) {
                                out = true;
                                return true;
                        }
                        if (value.equalsIgnoringCase("no") || value.equalsIgnoringCase("false") || value == Text("0") ||
                            value.equalsIgnoringCase("off")) {
                                out = false;
                                return true;
                        }
                        return false;
                }

                // Calls @p fn with each item of a comma-separated list, its spaces taken off.
                template <typename F> void eachItem(Text list, F &&fn) {
                        size_t at = 0;
                        while (at <= list.size()) {
                                size_t end = list.find(',', at);
                                if (end == Text::NoPos) end = list.size();
                                Text item = list.mid(at, end - at);
                                while (!item.isEmpty() && item[0] == ' ') item = item.mid(1);
                                while (!item.isEmpty() && item[item.size() - 1] == ' ')
                                        item = item.left(item.size() - 1);
                                if (!item.isEmpty()) fn(item);
                                at = end + 1;
                        }
                        return;
                }

                Error addItem(Vector<String> &list, Text item) {
                        String s;
                        if (s.assign(item) != Error::Ok) return Error::NoMem;
                        return list.append(std::move(s));
                }

                // `FILE:LINE: what`
                void problem(Vector<String> &problems, const char *file, size_t line, Text what, Text detail = Text()) {
                        String p;
                        (void)p.assign(file);
                        (void)p.append(':');
                        char   digits[20];
                        size_t n = 0;
                        do {
                                digits[n++] = char('0' + line % 10);
                                line /= 10;
                        } while (line > 0 && n < sizeof(digits));
                        while (n > 0) (void)p.append(digits[--n]);
                        (void)p.append(": ");
                        (void)p.append(what);
                        (void)p.append(detail);
                        (void)problems.append(std::move(p));
                        return;
                }

        } // namespace

        Error readNetworks(Text text, Vector<NetworkSettings> &out, Vector<String> &problems) {
                const char      *file = "networks.ini";
                Error            err = Error::Ok;
                NetworkSettings *current = nullptr;
                Vector<size_t>   firstLines; // each network's section's line
                readIni(
                        text,
                        [&](Text section, Text key, Text value, size_t line) {
                                if (section.isEmpty()) {
                                        problem(problems, file, line, "not in a network's [section]");
                                        return;
                                }
                                if (current == nullptr || !(current->name == section)) {
                                        current = nullptr;
                                        for (NetworkSettings &n : out)
                                                if (n.name == section) current = &n;
                                        if (current == nullptr) {
                                                NetworkSettings n;
                                                if (n.name.assign(section) != Error::Ok ||
                                                    out.append(std::move(n)) != Error::Ok) {
                                                        err = Error::NoMem;
                                                        return;
                                                }
                                                (void)firstLines.append(size_t(line));
                                                current = &out.last();
                                        }
                                }
                                NetworkSettings &n = *current;
                                Config          &c = n.session;
                                if (key == Text("host")) {
                                        (void)n.host.assign(value);
                                } else if (key == Text("port")) {
                                        unsigned v = 0;
                                        bool     ok = !value.isEmpty() && value.size() <= 5;
                                        for (char d : value) {
                                                if (d < '0' || d > '9') ok = false;
                                                v = v * 10 + unsigned(d - '0');
                                        }
                                        if (!ok || v == 0 || v > 65535) {
                                                problem(problems, file, line, "a port is 1 to 65535: ", value);
                                                return;
                                        }
                                        n.port = uint16_t(v);
                                } else if (key == Text("tls")) {
                                        if (!yes(value, n.tls))
                                                problem(problems, file, line, "tls is yes or no: ", value);
                                } else if (key == Text("autoconnect")) {
                                        if (!yes(value, n.autoconnect))
                                                problem(problems, file, line, "autoconnect is yes or no: ", value);
                                } else if (key == Text("nick")) {
                                        (void)c.nick.assign(value);
                                } else if (key == Text("altnicks")) {
                                        c.altNicks.clear();
                                        eachItem(value, [&](Text item) { (void)addItem(c.altNicks, item); });
                                } else if (key == Text("user")) {
                                        (void)c.user.assign(value);
                                } else if (key == Text("realname")) {
                                        (void)c.realName.assign(value);
                                } else if (key == Text("login")) {
                                        if (value.equalsIgnoringCase("none") || value.isEmpty())
                                                c.login = Login::None;
                                        else if (value.equalsIgnoringCase("sasl"))
                                                c.login = Login::Sasl;
                                        else if (value.equalsIgnoringCase("sasl-external"))
                                                c.login = Login::SaslExternal;
                                        else if (value.equalsIgnoringCase("nickserv"))
                                                c.login = Login::NickServ;
                                        else if (value.equalsIgnoringCase("password"))
                                                c.login = Login::Password;
                                        else
                                                problem(problems, file, line,
                                                        "login is none, sasl, sasl-external, nickserv or password: ",
                                                        value);
                                } else if (key == Text("account")) {
                                        (void)c.account.assign(value);
                                } else if (key == Text("password")) {
                                        (void)c.password.assign(value);
                                } else if (key == Text("channels")) {
                                        c.autojoin.clear();
                                        eachItem(value, [&](Text item) { (void)addItem(c.autojoin, item); });
                                } else if (key == Text("cert")) {
                                        (void)n.cert.assign(value);
                                } else if (key == Text("key")) {
                                        (void)n.key.assign(value);
                                } else if (key == Text("ca")) {
                                        (void)n.ca.assign(value);
                                } else if (key == Text("fingerprint")) {
                                        (void)n.fingerprint.assign(value);
                                } else {
                                        problem(problems, file, line, "not a network's setting: ", key);
                                }
                        },
                        [&](size_t line) { problem(problems, file, line, "not [section] or key = value"); });
                if (err != Error::Ok) return err;
                // A network without a host or a nick can't connect: kept (to be filled in), and said.
                for (size_t i = out.size(); i > 0; --i) {
                        NetworkSettings &n = out[i - 1];
                        if (n.host.isEmpty() || n.session.nick.isEmpty()) {
                                problem(problems, file, firstLines[i - 1], n.name,
                                        n.host.isEmpty() ? Text(": no host") : Text(": no nick (set one to connect)"));
                        } else if (n.session.login == Login::SaslExternal && (n.cert.isEmpty() || n.key.isEmpty())) {
                                problem(problems, file, firstLines[i - 1], n.name,
                                        ": sasl-external needs cert and key (the client certificate)");
                        } else if ((n.session.login == Login::Sasl || n.session.login == Login::NickServ ||
                                    n.session.login == Login::Password) &&
                                   n.session.password.isEmpty()) {
                                problem(problems, file, firstLines[i - 1], n.name, ": the login needs a password");
                        }
                }
                return Error::Ok;
        }

        Error readSettings(Text text, AppSettings &out, Vector<String> &problems) {
                const char *file = "settings.ini";
                if (out.logFolder.isEmpty() && out.logFolder.assign("logs") != Error::Ok) return Error::NoMem;
                readIni(
                        text,
                        [&](Text section, Text key, Text value, size_t line) {
                                if (section == Text("view") && key == Text("joins")) {
                                        if (!yes(value, out.showJoins))
                                                problem(problems, file, line, "joins is yes or no: ", value);
                                } else if (section == Text("view") && key == Text("tree")) {
                                        if (!yes(value, out.tree))
                                                problem(problems, file, line, "tree is yes or no: ", value);
                                } else if (section == Text("highlights") && key == Text("words")) {
                                        out.highlights.clear();
                                        eachItem(value, [&](Text item) { (void)addItem(out.highlights, item); });
                                } else if (section == Text("logs") && key == Text("enabled")) {
                                        if (!yes(value, out.logs))
                                                problem(problems, file, line, "enabled is yes or no: ", value);
                                } else if (section == Text("logs") && key == Text("folder")) {
                                        if (!value.isEmpty()) (void)out.logFolder.assign(value);
                                } else {
                                        String what;
                                        (void)what.assign(section);
                                        (void)what.append('/');
                                        (void)what.append(key);
                                        problem(problems, file, line, "not a setting: ", what);
                                }
                        },
                        [&](size_t line) { problem(problems, file, line, "not [section] or key = value"); });
                return Error::Ok;
        }

        namespace {

                Error copyList(const Vector<String> &from, Vector<String> &to) {
                        to.clear();
                        for (const String &s : from)
                                if (addItem(to, s) != Error::Ok) return Error::NoMem;
                        return Error::Ok;
                }

                void line(String &out, const char *key, Text value) {
                        (void)out.append(key);
                        (void)out.append(" = ");
                        (void)out.append(value);
                        (void)out.append('\n');
                        return;
                }

                void list(String &out, const char *key, const Vector<String> &items) {
                        (void)out.append(key);
                        (void)out.append(" =");
                        for (size_t i = 0; i < items.size(); ++i) {
                                (void)out.append(i == 0 ? " " : ", ");
                                (void)out.append(items[i]);
                        }
                        (void)out.append('\n');
                        return;
                }

        } // namespace

        Error copySettings(const NetworkSettings &from, NetworkSettings &to) {
                const Config &f = from.session;
                Config       &t = to.session;
                to.port = from.port;
                to.tls = from.tls;
                to.autoconnect = from.autoconnect;
                t.login = f.login;
                if (to.name.assign(from.name) != Error::Ok || to.host.assign(from.host) != Error::Ok ||
                    to.cert.assign(from.cert) != Error::Ok || to.key.assign(from.key) != Error::Ok ||
                    to.ca.assign(from.ca) != Error::Ok || to.fingerprint.assign(from.fingerprint) != Error::Ok ||
                    t.nick.assign(f.nick) != Error::Ok || t.user.assign(f.user) != Error::Ok ||
                    t.realName.assign(f.realName) != Error::Ok || t.account.assign(f.account) != Error::Ok ||
                    t.password.assign(f.password) != Error::Ok || t.version.assign(f.version) != Error::Ok ||
                    copyList(f.altNicks, t.altNicks) != Error::Ok || copyList(f.autojoin, t.autojoin) != Error::Ok)
                        return Error::NoMem;
                return Error::Ok;
        }

        Error writeNetworks(const Vector<NetworkSettings> &networks, String &out) {
                static const char *const Logins[] = {"none", "sasl", "sasl-external", "nickserv", "password"};
                out.clear();
                // The first file's comment (its lines starting with #), then each network.
                const Text   first = FirstNetworks;
                const size_t end = first.find('[');
                (void)out.append(first.left(end == Text::NoPos ? first.size() : end));
                for (const NetworkSettings &n : networks) {
                        char port[8];
                        snprintf(port, sizeof(port), "%u", unsigned(n.port));
                        (void)out.append('[');
                        (void)out.append(n.name);
                        (void)out.append("]\n");
                        line(out, "host", n.host);
                        line(out, "port", port);
                        line(out, "tls", n.tls ? "yes" : "no");
                        line(out, "nick", n.session.nick);
                        list(out, "altnicks", n.session.altNicks);
                        line(out, "user", n.session.user);
                        line(out, "realname", n.session.realName);
                        line(out, "login", Logins[size_t(n.session.login) < 5 ? size_t(n.session.login) : 0]);
                        line(out, "account", n.session.account);
                        line(out, "password", n.session.password);
                        if (!n.cert.isEmpty()) line(out, "cert", n.cert);
                        if (!n.key.isEmpty()) line(out, "key", n.key);
                        if (!n.ca.isEmpty()) line(out, "ca", n.ca);
                        if (!n.fingerprint.isEmpty()) line(out, "fingerprint", n.fingerprint);
                        list(out, "channels", n.session.autojoin);
                        line(out, "autoconnect", n.autoconnect ? "yes" : "no");
                        (void)out.append('\n');
                }
                return out.isEmpty() ? Error::NoMem : Error::Ok;
        }

        Error writeSettings(const AppSettings &s, String &out) {
                out.clear();
                (void)out.append("# IRC's settings (devplan/irc.md I3), written by IRC: its View menu keeps them.\n\n"
                                 "[view]\njoins = ");
                (void)out.append(s.showJoins ? "yes" : "no");
                (void)out.append("\ntree = ");
                (void)out.append(s.tree ? "yes" : "no");
                (void)out.append("\n\n[highlights]\nwords = ");
                for (size_t i = 0; i < s.highlights.size(); ++i) {
                        if (i > 0) (void)out.append(", ");
                        (void)out.append(s.highlights[i]);
                }
                (void)out.append("\n\n[logs]\nenabled = ");
                (void)out.append(s.logs ? "yes" : "no");
                (void)out.append("\nfolder = ");
                (void)out.append(s.logFolder);
                return out.append('\n');
        }

} // namespace irc
