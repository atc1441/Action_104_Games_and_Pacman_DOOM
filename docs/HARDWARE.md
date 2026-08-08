# Hardware notes

Two consoles, one SoC: the **104 Games** handheld and the **PAC-MAN**
handheld, both from Action. Their boot ROMs are byte-identical, so
everything down to the flashing tools is shared; their stock firmwares have
no code in common at all, so the display and button sections below are
given per console.

What is here was reconstructed from those stock firmwares and verified on
the devices. There is no datasheet. Where something is inferred rather than
proven it says so - `firmware/sdk/soc.h` marks every register `[V]`
(verified) or `[A]` (assumed).

## The SoC

Unmarked QFN48.

| | |
|---|---|
| Core | STAR-MC1 (Arm China), ARMv8-M Mainline, Cortex-M33 compatible |
| CPUID | `0x631F1320`, implementer `0x63` |
| Cache | 8 KB I-cache + 8 KB D-cache |
| FPU | present, enabled by the stock SystemInit |
| TrustZone | not implemented |
| Build flags | `-mcpu=cortex-m33 -mthumb -mfloat-abi=soft` |

The manufacturer was never identified. CoreSight only names Arm China as
the core supplier and no vendor ID register was found - do not spend time
trying to derive it from the CoreSight IDs, that road ends.

The better lead is the stock firmware itself, which names its SDK:
`Power by FlythingLite[%x]`, `FlyThings BuildTime %s`, and
`unsupport platform,please contact www.zkswe.com`. The application on top
is an NES emulator (`checkNESMagic: skipped rom with invalid mapper #%d`).
FlyThings/ZKSWE documentation would give the register map directly.

## Memory map

| Range | Contents |
|---|---|
| `0x00000000` | Boot ROM, 32 KB, mirrored every 32 KB |
| `0x08000000` | External SPI flash, 4 MB, memory-mapped (XIP). Alias at `0x0C000000`. |
| `0x08004000` | Where the bootloader jumps |
| `0x20000000`-`0x20045FFF` | SRAM, **280 KB** |
| `0x40000000`, `0x48000000`, `0x50000000`, `0x52000000` | peripherals |
| `0x60000000`-`0x60001FFF` | security / crypto block, holds the reference clock value |

SRAM size was measured, not read off a spec: a core read at `0x20046000`
triggers a system reset. Over SWD that same range reads back `0xAA`
perfectly happily - that is just how the bus answers for absent memory, not
a fill pattern. An early version of this document claimed 384 KB on the
strength of those `0xAA` reads.

## Peripherals

Base addresses look exactly like an STM32L4. **The register layouts do
not.**

| Peripheral | Base |
|---|---|
| RCC | `0x40021000` (clock enables at `+0x28`, `+0x2C`, `+0x38`) |
| GPIOA / GPIOB / GPIOC | `0x48000000` / `0x48000400` / `0x48000800` |
| SPI (LCD) | `0x40030000` |
| MPI / SPI (flash) | `0x52005000` - do not touch, XIP runs through it |
| USART1 / USART2 | `0x40013800` / `0x40004400` |

### GPIO

Proven deviations from the STM32 layout:

| Offset | Register |
|---|---|
| `+0x00` | MODER, 2 bits per pin |
| `+0x08` | **PUPDR**, 2 bits per pin, `01` = pull-up (not OSPEEDR) |
| `+0x0C` | **IDR** (not `+0x10`) |
| `+0x14` | **BSRR**, set in low 16, reset in high 16 (not `+0x18`) |
| `+0x20` / `+0x24` | AFRL / AFRH |

The PUPDR offset in particular cost real time: with OSPEEDR assumed there,
input init never enabled a pull-up, every input floated, and pressing a
button changed nothing at all.

### SPI

Both instances share the same IP block.

| Offset | Register |
|---|---|
| `+0x00` | data |
| `+0x08` | mode |
| `+0x0C` | TX control (bit 0 enable, bit 1 reset) |
| `+0x10` | RX control (bit 0 enable, bit 8 required) |
| `+0x18` | status (bit 3 TX full, bit 4 RX empty, bit 14 TX done) |
| `+0x20` | length |
| `+0x24` | start |
| `+0x2C` | XIP command register: `0x00040000` = manual, `0x05441849` = XIP |
| `+0x30` | XIP read command, `0xFAEB` |

