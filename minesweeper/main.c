/**
 * @file      main.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Minesweeper for BMDS: a window from the GUI service (bmds/gui.h, through the generated
 * wrappers of bmds/guiclasses.h) with a menu bar, a canvas the board is drawn into, and a status
 * bar. The rules are field.c's, the pictures draw.c's.
 *
 * Play: left click reveals, right click flags, a click on an open number whose flags match it
 * reveals the rest around it (also the middle button, or both). The arrows move a cursor; Space
 * reveals, F flags. F2 starts again. The best time of each level is kept in the application's
 * data folder.
 */

#include <bmds/app.h>
#include <bmds/gui.h>
#include <bmds/guiclasses.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "draw.h"
#include "field.h"

#ifndef MINESWEEPER_VERSION
#define MINESWEEPER_VERSION "dev"
#endif

// The levels: a standard three, and the player's own.
enum {
        BEGINNER,
        INTERMEDIATE,
        EXPERT,
        CUSTOM,
        LEVELS
};

static const struct {
                const char *name;
                int         width, height, mines;
} Levels[LEVELS] = {
        {"Beginner", 9, 9, 10}, {"Intermediate", 16, 16, 40}, {"Expert", 30, 16, 99}, {"Custom", 12, 12, 20}};

// HID usages the canvas's key event carries.
enum {
        KEY_F = 0x09,
        KEY_ENTER = 0x28,
        KEY_SPACE = 0x2c,
        KEY_RIGHT = 0x4f,
        KEY_LEFT = 0x50,
        KEY_DOWN = 0x51,
        KEY_UP = 0x52
};

// Mouse buttons, as the canvas's events carry them.
enum {
        BUTTON_LEFT = 1,
        BUTTON_RIGHT = 2,
        BUTTON_MIDDLE = 4
};

// The side a cell is drawn at when the window is made for a level.
#define CELL_SIDE 26

typedef struct {
                bmds_gui_t *gui;
                uint32_t    window, canvas, status, timer;
                uint32_t    newItem, levelItems[LEVELS], bestItem, quitItem, howItem, aboutItem;
                uint32_t    dialog, widthField, heightField, minesField, okButton, cancelButton; // Custom...
                field_t     field;
                int         level;
                int         custom[3]; // width, height, mines
                int         seconds;
                int         best[CUSTOM]; // seconds, 0: none yet
                picture_t   picture;
                layout_t    layout;
                int         cursorX, cursorY, cursorShown;
                int         held;  // mouse buttons down on the canvas
                int         chord; // both buttons were down: a chord on release
                char        dataFile[256];
} game_t;

static uint32_t seed(void) {
        uint32_t s = 0;
        if (getentropy(&s, sizeof(s)) != 0 || s == 0) s = (uint32_t)time(NULL) ^ (uint32_t)clock();
        return s;
}

// ---- Settings and best times: a few lines in the application's data folder

static void load(game_t *g) {
        char dir[200];
        if (bmds_app_data_dir(dir, sizeof(dir)) != 0) return; // not installed as an application: nothing kept
        snprintf(g->dataFile, sizeof(g->dataFile), "%s/minesweeper.conf", dir);
        FILE *f = fopen(g->dataFile, "r");
        if (f == NULL) return;
        char line[80];
        while (fgets(line, sizeof(line), f) != NULL) {
                int a, b, c;
                if (sscanf(line, "level %d", &a) == 1 && a >= 0 && a < LEVELS) g->level = a;
                if (sscanf(line, "custom %d %d %d", &a, &b, &c) == 3) {
                        g->custom[0] = a;
                        g->custom[1] = b;
                        g->custom[2] = c;
                }
                if (sscanf(line, "best %d %d", &a, &b) == 2 && a >= 0 && a < CUSTOM && b > 0) g->best[a] = b;
        }
        fclose(f);
        return;
}

static void save(const game_t *g) {
        if (g->dataFile[0] == '\0') return;
        char temp[sizeof(g->dataFile) + 4];
        snprintf(temp, sizeof(temp), "%s.new", g->dataFile);
        FILE *f = fopen(temp, "w");
        if (f == NULL) return;
        fprintf(f, "level %d\ncustom %d %d %d\n", g->level, g->custom[0], g->custom[1], g->custom[2]);
        for (int i = 0; i < CUSTOM; ++i)
                if (g->best[i] > 0) fprintf(f, "best %d %d\n", i, g->best[i]);
        // Written whole, then put in place: a reset never leaves half a file.
        if (fclose(f) == 0) rename(temp, g->dataFile);
        return;
}

