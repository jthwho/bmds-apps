# bmds-apps

Applications for [BMDS](https://github.com/jthwho/bmds), each a package that `pkg` installs,
and the repository that serves them: **Minesweeper**, **IRC**, the **VNC Server** and the **I/O
Manager**; more of BMDS's non-core applications will move here. IRC and the VNC server need
BMDS 0.4.0 or newer; the I/O Manager a BMDS newer than 0.4.0.

It is also the worked example of publishing packages for BMDS. Everything below applies to a
repository of your own: fork this one and follow [Your own repository](#your-own-repository).

## Installing the apps on BMDS

Add this repository once, in a terminal:

    pkg source add https://github.com/jthwho/bmds-apps/releases/latest/download
    pkg install minesweeper irc vncserver ioman

or in **Settings ▸ Software ▸ Add Source...** with the same address, then `pkg install`.

The packages are signed with the **bmds-apps key**:

    id   37ddce8c8eb0890a
    key  ed25519 1ca1f8bbb9d5130890d49226b003e6f93c2a4d94362159970993da9f295fb151

BMDS firmware has this key built in (`keys/packages.pub`, for packages only: it can't sign a
firmware update), so adding the source asks nothing. On a firmware without it, `pkg source add`
fetches `key.pub` from the release, shows the key and asks before trusting it: compare it with
the one above. Either way the source is then pinned to this key (`signed-by=37ddce8c8eb0890a`),
and an index signed by any other key is refused.

`pkg upgrade` brings each app to the newest release's version.

## What is here

    CMakeLists.txt          the SDK, each app, and the repository (bmds_add_repository)
    cmake/sdk.cmake         finds or downloads the BMDS SDK, before project()
    keys/bmds-apps.pub      the public half of the signing key (the private half is never here)
    minesweeper/            the game: field.c (rules), draw.c (pictures), main.c (the window),
                            app.desktop, icons/, DESCRIPTION, and its CMakeLists.txt
    irc/                    IRC (devplan/irc.md): core/ its portable C++ core, bmds/ the program;
                            app.desktop, DESCRIPTION, servers.ini
    vnc/                    the VNC server (devplan/vnc.md): core/ is its protocol and encodings in
                            portable C++, main.cpp and server.cpp the program; app.desktop,
                            DESCRIPTION
    ioman/                  the I/O Manager (devplan/ioman.md): core/ the pin list in portable C++,
                            main.cpp the program; app.desktop, DESCRIPTION, icons/
    tests/                  the rules' tests, on the PC: Minesweeper's, IRC's, VNC's and the I/O
                            Manager's cores, the fuzzers
    thirdparty/doctest/     the C++ tests' framework (MIT)
    thirdparty/zlib/        zlib 1.3.1's deflate, for the VNC server (the zlib licence; VENDORED.md)
    devplan/                designs, as BMDS keeps them
    tools/icons.py          draws Minesweeper's and the I/O Manager's icons
    tools/vncview.py        a test viewer for the VNC server (Python, Pillow)
    tools/release           builds, signs and (with --publish) releases
    tools/irc-test-server   a small IRC server (TLS with its own CA) to try IRC against
    .github/workflows/      the release workflow: a tag in, a GitHub release out

## Building

You need CMake (3.20 or newer), Ninja, Python 3 with the `cryptography` module, and Espressif's
RISC-V GCC: the version in the SDK's `sdk/TOOLCHAIN`, installed by ESP-IDF or from Espressif's
release tarball (the workflow shows how). The BMDS SDK itself is downloaded by the build: the
release pinned in `CMakeLists.txt` (`BMDS_SDK_VERSION`, `BMDS_SDK_SHA256`), checked against its
hash. To build against a BMDS checkout instead (a newer SDK, or BMDS's own development), give
`-DBMDS_SDK_DIR=<bmds>/sdk`.

    cmake -S . -B build -G Ninja -DBMDS_KEY=$HOME/.config/bmds/bmds-apps.key
    cmake --build build

`build/repository/` is then a complete repository: `index.bmpi`, the packages
(`minesweeper_1.0.0.bmpk`, `irc_0.1.0.bmpk`, `vncserver_0.1.0.bmpk`), `key.pub`. Without
`-DBMDS_KEY`, the dev key signs (the build warns): fine for trying on your own
board, but a source pinned to the bmds-apps key won't take it.

The rules' tests run on the PC, with the PC's compiler (C and C++, under the sanitizers):

    cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

Configured with clang (`CC=clang CXX=clang++`), `tests/` builds the fuzzers too (libFuzzer):
`irc_message_fuzz CORPUS-DIR` and `irc_session_fuzz CORPUS-DIR`.

### Trying it on a board

Any of:
- copy the package to the card and install it from the file: `pkg install ./minesweeper_1.0.0.bmpk`;
- serve the repository from your PC and add it as a source (what users do, over your network):

      cd build/repository && python3 -m http.server 8000
      # on the board:
      pkg source add http://YOUR-PC:8000
      pkg install minesweeper

- or copy `build/minesweeper/minesweeper.app` into the card's `apps/`, as the SDK's examples do.

## How a package is made

Each app's `CMakeLists.txt` uses the SDK's functions (`sdk/cmake/BmdsProgram.cmake` and
`BmdsPackage.cmake` in BMDS):

```cmake
set(MINESWEEPER_VERSION 1.0.0)
bmds_add_program(minesweeper SOURCES main.c field.c draw.c ICONS icons/16.png icons/32.png)
bmds_add_app(minesweeper PROGRAM minesweeper DESKTOP app.desktop)
bmds_add_package(minesweeper VERSION ${MINESWEEPER_VERSION}
        SUMMARY "Clear the field without setting off a mine"
        DESCRIPTION_FILE DESCRIPTION LICENSE MIT HOMEPAGE https://github.com/jthwho/bmds-apps
        APPS minesweeper)
```

- `bmds_add_program()` builds the program, with its icons built into it.
- `bmds_add_app()` makes `minesweeper.app`: the program and `app.desktop`, which gives the
  launcher its name, category and id (`X-BMDS-Id`, which names the app's data folder).
- `bmds_add_package()` makes `packages/minesweeper_1.0.0.bmpk`, installing
  `apps/minesweeper.app`. The package's title, category and icons come from the app unless
  given; its first description line is what the package list shows. It needs the SDK's ABI
  version of the firmware at least.
- In the top `CMakeLists.txt`, `bmds_add_repository(repository PACKAGES minesweeper ...)` puts
  every package in `repository/` with their index and `key.pub`, and checks the index as a board
  would.

**Versions:** a package's version says which is newer (`1.0.0` < `1.0.1` < `1.1`); raise it
whenever the app changes, or `pkg upgrade` won't install the change.

## Keys

A repository's index and packages are signed, so a board can check that they came from whoever
holds the key, whatever server they were downloaded from. The key was made once with BMDS's
`tools/mkpkg` (in the SDK tarball too):

    mkpkg keygen ~/.config/bmds/bmds-apps.key --comment bmds-apps

which wrote two files and printed the key's id:

- **`bmds-apps.key`, the private half.** It never goes in a repository. Back it up: if it is lost,
  nothing more can be signed as bmds-apps, and every user has to trust a new key. For the
  release workflow it is the repository secret `BMDS_APPS_KEY` (its file's whole text:
  `gh secret set BMDS_APPS_KEY < ~/.config/bmds/bmds-apps.key`), which the workflow writes to a
  temporary file for the signing step only.
- **`bmds-apps.pub`, the public half**: `keys/bmds-apps.pub` here, `key.pub` in every release
  (what `pkg source add` fetches), and its id in this README, for users to compare.

BMDS knows four kinds of key (its `keys/README.md`): the **dev** key (anyone has it), **trusted**
keys (built into a firmware; they can sign firmware updates too), **package** keys (built in, for
packages only: bmds-apps's), and keys **added** on a board (`pkg key add`, or trusted through
`pkg source add`). A repository's key must never be a trusted key: whoever holds it could then
sign firmware for every board that trusts it.

**Changing the key** (lost, or exposed): make a new one, sign with it, and publish both
public halves for a while; users add the new key (`pkg key add`) and their source names both ids
(`signed-by=OLD,NEW`) until the old one is gone.

## Releasing

A release is a git tag `vN` (one counter for the repository; each app keeps its own version):

    git tag v2 && git push origin v2

The workflow then builds everything from that commit with the pinned SDK, signs it with
`BMDS_APPS_KEY`, and makes the GitHub release with every package, `index.bmpi` and `key.pub`.
**Every release holds every package**, unchanged ones at their old version: the index users read
is the newest release's (`releases/latest/download/index.bmpi`), and its packages' names are
relative to it, so they must be beside it. A tag with a `-` (`v2-rc1`) makes a pre-release, which
"latest" never points at: try it with `pkg source add` on its own download URL
(`.../releases/download/v2-rc1`).

Until an SDK release is pinned in `CMakeLists.txt`, the workflow only notes that and stops: such
a release is made by hand, as below, with `--sdk-dir <bmds>/sdk`.

Or release from your own machine, the key never leaving it:

    tools/release --key ~/.config/bmds/bmds-apps.key --publish v2

(`--dev` makes one from any tree without publishing, to look at; `--sdk-dir` builds against a
BMDS checkout.)

## Your own repository

1. Fork this repository (or start from its layout).
2. Make your key: `mkpkg keygen ~/.config/bmds/mine.key --comment "My apps"`. Replace
   `keys/bmds-apps.pub` with your `.pub`, and put your key's id and line in your README.
3. Add the private half as your repository's `BMDS_APPS_KEY` secret (or rename it in the
   workflow), and keep a backup.
4. Change the names: `tools/release`'s `DEFAULT_REPO`, the `HOMEPAGE`s, and the addresses in your
   README.
5. Replace or add apps: a folder each with its `CMakeLists.txt`, an `add_subdirectory()` and its
   name in `bmds_add_repository()`.
6. Tag and push. Your users run `pkg source add https://github.com/YOU/REPO/releases/latest/download`,
   check the key it shows against your README, and say yes.

## Licence

MIT (`LICENSE`).
