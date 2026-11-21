/**
 * @file      portstab.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The Ports tab (devplan/ioman.md §4): BMDS's ports (`io/uartN`, `io/spiN`, the system's I²C
 * buses), the chosen one's value as fields written back through sysconf, and a tool for it:
 * a terminal on `/dev/ttySN`, `i2c_ctl` on `/dev/i2c-N`, `spi_ctl` on `/dev/spidevB.C`.
 */

#include "portstab.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <bmds/guiclasses.h>
#include <bmds/sys.h>

namespace ioman {

        namespace {

                constexpr int    RecheckMs = 300; // the kernel starts a port after its value changes
                constexpr size_t MaxBytes = 64;   // a register read or write, an I2C write's bytes
                constexpr size_t MaxSpiBytes = 256;

                uint64_t hashText(uint64_t h, const char *text) {
                        for (const char *p = text; *p != '\0'; ++p) h = (h ^ uint8_t(*p)) * 1099511628211ull;
                        return (h ^ 0xff) * 1099511628211ull; // a separator, so "ab"+"c" isn't "a"+"bc"
                }

                bool exists(const char *path) {
                        return access(path, F_OK) == 0;
                }

                // A speed as people write it: `1000000`, `500k`, `2m`.
                bool parseHz(const char *text, uint32_t &out) {
                        char   number[24];
                        size_t n = strnlen(text, sizeof(number) - 1);
                        memcpy(number, text, n);
                        number[n] = '\0';
                        while (n > 0 && number[n - 1] == ' ') number[--n] = '\0';
                        unsigned long scale = 1;
                        if (n > 0 && (number[n - 1] == 'k' || number[n - 1] == 'K')) scale = 1000;
                        if (n > 0 && (number[n - 1] == 'm' || number[n - 1] == 'M')) scale = 1000000;
                        if (scale != 1) number[--n] = '\0';
                        unsigned long v = 0;
                        if (!parseNumber(number, 4294967295ul / scale, v) || v == 0) return false;
                        out = uint32_t(v * scale);
                        return true;
                }

                // A GUI text value as a C string (cut to @p size).
                void textOf(const bmds_gui_value_t &v, char *out, size_t size) {
                        const size_t n = v.length < size - 1 ? v.length : size - 1;
                        memcpy(out, v.text, n);
                        out[n] = '\0';
                        return;
                }

                const char *errorText(int32_t r) {
                        switch (-r) {
                                case BMDS_ENXIO: return "no answer (nothing acknowledged)";
                                case BMDS_EBUSY: return "a BMDS driver's address: tick Force to reach it anyway";
                                case BMDS_ETIMEDOUT: return "timed out";
                                case BMDS_ENOTSUP: return "more than the bus takes in one transaction";
                                case BMDS_ENODEV: return "the port has stopped";
                                case BMDS_EMSGSIZE: return "too long";
                                default: return strerror(-r);
                        }
                }

        } // namespace

        // ---- Finding and listing the ports --------------------------------------------------------

        bool PortsTab::open() {
                const auto add = [this](PortKind kind, unsigned n, int32_t key, const char *name, const char *device) {
                        if (_portCount == MaxPorts) return;
                        Port &p = _ports[_portCount++];
                        p.kind = kind;
                        p.number = n;
                        p.key = key;
                        snprintf(p.name, sizeof(p.name), "%s", name);
                        snprintf(p.device, sizeof(p.device), "%s", device);
                        return;
                };
                char key[16], name[12], device[20];
                for (unsigned n = 0; n < 8; ++n) {
                        snprintf(key, sizeof(key), "io/uart%u", n);
                        const int32_t id = bmds_sysconf_find(key);
                        if (id < 0) continue;
                        snprintf(name, sizeof(name), "uart%u", n);
                        snprintf(device, sizeof(device), "/dev/ttyS%u", n);
                        add(PortKind::Uart, n, id, name, device);
                }
                for (unsigned n = 0; n < 8; ++n) {
                        snprintf(key, sizeof(key), "io/spi%u", n);
                        const int32_t id = bmds_sysconf_find(key);
                        if (id < 0) continue;
                        snprintf(name, sizeof(name), "spi%u", n);
                        snprintf(device, sizeof(device), "/dev/spidev%u.0", n);
                        add(PortKind::Spi, n, id, name, device);
                }
                for (unsigned n = 0; n < 4; ++n) {
                        snprintf(device, sizeof(device), "/dev/i2c-%u", n);
                        if (!exists(device)) continue;
                        snprintf(name, sizeof(name), "i2c-%u", n);
                        add(PortKind::I2c, n, -1, name, device);
                }
                if (_portCount == 0) return false;
                // Told of io/'s changes, wherever they come from (a shell's `sysconf set`).
                _watchFd = int(bmds_sysconf_watch("io/", BMDS_O_CLOEXEC));
                return true;
        }

        bool PortsTab::isRunning(const Port &p) const {
                if (p.kind == PortKind::I2c) return true;
                return exists(p.device); // there while the port runs (/dev/spidevN.0 for SPI's first)
        }

