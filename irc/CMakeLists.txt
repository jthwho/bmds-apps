# IRC (devplan/irc.md): the core (portable C++, tested on the PC: tests/irc) and the program, its
# app and its package. It needs BMDS's next release (TLS sockets, ChatView, the tabs' marks, the
# input line's keys): until then it stays out of the repository's package list, and checks for them
# when it starts.

set(IRC_VERSION 0.1.0)

add_library(irc_core STATIC core/message.cpp core/str.cpp core/isupport.cpp core/format.cpp core/session.cpp
            core/buffers.cpp core/commands.cpp core/complete.cpp core/settings.cpp)
target_include_directories(irc_core PUBLIC core)
target_link_libraries(irc_core PUBLIC bmds_headers)
target_compile_options(irc_core PRIVATE -Os -Wall -Wextra -Werror)

bmds_add_program(irc SOURCES bmds/main.cpp bmds/app.cpp bmds/connection.cpp bmds/logs.cpp)
target_include_directories(irc PRIVATE bmds)
target_link_libraries(irc PRIVATE irc_core)
target_compile_options(irc PRIVATE -Os -Wall -Wextra -Werror)

# apps/irc.app: the program and its desktop entry.
bmds_add_app(irc PROGRAM irc DESKTOP app.desktop)

bmds_add_package(irc VERSION ${IRC_VERSION}
        SUMMARY "Chat on IRC networks, over TLS"
        DESCRIPTION_FILE DESCRIPTION
        LICENSE MIT
        HOMEPAGE https://github.com/jthwho/bmds-apps
        APPS irc)
