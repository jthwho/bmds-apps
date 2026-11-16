/**
 * @file      field_test.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Minesweeper's rules (minesweeper/field.c), on the PC:
 *
 *   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
 */

#include "field.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond)                                                                                                    \
        do {                                                                                                           \
                if (!(cond)) {                                                                                         \
                        fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                       \
                        ++failures;                                                                                    \
                }                                                                                                      \
        } while (0)

static int count_mines(const field_t *f) {
        int n = 0;
        for (int i = 0; i < f->width * f->height; ++i)
                if (f->cells[i].marks & CELL_MINE) ++n;
        return n;
}

static int has(const field_t *f, int x, int y, int mark) {
        return (field_at(f, x, y)->marks & mark) != 0;
}

// The first reveal is never a mine and opens space, at every size and seed.
static void first_reveal_is_safe(void) {
        static const int sizes[][3] = {{9, 9, 10}, {16, 16, 40}, {30, 16, 99}, {5, 5, 16}};
        for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s)
                for (uint32_t seed = 1; seed < 300; ++seed) {
                        static field_t f;
                        field_new(&f, sizes[s][0], sizes[s][1], sizes[s][2], seed);
                        const int x = (int)(seed % (uint32_t)f.width), y = (int)(seed / 7 % (uint32_t)f.height);
                        CHECK(f.state == FIELD_READY);
                        CHECK(field_reveal(&f, x, y));
                        CHECK(count_mines(&f) == f.mines);
                        CHECK(f.state != FIELD_LOST);
                        CHECK(!has(&f, x, y, CELL_MINE));
                        // Room for it (16 of 25 leaves 9): its neighbours clear too, so it opened space.
                        if (f.width * f.height - 9 >= f.mines) CHECK(field_at(&f, x, y)->count == 0 && f.opened > 1);
                }
        return;
}

// Counts: every cell's is its neighbours' mines.
static void counts(void) {
        static field_t f;
        field_new(&f, 16, 16, 40, 1234);
        field_reveal(&f, 8, 8);
        for (int y = 0; y < f.height; ++y)
                for (int x = 0; x < f.width; ++x) {
                        int n = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx) {
                                        const field_cell_t *c = field_at(&f, x + dx, y + dy);
                                        if ((dx || dy) && c && (c->marks & CELL_MINE)) ++n;
                                }
                        CHECK(field_at(&f, x, y)->count == n);
                }
        return;
}

// The flood fill opens a whole empty region and its numbered edge, nothing past it.
static void flood(void) {
        static field_t f;
        field_new(&f, 9, 9, 10, 77);
        field_reveal(&f, 4, 4);
        for (int y = 0; y < 9; ++y)
                for (int x = 0; x < 9; ++x) {
                        if (!has(&f, x, y, CELL_OPEN)) continue;
                        CHECK(!has(&f, x, y, CELL_MINE));
                        // An open cell with no mines around it has every neighbour open.
                        if (field_at(&f, x, y)->count == 0)
                                for (int dy = -1; dy <= 1; ++dy)
                                        for (int dx = -1; dx <= 1; ++dx) {
                                                const field_cell_t *c = field_at(&f, x + dx, y + dy);
                                                CHECK(c == NULL || (c->marks & CELL_OPEN));
                                        }
                }
        // The changed box covers what opened.
        CHECK(f.x0 <= 4 && f.x1 >= 4 && f.y0 <= 4 && f.y1 >= 4);
        return;
}

// Flags: placed and taken off, never on an open cell; a flagged cell isn't revealed.
static void flags(void) {
        static field_t f;
        field_new(&f, 9, 9, 10, 5);
        CHECK(field_flag(&f, 0, 0));
        CHECK(field_mines_left(&f) == 9);
        CHECK(!field_reveal(&f, 0, 0)); // flagged: not even the first reveal
        CHECK(field_flag(&f, 0, 0));
        CHECK(field_mines_left(&f) == 10);
        field_reveal(&f, 4, 4);
        CHECK(!field_flag(&f, 4, 4)); // open
        return;
}

