/**
 * @file      field.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Minesweeper's rules: a field of cells, its mines placed after the first reveal (never under it
 * or beside it, when there is room), revealing, flags, chording, and winning or losing. No BMDS
 * calls: tests/field_test.c runs it on the PC.
 */

#ifndef MINESWEEPER_FIELD_H
#define MINESWEEPER_FIELD_H

#include <stdint.h>

#define FIELD_MIN_SIDE 5
#define FIELD_MAX_WIDTH 40
#define FIELD_MAX_HEIGHT 24
#define FIELD_MAX_CELLS (FIELD_MAX_WIDTH * FIELD_MAX_HEIGHT)

/** @brief Where a game is. */
typedef enum {
        FIELD_READY,   /**< Nothing revealed yet: the mines aren't placed. */
        FIELD_PLAYING, /**< The clock runs. */
        FIELD_WON,     /**< Every safe cell revealed. */
        FIELD_LOST,    /**< A mine revealed. */
} field_state_t;

/** @brief A cell's marks. */
enum {
        CELL_MINE = 1,
        CELL_OPEN = 2,
        CELL_FLAG = 4,
};

/** @brief A cell: its marks, and how many of its eight neighbours are mines. */
typedef struct {
                uint8_t marks;
                uint8_t count;
} field_cell_t;

/** @brief A game. */
typedef struct {
                int           width, height, mines;
                int           opened;   /**< Safe cells revealed. */
                int           flags;    /**< Flags placed. */
                int           exploded; /**< The mine revealed (y * width + x), or -1. */
                field_state_t state;
                uint32_t      random; /**< xorshift32's state (never 0). */
                field_cell_t  cells[FIELD_MAX_CELLS];
                /** The cells a call changed: their bounding box (x0 > x1: none). */
                int x0, y0, x1, y1;
} field_t;

/**
 * @brief Starts a game of @p width × @p height cells with @p mines mines, clamped to what fits
 *        (sides FIELD_MIN_SIDE to the maximums, mines 1 to cells - 1). @p seed picks where
 *        the mines go (0 is taken as 1).
 */
void field_new(field_t *f, int width, int height, int mines, uint32_t seed);

/** @brief The cell at (@p x, @p y), or NULL outside the field. */
const field_cell_t *field_at(const field_t *f, int x, int y);

/**
 * @brief Reveals (@p x, @p y): the first reveal places the mines first. A cell with no mine
 *        around it reveals its neighbours too, and theirs (no recursion). A flagged or open cell
 *        is left. @return 1 if anything changed.
 */
int field_reveal(field_t *f, int x, int y);

/** @brief Puts a flag on (@p x, @p y), or takes it off. @return 1 if it changed. */
int field_flag(field_t *f, int x, int y);

/**
 * @brief On an open number whose flags around it match it, reveals its other neighbours (a
 *        wrong flag loses the game, as it should). @return 1 if anything changed.
 */
int field_chord(field_t *f, int x, int y);

/** @brief Mines left to find: mines - flags (below zero with too many flags). */
int field_mines_left(const field_t *f);

#endif
