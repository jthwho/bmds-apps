# IRC: a Chat Client of Our Own

Planned 2026-11-16 (the user's ask: an IRC client for BMDS; ported or our own; ours: BMDS's
network.md §35, step 6, after TLS for programs). In bmds-apps, released after the next BMDS
release, which brings what it needs: TLS sockets (`tls_ctl`, network.md §35) and `ChatView`
(BMDS's `devplan/chatview.md`). **Approved as proposed** 2026-11-16 (§11).

Status: **step 0 built in BMDS** (`ChatView`: its chatview.md §7-§9; the tabs' additions: its
tabs.md §7-§9; the input line's keys: its chatview.md §10); **steps 1 to 3 built** (§12; §13,
§14: the core; §15: the app, against `tools/irc-test-server`, then Libera.Chat); step 4 next.
Paths below are this repository's unless they name BMDS.

## 1. Goal

- **A GUI IRC client that feels native:** a window with the networks and channels, the
  conversation, the people in it and a line to type in; links that open, the clipboard, tray
  notifications on a mention or a private message. Built on the public GUI API only, an ordinary
  application (`apps/irc.app`), a package from this repository.
- **Today's IRC:** TLS by default (6697), SASL (PLAIN, and EXTERNAL with a client certificate:
  CertFP), IRCv3's capabilities (`server-time`, `message-tags`, `away-notify`, `account-notify`,
  `extended-join`, `multi-prefix`, `cap-notify`, `chghost`, `echo-message`), a bouncer's playback
  shown at its own times.
- **Private chats** (queries) as well as channels: one person, in a tab of their own (§7b).
- **Logging in as mIRC offers it** (§7a): SASL, NickServ, a server password, or none, chosen
  for each network; registering a nick with NickServ from the client, and taking a nick that is
  yours back from whoever holds it.
- **Several networks at once** (no singletons): each its own connection, nick and channels.
- **Robust:** reconnects with backoff, rejoins, keeps what was typed, logs every buffer to the
  card.
- **Small:** a few hundred KB of program, scrollback in PSRAM with a cap, nothing polling.

Not in it: DCC (file transfers, and DCC CHAT: a direct connection, which NAT mostly stops;
private chats go through the server, §7b), scripting, a server, IRC over plain TCP by
default (allowed, not offered first).

## 2. Facts that shape it

- **Programs have TLS** from the next BMDS release (its network.md §35): `tls_ctl` on a socket,
  libtls (`tls.h`), fingerprints for pinning (`peer_sha256`: CertFP), client certificates, `poll`
  that never spins.
- **The GUI service has no class for chat text yet:** `Terminal` takes ANSI colours (monospace, no
  links, no reflow), `TextEdit` is one style, `Canvas` draws anything. BMDS's
  `devplan/chatview.md` plans `ChatView` for the next release: lines styled with mIRC's codes,
  wrapped with a hanging indent, links, a marker, selection.
- **BMDS's `Tabs` is made for dialogs:** 16 tabs at most, a row cut off when they don't fit, no
  close button, no mark for news. BMDS's `devplan/tabs.md` adds what a chat client needs, in the
  same release as `ChatView`.
