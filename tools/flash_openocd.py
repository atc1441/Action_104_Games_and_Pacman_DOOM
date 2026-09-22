#!/usr/bin/env python3
"""
Program the console's external SPI flash over CMSIS-DAP / OpenOCD.

Same RAM-writer protocol as flash.py (J-Link). There is no OpenOCD flash
bank for this SoC's MPI controller.

  python flash_openocd.py id
  python flash_openocd.py read  out.bin 0x08000000 0x400000
  python flash_openocd.py write img.bin 0x08004000
  python flash_openocd.py verify img.bin 0x08004000
"""
import os
import struct
import sys
import time

import ocd

WRITER_BIN = os.path.join(os.path.dirname(__file__),
                          'flashwriter', 'build', 'writer.bin')
LOAD_ADDR = 0x20010000
MBOX = 0x2001E000
BUF = 0x20020000
BUF_SIZE = 0x10000
MAGIC = 0xF1A54EE5

CMD_JEDEC, CMD_ERASE_4K, CMD_PROGRAM, CMD_READ, CMD_EXIT = 1, 2, 3, 4, 5

SECTOR = 0x1000
FLASH_BASE = 0x08000000
BOOT_VTOR_HOOK = 0x4584


def boot_until_flash_ready(dev):
    """Let the bootloader set up the MPI, then stop before the app."""
    dev.reset_halt()
    dev.bp_clear(BOOT_VTOR_HOOK)
    dev.bp_set(BOOT_VTOR_HOOK, 2)
    dev.resume()
    for _ in range(300):
        time.sleep(0.01)
        if dev.halted():
            break
    else:
        dev.halt()
        dev.bp_clear(BOOT_VTOR_HOOK)
        sys.exit("Bootloader never reached 0x4584 - is the console powered?")
    dev.bp_clear(BOOT_VTOR_HOOK)


def load_writer(dev):
    if not os.path.exists(WRITER_BIN):
        sys.exit("Writer not built. Run:  make -C tools/flashwriter")
    with open(WRITER_BIN, 'rb') as f:
        img = f.read()
    sp, pc = struct.unpack_from('<II', img, 0)

    boot_until_flash_ready(dev)
    dev.memory_write8(LOAD_ADDR, img)
    dev.memory_write32(MBOX, [0] * 6)

    dev.reg_write('msp', sp)
    dev.reg_write('pc', pc & ~1)
    dev.resume()

    for _ in range(200):
        time.sleep(0.01)
        if dev.memory_read32(MBOX + 0x14, 1)[0] == MAGIC:
            return
    sys.exit("Writer did not report in (magic missing). Is the console powered?")


def cmd(dev, c, addr=0, length=0, timeout=20.0):
    dev.memory_write32(MBOX + 0x04, [addr, length])
    dev.memory_write32(MBOX + 0x00, [c])
    t0 = time.time()
    while time.time() - t0 < timeout:
        if dev.memory_read32(MBOX, 1)[0] == 0:
            st = dev.memory_read32(MBOX + 0x0C, 1)[0]
            res = dev.memory_read32(MBOX + 0x10, 1)[0]
            if st:
                sys.exit("Writer reported error %s for command %s, address 0x%08X"
                         % (st, c, addr))
            return res
        time.sleep(0.001)
    sys.exit("Timeout on command %s" % c)


def to_offset(addr):
    return addr - FLASH_BASE if addr >= FLASH_BASE else addr