`+0x2C` is the actual switch between "controller fetches on its own" and
"we push bytes through the FIFO". Bits 5:6 of `+0x08` are not, despite
looking like they should be - they are 0 in both modes.

## Clock tree

From the stock `SystemCoreClock` routine at `0x08005588`:

```
source      RCC +0x1C bits[2:0]   0/1 reference, 2 = 12 MHz, 3 = PLL
PLL         F = Fref * (M + 12) / (P + 1) / (Q + 1)
              M = +0x4C bits[5:0], P = bits[10:8], Q = bits[15:12]
post-divide +0x20 bits[3:0] and [7:4], each +1
Fref        (0x600000B0 >> 2), valid when (0x60000080 & 0xFE00) == 0 and
            the upper half of 0x60000080 is the one's complement of the
            lower half; otherwise 16 MHz
```

On this device Fref is 15,525,065 Hz and `+0x4C` is `0x326` (M=38, P=3,
Q=0), giving 194.06 MHz - which is what the stock firmware runs at.

The bootloader hands over at 16 MHz. `clock_init()` in
`firmware/sdk/clock.c` replicates the stock SystemInit and reaches ~62 MHz.

### Why 194 MHz does not work here yet

The full PLL sequence is in `clock_boost()` and is correct - it was read
straight out of `app_board_init` at `0x0802AD96`. It still hangs the SoC,
and the reason has nothing to do with the register order:

**This firmware executes via XIP from the external SPI flash.** When the
core clock jumps, the flash interface timing no longer holds and the very
next instruction fetch fails. The signature is distinctive: current draw
goes from 61 mA to 124 mA, the display stays dark, the debug port is gone.
The PLL is running perfectly - the core just cannot read another
instruction.

The stock firmware handles this the only way that works. Before touching
the flash controller it copies a small routine into RAM at `0x0802AA70`
(the stored constants `0xF8413A01` / `0xD1F90F04` are Thumb code:
`SUBS r2,#1` / `STR.W r0,[r1,#4]!` / `BNE`), checks `0x52005004` and
`0x52005010`, masks interrupts, and calls it there.

To finish this: move `clock_boost()` into a `.ramfunc` section, copy it to
RAM at startup, call it with interrupts masked, and bring the MPI divider
along with the core clock.

## Display

Both consoles use a 320x240 RGB565 panel on the same SPI controller with
the same configuration (mode `0x2881`, RX control `0x100`) and the same
pins: PA1 = DC, PA2 = CS, PA3 = SCK, PA4 = MOSI. Both controllers answer a
Galaxycore-style command set (GC93xx / NV30xx family); neither exact part
was identified, and it does not matter, because the init sequences are
simply what the stock firmwares send.

Everything else differs, including what PA8 does.

### 104 Games

75-byte init sequence, in `firmware/boards/action104/board.h`. PA8 is the
backlight. The panel is used in landscape.

Two things that will bite anyone rebuilding this driver:

* **The sequence starts at `0x0802FE06`, not `0x08030000`.** Starting at
  the round number cuts off `FE`, `EF`, `36 E8` - and `0x36` is MADCTL, so
  the panel comes up rotated and in RGB instead of BGR.
* **CS must stay low across a command and its parameters.** Pulsing CS per
  byte means no parameter ever arrives. This is easy to miss, because the
  stock firmware has already configured the panel before you flash over it:
  the picture looks right while your own init does nothing. Only after a
  power cycle does the truth show up.

### PAC-MAN

78-byte init sequence, in `firmware/boards/pacman/board.h`. Two differences
from the other console that matter a great deal:

* **PA8 is the panel RESET here, not the backlight.** Without a low pulse
  before the init, a cold start leaves the controller undefined and it
  shows a white raster. The stock firmware pulses it at `0x08016B98`.
* **It ignores MADCTL.** Four values were tried from a clean cold start
  with the full init in place, including one with BGR cleared, and none of
  them changed anything. Rotation is done in software instead
  (`BOARD_LCD_ROT90`).

Its init is the same controller family with different parameters - MADCTL
`0x28`, an extra `0x21` for display inversion, its own timing, power and
gamma values. It was recovered statically from `sub_8016A84` by counting
writes to the SPI data register per function: 82 there against at most ten
anywhere else.

## Buttons