- **A package can't require a firmware version:** `BMDS_ABI_VERSION` stays 1 while system calls
  and classes are appended, and programs probe for what they need (BMDS's `docs/ABI.md`, rule 8).
  So the client checks at its start for `tls_ctl` (`bmds_sys_has`) and `ChatView` (`Describe`) and,
  without them, says "IRC needs a newer BMDS (TLS sockets and ChatView): update the firmware"
  instead of failing later.
- **IRC's formatting is already a markup:** `\x02` bold, `\x1d` italic, `\x1f` underline, `\x1e`
  strike, `\x11` monospace, `\x16` reverse, `\x03FG,BG` colours (99 of them), `\x04RRGGBB`,
  `\x0f` reset. Compact, well known, and what a client must parse anyway.
- **Apps keep their own data** in `bmds_app_data_dir()` (`$HOME/.local/share/ID`, apps.md §6);
  system settings belong in sysconf. A network list, nicks and SASL passwords are a user's
  data, not the system's.
- **This repository today:** C programs (Minesweeper), their rules tested on the PC with the PC's
  compiler and sanitizers (`tests/`), the SDK pinned to a release (0.3.1) or a BMDS checkout's
  (`-DBMDS_SDK_DIR`). It is still taking shape: it gains what the client needs (§3a).
  BMDS's `tools/tls-test-server` is the pattern for a test peer on the PC.

## 3. The shape

```
irc/core/              portable C++ (C++20, no exceptions or RTTI; no libstdc++ on the board):
                       tested on the PC
  message.*            a line's tags, source, command, parameters; and one built
  format.*             mIRC codes <-> spans; links found; colours; stripped text
  session.*            one network's protocol: registration, CAP, SASL, PING, nick fallback,
                       ISUPPORT (CASEMAPPING, PREFIX, CHANTYPES, NETWORK), channels and their
                       members, queries, modes, away; events out, lines to send out. No sockets.
  buffers.*            each channel's and query's state: unread and mention counts, attention,
                       who spoke last; events made into its view's line and its log's (the
                       lines themselves kept by its ChatView and the log, §14)
  commands.*           what is typed: /join /part /msg /me /query /nick /topic /away /whois
                       /mode /kick /quit /raw /clear /close; the rest sent as it is
  complete.*           Tab: nicks (recent speakers first), channels, commands
irc/bmds/              the platform: sockets and TLS (non-blocking, tls_ctl), the GUI
                       (libbmdsgui), poll() over the GUI's fd and every network's, timers,
                       notifications and the tray, logs and settings files
```

The core takes bytes and time and gives lines to send and events: no I/O, so the host tests drive
whole conversations (a scripted server's lines in, what the client sends checked), and a fuzzer
reads messages. As BMDS's Preso (its preso.md §2), the core could later run on a PC.

### 3a. What this repository gains

- **C++:** `project(bmds-apps C CXX ASM)`; the SDK builds C++ programs already (`bmds-c++`: its
  freestanding headers, no exceptions or RTTI).
- **PC tests in C++:** `tests/` gains CXX and doctest (vendored, as BMDS's `thirdparty/doctest`),
  the sanitizers kept; a fuzzer target under clang when there is one (`-fsanitize=fuzzer`).
- **`tools/irc-test-server`:** a small IRC server in Python for the board's tests (TLS with its
  own CA, CAP, SASL PLAIN and EXTERNAL, scripted joins, messages and failures), as BMDS's
  `tools/tls-test-server` is.
- **A `devplan/`** (this file), as BMDS has.
- **The SDK pin raised** to the next BMDS release when it is out (`BMDS_SDK_VERSION` and its
  hash); until then the client builds with `-DBMDS_SDK_DIR=<bmds>/sdk`, and isn't in the
  repository's package list (`bmds_add_repository`), so a release from here before then doesn't
  carry it.

## 4. Decisions to make

| | Proposed | Instead |
|---|---|---|
| I1 | **The chat view: BMDS's new `ChatView`** (its `devplan/chatview.md`), in the GUI service: lines appended with mIRC's codes as their styling, wrapped with a hanging indent, links clickable, selection and copy, a marker line, following the end unless scrolled back; for any program's chat or log | `Terminal` fed ANSI (works today; monospace, no links, a wrap that doesn't indent and doesn't reflow); our own layout on a `Canvas` (all of it in the app, and no other program gets it) |
| I2 | **C++ for the core and the app**, as BMDS's Preso and angel are (the SDK's C++: no exceptions, RTTI or libstdc++); this repository gains C++ (§3a) | C, as Minesweeper (more code for strings and lists); angel script (slower, and a chat client is a long-lived program) |
| I3 | **Its settings in its data folder:** `networks.ini` (a section a network: address, port, TLS, a pinned fingerprint, nicks, user and real name, the login (§7a), the channels to join, commands to send on connecting), `settings.ini` (appearance, highlights, notifications, logging); written `0600` | sysconf (system-wide, for every user: a nick and a password aren't the system's); a format of our own |
| I4 | **Logs:** `logs/NETWORK/BUFFER/YYYY-MM-DD.log` in the data folder, plain text with times; the folder a setting of the app's (`settings.ini`), as paths are settings | none; one file a buffer |
| I5 | **TLS by default:** 6697, the certificate checked; a self-signed server by pinning its fingerprint (offered, with the fingerprint shown, when the check fails), never "accept everything" silently | TLS optional and off |
| I6 | **The first networks offered:** Libera.Chat and OFTC, each one click and a nick | none: type an address |
| I7 | **One window; its conversations in tabs** (the default) **or a tree** (View), the user's choice (§6); a second window for a buffer torn off later | the tree only (as first proposed) |
| I8 | **The program's name `irc`, the application "IRC"**, `apps/irc.app`, a package here (`irc`), released after the next BMDS release | another name |

## 5. `ChatView` (I1)

BMDS's (`devplan/chatview.md`): built and released there first. What the client relies on: a
line's styles in mIRC's codes (what the core's `format` keeps, so a message is shown as sent), a
tab between the gutter (time and nick) and the text, `linkActivated`, `setMarker`,
`scrolledBack` for the unread counts, `maxLines`.

## 6. The window (I7)

```
+---------------------------------------------------------------------------+
| IRC   Network  Channel  View  Help                                        |
+---------------------------------------------------------------------------+
| Libera.Chat | #bmds [3] | #c | alice [1] x | OFTC | #oftc            < > v |
+--------------------------------------------------------------+------------+
| #bmds: BMDS, an OS for the ESP32-P4 | topic                  |  @jth      |
|--------------------------------------------------------------|   alice    |
| 10:41 <alice> did the TLS land?                              |   bob      |
| 10:42 <jth>   yes, slot 135                                  |            |
| 10:42 * bob waves                                            |            |
| ------------------ new since 10:40 -------------------       |            |
+--------------------------------------------------------------+------------+
| [jth] [I'm typing here...                                              ]  |
| Libera.Chat: connected (TLS 1.2, SASL as jth), lag 42 ms                  |
+---------------------------------------------------------------------------+
```

- **Tabs** (the default): a tab a conversation, in the order of the networks; each network's own
  tab first (its notices, errors, NickServ's and ChanServ's notices), then its channels, then
  its private chats (a new one placed there: `TabPage.index`). A tab shows its unread count
  (`badge`), activity or a mention (`attention`: a mention, or any private message, is an alert),
  and closes with its × or a middle click: a channel is parted, a private chat closed, a network's
  tab asks before disconnecting. A right click: Close, Part, Leave and forget, Logging, Notify on
  everything. A row too narrow scrolls, with a list of every tab (BMDS's tabs.md).
- **The tree** (View ▸ Conversations as a tree): the tab row hidden (`Tabs.barVisible`), a
  `TreeView` at the left (networks, and under each its conversations) choosing the page instead,
  with the same counts and marks (tabs.md §2a). Both are views of the same pages: switching costs
  nothing, and the choice is kept in `settings.ini`.
- **A conversation's page:** the topic (a `Label`), its `ChatView`, and for a channel the members
  (`ListView`, by rank then name; the away greyed) beside it in a `Splitter`. Each page keeps its
  own view, so its scroll position and marker stay when the user changes away and back.
- **Below the pages,** shared: the input (`LineEdit`: history with Up and Down, Tab completion, a
  nick button to change it) and the status bar (the connection, TLS, the login, lag).
- **Menus:** Network (Connect, Disconnect, Networks..., Register this nick..., Quit), Channel
  (Join..., Part, Topic..., Close), View (tabs or tree, members, times, joins and parts shown or
  not, font), Help. Shortcuts: Alt+1..9, Ctrl+PgUp/PgDn, Ctrl+N next unread, Ctrl+W close,
  Ctrl+K a colour code.
- **Networks...:** a dialog (`Form`): the list, and each one's fields (§4, I3), the login among
  them; a test connection.
- **Notifications:** a mention or private message in a conversation not in view: a
  `Notification` (Open, Dismiss) and the tray icon's mark; a highlight word list in
  `settings.ini`.

## 7. Protocol details

- **Registration:** `PASS` first when the login is a server password, `CAP LS 302`, the wanted
  caps requested, `AUTHENTICATE` for SASL (§7a), `CAP END`, `NICK`/`USER`; a taken nick that is
  ours taken back (§7a), any other tried with `_` appended, then the alternates.
- **Keeping it up:** our `PING` after 90 s quiet, the connection given up after 120 s more;
  reconnect after 2, 4, 8 ... 300 s with jitter, the channels rejoined, typed lines kept
  unsent. The network going down and up (`net_watch`) reconnects at once.
- **Lines:** at most 512 bytes (8191 for tags); a long message split at a word, UTF-8 whole; text
  that isn't UTF-8 read as Latin-1. Sent paced (a burst of 5, then one a second) so a paste
  doesn't flood the server off.
- **CTCP:** `ACTION` (`/me`), `VERSION` and `PING` answered, the rest ignored.
- **Casemapping** from ISUPPORT (rfc1459 by default) for every name compared.
- **Times:** `server-time` when there is one (a bouncer's playback in its own order), else ours.

### 7a. Logging in, and registering a nick

A network's login (`networks.ini`, `login=`), as mIRC's server settings offer:

| `login=` | What is sent |
|---|---|
| `sasl` (the default when there is a password) | SASL PLAIN while registering: logged in before any channel is joined, so a channel that wants a logged-in user (`+r`) and a cloak work at once |
| `sasl-external` | SASL EXTERNAL: the client certificate (`tls_ctl`'s `cert`/`key`) is the login (CertFP); no password stored |
| `nickserv` | `PRIVMSG NickServ :IDENTIFY nick password` once registered (`001`), and the autojoin waits for NickServ's answer (or 10 s), so `+r` channels still work |
| `password` | `PASS password` before registering (a bouncer such as ZNC, or a private server) |
| `none` | nothing |

- **`sasl` that the server doesn't offer, or refuses** (`904`/`905`): said in the network's tab,
  and with a stored password it falls back to `nickserv` for that connection, as mIRC does.
- **Registering a nick** (Network ▸ Register this nick...): asks for a password (twice) and an
  email address and sends `NickServ REGISTER password email`. NickServ's notices show in the
  dialog as they come. When the network mails a code (Libera, OFTC), the dialog asks for it and
  sends `NickServ VERIFY REGISTER nick code`. Done, the password is stored and the network's login
  set to `sasl`, so the next connection logs in before joining.
- **A nick that is ours but taken** (`433`, a password stored): we take a fallback nick, send
  `NickServ GHOST nick password` (Atheme's, Libera's, and OFTC's services all have it; `RECOVER`
  isn't on OFTC's), and send `NICK nick` once NickServ answers (or after 2 s).
- **`/ns` and `/cs`** send to NickServ and ChanServ (`/msg NickServ ...` works too); their
  notices go to the network's tab, not a private chat of their own. A notice asking us to
  identify, when we have a password and haven't, identifies (once a connection).
- **The password** is stored in `networks.ini` (`0600`, the app's data folder, I3), never shown
  once typed, and kept out of the logs: a line to NickServ with IDENTIFY, REGISTER, RECOVER,
  GHOST or SET PASSWORD is logged and shown with the password as `****`.

### 7b. Private chats

- **A private chat** (a query) is a conversation with one person: opened by `/msg nick text`,
  `/query nick`, a double click on a member, or a message from them. Its tab is their nick; it has
  no member list.
- **Every private message is an alert** (`attention` 2, a notification when not in view), unless
  the person is ignored (`/ignore`, kept in `settings.ini`).
- **A nick change** renames the tab and carries the conversation on; their quitting or coming back
  is shown in it (when we share a channel, or with `MONITOR` when the server has it: the
  client watches the nicks of its open private chats).
- **Notices** sent to us by a person go to the conversation in view (as most clients); a service's
  (NickServ, ChanServ, a server) to the network's tab.
- **CTCP** in a private message as in §7: `ACTION` shown as an action, `VERSION` and `PING`
  answered, the rest ignored.

## 8. Budget (estimates)

- **Program:** 250-350 KB (musl, libbmdsgui, libtls or `tls_ctl`, the core).
- **Memory:** about 60 KB a network (its TLS session 18 KB in the kernel, buffers, the members),
  scrollback 2000 lines a buffer at ~100 bytes: ~200 KB a busy channel, in PSRAM.
- **CPU:** nothing when nothing arrives (`poll`); a line's layout only when it is shown.

## 9. Testing

- **Host:** the core against scripted servers: registration with and without CAP, SASL PLAIN and
  EXTERNAL and their failures, a taken nick, ISUPPORT and casemapping, joins and parts with
  `extended-join`, `NAMES` with `multi-prefix`, modes, kicks, quits, nick changes in members and
  queries, `server-time` playback, CTCP, long lines split, the pacing; `format` both ways; a
  fuzzer for messages and formatting (§3a).
- **Board:** `tools/irc-test-server` (§3a) driven from the PC; then Libera.Chat by hand.
  Screenshots for the window.

## 10. Steps

0. **In BMDS:** `ChatView` (its `devplan/chatview.md`) and the tabs' additions (its
   `devplan/tabs.md`), before the next BMDS release.
1. **This repository's additions** (§3a): C++, the PC's C++ tests, `devplan/`.
2. **The core** (§3, §7) with its PC tests and fuzzer: needs nothing of the next release.
3. **The app:** one network, the window in tabs, private chats, TLS, the logins (§7a: SASL,
   NickServ, a server password), logs, the start's check for what it needs;
   built with `-DBMDS_SDK_DIR`, against `tools/irc-test-server`, then Libera.
4. **Several networks, Networks..., Register this nick... and taking our nick back, the tree
   view, notifications and the tray, pinning a fingerprint, reconnecting.**
5. **Release:** after BMDS's next release, the SDK pin raised, `irc` in the repository's
   packages, the README's list; a tag (`vN`) as every release here.

## 11. Decided

Approved as proposed (2026-11-16): I1, BMDS's `ChatView`; I2-I8 as in §4; §3a's additions; the
client designed for several networks from step 2 and offering them in step 4.

Added the same day (the user's): logging in as mIRC does, NickServ among the ways, and
registering a nick from the client (§7a); private chats spelled out (§7b); the conversations in
tabs by default, a tree in View instead (I7, §6), BMDS's `Tabs` gaining what that needs (its
`devplan/tabs.md`, for review). Building catgirl
against `tls.h` (BMDS's network.md §35j's open item) isn't needed for this client; it stays open
there until curses comes to the SDK.

## 12. Step 1, built

- **C++:** `project(bmds-apps C CXX ASM)`; `irc/CMakeLists.txt` builds the core for the board
  (`irc_core`, a static library: no program or package yet), so every build checks it compiles
  with the SDK's freestanding C++ headers (0.3.1's have them). `message.cpp` is 2.8 KB of code.
- **The PC's tests:** `tests/` is C and C++ (C++20), the sanitizers kept and made fatal
  (`-fno-sanitize-recover=undefined`). doctest is vendored as in BMDS (`thirdparty/doctest`,
  2.4, MIT); the core is built for the tests as for the board, without exceptions or RTTI.
  `irc_test` is a ctest of its own beside Minesweeper's `field`.
- **The fuzzer:** under clang (`CC=clang CXX=clang++`), `tests/` adds `irc_message_fuzz`
  (libFuzzer); the core's sources are built into it, so they are instrumented. The input's first
  byte chooses the chunks bytes arrive in, its second the room a split has.
- **The core's first part, `message`** (§3), what tests and fuzzer have to work on:
  - `Text`: a view of bytes (no `std::string_view` on the board): `mid`, `find`, `findLast`,
    `startsWith`, `equalsIgnoringCase`.
  - `parse()`: tags, source, command and up to 15 parameters, views into the line (the 15th
    takes the rest, as RFC 1459's); runs of spaces as one; no command is `Malformed`.
    `Message::tag()`/`hasTag()`, `numeric()`, `is()`; `unescapeTag()`; `parseSource()`.
  - `LineBuilder`: a line to send, at most 512 bytes with its CR LF (`TooLong` past it). It
    refuses a CR, LF or NUL anywhere, and in a middle parameter a space, a leading `:` or
    nothing, so typed text can't smuggle in a command. The command must be letters and digits:
    the fuzzer found that a parsed `@b` (from `@a @b :x`), built again, read back as tags.
  - `splitAt()`: a long message's first piece: at the last space past half the room, else as
    much as fits; never inside a UTF-8 character (three bytes back at most, for text that isn't
    UTF-8). §7's "split at a word".
  - `LineReader`: bytes into lines at LF (a CR before it dropped, empty lines skipped), in any
    chunks; a line past IRCv3's 8191 + 512 bytes dropped whole and counted. Its buffer
    (8.5 KB) is a member: one per connection.
- **Tested:** 14 cases, 164 checks (CR LF injection, the 512-byte edge, splits that join back,
  a byte at a time, the longest line and one byte more). The fuzzer, 5 minutes on three workers
  (5 million runs each, 289 edges): nothing after the command fix.

## 13. Step 2, first part, built (2026-11-17)

The core's containers, ISUPPORT, formatting and one network's session (§3, §7), tested on the PC;
`irc_core` built for the board (`-O2`: 39 KB, 30 KB of it the session).

- **`str.h`:** `String` and `Vector<T>` (the board has no libstdc++): move only, growth reported
  as `NoMem` rather than thrown. `Error` moved to `text.h` and gained `NoMem`, `NotFound`,
  `Invalid`. GCC's `-O2` saw a possible overflow in an inlined `append()` (it can't prove a
  `reserve()` succeeded): the growth is in `str.cpp`, with an explicit size-overflow check.
- **`isupport.h`:** casemapping (`ascii`, `rfc1459`, `strict-rfc1459`; `sameName()`), and
  `Isupport` from `005`: `CASEMAPPING`, `NETWORK`, `CHANTYPES`, `PREFIX`, `CHANMODES` (which modes
  take a parameter), `-TOKEN` back to the default, `STATUSMSG` prefixes taken off a target.
  Fixed sizes: no allocation.
- **`format.h`:** mIRC's codes stripped (for logs, notifications, mentions; `ChatView` draws them
  as they come), UTF-8 checked and Latin-1 read when a line isn't, CTCP parsed, a mention as a
  word of its own under the casemapping, `server-time` read, base64 for SASL.
- **`session.h`:** `Session` takes lines, time (`Now`: a monotonic clock for timers, the wall's
  for what is shown) and the user's actions, and gives lines (`takeLine()`) and events
  (`takeEvent()`); the platform polls by `sendWaitMs()` and `tickWaitMs()`, so nothing spins.
  - Registration: `PASS` for `login=password`; `CAP LS 302` (continued lines gathered), a `REQ`
    of the wanted caps offered (§1's, and `sasl` with a login); SASL PLAIN in 400-byte pieces
    (a `+` after an exact multiple) or EXTERNAL; `CAP END` after `903`, a refusal or a `NAK`;
    `NICK`/`USER`. A refused SASL is told (`LoginFailed`) and falls back to NickServ.
  - After `001`: our nick ghosted and taken back if a fallback was needed (§7a); `IDENTIFY` for
    `login=nickserv` (and a failed SASL), the joins waiting for NickServ's answer or 10 s; the
    settings' channels (`#chan key` too) and those of the last connection joined.
  - A taken nick while registering: the alternates, then `_` added (three times), then three
    digits.
  - Lines out: registration and `PONG` first and unpaced; the rest paced, 5 at once then one a
    second. On a lost connection the lines the user typed are kept for the next one, the rest
    dropped, and the channels remembered for joining again.
  - Keeping it up: our `PING` after 90 s quiet (the lag from its `PONG`), `Timeout` after 210 s.
  - Channels and members: our joins make a channel; `NAMES` (multi-prefix) fills it, a new one
    replacing it; `MODE` walks its modes with their parameters (`CHANMODES`) and keeps prefixes
    in rank order; nick changes, parts, quits, kicks, `away-notify`, `extended-join` (account
    and real name), `chghost`.
  - Events name their buffer: a channel, a person (`Query`: a private chat, and our own echoed
    message named for whom it was to), or `""` (the network's: server notices, numerics, a
    person's notice to us, services' notices flagged `Service`). Quits and nick changes come once
    a channel and once for a private chat with the person.
  - Messages: CTCP `ACTION` as a flag, `VERSION` and `PING` answered, the rest ignored;
    `STATUSMSG` targets (`@#chan`) to the channel; text read as UTF-8 or Latin-1; times from
    `server-time`. Our own messages shown at once without `echo-message`, by the echo with it;
    a long one split at words into lines that fit with the server's `:nick!user@host` added (a
    63-byte host assumed until the server shows ours).
  - Refused: a line break or NUL in anything typed, a target with a space or comma, nothing
    to say.
- **Tested:** 40 cases, 841 checks: containers (moves counted: nothing leaked), casemapping,
  ISUPPORT, the formatting; the session against scripted servers: registration with and without
  CAP, LS over two lines, SASL PLAIN and its pieces, SASL refused then NickServ, NickServ never
  answering, `PASS`, the nick chain and a ghosted nick retaken, NAMES and modes, nick changes,
  away, parts, quits, kicks, extended-join, messages, CTCP, STATUSMSG, server notices, Latin-1,
  sending with and without `echo-message`, a long message split, refusals, pacing with `PONG`
  first, PING and lag and the timeout, a lost connection's typed line and rejoin, ERROR, the
  reconnect delays. `irc_session_fuzz` (the server's lines, the user's, time and reconnects
  from the input): 3 minutes on three workers, about 2.4 million runs each, 1413 edges; every
  line sent one line within 512 bytes, every event's text UTF-8; nothing found.

## 14. Step 2, second part, built (2026-11-17)

Buffers, commands and Tab completion: the core is whole (60 KB for the board at `-O2`).

- **Changed from §3: buffers keep no lines.** Each conversation has a `ChatView` page of its
  own (§6), and `ChatView` keeps its lines (`maxLines`); the logs are on the card. A copy in the
  core would only cost PSRAM. A `Buffer` is what describes a conversation: its kind and name,
  unread and mention counts, its attention (a tab's, BMDS's tabs.md), whether its channel is
  joined, who spoke last (32, for Tab).
- **`Buffers`** (one a network): the session's events routed (§7b) and made into `Line`s, each
  with its view's text (`HH:MM marker`, a tab, the text, mIRC's codes kept; nicks in a colour of
  their own, ours plain (§15), a mention's marker reversed) and its log's (`HH:MM:SS marker text`,
  plain), in the zone the settings give.
  - A channel's or private chat's messages to it, a private chat opened by a message; a
    service's or the server's notice to the network's buffer, a person's to the buffer in view
    (when it is this network's).
  - Counts and attention for what isn't in view in a focused window: a message is activity; a
    mention (our nick or a highlight word, as a word of its own) or anything private an alert,
    and `notify` set on its line. `setActive()` marks a buffer read.
  - A private chat follows the person to their new nick; their quit is shown in it.
  - Joins, parts and quits shown or not (`showJoins`); kicked or parted, a channel's buffer
    stays, not joined; `disconnected()` marks every channel left.
  - A password typed to NickServ hidden in the view and the log (`Format::hideSecret`:
    `IDENTIFY [account] ****`, `REGISTER **** email`, `GHOST nick ****`, `SET PASSWORD ****`).
- **`runCommand()`:** text to the channel or person in view (`//` for text starting with `/`);
  `/me /msg /notice /query /ns /cs /join` (`#` added, `#a,#b` with keys) `/part /close /nick
  /topic` (none: shown) `/away /back /whois /names /mode` (`+o nick`: the channel in view's)
  `/kick /invite /quit /raw /quote /clear /help`; any other sent as `CMD args`. A mistake is a
  line in the buffer (`Usage: ...`, a line break refused), not an error. `Outcome` tells the
  window what else to do: show a private chat, clear, close, the network quit.
- **`Completer`:** Tab on the word before the cursor, again for the next match: commands at the
  line's start, channels, nicks (in a channel those who spoke last first, if still there, then
  the rest by name; never ours); `: ` after a nick starting the line.
- **Fixed on the way:** `PART` without a reason was sent as `PART :#chan`.
- **Tested:** 50 cases, 1327 checks (buffers' routing, lines, counts and hiding; every command
  and its mistakes; Tab's orders and cycling). `irc_session_fuzz` now types commands in its
  buffers, presses Tab and passes every event through the buffers: 3 minutes on three workers,
  about 1.9 million runs each, 1757 edges, nothing found.

## 15. Step 3, the app, built (2026-11-17)

One network's window, over TLS, with the logins, logs and the start's check; tried on the board
against `tools/irc-test-server`. 199 KB of code (`-Os`; the core 60 KB of it at `-O2`, now `-Os`
too), inside §8's estimate.

- **In BMDS first: the input line's keys** (its chatview.md §10). Tab moved the focus and a
  `LineEdit`'s cursor couldn't be read, so Tab completion and the history (§6) had no way in.
  `LineEdit` gained `cursor` and `keys` (Tab, Up, Down, PageUp, PageDown taken by the program,
  told as `keyPressed`), in the same release as `ChatView`.
- **`core/settings`:** `networks.ini` (a section a network: host, port, tls, nick, altnicks, user,
  realname, login, account, password, cert, key, ca, channels, autoconnect) and `settings.ini`
  (`[view] joins`, `[highlights] words`, `[logs] enabled, folder`); each mistake told with its
  file and line in the network's tab; a network without a host or nick left out. A first start
  writes `networks.ini` with Libera.Chat and OFTC waiting for a nick (I6; the Networks dialog is
  step 4). `ca` (a PEM file of roots, added to the system's) is new: a network with a CA of its
  own (the test server's, a private one) without turning the check off (I5).
- **`bmds/connection`:** the name looked up on a thread of its own (getaddrinfo blocks; the
  thread owns its pipe's write end, so a connection given up meanwhile costs nothing), each
  address tried with a non-blocking connect (30 s), then libtls over the socket (the handshake
  the kernel's, `TLS_WANT_POLLOUT` while it runs); everything waits in the one `poll()` with the
  GUI's fd. SIGPIPE is ignored.
- **`bmds/logs`:** `logs/NETWORK/BUFFER/YYYY-MM-DD.log` in the data folder, the folder names lower
  case and safe for FAT. Found on the board: BMDS's FAT locks an open file, so logs kept open
  couldn't be read while IRC ran. Lines now wait in memory and are appended (open, write, close)
  every 5 s and at the end.
- **`bmds/app`:** the window (§6) without the tree, the tray and notifications (step 4): menus
  (Network: Connect, Disconnect, Quit; Channel: Join..., Part, Close; View: joins and parts; Help:
  About), a tab a buffer (a channel's with its topic, and its members in a splitter, by rank then
  name, the away marked), the tabs' badges (unread) and attention, close buttons (a channel
  parted first), the nick button, the input line (Enter: `runCommand`; Tab: the completer, with
  the line's cursor; Up and Down: the last 100 lines), the status bar (the state, TLS's version,
  the account, the lag; the cipher in the network's tab). A link clicked: an `irc://` one joins
  its channel, any other is copied (BMDS has no browser yet) and said. A lost connection is
  said with when it tries again (§7's backoff); a timeout drops it. Closing the window sends
  `QUIT` and gives it a second to go. The start checks for `tls_ctl`, `ChatView`,
  `TabPage.closable` and `LineEdit.keys`, and says the firmware needs updating if one is missing.
  The data folder is `bmds_app_data_dir()`'s, or `$HOME/.local/share/irc` run from a shell.
- **`tools/irc-test-server`:** Python's standard library and `openssl`; TLS (its own CA, `ca.pem`
  written for the client) and plain; CAP 302 with §1's caps, SASL PLAIN, NickServ (IDENTIFY,
  GHOST), channels, NAMES, modes, away, server-time, echo-message; alice chatting in #bmds
  (styles, a link, a /me, mentions), bob joining and leaving; TestServ (`KILL`, `SILENT n`,
  `FLOOD n`, `QUERY`). Its keys in a folder under ~/.cache, removed when it ends (SIGTERM too).
- **On the board** (the PC's server, `ca` and SASL in `networks.ini`, the data folder under a
  `HOME` of the test's): TLS 1.2 up, logged in by SASL, #bmds joined with its topic and members;
  alice's styles, colours, link and /me; a mention reversed; a line typed; `al` + Tab gave
  `alice: `; `/me`; the member list following bob; the server's `KILL`: "connecting again in 2
  s", connected, logged in and #bmds joined again by itself; alice's private message opened a
  tab with an alert; `/ns IDENTIFY` answered (the password hidden); `/close` closed the private
  chat; the channel's close button parted it; closing the window sent `QUIT`; the logs readable
  while running and after.
- **Fixed on the board:** our own nick bold left gaps (a run in another font starts on a 4-pixel
  column): plain now. A service's notice naming us ("identified for jth") counted as a mention:
  services and the server don't mention. The status bar was too narrow for the cipher suite:
  the version only there. A dropped connection left the member lists as they were: emptied.
- **Libera.Chat by hand** (the user's go-ahead; a test nick, `bmdstest974`, no login, only a
  channel of our own, `#bmds-test`): TLS 1.2 with Libera's certificate checked against the
  system's roots, registered, the MOTD (its links clickable), user modes, #bmds-test joined
  (ours: `@`, the server's `+Cnst`), a line sent, the topic set and shown above the view,
  `/whois` answered in the network's tab ("using a secure connection [TLSv1.2,
  ECDHE-RSA-CHACHA20-POLY1305]"), `QUIT` on closing. Seen there: the server's notices
  (`-tungsten.libera.chat-`) make the network tab's gutter as wide as the server's name, as
  `ChatView` lines gutters up at the widest; a server's notice is better marked `--` (step 4).
  An empty topic leaves an empty bar above the view.
- **Not yet:** step 4's Networks dialog, nick registration, several networks shown, the tree
  view, notifications and the tray, pinning a fingerprint, and the marker ("new since") on going
  back to a tab.