// ---- Drawing: cells into the picture, the changed part sent to the canvas

static void send(game_t *g, int x, int y, int w, int h) {
        if (g->picture.pixels == NULL) return;
        if (x < 0) {
                w += x;
                x = 0;
        }
        if (y < 0) {
                h += y;
                y = 0;
        }
        if (x + w > g->picture.width) w = g->picture.width - x;
        if (y + h > g->picture.height) h = g->picture.height - y;
        if (w <= 0 || h <= 0) return;
        bmds_gui_put_pixels(g->gui, g->canvas, x, y, (uint32_t)w, (uint32_t)h,
                            g->picture.pixels + y * g->picture.stride + x, (uint32_t)g->picture.stride);
        return;
}

// Cells (x0, y0) to (x1, y1) drawn and sent.
static void cells(game_t *g, int x0, int y0, int x1, int y1) {
        if (g->picture.pixels == NULL || x0 > x1) return;
        for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                        draw_cell(&g->picture, &g->layout, &g->field, x, y,
                                  g->cursorShown && x == g->cursorX && y == g->cursorY);
        const int s = g->layout.cell;
        send(g, g->layout.left + x0 * s, g->layout.top + y0 * s, (x1 - x0 + 1) * s, (y1 - y0 + 1) * s);
        return;
}

static void redraw(game_t *g) {
        if (g->picture.pixels == NULL) return;
        g->layout = draw_layout(&g->field, g->picture.width, g->picture.height);
        draw_background(&g->picture);
        for (int y = 0; y < g->field.height; ++y)
                for (int x = 0; x < g->field.width; ++x)
                        draw_cell(&g->picture, &g->layout, &g->field, x, y,
                                  g->cursorShown && x == g->cursorX && y == g->cursorY);
        send(g, 0, 0, g->picture.width, g->picture.height);
        return;
}

// The canvas's new size: a picture as big, drawn whole.
static void resized(game_t *g, int width, int height) {
        free(g->picture.pixels);
        g->picture.pixels = NULL;
        if (width <= 0 || height <= 0) return;
        g->picture.pixels = malloc((size_t)width * (size_t)height * sizeof(uint16_t));
        if (g->picture.pixels == NULL) return;
        g->picture.width = g->picture.stride = width;
        g->picture.height = height;
        redraw(g);
        return;
}

// ---- The status bar, the clock

static void status(game_t *g) {
        char text[80];
        if (g->field.state == FIELD_WON)
                snprintf(text, sizeof(text), "Cleared in %d s!", g->seconds);
        else if (g->field.state == FIELD_LOST)
                snprintf(text, sizeof(text), "Boom! F2 to play again");
        else
                snprintf(text, sizeof(text), "Mines: %d   Time: %d", field_mines_left(&g->field), g->seconds);
        bmds_status_bar_set_field(g->gui, g->status, 0, text);
        return;
}

static void message(game_t *g, const char *kind, const char *title, const char *text) {
        const uint32_t box = bmds_message_box_new(g->gui, 0);
        bmds_message_box_set_kind(g->gui, box, kind);
        bmds_message_box_set_title(g->gui, box, title);
        bmds_message_box_set_text(g->gui, box, text);
        bmds_message_box_set_buttons(g->gui, box, "ok");
        bmds_message_box_set_owner(g->gui, box, g->window);
        bmds_message_box_open(g->gui, box);
        return;
}

// After a move: the clock started or stopped, a win's time kept.
static void moved(game_t *g, field_state_t before) {
        if (before == FIELD_READY && g->field.state == FIELD_PLAYING) bmds_timer_start(g->gui, g->timer);
        const int over = g->field.state == FIELD_WON || g->field.state == FIELD_LOST;
        if (over && before != g->field.state) {
                bmds_timer_stop(g->gui, g->timer);
                if (g->field.state == FIELD_WON && g->level != CUSTOM &&
                    (g->best[g->level] == 0 || g->seconds < g->best[g->level])) {
                        g->best[g->level] = g->seconds;
                        save(g);
                        char text[120];
                        snprintf(text, sizeof(text), "A new best time for %s: %d seconds.", Levels[g->level].name,
                                 g->seconds);
                        message(g, "information", "Best Time", text);
                }
        }
        status(g);
        return;
}

