# VNC Server: the BMDS Desktop from Another Computer

Planned 2026-11-16 (the user's ask: a VNC server for BMDS, in this repository; BMDS gains what it
needs, and the hardware JPEG encoder is used where it fits). What BMDS adds for it is BMDS's
`devplan/vnc.md` (the display's output off, screen capture with the changed tiles, injected
input, `image_encode` into memory); this plan is the server. It ships after the BMDS release that
carries those. Paths below are this repository's unless they name BMDS.

Status: **approved** (2026-11-16, §10); **steps 1-3 built and run on a board** (§11, §12).

## 1. Goal

- **The desktop from any VNC viewer:** TigerVNC, TurboVNC, noVNC (through websockify),
  RealVNC; keys, the pointer and the wheel; the clipboard both ways.
- **Sharp text, fast pictures:** flat UI and text sent losslessly (solid fills, palettes,
  zlib), photos, video and games through the hardware JPEG encoder.
- **Safe by default:** listening on the board's loopback only, reached through SSH (BMDS has
  Dropbear), as BMDS's remote GUI programs are (its `guiprograms.md` P9). The network's
  interfaces only by choice, and then only with TLS.
- **Headless use:** the monitor's output off while a viewer is connected (BMDS's
  `display/output` = `remote`), giving PSRAM's bandwidth to the capture and the encoder.
- **Started by the user when wanted** (§5e): `vncserver` from `autoexec.sh` for a board that is
  always shared, or over SSH when it's needed later; it runs on in the background either way.
- **Always visible while it runs:** a tray icon with the viewers, and a notification when a
  viewer connects or leaves (§5d). Being watched is never silent.
- **An ordinary application** (`apps/vncserver.app`), with a settings window from its tray icon.

Not in it: being a viewer (a client is another app), file transfer, audio, several screens.

## 2. Facts that shape it

- **What BMDS gives** (its `vnc.md`): `SHELL`'s `watchScreen` and `screenChanged`;
  `gui_screen_capture()` copying the changed 32×16 tiles' rectangles from the shown picture into
  our buffer (`Bgr24` or `Rgb565`, whole frames); `injectKey` (HID usages) and `injectPointer`
  (absolute); `image_encode` into memory, `-BMDS_EOVERFLOW` when it might be cut. The server
  must be in `gui/shell-clients`: the user puts it there; the system shows a tray icon while it
  watches.
- **The JPEG codec:** 1280×720 in 45 ms (about 20 whole frames a second at most); one for the
  system, shared with the image viewer and Preso; `Bgr24` is its own input (no copy).
- **RFB has no JPEG encoding of its own:** JPEG comes through **Tight** (a rectangle as a fill,
  a JPEG, or palette/raw data through one of four zlib streams). TigerVNC, TurboVNC and noVNC
  speak it; RealVNC's viewer doesn't (ZRLE is the common one there).
- **Tight's 24-bit pixels** (`TPIXEL`: three bytes, R, G, B, when the client's format is 32-bit
  with 8-bit channels) are our `Bgr24` with the bytes swapped: no translation to the client's
  format for most viewers.
- **zlib's memory:** deflate's state is `2^(windowBits+2) + 2^(memLevel+9)` bytes: 256 KB at the
  defaults, per stream. A viewer inflates whatever window we chose, so small windows cost only
  ratio.
