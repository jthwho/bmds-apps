/**
 * @file      draw.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "draw.h"

#include <stddef.h>

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

// The look: a cool slate board, pale open cells, numbers in their own colours.
static const uint16_t Background = RGB(0x3a, 0x45, 0x52);
static const uint16_t Hidden = RGB(0x8f, 0xa3, 0xb8);
static const uint16_t HiddenLight = RGB(0xc4, 0xd2, 0xe0);
static const uint16_t HiddenDark = RGB(0x56, 0x66, 0x78);
static const uint16_t Open = RGB(0xe8, 0xec, 0xf0);
static const uint16_t Grid = RGB(0xb8, 0xc0, 0xc8);
static const uint16_t Exploded = RGB(0xe0, 0x40, 0x40);
static const uint16_t Mine = RGB(0x20, 0x22, 0x28);
static const uint16_t Shine = RGB(0xf0, 0xf0, 0xf0);
static const uint16_t FlagRed = RGB(0xe0, 0x30, 0x30);
static const uint16_t Pole = RGB(0x30, 0x30, 0x38);
static const uint16_t Cursor = RGB(0xff, 0xb0, 0x20);
static const uint16_t Cross = RGB(0xd0, 0x10, 0x10);
static const uint16_t Numbers[9] = {
        0,
        RGB(0x20, 0x60, 0xd0), // 1
        RGB(0x20, 0x90, 0x40), // 2
        RGB(0xd0, 0x30, 0x30), // 3
        RGB(0x50, 0x30, 0xa0), // 4
        RGB(0xa0, 0x50, 0x10), // 5
        RGB(0x10, 0x90, 0x90), // 6
        RGB(0x30, 0x30, 0x30), // 7
        RGB(0x80, 0x80, 0x80), // 8
};

// The digits 1 to 8, 5 × 7, a row a byte (bit 4 the left).
static const uint8_t Digits[9][7] = {
        {0},
        {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
        {0x0e, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1f},
        {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e},
        {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
        {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},
        {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e},
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
};

static void fill(picture_t *p, int x, int y, int w, int h, uint16_t c) {
        if (x < 0) {
                w += x;
                x = 0;
        }
        if (y < 0) {
                h += y;
                y = 0;
        }
        if (x + w > p->width) w = p->width - x;
        if (y + h > p->height) h = p->height - y;
        for (int j = 0; j < h; ++j) {
                uint16_t *row = p->pixels + (y + j) * p->stride + x;
                for (int i = 0; i < w; ++i) row[i] = c;
        }
        return;
}

static void dot(picture_t *p, int x, int y, uint16_t c) {
        if (x >= 0 && y >= 0 && x < p->width && y < p->height) p->pixels[y * p->stride + x] = c;
        return;
}

static void disc(picture_t *p, int cx, int cy, int r, uint16_t c) {
        for (int y = -r; y <= r; ++y)
                for (int x = -r; x <= r; ++x)
                        if (x * x + y * y <= r * r + r / 2) dot(p, cx + x, cy + y, c);
        return;
}

// A line from (x0, y0) to (x1, y1), @p thick pixels square.
static void line(picture_t *p, int x0, int y0, int x1, int y1, int thick, uint16_t c) {
        const int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int       err = dx - dy;
        for (;;) {
                fill(p, x0 - thick / 2, y0 - thick / 2, thick, thick, c);
                if (x0 == x1 && y0 == y1) break;
                const int e2 = 2 * err;
                if (e2 > -dy) {
                        err -= dy;
                        x0 += sx;
                }
                if (e2 < dx) {
                        err += dx;
                        y0 += sy;
                }
        }
        return;
}

layout_t draw_layout(const field_t *f, int width, int height) {
        layout_t  l;
        const int byWidth = width / f->width, byHeight = height / f->height;
        l.cell = byWidth < byHeight ? byWidth : byHeight;
        if (l.cell < 8) l.cell = 8;
        l.left = (width - l.cell * f->width) / 2;
        l.top = (height - l.cell * f->height) / 2;
        return l;
}

void draw_background(picture_t *p) {
        fill(p, 0, 0, p->width, p->height, Background);
        return;
}

static void mine(picture_t *p, int x, int y, int s) {
        const int cx = x + s / 2, cy = y + s / 2, r = s * 3 / 10;
        const int t = s >= 20 ? 2 : 1;
        line(p, cx - r - r / 2, cy, cx + r + r / 2, cy, t, Mine);
        line(p, cx, cy - r - r / 2, cx, cy + r + r / 2, t, Mine);
        line(p, cx - r, cy - r, cx + r, cy + r, t, Mine);
        line(p, cx - r, cy + r, cx + r, cy - r, t, Mine);
        disc(p, cx, cy, r, Mine);
        disc(p, cx - r / 3, cy - r / 3, r / 4 > 0 ? r / 4 : 1, Shine);
        return;
}

static void flag(picture_t *p, int x, int y, int s) {
        const int pole = x + s / 2 + s / 10, top = y + s / 5, bottom = y + s * 4 / 5;
        const int t = s >= 20 ? 2 : 1;
        fill(p, pole, top, t, bottom - top, Pole);
        fill(p, x + s / 4, bottom - t, s / 2 + t, t * 2, Pole); // the base
        // A pennant: a triangle to the left of the pole, widest halfway down.
        const int h = (bottom - top) / 2, half = h / 2 > 0 ? h / 2 : 1, widest = s * 2 / 5;
        for (int j = 0; j <= h; ++j) {
                const int off = j > half ? j - half : half - j;
                const int len = widest * (half - off) / half;
                if (len > 0) fill(p, pole - len, top + j, len, 1, FlagRed);
        }
        return;
}

static void digit(picture_t *p, int x, int y, int s, int n) {
        int k = s / 9;
        if (k < 1) k = 1;
        const int w = 5 * k, h = 7 * k;
        const int ox = x + (s - w) / 2, oy = y + (s - h) / 2;
        for (int r = 0; r < 7; ++r)
                for (int c = 0; c < 5; ++c)
                        if (Digits[n][r] & (0x10 >> c)) fill(p, ox + c * k, oy + r * k, k, k, Numbers[n]);
        // Bolder when big enough: the same again a pixel over.
        if (k >= 2)
                for (int r = 0; r < 7; ++r)
                        for (int c = 0; c < 5; ++c)
                                if (Digits[n][r] & (0x10 >> c)) fill(p, ox + c * k + 1, oy + r * k, k, k, Numbers[n]);
        return;
}

void draw_cell(picture_t *p, const layout_t *l, const field_t *f, int x, int y, int cursor) {
        const field_cell_t *c = field_at(f, x, y);
        if (c == NULL) return;
        const int s = l->cell, px = l->left + x * s, py = l->top + y * s;
        const int over = f->state == FIELD_WON || f->state == FIELD_LOST;
        const int mineShown = (c->marks & CELL_MINE) && f->state == FIELD_LOST && !(c->marks & CELL_FLAG);
        const int open = (c->marks & CELL_OPEN) || mineShown;
        const int b = s >= 24 ? 3 : s >= 14 ? 2 : 1; // the bevel
        if (open) {
                const int exploded = f->exploded == y * f->width + x;
                fill(p, px, py, s, s, exploded ? Exploded : Open);
                fill(p, px, py, s, 1, Grid);
                fill(p, px, py, 1, s, Grid);
                if (c->marks & CELL_MINE)
                        mine(p, px, py, s);
                else if (c->count > 0)
                        digit(p, px, py, s, c->count);
        } else {
                fill(p, px, py, s, s, Hidden);
                fill(p, px, py, s, b, HiddenLight);
                fill(p, px, py, b, s, HiddenLight);
                fill(p, px, py + s - b, s, b, HiddenDark);
                fill(p, px + s - b, py, b, s, HiddenDark);
                if (c->marks & CELL_FLAG) {
                        flag(p, px, py, s);
                        // A flag on a safe cell, at the end: crossed out.
                        if (over && !(c->marks & CELL_MINE)) {
                                const int t = s >= 20 ? 2 : 1;
                                line(p, px + s / 5, py + s / 5, px + s * 4 / 5, py + s * 4 / 5, t, Cross);
                                line(p, px + s / 5, py + s * 4 / 5, px + s * 4 / 5, py + s / 5, t, Cross);
                        }
                }
        }
        if (cursor) {
                const int t = s >= 20 ? 2 : 1;
                fill(p, px, py, s, t, Cursor);
                fill(p, px, py + s - t, s, t, Cursor);
                fill(p, px, py, t, s, Cursor);
                fill(p, px + s - t, py, t, s, Cursor);
        }
        return;
}
