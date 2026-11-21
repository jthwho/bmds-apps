/**
 * @file      setup.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ioman {

        /**
         * @brief One pin's entry in BMDS's `io/gpio` (BMDS's devplan/io.md §10.1): the words
         *        the editor knows as fields, and the rest (`func=`, `out:`, `in:`, `drive=`) kept
         *        as they were written, in order.
         */
        struct PinSetup {
                        /** @brief Its direction; `None`: the pin has no entry. */
                        enum class Mode : uint8_t {
                                None,
                                In,
                                Out,
                                OpenDrain,
                                Off
                        };
                        enum class Pull : uint8_t {
                                None,
                                Up,
                                Down,
                                Both
                        };
                        enum class Edges : uint8_t {
                                None,
                                Both,
                                Rising,
                                Falling
                        };

                        /** @brief Room for the other words, NUL included. */
                        static constexpr size_t ExtraBytes = 160;

                        Mode     mode = Mode::None;
                        bool     high = false;
                        Pull     pull = Pull::None;
                        Edges    edges = Edges::None;
                        uint32_t debounceUs = 0;
                        bool     force = false;
                        char     extra[ExtraBytes] = {}; ///< The other words, comma-separated.
        };

        /** @brief Room for `entryText()`, NUL included. */
        inline constexpr size_t EntryBytes = 256;

        /**
         * @brief Pad @p pad's entry in @p value (`14=out,high`, `GPIO14=...`), into @p out.
         * @return True if it has one (else @p out is `Mode::None`).
         */
        bool findEntry(const char *value, unsigned pad, PinSetup &out);

        /** @brief @p setup as an entry for pad @p pad: `14=out,high,pull-up`. */
        void entryText(unsigned pad, const PinSetup &setup, char (&out)[EntryBytes]);

        /**
         * @brief @p value with pad @p pad's entry replaced by @p setup, added at the end if it had
         *        none, or taken out (@p setup null or `Mode::None`); every other entry kept as it
         *        was. Into @p out (@p size bytes).
         * @return False if it doesn't fit.
         */
        bool withEntry(const char *value, unsigned pad, const PinSetup *setup, char *out, size_t size);

        /** @brief An edge, as the strip needs it. */
        struct EdgeAt {
                        int64_t timeNs = 0; ///< When (ns since boot).
                        bool    rising = false;
        };

        /**
         * @brief The pin's level in each of @p columns slices of the last @p spanNs before
         *        @p nowNs (the oldest first), from its edges (@p count, the oldest first) and its
         *        level now: after an edge, its way; before the first, the other way; with none,
         *        the level now.
         */
        void levelStrip(bool levelNow, const EdgeAt *edges, size_t count, int64_t nowNs, int64_t spanNs, bool *out,
                        size_t columns);

} // namespace ioman