// ---- A game

static void level_size(const game_t *g, int level, int *w, int *h, int *m) {
        if (level == CUSTOM) {
                *w = g->custom[0];
                *h = g->custom[1];
                *m = g->custom[2];
        } else {
                *w = Levels[level].width;
                *h = Levels[level].height;
                *m = Levels[level].mines;
        }
        return;
}

static void new_game(game_t *g) {
        int w, h, m;
        level_size(g, g->level, &w, &h, &m);
        field_new(&g->field, w, h, m, seed());
        bmds_timer_stop(g->gui, g->timer);
        g->seconds = 0;
        g->held = g->chord = 0;
        if (g->cursorX >= g->field.width) g->cursorX = g->field.width - 1;
        if (g->cursorY >= g->field.height) g->cursorY = g->field.height - 1;
        for (int i = 0; i < LEVELS; ++i) bmds_menu_item_set_checked(g->gui, g->levelItems[i], i == g->level);
        redraw(g);
        status(g);
        return;
}

// A level chosen: the window made to fit it (the board scales to whatever size it is given).
static void set_level(game_t *g, int level) {
        g->level = level;
        save(g);
        char title[64];
        snprintf(title, sizeof(title), "Minesweeper: %s", Levels[level].name);
        bmds_window_set_title(g->gui, g->window, title);
        int w, h, m;
        level_size(g, level, &w, &h, &m);
        bmds_canvas_set_preferred_width(g->gui, g->canvas, w * CELL_SIDE);
        bmds_canvas_set_preferred_height(g->gui, g->canvas, h * CELL_SIDE);
        // The menu bar and the status bar above and below the canvas: about a line each.
        bmds_window_set_width(g->gui, g->window, w * CELL_SIDE);
        bmds_window_set_height(g->gui, g->window, h * CELL_SIDE + 52);
        new_game(g);
        return;
}

// The cell under (px, py), canvas coordinates: 1 if there is one.
static int cell_at(const game_t *g, int px, int py, int *x, int *y) {
        const int s = g->layout.cell;
        if (s <= 0 || px < g->layout.left || py < g->layout.top) return 0;
        *x = (px - g->layout.left) / s;
        *y = (py - g->layout.top) / s;
        return *x < g->field.width && *y < g->field.height;
}

// What the player did at (x, y): reveal (or chord on an open number), flag, chord.
enum {
        REVEAL,
        FLAG,
        CHORD
};

static void act(game_t *g, int what, int x, int y) {
        const field_state_t before = g->field.state;
        const field_cell_t *c = field_at(&g->field, x, y);
        if (c == NULL) return;
        int changed = 0;
        if (what == FLAG)
                changed = field_flag(&g->field, x, y);
        else if (what == CHORD || (c->marks & CELL_OPEN))
                changed = field_chord(&g->field, x, y);
        else
                changed = field_reveal(&g->field, x, y);
        if (changed) cells(g, g->field.x0, g->field.y0, g->field.x1, g->field.y1);
        moved(g, before);
        return;
}

static void move_cursor(game_t *g, int dx, int dy) {
        const int x = g->cursorX, y = g->cursorY, shown = g->cursorShown;
        g->cursorShown = 1;
        g->cursorX = (g->cursorX + dx + g->field.width) % g->field.width;
        g->cursorY = (g->cursorY + dy + g->field.height) % g->field.height;
        if (shown) cells(g, x, y, x, y);
        cells(g, g->cursorX, g->cursorY, g->cursorX, g->cursorY);
        return;
}

static void hide_cursor(game_t *g) {
        if (!g->cursorShown) return;
        g->cursorShown = 0;
        cells(g, g->cursorX, g->cursorY, g->cursorX, g->cursorY);
        return;
}

// ---- Custom...: a small modal window of its own