        void PortsTab::refreshPorts() {
                static char value[BMDS_SYSCONF_VALUE_MAX + 1];
                static char saved[BMDS_SYSCONF_VALUE_MAX + 1];
                for (size_t i = 0; i < _portCount; ++i) {
                        Port &p = _ports[i];
                        char  device[48] = {};
                        char  state[48] = {};
                        value[0] = '\0';
                        const bool wasRunning = p.running;
                        p.running = isRunning(p);
                        if (p.kind == PortKind::I2c) {
                                snprintf(device, sizeof(device), "%s", p.device);
                                snprintf(state, sizeof(state), "System bus");
                                snprintf(value, sizeof(value), "(BMDS's drivers share it)");
                        } else {
                                if (bmds_sysconf_get(p.key, BMDS_SYSCONF_FIELD_VALUE, value, sizeof(value)) < 0)
                                        value[0] = '\0';
                                const bool hasSaved =
                                        bmds_sysconf_get(p.key, BMDS_SYSCONF_FIELD_SAVED, saved, sizeof(saved)) >= 0;
                                const bool same = hasSaved ? strcmp(saved, value) == 0 : value[0] == '\0';
                                if (value[0] == '\0')
                                        snprintf(state, sizeof(state), "Off%s", same ? "" : " · not saved");
                                else
                                        snprintf(state, sizeof(state), "%s · %s", p.running ? "Running" : "Not running",
                                                 same ? "saved" : "not saved");
                                if (p.kind == PortKind::Uart) {
                                        snprintf(device, sizeof(device), "%s", p.device);
                                } else {
                                        // Every chip select's device there: `/dev/spidev2.0 .1`.
                                        size_t n = size_t(snprintf(device, sizeof(device), "/dev/spidev%u.", p.number));
                                        bool   any = false;
                                        for (unsigned c = 0; c < MaxSpiDevices; ++c) {
                                                char path[24];
                                                snprintf(path, sizeof(path), "/dev/spidev%u.%u", p.number, c);
                                                if (!exists(path)) continue;
                                                if (n < sizeof(device))
                                                        n += size_t(snprintf(device + n, sizeof(device) - n, "%s%u",
                                                                             any ? " ." : "", c));
                                                any = true;
                                        }
                                        if (!any) snprintf(device, sizeof(device), "/dev/spidev%u.C", p.number);
                                }
                        }
                        const char *texts[4] = {p.name, device, state, value[0] != '\0' ? value : "-"};
                        uint64_t    h = 1469598103934665603ull;
                        for (const char *t : texts) h = hashText(h, t);
                        if (h != p.shown) {
                                p.shown = h;
                                bmds_details_view_set_row(_gui, _table, int32_t(i), texts, 4);
                        }
                        // The chosen port started or stopped: its tool follows.
                        if (int(i) == _chosen && p.running != wasRunning) {
                                closeTool();
                                openTool();
                        }
                }
                return;
        }

        // ---- Building -----------------------------------------------------------------------------

        void PortsTab::build(bmds_gui_t *gui, uint32_t tabs, uint32_t window, Say say, void *arg) {
                _gui = gui;
                _window = window;
                _say = say;
                _sayArg = arg;
                const uint32_t page = bmds_tab_page_new(_gui, tabs);
                bmds_tab_page_set_title(_gui, page, "Ports");
                bmds_box_set_margin(_gui, page, 4);
                const uint32_t split = bmds_splitter_new(_gui, page);
                bmds_widget_set_stretch(_gui, split, 1);
                bmds_splitter_set_direction(_gui, split, "across");
                bmds_splitter_set_min_first(_gui, split, 300);
                bmds_splitter_set_min_second(_gui, split, 260);

                // Left: the ports above the chosen one's setup.
                const uint32_t left = bmds_splitter_new(_gui, split);
                bmds_splitter_set_direction(_gui, left, "down");
                bmds_splitter_set_min_first(_gui, left, 120);
                bmds_splitter_set_min_second(_gui, left, 160);
                _table = bmds_details_view_new(_gui, left);
                static const char *const Columns[] = {"Port", "Device", "State", "Setup"};
                static constexpr int     Widths[] = {60, 130, 150, 300};
                bmds_details_view_set_columns(_gui, _table, Columns, 4);
                for (int32_t c = 0; c < 4; ++c) bmds_details_view_set_column_width(_gui, _table, c, Widths[c]);
                for (size_t i = 0; i < _portCount; ++i) {
                        const char *texts[4] = {_ports[i].name, "", "", ""};
                        bmds_details_view_add_row(_gui, _table, texts, 4);
                }
                const uint32_t setup = bmds_box_new(_gui, left);
                bmds_box_set_spacing(_gui, setup, 4);
                buildForm(PortKind::Uart, setup);
                buildForm(PortKind::Spi, setup);
                _setupNote = bmds_label_new(_gui, setup);
                bmds_label_set_text(_gui, _setupNote, "Choose a port.");
                bmds_widget_set_stretch(_gui, _setupNote, 1);
                const uint32_t buttons = bmds_box_new(_gui, setup);
                bmds_box_set_direction(_gui, buttons, "across");
                bmds_box_set_spacing(_gui, buttons, 4);
                _apply = bmds_button_new(_gui, buttons);
                bmds_button_set_text(_gui, _apply, "&Apply");
                _stop = bmds_button_new(_gui, buttons);
                bmds_button_set_text(_gui, _stop, "S&top");
                _save = bmds_button_new(_gui, buttons);
                bmds_button_set_text(_gui, _save, "&Save");
                bmds_splitter_set_position(_gui, left, 190);

                // Right: the chosen port's tool.
                const uint32_t right = bmds_box_new(_gui, split);
                bmds_box_set_spacing(_gui, right, 4);
                buildTools(right);
                bmds_splitter_set_position(_gui, split, 470);

                _recheck = bmds_timer_new(_gui, 0);
                bmds_timer_set_interval(_gui, _recheck, RecheckMs);
                bmds_timer_set_repeat(_gui, _recheck, 0);
                refreshPorts();
                choose(-1);
                return;
        }

