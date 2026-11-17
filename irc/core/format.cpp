/**
 * @file      format.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "format.h"

namespace irc {

        namespace Format {

                namespace {

                        bool isDigit(char c) {
                                return c >= '0' && c <= '9';
                        }

                        bool isHex(char c) {
                                return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                        }

                        // @p count characters of @p kind at @p at in @p text.
                        template <typename F> bool run(Text text, size_t at, size_t count, F kind) {
                                if (at + count > text.size()) return false;
                                for (size_t i = 0; i < count; ++i)
                                        if (!kind(text[at + i])) return false;
                                return true;
                        }

                        // A character a nick may have (RFC 2812's, and what servers allow).
                        bool isNickChar(char c) {
                                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c) ||
                                       Text("[]\\`_^{|}-").find(c) != Text::NoPos;
                        }

                        // Days from 1970-01-01 to the date (Howard Hinnant's days_from_civil).
                        int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
                                y -= m <= 2;
                                const int64_t  era = (y >= 0 ? y : y - 399) / 400;
                                const unsigned yoe = unsigned(y - era * 400);
                                const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
                                const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
                                return era * 146097 + int64_t(doe) - 719468;
                        }

                        // @p count digits at @p at as a number, or -1.
                        int64_t number(Text text, size_t at, size_t count) {
                                if (!run(text, at, count, isDigit)) return -1;
                                int64_t n = 0;
                                for (size_t i = 0; i < count; ++i) n = n * 10 + (text[at + i] - '0');
                                return n;
                        }

                } // namespace

                Error strip(Text text, String &out) {
                        out.clear();
                        size_t i = 0;
                        while (i < text.size()) {
                                const char c = text[i];
                                if (c == '\x02' || c == '\x1d' || c == '\x1f' || c == '\x1e' || c == '\x11' ||
                                    c == '\x16' || c == '\x0f') {
                                        ++i;
                                        continue;
                                }
                                if (c == '\x03') {
                                        // FG (one or two digits), then ,BG only after a FG.
                                        ++i;
                                        size_t n = 0;
                                        while (n < 2 && i < text.size() && isDigit(text[i])) ++i, ++n;
                                        if (n > 0 && i + 1 < text.size() && text[i] == ',' && isDigit(text[i + 1])) {
                                                i += 2;
                                                if (i < text.size() && isDigit(text[i])) ++i;
                                        }
                                        continue;
                                }
                                if (c == '\x04') {
                                        ++i;
                                        if (run(text, i, 6, isHex)) {
                                                i += 6;
                                                if (i < text.size() && text[i] == ',' && run(text, i + 1, 6, isHex))
                                                        i += 7;
                                        }
                                        continue;
                                }
                                // Up to the next code at once.
                                size_t end = i + 1;
                                while (end < text.size()) {
                                        const char e = text[end];
                                        if (e == '\x02' || e == '\x1d' || e == '\x1f' || e == '\x1e' || e == '\x11' ||
                                            e == '\x16' || e == '\x0f' || e == '\x03' || e == '\x04')
                                                break;
                                        ++end;
                                }
                                if (out.append(text.mid(i, end - i)) != Error::Ok) return Error::NoMem;
                                i = end;
                        }
                        return Error::Ok;
                }

                bool isUtf8(Text text) {
                        size_t i = 0;
                        while (i < text.size()) {
                                const uint8_t c = uint8_t(text[i]);
                                if (c < 0x80) {
                                        ++i;
                                        continue;
                                }
                                size_t   n = 0;
                                uint32_t cp = 0, least = 0;
                                if ((c & 0xe0) == 0xc0) {
                                        n = 1, cp = c & 0x1f, least = 0x80;
                                } else if ((c & 0xf0) == 0xe0) {
                                        n = 2, cp = c & 0x0f, least = 0x800;
                                } else if ((c & 0xf8) == 0xf0) {
                                        n = 3, cp = c & 0x07, least = 0x10000;
                                } else {
                                        return false;
                                }
                                if (i + n >= text.size()) return false; // cut short
                                for (size_t k = 1; k <= n; ++k) {
                                        const uint8_t b = uint8_t(text[i + k]);
                                        if ((b & 0xc0) != 0x80) return false;
                                        cp = (cp << 6) | (b & 0x3f);
                                }
                                if (cp < least || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
                                i += n + 1;
                        }
                        return true;
                }

                Error toUtf8(Text text, String &out) {
                        out.clear();
                        const bool utf8 = isUtf8(text);
                        for (size_t i = 0; i < text.size(); ++i) {
                                const uint8_t c = uint8_t(text[i]);
                                if (c == 0) continue;
                                Error err;
                                if (utf8 || c < 0x80) {
                                        err = out.append(char(c));
                                } else {
                                        const char two[2] = {char(0xc0 | (c >> 6)), char(0x80 | (c & 0x3f))};
                                        err = out.append(Text(two, 2));
                                }
                                if (err != Error::Ok) return err;
                        }
                        return Error::Ok;
                }

                bool parseCtcp(Text text, Ctcp &out) {
                        if (text.isEmpty() || text[0] != '\x01') return false;
                        Text body = text.mid(1);
                        if (!body.isEmpty() && body[body.size() - 1] == '\x01') body = body.left(body.size() - 1);
                        const size_t space = body.find(' ');
                        out.command = space == Text::NoPos ? body : body.left(space);
                        out.args = space == Text::NoPos ? Text() : body.mid(space + 1);
                        return !out.command.isEmpty();
                }

                bool mentions(Text text, Text nick, CaseMapping mapping) {
                        if (nick.isEmpty()) return false;
                        String plain;
                        if (strip(text, plain) != Error::Ok) plain.clear();
                        const Text t = plain.isEmpty() ? text : plain.view();
                        for (size_t i = 0; i + nick.size() <= t.size(); ++i) {
                                if (i > 0 && isNickChar(t[i - 1])) continue;
                                if (i + nick.size() < t.size() && isNickChar(t[i + nick.size()])) continue;
                                if (sameName(t.mid(i, nick.size()), nick, mapping)) return true;
                        }
                        return false;
                }

                bool parseServerTime(Text time, int64_t &ms) {
                        // YYYY-MM-DDTHH:MM:SS[.fff...]Z
                        if (time.size() < 20 || time[4] != '-' || time[7] != '-' || time[10] != 'T' ||
                            time[13] != ':' || time[16] != ':')
                                return false;
                        const int64_t y = number(time, 0, 4), mo = number(time, 5, 2), d = number(time, 8, 2);
                        const int64_t h = number(time, 11, 2), mi = number(time, 14, 2), s = number(time, 17, 2);
                        if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 ||
                            s < 0 || s > 60)
                                return false;
                        size_t  at = 19;
                        int64_t fraction = 0;
                        if (time[at] == '.') {
                                ++at;
                                int64_t scale = 100;
                                while (at < time.size() && isDigit(time[at])) {
                                        fraction += (time[at] - '0') * scale;
                                        scale /= 10;
                                        ++at;
                                }
                        }
                        if (at + 1 != time.size() || time[at] != 'Z') return false;
                        const int64_t days = daysFromCivil(y, unsigned(mo), unsigned(d));
                        ms = ((days * 24 + h) * 60 + mi) * 60000 + s * 1000 + fraction;
                        return true;
                }

                Error base64(Text bytes, String &out) {
                        static const char alphabet[] =
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        out.clear();
                        for (size_t i = 0; i < bytes.size(); i += 3) {
                                const uint32_t b0 = uint8_t(bytes[i]);
                                const uint32_t b1 = i + 1 < bytes.size() ? uint8_t(bytes[i + 1]) : 0;
                                const uint32_t b2 = i + 2 < bytes.size() ? uint8_t(bytes[i + 2]) : 0;
                                const uint32_t v = b0 << 16 | b1 << 8 | b2;
                                const char     quad[4] = {alphabet[v >> 18 & 63], alphabet[v >> 12 & 63],
                                                      i + 1 < bytes.size() ? alphabet[v >> 6 & 63] : '=',
                                                      i + 2 < bytes.size() ? alphabet[v & 63] : '='};
                                if (out.append(Text(quad, 4)) != Error::Ok) return Error::NoMem;
                        }
                        return Error::Ok;
                }


                LocalTime localTime(int64_t utcMs, int32_t offsetMinutes) {
                        // Floor division: times before 1970 too.
                        const int64_t ms = utcMs + int64_t(offsetMinutes) * 60000;
                        int64_t       days = ms / 86400000;
                        int64_t       rest = ms % 86400000;
                        if (rest < 0) {
                                rest += 86400000;
                                --days;
                        }
                        // Howard Hinnant's civil_from_days.
                        const int64_t  z = days + 719468;
                        const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
                        const unsigned doe = unsigned(z - era * 146097);
                        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
                        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
                        const unsigned mp = (5 * doy + 2) / 153;
                        LocalTime      t;
                        t.day = int(doy - (153 * mp + 2) / 5 + 1);
                        t.month = int(mp < 10 ? mp + 3 : mp - 9);
                        t.year = int(int64_t(yoe) + era * 400 + (t.month <= 2));
                        t.hour = int(rest / 3600000);
                        t.minute = int(rest / 60000 % 60);
                        t.second = int(rest / 1000 % 60);
                        return t;
                }

                uint8_t nickColour(Text nick, CaseMapping mapping) {
                        // mIRC's dark ones: navy, green, red, brown, purple, orange, teal, blue, pink.
                        static const uint8_t colours[] = {2, 3, 4, 5, 6, 7, 10, 12, 13};
                        uint32_t             h = 2166136261u; // FNV-1a over the folded nick
                        for (char c : nick) {
                                h ^= uint8_t(foldCase(c, mapping));
                                h *= 16777619u;
                        }
                        return colours[h % sizeof(colours)];
                }

                bool hideSecret(Text target, Text text, String &out) {
                        out.clear();
                        if (!target.equalsIgnoringCase("NickServ")) return false;
                        // The words, the separators kept as they were.
                        size_t starts[8], ends[8], n = 0, at = 0;
                        while (at < text.size() && n < 8) {
                                while (at < text.size() && text[at] == ' ') ++at;
                                if (at == text.size()) break;
                                starts[n] = at;
                                while (at < text.size() && text[at] != ' ') ++at;
                                ends[n++] = at;
                        }
                        if (n < 2) return false;
                        const Text command = text.mid(starts[0], ends[0] - starts[0]);
                        size_t     hidden = n; // the word hidden (n: none)
                        if (command.equalsIgnoringCase("IDENTIFY")) {
                                hidden = n == 2 ? 1 : 2;
                        } else if (command.equalsIgnoringCase("REGISTER")) {
                                hidden = 1;
                        } else if (command.equalsIgnoringCase("GHOST") || command.equalsIgnoringCase("REGAIN") ||
                                   command.equalsIgnoringCase("RECOVER")) {
                                hidden = n >= 3 ? 2 : n;
                        } else if (command.equalsIgnoringCase("SET") && n >= 3 &&
                                   text.mid(starts[1], ends[1] - starts[1]).equalsIgnoringCase("PASSWORD")) {
                                hidden = 2;
                        }
                        if (hidden >= n) return false;
                        // The word replaced, the rest as it was.
                        if (out.append(text.left(starts[hidden])) != Error::Ok || out.append("****") != Error::Ok ||
                            out.append(text.mid(ends[hidden])) != Error::Ok)
                                return false;
                        return true;
                }

        } // namespace Format

} // namespace irc
