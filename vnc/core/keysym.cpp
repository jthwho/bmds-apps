/**
 * @file      keysym.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * X keysyms (X11's keysymdef.h) as HID usages (USB HID Usage Tables §10) on a US layout.
 */

#include "keysym.h"

namespace vnc {

        namespace {

                struct Entry {
                                uint32_t keysym;
                                uint8_t  usage;
                                bool     shift;
                };

                // ASCII's printable characters by their key on a US keyboard (0x20-0x7e).
                constexpr Entry Ascii[] = {
                        {' ', 0x2c, false},  {'!', 0x1e, true},  {'"', 0x34, true},  {'#', 0x20, true},
                        {'$', 0x21, true},   {'%', 0x22, true},  {'&', 0x24, true},  {'\'', 0x34, false},
                        {'(', 0x26, true},   {')', 0x27, true},  {'*', 0x25, true},  {'+', 0x2e, true},
                        {',', 0x36, false},  {'-', 0x2d, false}, {'.', 0x37, false}, {'/', 0x38, false},
                        {':', 0x33, true},   {';', 0x33, false}, {'<', 0x36, true},  {'=', 0x2e, false},
                        {'>', 0x37, true},   {'?', 0x38, true},  {'@', 0x1f, true},  {'[', 0x2f, false},
                        {'\\', 0x31, false}, {']', 0x30, false}, {'^', 0x23, true},  {'_', 0x2d, true},
                        {'`', 0x35, false},  {'{', 0x2f, true},  {'|', 0x31, true},  {'}', 0x30, true},
                        {'~', 0x35, true},
                };

                // The X keysyms of keys that aren't characters (0xff00-0xffff).
                constexpr Entry Function[] = {
                        {0xff08, 0x2a, false}, // BackSpace
                        {0xff09, 0x2b, false}, // Tab
                        {0xfe20, 0x2b, true},  // ISO_Left_Tab (Shift+Tab)
                        {0xff0d, 0x28, false}, // Return
                        {0xff13, 0x48, false}, // Pause
                        {0xff14, 0x47, false}, // Scroll_Lock
                        {0xff15, 0x46, false}, // Sys_Req
                        {0xff1b, 0x29, false}, // Escape
                        {0xff50, 0x4a, false}, // Home
                        {0xff51, 0x50, false}, // Left
                        {0xff52, 0x52, false}, // Up
                        {0xff53, 0x4f, false}, // Right
                        {0xff54, 0x51, false}, // Down
                        {0xff55, 0x4b, false}, // Prior (Page Up)
                        {0xff56, 0x4e, false}, // Next (Page Down)
                        {0xff57, 0x4d, false}, // End
                        {0xff61, 0x46, false}, // Print
                        {0xff63, 0x49, false}, // Insert
                        {0xff67, 0x65, false}, // Menu
                        {0xff7f, 0x53, false}, // Num_Lock
                        {0xff8d, 0x58, false}, // KP_Enter
                        {0xff95, 0x5f, false}, // KP_Home (7)
                        {0xff96, 0x5c, false}, // KP_Left (4)
                        {0xff97, 0x60, false}, // KP_Up (8)
                        {0xff98, 0x5e, false}, // KP_Right (6)
                        {0xff99, 0x5a, false}, // KP_Down (2)
                        {0xff9a, 0x61, false}, // KP_Prior (9)
                        {0xff9b, 0x5b, false}, // KP_Next (3)
                        {0xff9c, 0x59, false}, // KP_End (1)
                        {0xff9d, 0x5d, false}, // KP_Begin (5)
                        {0xff9e, 0x62, false}, // KP_Insert (0)
                        {0xff9f, 0x63, false}, // KP_Delete (.)
                        {0xffaa, 0x55, false}, // KP_Multiply
                        {0xffab, 0x57, false}, // KP_Add
                        {0xffad, 0x56, false}, // KP_Subtract
                        {0xffae, 0x63, false}, // KP_Decimal
                        {0xffaf, 0x54, false}, // KP_Divide
                        {0xffbd, 0x67, false}, // KP_Equal
                        {0xffe5, 0x39, false}, // Caps_Lock
                        {0xffff, 0x4c, false}, // Delete
                };

                // The modifier keys: their usage and their bit.
                struct ModifierEntry {
                                uint32_t keysym;
                                uint8_t  usage;
                                uint8_t  bit;
                };
                constexpr ModifierEntry Modifiers[] = {
                        {0xffe1, 0xe1, Modifier::LeftShift}, {0xffe2, 0xe5, Modifier::RightShift},
                        {0xffe3, 0xe0, Modifier::LeftCtrl},  {0xffe4, 0xe4, Modifier::RightCtrl},
                        {0xffe7, 0xe3, Modifier::LeftGui},   {0xffe8, 0xe7, Modifier::RightGui}, // Meta
                        {0xffe9, 0xe2, Modifier::LeftAlt},   {0xffea, 0xe6, Modifier::RightAlt},
                        {0xffeb, 0xe3, Modifier::LeftGui},   {0xffec, 0xe7, Modifier::RightGui}, // Super
                        {0xfe03, 0xe6, Modifier::RightAlt}, // ISO_Level3_Shift (AltGr)
                };

        } // namespace

        KeyMapping mapKeysym(uint32_t keysym) {
                KeyMapping m;
                if (keysym >= 'a' && keysym <= 'z') {
                        m.usage = uint8_t(0x04 + (keysym - 'a'));
                        return m;
                }
                if (keysym >= 'A' && keysym <= 'Z') {
                        m.usage = uint8_t(0x04 + (keysym - 'A'));
                        m.shift = true;
                        return m;
                }
                if (keysym >= '1' && keysym <= '9') {
                        m.usage = uint8_t(0x1e + (keysym - '1'));
                        return m;
                }
                if (keysym == '0') {
                        m.usage = 0x27;
                        return m;
                }
                if (keysym >= 0xffbe && keysym <= 0xffc9) { // F1-F12
                        m.usage = uint8_t(0x3a + (keysym - 0xffbe));
                        return m;
                }
                if (keysym >= 0xffb0 && keysym <= 0xffb9) { // KP_0-KP_9
                        m.usage = keysym == 0xffb0 ? 0x62 : uint8_t(0x59 + (keysym - 0xffb1));
                        return m;
                }
                for (const Entry &e : Ascii)
                        if (e.keysym == keysym) return KeyMapping{e.usage, e.shift, 0};
                for (const Entry &e : Function)
                        if (e.keysym == keysym) return KeyMapping{e.usage, e.shift, 0};
                for (const ModifierEntry &e : Modifiers)
                        if (e.keysym == keysym) return KeyMapping{e.usage, false, e.bit};
                return m;
        }

        KeyState::Key KeyState::event(bool down, uint32_t keysym) {
                const KeyMapping m = mapKeysym(keysym);
                Key              k;
                if (m.usage == 0) return k;
                if (m.modifier != 0) {
                        _modifiers = down ? uint8_t(_modifiers | m.modifier) : uint8_t(_modifiers & ~m.modifier);
                        k.usage = m.usage;
                        k.modifiers = _modifiers;
                        k.down = down;
                        return k;
                }
                const uint8_t shifts = Modifier::LeftShift | Modifier::RightShift;
                k.usage = m.usage;
                k.down = down;
                k.modifiers = _modifiers;
                // A character typed with Shift has it; one typed without (a viewer that sends '1'
                // with its Shift held for another reason) keeps what is held.
                if (m.shift && (_modifiers & shifts) == 0) k.modifiers = uint8_t(k.modifiers | Modifier::LeftShift);
                return k;
        }

} // namespace vnc