        void PortsTab::buildForm(PortKind kind, uint32_t parent) {
                Form &f = _forms[kind == PortKind::Uart ? 0 : 1];
                f.form = bmds_form_new(_gui, parent);
                bmds_widget_set_stretch(_gui, f.form, 1);
                f.section = bmds_form_section_new(_gui, f.form);
                size_t           count = 0;
                const PortField *fields = portFields(kind, count);
                for (size_t i = 0; i < count; ++i) {
                        const PortField &field = fields[i];
                        const uint32_t   row = bmds_form_row_new(_gui, f.form);
                        switch (field.kind) {
                                case FieldKind::Text:
                                        bmds_form_row_set_label(_gui, row, field.label);
                                        f.field[i] = bmds_line_edit_new(_gui, row);
                                        bmds_widget_set_stretch(_gui, f.field[i], 1);
                                        break;
                                case FieldKind::Choice:
                                        bmds_form_row_set_label(_gui, row, field.label);
                                        f.field[i] = bmds_combo_box_new(_gui, row);
                                        bmds_combo_box_set_items(_gui, f.field[i], field.choices,
                                                                 uint32_t(field.choiceCount));
                                        break;
                                case FieldKind::Flag:
                                        bmds_form_row_set_label(_gui, row, "");
                                        f.field[i] = bmds_check_box_new(_gui, row);
                                        bmds_check_box_set_text(_gui, f.field[i], field.label);
                                        break;
                        }
                }
                const uint32_t row = bmds_form_row_new(_gui, f.form);
                bmds_form_row_set_label(_gui, row, "Other words:");
                f.other = bmds_line_edit_new(_gui, row);
                bmds_widget_set_stretch(_gui, f.other, 1);
                bmds_widget_set_visible(_gui, f.form, 0);
                return;
        }

        void PortsTab::buildTools(uint32_t parent) {
                _toolNote = bmds_label_new(_gui, parent);
                bmds_label_set_text(_gui, _toolNote, "");

                // UART: a terminal on its tty.
                _uartTool = bmds_box_new(_gui, parent);
                bmds_widget_set_stretch(_gui, _uartTool, 1);
                bmds_box_set_spacing(_gui, _uartTool, 4);
                _terminal = bmds_terminal_new(_gui, _uartTool);
                bmds_widget_set_stretch(_gui, _terminal, 1);
                _clear = bmds_button_new(_gui, _uartTool);
                bmds_button_set_text(_gui, _clear, "&Clear");

                const auto field = [this](uint32_t row, const char *label, const char *text, int stretch) {
                        const uint32_t l = bmds_label_new(_gui, row);
                        bmds_label_set_text(_gui, l, label);
                        const uint32_t e = bmds_line_edit_new(_gui, row);
                        bmds_line_edit_set_text(_gui, e, text);
                        bmds_widget_set_stretch(_gui, e, stretch);
                        return e;
                };
                const auto rowOf = [this](uint32_t box) {
                        const uint32_t row = bmds_box_new(_gui, box);
                        bmds_box_set_direction(_gui, row, "across");
                        bmds_box_set_spacing(_gui, row, 4);
                        return row;
                };
                const auto button = [this](uint32_t row, const char *text) {
                        const uint32_t b = bmds_button_new(_gui, row);
                        bmds_button_set_text(_gui, b, text);
                        return b;
                };
                const auto logView = [this](uint32_t box) {
                        const uint32_t t = bmds_text_edit_new(_gui, box);
                        bmds_widget_set_stretch(_gui, t, 1);
                        bmds_text_edit_set_read_only(_gui, t, 1);
                        return t;
                };

                // I2C: an address's registers, and a scan.
                _i2cTool = bmds_box_new(_gui, parent);
                bmds_widget_set_stretch(_gui, _i2cTool, 1);
                bmds_box_set_spacing(_gui, _i2cTool, 4);
                uint32_t row = rowOf(_i2cTool);
                _i2cAddress = field(row, "Address:", "0x50", 1);
                _i2cRegister = field(row, "Register:", "0x00", 1);
                const uint32_t countLabel = bmds_label_new(_gui, row);
                bmds_label_set_text(_gui, countLabel, "Count:");
                _i2cCount = bmds_number_field_new(_gui, row);
                bmds_number_field_set_minimum(_gui, _i2cCount, 1);
                bmds_number_field_set_maximum(_gui, _i2cCount, int32_t(MaxBytes));
                bmds_number_field_set_value(_gui, _i2cCount, 16);
                row = rowOf(_i2cTool);
                _i2cBytes = field(row, "Bytes:", "", 1);
                _i2cForce = bmds_check_box_new(_gui, row);
                bmds_check_box_set_text(_gui, _i2cForce, "&Force");
                row = rowOf(_i2cTool);
                _i2cScan = button(row, "S&can");
                _i2cRead = button(row, "&Read");
                _i2cWrite = button(row, "&Write");
                _i2cLog = logView(_i2cTool);

                // SPI: a device's settings, and transfers.
                _spiTool = bmds_box_new(_gui, parent);
                bmds_widget_set_stretch(_gui, _spiTool, 1);
                bmds_box_set_spacing(_gui, _spiTool, 4);
                row = rowOf(_spiTool);
                const uint32_t deviceLabel = bmds_label_new(_gui, row);
                bmds_label_set_text(_gui, deviceLabel, "Device:");
                _spiDevice = bmds_combo_box_new(_gui, row);
                const uint32_t modeLabel = bmds_label_new(_gui, row);
                bmds_label_set_text(_gui, modeLabel, "Mode:");
                _spiMode = bmds_combo_box_new(_gui, row);
                static const char *const Modes[] = {"0", "1", "2", "3"};
                bmds_combo_box_set_items(_gui, _spiMode, Modes, 4);
                row = rowOf(_spiTool);
                const uint32_t bitsLabel = bmds_label_new(_gui, row);
                bmds_label_set_text(_gui, bitsLabel, "Bits:");
                _spiBits = bmds_combo_box_new(_gui, row);
                static const char *const Bits[] = {"8", "16", "32"};
                bmds_combo_box_set_items(_gui, _spiBits, Bits, 3);
                _spiSpeed = field(row, "Speed:", "", 1);
                row = rowOf(_spiTool);
                _spiSend = field(row, "Send:", "", 1);
                _spiGo = button(row, "&Transfer");
                _spiLog = logView(_spiTool);
                return;
        }

