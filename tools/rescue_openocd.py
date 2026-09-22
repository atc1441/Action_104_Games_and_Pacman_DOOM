#!/usr/bin/env python3
"""
Recover a console whose firmware wedges the debug bus, using OpenOCD
and a Pico debugprobe (CMSIS-DAP).

Same idea as rescue.py (J-Link): hammer attach-and-halt while you
power-cycle the console. nRESET is not on the board.

  python rescue_openocd.py
  python rescue_openocd.py --write ../firmware/build/action104/doom.bin --addr 0x08004000
"""
import argparse
import os
import subprocess
import sys
import time

import flash_openocd
import ocd


def try_catch_once():
    """Spawn a fresh OpenOCD, halt, return the session or None."""
    subprocess.call(['killall', '-9', 'openocd'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.05)
    log = open('/tmp/openocd-rescue.log', 'ab', buffering=0)
    proc = subprocess.Popen(
        ['openocd', '-f', ocd.CFG,
         '-c', 'gdb_port disabled',
         '-c', 'telnet_port disabled',
         '-c', 'adapter serial EB1FC28122DFFC1B'],
        stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    proc._log = log
    dev = ocd.OpenOcd(proc=proc)
    try:
        dev.connect(timeout=0.8)
        dev.sock.settimeout(1.5)
        try:
            dev.cmd('init')
        except Exception:
            pass
        try:
            if not dev.halted():
                dev.halt()
        except Exception:
            pass
        if dev.halted():
            dev.sock.settimeout(30.0)
            return dev
        raise RuntimeError('not halted')
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass
        try:
            log.close()
        except Exception:
            pass
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--window', type=float, default=120.0,
                    help='seconds to keep trying (default 120)')
    ap.add_argument('--write', metavar='FILE',
                    help='flash this file once the core is caught')
    ap.add_argument('--addr', default='0x08004000')
    args = ap.parse_args()

    print("Hammering OpenOCD attach attempts. Interrupt the console's power now -\n"
          "pull a battery, unplug USB, flip the switch - and keep doing it\n"
          "every second or two. Waiting up to %.0f s.\n" % args.window,
          flush=True)

    t0, tries, dev = time.time(), 0, None
    while time.time() - t0 < args.window:
        tries += 1
        dev = try_catch_once()
        if dev is not None:
            break
    dt = time.time() - t0

    if dev is None:
        print("Not caught (%d attempts in %.1f s).\n"
              "Check the SWD wiring and that the console really is losing "
              "power between attempts." % (tries, dt), flush=True)
        sys.exit(1)

    try:
        pc = dev.reg_read('pc')
        print("Caught after %.2f s (%d attempts), PC=0x%08X" % (dt, tries, pc),
              flush=True)
    except Exception as e:
        print("Caught after %.2f s, PC unreadable (%s)" % (dt, e), flush=True)

    if args.write:
        print("Loading writer to 0x%08X ..." % flash_openocd.LOAD_ADDR, flush=True)
        flash_openocd.load_writer(dev)
        print("Writer running, controller in manual mode.", flush=True)
        flash_openocd.do_write(dev, args.write, int(args.addr, 0))
        try:
            dev.reset_halt()
            dev.resume()
        except Exception:
            pass
        print("Flashed, console restarted.", flush=True)
    else:
        print("Core halted. Leave this running and flash from here with "
              "--write, or the firmware will hang again the moment the "
              "connection closes.", flush=True)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass

    dev.close()


if __name__ == '__main__':
    main()
