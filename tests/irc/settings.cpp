/**
 * @file      settings.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * IRC's settings files (irc/core/settings.h): networks.ini and settings.ini read, mistakes told.
 */

#include "helpers.h"
#include "settings.h"

#include <cstdio>

using irc::Error;
using irc::Login;
using irc::Text;

namespace {

        std::vector<std::string> strings(const irc::Vector<irc::String> &list) {
                std::vector<std::string> out;
                for (const irc::String &s : list) out.push_back(test::str(s));
                return out;
        }

} // namespace

TEST_CASE("networks.ini: every setting, comments, spaces, sections") {
        const char                       *ini = "# a comment\n"
                                                "; another\n"
                                                "[Libera.Chat]\n"
                                                "  host = irc.libera.chat  \r\n"
                                                "port=6697\n"
                                                "tls = yes\n"
                                                "nick = jth\n"
                                                "altnicks = jth_, jth2 ,\n"
                                                "user = u\n"
                                                "realname = Jason H\n"
                                                "login = SASL\n"
                                                "account = jthacct\n"
                                                "password = p a s s\n"
                                                "channels = #bmds, #c key , \n"
                                                "autoconnect = no\n"
                                                "\n"
                                                "[Test]\n"
                                                "host = 172.20.0.43\n"
                                                "port = 6697\n"
                                                "nick = tester\n"
                                                "login = sasl-external\n"
                                                "cert = certs/me.pem\n"
                                                "key = certs/me.key\n"
                                                "ca = test-ca.pem\n"
                                                "tls = off\n";
        irc::Vector<irc::NetworkSettings> nets;
        irc::Vector<irc::String>          problems;
        REQUIRE(irc::readNetworks(ini, nets, problems) == Error::Ok);
        CHECK(strings(problems).empty());
        REQUIRE(nets.size() == 2);
        const irc::NetworkSettings &l = nets[0];
        CHECK(l.name == Text("Libera.Chat"));
        CHECK(l.host == Text("irc.libera.chat"));
        CHECK(l.port == 6697);
        CHECK(l.tls);
        CHECK_FALSE(l.autoconnect);
        CHECK(l.session.nick == Text("jth"));
        CHECK(strings(l.session.altNicks) == std::vector<std::string>{"jth_", "jth2"});
        CHECK(l.session.user == Text("u"));
        CHECK(l.session.realName == Text("Jason H"));
        CHECK(l.session.login == Login::Sasl);
        CHECK(l.session.account == Text("jthacct"));
        CHECK(l.session.password == Text("p a s s"));
        CHECK(strings(l.session.autojoin) == std::vector<std::string>{"#bmds", "#c key"});
        const irc::NetworkSettings &t = nets[1];
        CHECK_FALSE(t.tls);
        CHECK(t.autoconnect); // the default
        CHECK(t.session.login == Login::SaslExternal);
        CHECK(t.cert == Text("certs/me.pem"));
        CHECK(t.key == Text("certs/me.key"));
        CHECK(t.ca == Text("test-ca.pem"));
}

