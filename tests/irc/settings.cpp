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
        REQUIRE(nets.size() == 3); // A, D and E: kept, as they can try
        CHECK(nets[0].name == Text("A"));
        CHECK(nets[0].port == 6697); // the bad one ignored
}

TEST_CASE("networks.ini: the first one written has Libera.Chat and OFTC, waiting for a nick") {
        irc::Vector<irc::NetworkSettings> nets;
        irc::Vector<irc::String>          problems;
        REQUIRE(irc::readNetworks(irc::FirstNetworks, nets, problems) == Error::Ok);
        CHECK(nets.size() == 0);
        const std::vector<std::string> p = strings(problems);
        REQUIRE(p.size() == 2);
        CHECK(p[0].find("OFTC: no nick") != std::string::npos);
        CHECK(p[1].find("Libera.Chat: no nick") != std::string::npos);
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
                                       "font = huge\n";
        irc::AppSettings         s;
        irc::Vector<irc::String> problems;
        REQUIRE(irc::readSettings(ini, s, problems) == Error::Ok);
        CHECK_FALSE(s.showJoins);
        CHECK(strings(s.highlights) == std::vector<std::string>{"bmds", "esp32", "p4"});
        CHECK_FALSE(s.logs);
        CHECK(s.logFolder == Text("/sd0/p0/irclogs"));
        CHECK(strings(problems) == std::vector<std::string>{"settings.ini:9: not a setting: view/font"});

        irc::AppSettings empty;
        REQUIRE(irc::readSettings("", empty, problems) == Error::Ok);
        CHECK(empty.showJoins);
        CHECK(empty.logs);
        CHECK(empty.logFolder == Text("logs"));
}
