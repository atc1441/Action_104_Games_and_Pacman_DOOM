#!/usr/bin/env python3
"""
Recover a console whose firmware wedges the debug bus.

If flashed firmware hangs shortly after startup, the CPU becomes
unreachable over SWD - and switching the console off and on does not help
on its own, because the same firmware restarts and hangs again. "Failed to
attach to CPU" at a healthy 3 V VTref is exactly that picture.

The way in is timing. This script hammers connect-and-halt attempts as
fast as J-Link allows; you power-cycle the console while it runs. Sooner
or later an attempt lands in the window between power-on and the crash,
the core is halted, and from there flash.py takes over - it stops inside
the BootROM anyway and never lets the broken application start.

  python rescue.py                                  # just catch and halt
  python rescue.py --write ../firmware/build/action104/doom.bin --addr 0x08004000

All you need is the SWD probe and a way to interrupt power: pull a
battery, unplug USB, flip the power switch. Keep doing that every second
or two until it reports a catch.

Two things that are easy to get wrong and cost a lot of time:

  * Use a FRESH pylink.JLink object per attempt. Reusing one makes
    connect() fail instantly after the first failure (3.5 ms instead of
    340 ms) without retrying the handshake at all. One run reported 51356
    attempts, not one of which was real.

  * Do the flashing in this same process. Closing the connection lets the
    core run on, it hangs again immediately, and the next process never
    gets near it. Hence --write rather than "now run flash.py".
"""
import argparse, sys, time

import pylink
import flash

DEVICE, SPEED = 'Cortex-M4', 4000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--window', type=float, default=120.0,
                    help='seconds to keep trying (default 120)')
    ap.add_argument('--write', metavar='FILE',
                    help='flash this file once the core is caught')
    ap.add_argument('--addr', default='0x08004000')
    args = ap.parse_args()

    print("Hammering connect attempts. Interrupt the console's power now -\n"
          "pull a battery, unplug USB, flip the switch - and keep doing it\n"
          "every second or two. Waiting up to %.0f s.\n" % args.window,
          flush=True)

    t0, tries, caught, jl = time.time(), 0, False, None
    while time.time() - t0 < args.window:
        tries += 1
        try:
            if jl:
                jl.close()
        except Exception:
            pass
        try:
            jl = pylink.JLink()
            jl.open()
            jl.set_tif(pylink.enums.JLinkInterfaces.SWD)
            jl.connect(DEVICE, speed=SPEED)
            jl.halt()
            caught = True
            break
        except Exception:
            pass
    dt = time.time() - t0

    if not caught:
        print("Not caught (%d attempts in %.1f s).\n"
              "Check the SWD wiring and that the console really is losing "
              "power between attempts." % (tries, dt), flush=True)
        sys.exit(1)

    try:
        pc = jl.register_read(15)
        print("Caught after %.2f s (%d attempts), PC=0x%08X" % (dt, tries, pc),
              flush=True)
    except Exception as e:
        print("Caught after %.2f s, PC unreadable (%s)" % (dt, e), flush=True)

    if args.write:
        print("Loading writer to 0x%08X ..." % flash.LOAD_ADDR, flush=True)
        flash.load_writer(jl)
        print("Writer running, controller in manual mode.", flush=True)
        flash.do_write(jl, args.write, int(args.addr, 0))
        jl.reset()
        jl.restart()
        print("Flashed, console restarted.", flush=True)
    else:
        print("Core halted. Leave this running and flash from here with "
              "--write, or the firmware will hang again the moment the "
              "connection closes.", flush=True)

    jl.close()


if __name__ == '__main__':
    main()
