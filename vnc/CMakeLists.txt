# The VNC server (devplan/vnc.md): the core (portable C++, tested on the PC: tests/vnc), zlib
# (thirdparty/zlib: deflate, and inflate for the Extended Clipboard), and the program, its app and
# its package. It needs a BMDS with screen capture (gui_screen_capture, slot 136): it checks at
# its start and says so.

set(VNCSERVER_VERSION 0.1.0)

add_library(vnc_zlib STATIC
        ${CMAKE_CURRENT_LIST_DIR}/../thirdparty/zlib/adler32.c
        ${CMAKE_CURRENT_LIST_DIR}/../thirdparty/zlib/crc32.c
        ${CMAKE_CURRENT_LIST_DIR}/../thirdparty/zlib/deflate.c
        ${CMAKE_CURRENT_LIST_DIR}/../thirdparty/zlib/inffast.c
        ${CMAKE_CURRENT_LIST_DIR}/../thirdparty/zlib/inflate.c
        ${CMAKE_CURRENT_LIST_DIR}/../thirdparty/zlib/inftrees.c
        ${CMAKE_CURRENT_LIST_DIR}/../thirdparty/zlib/trees.c
        ${CMAKE_CURRENT_LIST_DIR}/../thirdparty/zlib/zutil.c)
target_include_directories(vnc_zlib PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../thirdparty/zlib)
target_link_libraries(vnc_zlib PUBLIC bmds_headers)
target_compile_options(vnc_zlib PRIVATE -O2)

add_library(vnc_core STATIC core/clipboard.cpp core/des.cpp core/encode.cpp core/keysym.cpp core/pixelformat.cpp core/rfb.cpp
            core/text.cpp)
target_include_directories(vnc_core PUBLIC core)
target_link_libraries(vnc_core PUBLIC bmds_headers vnc_zlib)
target_compile_options(vnc_core PRIVATE -O2 -Wall -Wextra -Werror)

bmds_add_program(vncserver SOURCES main.cpp server.cpp)
target_link_libraries(vncserver PRIVATE vnc_core)
target_compile_options(vncserver PRIVATE -O2 -Wall -Wextra -Werror)

# apps/vncserver.app: the program and its desktop entry.
bmds_add_app(vncserver PROGRAM vncserver DESKTOP app.desktop)

bmds_add_package(vncserver VERSION ${VNCSERVER_VERSION}
        SUMMARY "Share the desktop with VNC viewers"
        DESCRIPTION_FILE DESCRIPTION
        LICENSE MIT
        HOMEPAGE https://github.com/jthwho/bmds-apps
        APPS vncserver)