static uint32_t number_row(game_t *g, const char *label, int value, int lo, int hi) {
        const uint32_t row = bmds_box_new(g->gui, g->dialog);
        bmds_box_set_direction(g->gui, row, "across");
        bmds_box_set_spacing(g->gui, row, 8);
        const uint32_t text = bmds_label_new(g->gui, row);
        bmds_label_set_text(g->gui, text, label);
        bmds_spacer_new(g->gui, row);
        const uint32_t field = bmds_number_field_new(g->gui, row);
        bmds_number_field_set_minimum(g->gui, field, lo);
        bmds_number_field_set_maximum(g->gui, field, hi);
        bmds_number_field_set_value(g->gui, field, value);
        return field;
}

static void open_custom(game_t *g) {
        if (g->dialog != 0) return;
        g->dialog = bmds_window_new(g->gui, 0);
        bmds_window_set_title(g->gui, g->dialog, "Custom Field");
        bmds_window_set_owner(g->gui, g->dialog, g->window);
        bmds_window_set_modal(g->gui, g->dialog, 1);
        bmds_window_set_resizable(g->gui, g->dialog, 0);
        bmds_window_set_width(g->gui, g->dialog, 240);
        bmds_window_set_height(g->gui, g->dialog, 124);
        bmds_window_set_margin(g->gui, g->dialog, 10);
        bmds_window_set_spacing(g->gui, g->dialog, 6);
        g->widthField = number_row(g, "Width (cells)", g->custom[0], FIELD_MIN_SIDE, FIELD_MAX_WIDTH);
        g->heightField = number_row(g, "Height (cells)", g->custom[1], FIELD_MIN_SIDE, FIELD_MAX_HEIGHT);
        g->minesField = number_row(g, "Mines", g->custom[2], 1, FIELD_MAX_CELLS - 1);
        const uint32_t row = bmds_box_new(g->gui, g->dialog);
        bmds_box_set_direction(g->gui, row, "across");
        bmds_box_set_spacing(g->gui, row, 6);
        bmds_spacer_new(g->gui, row);
        g->okButton = bmds_button_new(g->gui, row);
        bmds_button_set_text(g->gui, g->okButton, "&Play");
        g->cancelButton = bmds_button_new(g->gui, row);
        bmds_button_set_text(g->gui, g->cancelButton, "Cancel");
        bmds_window_show(g->gui, g->dialog);
        return;
}

static int number(game_t *g, uint32_t field) {
        bmds_gui_value_t v;
        return bmds_number_field_get_value(g->gui, field, &v) == 0 ? (int)v.number : 0;
}

static void close_custom(game_t *g, int play) {
        if (g->dialog == 0) return;
        if (play) {
                g->custom[0] = number(g, g->widthField);
                g->custom[1] = number(g, g->heightField);
                g->custom[2] = number(g, g->minesField);
                // As the field will take them (field_new clamps the same way).
                if (g->custom[2] >= g->custom[0] * g->custom[1]) g->custom[2] = g->custom[0] * g->custom[1] - 1;
        }
        bmds_gui_destroy(g->gui, g->dialog);
        g->dialog = 0;
        if (play) set_level(g, CUSTOM);
        return;
}

// ---- Help

static void best_times(game_t *g) {
        char text[200];
        int  n = snprintf(text, sizeof(text), "Best times:\n");
        for (int i = 0; i < CUSTOM && n > 0 && (size_t)n < sizeof(text); ++i) {
                if (g->best[i] > 0)
                        n += snprintf(text + n, sizeof(text) - (size_t)n, "\n%s: %d seconds", Levels[i].name,
                                      g->best[i]);
                else
                        n += snprintf(text + n, sizeof(text) - (size_t)n, "\n%s: none yet", Levels[i].name);
        }
        message(g, "information", "Best Times", text);
        return;
}

static const char HowToPlay[] =
        "Clear every cell that hides no mine.\n\n"
        "A number tells how many of the eight cells around it hide mines. Left click reveals a cell; "
        "the first is always safe. Right click puts a flag where you think a mine is. Clicking a number "
        "whose flags are all placed reveals the rest around it.\n\n"
        "Keys: the arrows move, Space reveals, F flags, F2 starts again.";

// ---- Events