        // ---- The chosen port ----------------------------------------------------------------------

        void PortsTab::choose(int row) {
                closeTool();
                _chosen = row >= 0 && size_t(row) < _portCount ? row : -1;
                const Port *p = _chosen >= 0 ? &_ports[_chosen] : nullptr;
                const bool  uart = p != nullptr && p->kind == PortKind::Uart;
                const bool  spi = p != nullptr && p->kind == PortKind::Spi;
                bmds_widget_set_visible(_gui, _forms[0].form, uart);
                bmds_widget_set_visible(_gui, _forms[1].form, spi);
                bmds_widget_set_visible(_gui, _setupNote, !uart && !spi);
                if (p == nullptr)
                        bmds_label_set_text(_gui, _setupNote, "Choose a port.");
                else if (p->kind == PortKind::I2c)
                        bmds_label_set_text(_gui, _setupNote, "A system bus, always there: BMDS's drivers share it.");
                bmds_widget_set_enabled(_gui, _apply, uart || spi);
                bmds_widget_set_enabled(_gui, _stop, uart || spi);
                bmds_widget_set_enabled(_gui, _save, uart || spi);
                if (uart || spi) {
                        char title[48];
                        snprintf(title, sizeof(title), "Set up (io/%s)", p->name);
                        bmds_form_section_set_title(_gui, _forms[spi ? 1 : 0].section, title);
                        loadFields(false);
                }
                openTool();
                return;
        }

        void PortsTab::loadFields(bool keepIfOff) {
                static char value[BMDS_SYSCONF_VALUE_MAX + 1];
                const Port &p = _ports[_chosen];
                if (bmds_sysconf_get(p.key, BMDS_SYSCONF_FIELD_VALUE, value, sizeof(value)) < 0) value[0] = '\0';
                if (keepIfOff && value[0] == '\0') return; // stopped: what it was, for Apply to start again
                PortWords w;
                splitWords(p.kind, value, w);
                const Form      &f = _forms[p.kind == PortKind::Uart ? 0 : 1];
                size_t           count = 0;
                const PortField *fields = portFields(p.kind, count);
                for (size_t i = 0; i < count; ++i) {
                        switch (fields[i].kind) {
                                case FieldKind::Text: bmds_line_edit_set_text(_gui, f.field[i], w.field[i]); break;
                                case FieldKind::Choice:
                                        for (size_t c = 0; c < fields[i].choiceCount; ++c)
                                                if (strcmp(fields[i].choices[c], w.field[i]) == 0)
                                                        bmds_combo_box_set_current(_gui, f.field[i], int32_t(c));
                                        break;
                                case FieldKind::Flag:
                                        bmds_check_box_set_checked(_gui, f.field[i], w.field[i][0] != '\0');
                                        break;
                        }
                }
                bmds_line_edit_set_text(_gui, f.other, w.other);
                return;
        }

        bool PortsTab::readFields(PortWords &out) {
                const Port      &p = _ports[_chosen];
                const Form      &f = _forms[p.kind == PortKind::Uart ? 0 : 1];
                size_t           count = 0;
                const PortField *fields = portFields(p.kind, count);
                out = PortWords();
                bmds_gui_value_t v;
                for (size_t i = 0; i < count; ++i) {
                        switch (fields[i].kind) {
                                case FieldKind::Text:
                                        if (bmds_line_edit_get_text(_gui, f.field[i], &v) == 0)
                                                textOf(v, out.field[i], FieldBytes);
                                        break;
                                case FieldKind::Choice: {
                                        const size_t c = bmds_combo_box_get_current(_gui, f.field[i], &v) == 0 &&
                                                                         v.number >= 0 &&
                                                                         size_t(v.number) < fields[i].choiceCount
                                                                 ? size_t(v.number)
                                                                 : 0;
                                        snprintf(out.field[i], FieldBytes, "%s", fields[i].choices[c]);
                                        break;
                                }
                                case FieldKind::Flag:
                                        if (bmds_check_box_get_checked(_gui, f.field[i], &v) == 0 && v.number != 0)
                                                snprintf(out.field[i], FieldBytes, "1");
                                        break;
                        }
                }
                if (bmds_line_edit_get_text(_gui, f.other, &v) == 0) textOf(v, out.other, OtherBytes);
                return true;
        }

