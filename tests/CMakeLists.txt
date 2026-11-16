# Minesweeper's rules on the PC, with the PC's own compiler (the apps are built with the SDK's).
#
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.16)
project(bmds-apps-tests C)
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror -fsanitize=address,undefined)
add_link_options(-fsanitize=address,undefined)
add_executable(field_test field_test.c ../minesweeper/field.c)
target_include_directories(field_test PRIVATE ../minesweeper)
enable_testing()
add_test(NAME field COMMAND field_test)
