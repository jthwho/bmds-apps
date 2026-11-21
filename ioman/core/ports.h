/**
 * @file      ports.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ioman {

        /** @brief The ports the Ports tab knows (devplan/ioman.md §4). */
        enum class PortKind : uint8_t {
                Uart, ///< `io/uartN`, `/dev/ttySN`.
                Spi,  ///< `io/spiN`, `/dev/spidevN.C`.
                I2c   ///< A system I²C bus, `/dev/i2c-N` (no setup).
        };

        /** @brief How a field of a port's setup is written in its value. */
        enum class FieldKind : uint8_t {
                Text,   ///< `key=text`, left out when empty.
                Choice, ///< `key=choice`, left out when the first (the kernel's default).
                Flag    ///< The word `key`, there or not.
        };

        /** @brief A field of a port's setup. */
        struct PortField {
                        const char        *label;       ///< `TX:`
                        const char        *key;         ///< `tx`
                        FieldKind          kind;        ///< How it is written.
                        const char *const *choices;     ///< A `Choice`'s, the default first.
                        size_t             choiceCount; ///< How many.
        };

        inline constexpr size_t MaxFields = 12;   ///< A kind's fields at most.
        inline constexpr size_t FieldBytes = 40;  ///< A field's text, NUL included.
        inline constexpr size_t OtherBytes = 160; ///< The other words, NUL included.

        /** @brief @p kind's fields (none for `I2c`), @p count of them. */
        const PortField *portFields(PortKind kind, size_t &count);

        /**
         * @brief A value as its fields: a `Text`'s text, a `Choice`'s choice (the first when its
         *        word is missing), a `Flag`'s "1" or ""; every other word in `other`, in order.
         */
        struct PortWords {
                        char field[MaxFields][FieldBytes] = {};
                        char other[OtherBytes] = {};
        };

        /** @brief @p value (`tx=37 rx=38 force`) split into @p kind's fields. */
        void splitWords(PortKind kind, const char *value, PortWords &out);

        /**
         * @brief @p in joined back into a value: the fields in order, then the other words.
         * @param[out] bad The field that can't be written (a space in it), or `MaxFields`.
         * @return False if a field has a space in it, or it doesn't fit in @p size.
         */
        bool joinWords(PortKind kind, const PortWords &in, char *out, size_t size, size_t &bad);

        /** @brief True if @p value has the word @p word. */
        bool hasWord(const char *value, const char *word);

        /**
         * @brief A number as people write it (`0x50`, `80`, `0b1010` not), into @p out.
         * @return False if it isn't one, or is past @p max.
         */
        bool parseNumber(const char *text, unsigned long max, unsigned long &out);

        /**
         * @brief Bytes in hex (`de ad`, `dead`, `0xde,0xad`, `DE:AD`) into @p out.
         * @return How many, or -1 if it isn't hex bytes or there are more than @p max.
         */
        long parseBytes(const char *text, uint8_t *out, size_t max);

        /**
         * @brief @p count bytes as `de ad be ef`, sixteen a line, each line after the first
         *        indented by @p indent spaces.
         * @return The length written (cut to @p size).
         */
        size_t bytesText(const uint8_t *bytes, size_t count, size_t indent, char *out, size_t size);

        /** @brief What an address did in a scan. */
        enum class ScanResult : uint8_t {
                Skipped, ///< Not tried (the reserved ranges).
                None,    ///< Nothing answered.
                Found,   ///< Something acknowledged it.
                Driver   ///< A BMDS driver's (not tried).
        };

        /** @brief `i2cdetect`'s grid of @p results (128 of them). @return The length written. */
        size_t scanGrid(const ScanResult *results, char *out, size_t size);

} // namespace ioman
