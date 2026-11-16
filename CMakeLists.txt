# bmds-apps: applications for BMDS, each a package, and the repository that serves them
# (README.md says how to build, sign and publish).
#
#   cmake -S . -B build -G Ninja [-DBMDS_KEY=~/.config/bmds/KEY.key] [-DBMDS_SDK_DIR=<bmds>/sdk]
#   cmake --build build          # build/repository/: index.bmpi, the packages, key.pub
cmake_minimum_required(VERSION 3.20)

# The SDK: a BMDS release's (pinned here), or a checkout's with -DBMDS_SDK_DIR. Not released yet:
# until a BMDS release carries bmds-sdk-VERSION.tar.gz, build with -DBMDS_SDK_DIR.
set(BMDS_SDK_VERSION "" CACHE STRING "")
set(BMDS_SDK_SHA256 "" CACHE STRING "")
include(cmake/sdk.cmake)

project(bmds-apps C ASM)
include(${BMDS_SDK_DIR}/cmake/BmdsProgram.cmake)

# The apps: each folder makes its program(s) and its package.
add_subdirectory(minesweeper)

# The repository: every package, its index and the public key, signed with BMDS_KEY. A GitHub
# release of these files is the source users add:
#   pkg source add https://github.com/jthwho/bmds-apps/releases/latest/download
bmds_add_repository(repository PACKAGES minesweeper LABEL "bmds-apps")