- **Programs have TLS** (BMDS's `network.md` §35: `tls_ctl`, libtls): VeNCrypt's X509 schemes
  are TLS on the socket before RFB's own security.

## 3. The shape

- `vnc/core/` (portable C++, host-tested like IRC's core):
  - `rfb` — the protocol: handshake (3.3, 3.7, 3.8), security types (None, VNC, VeNCrypt),
    `ClientInit`/`ServerInit`, the client's messages parsed from a byte buffer and the server's
    written into one; no I/O.
  - `keysym` — X keysyms to HID usages and modifiers (US layout first), characters with no key
    on the layout typed through their shifted key or dropped (§9).
  - `encode` — the per-rectangle chooser (§5a), Tight (fill, palette, basic, JPEG from a given
    stream), ZRLE, Raw, and the pixel translation for clients not on Tight's 24-bit path.
  - `vncauth` — the VNC challenge's DES (its odd bit order), for loopback use.
- `vnc/` — the BMDS side: the GUI connection (`SHELL`, the window, the tray icon), capture,
  `image_encode`, sockets and `poll`, settings.

## 4. Decisions to make

| | Proposed | Instead |
|---|---|---|
| A1 | **Tight first** (fill, palette+zlib, basic zlib, JPEG), **ZRLE** for viewers without Tight, **Raw** always; pseudo-encodings `DesktopSize`, `LastRect`, `QualityLevel`, `CompressLevel` | Raw and ZRLE only (no JPEG); Hextile (worse than ZRLE everywhere) |
| A2 | **Per rectangle, the cheapest that's exact, and JPEG only for pictures** (§5a): text stays sharp | JPEG for everything (blurred text, ringing round edges); the viewer's quality level alone deciding |
| A3 | **Small zlib windows:** `windowBits` 11, `memLevel` 6 (40 KB a stream; Tight's 4 and ZRLE's 1 a viewer: 200 KB in PSRAM), level from `CompressLevel` (default 6) | zlib's defaults (1.25 MB a viewer) |
| A4 | **zlib vendored** (`thirdparty/zlib`, its own licence kept) | miniz (smaller, slower on deflate); a BMDS call to the ROM's deflate (no streaming state across rectangles yet) |
| A5 | **Listen on `127.0.0.1:5900` with the VNC password** by default; `vnc/listen` = `lan` adds the interfaces, and then only VeNCrypt (X509Vnc: TLS with the board's own certificate, then the password) is offered | the LAN with VNC's DES challenge (readable on the wire, its password guessable offline); no authentication on loopback (any program on the board could drive the desktop) |
| A6 | **One viewer in v1, several in step 5**, each with its own watcher (BMDS allows 4) and its own zlib streams | sharing one capture among viewers (their requests come at different times) |
| A7 | **Pacing by the viewer and the socket:** an update is sent only for a pending `FramebufferUpdateRequest`, at most once a display frame; while the socket can't take more (`poll`), changes collect in BMDS's tile bitmap and the newest pixels win. Nothing is queued | sending each change as it comes (a slow link queues seconds of stale frames) |
| A8 | **The monitor off while viewed** offered in the settings window, as BMDS's `display/output` (`remote`): the server sets nothing itself | the server turning the output off and on (two owners of one state) |

## 5. Design

### 5a. Choosing an encoding for a rectangle (A2)

Each rectangle from `gui_screen_capture()` (whole 32×16 tiles, merged) is split into Tight's
largest allowed (2048 pixels wide, 64 KB of data before compression), then, in this order:

1. **One colour:** a fill (Tight's 4 bytes, or ZRLE's solid tile). Checked first and cheapest:
   the scan stops at the first different pixel.
2. **Two colours:** Tight's palette of two (a bit a pixel) through zlib: text on a background.
3. **Up to 16 colours** (counted with an early stop at 17): Tight's palette (a byte a pixel)
   through zlib: UI, icons, coloured text.
4. **Many colours, at least 64×32 pixels, and the viewer sent a `QualityLevel`:** a JPEG by the
   hardware at that level (`QualityLevel` 0-9 mapped to 30-90, 4:2:0 at 0-6 and 4:4:4 above),
   straight from the capture buffer (`Bgr24`, no copy). Narrower or shorter rectangles go to 5:
   a JPEG's headers outweigh them, and their MCU padding wastes the encoder.
5. **Otherwise:** Tight's basic (24-bit pixels through zlib), or, without Tight, ZRLE's tiles
   (each 64×64 tile solid, packed palette, RLE, or raw: ZRLE's own rule).

The capture buffer is in internal RAM for rectangles up to 64 KB (the colour counting reads it
at the CPU's speed) and in PSRAM for bigger ones, which go straight to the JPEG codec when step 4
picks them; a big rectangle that turns out flat is counted from PSRAM in 8 KB strips.

### 5b. One update

1. `screenChanged` (or the viewer's request with nothing pending): if a request is pending and
   the socket can take more, `gui_screen_capture()` into the buffer.
2. Encode each rectangle (§5a) into the send buffer; a JPEG with `-BMDS_EOVERFLOW` tried once
   at double the room, then as basic.
3. `FramebufferUpdate` with `LastRect` (the count isn't known when the header goes out), written
   as the socket takes it.
4. A change of the shown picture's size (a game flipped full screen in its own mode):
   `DesktopSize` to viewers that sent it; any other viewer is disconnected with a reason (RFB
   gives no other way to change the size; §9).

### 5c. Input

- `KeyEvent`: the keysym to a usage and modifiers (`keysym`), then `injectKey`; the modifiers
  are tracked from the viewer's own Shift, Control, Alt and Super keysyms, so a chord arrives as
  the local one would.
- `PointerEvent`: `injectPointer` at the position, with the buttons (1-3) and the wheel
  (buttons 4 and 5, 6 and 7 for sideways).
- `ClientCutText`: put on BMDS's clipboard (`GuiClipboard`); a change of the clipboard sent as
  `ServerCutText` (Latin-1 in RFB; UTF-8 with the Extended Clipboard pseudo-encoding, step 5).

### 5d. The window and the tray

- **Settings** (sysconf parameters of the app's own, `vnc/*`): the port, the password (stored
  as BMDS keeps passwords: hashed for the check, but VNC's challenge needs the password itself,
  so it is kept in the app's private settings, readable only by it), `vnc/listen`
  (`loopback`, `lan`), the JPEG ceiling, the monitor off while viewed (A8).
- **The tray icon, from start to end:** there while the server runs, whether a viewer is
  connected or not (its look says which: idle, or watched). Its menu lists the viewers (address,
  since when), each with Disconnect, then Settings... and Stop Server. A tooltip-like status line
  in the menu gives the address it listens on.
- **Notifications** (BMDS's notifications, `bmds_notification_t` through the GUI protocol's
  `Notification`): "Viewer connected: ADDRESS" (normal urgency: a balloon and a sound) with an
  action to Disconnect; "Viewer left: ADDRESS" (low: a balloon only); a failed password from an
  address, normal, and after 3 in a minute that address refused for a minute, critical. One on
  start ("VNC server listening on 127.0.0.1:5900"), low, so a server started from `autoexec.sh`
  or over SSH is noticed at the board.
- **BMDS's own "screen shared" icon** (its vnc.md R9) appears too while a viewer is connected:
  the system's, which the server can't hide; ours carries the controls.

### 5e. Starting and stopping

- **`vncserver`** starts the server and returns once it listens, printing the address; the
  server itself runs on in a session of its own (spawned with a new session, so the end of the
  SSH login or terminal that started it doesn't hang it up; BMDS's `posix_spawn` and `setsid` to
  check for this). `vncserver -f` stays in the foreground (for testing, logs to stderr).
- **From `autoexec.sh`:** the GUI service may not be up yet; the server waits for it (the GUI
  connection retried with backoff for up to `vnc/start-wait`, 30 s) before listening, so a line
  after `win start` or before it both work.
- **Over SSH:** the same command; the server connects to the board's GUI service as any local
  program does (the SSH session isn't its display).
- **One a port:** a second `vncserver` finds the first (its port taken, and a pid file in the
  app's runtime folder) and says so. `vncserver stop` ends it (a clean stop: viewers told,
  injected keys released by BMDS); `vncserver status` prints the address and the viewers.
- **Not in `gui/shell-clients`:** `vncserver` fails at once with the setting to change (and the
  window offers it when started from the desktop); it never adds itself.

## 6. Budget (estimates)

- **Program:** about 120 KB, zlib about 60 KB more.
- **Memory a viewer:** zlib streams 200 KB (PSRAM, A3); the capture buffer 64 KB (internal)
  plus 2.8 MB (PSRAM, a whole 720p frame, for the first update and JPEGs); the send buffer
  256 KB (PSRAM).
- **CPU:** typing in a terminal, under 10 % of a core (small palette rectangles); a full-screen
  video or game, 15-20 frames a second, held by the JPEG codec (45 ms at 720p).
- **Network:** a full 720p desktop as palette+zlib about 100-300 KB; as JPEG at quality 6
  about 150 KB; 100 Mbit/s is not the limit.

## 7. Testing

- **Host:** `rfb` against recorded handshakes and messages (each version, each security type);
  `encode` round trips through our own decoders for Tight (non-JPEG kinds) and ZRLE, against
  libvncclient's decoder in a test build; `keysym` tables; the chooser's picks on rendered UI,
  text and photos; fuzzers for the client's messages, as IRC's.
- **Board:** TigerVNC through `ssh -L 5900:localhost:5900`: the desktop, typing, Alt+Tab,
  menus, a game full screen with `DesktopSize`; noVNC through websockify; RealVNC on ZRLE; the
  monitor off while viewed, and back on with a local key; an hour connected with a terminal
  scrolling, memory unchanged.

## 8. Steps

1. **The core:** `rfb`, `vncauth`, Raw and ZRLE, `keysym`; host-tested.
2. **On BMDS:** capture, input and the socket; one viewer through SSH; Raw and ZRLE.
3. **Tight and the hardware JPEG:** the chooser (§5a), zlib streams, quality levels.
4. **The LAN:** VeNCrypt (X509Vnc) with the board's certificate.
5. **More:** several viewers, the clipboard's UTF-8 (Extended Clipboard), the pointer's shape
   (BMDS's open question), CopyRect for window moves.

## 9. Open questions

- **Layouts:** a viewer sends keysyms for characters; on a non-US BMDS layout, the usage for a
  keysym comes from BMDS's layout tables once they exist (BMDS's input roadmap).
- **The password's storage** next to BMDS's own (SSH's in sysconf): one place for secrets an
  application needs in clear.
- **A game in a mode of its own** without `DesktopSize` in the viewer: disconnect (proposed), or
  scale the picture into the desktop's size by the PPA.

## 10. Decided (2026-11-16, with the user)

- **A5, how it's used:** started by the user, from `autoexec.sh` or over SSH (§5e); listening on
  loopback by default stands, reached through an SSH tunnel.
- **Seen while it runs:** a tray icon for its whole life and notifications for viewers coming and
  going (§5d), beside BMDS's own "screen shared" icon.
- **The whole plan approved** ("go ahead"), 2026-11-16.

## 11. As built (2026-11-16): steps 1-3, built and host-tested; no board yet

Written on a laptop without a board, against BMDS's checkout (its screen capture is new and also
untried on the hardware: BMDS's vnc.md §11). Everything below builds for the board without
warnings, and the core's tests pass on the PC under ASan and UBSan.

- **The core** (`vnc/core`, `tests/vnc`, 14 cases):
  - `bytes.h`: a `Writer` that refuses to run past its buffer (`overflowed()`, `truncate()`).
  - `des`: DES's one-block encryption and VNC's response, checked against OpenSSL on 11 vectors and
    a VNC challenge. (The first S-boxes, typed from memory, had the last box's tail wrong: the
    vectors caught it; the tables now match busybox's copy of FIPS 46-3.)
  - `rfb`: RFB 3.3, 3.7 and 3.8 (and later, as 3.8), None or VNC authentication, ClientInit and
    ServerInit, the viewer's six messages (whole or split anywhere), the Extended Clipboard's form
    skipped, a colour map refused; the server's messages.
  - `pixelformat`: PIXEL, ZRLE's CPIXEL and Tight's TPIXEL for any true-colour format.
  - `encode`: Raw, ZRLE (solid, packed palette, raw tiles) and Tight (fill, two-colour and
    palette filters, basic, JPEG through a callback), as §5a chooses; zlib streams of 40 KB (A3).
    Each is decoded back in the tests by a viewer of the tests' own (zlib's inflate), across
    updates, in 32 and 16 bits. `worstCase()` says how much room a rectangle needs. A rectangle
    that didn't fit resets Tight's streams (the viewer told by the next control byte); ZRLE's
    stream can't be reset, so the session ends (`isBroken()`).
  - `keysym`: Latin-1's characters, the editing, function and keypad keys and the modifiers on a
    US layout; Shift added for a character typed with it when the viewer didn't send it.
- **The program** (`vnc/main.cpp`, `vnc/server.cpp`; 286 KB):
  - `vncserver` starts a copy of itself with `vfork` and `setsid` and waits on a pipe for "0
    ADDRESS" or "1 REASON"; `-f` runs in the foreground; `stop`, `status`, `passwd`. The pid is
    in `/tmp/vncserver.pid`.
  - **The password** is in `$HOME/.config/vncserver/password` (mode 600), not sysconf: a
    `SECRET` setting reads back as "(set)", and VNC's challenge needs the password itself (§9).
  - The settings it declares: `vnc/port`, `vnc/listen` (`lan` refused until VeNCrypt, A5),
    `vnc/jpeg-quality`, `vnc/start-wait`.
  - One viewer at a time; the screen watched only from its ServerInit to its leaving, so the
    system's icon shows exactly while someone watches. Updates as §5b: a capture into a 1 MB
    buffer (the rest left for the next update: `more`), each rectangle encoded in bands of 16 rows
    sized to the room left in a 1 MB update, a size change sent as DesktopSize (or the viewer let
    go). The JPEG is `image_encode` into memory (4:2:0, 4:4:4 from quality 90).
  - Keys through `KeyState` into `Shell.injectKey`, the pointer and the wheel (buttons 4 and 5)
    into `injectPointer`.
  - The tray icon for the server's life (the viewer and since when, Disconnect, Stop Server), and
    notifications: started (low), a viewer connected (normal, with Disconnect), left (low), a
    wrong password (normal) and the third within a minute (critical; that address refused for a
    minute).
  - About 2.7 MB of PSRAM with a viewer: the update and capture buffers (1 MB each), the
    encoder's working memory (512 KB), zlib's streams.
- **Not done yet:** the clipboard both ways (step 5), several viewers, VeNCrypt, CopyRect, the
  pointer's shape; the package isn't in the repository's list until the BMDS release with screen
  capture.

## 12. On the board (2026-11-16, bmds0)

Through `ssh -L 15900:localhost:5900 root@bmds0.local`, with `tools/vncview.py` (a test viewer:
RFB 3.8, each encoding decoded with Pillow) as the viewer; BMDS's own fixes on the way are its
vnc.md §12.

- **Starting:** `vncserver` returned with "Listening on 127.0.0.1:5900" and the server ran on in
  its own session; `status`, `stop` (SIGTERM, clean), `passwd` all worked.
- **The picture:** Raw, ZRLE and Tight each matched `screenshot` but for the notification balloons
  that came and went between them. A 1 MB update carries about 190 rows of Raw; the next request
  takes the rest. Tight with quality 6 sent JPEGs for the wallpaper.
- **Input:** `echo vnc ok` typed into a terminal through the viewer ran.
- **Told:** "Viewer connected" with Disconnect, the tray icon, and the system's "screen shared"
  icon beside it; "VNC server started"; wrong passwords twice, then the third's critical "someone
  is guessing"; the right password refused for the minute after.
- **Found and fixed:**
  - The first update could be asked for before the service had taken `watchScreen`: the capture
    answered `-BMDS_EACCES` and the viewer was dropped. Updates now start with the first
    `screenChanged`, and a refused capture keeps the request (a full one too).
  - `-v` (with `-f`): what the loop does, on stderr. It found the next two, in BMDS: a
    non-blocking `send()` of the 1 MB update returned `EAGAIN` for ever, and the loopback's unread
    data could hold all of lwIP's heap (every SSH login hung). Both fixed there (its network.md §36).
- **To think about:** every viewer through an SSH tunnel comes from 127.0.0.1, so three wrong
  passwords from one of them lock all of them out for a minute. Counting per connection rather
  than per address, or not locking loopback out, is the choice to make (§9).

## 13. The network without TLS, with a password (2026-11-16, the user's choice)

A5 changed: `vnc/listen` = `lan` is taken when a password is set (refused without one, saying to
run `vncserver passwd`), until VeNCrypt (step 4) gives TLS. The start's notification says, at
normal urgency, that the session isn't encrypted. Tried on bmds0: Tight with JPEG straight to
172.20.201.169:5900, a wrong password refused.

`vncserver status` said where the settings point now, not where the running server listens (the
user had changed `vnc/listen` under a running server and couldn't connect: the settings are read
at the start). The pid file's second line is now the address the server bound, and `status` says
that.

## 14. One icon, and Blank Display While Connected (2026-11-16, the user's ask)

- **One icon:** the server's own; while a viewer is in, BMDS draws its red dot on it (BMDS's
  vnc.md §13), and the system's separate display icon no longer shows.
- **Blank Display While Connected**, a checkbox in the tray's menu. BMDS has this behaviour
  already as `display/output` = `remote` (off while a program watches; the server watches only
  while a viewer is in), so the item is that setting rather than a second one that would fight
  it: checked sets `remote`, unchecked `on`, saved either way; a watch on the setting keeps the
  check right when it changes elsewhere (Settings, `sysconf`).
- **Tried on bmds0:** clicking it with a viewer in turned the HDMI dark at once (and saved
  `remote`); the display came back as the viewer left; the check followed `sysconf setdef` from
  the shell.

## 15. No setting to add (2026-11-16)

BMDS's `ScreenShare` (its vnc.md §14) takes any local program: the server uses it in place of
`Shell` and no longer asks for `gui/shell-clients`, so `pkg install vncserver` then `vncserver`
works, from the desktop, `autoexec.sh` or SSH. The tray's red dot on the server's icon is what
tells the user the screen is watched. Tried on bmds0 with `gui/shell-clients` empty.

## 16. Speed, first round (2026-11-16, the user's ask)

Measured on bmds0 with `-v`'s new per-update times (capture, JPEG, zlib, encode) and
`tools/vncview.py --skip --seconds N` (parsing without decoding, so the viewer isn't the limit).

- **Where the time was:** not the hardware JPEG (15 ms for 260K pixels), but the CPU: per-pixel
  conversion through `Picture::rgb()` and `PixelFormat`'s generic stores, a palette searched for
  every pixel, and two passes over the captured pixels (in PSRAM) for a palette; then zlib.
- **Done:**
  - Row pointers and the last colour's answer kept; the colours counted and indexed in one pass
    (the indices are the palette's data as they come); two colours packed in place.
  - Fast paths for the usual 32-bit format (`PixelFormat::isBgrx()`): Raw's pixels a byte
    shuffle, ZRLE's raw tiles a `memcpy` (its CPIXEL is the screen's B, G, R), Tight's 24-bit
    pixels a byte swap.
  - zlib's `Z_RLE` strategy on Tight's palette streams (indices are runs).
- **Measured, the board idle:** a whole 1280x720 desktop in three updates, about 290 ms on the
  server (340 before); Raw 49 ms a megabyte (112). An update of 260K pixels of desktop is now
  about 20 ms capturing, 40-70 ms in zlib, 25-35 ms the rest; JPEG areas about 45 ms a megabyte.
- **Under load it is the core, not the code:** a terminal scrolling flat out gave 2 updates a
  second. The server had 31 % of a core: the loader placed it on core 1 when the board was idle,
  and the scrolling shell and the terminal took core 1 too, while core 0 was half idle.
- **Next, by what they would win:** the capture by 2D-DMA in the kernel (20 ms a megabyte of CPU,
  under the window manager's lock); choosing the core (or a second thread on the other one) when
  the board is busy; CompressLevel 0 as zlib's stored blocks (a LAN carries an uncompressed
  palette faster than zlib makes one: 260 KB is 22 ms at 100 Mbit/s); Tight's large solid areas
  split out as fills before the palette (libvncserver's), so a mostly flat rectangle isn't a
  byte a pixel through zlib.

## 17. A worker on the less busy core (2026-11-16, the user's go-ahead on §16's first two)

- **The worker:** a thread captures and encodes each update; the main thread keeps the sockets,
  the GUI and input. A job at a time, through two pipes (the loop's `startUpdate()`, the worker's
  `buildUpdate()`, the loop's `updateDone()`). The viewer's SetPixelFormat and SetEncodings are
  held and given to the encoder before the next job, so the threads never share it; `drop()`
  waits for a job under way.
- **Its core:** it pins itself first thing, in an entry with no floating point or PIE (either fixes
  a thread's core), as libbmdsgame's audio threads do. At the start, the less busy core over 200 ms
  (`cpu.coreN.busy_us`); then every 3 s, the cores' load against its own share: moved (a new
  thread, the old one ended) when the other core, the worker aside, is 20 points less busy.
- **The capture buffer** is on `BMDS_CAPTURE_ALIGN`, so BMDS's 2D-DMA copies into it (its vnc.md
  §15): a megabyte in 7-10 ms (21 by the CPU).
- **Measured on bmds0:** with a terminal scrolling flat out on core 1, the worker went to core 0
  and had 85 % of it; when Doom then loaded core 0, it moved itself to core 1. Updates of the
  scrolling terminal 2.4 a second (2.0): the limit is now zlib, about 200 ms an update for 400 KB of
  palette indices, on a core of its own.
- **Found: memory.** Doom (8.4 MB) and a viewer (about 2.5 MB: the update and capture buffers, 1 MB
  each, the encoder's 512 KB) left PSRAM's largest block at 2.4 MB; a second viewer's buffers then
  couldn't be had and it was closed at once, and SSH logins failed (internal RAM down to 7 KB, from
  the game). Smaller buffers are the next thing (§9).
- **Next for speed:** zlib: CompressLevel 0 as stored blocks for a fast network, and Tight's large
  solid areas as fills before a palette (§16).

## 18. Smaller buffers, solid areas, compression level 0 (2026-11-16, the user's go-ahead)

- **Memory:** the update buffer 384 KB (1 MB), the capture buffer 768 KB (1 MB), Tight's largest
  piece 32K pixels (64K), so the encoder's two buffers 128 KB each: about 1.5 MB with a viewer
  (2.6), measured. JPEG's least height is 16 rows (32): 1280-wide pieces are 16 rows now.
- **Solid areas** (`tightSolids()`, libvncserver's sizes): a Tight rectangle of 4K pixels or more
  is searched on a 16-pixel grid for a solid block, grown right then down; 2K pixels or more go as
  a fill and the four rectangles around it are encoded the same way. A test of a terminal-shaped
  picture (text on the left, dark elsewhere) checks the fills and that it decodes exactly. A whole
  idle desktop is now 67 KB at level 6.
- **CompressLevel 0** is zlib's stored blocks already (`deflateInit2` at level 0): nothing to add.
- **Measured on bmds0,** a terminal scrolling flat out, the server's throughput (pixels sent over
  its time): with the 384 KB capture buffer tried first, 1.11 Mpixel/s at level 6 (the 1 MB one
  gave about 1.2): each capture pays about 35 ms under that load (the window manager's round and
  the frame), more often with smaller captures. With 768 KB: **1.38 Mpixel/s at level 6** (59 KB a
  Mpixel sent), **2.60 at level 0** (596 KB a Mpixel: 0.6 MB/s, nothing on a LAN). The solid areas
  don't help this case (`ls -l` fills most lines); they do on desktops and editors.
- **Next:** the capture's wait under load (§9); a viewer's own choice of level stays its own.
- **Then the capture's wait** (BMDS's vnc.md §16): the desktop's latest present copied without the
  window manager's lock; a capture under load 11 ms (21-33); scrolling 4.5 Mpixel/s at level 0,
  2.5 at level 6, nothing changed here.
