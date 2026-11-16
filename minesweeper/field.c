/**
 * @file      field.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "field.h"

#include <string.h>

static int clamp(int v, int lo, int hi) {
        return v < lo ? lo : v > hi ? hi : v;
}

static uint32_t next(field_t *f) {
        uint32_t x = f->random;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        f->random = x;
        return x;
}

static void touched(field_t *f, int x, int y) {
        if (f->x0 > f->x1) {
                f->x0 = f->x1 = x;
                f->y0 = f->y1 = y;
                return;
        }
        if (x < f->x0) f->x0 = x;
        if (x > f->x1) f->x1 = x;
        if (y < f->y0) f->y0 = y;
        if (y > f->y1) f->y1 = y;
        return;
}

static void untouched(field_t *f) {
        f->x0 = 1;
        f->x1 = 0;
        return;
}

static field_cell_t *cell(field_t *f, int x, int y) {
        if (x < 0 || y < 0 || x >= f->width || y >= f->height) return NULL;
        return &f->cells[y * f->width + x];
}

void field_new(field_t *f, int width, int height, int mines, uint32_t seed) {
        memset(f, 0, sizeof(*f));
        f->width = clamp(width, FIELD_MIN_SIDE, FIELD_MAX_WIDTH);
        f->height = clamp(height, FIELD_MIN_SIDE, FIELD_MAX_HEIGHT);
        f->mines = clamp(mines, 1, f->width * f->height - 1);
        f->exploded = -1;
        f->state = FIELD_READY;
        f->random = seed != 0 ? seed : 1;
        untouched(f);
        return;
}

const field_cell_t *field_at(const field_t *f, int x, int y) {
        return cell((field_t *)f, x, y);
}

// Within a cell of (fx, fy): the first reveal and its neighbours.
static int near(int x, int y, int fx, int fy) {
        return x >= fx - 1 && x <= fx + 1 && y >= fy - 1 && y <= fy + 1;
}

// The mines, anywhere but the first reveal and, when there is room, its neighbours: the first
// reveal then opens space, as it should.
static void place(field_t *f, int fx, int fy) {
        const int cells = f->width * f->height;
        int       kept = 0;
        for (int y = fy - 1; y <= fy + 1; ++y)
                for (int x = fx - 1; x <= fx + 1; ++x)
                        if (cell(f, x, y) != NULL) ++kept;
        const int clear = cells - kept >= f->mines; // room to keep the neighbours clear
        for (int placed = 0; placed < f->mines;) {
                const int at = (int)(next(f) % (uint32_t)cells);
                const int x = at % f->width, y = at / f->width;
                if (f->cells[at].marks & CELL_MINE) continue;
                if (clear ? near(x, y, fx, fy) : (x == fx && y == fy)) continue;
                f->cells[at].marks |= CELL_MINE;
                ++placed;
        }
        for (int y = 0; y < f->height; ++y)
                for (int x = 0; x < f->width; ++x) {
                        int n = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx) {
                                        const field_cell_t *c = cell(f, x + dx, y + dy);
                                        if ((dx || dy) && c != NULL && (c->marks & CELL_MINE)) ++n;
                                }
                        cell(f, x, y)->count = (uint8_t)n;
                }
        f->state = FIELD_PLAYING;
        return;
}

// Every mine shown at the end (lost), or flagged (won).
static void finish(field_t *f, field_state_t state) {
        f->state = state;
        for (int y = 0; y < f->height; ++y)
                for (int x = 0; x < f->width; ++x) {
                        field_cell_t *c = cell(f, x, y);
                        if (!(c->marks & CELL_MINE) && !(c->marks & CELL_FLAG)) continue;
                        if (state == FIELD_WON && !(c->marks & CELL_FLAG)) {
                                c->marks |= CELL_FLAG;
                                ++f->flags;
                        }
                        touched(f, x, y);
                }
        return;
}

// Opens (x, y), and if it has no mine around it, its neighbours in turn: a stack of cells to
// visit, at most every cell once (no recursion on a program's small stack).
static int open_from(field_t *f, int x, int y) {
        static int16_t stack[FIELD_MAX_CELLS];
        int            top = 0, changed = 0;
        field_cell_t  *c = cell(f, x, y);
        if (c == NULL || (c->marks & (CELL_OPEN | CELL_FLAG))) return 0;
        if (c->marks & CELL_MINE) {
                c->marks |= CELL_OPEN;
                f->exploded = y * f->width + x;
                touched(f, x, y);
                finish(f, FIELD_LOST);
                return 1;
        }
        c->marks |= CELL_OPEN;
        stack[top++] = (int16_t)(y * f->width + x);
        while (top > 0) {
                const int at = stack[--top];
                const int cx = at % f->width, cy = at / f->width;
                ++f->opened;
                changed = 1;
                touched(f, cx, cy);
                if (f->cells[at].count != 0) continue;
                for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                                field_cell_t *n = cell(f, cx + dx, cy + dy);
                                if (n == NULL || (n->marks & (CELL_OPEN | CELL_FLAG | CELL_MINE))) continue;
                                n->marks |= CELL_OPEN; // marked as it is stacked: never twice
                                stack[top++] = (int16_t)((cy + dy) * f->width + cx + dx);
                        }
        }
        if (f->opened == f->width * f->height - f->mines) finish(f, FIELD_WON);
        return changed;
}

int field_reveal(field_t *f, int x, int y) {
        untouched(f);
        if (f->state == FIELD_WON || f->state == FIELD_LOST || cell(f, x, y) == NULL) return 0;
        if (f->state == FIELD_READY) {
                if (cell(f, x, y)->marks & CELL_FLAG) return 0;
                place(f, x, y);
        }
        return open_from(f, x, y);
}

int field_flag(field_t *f, int x, int y) {
        untouched(f);
        field_cell_t *c = cell(f, x, y);
        if (c == NULL || f->state == FIELD_WON || f->state == FIELD_LOST || (c->marks & CELL_OPEN)) return 0;
        c->marks ^= CELL_FLAG;
        f->flags += (c->marks & CELL_FLAG) ? 1 : -1;
        touched(f, x, y);
        return 1;
}

int field_chord(field_t *f, int x, int y) {
        untouched(f);
        const field_cell_t *c = cell(f, x, y);
        if (c == NULL || f->state != FIELD_PLAYING || !(c->marks & CELL_OPEN) || c->count == 0) return 0;
        int flags = 0;
        for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                        const field_cell_t *n = cell(f, x + dx, y + dy);
                        if (n != NULL && (n->marks & CELL_FLAG)) ++flags;
                }
        if (flags != c->count) return 0;
        int changed = 0;
        for (int dy = -1; dy <= 1 && f->state == FIELD_PLAYING; ++dy)
                for (int dx = -1; dx <= 1 && f->state == FIELD_PLAYING; ++dx) changed |= open_from(f, x + dx, y + dy);
        // Every cell the chord opened is in the box: the calls above only grew it.
        return changed;
}

int field_mines_left(const field_t *f) {
        return f->mines - f->flags;
}