static void key(game_t *g, const bmds_gui_event_t *e) {
        if (e->count < 4 || !e->values[2].number) return; // a press (or its repeat) only
        const int usage = (int)e->values[0].number;
        switch (usage) {
                case KEY_LEFT: move_cursor(g, -1, 0); break;
                case KEY_RIGHT: move_cursor(g, 1, 0); break;
                case KEY_UP: move_cursor(g, 0, -1); break;
                case KEY_DOWN: move_cursor(g, 0, 1); break;
                case KEY_SPACE:
                case KEY_ENTER:
                        if (!g->cursorShown) move_cursor(g, 0, 0);
                        act(g, REVEAL, g->cursorX, g->cursorY);
                        break;
                case KEY_F:
                        if (!g->cursorShown) move_cursor(g, 0, 0);
                        act(g, FLAG, g->cursorX, g->cursorY);
                        break;
                default: break;
        }
        return;
}

static void pointer(game_t *g, const bmds_gui_event_t *e, int pressed) {
        if (e->count < 3) return;
        const int button = (int)e->values[2].number;
        int       x = 0, y = 0;
        const int on = cell_at(g, (int)e->values[0].number, (int)e->values[1].number, &x, &y);
        if (pressed) {
                hide_cursor(g);
                g->held |= button;
                if ((g->held & (BUTTON_LEFT | BUTTON_RIGHT)) == (BUTTON_LEFT | BUTTON_RIGHT)) g->chord = 1;
                // A flag goes on as the right button goes down (unless both are: a chord).
                if (button == BUTTON_RIGHT && !g->chord && on) act(g, FLAG, x, y);
                return;
        }
        g->held &= ~button;
        if (g->chord) {
                // Both buttons: the chord on the first release, nothing on the second.
                if (on && g->held != 0) act(g, CHORD, x, y);
                if (g->held == 0) g->chord = 0;
                return;
        }
        if (!on) return;
        if (button == BUTTON_LEFT) act(g, REVEAL, x, y);
        if (button == BUTTON_MIDDLE) act(g, CHORD, x, y);
        return;
}

static int handle(game_t *g, const bmds_gui_event_t *e) {
        bmds_gui_t *gui = g->gui;
        if (e->source == g->window && (bmds_gui_event_is(gui, e, BMDS_GUI_WINDOW_CLOSED) ||
                                       bmds_gui_event_is(gui, e, BMDS_GUI_WINDOW_CLOSE_REQUESTED)))
                return 0;
        if (g->dialog != 0 &&
            ((e->source == g->dialog && (bmds_gui_event_is(gui, e, BMDS_GUI_WINDOW_CLOSED) ||
                                         bmds_gui_event_is(gui, e, BMDS_GUI_WINDOW_CLOSE_REQUESTED))) ||
             (e->source == g->cancelButton && bmds_gui_event_is(gui, e, BMDS_GUI_BUTTON_CLICKED)))) {
                close_custom(g, 0);
                return 1;
        }
        if (g->dialog != 0 && e->source == g->okButton && bmds_gui_event_is(gui, e, BMDS_GUI_BUTTON_CLICKED)) {
                close_custom(g, 1);
                return 1;
        }
        // A message answered: it's done with (each is made for one message).
        if (bmds_gui_event_is(gui, e, BMDS_GUI_MESSAGE_BOX_ANSWERED)) {
                bmds_gui_destroy(gui, e->source);
                return 1;
        }
        if (e->source == g->timer && bmds_gui_event_is(gui, e, BMDS_GUI_TIMER_TIMEOUT)) {
                if (g->field.state == FIELD_PLAYING && g->seconds < 999) {
                        ++g->seconds;
                        status(g);
                }
                return 1;
        }
        if (e->source == g->canvas) {
                if (bmds_gui_event_is(gui, e, BMDS_GUI_CANVAS_RESIZED) && e->count == 2)
                        resized(g, (int)e->values[0].number, (int)e->values[1].number);
                else if (bmds_gui_event_is(gui, e, BMDS_GUI_CANVAS_PRESSED))
                        pointer(g, e, 1);
                else if (bmds_gui_event_is(gui, e, BMDS_GUI_CANVAS_RELEASED))
                        pointer(g, e, 0);
                else if (bmds_gui_event_is(gui, e, BMDS_GUI_CANVAS_KEY))
                        key(g, e);
                return 1;
        }
        if (!bmds_gui_event_is(gui, e, BMDS_GUI_MENU_ITEM_CHOSEN)) return 1;
        if (e->source == g->quitItem) return 0;
        if (e->source == g->newItem) new_game(g);
        for (int i = 0; i < CUSTOM; ++i)
                if (e->source == g->levelItems[i]) set_level(g, i);
        if (e->source == g->levelItems[CUSTOM]) open_custom(g);
        if (e->source == g->bestItem) best_times(g);
        if (e->source == g->howItem) message(g, "information", "How to Play", HowToPlay);
        if (e->source == g->aboutItem)
                message(g, "information", "About Minesweeper",
                        "Minesweeper " MINESWEEPER_VERSION "\n\nAn example of a BMDS package: its source, and how it "
                        "is built, signed and published, are at https://github.com/jthwho/bmds-apps");
        return 1;
}