        // The chosen port's value set to @p value; a held pin asks before `force` (@p asked: it did).
        void PortsTab::setValue(const char *value, bool asked) {
                const Port   &p = _ports[_chosen];
                const int32_t r = bmds_sysconf_set(p.key, BMDS_SYSCONF_OP_SET, value);
                if (r < 0) {
                        char why[256] = {};
                        if (bmds_sysconf_get(p.key, BMDS_SYSCONF_FIELD_REFUSAL, why, sizeof(why)) < 0 || why[0] == '\0')
                                snprintf(why, sizeof(why), "io/%s: %s", p.name, strerror(-r));
                        if (r == -BMDS_EBUSY && !asked && !hasWord(value, "force")) {
                                ask(value, why);
                                return;
                        }
                        say(why);
                        return;
                }
                char text[64];
                snprintf(text, sizeof(text), value[0] != '\0' ? "io/%s set" : "io/%s stopped", p.name);
                say(text);
                // The fields as the value now is (a `force` added shows in Other words); a stopped
                // port's kept as they were, so Apply starts it again.
                loadFields(true);
                refreshPorts();
                bmds_timer_start(_gui, _recheck);
                return;
        }

        void PortsTab::ask(const char *value, const char *why) {
                if (_ask != 0) bmds_gui_destroy(_gui, _ask);
                snprintf(_asking, sizeof(_asking), "%s%sforce", value, value[0] != '\0' ? " " : "");
                char text[384];
                snprintf(text, sizeof(text), "%s\n\nUse those pins for io/%s anyway?", why, _ports[_chosen].name);
                _ask = bmds_gui_new(_gui, "MessageBox", 0);
                bmds_message_box_set_kind(_gui, _ask, "warning");
                bmds_message_box_set_title(_gui, _ask, "Pins in use");
                bmds_message_box_set_text(_gui, _ask, text);
                bmds_message_box_set_buttons(_gui, _ask, "yesNo");
                bmds_message_box_set_owner(_gui, _ask, _window);
                bmds_message_box_open(_gui, _ask);
                return;
        }

        // ---- The tools ----------------------------------------------------------------------------

        void PortsTab::showTool() {
                const Port *p = _chosen >= 0 ? &_ports[_chosen] : nullptr;
                const bool  on = p != nullptr && p->running;
                bmds_widget_set_visible(_gui, _uartTool, on && p->kind == PortKind::Uart);
                bmds_widget_set_visible(_gui, _i2cTool, on && p->kind == PortKind::I2c);
                bmds_widget_set_visible(_gui, _spiTool, on && p->kind == PortKind::Spi);
                bmds_widget_set_visible(_gui, _toolNote, !on);
                bmds_label_set_text(_gui, _toolNote,
                                    p == nullptr ? "" : "Start the port (Apply) to talk to what is on it.");
                return;
        }

