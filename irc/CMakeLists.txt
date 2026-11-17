# IRC (devplan/irc.md): the core, portable C++ tested on the PC (tests/irc), built here for the
# board. The program, its app and its package come with step 3; until BMDS's next release (TLS
# sockets and ChatView) it stays out of the repository's package list.

add_library(irc_core STATIC core/message.cpp)
target_include_directories(irc_core PUBLIC core)
target_link_libraries(irc_core PUBLIC bmds_headers)
target_compile_options(irc_core PRIVATE -O2 -Wall -Wextra -Werror)