def do_id(dev):
    jid = cmd(dev, CMD_JEDEC)
    mfr, typ, cap = (jid >> 16) & 0xFF, (jid >> 8) & 0xFF, jid & 0xFF
    size = 1 << cap if 0x10 <= cap <= 0x20 else 0
    print("JEDEC id: 0x%06X  manufacturer 0x%02X, type 0x%02X, capacity 0x%02X"
          % (jid, mfr, typ, cap)
          + (" = %d MB" % (size // (1024 * 1024)) if size else ""))
    known = {0xEF: "Winbond", 0x5E: "Zbit", 0xC8: "GigaDevice",
             0x1C: "Eon", 0xA1: "Fudan", 0x68: "Boya"}
    if mfr in known:
        print("  -> %s" % known[mfr])
    if jid == 0x5E4016:
        print("     Zbit ZB25VQ32, 4 MB - the chip fitted here (W25Q compatible)")
    return jid


def do_read(dev, path, addr, length):
    off = to_offset(addr)
    out = bytearray()
    t0 = time.time()
    while len(out) < length:
        n = min(BUF_SIZE, length - len(out))
        cmd(dev, CMD_READ, off + len(out), n)
        out += bytes(dev.memory_read8(BUF, n))
        print("\r  read %d/%d" % (len(out), length), end='', flush=True)
    print("   (%.1f s)" % (time.time() - t0))
    with open(path, 'wb') as f:
        f.write(out)
    print("-> %s" % path)


def do_write(dev, path, addr, verify=True):
    with open(path, 'rb') as f:
        img = f.read()
    off = to_offset(addr)

    if off % SECTOR:
        sys.exit("Start address must be 4 KB aligned "
                 "(0x%08X -> offset 0x%X)" % (addr, off))

    nsec = (len(img) + SECTOR - 1) // SECTOR
    print("%d bytes to 0x%08X (offset 0x%X), %d sectors"
          % (len(img), addr, off, nsec))

    t0 = time.time()
    for i in range(nsec):
        cmd(dev, CMD_ERASE_4K, off + i * SECTOR, timeout=10.0)
        print("\r  erasing %d/%d" % (i + 1, nsec), end='', flush=True)
    print("   (%.1f s)" % (time.time() - t0))

    t0 = time.time()
    done = 0
    while done < len(img):
        n = min(BUF_SIZE, len(img) - done)
        dev.memory_write8(BUF, img[done:done + n])
        cmd(dev, CMD_PROGRAM, off + done, n, timeout=60.0)
        done += n
        print("\r  writing %d/%d" % (done, len(img)), end='', flush=True)
    print("   (%.1f s)" % (time.time() - t0))

    if verify:
        do_verify(dev, path, addr)


def do_verify(dev, path, addr):
    with open(path, 'rb') as f:
        img = f.read()
    off = to_offset(addr)
    bad = 0
    done = 0
    while done < len(img):
        n = min(BUF_SIZE, len(img) - done)
        cmd(dev, CMD_READ, off + done, n)
        got = bytes(dev.memory_read8(BUF, n))
        want = img[done:done + n]
        if got != want:
            for k in range(n):
                if got[k] != want[k]:
                    if bad < 8:
                        print("\n  mismatch at 0x%08X: read 0x%02X, expected 0x%02X"
                              % (addr + done + k, got[k], want[k]))
                    bad += 1
        done += n
        print("\r  verifying %d/%d" % (done, len(img)), end='', flush=True)
    print()
    if bad:
        sys.exit("FAILED: %d bytes differ" % bad)
    print("Verify OK.")


def main():
    import argparse
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='action', required=True)
    sub.add_parser('id')
    p = sub.add_parser('read')
    p.add_argument('file')
    p.add_argument('addr')
    p.add_argument('len')
    p = sub.add_parser('write')
    p.add_argument('file')
    p.add_argument('addr')
    p.add_argument('--no-verify', action='store_true')
    p.add_argument('--leave-halted', action='store_true',
                   help='do not resume after write (boot without the probe)')
    p = sub.add_parser('verify')
    p.add_argument('file')
    p.add_argument('addr')
    args = ap.parse_args()

    dev = ocd.connect_or_spawn()
    print("Connected. Loading writer to 0x%08X ..." % LOAD_ADDR)
    load_writer(dev)
    print("Writer running, controller in manual mode.")

    try:
        if args.action == 'id':
            do_id(dev)
        elif args.action == 'read':
            do_read(dev, args.file, int(args.addr, 0), int(args.len, 0))
        elif args.action == 'write':
            do_write(dev, args.file, int(args.addr, 0), not args.no_verify)
        elif args.action == 'verify':
            do_verify(dev, args.file, int(args.addr, 0))
    finally:
        print("Restoring XIP mode ...")
        try:
            dev.memory_write32(MBOX + 0x00, [CMD_EXIT])
            time.sleep(0.2)
        except Exception:
            pass
        try:
            if getattr(args, 'leave_halted', False):
                dev.reset_halt()
            else:
                dev.reset_halt()
                dev.resume()
        except Exception:
            pass
        dev.close()
        if getattr(args, 'leave_halted', False):
            print("Done, core left halted. Power-cycle to boot without SWD.")
        else:
            print("Done, console restarted.")


if __name__ == '__main__':
    main()