        void PortsTab::openTool() {
                if (_chosen >= 0) _ports[_chosen].running = isRunning(_ports[_chosen]);
                showTool();
                if (_chosen < 0 || !_ports[_chosen].running) return;
                const Port &p = _ports[_chosen];
                switch (p.kind) {
                        case PortKind::Uart:
                                _ttyFd = ::open(p.device, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
                                if (_ttyFd < 0) {
                                        char text[96];
                                        snprintf(text, sizeof(text), "%s: %s", p.device, strerror(errno));
                                        say(text);
                                }
                                break;
                        case PortKind::I2c: _i2cFd = ::open(p.device, O_RDWR | O_CLOEXEC); break;
                        case PortKind::Spi: {
                                // Its devices, one entry each.
                                _spiCount = 0;
                                const char *items[MaxSpiDevices];
                                static char names[MaxSpiDevices][24];
                                for (unsigned c = 0; c < MaxSpiDevices; ++c) {
                                        snprintf(names[_spiCount], sizeof(names[0]), "/dev/spidev%u.%u", p.number, c);
                                        if (!exists(names[_spiCount])) continue;
                                        _spiIndexes[_spiCount] = int(c);
                                        items[_spiCount] = names[_spiCount];
                                        ++_spiCount;
                                }
                                bmds_combo_box_set_items(_gui, _spiDevice, items, uint32_t(_spiCount));
                                if (_spiCount > 0) {
                                        bmds_combo_box_set_current(_gui, _spiDevice, 0);
                                        openSpi(0);
                                }
                                break;
                        }
                }
                return;
        }

        void PortsTab::closeTool() {
                int *const fds[] = {&_ttyFd, &_i2cFd, &_spiFd};
                for (int *fd : fds) {
                        if (*fd >= 0) ::close(*fd);
                        *fd = -1;
                }
                return;
        }

        void PortsTab::openSpi(int entry) {
                if (_spiFd >= 0) ::close(_spiFd);
                _spiFd = -1;
                if (_chosen < 0 || entry < 0 || size_t(entry) >= _spiCount) return;
                char path[24];
                snprintf(path, sizeof(path), "/dev/spidev%u.%d", _ports[_chosen].number, _spiIndexes[entry]);
                _spiFd = ::open(path, O_RDWR | O_CLOEXEC);
                bmds_spi_info_t info{};
                info.size = sizeof(info);
                if (_spiFd < 0 || bmds_spi_ctl(_spiFd, BMDS_SPI_INFO, &info) < 0) return;
                // Its settings as it has them.
                bmds_combo_box_set_current(_gui, _spiMode, int32_t(info.mode & (BMDS_SPI_CPOL | BMDS_SPI_CPHA)));
                bmds_combo_box_set_current(_gui, _spiBits,
                                           info.bits_per_word == 16   ? 1
                                           : info.bits_per_word == 32 ? 2
                                                                      : 0);
                char speed[16];
                snprintf(speed, sizeof(speed), "%u", unsigned(info.max_speed_hz));
                bmds_line_edit_set_text(_gui, _spiSpeed, speed);
                return;
        }

        void PortsTab::readTty() {
                char buf[513];
                for (;;) {
                        const ssize_t n = ::read(_ttyFd, buf, sizeof(buf) - 1);
                        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
                        if (n <= 0) { // the port stopped under it
                                closeTool();
                                refreshPorts();
                                return;
                        }
                        // The terminal takes text: a NUL ends it, so they are left out.
                        size_t k = 0;
                        for (ssize_t i = 0; i < n; ++i)
                                if (buf[i] != '\0') buf[k++] = buf[i];
                        buf[k] = '\0';
                        if (k > 0) bmds_terminal_write(_gui, _terminal, buf);
                }
        }

        // The newest of a tool's log: @p text added at the end, the oldest lines dropped to fit.
        void PortsTab::log(uint32_t view, char *buffer, const char *text) {
                const size_t add = strlen(text);
                size_t       have = strlen(buffer);
                if (add + 1 >= LogBytes) return;
                if (have + add + 1 > LogBytes) {
                        size_t cut = have + add + 1 - LogBytes;
                        while (cut < have && buffer[cut - 1] != '\n') ++cut;
                        memmove(buffer, buffer + cut, have - cut + 1);
                        have -= cut;
                }
                memcpy(buffer + have, text, add + 1);
                bmds_text_edit_set_text(_gui, view, buffer);
                bmds_text_edit_go_to_line(_gui, view, 1000000); // the newest in sight
                return;
        }

        // The I2C file's target @p address (forced if Force is ticked). @return 0 or -errno.
        int PortsTab::i2cTarget(unsigned long address) {
                bmds_gui_value_t  v;
                const bool        force = bmds_check_box_get_checked(_gui, _i2cForce, &v) == 0 && v.number != 0;
                bmds_i2c_target_t t{};
                t.size = sizeof(t);
                t.address = uint32_t(address);
                t.flags = force ? BMDS_I2C_FORCE : 0;
                return int(bmds_i2c_ctl(_i2cFd, BMDS_I2C_SET_TARGET, &t));
        }

        void PortsTab::i2cScan() {
                // A driver's address is refused as a target (not probed); any other gets an empty
                // write, which a device there acknowledges.
                ScanResult results[128];
                for (unsigned a = 0; a < 128; ++a) {
                        results[a] = ScanResult::Skipped;
                        if (a < 0x08 || a > 0x77) continue;
                        bmds_i2c_target_t t{};
                        t.size = sizeof(t);
                        t.address = a;
                        if (bmds_i2c_ctl(_i2cFd, BMDS_I2C_SET_TARGET, &t) == -BMDS_EBUSY) {
                                results[a] = ScanResult::Driver;
                                continue;
                        }
                        bmds_i2c_msg_t      m{};
                        bmds_i2c_transfer_t x{};
                        m.address = uint16_t(a);
                        x.size = sizeof(x);
                        x.count = 1;
                        x.messages = &m;
                        results[a] =
                                bmds_i2c_ctl(_i2cFd, BMDS_I2C_TRANSFER, &x) >= 0 ? ScanResult::Found : ScanResult::None;
                }
                char         grid[1024];
                const size_t n = scanGrid(results, grid, sizeof(grid) - 16);
                snprintf(grid + n, sizeof(grid) - n, "\n");
                char head[48];
                snprintf(head, sizeof(head), "Scan of %s (UU: a driver's):\n", _ports[_chosen].device);
                log(_i2cLog, _i2cText, head);
                log(_i2cLog, _i2cText, grid);
                return;
        }

        void PortsTab::i2cReadWrite(bool write) {
                bmds_gui_value_t v;
                char             text[256];
                unsigned long    address = 0, reg = 0;
                if (bmds_line_edit_get_text(_gui, _i2cAddress, &v) == 0) textOf(v, text, sizeof(text));
                if (!parseNumber(text, 0x7f, address)) {
                        say("The address is 0x00 to 0x7f");
                        return;
                }
                if (bmds_line_edit_get_text(_gui, _i2cRegister, &v) == 0) textOf(v, text, sizeof(text));
                if (!parseNumber(text, 0xff, reg)) {
                        say("The register is 0x00 to 0xff");
                        return;
                }
                if (const int r = i2cTarget(address); r < 0) {
                        snprintf(text, sizeof(text), "0x%02lx: %s\n", address, errorText(r));
                        log(_i2cLog, _i2cText, text);
                        return;
                }
                uint8_t        out[1 + MaxBytes];
                uint8_t        in[MaxBytes];
                size_t         count = 0;
                bmds_i2c_msg_t m[2]{};
                out[0] = uint8_t(reg);
                m[0].address = m[1].address = uint16_t(address);
                m[0].data = out;
                m[0].length = 1;
                if (write) {
                        char bytes[256] = {};
                        if (bmds_line_edit_get_text(_gui, _i2cBytes, &v) == 0) textOf(v, bytes, sizeof(bytes));
                        const long n = parseBytes(bytes, out + 1, MaxBytes);
                        if (n <= 0) {
                                say("Bytes: hex, `01 02` or `0102`, up to 64");
                                return;
                        }
                        count = size_t(n);
                        m[0].length = uint16_t(1 + count);
                } else {
                        count = bmds_number_field_get_value(_gui, _i2cCount, &v) == 0 && v.number > 0 &&
                                                size_t(v.number) <= MaxBytes
                                        ? size_t(v.number)
                                        : 1;
                        m[1].flags = BMDS_I2C_MSG_READ;
                        m[1].length = uint16_t(count);
                        m[1].data = in;
                }
                bmds_i2c_transfer_t x{};
                x.size = sizeof(x);
                x.count = write ? 1 : 2;
                x.messages = m;
                const int32_t r = bmds_i2c_ctl(_i2cFd, BMDS_I2C_TRANSFER, &x);
                // The bytes under it, sixteen a line (the log doesn't wrap).
                size_t n = size_t(snprintf(text, sizeof(text), "%s 0x%02lx at 0x%02lx:%s", write ? "Wrote" : "Read",
                                           address, reg, r < 0 ? " " : "\n  "));
                if (r < 0)
                        snprintf(text + n, sizeof(text) - n, "%s\n", errorText(r));
                else {
                        n += bytesText(write ? out + 1 : in, count, 2, text + n, sizeof(text) - n - 1);
                        snprintf(text + n, sizeof(text) - n, "\n");
                }
                log(_i2cLog, _i2cText, text);
                return;
        }

        void PortsTab::spiTransfer() {
                if (_spiFd < 0) return;
                bmds_gui_value_t v;
                char             text[1200];
                // The device's settings from the fields, its own bits (3-wire, loopback) as they are.
                bmds_spi_info_t info{};
                info.size = sizeof(info);
                if (bmds_spi_ctl(_spiFd, BMDS_SPI_INFO, &info) < 0) return;
                bmds_spi_setup_t s{};
                s.size = sizeof(s);
                const uint32_t mode =
                        bmds_combo_box_get_current(_gui, _spiMode, &v) == 0 ? uint32_t(v.number) & 3u : 0u;
                s.mode = (info.mode & ~uint32_t(BMDS_SPI_CPOL | BMDS_SPI_CPHA)) | mode;
                const int bits = bmds_combo_box_get_current(_gui, _spiBits, &v) == 0 ? int(v.number) : 0;
                s.bits_per_word = bits == 1 ? 16 : bits == 2 ? 32 : 8;
                if (bmds_line_edit_get_text(_gui, _spiSpeed, &v) == 0) textOf(v, text, sizeof(text));
                if (!parseHz(text, s.max_speed_hz)) {
                        say("Speed: Hz, or with k or m (1m)");
                        return;
                }
                if (const int32_t r = bmds_spi_ctl(_spiFd, BMDS_SPI_SETUP, &s); r < 0) {
                        snprintf(text, sizeof(text), "Settings: %s\n", errorText(r));
                        log(_spiLog, _spiText, text);
                        return;
                }
                char send[600] = {};
                if (bmds_line_edit_get_text(_gui, _spiSend, &v) == 0) textOf(v, send, sizeof(send));
                uint8_t    out[MaxSpiBytes], in[MaxSpiBytes];
                const long n = parseBytes(send, out, MaxSpiBytes);
                if (n <= 0) {
                        say("Send: bytes in hex, `9f 00 00` or `9f0000`, up to 256");
                        return;
                }
                bmds_spi_xfer_t xfer{};
                xfer.tx_buf = uintptr_t(out);
                xfer.rx_buf = uintptr_t(in);
                xfer.len = uint32_t(n);
                bmds_spi_transfer_t t{};
                t.size = sizeof(t);
                t.count = 1;
                t.transfers = &xfer;
                const int32_t r = bmds_spi_ctl(_spiFd, BMDS_SPI_TRANSFER, &t);
                size_t        k = size_t(snprintf(text, sizeof(text), "> "));
                k += bytesText(out, size_t(n), 2, text + k, sizeof(text) - k);
                if (r < 0) {
                        snprintf(text + k, sizeof(text) - k, "\n  %s\n", errorText(r));
                } else if (k + 4 < sizeof(text)) {
                        k += size_t(snprintf(text + k, sizeof(text) - k, "\n< "));
                        k += bytesText(in, size_t(n), 2, text + k, sizeof(text) - k - 1);
                        snprintf(text + k, sizeof(text) - k, "\n");
                }
                log(_spiLog, _spiText, text);
                return;
        }

        // ---- Events and fds -----------------------------------------------------------------------

        bool PortsTab::event(const bmds_gui_event_t &e) {
                bmds_gui_t *gui = _gui;
                if (gui == nullptr) return false;
                if (e.source == _table && bmds_gui_event_is(gui, &e, BMDS_GUI_ITEM_VIEW_CURRENT_CHANGED) &&
                    e.count == 1) {
                        if (int(e.values[0].number) != _chosen) choose(int(e.values[0].number));
                        return true;
                }
                if (e.source == _recheck && bmds_gui_event_is(gui, &e, BMDS_GUI_TIMER_TIMEOUT)) {
                        refreshPorts();
                        return true;
                }
                if (e.source == _ask && _ask != 0 && bmds_gui_event_is(gui, &e, BMDS_GUI_MESSAGE_BOX_ANSWERED) &&
                    e.count == 1) {
                        const bool yes = strcmp(bmds_gui_atom_name(gui, uint32_t(e.values[0].number)), "yes") == 0;
                        bmds_gui_destroy(gui, _ask);
                        _ask = 0;
                        if (yes && _chosen >= 0) setValue(_asking, true);
                        return true;
                }
                if (e.source == _terminal && bmds_gui_event_is(gui, &e, BMDS_GUI_TERMINAL_TYPED) && e.count == 1) {
                        if (_ttyFd >= 0) (void)::write(_ttyFd, e.values[0].text, e.values[0].length);
                        return true;
                }
                if (e.source == _spiDevice && bmds_gui_event_is(gui, &e, BMDS_GUI_COMBO_BOX_CHANGED) && e.count == 1) {
                        openSpi(int(e.values[0].number));
                        return true;
                }
                if (e.source == _spiSend && bmds_gui_event_is(gui, &e, BMDS_GUI_LINE_EDIT_ACCEPTED)) {
                        spiTransfer();
                        return true;
                }
                if (!bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED)) return false;
                if (e.source == _apply && _chosen >= 0) {
                        PortWords w;
                        readFields(w);
                        static char value[BMDS_SYSCONF_VALUE_MAX + 1];
                        size_t      bad = MaxFields;
                        if (!joinWords(_ports[_chosen].kind, w, value, sizeof(value), bad)) {
                                size_t           count = 0;
                                const PortField *fields = portFields(_ports[_chosen].kind, count);
                                char             text[64];
                                if (bad < count)
                                        snprintf(text, sizeof(text), "%s no spaces in it", fields[bad].label);
                                else
                                        snprintf(text, sizeof(text), "The setup is too long");
                                say(text);
                                return true;
                        }
                        setValue(value, false);
                        return true;
                }
                if (e.source == _stop && _chosen >= 0) {
                        setValue("", false);
                        return true;
                }
                if (e.source == _save && _chosen >= 0) {
                        const Port   &p = _ports[_chosen];
                        const int32_t r = bmds_sysconf_set(p.key, BMDS_SYSCONF_OP_SAVE, nullptr);
                        char          text[80];
                        snprintf(text, sizeof(text),
                                 r < 0 ? "io/%s couldn't be saved" : "io/%s saved: the port comes up so at boot",
                                 p.name);
                        say(text);
                        refreshPorts();
                        return true;
                }
                if (e.source == _clear) {
                        bmds_terminal_clear(gui, _terminal);
                        return true;
                }
                if (_i2cFd >= 0 && e.source == _i2cScan) {
                        i2cScan();
                        return true;
                }
                if (_i2cFd >= 0 && (e.source == _i2cRead || e.source == _i2cWrite)) {
                        i2cReadWrite(e.source == _i2cWrite);
                        return true;
                }
                if (e.source == _spiGo) {
                        spiTransfer();
                        return true;
                }
                return false;
        }

