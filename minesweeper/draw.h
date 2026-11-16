/**
 * @file      draw.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Minesweeper's pictures, drawn in code into an RGB565 picture (what a Canvas takes): cells,
 * numbers, flags, mines and the keyboard's cursor, at any cell size. No BMDS calls.
 */

#ifndef MINESWEEPER_DRAW_H
#define MINESWEEPER_DRAW_H

#include <stdint.h>

#include "field.h"

/** @brief A picture: its pixels (RGB565), a row @p stride pixels long. */
typedef struct {
                uint16_t *pixels;
                int       width, height, stride;
} picture_t;

/** @brief Where the board is in the picture: its corner and its cells' side. */
typedef struct {
                int left, top, cell;
} layout_t;

/** @brief The layout that fits @p f's board in @p width × @p height, centred. */
layout_t draw_layout(const field_t *f, int width, int height);

/** @brief Fills the whole picture with the background. */
void draw_background(picture_t *p);

/**
 * @brief Draws cell (@p x, @p y) of @p f as it is (hidden, flagged, open with its number, a mine
 *        at the end), with the cursor's ring when @p cursor.
 */
void draw_cell(picture_t *p, const layout_t *l, const field_t *f, int x, int y, int cursor);

#endif
