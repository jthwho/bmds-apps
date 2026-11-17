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

# The fuzzers: every object instrumented, so the core's sources are built into each.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_executable(irc_message_fuzz irc/message_fuzz.cpp ${IRC_CORE_SRCS})
        target_include_directories(irc_message_fuzz PRIVATE ${IRC_CORE})
        target_compile_options(irc_message_fuzz PRIVATE -g -O1 -fsanitize=fuzzer)
        target_link_options(irc_message_fuzz PRIVATE -fsanitize=fuzzer)
endif()
