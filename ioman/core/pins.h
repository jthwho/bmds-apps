/**
 * @file      pins.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ioman {

        /** @brief Who may change a pad (BMDS's `BMDS_GPIO_CLASS_...`, in the same order). */
        enum class PinClass : uint8_t {
                Free,   ///< Nobody holds it, and the board leaves it to the user.
                Fixed,  ///< The board wires it to something a change would break.
                System, ///< A driver holds it, or the board wires it to an on-board device.
                User,   ///< The user holds it.
        };

        /** @brief The number of classes. */
        inline constexpr size_t PinClassCount = 4;

        /** @brief The class's name, lower case (`free`, `fixed`, `system`, `user`). */
        const char *pinClassName(PinClass c);

        /**
         * @brief One pad as the system says it (`gpio_ctl`'s `BMDS_GPIO_PINS`), the fields the
         *        window shows: what the program copies out of `bmds_gpio_pin_t`.
         */
        struct Pin {
                        uint8_t  offset = 0; ///< Its GPIO number.
                        PinClass pinClass = PinClass::Free;
                        char     level = '-';        ///< `1`, `0`, `-` (none), `?` (the LP domain's).
                        char     name[8] = {};       ///< `GPIO14`.
                        char     owner[16] = {};     ///< Who holds it, or "".
                        char     use[32] = {};       ///< The holder's use of it, else the board's label, or "".
                        char     route[48] = {};     ///< `GPIO`, `UART0_TXD (IO_MUX)`, `I2C0_SDA`, `-`.
                        char     direction[12] = {}; ///< `in`, `out`, `in/out`, `periph`, `-` (`,od`).
                        char     pull[8] = {};       ///< `up`, `down`, `up+down`, `-`.
                        char     where[16] = {};     ///< The connector pin it is brought out to (`UEXT.3`), or "".
                        bool     peripheral = false; ///< A peripheral drives it (`BMDS_GPIO_PIN_PERIPHERAL`).
                        bool     watched = false;    ///< Its edges are watched (`BMDS_GPIO_PIN_EDGES`).
        };

        /** @brief Room for `ownerText()`'s text, NUL included. */
        inline constexpr size_t OwnerBytes = 16 + 32 + 2;

        /** @brief Room for `directionText()`'s text, NUL included. */
        inline constexpr size_t DirectionBytes = 12 + 8 + 2;

        /**
         * @brief The Owner column: `eth0: RMII TXD1`, `eth0`, the board's label for a pad
         *        nobody holds, or "".
         */
        void ownerText(const Pin &pin, char (&out)[OwnerBytes]);

        /** @brief The Direction column: the direction, its pull after it (`in up`, `out`, `-`). */
        void directionText(const Pin &pin, char (&out)[DirectionBytes]);

        /**
         * @brief Which swatch shows the pad: its class (0-3) times 3, plus its level (0 high,
         *        1 low, 2 none to show), 0 to 11. A pad a peripheral drives shows none: a
         *        running clock or data line sampled ten times a second is noise, not a level.
         */
        unsigned swatchOf(const Pin &pin);

        /** @brief The number of `swatchOf()`'s swatches. */
        inline constexpr unsigned SwatchCount = 12;

        /**
         * @brief A hash of a pad's words (all but its level, which `swatchOf()` shows): a
         *        refresh whose words didn't change rebuilds nothing.
         */
        uint64_t pinHash(const Pin &pin);

        /**
         * @brief The Pins tab's rows (devplan/ioman.md): which pads show, their order, and each
         *        class's count.
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class PinList {
                public:
                        /** @brief Which pads are shown. */
                        enum class Show : uint8_t {
                                All,       ///< Every pad.
                                Free,      ///< `PinClass::Free`.
                                User,      ///< `PinClass::User`.
                                System,    ///< `PinClass::System`.
                                Fixed,     ///< `PinClass::Fixed`.
                                Connector, ///< On the connector `setShow()` names.
                        };

                        /** @brief The columns, in order. */
                        enum Column : size_t {
                                Gpio,
                                Class,
                                Owner,
                                Route,
                                Direction,
                                Where,
                                ColumnCount,
                        };

                        /** @brief Most pads. */
                        static constexpr size_t MaxPins = 64;

                        /**
                         * @brief Shows the pads @p show says (with `Show::Connector`, those on
                         *        @p connector: its name, kept by the caller).
                         */
                        void setShow(Show show, const char *connector = "");

                        /** @brief Sorts by @p column, @p descending or not; ties by pad number, up. */
                        void setSort(size_t column, bool descending);

                        /** @brief Returns the column sorted by. */
                        size_t sortColumn() const { return _sortColumn; }

                        /** @brief Returns whether the sort is descending. */
                        bool isDescending() const { return _descending; }

                        /**
                         * @brief Takes @p pins (@p count, every pad's, kept by the caller until
                         *        the next call; `MaxPins` at most) and orders the shown ones.
                         */
                        void update(const Pin *pins, size_t count);

                        /** @brief Returns the rows shown. */
                        size_t count() const { return _count; }

                        /** @brief Returns shown row @p index. */
                        const Pin &at(size_t index) const { return _pins[_order[index]]; }

                        /** @brief Returns the row showing pad @p offset, or -1. */
                        int rowOf(unsigned offset) const;

                        /** @brief Returns the pads of class @p c among all (shown or not). */
                        size_t classCount(PinClass c) const { return _classes[size_t(c)]; }

                        /** @brief Returns every pad's count (shown or not). */
                        size_t total() const { return _total; }

                private:
                        bool shows(const Pin &pin) const;
                        bool less(const Pin &a, const Pin &b) const;

                        const Pin  *_pins = nullptr;
                        size_t      _total = 0;
                        uint8_t     _order[MaxPins] = {};
                        size_t      _count = 0;
                        size_t      _classes[PinClassCount] = {};
                        const char *_connector = "";
                        size_t      _sortColumn = Gpio;
                        Show        _show = Show::All;
                        bool        _descending = false;
        };

} // namespace ioman