TEST_CASE("networks.ini: mistakes told by line; a network without a host or nick left out") {
        const char *ini = "host = nowhere\n"         // 1: no section
                          "[A]\n"                    // 2
                          "host = a.example\n"       // 3
                          "port = 70000\n"           // 4
                          "tls = maybe\n"            // 5
                          "colour = blue\n"          // 6
                          "this is not a line\n"     // 7
                          "nick = a\n"               // 8
                          "login = telepathy\n"      // 9
                          "[B]\n"                    // 10
                          "host = b.example\n"       // 11
                          "[C]\n"                    // 12
                          "nick = c\n"               // 13
                          "[D]\n"                    // 14
                          "host = d.example\n"       // 15
                          "nick = d\n"               // 16
                          "login = nickserv\n"       // 17
                          "[E]\n"                    // 18
                          "host = e.example\n"       // 19
                          "nick = e\n"               // 20
                          "login = sasl-external\n"; // 21
        irc::Vector<irc::NetworkSettings> nets;
        irc::Vector<irc::String>          problems;
        REQUIRE(irc::readNetworks(ini, nets, problems) == Error::Ok);
        const std::vector<std::string> p = strings(problems);
        auto                           has = [&](const std::string &want) {
                for (const std::string &s : p)
                        if (s == want) return true;
                return false;
        };
        CHECK(has("networks.ini:1: not in a network's [section]"));
        CHECK(has("networks.ini:4: a port is 1 to 65535: 70000"));
        CHECK(has("networks.ini:5: tls is yes or no: maybe"));
        CHECK(has("networks.ini:6: not a network's setting: colour"));
        CHECK(has("networks.ini:7: not [section] or key = value"));
        CHECK(has("networks.ini:9: login is none, sasl, sasl-external, nickserv or password: telepathy"));
        CHECK(has("networks.ini:11: B: no nick (set one to connect)"));
        CHECK(has("networks.ini:13: C: no host"));
        CHECK(has("networks.ini:15: D: the login needs a password"));
        CHECK(has("networks.ini:19: E: sasl-external needs cert and key (the client certificate)"));
        REQUIRE(nets.size() == 5); // every one kept, to be filled in
        CHECK(nets[0].name == Text("A"));
        CHECK(nets[0].port == 6697); // the bad one ignored
        CHECK(nets[0].isComplete());
        CHECK_FALSE(nets[1].isComplete()); // B: no nick
        CHECK_FALSE(nets[2].isComplete()); // C: no host
        CHECK(nets[3].isComplete());       // D: complete, though its login can't work
}

TEST_CASE("servers.ini: read; the first networks.ini made of the ones offered, waiting for a nick") {
        const char                   *ini = "# the list\n"
                                            "[Libera.Chat]\nhost = irc.libera.chat\nport = 6697\ntls = yes\nlogin = sasl\n"
                                            "about = Free and open source projects\noffer = yes\n"
                                            "[Plain]\nhost = irc.plain.example\nport = 6667\ntls = no\n"
                                            "[NoHost]\nport = 6697\n"
                                            "[Odd]\nhost = odd.example\nport = 99999\nlogin = magic\ncolour = blue\n"
                                            "[OFTC]\nhost = irc.oftc.net\noffer = yes\n";
        irc::Vector<irc::ServerEntry> servers;
        irc::Vector<irc::String>      problems;
        REQUIRE(irc::readServers(ini, servers, problems) == Error::Ok);
        REQUIRE(servers.size() == 4); // NoHost dropped
        CHECK(servers[0].login == Login::Sasl);
        CHECK(servers[0].offer);
        CHECK(servers[0].about == Text("Free and open source projects"));
        CHECK(servers[1].port == 6667);
        CHECK_FALSE(servers[1].tls);
        CHECK(servers[2].port == 6697); // the bad port ignored
        CHECK(servers[3].port == 6697); // the default
        const std::vector<std::string> p = strings(problems);
        REQUIRE(p.size() == 4);
        CHECK(p[0].find("a port is 1 to 65535") != std::string::npos);
        CHECK(p[1].find("login is none, sasl or nickserv") != std::string::npos);
        CHECK(p[2].find("not a list entry's setting: colour") != std::string::npos);
        CHECK(p[3] == "servers.ini: NoHost: no host");

        irc::String text;
        REQUIRE(irc::firstNetworks(servers, text) == Error::Ok);
        CHECK(test::str(text).rfind("# IRC's networks", 0) == 0);
        irc::Vector<irc::NetworkSettings> nets;
        problems.clear();
        REQUIRE(irc::readNetworks(text, nets, problems) == Error::Ok);
        REQUIRE(nets.size() == 2);
        CHECK(nets[0].name == Text("Libera.Chat"));
        CHECK(nets[1].host == Text("irc.oftc.net"));
        CHECK_FALSE(nets[0].isComplete());
        CHECK_FALSE(nets[0].autoconnect);
        CHECK(nets[0].session.login == Login::None); // no password yet
        REQUIRE(strings(problems).size() == 2);
        CHECK(strings(problems)[0].find("no nick") != std::string::npos);

        irc::Vector<irc::ServerEntry> none;
        REQUIRE(irc::firstNetworks(none, text) == Error::Ok);
        nets.clear();
        REQUIRE(irc::readNetworks(text, nets, problems) == Error::Ok);
        CHECK(nets.size() == 0); // the comment alone
}