static uint32_t item(game_t *g, uint32_t menu, const char *text, const char *shortcut) {
        const uint32_t id = bmds_menu_item_new(g->gui, menu);
        bmds_menu_item_set_text(g->gui, id, text);
        if (shortcut != NULL) bmds_menu_item_set_shortcut(g->gui, id, shortcut);
        return id;
}

static void build(game_t *g) {
        bmds_gui_t *gui = g->gui;
        g->window = bmds_window_new(gui, 0);
        bmds_window_set_title(gui, g->window, "Minesweeper");
        bmds_window_set_min_width(gui, g->window, 200);
        bmds_window_set_min_height(gui, g->window, 160);
        const uint32_t closeRequested = bmds_gui_atom(gui, BMDS_GUI_WINDOW_CLOSE_REQUESTED);
        bmds_gui_listen(gui, g->window, &closeRequested, 1, 1);

        const uint32_t bar = bmds_menu_bar_new(gui, g->window);
        const uint32_t game = bmds_menu_new(gui, bar);
        bmds_menu_set_title(gui, game, "&Game");
        g->newItem = item(g, game, "&New", "F2");
        bmds_menu_add_separator(gui, game);
        static const char *const levelText[LEVELS] = {"&Beginner", "&Intermediate", "&Expert", "&Custom..."};
        for (int i = 0; i < LEVELS; ++i) g->levelItems[i] = item(g, game, levelText[i], NULL);
        bmds_menu_add_separator(gui, game);
        g->bestItem = item(g, game, "Best &Times...", NULL);
        bmds_menu_add_separator(gui, game);
        g->quitItem = item(g, game, "&Quit", "Ctrl+Q");
        const uint32_t help = bmds_menu_new(gui, bar);
        bmds_menu_set_title(gui, help, "&Help");
        g->howItem = item(g, help, "&How to Play", "F1");
        g->aboutItem = item(g, help, "&About Minesweeper", NULL);

        g->canvas = bmds_canvas_new(gui, g->window);
        bmds_widget_set_stretch(gui, g->canvas, 1);
        g->status = bmds_status_bar_new(gui, g->window);
        bmds_status_bar_add_field(gui, g->status, 0, NULL);
        g->timer = bmds_timer_new(gui, 0);
        bmds_timer_set_interval(gui, g->timer, 1000);
        bmds_timer_set_repeat(gui, g->timer, 1);
        return;
}

int main(void) {
        static game_t g; // a field is about 2 KB: off the stack
        g.gui = bmds_gui_connect(NULL);
        if (g.gui == NULL) {
                fprintf(stderr, "minesweeper: no window system (%s)\n", strerror(errno));
                return 1;
        }
        g.level = BEGINNER;
        g.custom[0] = Levels[CUSTOM].width;
        g.custom[1] = Levels[CUSTOM].height;
        g.custom[2] = Levels[CUSTOM].mines;
        load(&g);
        build(&g);
        set_level(&g, g.level);
        bmds_window_show(g.gui, g.window);
        bmds_widget_set_focus(g.gui, g.canvas);
        for (;;) {
                bmds_gui_event_t e;
                if (bmds_gui_next_event(g.gui, &e, -1) < 0) break;
                if (!handle(&g, &e)) break;
        }
        bmds_gui_close(g.gui);
        free(g.picture.pixels);
        return 0;
}