// Revealing every safe cell wins, and flags every mine; a mine loses.
static void win_and_lose(void) {
        static field_t f;
        field_new(&f, 9, 9, 10, 99);
        field_reveal(&f, 0, 0);
        for (int y = 0; y < 9; ++y)
                for (int x = 0; x < 9; ++x)
                        if (!has(&f, x, y, CELL_MINE)) field_reveal(&f, x, y);
        CHECK(f.state == FIELD_WON);
        CHECK(f.opened == 81 - 10);
        CHECK(field_mines_left(&f) == 0);
        CHECK(!field_reveal(&f, 0, 0)); // over

        field_new(&f, 9, 9, 10, 99);
        field_reveal(&f, 0, 0);
        int mine = -1;
        for (int i = 0; i < 81 && mine < 0; ++i)
                if (f.cells[i].marks & CELL_MINE) mine = i;
        CHECK(field_reveal(&f, mine % 9, mine / 9));
        CHECK(f.state == FIELD_LOST);
        CHECK(f.exploded == mine);
        return;
}

// Chording: with the right flags around a number its other neighbours open; with a wrong flag,
// the game is lost; with too few, nothing happens.
static void chording(void) {
        static field_t f;
        field_new(&f, 16, 16, 40, 4242);
        field_reveal(&f, 8, 8);
        // An open number with a hidden neighbour that isn't a mine.
        int nx = -1, ny = -1;
        for (int y = 0; y < 16 && nx < 0; ++y)
                for (int x = 0; x < 16 && nx < 0; ++x) {
                        const field_cell_t *c = field_at(&f, x, y);
                        if (!(c->marks & CELL_OPEN) || c->count == 0) continue;
                        for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx) {
                                        const field_cell_t *n = field_at(&f, x + dx, y + dy);
                                        if (n && !(n->marks & (CELL_OPEN | CELL_MINE))) {
                                                nx = x;
                                                ny = y;
                                        }
                                }
                }
        CHECK(nx >= 0);
        if (nx < 0) return;
        CHECK(!field_chord(&f, nx, ny)); // no flags yet
        for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                        const field_cell_t *n = field_at(&f, nx + dx, ny + dy);
                        if (n && (n->marks & CELL_MINE)) field_flag(&f, nx + dx, ny + dy);
                }
        CHECK(field_chord(&f, nx, ny));
        CHECK(f.state != FIELD_LOST);
        for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                        const field_cell_t *n = field_at(&f, nx + dx, ny + dy);
                        CHECK(n == NULL || (n->marks & (CELL_OPEN | CELL_FLAG)));
                }

        // A wrong flag: the flags match the number, but one is on a safe cell.
        field_new(&f, 16, 16, 40, 4242);
        field_reveal(&f, 8, 8);
        for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) {
                        const field_cell_t *c = field_at(&f, x, y);
                        if (!(c->marks & CELL_OPEN) || c->count != 1) continue;
                        int safe = -1, mine = -1;
                        for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx) {
                                        const field_cell_t *n = field_at(&f, x + dx, y + dy);
                                        if (!n || (n->marks & CELL_OPEN)) continue;
                                        if (n->marks & CELL_MINE)
                                                mine = (y + dy) * 16 + x + dx;
                                        else
                                                safe = (y + dy) * 16 + x + dx;
                                }
                        if (safe < 0 || mine < 0) continue;
                        field_flag(&f, safe % 16, safe / 16);
                        CHECK(field_chord(&f, x, y));
                        CHECK(f.state == FIELD_LOST);
                        CHECK(f.exploded == mine);
                        return;
                }
        CHECK(!"no number of 1 with a safe and a mine hidden around it");
        return;
}

// Sizes clamp to what fits.
static void clamping(void) {
        static field_t f;
        field_new(&f, 1, 100, 1000, 0);
        CHECK(f.width == FIELD_MIN_SIDE);
        CHECK(f.height == FIELD_MAX_HEIGHT);
        CHECK(f.mines == f.width * f.height - 1);
        CHECK(f.random != 0);
        CHECK(field_reveal(&f, 0, 0)); // every other cell a mine: the first is still safe
        CHECK(f.state == FIELD_WON);
        return;
}

int main(void) {
        first_reveal_is_safe();
        counts();
        flood();
        flags();
        win_and_lose();
        chording();
        clamping();
        if (failures != 0) {
                fprintf(stderr, "field_test: %d failed\n", failures);
                return 1;
        }
        printf("field_test: ok\n");
        return 0;
}