Both consoles use the same pins for the d-pad, A and B - the Pac-Man simply
populates fewer of them. Each map was read out of the respective stock
firmware and then confirmed on the device by pressing every button while
watching the input registers.

### 104 Games

The stock key scan is `app_input_poll` at `0x0803F080`; the dense run of
GPIO IDR reads starts at `0x08041842`. It builds a bit mask, stores it at
`0x20030329`, and prints `get key[%02x]` next to it. Each test is
`LSLS Rx,#n` followed by `PL` or `EQ`, i.e. bit `31-n`, active low.

The bit order is an NES joypad byte, which fits an NES emulator exactly:

| Bit | Value | Pin | Button on the case |
|---|---|---|---|
| 0 | 0x01 | PB0 | right button 4 (bottom) |
| 1 | 0x02 | PA11 | right button 3 |
| 2 | 0x04 | PB3 | **not populated** |
| 3 | 0x08 | PC8 | START |
| 4 | 0x10 | PB7 | d-pad up |
| 5 | 0x20 | PB5 | d-pad down |
| 6 | 0x40 | PB6 | d-pad left |
| 7 | 0x80 | PB4 | d-pad right |

Plus two pins the stock firmware ORs onto existing bits behind a counter -
the turbo buttons:

| Pin | Button | In the stock firmware |
|---|---|---|
| PA12 | right button 1 (top) | same bit as PA11 |
| PC6 | right button 2 | same bit as PB0 |

Handled separately:

* **PC7 = MENU.** In `app_input_poll` this pin shuts peripherals and the
  LCD SPI down, so it is closer to a power button than a game button.
* **PA0, PB2, PC13** are the three-position volume slider, read by a
  RAM-resident routine at `0x20036000` that prints `AudioVolume:%d`.

All of this was then confirmed on the device by pressing each button and
watching the mirrored input registers: ten distinct pins, all from the set
above, with the d-pad landing exactly on the direction bits.

### PAC-MAN

Eight buttons, mapped without any disassembly at all: poll the GPIO input
registers over SWD while the stock firmware runs and press each button in
turn. Reads are harmless on this SoC - it is writes that wedge the debug
bus - so the board never had to be touched. Eight buttons, eight pins, one
pass.

| Button | Pin | | Button | Pin |
|---|---|---|---|---|
| D-pad up | PB7 | | A | PB0 |
| D-pad down | PB5 | | B | PA11 |
| D-pad left | PB6 | | VOL+ -> START | PA0 |
| D-pad right | PB4 | | VOL- -> SELECT | PB2 |

The volume buttons become START and SELECT: DOOM needs those, and there is
nothing else left. No strafe key on this console.

## Audio

Not implemented yet, but fully mapped - the block below was read off the
running stock firmware over SWD, the same way the LCD registers were.

A PCM/DAC block at **`0x40012C00`**, fed by DMA:

| Register | Observed | Role |
|---|---|---|
| `+0x00` | `0x00000081` | bit 0 set on start |
| `+0x0C` | `0x00000200` | |
| `+0x10` | `0x00000003` | |
| `+0x14` | | written during start |
| `+0x18` | `0x00000068` | likely a divider |
| `+0x20` | `0x00000001` | bit 0 = enable, last thing set |
| `+0x24` | `0x00000089` | |
| `+0x28` | `0x00000010` | |
| `+0x2C` | `0x000000FF` | |
| `+0x34` | | data register - DMA writes here |
| `+0x44` | `0x00008000` | bit 15 set on start |

`RCC+0x38` bit 6 is toggled off and on around the setup - that is the
block's reset.

### DMA

Channels are `0x40` apart from `0x40031100`. Two are in use:

| Channel | Destination |
|---|---|
| 0 | `0x40012C34` - the audio data register |
| 1 | `0x40030000` - the LCD SPI data register |

Channel 0 live: source advancing through the sample buffer, `+0x0C` =
`0x854002CA` (the low half counts down, so it is the remaining transfer
count), `+0x10` = `0x000A0083` with bit 0 as the channel enable. Global
`+0x14` bit 0 signals "channel done" and `+0x08` acknowledges it.

Channel 1 is worth remembering independently of audio: the display can be
driven by DMA rather than the byte-pushing loop in `lcd.c`, which is where
the frame rate currently goes.

### How the stock firmware plays sound

