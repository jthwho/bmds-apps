# The I/O Manager (devplan/ioman.md): every pin of the chip, who holds it, its routing and live
# level, and the board's connectors. The core (portable C++, tested on the PC: tests/ioman) and
# the program, its app and its package. It needs a BMDS with gpio_ctl (slot 139, /dev/gpiochip0):
# it checks at its start and says so.

set(IOMAN_VERSION 0.1.0)

# The core is built into the program (not a library after libc on the link line, whose calls it
# makes and the program might not).
bmds_add_program(ioman SOURCES main.cpp core/pins.cpp core/setup.cpp ICONS icons/16.png icons/32.png)
target_include_directories(ioman PRIVATE core)
target_compile_options(ioman PRIVATE -Os -Wall -Wextra -Werror)

# apps/ioman.app: the program and its desktop entry.
bmds_add_app(ioman PROGRAM ioman DESKTOP app.desktop)

bmds_add_package(ioman VERSION ${IOMAN_VERSION}
        SUMMARY "See every pin of the chip: who holds it, its routing, its level"
        DESCRIPTION_FILE DESCRIPTION
        LICENSE MIT
        HOMEPAGE https://github.com/jthwho/bmds-apps
        APPS ioman)