TEST_CASE("servers.ini as shipped: every entry whole, TLS, Libera.Chat and OFTC offered") {
        // irc/servers.ini, beside this file's folder's parent.
        std::string path = __FILE__;
        path = path.substr(0, path.rfind('/')); // tests/irc
        path = path.substr(0, path.rfind('/')); // tests
        path = path.substr(0, path.rfind('/')) + "/irc/servers.ini";
        FILE *f = fopen(path.c_str(), "rb");
        if (f == nullptr) {
                FAIL_CHECK("no ", path); // without exceptions a REQUIRE doesn't stop the test
                return;
        }
        std::string text;
        char        chunk[4096];
        size_t      n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, n);
        fclose(f);
        irc::Vector<irc::ServerEntry> servers;
        irc::Vector<irc::String>      problems;
        REQUIRE(irc::readServers(test::text(text), servers, problems) == Error::Ok);
        CHECK(strings(problems).empty());
        CHECK(servers.size() >= 15);
        std::vector<std::string> offered;
        for (const irc::ServerEntry &e : servers) {
                CAPTURE(test::str(e.name));
                CHECK(!e.about.isEmpty());
                CHECK(e.host.view().find('.') != Text::NoPos);
                if (e.offer) offered.push_back(test::str(e.name));
        }
        CHECK(offered == std::vector<std::string>{"Libera.Chat", "OFTC"});
}

TEST_CASE("networks.ini: written and read back the same; copied") {
        const char                       *ini = "[Libera.Chat]\n"
                                                "host = irc.libera.chat\nport = 6697\ntls = yes\nnick = jth\naltnicks = jth_, jth2\n"
                                                "user = u\nrealname = Jason H\nlogin = sasl\naccount = jthacct\npassword = p a s s\n"
                                                "channels = #bmds, #c key\nperform = /mode $me +x\n"
                                                "perform = /msg bot hello\nnotify = alice, bob\nautoconnect = no\n"
                                                "[Test]\nhost = 1.2.3.4\nport = 7000\ntls = no\nnick = t\nlogin = sasl-external\n"
                                                "cert = me.pem\nkey = me.key\nca = ca.pem\nfingerprint = SHA256:abcd\n";
        irc::Vector<irc::NetworkSettings> nets, back;
        irc::Vector<irc::String>          problems;
        REQUIRE(irc::readNetworks(ini, nets, problems) == Error::Ok);
        irc::String text;
        REQUIRE(irc::writeNetworks(nets, text) == Error::Ok);
        CHECK(test::str(text).rfind("# IRC's networks", 0) == 0); // the comment kept
        REQUIRE(irc::readNetworks(text, back, problems) == Error::Ok);
        CHECK(strings(problems).empty());
        REQUIRE(back.size() == 2);
        for (size_t i = 0; i < 2; ++i) {
                const irc::NetworkSettings &a = nets[i], &b = back[i];
                CHECK(a.name == b.name.view());
                CHECK(a.host == b.host.view());
                CHECK(a.port == b.port);
                CHECK(a.tls == b.tls);
                CHECK(a.autoconnect == b.autoconnect);
                CHECK(a.session.nick == b.session.nick.view());
                CHECK(strings(a.session.altNicks) == strings(b.session.altNicks));
                CHECK(a.session.user == b.session.user.view());
                CHECK(a.session.realName == b.session.realName.view());
                CHECK(a.session.login == b.session.login);
                CHECK(a.session.account == b.session.account.view());
                CHECK(a.session.password == b.session.password.view());
                CHECK(strings(a.session.autojoin) == strings(b.session.autojoin));
                CHECK(strings(a.session.perform) == strings(b.session.perform));
                CHECK(strings(a.notify) == strings(b.notify));
                CHECK(a.cert == b.cert.view());
                CHECK(a.key == b.key.view());
                CHECK(a.ca == b.ca.view());
                CHECK(a.fingerprint == b.fingerprint.view());
        }
        CHECK(strings(nets[0].session.perform) == std::vector<std::string>{"/mode $me +x", "/msg bot hello"});
        CHECK(strings(nets[0].notify) == std::vector<std::string>{"alice", "bob"});
        irc::NetworkSettings copy;
        REQUIRE(irc::copySettings(nets[0], copy) == Error::Ok);
        CHECK(strings(copy.session.perform).size() == 2);
        CHECK(strings(copy.notify).size() == 2);
        REQUIRE(irc::copySettings(nets[1], copy) == Error::Ok);
        CHECK(copy.fingerprint == Text("SHA256:abcd"));
        CHECK(copy.session.login == Login::SaslExternal);
        CHECK(copy.port == 7000);
}