Two ping-pong buffers at `0x20044000` and `0x200445CA` hold 16-bit
samples. DMA channel 0 streams one while the firmware refills the other;
on the done flag it acknowledges, swaps the pointers (kept at
`0x20044B94`/`0x20044B98`) and refills. Sample values are 8-bit source
data divided by a volume divisor from a table at `0x080341D0`, indexed by
the volume variable - so **volume is done in software**, which is why the
three-position slider is just three GPIO pins.

Output is 16-bit, so this is a real DAC path, not one-bit PWM.

### Bringing it up: three things that were not in the code

Replaying the observed DAC registers was not enough. Three separate
enables had to be found by diffing the whole peripheral state against the
stock firmware while it was playing:

* **`RCC+0x28` bits 0-1 and `RCC+0x38` bit 9.** Without them the DMA
  controller has no clock, and its channel registers read back as zero no
  matter how often you write them.
* **`0x40031030` = 1**, the DMA controller's master enable. This one is
  nasty: with it clear, the channel registers accept writes, read back
  correctly, and report the channel as enabled - and nothing moves.

The output rate is **~14 kHz**, measured by counting completed DMA blocks
(741 samples each) over 12 seconds. Nothing in the register map states it;
`+0x18` = 0x68 remains the likely divider but that is unconfirmed.

### Where it stands

`firmware/sdk/audio.c` and `firmware/port/i_sound_console.c` hold a
working bring-up and a software mixer. They are **not built** - see the
note in the Makefile - because they never got as far as usable sound.
What they contain is real though, and it is the starting point for
anyone finishing this.

What is proven to work:

* the DAC configures and the DMA channel arms exactly as in the stock
  firmware, down to `CFG` reading back `0x000A0083`
* sound does come out - one build produced a loud tone through the
  speaker, so the whole path from RAM to speaker is real
* the mixer receives sounds correctly: a menu blip arrived with the right
  length and a resampling step of `0xC95E`, which is exactly
  11025/14016

The wall is the reload mechanism. The channel plays one buffer of 741
samples, loads the next source address from the word its `+0x08` points
at, and then stops with the transfer count at zero - the count is not
reloaded with it. So the output is one buffer and then silence, or one
buffer repeating, which is what the tone was.

Two things were tried and did not work: re-arming `+0x08` from software
on every buffer switch, and treating `+0x08` as the head of a
scatter-gather chain whose fields mirror the channel registers
(`SRC/DST/NEXT/CTRL`). The second one transferred a couple of hundred
samples and stalled, so that field layout is wrong.

**The next step is to read the stock descriptor rather than guess it.**
The stock firmware's `+0x08` pointed at `0x20044BA0`; dump 32 bytes from
there while it is playing and the layout is settled. That needs one more
stock-firmware restore cycle - the same procedure that produced
everything else in this section.

Two smaller findings worth keeping:

* GPIOA `+0x1C` and `+0x28` have to be set (the stock has `0x20000114`
  and `0x00009F3F`). Without them the channel arms and then sits there,
  because the DAC never raises a request. Writing them by assignment
  rather than OR takes the debug port down - PA13/PA14 are SWD on the
  same port.
* The engine side is done: GBADoom's `i_audio.c` is GBA/Maxmod only, so
  `i_sound_console.c` replaces it with an eight-channel mixer reading the
  `DS*` lumps straight out of XIP flash.

### A trap in the WAD

The WAD shipped here has **no sound lumps at all**. `GbaWadUtil` strips
them, because GBADoom plays sound from a Maxmod soundbank instead. The
original shareware IWAD is in the GBADoom tree at
`GbaWadUtil/doom1.wad` and does have all 55 `DS*` lumps, 523 KB of them.
Merging those back in is a few lines; the only catch is that the result
is 3.23 MB and no longer fits below the WAD base, so the base has to move
down to about `0x08090000`.

## Two ways to lose the debug port

Both look identical from outside - VTref fine, current normal, `Failed to
attach to CPU` - and both need a power cycle.

1. **Writing GPIO registers over SWD** wedges the debug bus, whether the
   core is running or halted. Reading SRAM and peripherals is fine. So pin
   configuration belongs in the firmware; the host reads SRAM only. The
   scan mode in `firmware/sdk/input.c` exists for exactly this reason.
2. **`wfi` in an endless loop** (fault handlers, `I_Error`) powers the
   debug domain down and makes a crash impossible to investigate. Use
   `for(;;){}`.
