/**
 * @file      settings.h
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

        /** @brief A network's settings (`networks.ini`, a section each: devplan/irc.md I3). */
        struct NetworkSettings {
                        String   name; ///< The section's: Libera.Chat.
                        String   host;
                        uint16_t port = 6697;
                        bool     tls = true;
                        bool     autoconnect = true;
                        Config   session; ///< The nick, login, channels... (`login=`: §7a).
                        String   cert;    ///< SASL EXTERNAL's client certificate (PEM file), and its key.
                        String   key;
                        String   ca; ///< More roots trusted for this network (a server with its own CA).
                        String   fingerprint; ///< A certificate trusted by its SHA-256 ("SHA256:hex", I5): pinned.
                        Vector<String> notify;      ///< People watched on it (`notify = a, b`, §7g).

                        /** @brief Returns true if it has what connecting needs: a host and a nick. */
                        bool isComplete() const { return !host.isEmpty() && !session.nick.isEmpty(); }
        };

        /** @brief Copies @p from into @p to (the settings are move only, as their strings). @return `Ok` or `NoMem`. */
        Error copySettings(const NetworkSettings &from, NetworkSettings &to);

        /** @brief The app's settings (`settings.ini`). */
        struct AppSettings {
                        Vector<String> highlights;       ///< `[highlights] words = a, b`
                        bool           showJoins = true; ///< `[view] joins = yes`
                        bool           tree = false; ///< `[view] tree = no`: the conversations in a tree, not tabs (I7)
                        bool           logs = true;      ///< `[logs] enabled = yes`
                        String         logFolder; ///< `[logs] folder = logs` (below the data folder, or absolute)
                        bool           rejoin = false;      ///< `[behaviour] rejoin = no`: kicked, joined again (§7l)
                        bool           joinInvited = false; ///< `[behaviour] invite = no`: invited, joined (§7l)
                        Vector<String> ignores; ///< `[ignore] masks = a!*@*, *!*@h@Network` (§7f, `Ignores::read()`)
        };

        /** @brief A network of the list shipped with IRC (`servers.ini`, §7c). */
        struct ServerEntry {
                        String   name; ///< The section's: Libera.Chat.
                        String   host;
                        String   about; ///< A line: who it's for.
                        uint16_t port = 6697;
                        bool     tls = true;
                        Login    login = Login::None; ///< What it offers for a registered nick (sasl...).
                        bool     offer = false;       ///< One of the first networks offered (I6).
        };

        /**
         * @brief INI as the settings are written: `[section]`, `key = value`, `#` or `;` starting a
         *        comment line; spaces around keys and values taken off. Calls @p fn(section, key,
         *        value, line number) for each value; @p bad(line number) for a line that is none
         *        of these.
         */
        template <typename F, typename B> void readIni(Text text, F &&fn, B &&bad) {
                Text   section;
                size_t at = 0, number = 0;
                while (at < text.size()) {
                        size_t end = text.find('\n', at);
                        if (end == Text::NoPos) end = text.size();
                        Text line = text.mid(at, end - at);
                        at = end + 1;
                        ++number;
                        while (!line.isEmpty() && (line[0] == ' ' || line[0] == '\t')) line = line.mid(1);
                        while (!line.isEmpty() && (line[line.size() - 1] == ' ' || line[line.size() - 1] == '\t' ||
                                                   line[line.size() - 1] == '\r'))
                                line = line.left(line.size() - 1);
                        if (line.isEmpty() || line[0] == '#' || line[0] == ';') continue;
                        if (line[0] == '[') {
                                if (line[line.size() - 1] != ']' || line.size() < 3) {
                                        bad(number);
                                        continue;
                                }
                                section = line.mid(1, line.size() - 2);
                                continue;
                        }
                        const size_t eq = line.find('=');
                        if (eq == Text::NoPos || eq == 0) {
                                bad(number);
                                continue;
                        }
                        Text key = line.left(eq), value = line.mid(eq + 1);
                        while (!key.isEmpty() && (key[key.size() - 1] == ' ' || key[key.size() - 1] == '\t'))
                                key = key.left(key.size() - 1);
                        while (!value.isEmpty() && (value[0] == ' ' || value[0] == '\t')) value = value.mid(1);
                        fn(section, key, value, number);
                }
                return;
        }

        /**
         * @brief Reads `networks.ini` into @p out (a network a section, each kept; one without a
         *        host or a nick isn't complete and is said), each problem a line of @p problems
         *        (`networks.ini:12: ...`).
         * @return `Ok` or `NoMem`.
         */
        Error readNetworks(Text text, Vector<NetworkSettings> &out, Vector<String> &problems);

        /** @brief Reads `settings.ini` into @p out, its problems into @p problems. @return `Ok` or `NoMem`. */
        Error readSettings(Text text, AppSettings &out, Vector<String> &problems);

        /** @brief Writes @p networks as `networks.ini` into @p out (read back the same). @return `Ok` or `NoMem`. */
        Error writeNetworks(const Vector<NetworkSettings> &networks, String &out);

        /** @brief Writes @p settings as `settings.ini` into @p out. @return `Ok` or `NoMem`. */
        Error writeSettings(const AppSettings &settings, String &out);

        /** @brief `networks.ini`'s comment, written at its top (the networks follow it). */
        extern const char *const NetworksComment;

        /**
         * @brief Reads `servers.ini` (§7c) into @p out: a section a network, `host` (needed),
         *        `port`, `tls`, `login`, `about`, `offer`; each problem a line of @p problems.
         * @return `Ok` or `NoMem`.
         */
        Error readServers(Text text, Vector<ServerEntry> &out, Vector<String> &problems);

        /**
         * @brief Writes a first `networks.ini` into @p out: the comment, then the entries of
         *        @p servers marked `offer` (I6), each without a nick (so not connecting until one is
         *        set). @return `Ok` or `NoMem`.
         */
        Error firstNetworks(const Vector<ServerEntry> &servers, String &out);

        /** @brief Makes @p entry a network's settings in @p out (no nick; autoconnect off). @return `Ok` or `NoMem`. */
        Error networkFrom(const ServerEntry &entry, NetworkSettings &out);

} // namespace irc
