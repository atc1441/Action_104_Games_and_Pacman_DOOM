# WAD

`doom1_e1m3_sfx.wad` is the one to flash: the DOOM shareware IWAD with
E1M1-E1M3 and the 55 `DS*` sound lumps, 3,558,448 bytes. Shared resources
(palettes, textures, sprites, graphics) are untouched; E1M4-E1M9 and the
music are gone.

Flash it to `0x08090000`:

```sh
python tools/flash_openocd.py write wad/doom1_e1m3_sfx.wad 0x08090000 --leave-halted
```

The firmware reads it by pointer straight out of the memory-mapped flash,
so nothing is copied into RAM. See `firmware/port/doom_iwad_flash.c`.

## Why three maps

The flash is 4 MB. The bootloader wants the first 16 KB, the firmware
takes about 545 KB, and from `0x08090000` to the end of the flash there are
3,604,480 bytes for the WAD. In the layout GBADoom reads (below), the maps
take:

| Map | Bytes | | Map | Bytes |
|---|---|---|---|---|
| E1M1 | 80,508 | | E1M6 | 219,787 |
| E1M2 | 167,074 | | E1M7 | 154,739 |
| E1M3 | 163,113 | | E1M8 | 75,061 |
| E1M4 | 129,992 | | E1M9 | 108,198 |
| E1M5 | 130,813 | | | |

E1M1-E1M3 leave 46,032 bytes free. The whole episode would be 774 KB
short.

RAM is not the limit. The zone heap is 204,780 bytes and the engine keeps
29,924 of them. Measured over SWD, levels took 55 KB more in E1M1 (in
play) and 111 KB in E1M3 (demo 2), leaving 63 KB free. Scaled by map
size, the largest map, E1M6, would leave about 30 KB free.

## The files

| File | Contents |
|---|---|
| `doom1_e1m1.wad` | E1M1 only, as `GbaWadUtil` wrote it, no sound |
| `doom1_e1m1_sfx.wad` | the same plus the `DS*` lumps |
| `doom1_e1m3_sfx.wad` | the same plus E1M2 and E1M3 |
| `doom1_e1m3_music.wad` | the same minus unused lumps, plus the music for E1M1-E1M3 |

`doom1_e1m3_music.wad` is 3,481,192 bytes and leaves 123,288 bytes free.
The firmware does not play music yet; the WAD works with it as is.

## Making your own

`GbaWadUtil` (github.com/doomhack/GbaWadUtil) converts an IWAD to what
GBADoom reads in place: VERTEXES, LINEDEFS, SEGS and SIDEDEFS become the
engine's own structs, and it drops the `DS*`, `DP*` and `D_*` lumps.
Trimming the maps was done in a research tree that is not in this
repository.

To put the sounds back, then add maps from a full shareware IWAD
(v1.9, SHA-1 `5b2e249b9c5133ec987b3ea77596381dc0d6bc1d`):

```sh
python tools/merge_sfx.py --iwad doom1.wad --into wad/doom1_e1m1.wad \
    --out wad/doom1_e1m1_sfx.wad
python tools/edit_wad.py --iwad doom1.wad --into wad/doom1_e1m1_sfx.wad \
    --maps E1M2 E1M3 --out wad/doom1_e1m3_sfx.wad
python tools/edit_wad.py --iwad doom1.wad --into wad/doom1_e1m1_sfx.wad \
    --maps E1M2 E1M3 \
    --drop HELP1 'WILV1?' 'WILV2?' DEMO1 DEMO3 \
           'STFB?' 'STPB?' 'WIBP?' WIFRGS WIKILRS WIOSTS WIOSTF \
           M_RDTHIS M_DISOPT M_MSENS \
           DMXGUS ENDOOM STCDROM STDISK CREDITS 'STTNUM?' STTMINUS \
    --add GENMIDI D_INTRO D_E1M1 D_E1M2 D_E1M3 D_INTER \
    --out wad/doom1_e1m3_music.wad
```

`edit_wad.py` does three things:

* `--maps` adds maps with a port of the `GbaWadUtil` map conversion.
  Before it writes anything it converts the maps already in `--into` from
  the IWAD as well and stops unless the result is byte for byte what is
  there.
* `--add` copies lumps from the IWAD unchanged.
* `--drop` removes lumps by name or shell pattern. Lumps inside a map are
  never dropped, and a pattern that matches nothing is an error.

### Music

`--add` puts back what the music needs: `GENMIDI` (11,908 bytes, the OPL2
instrument patches) and the MUS songs the shareware maps and screens use,
`D_INTRO` (title), `D_E1M1`-`D_E1M3` and `D_INTER` (stats screen), 103,902
bytes. Together that is 115,810 bytes.

### Dropped lumps

None of these is read by GBADoom with E1M1-E1M3. Found by searching the
engine source for each lump name, then checked on the console: title loop,
every menu page, status bar, and the E1M1 and E1M2 stats screens.

| Lumps | Bytes | Why unused |
|---|---|---|
| `HELP1` | 68,168 | GBADoom's title loop and read-this pages show `HELP2` and `CREDIT` only |
| `WILV1?`, `WILV2?` | 54,944 | level names of episodes 2 and 3, which shareware never reaches |
| `DEMO1`, `DEMO3` | 28,668 | recorded in E1M5 and E1M7, so they are skipped anyway |
| `STFB?`, `STPB?`, `WIBP?`, `WIFRGS`, `WIKILRS`, `WIOSTS`, `WIOSTF` | 16,464 | multiplayer status bar and intermission |
| `M_RDTHIS`, `M_DISOPT`, `M_MSENS` | 10,500 | menu items GBADoom removed |
| `DMXGUS`, `ENDOOM`, `STCDROM`, `STDISK`, `CREDITS`, `STTNUM?`, `STTMINUS` | 13,520 | DOS-only (GUS setup, exit screen, disk icons), the unused `CREDITS` page `GbaWadUtil` adds, and the status bar digits GBADoom replaced with `STGANUM?` |

That is 192,264 bytes, more than the music adds.

Three engine patches in `firmware/GBADoom/engine-patches.diff` handle a WAD
that holds only part of the episode. All three do nothing with a full WAD:

* a demo whose map is missing is replaced by the title screen. With
  E1M1-E1M3 only demo 2 plays; demos 1 and 3 are recorded in E1M5 and E1M7.
* the game mode is inferred from how many maps exist, and with fewer than
  nine no threshold matches. The four-episode menu would appear and abort
  with `M_EPI4 not found`, so the mode is forced to shareware.
* finishing a level whose next map is missing restarts the console to the
  title screen.

## Licence

The DOOM shareware IWAD is freely redistributable under id Software's
shareware terms - that is the whole point of the shareware release. It is
not GPL; the GPL applies to the engine source, not to the game data.

Retail WADs are not redistributable. If you want DOOM II or Ultimate DOOM
on the console, use your own copy - and note that a full IWAD will not fit
in the remaining flash without trimming.
