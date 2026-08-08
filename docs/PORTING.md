# Porting to another board

The firmware is split so that everything board-specific lives in one
header. `firmware/sdk/` and `firmware/port/` contain no board constants at
all - they pull them from `firmware/boards/<board>/board.h`.

```sh
cd firmware
make BOARD=pacman
```

## Two boards exist, and the second one is the useful example

`boards/action104/` and `boards/pacman/` are both complete. The Pac-Man
one is worth reading before starting a third, because its stock firmware
shares nothing with the other board's - it is a licensed Bandai Namco
arcade emulator written in C++, where the 104-games board runs a
FlyThings/ZKSWE NES emulator. Everything still came out of it, and the
methods below are what worked.

The boot ROM was byte-identical between the two boards, so the SoC and the
flashing tools needed no changes at all. Check that first on a new board:
dump it and compare.

## What a new board needs

All of it comes out of that board's own stock firmware, so dump it first
(see [FLASHING.md](FLASHING.md)) and keep the dump:

```sh
python tools/flash.py read newboard_stock.bin 0x08000000 0x400000
```

Then open it in a disassembler as **ARM little-endian, Cortex-M33, load
base `0x08000000`**.

### 1. The panel init

**Count writes to the SPI data register `0x40030000` per function.** On
the Pac-Man one function had 82 of them and everything else had at most
ten - that was the init, sitting there as unrolled code. Searching for a
byte table, for MADCTL/COLMOD near each other, and for dense `MOVS`+`BL`
regions all found nothing first; this found it immediately.

Then walk the instructions and rebuild the byte stream: a `MOVS Rn,#imm`
followed by a store to the data register is one transferred byte, and
writes to GPIO BSRR toggle the DC line between commands and parameters.

A neighbouring function is worth finding too - the one that sends `0x2A`.
That is the window routine, and it tells you DC, CS and the SPI register
offsets without any guessing.

**PA8 may be a panel reset rather than a backlight.** It is the backlight
on the 104-games board and the reset on the Pac-Man, and getting that
wrong produces a failure that hides itself: everything looks fine until
the first real power cycle, because flashing does not cut power and the
stock firmware's panel setup survives it. Then the panel comes up
undefined and shows a white raster. Look for a lone BSRR write with an
unfamiliar bit near the start of the init, followed by a delay.

**Test from a cold start, not just after flashing.** Cut power for a few
seconds. That is the only test that proves your init does anything.

Two more traps documented in [HARDWARE.md](HARDWARE.md#display): starting
the extraction at a round address can cut off MADCTL, and CS has to stay
low across a command and its parameters.

### 1b. If MADCTL does nothing

It happens - the Pac-Man panel ignores it entirely. Four values, including
one with BGR cleared that must change the colours if it lands, moved
nothing. `BOARD_LCD_ROT90` in `boards/pacman/board.h` turns the frame in
software instead, on the way to the panel.

### 2. LCD register values

Easier read live than derived. Halt the stock firmware once it has drawn
something and dump GPIO MODER/AFRL/PUPDR plus the SPI controller
registers, then put those values into `BOARD_LCD_*`.

### 3. Buttons

The fastest route needs no disassembly and no flashing at all: **poll the
GPIO input registers over SWD while the stock firmware runs** and press
each button in turn. Reads are harmless on this SoC - it is writes to GPIO
registers that wedge the debug bus - so the board can stay untouched. That
is how the Pac-Man's eight buttons were mapped, eight for eight, in one
pass.

Both boards turned out to use the same pins for the d-pad, A and B, so try
the existing tables before assuming anything.

If you do want it from the firmware: find the key scan by its debug string
- on the 104-games board it sits right next to `get key[%02x]`, with a
dense run of GPIO IDR reads. Each test is `LSLS Rx,#n` plus `PL`/`EQ`,
meaning bit `31-n`, active low.

Then confirm on the device rather than trusting the disassembly. The
firmware has a scan mode for this: `input_scan_init()` mirrors all three
GPIO input registers into the host mailbox at `0x20000000`, so a host
script only has to poll six words there with pylink and watch which bit
moves while you press each button. Three things that mattered when doing
this the first time:

* **Mask PA13/PA14/PA15.** Those are SWDIO/SWCLK/JTDI and they toggle from
  your own read traffic. A first run masked only PA14/PA15, so every step
  instantly "detected" PA13, skipped ahead, and buried every real press.
* **Require several consecutive identical reads** before accepting a
  press, against interference and bounce.
* **Do not reconfigure pins** to hunt for buttons. Setting every unused
  pin to input-with-pull-up crashed the console at startup - something on
  one of those pins is needed as an output. And writing GPIO registers
  from the host over SWD wedges the debug bus outright.

### 4. Memory

Check the SRAM size before trusting the linker script. On the 104-games
board it is 280 KB, and reading past the end resets the chip - while the
same range reads back `0xAA` happily over SWD. Do not take the SWD read as
proof that memory is there.

## What you should not need to touch

* `firmware/sdk/soc.h` - same SoC, same register map
* `firmware/sdk/clock.c` - same clock tree; only `CPU_HZ_BASE` may differ
  if the board runs a different reference
* `firmware/sdk/lcd.c`, `firmware/sdk/input.c` - driven entirely by
  `board.h`
* `firmware/port/` - the geometry constants derive from `LCD_WIDTH` and
  `LCD_HEIGHT`

If a smaller panel makes the 240x160 DOOM image not fit, that is the one
place in `port/i_system_console.c` that needs attention - `ORIGIN_X` and
`ORIGIN_Y` go negative and the image needs cropping instead of centring.

## Contributing a board

A board is done when `make BOARD=<name>` produces an image that boots,
shows the title screen with correct colours, and responds to every button.
Please include the stock dump under `dumps/<board>/` - without it nobody
can check or extend your work, which is the whole reason this repository
has that folder.
