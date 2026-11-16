# Minesweeper: the program, its application folder and its package.

# The package's version: raise it for every release that changes the game (pkg upgrade installs
# a newer version only).
set(MINESWEEPER_VERSION 1.0.0)

bmds_add_program(minesweeper SOURCES main.c field.c draw.c ICONS icons/16.png icons/32.png)
target_compile_options(minesweeper PRIVATE -O2 -Wall -Wextra -Werror)
target_compile_definitions(minesweeper PRIVATE MINESWEEPER_VERSION="${MINESWEEPER_VERSION}")

# apps/minesweeper.app: the program and its desktop entry (the launcher's name, category, id).
bmds_add_app(minesweeper PROGRAM minesweeper DESKTOP app.desktop)

# The package: the title, category and icons come from the app.
bmds_add_package(minesweeper VERSION ${MINESWEEPER_VERSION}
        SUMMARY "Clear the field without setting off a mine"
        DESCRIPTION_FILE DESCRIPTION
        LICENSE MIT
        HOMEPAGE https://github.com/jthwho/bmds-apps
        APPS minesweeper)
