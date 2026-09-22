# WAD

`doom1_e1m1_sfx.wad` is the DOOM shareware IWAD with every map except E1M1
removed, plus the 55 `DS*` sound lumps merged back in (`tools/merge_sfx.py`).
About 3.1 MB. Shared resources (palettes, textures, sprites, graphics) are
untouched; only the other maps' lumps are gone.

Why trim maps: the flash is 4 MB, the firmware takes about 545 KB, and the
bootloader wants the first 16 KB. Why add sounds: GBADoom's `GbaWadUtil`
strips `DS*` because that engine used a Maxmod bank; this port mixes those
lumps from XIP instead.

Flash it to `0x08090000` (not `0x08110000` — the extra 523 KB of PCM does
not fit above that):

```sh
python tools/flash_openocd.py write wad/doom1_e1m1_sfx.wad 0x08090000 --leave-halted
```

The firmware reads it by pointer straight out of the memory-mapped flash,
so nothing is copied into RAM. See `firmware/port/doom_iwad_flash.c`.

## Making your own

To rebuild the SFX WAD from a trimmed E1M1 WAD and a full shareware IWAD:

```sh
python tools/merge_sfx.py --iwad doom1.wad --into doom1_e1m1.wad \
    --out wad/doom1_e1m1_sfx.wad
```

Map trimming itself lived in the research tree and is not in this
repository. If you replace this with a full WAD, revert the two `d_main.c`
patches in `firmware/GBADoom/engine-patches.diff`. They exist only because
a single-map WAD breaks two assumptions in the engine:

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