TEST_CASE("settings.ini: joins, highlights, logs; mistakes told") {
        const char              *ini = "[view]\n"
                                       "joins = no\n"
                                       "[highlights]\n"
                                       "words = bmds, esp32 ,p4\n"
                                       "[logs]\n"
                                       "enabled = no\n"
                                       "folder = /sd0/p0/irclogs\n"
                                       "[view]\n"
                                       "font = huge\n"
                                       "[behaviour]\n"
                                       "rejoin = yes\n"
                                       "invite = maybe\n"
                                       "[ignore]\n"
                                       "masks = spam!*@*, *!*@x@Libera.Chat\n";
        irc::AppSettings         s;
        irc::Vector<irc::String> problems;
        REQUIRE(irc::readSettings(ini, s, problems) == Error::Ok);
        CHECK_FALSE(s.showJoins);
        CHECK(strings(s.highlights) == std::vector<std::string>{"bmds", "esp32", "p4"});
        CHECK_FALSE(s.logs);
        CHECK(s.logFolder == Text("/sd0/p0/irclogs"));
        CHECK(strings(problems) == std::vector<std::string>{"settings.ini:9: not a setting: view/font",
                                                            "settings.ini:12: invite is yes or no: maybe"});
        CHECK(s.rejoin);
        CHECK_FALSE(s.joinInvited);
        CHECK(strings(s.ignores) == std::vector<std::string>{"spam!*@*", "*!*@x@Libera.Chat"});

        // Written and read back: the same.
        s.tree = true;
        irc::String text;
        REQUIRE(irc::writeSettings(s, text) == Error::Ok);
        irc::AppSettings         back;
        irc::Vector<irc::String> none;
        REQUIRE(irc::readSettings(text, back, none) == Error::Ok);
        CHECK(none.size() == 0);
        CHECK_FALSE(back.showJoins);
        CHECK(back.tree);
        CHECK(strings(back.highlights) == std::vector<std::string>{"bmds", "esp32", "p4"});
        CHECK_FALSE(back.logs);
        CHECK(back.logFolder == Text("/sd0/p0/irclogs"));
        CHECK(back.rejoin);
        CHECK(strings(back.ignores) == strings(s.ignores));

        irc::AppSettings empty;
        REQUIRE(irc::readSettings("", empty, problems) == Error::Ok);
        CHECK(empty.showJoins);
        CHECK_FALSE(empty.tree);
        CHECK(empty.logs);
        CHECK(empty.logFolder == Text("logs"));
}
