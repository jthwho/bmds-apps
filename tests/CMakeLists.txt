# The apps' rules on the PC, with the PC's own compiler (the apps are built with the SDK's):
# Minesweeper's (C) and IRC's core (C++, doctest).
#
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
#
# Under clang (CC=clang CXX=clang++ cmake ...), the fuzzers too (libFuzzer): irc_message_fuzz.
cmake_minimum_required(VERSION 3.16)
project(bmds-apps-tests C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined)
add_link_options(-fsanitize=address,undefined)
enable_testing()

add_executable(field_test field_test.c ../minesweeper/field.c)
target_include_directories(field_test PRIVATE ../minesweeper)
add_test(NAME field COMMAND field_test)

# IRC's core as the board builds it: no exceptions or RTTI, and only C's headers and the
# freestanding C++ ones (the board has no libstdc++).
set(IRC_CORE ${CMAKE_CURRENT_LIST_DIR}/../irc/core)
set(IRC_CORE_SRCS ${IRC_CORE}/message.cpp)
add_library(irc_core STATIC ${IRC_CORE_SRCS})
target_include_directories(irc_core PUBLIC ${IRC_CORE})
target_compile_options(irc_core PRIVATE -fno-exceptions -fno-rtti)

add_executable(irc_test irc/main.cpp irc/message.cpp)
target_include_directories(irc_test PRIVATE ../thirdparty/doctest)
target_compile_definitions(irc_test PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(irc_test PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(irc_test PRIVATE irc_core)
add_test(NAME irc COMMAND irc_test)

# The VNC server's core (devplan/vnc.md §7), as the board builds it; zlib is the PC's here (the
# board's is thirdparty/zlib, deflate only: the tests' viewer inflates).
find_package(ZLIB REQUIRED)
set(VNC_CORE ${CMAKE_CURRENT_LIST_DIR}/../vnc/core)
set(VNC_CORE_SRCS ${VNC_CORE}/des.cpp ${VNC_CORE}/encode.cpp ${VNC_CORE}/keysym.cpp ${VNC_CORE}/pixelformat.cpp
                  ${VNC_CORE}/rfb.cpp)
add_library(vnc_core STATIC ${VNC_CORE_SRCS})
target_include_directories(vnc_core PUBLIC ${VNC_CORE})
target_compile_options(vnc_core PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(vnc_core PUBLIC ZLIB::ZLIB)

add_executable(vnc_test vnc/main.cpp vnc/rfb.cpp vnc/encode.cpp)
target_include_directories(vnc_test PRIVATE ../thirdparty/doctest)
target_compile_definitions(vnc_test PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_link_libraries(vnc_test PRIVATE vnc_core)
add_test(NAME vnc COMMAND vnc_test)

# The fuzzers: every object instrumented, so the core's sources are built into each.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_executable(irc_message_fuzz irc/message_fuzz.cpp ${IRC_CORE_SRCS})
        target_include_directories(irc_message_fuzz PRIVATE ${IRC_CORE})
        target_compile_options(irc_message_fuzz PRIVATE -g -O1 -fsanitize=fuzzer)
        target_link_options(irc_message_fuzz PRIVATE -fsanitize=fuzzer)
endif()
