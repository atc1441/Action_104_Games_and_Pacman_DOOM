#!/usr/bin/env python3
"""
Where the CPU time goes, read over SWD from a PROFILE=1 build.

  make -C ../firmware PROFILE=1
  python profile.py ../firmware/build/action104-prof/doom.elf [seconds]

The firmware adds the core cycle counter (DWT_CYCCNT) into a few globals:
the audio refill interrupt, the frame push to the LCD and, inside it, the
wait for each SPI transfer, and the engine's wait for the next tic. This
reads them twice, `seconds` apart (default 3), and prints the shares.
Reads only, so it is safe on a running console. CYCCNT wraps after
22 s at 194 MHz, so keep the window shorter than that.

Needs arm-none-eabi-nm on PATH for the symbol addresses.
"""
import argparse
import subprocess
import sys
import time

import ocd

COUNTERS = ('prof_audio_cyc', 'prof_frames', 'prof_push_cyc',
            'prof_spi_wait_cyc', 'prof_idle_cyc')
DWT_CYCCNT = 0xE0001004


def symbols(elf):
    out = subprocess.run(['arm-none-eabi-nm', elf], capture_output=True,
                         text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf', help='doom.elf of the image on the console')
    ap.add_argument('seconds', nargs='?', type=float, default=3.0)
    args = ap.parse_args()

    syms = symbols(args.elf)
    missing = [n for n in COUNTERS + ('g_cpu_hz',) if n not in syms]
    if missing:
        sys.exit('%s: no %s - not a PROFILE=1 build?'
                 % (args.elf, ', '.join(missing)))

    o = ocd.connect_or_spawn()
    hz = o.mdw(syms['g_cpu_hz'])[0]

    def snap():
        return [o.mdw(syms[n])[0] for n in COUNTERS] + [o.mdw(DWT_CYCCNT)[0]]

    a = snap()
    time.sleep(args.seconds)
    b = snap()
    audio, frames, push, spi_wait, idle, cyc = \
        [(y - x) & 0xFFFFFFFF for x, y in zip(a, b)]

    pct = lambda v: 100.0 * v / cyc
    ms = lambda v: 1000.0 * v / hz / max(frames, 1)
    print('%.2f s at %.2f MHz, %d frames: %.1f fps, %.1f ms per frame'
          % (cyc / hz, hz / 1e6, frames, frames * hz / cyc, ms(cyc)))
    print('  audio refill  %5.1f %%' % pct(audio))
    print('  LCD push      %5.1f %%  %.2f ms per frame' % (pct(push), ms(push)))
    print('    SPI wait    %5.1f %%  %.2f ms per frame'
          % (pct(spi_wait), ms(spi_wait)))
    print('  tic wait      %5.1f %%  (audio interrupts taken meanwhile '
          'count here too)' % pct(idle))
    print('  engine       ~%5.1f %%  (the rest)'
          % pct(cyc - audio - push - idle))


if __name__ == '__main__':
    main()
