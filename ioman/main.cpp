/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ioman, the I/O Manager (devplan/ioman.md): every pin of the chip, who holds it, what the IO_MUX
 * and the GPIO matrix route through it and its live level (Pins, with the chosen pin's every
 * detail beside the table), and the board's connectors as they are laid out (Board). It reads
 * `/dev/gpiochip0` with `gpio_ctl` (BMDS's slot 139) ten times a second while its window is shown
 * and not minimized, and draws only what changed. One at a time: a second start raises the first.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <bmds/gui.h>
#include <bmds/guiclasses.h>
#include <bmds/sys.h>

#include "pins.h"
#include "setup.h"

using ioman::Pin;
using ioman::PinClass;
using ioman::PinList;

namespace {

        constexpr const char *Service = "ioman";
        constexpr int         RefreshMs = 100; // live levels: ten times a second while shown
        constexpr size_t      MaxPins = PinList::MaxPins;
        constexpr size_t      MaxConnectors = 8;
        constexpr size_t      MaxConnectorPins = 64;
        constexpr size_t      PadsPerRow = 11; // the chip's pads on the Board tab
        constexpr int         SwatchSide = 16;
        constexpr uint8_t     NoSwatch = 0xff;   // a button not given one yet
        constexpr size_t      EdgeLogSize = 32;  // the chosen pin's last edges kept
        constexpr size_t      StripColumns = 32; // its last 2 s, 62.5 ms a column (fits the pane)
        constexpr int64_t     StripNs = 2000000000;
        constexpr int64_t     SayNs = 4000000000; // a message's time in the status bar

        int64_t monotonicNs() {
                struct timespec t;
                clock_gettime(CLOCK_MONOTONIC, &t);
                return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
        }

        constexpr size_t InspectorColumns = 43; // the details' width, in its font's characters
        constexpr size_t LabelColumns = 11;     // `Direction: `

        // A class's colour (ARGB): readable on light and dark faces alike.
        constexpr uint32_t ClassColors[ioman::PinClassCount] = {0xff909090u, 0xffd83030u, 0xff3878e0u, 0xff30b040u};
        constexpr uint32_t SupplyColor = 0xff505050u;
        constexpr uint32_t HighColor = 0xffffffffu;
        constexpr uint32_t LowColor = 0xff000000u;

        // The Show box's entries: the classes, then each connector.
        struct ShowEntry {
                        const char   *text;
                        PinList::Show show;
        };
        constexpr ShowEntry ShowEntries[] = {
                {"All pins", PinList::Show::All},  {"Free", PinList::Show::Free},   {"User", PinList::Show::User},
                {"System", PinList::Show::System}, {"Fixed", PinList::Show::Fixed},
        };
        constexpr size_t ShowEntryCount = sizeof(ShowEntries) / sizeof(ShowEntries[0]);

        // A connector of the board's, with its pins' buttons on the Board tab.
        struct Connector {
                        bmds_gpio_connector_t info{};
                        int32_t               gpio[MaxConnectorPins] = {};
                        char                  label[MaxConnectorPins][16] = {};
                        uint32_t              button[MaxConnectorPins] = {};
                        uint64_t              shown[MaxConnectorPins] = {};  // the hash of what the button says
                        uint8_t               swatch[MaxConnectorPins] = {}; // its swatch shown, NoSwatch before any
        };

        class App {
                public:
                        int run();

                private:
                        bool   open();
                        void   build();
                        void   buildPins(uint32_t tabs);
                        void   buildBoard(uint32_t tabs);
                        void   makeSwatches();
                        void   event(const bmds_gui_event_t &e);
                        void   refresh();
                        void   refreshPins();
                        void   refreshBoard();
                        void   refreshInspector();
                        void   refreshStatus();
                        void   select(int pad, bool fromTable);
                        void   buildEditor(uint32_t side);
                        void   loadEditor();
                        void   enableEditor();
                        bool   editorSetup(ioman::PinSetup &out);
                        void   setEntry(const ioman::PinSetup *setup);
                        void   ask(const ioman::PinSetup &setup, const char *why);
                        size_t edgesText(char *out, size_t size);
                        void   say(const char *text);
                        bool   isMinimized();

                        bmds_gui_t *_gui = nullptr;
                        int         _fd = -1;
                        bool        _done = false;
                        char        _problem[160] = {}; // why there is nothing to show, or ""

                        bmds_gpio_chip_info_t _chip{};
                        Pin                   _pins[MaxPins];
                        size_t                _pinCount = 0;
                        Connector             _connectors[MaxConnectors];
                        size_t                _connectorCount = 0;
                        PinList               _list;
                        int                   _selected = -1;

                        uint32_t _services = 0;
                        uint32_t _window = 0;
                        uint32_t _tabs = 0;
                        uint32_t _status = 0;
                        uint32_t _timer = 0;
                        uint32_t _swatch[ioman::SwatchCount] = {};
                        uint32_t _supply = 0;
                        int32_t  _tab = 0;

                        // Pins.
                        uint32_t _show = 0;
                        uint32_t _table = 0;
                        uint32_t _inspector = 0;
                        size_t   _rowCount = SIZE_MAX;     // the rows the table shows (none yet)
                        uint8_t  _rowOffset[MaxPins] = {}; // each row's pad
                        uint64_t _rowHash[MaxPins] = {};   // each row's words shown (pinHash())
                        uint8_t  _rowSwatch[MaxPins] = {}; // each row's swatch shown
                        uint32_t _detailTicks = 0;
                        char     _statusShown[96] = {};
                        char     _detailShown[BMDS_GPIO_DETAIL_MAX * 200] = {};
                        int64_t  _sayUntilNs = 0; // a message holds the status bar until then

                        // The editor of the chosen pin's io/gpio entry (devplan/ioman.md §3).
                        int32_t         _gpioKey = -1; // io/gpio's sysconf id (-1: this BMDS has none)
                        uint32_t        _mode = 0, _level = 0, _pull = 0, _edges = 0, _debounce = 0, _extra = 0;
                        uint32_t        _apply = 0, _release = 0, _save = 0;
                        uint32_t        _ask = 0; // the box asking to force a held pin, or 0
                        ioman::PinSetup _asking;  // what it asks to set; the entry's force, kept by Apply
                        int             _askingPad = -1;

                        // The chosen pin's edges, while io/gpio watches it.
                        uint32_t      _edgeAfter = 0;
                        ioman::EdgeAt _edgeLog[EdgeLogSize];
                        size_t        _edgeCount = 0;
                        uint32_t      _edgeLost = 0;


                        // Board.
                        uint32_t _padButton[MaxPins] = {};
                        uint8_t  _padSwatch[MaxPins] = {};
                        bool     _padNamed[MaxPins] = {};
        };

        // ---- Reading the chip ----------------------------------------------------------------

        // A text field of the ABI's (NUL-terminated unless full) into ours, cut to fit.
        template <size_t N, size_t M> void copyField(char (&out)[N], const char (&in)[M]) {
                size_t n = 0;
                while (n < M && n + 1 < N && in[n] != '\0') ++n;
                memcpy(out, in, n);
                out[n] = '\0';
                return;
        }

        void copyPin(const bmds_gpio_pin_t &in, Pin &out) {
                out = Pin();
                out.offset = uint8_t(in.offset);
                out.pinClass = in.pin_class < ioman::PinClassCount ? PinClass(in.pin_class) : PinClass::Free;
                out.level = in.level;
                out.peripheral = (in.flags & BMDS_GPIO_PIN_PERIPHERAL) != 0;
                out.watched = (in.flags & BMDS_GPIO_PIN_EDGES) != 0;
                copyField(out.name, in.name);
                copyField(out.owner, in.owner);
                copyField(out.use, in.use);
                copyField(out.route, in.route);
                copyField(out.direction, in.direction);
                copyField(out.pull, in.pull);
                copyField(out.where, in.where);
                return;
        }

        bool App::open() {
                _fd = ::open("/dev/gpiochip0", O_RDONLY);
                if (_fd < 0) {
                        snprintf(_problem, sizeof(_problem), "/dev/gpiochip0: %s (this BMDS is older than gpio_ctl)",
                                 strerror(errno));
                        return false;
                }
                _gpioKey = bmds_sysconf_find("io/gpio"); // -1 on a BMDS without pin setups: read-only
                _chip.size = sizeof(_chip);
                const int32_t r = bmds_gpio_ctl(_fd, BMDS_GPIO_CHIP_INFO, &_chip);
                if (r < 0) {
                        snprintf(_problem, sizeof(_problem), "%s",
                                 r == -BMDS_ENOSYS ? "This BMDS is older than gpio_ctl: update it to see its pins"
                                                   : "/dev/gpiochip0 isn't a GPIO chip");
                        return false;
                }
                for (uint32_t c = 0; c < _chip.connectors && _connectorCount < MaxConnectors; ++c) {
                        Connector &k = _connectors[_connectorCount];
                        k.info.size = sizeof(k.info);
                        k.info.index = c;
                        if (bmds_gpio_ctl(_fd, BMDS_GPIO_CONNECTOR, &k.info) < 0) continue;
                        for (uint32_t p = 0; p < k.info.pins && p < MaxConnectorPins; ++p) {
                                bmds_gpio_connector_pin_t pin{};
                                pin.size = sizeof(pin);
                                pin.connector = c;
                                pin.pin = p;
                                k.gpio[p] = -1;
                                if (bmds_gpio_ctl(_fd, BMDS_GPIO_CONNECTOR_PIN, &pin) < 0) continue;
                                k.gpio[p] = pin.gpio;
                                copyField(k.label[p], pin.label);
                        }
                        ++_connectorCount;
                }
                return true;
        }

        // ---- Building ------------------------------------------------------------------------

        // A swatch: the class's colour around the level (white high, black low; none: solid).
        uint32_t swatchImage(bmds_gui_t *gui, uint32_t color, int level) {
                static uint32_t argb[SwatchSide * SwatchSide];
                const int       in = 5;
                for (int y = 0; y < SwatchSide; ++y) {
                        for (int x = 0; x < SwatchSide; ++x) {
                                const bool edge = x < 2 || y < 2 || x >= SwatchSide - 2 || y >= SwatchSide - 2;
                                const bool inside = x >= in && y >= in && x < SwatchSide - in && y < SwatchSide - in;
                                uint32_t   c = edge ? 0x00000000u : color; // a clear margin around it
                                if (!edge && inside && level == 0) c = HighColor;
                                if (!edge && inside && level == 1) c = LowColor;
                                argb[y * SwatchSide + x] = c;
                        }
                }
                const uint32_t image = bmds_gui_new(gui, "Image", 0);
                bmds_gui_put_image(gui, image, SwatchSide, SwatchSide, argb);
                return image;
        }

        void App::makeSwatches() {
                for (unsigned c = 0; c < ioman::PinClassCount; ++c)
                        for (int level = 0; level < 3; ++level)
                                _swatch[c * 3 + unsigned(level)] = swatchImage(_gui, ClassColors[c], level);
                _supply = swatchImage(_gui, SupplyColor, 2);
                return;
        }

        void App::buildPins(uint32_t tabs) {
                const uint32_t page = bmds_tab_page_new(_gui, tabs);
                bmds_tab_page_set_title(_gui, page, "Pins");
                bmds_box_set_margin(_gui, page, 4);
                bmds_box_set_spacing(_gui, page, 4);
                const uint32_t bar = bmds_box_new(_gui, page);
                bmds_box_set_direction(_gui, bar, "across");
                bmds_box_set_spacing(_gui, bar, 4);
                const uint32_t label = bmds_label_new(_gui, bar);
                bmds_label_set_text(_gui, label, "Show:");
                _show = bmds_combo_box_new(_gui, bar);
                for (const ShowEntry &s : ShowEntries) bmds_combo_box_add_item(_gui, _show, s.text);
                for (size_t c = 0; c < _connectorCount; ++c) {
                        char text[40];
                        snprintf(text, sizeof(text), "On %.*s", int(sizeof(_connectors[c].info.name)),
                                 _connectors[c].info.name);
                        bmds_combo_box_add_item(_gui, _show, text);
                }
                bmds_combo_box_set_current(_gui, _show, 0);

                const uint32_t split = bmds_splitter_new(_gui, page);
                bmds_widget_set_stretch(_gui, split, 1);
                bmds_splitter_set_direction(_gui, split, "across");
                bmds_splitter_set_min_first(_gui, split, 320);
                bmds_splitter_set_min_second(_gui, split, 200);
                _table = bmds_details_view_new(_gui, split);
                static const char *const Columns[PinList::ColumnCount] = {"GPIO",  "Class",     "Owner",
                                                                          "Route", "Direction", "Where"};
                static constexpr int     Widths[PinList::ColumnCount] = {88, 60, 180, 152, 96, 64};
                bmds_details_view_set_columns(_gui, _table, Columns, PinList::ColumnCount);
                for (size_t c = 0; c < PinList::ColumnCount; ++c)
                        bmds_details_view_set_column_width(_gui, _table, int32_t(c), Widths[c]);
                bmds_details_view_set_sort_indicator(_gui, _table, int32_t(_list.sortColumn()), _list.isDescending());
                const uint32_t side = bmds_box_new(_gui, split);
                bmds_box_set_spacing(_gui, side, 4);
                buildEditor(side);
                _inspector = bmds_text_edit_new(_gui, side);
                bmds_widget_set_stretch(_gui, _inspector, 1);
                bmds_text_edit_set_read_only(_gui, _inspector, 1);
                bmds_text_edit_set_text(_gui, _inspector, "Choose a pin.");
                bmds_splitter_set_position(_gui, split, 600);
                return;
        }

        // A row of the editor's form: its label, then @p make's widget in it.
        template <typename Make> uint32_t formRow(bmds_gui_t *gui, uint32_t form, const char *label, Make make) {
                const uint32_t row = bmds_form_row_new(gui, form);
                bmds_form_row_set_label(gui, row, label);
                return make(row);
        }

        void App::buildEditor(uint32_t side) {
                const uint32_t form = bmds_form_new(_gui, side);
                const uint32_t section = bmds_form_section_new(_gui, form);
                bmds_form_section_set_title(_gui, section, "Set up (io/gpio)");
                static const char *const Modes[] = {"Not set up", "Input", "Output", "Open drain", "Off"};
                static const char *const Levels[] = {"Low", "High"};
                static const char *const Pulls[] = {"None", "Up", "Down", "Up and down"};
                static const char *const Edges[] = {"Not watched", "Both", "Rising", "Falling"};
                const auto               combo = [this](const char *const *items, size_t n) {
                        return [this, items, n](uint32_t row) {
                                const uint32_t box = bmds_combo_box_new(_gui, row);
                                for (size_t i = 0; i < n; ++i) bmds_combo_box_add_item(_gui, box, items[i]);
                                bmds_combo_box_set_current(_gui, box, 0);
                                return box;
                        };
                };
                _mode = formRow(_gui, form, "Mode:", combo(Modes, 5));
                _level = formRow(_gui, form, "Level:", combo(Levels, 2));
                _pull = formRow(_gui, form, "Pull:", combo(Pulls, 4));
                _edges = formRow(_gui, form, "Edges:", combo(Edges, 4));
                _debounce = formRow(_gui, form, "Debounce (us):", [this](uint32_t row) {
                        const uint32_t f = bmds_number_field_new(_gui, row);
                        bmds_number_field_set_minimum(_gui, f, 0);
                        bmds_number_field_set_maximum(_gui, f, 1000000);
                        bmds_number_field_set_step(_gui, f, 1000);
                        return f;
                });
                _extra = formRow(_gui, form, "Other words:", [this](uint32_t row) {
                        const uint32_t f = bmds_line_edit_new(_gui, row);
                        bmds_widget_set_stretch(_gui, f, 1);
                        return f;
                });
                const uint32_t buttons = bmds_box_new(_gui, side);
                bmds_box_set_direction(_gui, buttons, "across");
                bmds_box_set_spacing(_gui, buttons, 4);
                _apply = bmds_button_new(_gui, buttons);
                bmds_button_set_text(_gui, _apply, "&Apply");
                _release = bmds_button_new(_gui, buttons);
                bmds_button_set_text(_gui, _release, "&Release");
                _save = bmds_button_new(_gui, buttons);
                bmds_button_set_text(_gui, _save, "&Save");
                enableEditor();
                return;
        }

        // The fields the chosen mode uses, and the buttons a pin and a BMDS with io/gpio allow.
        void App::enableEditor() {
                bmds_gui_value_t v;
                const int        mode = bmds_combo_box_get_current(_gui, _mode, &v) == 0 ? int(v.number) : 0;
                const bool       can = _gpioKey >= 0 && _selected >= 0;
                const bool       output =
                        mode == int(ioman::PinSetup::Mode::Out) || mode == int(ioman::PinSetup::Mode::OpenDrain);
                const bool input =
                        mode == int(ioman::PinSetup::Mode::In) || mode == int(ioman::PinSetup::Mode::OpenDrain);
                bmds_widget_set_enabled(_gui, _mode, can);
                bmds_widget_set_enabled(_gui, _level, can && output);
                bmds_widget_set_enabled(_gui, _pull, can && mode != 0);
                bmds_widget_set_enabled(_gui, _edges, can && input);
                bmds_widget_set_enabled(_gui, _debounce, can && input);
                bmds_widget_set_enabled(_gui, _extra, can && mode != 0);
                bmds_widget_set_enabled(_gui, _apply, can);
                bmds_widget_set_enabled(_gui, _release, can);
                bmds_widget_set_enabled(_gui, _save, _gpioKey >= 0);
                return;
        }

        // The editor filled from the chosen pin's io/gpio entry (none: "Not set up").
        void App::loadEditor() {
                ioman::PinSetup s;
                if (_gpioKey >= 0 && _selected >= 0) {
                        static char value[BMDS_SYSCONF_VALUE_MAX + 1];
                        if (bmds_sysconf_get(_gpioKey, BMDS_SYSCONF_FIELD_VALUE, value, sizeof(value)) >= 0)
                                (void)ioman::findEntry(value, unsigned(_selected), s);
                }
                bmds_combo_box_set_current(_gui, _mode, int32_t(s.mode));
                bmds_combo_box_set_current(_gui, _level, s.high ? 1 : 0);
                bmds_combo_box_set_current(_gui, _pull, int32_t(s.pull));
                bmds_combo_box_set_current(_gui, _edges, int32_t(s.edges));
                bmds_number_field_set_value(_gui, _debounce, int32_t(s.debounceUs));
                bmds_line_edit_set_text(_gui, _extra, s.extra);
                _asking = s; // its force kept by Apply
                enableEditor();
                return;
        }

        // The editor's fields as an entry. @return False for "Not set up".
        bool App::editorSetup(ioman::PinSetup &out) {
                bmds_gui_value_t v;
                const auto       current = [&](uint32_t box) {
                        return bmds_combo_box_get_current(_gui, box, &v) == 0 ? int(v.number) : 0;
                };
                const bool force = _asking.force; // forced before: still
                out = ioman::PinSetup();
                out.mode = ioman::PinSetup::Mode(current(_mode));
                if (out.mode == ioman::PinSetup::Mode::None) return false;
                out.high = current(_level) == 1;
                out.pull = ioman::PinSetup::Pull(current(_pull));
                const bool input =
                        out.mode == ioman::PinSetup::Mode::In || out.mode == ioman::PinSetup::Mode::OpenDrain;
                out.edges = input ? ioman::PinSetup::Edges(current(_edges)) : ioman::PinSetup::Edges::None;
                if (input && bmds_number_field_get_value(_gui, _debounce, &v) == 0 && v.number > 0)
                        out.debounceUs = uint32_t(v.number);
                if (bmds_line_edit_get_text(_gui, _extra, &v) == 0)
                        snprintf(out.extra, sizeof(out.extra), "%.*s", int(v.length), v.text);
                out.force = force;
                return true;
        }

        // io/gpio with the chosen pin's entry set to @p setup (null: taken out), as `sysconf set`
        // would; a pin another owner holds asks first.
        void App::setEntry(const ioman::PinSetup *setup) {
                static char value[BMDS_SYSCONF_VALUE_MAX + 1];
                static char next[BMDS_SYSCONF_VALUE_MAX + 1];
                if (_gpioKey < 0 || _selected < 0) return;
                if (bmds_sysconf_get(_gpioKey, BMDS_SYSCONF_FIELD_VALUE, value, sizeof(value)) < 0) value[0] = '\0';
                if (!ioman::withEntry(value, unsigned(_selected), setup, next, sizeof(next))) {
                        say("io/gpio would be too long");
                        return;
                }
                const int32_t r = bmds_sysconf_set(_gpioKey, BMDS_SYSCONF_OP_SET, next);
                if (r < 0) {
                        // The kernel's own words (a BMDS before them: the error's).
                        char why[256] = {};
                        if (bmds_sysconf_get(_gpioKey, BMDS_SYSCONF_FIELD_REFUSAL, why, sizeof(why)) < 0 ||
                            why[0] == '\0')
                                snprintf(why, sizeof(why), "io/gpio: %s", strerror(-r));
                        if (r == -BMDS_EBUSY && setup != nullptr && !setup->force) {
                                ask(*setup, why);
                                return;
                        }
                        say(why);
                        return;
                }
                char text[64];
                snprintf(text, sizeof(text), setup != nullptr ? "GPIO%d set up" : "GPIO%d released", _selected);
                say(text);
                loadEditor();
                _detailShown[0] = '\0';
                refresh();
                return;
        }

        // Asks before taking a pin from its holder: the kernel's reason, and Yes to force it.
        void App::ask(const ioman::PinSetup &setup, const char *why) {
                if (_ask != 0) bmds_gui_destroy(_gui, _ask);
                _asking = setup;
                _askingPad = _selected;
                char text[384];
                snprintf(text, sizeof(text), "%s\n\nChange GPIO%d anyway?", why, _selected);
                _ask = bmds_gui_new(_gui, "MessageBox", 0);
                bmds_message_box_set_kind(_gui, _ask, "warning");
                bmds_message_box_set_title(_gui, _ask, "A pin in use");
                bmds_message_box_set_text(_gui, _ask, text);
                bmds_message_box_set_buttons(_gui, _ask, "yesNo");
                bmds_message_box_set_owner(_gui, _ask, _window);
                bmds_message_box_open(_gui, _ask);
                return;
        }

        void App::buildBoard(uint32_t tabs) {
                memset(_padSwatch, NoSwatch, sizeof(_padSwatch));
                for (size_t c = 0; c < _connectorCount; ++c)
                        memset(_connectors[c].swatch, NoSwatch, sizeof(_connectors[c].swatch));
                const uint32_t page = bmds_tab_page_new(_gui, tabs);
                bmds_tab_page_set_title(_gui, page, "Board");
                // A form scrolls: the connectors, then the chip's pads, each a row of it.
                const uint32_t form = bmds_form_new(_gui, page);
                bmds_widget_set_stretch(_gui, form, 1);
                for (size_t c = 0; c < _connectorCount; ++c) {
                        Connector     &k = _connectors[c];
                        const uint32_t section = bmds_form_section_new(_gui, form);
                        char           title[96];
                        snprintf(title, sizeof(title), "%.*s%s", int(sizeof(k.info.description)), k.info.description,
                                 (k.info.flags & BMDS_GPIO_CONNECTOR_FITTED) != 0 ? "" : " (not fitted)");
                        bmds_form_section_set_title(_gui, section, title);
                        const uint32_t columns = k.info.columns > 0 ? k.info.columns : 1;
                        uint32_t       row = 0;
                        for (uint32_t p = 0; p < k.info.pins && p < MaxConnectorPins; ++p) {
                                if (p % columns == 0) {
                                        row = bmds_box_new(_gui, form);
                                        bmds_box_set_direction(_gui, row, "across");
                                        bmds_box_set_spacing(_gui, row, 4);
                                }
                                k.button[p] = bmds_button_new(_gui, row);
                                bmds_widget_set_stretch(_gui, k.button[p], 1);
                                bmds_button_set_centered(_gui, k.button[p], 0);
                                if (k.gpio[p] < 0) {
                                        char text[32];
                                        snprintf(text, sizeof(text), "%2u  %s", unsigned(p + 1),
                                                 k.label[p][0] != '\0' ? k.label[p] : "-");
                                        bmds_button_set_text(_gui, k.button[p], text);
                                        bmds_button_set_icon_image(_gui, k.button[p], _supply);
                                        bmds_widget_set_enabled(_gui, k.button[p], 0);
                                }
                        }
                }
                const uint32_t section = bmds_form_section_new(_gui, form);
                char           title[96];
                snprintf(title, sizeof(title), "%.*s: every pad", int(sizeof(_chip.label)), _chip.label);
                bmds_form_section_set_title(_gui, section, title);
                uint32_t row = 0;
                for (size_t p = 0; p < _chip.lines && p < MaxPins; ++p) {
                        if (p % PadsPerRow == 0) {
                                row = bmds_box_new(_gui, form);
                                bmds_box_set_direction(_gui, row, "across");
                                bmds_box_set_spacing(_gui, row, 4);
                        }
                        _padButton[p] = bmds_button_new(_gui, row);
                        bmds_widget_set_stretch(_gui, _padButton[p], 1);
                        bmds_button_set_centered(_gui, _padButton[p], 0);
                }
                const uint32_t legend = bmds_label_new(_gui, form);
                bmds_label_set_text(
                        _gui, legend,
                        "Grey: free  ·  green: user  ·  blue: system  ·  red: fixed  ·  inside, white: high, "
                        "black: low");
                return;
        }

        void App::build() {
                _window = bmds_window_new(_gui, 0);
                bmds_window_set_title(_gui, _window, "I/O Manager");
                bmds_window_set_width(_gui, _window, 1000);
                bmds_window_set_height(_gui, _window, 600);
                bmds_window_set_min_width(_gui, _window, 480);
                bmds_window_set_min_height(_gui, _window, 240);
                makeSwatches();
                _tabs = bmds_tabs_new(_gui, _window);
                bmds_widget_set_stretch(_gui, _tabs, 1);
                _status = bmds_status_bar_new(_gui, _window);
                bmds_status_bar_add_field(_gui, _status, 0, nullptr);
                bmds_status_bar_add_field(_gui, _status, 300, nullptr);
                if (_problem[0] == '\0') {
                        buildPins(_tabs);
                        buildBoard(_tabs);
                        char text[128];
                        snprintf(text, sizeof(text), "%.*s on %.*s", int(sizeof(_chip.label)), _chip.label,
                                 int(sizeof(_chip.board)), _chip.board);
                        bmds_status_bar_set_field(_gui, _status, 1, text);
                } else {
                        const uint32_t page = bmds_tab_page_new(_gui, _tabs);
                        bmds_tab_page_set_title(_gui, page, "Pins");
                        bmds_box_set_margin(_gui, page, 8);
                        const uint32_t label = bmds_label_new(_gui, page);
                        bmds_label_set_text(_gui, label, "The I/O Manager can't read this board's pins.");
                }
                _timer = bmds_timer_new(_gui, 0);
                bmds_timer_set_interval(_gui, _timer, RefreshMs);
                bmds_timer_set_repeat(_gui, _timer, 1);
                return;
        }

        // ---- Running -------------------------------------------------------------------------

        bool App::isMinimized() {
                bmds_gui_value_t v;
                return bmds_window_get_minimized(_gui, _window, &v) == 0 && v.number != 0;
        }

        void App::refresh() {
                if (_fd < 0) return;
                static bmds_gpio_pin_t raw[MaxPins];
                bmds_gpio_pins_t       request{};
                request.size = sizeof(request);
                request.first = 0;
                request.count = MaxPins;
                request.pin_size = sizeof(raw[0]);
                request.pins = raw;
                const int32_t n = bmds_gpio_ctl(_fd, BMDS_GPIO_PINS, &request);
                if (n < 0) {
                        say("The pins can't be read");
                        return;
                }
                _pinCount = size_t(n) < MaxPins ? size_t(n) : MaxPins;
                for (size_t i = 0; i < _pinCount; ++i) copyPin(raw[i], _pins[i]);
                _list.update(_pins, _pinCount);
                if (_tab == 0) {
                        refreshPins();
                        refreshInspector();
                } else {
                        refreshBoard();
                }
                refreshStatus();
                return;
        }

        // A row's texts: the columns, in order.
        void rowTexts(const Pin &p, char (&owner)[ioman::OwnerBytes], char (&direction)[ioman::DirectionBytes],
                      const char *(&texts)[PinList::ColumnCount]) {
                ioman::ownerText(p, owner);
                ioman::directionText(p, direction);
                texts[0] = p.name;
                texts[1] = ioman::pinClassName(p.pinClass);
                texts[2] = owner;
                texts[3] = p.route;
                texts[4] = direction;
                texts[5] = p.where;
                return;
        }

        void App::refreshPins() {
                // The same pads in the same order: each row changed in place (DetailsView.setRow),
                // its words or its icon only when they moved, the table's scroll and selection
                // kept. Other pads or another order (a filter, a sort): the table again.
                bool same = _list.count() == _rowCount;
                for (size_t i = 0; same && i < _list.count(); ++i) same = _list.at(i).offset == _rowOffset[i];
                char        owner[ioman::OwnerBytes];
                char        direction[ioman::DirectionBytes];
                const char *texts[PinList::ColumnCount];
                if (same) {
                        for (size_t i = 0; i < _list.count(); ++i) {
                                const Pin     &p = _list.at(i);
                                const uint64_t h = ioman::pinHash(p);
                                if (h != _rowHash[i]) {
                                        _rowHash[i] = h;
                                        rowTexts(p, owner, direction, texts);
                                        bmds_details_view_set_row(_gui, _table, int32_t(i), texts,
                                                                  PinList::ColumnCount);
                                }
                                const uint8_t swatch = uint8_t(ioman::swatchOf(p));
                                if (swatch == _rowSwatch[i]) continue;
                                _rowSwatch[i] = swatch;
                                bmds_item_view_set_icon_image(_gui, _table, int32_t(i), _swatch[swatch]);
                        }
                        return;
                }
                bmds_item_view_clear(_gui, _table);
                _rowCount = _list.count();
                for (size_t i = 0; i < _list.count(); ++i) {
                        const Pin &p = _list.at(i);
                        rowTexts(p, owner, direction, texts);
                        bmds_details_view_add_row(_gui, _table, texts, PinList::ColumnCount);
                        _rowOffset[i] = p.offset;
                        _rowHash[i] = ioman::pinHash(p);
                        _rowSwatch[i] = uint8_t(ioman::swatchOf(p));
                        bmds_item_view_set_icon_image(_gui, _table, int32_t(i), _swatch[_rowSwatch[i]]);
                }
                const int row = _selected >= 0 ? _list.rowOf(unsigned(_selected)) : -1;
                bmds_item_view_set_current(_gui, _table, row);
                return;
        }

        void App::refreshInspector() {
                static bmds_gpio_pin_detail_t detail;
                static char                   text[sizeof(_detailShown)];
                if (_selected < 0) return;
                // A peripheral's pad: its level is a sample of a running line, so once a second.
                if (size_t(_selected) < _pinCount && _pins[_selected].peripheral && _detailShown[0] != '\0' &&
                    ++_detailTicks % 10 != 0)
                        return;
                memset(&detail, 0, sizeof(detail));
                detail.size = sizeof(detail);
                detail.offset = uint32_t(_selected);
                if (bmds_gpio_ctl(_fd, BMDS_GPIO_PIN_DETAIL, &detail) < 0) return;
                // The text editor doesn't wrap: each line is broken here at the pane's width, at a
                // space where there is one, its continuation under the text, past the labels.
                size_t n = size_t(snprintf(text, sizeof(text), "GPIO%d\n", _selected));
                // A watched pin's edges first, in sight; then every detail.
                if (size_t(_selected) < _pinCount && _pins[_selected].watched)
                        n += edgesText(text + n, sizeof(text) - n);
                if (n < sizeof(text)) n += size_t(snprintf(text + n, sizeof(text) - n, "\n"));
                for (uint32_t i = 0; i < detail.count && i < BMDS_GPIO_DETAIL_MAX; ++i) {
                        char label[20];
                        snprintf(label, sizeof(label), "%.15s:", detail.lines[i].label);
                        const char *t = detail.lines[i].text;
                        size_t      left = strnlen(t, sizeof(detail.lines[i].text));
                        bool        first = true;
                        do {
                                size_t take = left;
                                if (take > InspectorColumns - LabelColumns) {
                                        take = InspectorColumns - LabelColumns;
                                        while (take > 8 && t[take] != ' ') --take;
                                        if (t[take] != ' ') take = InspectorColumns - LabelColumns;
                                }
                                if (n < sizeof(text))
                                        n += size_t(snprintf(text + n, sizeof(text) - n, "%-*s%.*s\n",
                                                             int(LabelColumns), first ? label : "", int(take), t));
                                t += take;
                                left -= take;
                                while (left > 0 && *t == ' ') {
                                        ++t;
                                        --left;
                                }
                                first = false;
                        } while (left > 0);
                }
                // The same as shown: nothing sent (the reader's place in it kept).
                if (strcmp(text, _detailShown) == 0) return;
                snprintf(_detailShown, sizeof(_detailShown), "%s", text);
                bmds_text_edit_set_text(_gui, _inspector, text);
                return;
        }

        // The chosen pin's newest edges into the log, then a strip of its last 2 s and the last few.
        size_t App::edgesText(char *out, size_t size) {
                static bmds_gpio_edge_t got[16];
                for (;;) {
                        bmds_gpio_edges_t e{};
                        e.size = sizeof(e);
                        e.offset = uint32_t(_selected);
                        e.after = _edgeAfter;
                        e.count = 16;
                        e.edge_size = sizeof(got[0]);
                        e.edges = got;
                        const int32_t n = bmds_gpio_ctl(_fd, BMDS_GPIO_EDGES, &e);
                        if (n < 0) break;
                        _edgeLost += e.lost;
                        for (int32_t i = 0; i < n; ++i) {
                                if (_edgeCount == EdgeLogSize) {
                                        memmove(_edgeLog, _edgeLog + 1, sizeof(_edgeLog[0]) * (EdgeLogSize - 1));
                                        --_edgeCount;
                                }
                                _edgeLog[_edgeCount++] = ioman::EdgeAt{got[i].time_ns, got[i].rising != 0};
                        }
                        _edgeAfter = e.next;
                        if (n < 16) break;
                }
                bool strip[StripColumns];
                ioman::levelStrip(_pins[_selected].level == '1', _edgeLog, _edgeCount, monotonicNs(), StripNs, strip,
                                  StripColumns);
                size_t k = size_t(snprintf(out, size, "\nLast 2 s: "));
                for (size_t c = 0; c < StripColumns && k + 4 < size; ++c) {
                        // ▔ high, ▁ low (three bytes each).
                        memcpy(out + k, strip[c] ? "\xe2\x96\x94" : "\xe2\x96\x81", 3);
                        k += 3;
                }
                if (k < size) k += size_t(snprintf(out + k, size - k, "\n"));
                if (_edgeLost != 0 && k < size)
                        k += size_t(snprintf(out + k, size - k, "(%u lost)\n", unsigned(_edgeLost)));
                const size_t shown = _edgeCount < 6 ? _edgeCount : 6;
                for (size_t i = 0; i < shown && k < size; ++i) {
                        const ioman::EdgeAt &e = _edgeLog[_edgeCount - 1 - i];
                        k += size_t(snprintf(out + k, size - k, "  %-8s at %.3f s\n", e.rising ? "rising" : "falling",
                                             double(e.timeNs) / 1e9));
                }
                if (_edgeCount == 0 && k < size) k += size_t(snprintf(out + k, size - k, "  no edges yet\n"));
                return k < size ? k : size - 1;
        }

        void App::refreshBoard() {
                char text[96];
                for (size_t c = 0; c < _connectorCount; ++c) {
                        Connector &k = _connectors[c];
                        for (uint32_t p = 0; p < k.info.pins && p < MaxConnectorPins; ++p) {
                                const int32_t g = k.gpio[p];
                                if (g < 0 || size_t(g) >= _pinCount) continue;
                                const Pin     &pin = _pins[g];
                                const uint64_t h = ioman::pinHash(pin);
                                const uint8_t  swatch = uint8_t(ioman::swatchOf(pin));
                                if (h != k.shown[p]) {
                                        k.shown[p] = h;
                                        char owner[ioman::OwnerBytes];
                                        ioman::ownerText(pin, owner);
                                        snprintf(text, sizeof(text), "%2u  %s  %s", unsigned(p + 1), pin.name, owner);
                                        bmds_button_set_text(_gui, k.button[p], text);
                                }
                                if (swatch != k.swatch[p]) {
                                        k.swatch[p] = swatch;
                                        bmds_button_set_icon_image(_gui, k.button[p], _swatch[swatch]);
                                }
                        }
                }
                for (size_t p = 0; p < _pinCount; ++p) {
                        if (_padButton[p] == 0) continue;
                        if (!_padNamed[p]) {
                                _padNamed[p] = true;
                                snprintf(text, sizeof(text), "%u", unsigned(_pins[p].offset));
                                bmds_button_set_text(_gui, _padButton[p], text);
                        }
                        const uint8_t swatch = uint8_t(ioman::swatchOf(_pins[p]));
                        if (swatch == _padSwatch[p]) continue;
                        _padSwatch[p] = swatch;
                        bmds_button_set_icon_image(_gui, _padButton[p], _swatch[swatch]);
                }
                return;
        }

        void App::refreshStatus() {
                char text[96];
                snprintf(text, sizeof(text), "%u pins: %u free, %u user, %u system, %u fixed", unsigned(_list.total()),
                         unsigned(_list.classCount(PinClass::Free)), unsigned(_list.classCount(PinClass::User)),
                         unsigned(_list.classCount(PinClass::System)), unsigned(_list.classCount(PinClass::Fixed)));
                if (strcmp(text, _statusShown) == 0 || monotonicNs() < _sayUntilNs) return;
                snprintf(_statusShown, sizeof(_statusShown), "%s", text);
                bmds_status_bar_set_field(_gui, _status, 0, text);
                return;
        }

        void App::say(const char *text) {
                _statusShown[0] = '\0'; // the counts again once it has been read
                _sayUntilNs = monotonicNs() + SayNs;
                bmds_status_bar_set_field(_gui, _status, 0, text);
                return;
        }

        void App::select(int pad, bool fromTable) {
                if (pad == _selected && fromTable) return;
                _selected = pad;
                _edgeAfter = 0;
                _edgeCount = 0;
                _edgeLost = 0;
                _detailShown[0] = '\0';
                if (!fromTable) bmds_item_view_set_current(_gui, _table, _list.rowOf(unsigned(pad)));
                loadEditor();
                refreshInspector();
                return;
        }

        void App::event(const bmds_gui_event_t &e) {
                bmds_gui_t *gui = _gui;
                if (e.source == _window && (bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_CLOSED) ||
                                            bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_CLOSE_REQUESTED))) {
                        _done = true;
                        return;
                }
                if (e.source == _services && bmds_gui_event_is(gui, &e, BMDS_GUI_SERVICES_MESSAGE)) {
                        // Any message raises it ("raise": a second start).
                        bmds_window_show(gui, _window);
                        bmds_window_raise(gui, _window);
                        return;
                }
                if (e.source == _ask && _ask != 0 && bmds_gui_event_is(gui, &e, BMDS_GUI_MESSAGE_BOX_ANSWERED) &&
                    e.count == 1) {
                        const bool yes = strcmp(bmds_gui_atom_name(gui, uint32_t(e.values[0].number)), "yes") == 0;
                        bmds_gui_destroy(gui, _ask);
                        _ask = 0;
                        if (yes && _askingPad == _selected) {
                                _asking.force = true;
                                const ioman::PinSetup forced = _asking;
                                setEntry(&forced);
                        }
                        return;
                }
                if (bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED) &&
                    (e.source == _apply || e.source == _release || e.source == _save)) {
                        if (e.source == _save) {
                                const int32_t r = bmds_sysconf_set(_gpioKey, BMDS_SYSCONF_OP_SAVE, nullptr);
                                say(r < 0 ? "io/gpio couldn't be saved" : "io/gpio saved: the pins come up so at boot");
                        } else if (e.source == _release) {
                                setEntry(nullptr);
                        } else {
                                ioman::PinSetup s;
                                if (editorSetup(s))
                                        setEntry(&s);
                                else
                                        setEntry(nullptr);
                        }
                        return;
                }
                if (e.source == _mode && bmds_gui_event_is(gui, &e, BMDS_GUI_COMBO_BOX_CHANGED)) {
                        enableEditor();
                        return;
                }
                if (e.source == _timer && bmds_gui_event_is(gui, &e, BMDS_GUI_TIMER_TIMEOUT)) {
                        // Nobody sees it: nothing read.
                        if (!isMinimized()) refresh();
                        return;
                }
                if (e.source == _tabs && bmds_gui_event_is(gui, &e, BMDS_GUI_TABS_CHANGED) && e.count == 1) {
                        _tab = int32_t(e.values[0].number);
                        refresh();
                        return;
                }
                if (e.source == _show && bmds_gui_event_is(gui, &e, BMDS_GUI_COMBO_BOX_CHANGED) && e.count == 1) {
                        const size_t i = size_t(e.values[0].number);
                        if (i < ShowEntryCount)
                                _list.setShow(ShowEntries[i].show);
                        else if (i - ShowEntryCount < _connectorCount)
                                _list.setShow(PinList::Show::Connector, _connectors[i - ShowEntryCount].info.name);
                        refresh();
                        return;
                }
                if (e.source == _table && bmds_gui_event_is(gui, &e, BMDS_GUI_DETAILS_VIEW_SORT_ASKED) &&
                    e.count == 1) {
                        const size_t column = size_t(e.values[0].number);
                        const bool   down = column == _list.sortColumn() ? !_list.isDescending() : false;
                        _list.setSort(column, down);
                        bmds_details_view_set_sort_indicator(gui, _table, int32_t(column), down);
                        refresh();
                        return;
                }
                if (e.source == _table && bmds_gui_event_is(gui, &e, BMDS_GUI_ITEM_VIEW_CURRENT_CHANGED) &&
                    e.count == 1) {
                        const int row = int(e.values[0].number);
                        if (row >= 0 && size_t(row) < _list.count()) select(_list.at(size_t(row)).offset, true);
                        return;
                }
                if (bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED)) {
                        // A pin on the Board tab: to its row on Pins, every pin shown so it is there.
                        int pad = -1;
                        for (size_t p = 0; p < _pinCount && pad < 0; ++p)
                                if (_padButton[p] == e.source) pad = int(p);
                        for (size_t c = 0; c < _connectorCount && pad < 0; ++c)
                                for (uint32_t p = 0; p < _connectors[c].info.pins && p < MaxConnectorPins; ++p)
                                        if (_connectors[c].button[p] == e.source) pad = _connectors[c].gpio[p];
                        if (pad < 0) return;
                        _list.setShow(PinList::Show::All);
                        bmds_combo_box_set_current(gui, _show, 0);
                        bmds_tabs_set_current(gui, _tabs, 0);
                        _tab = 0;
                        _rowCount = SIZE_MAX; // the table again, every pin in it
                        _selected = -1;       // select() below: as any other choice
                        refresh();
                        select(pad, false);
                }
                return;
        }

        int App::run() {
                _gui = bmds_gui_connect(nullptr);
                if (_gui == nullptr) {
                        fprintf(stderr, "ioman: no GUI (%s)\n", strerror(errno));
                        return 1;
                }
                // One at a time: a second asks the first to come forward, and ends.
                _services = bmds_services_new(_gui, 0);
                bmds_gui_value_t running;
                if (bmds_services_has(_gui, _services, Service, &running) == 0 && running.number != 0) {
                        bmds_services_send(_gui, _services, Service, "raise", 5);
                        bmds_gui_close(_gui);
                        return 0;
                }
                bmds_services_register(_gui, _services, Service);
                if (!open()) fprintf(stderr, "ioman: %s\n", _problem);
                build();
                const uint32_t closeRequested = bmds_gui_atom(_gui, BMDS_GUI_WINDOW_CLOSE_REQUESTED);
                bmds_gui_listen(_gui, _window, &closeRequested, 1, 1);
                bmds_window_show(_gui, _window);
                if (_problem[0] != '\0') {
                        say(_problem);
                } else {
                        refresh();
                        bmds_timer_start(_gui, _timer);
                }
                while (!_done) {
                        bmds_gui_event_t e;
                        if (bmds_gui_flush(_gui) != 0) break;
                        const int r = bmds_gui_next_event(_gui, &e, -1);
                        if (r < 0) break;
                        if (r > 0) event(e);
                }
                if (_fd >= 0) ::close(_fd);
                bmds_gui_close(_gui);
                return 0;
        }

} // namespace

int main(int argc, char **argv) {
        (void)argv;
        if (argc > 1) {
                fprintf(stderr, "usage: ioman\n");
                return 2;
        }
        static App app; // large (the pins, the connectors): not on the stack
        return app.run();
}
