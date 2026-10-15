# WAD

`doom1_e1m5_music.wad` is the one to flash: the DOOM shareware IWAD with
E1M1-E1M5, the 55 `DS*` sound lumps and the music for those maps,
3,595,968 bytes. E1M6-E1M9 are gone, and so are lumps nothing in E1M1-E1M5
reads (see [Dropped lumps](#dropped-lumps)).

Flash it to `0x08090000`:

```sh
python tools/flash_openocd.py write wad/doom1_e1m5_music.wad 0x08090000 --leave-halted
```

The firmware reads it by pointer straight out of the memory-mapped flash,
so nothing is copied into RAM. See `firmware/port/doom_iwad_flash.c`.

## Why five maps

The flash is 4 MB. The bootloader wants the first 16 KB, the firmware
takes about 550 KB, and from `0x08090000` to the end of the flash there are
3,604,480 bytes for the WAD. In the layout GBADoom reads (below), the maps
take:

| Map | Bytes | | Map | Bytes |
|---|---|---|---|---|
| E1M1 | 80,508 | | E1M6 | 219,787 |
| E1M2 | 167,074 | | E1M7 | 154,739 |
| E1M3 | 163,113 | | E1M8 | 75,061 |
| E1M4 | 129,992 | | E1M9 | 108,198 |
| E1M5 | 130,813 | | | |

With the music and without the dropped lumps, E1M1-E1M5 leave 8,512
bytes free. E1M6 alone is 219,787 bytes, and E1M6-E1M9 are 557,785
bytes before their music and the Baron E1M8 needs.

RAM is not the limit. The zone heap is 204,780 bytes and the engine keeps
29,924 of them. Measured over SWD, levels took 55 KB more in E1M1 (in
play) and 111 KB in E1M3 (demo 2), leaving 63 KB free. Scaled by map
size, the largest map, E1M6, would leave about 30 KB free.

## The files

| File | Contents |
|---|---|
| `doom1_e1m1.wad` | E1M1 only, as `GbaWadUtil` wrote it, no sound |
| `doom1_e1m1_sfx.wad` | the same plus the `DS*` lumps |
| `doom1_e1m5_music.wad` | the same plus E1M2-E1M5 and the music, minus the dropped lumps |

## Making your own

`GbaWadUtil` (github.com/doomhack/GbaWadUtil) converts an IWAD to what
GBADoom reads in place: VERTEXES, LINEDEFS, SEGS and SIDEDEFS become the
engine's own structs, and it drops the `DS*`, `DP*` and `D_*` lumps.
Trimming the maps was done in a research tree that is not in this
repository.

To put the sounds back, then add maps and music and drop what is unused,
from a full shareware IWAD (v1.9, SHA-1
`5b2e249b9c5133ec987b3ea77596381dc0d6bc1d`):

```sh
python tools/merge_sfx.py --iwad doom1.wad --into wad/doom1_e1m1.wad \
    --out wad/doom1_e1m1_sfx.wad
python tools/edit_wad.py --iwad doom1.wad --into wad/doom1_e1m1_sfx.wad \
    --maps E1M2 E1M3 E1M4 E1M5 \
    --drop HELP1 'WILV1?' 'WILV2?' DEMO1 DEMO3 \
           'STFB?' 'STPB?' 'WIBP?' WIFRGS WIKILRS WIOSTS WIOSTF \
           M_RDTHIS M_DISOPT M_MSENS \
           DMXGUS ENDOOM STCDROM STDISK CREDITS 'STTNUM?' STTMINUS \
           'BOSS*' 'BAL7*' DSBRSSIT DSBRSDTH \
    --add GENMIDI D_INTRO D_E1M1 D_E1M2 D_E1M3 D_E1M4 D_E1M5 D_INTER \
    --out wad/doom1_e1m5_music.wad
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
`D_INTRO` (title), `D_E1M1`-`D_E1M5` and `D_INTER` (stats screen), 131,948
bytes. Together that is 143,856 bytes. `firmware/port/i_music_console.c`
plays them.

### Dropped lumps

None of these is read by GBADoom with E1M1-E1M5. Found by searching the
engine source for each lump name and the maps' things on every skill,
then checked on the console: title loop, every menu page, status bar, the
E1M1, E1M2 and E1M4 stats screens, and E1M4 and E1M5.

| Lumps | Bytes | Why unused |
|---|---|---|
| `HELP1` | 68,168 | GBADoom's title loop and read-this pages show `HELP2` and `CREDIT` only |
| `WILV1?`, `WILV2?` | 54,944 | level names of episodes 2 and 3, which shareware never reaches |
| `DEMO1`, `DEMO3` | 28,668 | recorded in E1M5 and E1M7, so they are skipped anyway |
| `STFB?`, `STPB?`, `WIBP?`, `WIFRGS`, `WIKILRS`, `WIOSTS`, `WIOSTF` | 16,464 | multiplayer status bar and intermission |
| `M_RDTHIS`, `M_DISOPT`, `M_MSENS` | 10,500 | menu items GBADoom removed |
| `DMXGUS`, `ENDOOM`, `STCDROM`, `STDISK`, `CREDITS`, `STTNUM?`, `STTMINUS` | 13,520 | DOS-only (GUS setup, exit screen, disk icons), the unused `CREDITS` page `GbaWadUtil` adds, and the status bar digits GBADoom replaced with `STGANUM?` |
| `BOSS*`, `BAL7*`, `DSBRSSIT`, `DSBRSDTH` | 173,317 | the Baron of Hell, its fireball and its sounds: in the shareware episode only E1M8 has Barons. A sprite with no frames at all is skipped by `R_InitSprites` (one with only some frames is an error, so all go together), and a missing sound is not played |

That is 365,581 bytes.

Three engine patches in `firmware/GBADoom/engine-patches.diff` handle a WAD
that holds only part of the episode. All three do nothing with a full WAD:

* a demo whose map is missing is replaced by the title screen. With
  E1M1-E1M5 only demo 2 plays: demo 3 is recorded in E1M7, and demo 1,
  recorded in E1M5, was dropped before E1M5 came in and does not fit back
  (20,118 bytes).
  Demo 2 also goes out of sync after about 15 s: its player dies and the
  rest of the recorded input runs into walls. Upstream GBADoom lists demo
  compatibility as broken, and it happens with or without our changes.
* the game mode is inferred from how many maps exist, and with fewer than
  nine no threshold matches. The four-episode menu would appear and abort
  with `M_EPI4 not found`, so the mode is forced to shareware.
* finishing a level whose next map is missing restarts the console to the
  title screen, without the stats screen.

## Licence

The DOOM shareware IWAD is freely redistributable under id Software's
shareware terms - that is the whole point of the shareware release. It is
not GPL; the GPL applies to the engine source, not to the game data.

Retail WADs are not redistributable. If you want DOOM II or Ultimate DOOM
on the console, use your own copy - and note that a full IWAD will not fit
in the remaining flash without trimming.
