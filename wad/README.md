# WAD

`doom1_e1m1.wad` is the DOOM shareware IWAD with every map except E1M1
removed - 2.6 MB instead of 3.8 MB. All shared resources (palettes,
textures, sprites, graphics) are untouched; only the map lumps of the
other levels are gone.

Why: the flash is 4 MB, the firmware takes about 525 KB of it, and the
bootloader wants the first 16 KB. The eight extra maps E1M2..E1M9 alone
account for 1.15 MB.

Flash it to `0x08110000`:

```sh
python tools/flash.py write wad/doom1_e1m1.wad 0x08110000
```

The firmware reads it by pointer straight out of the memory-mapped flash,
so nothing is copied into RAM.

## Making your own

The trimming was done with a small script that keeps a map header plus the
lumps directly following it (`THINGS`, `LINEDEFS`, ... `BLOCKMAP`) and
drops those groups for unwanted maps. That script lived in the research
tree and is not part of this repository - the result is shipped instead.

If you replace this with a full WAD, revert the two `d_main.c` patches in
`firmware/GBADoom/engine-patches.diff`. They exist only because a
single-map WAD breaks two assumptions in the engine:

* the attract loop plays demos recorded in maps that are no longer there
  (`W_GetNumForName: E1M5 not found`)
* the game mode is inferred from how many maps exist, and with one map no
  threshold matches, so the four-episode menu appears and aborts with
  `M_EPI4 not found`

## Licence

The DOOM shareware IWAD is freely redistributable under id Software's
shareware terms - that is the whole point of the shareware release. It is
not GPL; the GPL applies to the engine source, not to the game data.

Retail WADs are not redistributable. If you want DOOM II or Ultimate DOOM
on the console, use your own copy - and note that a full IWAD will not fit
in the remaining flash without trimming.
