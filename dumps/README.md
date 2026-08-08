# Dumps

One folder per board. Everything here was read out over SWD with
`tools/flash.py` and is unmodified.

```
python tools/flash.py read extflash_0x08000000_4MB.bin 0x08000000 0x400000
```

## action104/

| File | What it is |
|---|---|
| `bootrom_0x00000000_32K.bin` | On-chip boot ROM, 32 KB. Mirrored every 32 KB across the low address space. Sets up the flash controller and jumps to `0x08004000`. |
| `extflash_0x08000000_4MB.bin` | Full contents of the external SPI flash as shipped, i.e. the stock firmware. |

**Make this dump before you flash anything.** It is the only way back to
the original console, and it is where the register map, the panel init
sequence and the button matrix in this repo came from. Written back to
`0x08000000` it restores the device completely.

## pacman/

Same two files, same procedure. Two things worth knowing about them:

* `bootrom_0x00000000_32K.bin` is **byte-identical** to the 104-games
  one. Same mask ROM, same SoC - which is why the flashing tools work on
  both boards without a change.
* `extflash_0x08000000_4MB.bin` is a completely different application: a
  licensed Bandai Namco arcade emulator ("Featuring Moo Emulation", build
  V.289.1 Aug 12 2025) written in C++. None of the FlyThings/ZKSWE
  strings from the other board appear in it, so nothing could be lifted
  from its disassembly.

## A note on redistribution

`extflash_*.bin` contains the vendor's application and the game ROMs it
ships with, none of which are ours to license. It is included because
without it none of the reverse engineering in this repo can be checked or
carried on. If that is a problem for you, delete the folder - nothing in
the build depends on it.
