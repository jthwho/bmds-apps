/**
 * @file      portstab.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#include <bmds/gui.h>
#include <bmds/sys.h>

#include "ports.h"

namespace ioman {

        /**
         * @brief The Ports tab (devplan/ioman.md §4): BMDS's ports listed, the chosen one's setup
         *        as fields, and a tool for its kind (a terminal on a UART, an I²C bus's scan and
         *        registers, an SPI device's transfers).
         *
         * Its fds (the sysconf watch, a UART's tty) go in the program's `poll()`.
         */
        class PortsTab {
                public:
                        /** @brief Shows a message in the window's status bar. */
                        using Say = void (*)(void *arg, const char *text);

                        /** @brief Finds the ports this BMDS has. @return False if none (no tab). */
                        bool open();

                        /** @brief Its page in @p tabs, its boxes owned by @p window. */
                        void build(bmds_gui_t *gui, uint32_t tabs, uint32_t window, Say say, void *arg);

                        /** @brief Handles @p e if it is the tab's. @return True if it was. */
                        bool event(const bmds_gui_event_t &e);

                        /** @brief Its fds to wait on, into @p fds (room for @p max). @return How many. */
                        size_t pollFds(struct pollfd *fds, size_t max) const;

                        /** @brief What `poll()` said of the fds `pollFds()` gave. */
                        void serviceFds(const struct pollfd *fds, size_t count);

                        /** @brief Everything it opened, closed. */
                        void close();

                private:
                        static constexpr size_t MaxPorts = 12;
                        static constexpr size_t LogBytes = 6144;
                        static constexpr size_t MaxSpiDevices = 6;

                        struct Port {
                                        PortKind kind = PortKind::Uart;
                                        unsigned number = 0;
                                        int32_t  key = -1;      // io/...'s sysconf id (-1: a system I2C bus)
                                        char     name[12] = {}; // `uart1`
                                        char     device[20] = {};
                                        uint64_t shown = 0; // its row's texts, hashed
                                        bool     running = false;
                        };

                        struct Form {
                                        uint32_t form = 0;
                                        uint32_t section = 0;
                                        uint32_t field[MaxFields] = {};
                                        uint32_t other = 0;
                        };

                        void buildForm(PortKind kind, uint32_t parent);
                        void buildTools(uint32_t parent);
                        void refreshPorts();
                        void choose(int row);
                        void loadFields(bool keepIfOff);
                        bool readFields(PortWords &out);
                        void setValue(const char *value, bool asked);
                        void ask(const char *value, const char *why);
                        bool isRunning(const Port &p) const;
                        void openTool();
                        void closeTool();
                        void showTool();
                        void openSpi(int index);
                        void readTty();
                        void i2cScan();
                        void i2cReadWrite(bool write);
                        int  i2cTarget(unsigned long address);
                        void spiTransfer();
                        void log(uint32_t view, char *buffer, const char *text);
                        void say(const char *text) { _say(_sayArg, text); }

                        bmds_gui_t *_gui = nullptr;
                        uint32_t    _window = 0;
                        Say         _say = nullptr;
                        void       *_sayArg = nullptr;

                        Port   _ports[MaxPorts];
                        size_t _portCount = 0;
                        int    _chosen = -1;
                        int    _watchFd = -1; // sysconf's io/ changes

                        uint32_t _table = 0;
                        uint32_t _recheck = 0; // a look at the devices after a change (a one-shot timer)
                        Form     _forms[2];    // Uart, Spi
                        uint32_t _setupNote = 0;
                        uint32_t _apply = 0, _stop = 0, _save = 0;
                        uint32_t _ask = 0;
                        char     _asking[BMDS_SYSCONF_VALUE_MAX + 1] = {};

                        // The tools.
                        uint32_t _toolNote = 0;
                        uint32_t _uartTool = 0, _terminal = 0, _clear = 0;
                        int      _ttyFd = -1;
                        uint32_t _i2cTool = 0, _i2cAddress = 0, _i2cRegister = 0, _i2cCount = 0, _i2cBytes = 0,
                                 _i2cForce = 0, _i2cScan = 0, _i2cRead = 0, _i2cWrite = 0, _i2cLog = 0;
                        int      _i2cFd = -1;
                        char     _i2cText[LogBytes] = {};
                        uint32_t _spiTool = 0, _spiDevice = 0, _spiMode = 0, _spiSpeed = 0, _spiBits = 0, _spiSend = 0,
                                 _spiGo = 0, _spiLog = 0;
                        int    _spiFd = -1;
                        int    _spiIndexes[MaxSpiDevices] = {}; // the Device box's entries: C of each
                        size_t _spiCount = 0;
                        char   _spiText[LogBytes] = {};
        };

} // namespace ioman
