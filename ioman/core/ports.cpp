/**
 * @file      ports.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The Ports tab's words (devplan/ioman.md §4): a port's value as fields and back, bytes in hex,
 * i2cdetect's grid.
 */

#include "ports.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ioman {

        namespace {

                constexpr const char *Flows[] = {"none", "rtscts"};
                constexpr const char *Modes[] = {"0", "1", "2", "3"};
                constexpr const char *Bits[] = {"8", "16", "32"};

                constexpr PortField UartFields[] = {
                        {"TX:", "tx", FieldKind::Text, nullptr, 0},
                        {"RX:", "rx", FieldKind::Text, nullptr, 0},
                        {"RTS:", "rts", FieldKind::Text, nullptr, 0},
                        {"CTS:", "cts", FieldKind::Text, nullptr, 0},
                        {"Baud:", "baud", FieldKind::Text, nullptr, 0},
                        {"Format:", "format", FieldKind::Text, nullptr, 0},
                        {"Flow:", "flow", FieldKind::Choice, Flows, 2},
                        {"Loopback", "loopback", FieldKind::Flag, nullptr, 0},
                };

                constexpr PortField SpiFields[] = {
                        {"SCK:", "sck", FieldKind::Text, nullptr, 0},
                        {"MOSI:", "mosi", FieldKind::Text, nullptr, 0},
                        {"MISO:", "miso", FieldKind::Text, nullptr, 0},
                        {"CS:", "cs", FieldKind::Text, nullptr, 0},
                        {"Mode:", "mode", FieldKind::Choice, Modes, 4},
                        {"Clock:", "clock", FieldKind::Text, nullptr, 0},
                        {"Bits:", "bits", FieldKind::Choice, Bits, 3},
                        {"LSB first", "lsb", FieldKind::Flag, nullptr, 0},
                        {"CS active high", "cs-high", FieldKind::Flag, nullptr, 0},
                        {"3-wire", "3wire", FieldKind::Flag, nullptr, 0},
                        {"Loopback", "loopback", FieldKind::Flag, nullptr, 0},
                };

                static_assert(sizeof(UartFields) / sizeof(UartFields[0]) <= MaxFields, "MaxFields");
                static_assert(sizeof(SpiFields) / sizeof(SpiFields[0]) <= MaxFields, "MaxFields");

                // The next word of @p p (spaces between), its length; @p p moved past it.
                const char *nextWord(const char *&p, size_t &length) {
                        while (*p == ' ') ++p;
                        const char *word = p;
                        while (*p != '\0' && *p != ' ') ++p;
                        length = size_t(p - word);
                        return length > 0 ? word : nullptr;
                }

                void append(char *out, size_t size, size_t &n, const char *text, size_t length, bool &fits) {
                        const size_t gap = n > 0 ? 1 : 0;
                        if (n + gap + length + 1 > size) {
                                fits = false;
                                return;
                        }
                        if (gap) out[n++] = ' ';
                        memcpy(out + n, text, length);
                        n += length;
                        out[n] = '\0';
                        return;
                }

                int hexDigit(char c) {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                }

        } // namespace

        const PortField *portFields(PortKind kind, size_t &count) {
                switch (kind) {
                        case PortKind::Uart: count = sizeof(UartFields) / sizeof(UartFields[0]); return UartFields;
                        case PortKind::Spi: count = sizeof(SpiFields) / sizeof(SpiFields[0]); return SpiFields;
                        case PortKind::I2c: break;
                }
                count = 0;
                return nullptr;
        }

        void splitWords(PortKind kind, const char *value, PortWords &out) {
                out = PortWords();
                size_t           count = 0;
                const PortField *fields = portFields(kind, count);
                for (size_t f = 0; f < count; ++f)
                        if (fields[f].kind == FieldKind::Choice)
                                snprintf(out.field[f], FieldBytes, "%s", fields[f].choices[0]);
                size_t      otherLength = 0;
                bool        fits = true;
                size_t      length = 0;
                const char *p = value != nullptr ? value : "";
                while (const char *word = nextWord(p, length)) {
                        const char  *eq = static_cast<const char *>(memchr(word, '=', length));
                        const size_t keyLength = eq != nullptr ? size_t(eq - word) : length;
                        bool         taken = false;
                        for (size_t f = 0; f < count && !taken; ++f) {
                                const PortField &field = fields[f];
                                if (strlen(field.key) != keyLength || memcmp(field.key, word, keyLength) != 0) continue;
                                if (field.kind == FieldKind::Flag) {
                                        if (eq != nullptr) break; // `loopback=1`: not this field's word
                                        snprintf(out.field[f], FieldBytes, "1");
                                        taken = true;
                                        break;
                                }
                                if (eq == nullptr) break;
                                const char  *text = eq + 1;
                                const size_t textLength = length - keyLength - 1;
                                if (field.kind == FieldKind::Choice) {
                                        bool known = false;
                                        for (size_t c = 0; c < field.choiceCount && !known; ++c)
                                                known = strlen(field.choices[c]) == textLength &&
                                                        memcmp(field.choices[c], text, textLength) == 0;
                                        if (!known) break; // kept as written, in the other words
                                }
                                if (textLength + 1 > FieldBytes) break;
                                memcpy(out.field[f], text, textLength);
                                out.field[f][textLength] = '\0';
                                taken = true;
                        }
                        if (!taken) append(out.other, OtherBytes, otherLength, word, length, fits);
                }
                return;
        }

        bool joinWords(PortKind kind, const PortWords &in, char *out, size_t size, size_t &bad) {
                bad = MaxFields;
                if (size == 0) return false;
                out[0] = '\0';
                size_t           count = 0;
                const PortField *fields = portFields(kind, count);
                size_t           n = 0;
                bool             fits = true;
                char             word[FieldBytes + 16];
                for (size_t f = 0; f < count; ++f) {
                        const PortField &field = fields[f];
                        const char      *text = in.field[f];
                        if (strchr(text, ' ') != nullptr) {
                                bad = f;
                                return false;
                        }
                        if (field.kind == FieldKind::Flag) {
                                if (text[0] != '\0') append(out, size, n, field.key, strlen(field.key), fits);
                                continue;
                        }
                        if (text[0] == '\0') continue;
                        if (field.kind == FieldKind::Choice && strcmp(text, field.choices[0]) == 0) continue;
                        const int length = snprintf(word, sizeof(word), "%s=%s", field.key, text);
                        append(out, size, n, word, size_t(length), fits);
                }
                const char *p = in.other;
                size_t      length = 0;
                while (const char *w = nextWord(p, length)) append(out, size, n, w, length, fits);
                return fits;
        }

        bool hasWord(const char *value, const char *word) {
                const char  *p = value != nullptr ? value : "";
                const size_t want = strlen(word);
                size_t       length = 0;
                while (const char *w = nextWord(p, length))
                        if (length == want && memcmp(w, word, length) == 0) return true;
                return false;
        }

        bool parseNumber(const char *text, unsigned long max, unsigned long &out) {
                while (*text == ' ') ++text;
                if (*text == '\0' || *text == '-' || *text == '+') return false;
                char         *end = nullptr;
                unsigned long v = strtoul(text, &end, 0);
                while (end != nullptr && *end == ' ') ++end;
                if (end == text || end == nullptr || *end != '\0' || v > max) return false;
                out = v;
                return true;
        }

        long parseBytes(const char *text, uint8_t *out, size_t max) {
                size_t n = 0;
                for (const char *p = text; *p != '\0';) {
                        if (*p == ' ' || *p == ',' || *p == ':') {
                                ++p;
                                continue;
                        }
                        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
                        // A run of hex digits: two to a byte.
                        const char *run = p;
                        while (hexDigit(*p) >= 0) ++p;
                        const size_t digits = size_t(p - run);
                        if (digits == 0 || (*p != '\0' && *p != ' ' && *p != ',' && *p != ':')) return -1;
                        size_t i = 0;
                        if (digits % 2 == 1) { // `0xa`, `a`: one digit makes a byte
                                if (n == max) return -1;
                                out[n++] = uint8_t(hexDigit(run[0]));
                                i = 1;
                        }
                        for (; i < digits; i += 2) {
                                if (n == max) return -1;
                                out[n++] = uint8_t(hexDigit(run[i]) << 4 | hexDigit(run[i + 1]));
                        }
                }
                return long(n);
        }

        size_t bytesText(const uint8_t *bytes, size_t count, size_t indent, char *out, size_t size) {
                if (size == 0) return 0;
                size_t n = 0;
                out[0] = '\0';
                for (size_t i = 0; i < count && n + 4 < size; ++i) {
                        if (i > 0 && i % 16 == 0) {
                                n += size_t(snprintf(out + n, size - n, "\n%*s", int(indent), ""));
                                if (n >= size) return size - 1;
                        } else if (i > 0) {
                                out[n++] = ' ';
                        }
                        n += size_t(snprintf(out + n, size - n, "%02x", bytes[i]));
                }
                return n < size ? n : size - 1;
        }

        size_t scanGrid(const ScanResult *results, char *out, size_t size) {
                if (size == 0) return 0;
                size_t     n = 0;
                const auto put = [&](const char *text) {
                        if (n < size) n += size_t(snprintf(out + n, size - n, "%s", text));
                };
                put("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
                char cell[8];
                for (unsigned row = 0; row < 0x80; row += 0x10) {
                        snprintf(cell, sizeof(cell), "%02x:", row);
                        put(cell);
                        for (unsigned a = row; a < row + 0x10; ++a) {
                                switch (results[a]) {
                                        case ScanResult::Skipped: put("   "); break;
                                        case ScanResult::None: put(" --"); break;
                                        case ScanResult::Driver: put(" UU"); break;
                                        case ScanResult::Found:
                                                snprintf(cell, sizeof(cell), " %02x", a);
                                                put(cell);
                                                break;
                                }
                        }
                        put("\n");
                }
                return n < size ? n : size - 1;
        }

} // namespace ioman
