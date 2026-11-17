/**
 * @file      keysym.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>

namespace vnc {

        /** @brief HID's modifier bits (a keyboard report's first byte), as BMDS's `KeyEvent` has them. */
        namespace Modifier {
                inline constexpr uint8_t LeftCtrl = 0x01;
                inline constexpr uint8_t LeftShift = 0x02;
                inline constexpr uint8_t LeftAlt = 0x04;
                inline constexpr uint8_t LeftGui = 0x08;
                inline constexpr uint8_t RightCtrl = 0x10;
                inline constexpr uint8_t RightShift = 0x20;
                inline constexpr uint8_t RightAlt = 0x40;
                inline constexpr uint8_t RightGui = 0x80;
        } // namespace Modifier

        /** @brief An X keysym as a key on a US keyboard. */
        struct KeyMapping {
                        uint8_t usage = 0;     ///< HID's usage (keyboard page); 0: none.
                        bool    shift = false; ///< Typed with Shift (a capital, `!`).
                        uint8_t modifier = 0;  ///< A modifier key's own bit (`Modifier`), else 0.
        };

        /**
         * @brief Maps @p keysym (what a VNC viewer sends, RFB 3.8 §7.5.4) to the key that types it
         *        on a US layout (devplan/vnc.md §5c): Latin-1's printable characters, the editing
         *        and function keys, the keypad and the modifiers. Characters with no US key map to
         *        nothing (usage 0).
         */
        KeyMapping mapKeysym(uint32_t keysym);

        /**
         * @brief A viewer's keys as BMDS's: the modifiers held tracked from the viewer's own
         *        modifier keys, so a chord arrives as the local one would, and a character typed
         *        with Shift given it if the viewer didn't send Shift itself.
         */
        class KeyState {
                public:
                        /** @brief What to inject for one key event. */
                        struct Key {
                                        uint8_t usage = 0;     ///< 0: nothing to inject.
                                        uint8_t modifiers = 0; ///< Held with it.
                                        bool    down = false;
                        };

                        /** @brief The viewer's key @p keysym went @p down. */
                        Key event(bool down, uint32_t keysym);

                        /** @brief The modifiers held now. */
                        uint8_t modifiers() const { return _modifiers; }

                private:
                        uint8_t _modifiers = 0;
        };

} // namespace vnc
