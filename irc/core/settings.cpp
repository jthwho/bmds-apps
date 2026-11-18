/**
 * @file      settings.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "settings.h"

namespace irc {

        const char *const FirstNetworks =
                "# IRC's networks (devplan/irc.md §4 I3): a section a network. Set your nick and\n"
                "# autoconnect = yes, then Network > Connect.\n"
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
                                } else {
                                        problem(problems, file, line, "not a network's setting: ", key);
                                }
                        },
                        [&](size_t line) { problem(problems, file, line, "not [section] or key = value"); });
                if (err != Error::Ok) return err;
                // A network without a host or a nick can't connect: left out, and said.
                for (size_t i = out.size(); i > 0; --i) {
                        NetworkSettings &n = out[i - 1];
                        if (n.host.isEmpty() || n.session.nick.isEmpty()) {
                                problem(problems, file, firstLines[i - 1], n.name,
                                        n.host.isEmpty() ? Text(": no host") : Text(": no nick (set one to connect)"));
                                out.removeAt(i - 1);
                                firstLines.removeAt(i - 1);
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

} // namespace irc
