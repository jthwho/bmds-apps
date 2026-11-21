/**
 * @file      setup.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * One pin's entry in BMDS's io/gpio, read and written back (devplan/ioman.md §3).
 */

#include "setup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ioman {

        namespace {

                bool same(const char *a, size_t n, const char *word) {
                        return std::strlen(word) == n && std::strncmp(a, word, n) == 0;
                }

                bool startsWith(const char *a, size_t n, const char *prefix) {
                        const size_t k = std::strlen(prefix);
                        return n >= k && std::strncmp(a, prefix, k) == 0;
                }

                // The pad an entry names (`14`, `GPIO14`, `gpio14`), or -1.
                int padOf(const char *text, size_t n) {
                        if (n > 4 && (std::strncmp(text, "GPIO", 4) == 0 || std::strncmp(text, "gpio", 4) == 0)) {
                                text += 4;
                                n -= 4;
                        }
                        if (n == 0 || n > 3) return -1;
                        int v = 0;
                        for (size_t i = 0; i < n; ++i) {
                                if (text[i] < '0' || text[i] > '9') return -1;
                                v = v * 10 + (text[i] - '0');
                        }
                        return v;
                }

                // Calls fn(entry, length, pad) for each entry of @p value.
                template <typename F> void eachEntry(const char *value, F fn) {
                        const char *p = value != nullptr ? value : "";
                        while (*p != '\0') {
                                while (*p == ' ') ++p;
                                const char *start = p;
                                while (*p != '\0' && *p != ' ') ++p;
                                const size_t n = size_t(p - start);
                                if (n == 0) continue;
                                const char *eq = static_cast<const char *>(std::memchr(start, '=', n));
                                fn(start, n, eq != nullptr ? padOf(start, size_t(eq - start)) : -1);
                        }
                        return;
                }

                void appendWord(char (&out)[PinSetup::ExtraBytes], const char *w, size_t n) {
                        size_t at = std::strlen(out);
                        if (at + (at > 0 ? 1 : 0) + n + 1 > sizeof(out)) return;
                        if (at > 0) out[at++] = ',';
                        std::memcpy(out + at, w, n);
                        out[at + n] = '\0';
                        return;
                }

        } // namespace

        bool findEntry(const char *value, unsigned pad, PinSetup &out) {
                out = PinSetup();
                bool found = false;
                eachEntry(value, [&](const char *e, size_t n, int p) {
                        if (found || p != int(pad)) return;
                        found = true;
                        out.mode = PinSetup::Mode::In; // the default direction
                        const char *w = static_cast<const char *>(std::memchr(e, '=', n)) + 1;
                        const char *end = e + n;
                        while (w < end) {
                                const char  *c = static_cast<const char *>(std::memchr(w, ',', size_t(end - w)));
                                const size_t k = size_t((c != nullptr ? c : end) - w);
                                if (same(w, k, "in"))
                                        out.mode = PinSetup::Mode::In;
                                else if (same(w, k, "out"))
                                        out.mode = PinSetup::Mode::Out;
                                else if (same(w, k, "od"))
                                        out.mode = PinSetup::Mode::OpenDrain;
                                else if (same(w, k, "off"))
                                        out.mode = PinSetup::Mode::Off;
                                else if (same(w, k, "high") || same(w, k, "1"))
                                        out.high = true;
                                else if (same(w, k, "low") || same(w, k, "0"))
                                        out.high = false;
                                else if (same(w, k, "pull-up"))
                                        out.pull = out.pull == PinSetup::Pull::Down ? PinSetup::Pull::Both
                                                                                    : PinSetup::Pull::Up;
                                else if (same(w, k, "pull-down"))
                                        out.pull = out.pull == PinSetup::Pull::Up ? PinSetup::Pull::Both
                                                                                  : PinSetup::Pull::Down;
                                else if (same(w, k, "edges"))
                                        out.edges = PinSetup::Edges::Both;
                                else if (same(w, k, "rising"))
                                        out.edges = out.edges == PinSetup::Edges::Falling ? PinSetup::Edges::Both
                                                                                          : PinSetup::Edges::Rising;
                                else if (same(w, k, "falling"))
                                        out.edges = out.edges == PinSetup::Edges::Rising ? PinSetup::Edges::Both
                                                                                         : PinSetup::Edges::Falling;
                                else if (startsWith(w, k, "debounce="))
                                        out.debounceUs = uint32_t(std::strtoul(w + 9, nullptr, 10));
                                else if (same(w, k, "force"))
                                        out.force = true;
                                else if (k > 0)
                                        appendWord(out.extra, w, k);
                                w += k + 1;
                        }
                });
                return found;
        }

        void entryText(unsigned pad, const PinSetup &s, char (&out)[EntryBytes]) {
                static constexpr const char *Modes[] = {"in", "in", "out", "od", "off"};
                size_t     n = size_t(std::snprintf(out, sizeof(out), "%u=%s", pad, Modes[size_t(s.mode)]));
                const auto add = [&](const char *word) {
                        if (n < sizeof(out)) n += size_t(std::snprintf(out + n, sizeof(out) - n, ",%s", word));
                };
                if (s.mode == PinSetup::Mode::Out || s.mode == PinSetup::Mode::OpenDrain) add(s.high ? "high" : "low");
                if (s.pull == PinSetup::Pull::Up || s.pull == PinSetup::Pull::Both) add("pull-up");
                if (s.pull == PinSetup::Pull::Down || s.pull == PinSetup::Pull::Both) add("pull-down");
                if (s.edges == PinSetup::Edges::Both) add("edges");
                if (s.edges == PinSetup::Edges::Rising) add("rising");
                if (s.edges == PinSetup::Edges::Falling) add("falling");
                if (s.edges != PinSetup::Edges::None && s.debounceUs != 0) {
                        char d[24];
                        std::snprintf(d, sizeof(d), "debounce=%u", unsigned(s.debounceUs));
                        add(d);
                }
                if (s.extra[0] != '\0') add(s.extra);
                if (s.force) add("force");
                if (n >= sizeof(out)) out[sizeof(out) - 1] = '\0';
                return;
        }

        bool withEntry(const char *value, unsigned pad, const PinSetup *setup, char *out, size_t size) {
                if (size == 0) return false;
                size_t     n = 0;
                bool       fits = true, placed = false;
                char       entry[EntryBytes] = {};
                const bool keep = setup != nullptr && setup->mode != PinSetup::Mode::None;
                if (keep) entryText(pad, *setup, entry);
                const auto put = [&](const char *text, size_t k) {
                        if (n + (n > 0 ? 1 : 0) + k + 1 > size) {
                                fits = false;
                                return;
                        }
                        if (n > 0) out[n++] = ' ';
                        std::memcpy(out + n, text, k);
                        n += k;
                };
                eachEntry(value, [&](const char *e, size_t k, int p) {
                        if (p != int(pad)) {
                                put(e, k);
                        } else if (keep && !placed) {
                                put(entry, std::strlen(entry)); // in the old one's place
                                placed = true;
                        }
                });
                if (keep && !placed) put(entry, std::strlen(entry));
                out[n] = '\0';
                return fits;
        }

        void levelStrip(bool levelNow, const EdgeAt *edges, size_t count, int64_t nowNs, int64_t spanNs, bool *out,
                        size_t columns) {
                for (size_t c = 0; c < columns; ++c) {
                        // The slice's end: the level then is that of the last edge before it.
                        const int64_t at = nowNs - spanNs + int64_t(c + 1) * spanNs / int64_t(columns);
                        bool          level = count > 0 ? !edges[0].rising : levelNow;
                        for (size_t e = 0; e < count && edges[e].timeNs <= at; ++e) level = edges[e].rising;
                        out[c] = level;
                }
                return;
        }

} // namespace ioman
