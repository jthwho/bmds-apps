# The I/O Manager (`ioman`)

Status: **steps 1 and 2 built** (2026-11-20), tried on bmds0. The window of BMDS's I/O plan
(BMDS's `devplan/io.md` §3.10), moved here at the user's word ("We can make the ioman app live
in the bmds-apps repo") after a first version built into the firmware. It needs a BMDS newer
than 0.4.0: `gpio_ctl` (slot 139) and `/dev/gpiochip0` came with that plan's step 2.

## 1. What it is

One window that shows every pin of the board's chip:

- **Pins:** a table of every pad: the GPIO with a swatch of its class's colour (free grey, user
  green, system blue, fixed red) and its level inside (white high, black low), its class, who
  holds it and for what (`eth0: RMII TXD1`, or the board's label for a pad nobody holds), what
  the IO_MUX and the GPIO matrix route through it, its direction and pull, and the connector pin
  it is brought out to. Show: all pins, one class, or one connector's. Sortable by any column.
  Beside it, behind a splitter, the chosen pin's every detail (`io pin N`'s lines) in a read-only
  text editor, so they can be copied.
- **Board:** each connector of the board's as its header is laid out (`columns` pins across),
  every pin a button with its swatch, number, GPIO and holder (supplies greyed out), then every
  pad of the chip eleven to a row, then a legend. A click opens the pin on Pins.
- A status bar: the classes' counts; the chip and the board.

It only reads. Taking and driving pins, muxing and ports come with BMDS's step 3 and later.

## 2. How

- **The kernel:** `/dev/gpiochip0`, `gpio_ctl`: `BMDS_GPIO_CHIP_INFO`, `BMDS_GPIO_PINS` (every
  pad in one call: class, level, flags and the words BMDS's `io` prints), `BMDS_GPIO_PIN_DETAIL`,
  `BMDS_GPIO_CONNECTOR` and `BMDS_GPIO_CONNECTOR_PIN`, read once at start. An older BMDS has no
  device or no slot: the window says so in its status bar and shows nothing else.
- **Live levels:** a GUI `Timer` at 100 ms; each tick reads every pad unless the window is
  minimized (nothing is read for a window nobody sees), and only the tab shown is updated.
- **Only what changed is sent:** a pad's words (owner, use, route, direction, connector pin) are
  hashed apart from its level (`pinHash()`, `swatchOf()`). While the table shows the same pads in
  the same order, a row whose words moved is replaced in place (`DetailsView.setRow`, added to
  BMDS's GUI classes for this, gui.md §4x) and a row whose level moved gets its icon
  (`ItemView.setIconImage`); neither touches the table's scroll or selection. Other pads or
  another order (a filter, a sort) clear and fill it again, the chosen row kept. The first version
  rebuilt the table whenever any level moved, ten times a second, and the user found it wouldn't
  stay scrolled and drew all the time. Board buttons keep their words and swatch apart the same
  way; the status bar and the inspector are sent only when their text changes.
- **A peripheral's pad shows no level:** a running clock or data line (I²S, RMII, I²C) read ten
  times a second is a random sample; its swatch is the class's colour alone, and the inspector
  for such a pad is read once a second. Measured on bmds0 with the window idle: two window
  manager rounds a second, GPIO26's PPS going up and down.
- **The SDK's limits it fits:** a `DetailsView` has at most six columns, so the level and class
  are the row's icon (twelve swatches made once with `PutImage`) and direction and pull share a
  column; a `Label` keeps 63 bytes, so the details are a read-only `TextEdit`, which doesn't
  wrap: the program breaks each line at 43 characters, at a space, its continuation under the text.
- **One at a time:** the service `ioman`; a second start sends "raise" and ends.
- **The core** (`ioman/core/pins.*`, portable C++ tested on the PC in `tests/ioman`): `Pin` (what
  the program copies out of `bmds_gpio_pin_t`), `PinList` (which pads show, their order, the
  counts), the columns' words, the swatch index, the hash.

## 3. Step 2: setting pins up (BMDS's io.md §10.7, 2026-11-20)

- **The editor**, above the details: the chosen pin's entry in BMDS's `io/gpio` as fields: Mode
  (Not set up, Input, Output, Open drain, Off), Level, Pull, Edges (Not watched, Both, Rising,
  Falling), Debounce (µs), and Other words for the rest (`func=`, `out:`, `in:`, `drive=`, kept
  as written); Apply, Release (the entry taken out), Save (`io/gpio` saved: so at boot). Fields a
  mode doesn't use are greyed. A BMDS without `io/gpio` leaves the editor off.
- **Writing:** the whole value through `sysconf_set`, the chosen pin's entry replaced, added or
  taken out and every other kept as written (`core/setup.*`: `findEntry()`, `entryText()`,
  `withEntry()`, tested on the PC). A pin's `force` stays while its entry is edited.
- **A held pin:** the set is refused (`EBUSY`); the window reads the kernel's reason
  (`BMDS_SYSCONF_FIELD_REFUSAL`) and asks in a box, "...Change GPIO21 anyway?"; Yes sets it again
  with `force`. Other refusals go to the status bar in the kernel's words, held 4 s.
- **Edges:** a pin `io/gpio` watches (`BMDS_GPIO_PIN_EDGES`) has its edges read each refresh
  (`BMDS_GPIO_EDGES`, after the last number seen) into a log of 32, shown at the top of the
  details: its last 2 s as a strip (`▔` high, `▁` low, 32 columns of 62.5 ms, `levelStrip()`;
  wider ran past the pane, as the text editor doesn't wrap) and its last six edges' times.
- **Checked on bmds0:** GPIO2 set up as an output; GPIO21 refused and asked, answered No;
  GPIO14 as open drain with edges, three pulses from `io pulse` listed and drawn.

## 4. Next

- With step 4: a Ports tab (UART, I²C, SPI on chosen pins) with a tool for each.