        size_t PortsTab::pollFds(struct pollfd *fds, size_t max) const {
                size_t n = 0;
                if (_watchFd >= 0 && n < max) fds[n++] = pollfd{_watchFd, POLLIN, 0};
                if (_ttyFd >= 0 && n < max) fds[n++] = pollfd{_ttyFd, POLLIN, 0};
                return n;
        }

        void PortsTab::serviceFds(const struct pollfd *fds, size_t count) {
                for (size_t i = 0; i < count; ++i) {
                        if (fds[i].revents == 0) continue;
                        if (fds[i].fd == _watchFd) {
                                // One read (a second would wait): what's left wakes the poll again.
                                // The list is read again, and the devices once more after the kernel
                                // has started or stopped them; the chosen port's fields only if its
                                // own value changed (else a half-typed edit would go).
                                uint32_t      ids[16];
                                const ssize_t got = ::read(_watchFd, ids, sizeof(ids));
                                bool          mine = false;
                                for (ssize_t k = 0; k + 4 <= got; k += 4) {
                                        const uint32_t id = ids[k / 4];
                                        mine = mine || id == BMDS_SYSCONF_OVERFLOW ||
                                               (_chosen >= 0 && int32_t(id) == _ports[_chosen].key);
                                }
                                refreshPorts();
                                if (mine && _chosen >= 0 && _ports[_chosen].kind != PortKind::I2c) loadFields(true);
                                bmds_timer_start(_gui, _recheck);
                        } else if (fds[i].fd == _ttyFd) {
                                readTty();
                        }
                }
                return;
        }

        void PortsTab::close() {
                closeTool();
                if (_watchFd >= 0) ::close(_watchFd);
                _watchFd = -1;
                return;
        }

} // namespace ioman
