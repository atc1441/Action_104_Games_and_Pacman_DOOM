#!/usr/bin/env python3
"""
Program the console's external SPI flash over J-Link.

J-Link cannot do this flash by itself - there is no flash algorithm for
this SoC's MPI controller. So this tool loads a small RAM-resident writer
(tools/flashwriter) to 0x20010000, starts it, and hands it jobs through a
mailbox in SRAM.

  python flash.py id                       # read the JEDEC id only
  python flash.py read  out.bin 0x08000000 0x400000
  python flash.py write img.bin 0x08004000
  python flash.py write img.bin 0x08004000 --leave-halted
  python flash.py verify img.bin 0x08004000

The writer takes the controller out of XIP mode. While it runs the flash
is NOT readable as memory - which is exactly why the writer has to live
in RAM. Its "exit" command restores XIP mode.
"""
import argparse, os, struct, sys, time

import pylink

DEVICE      = 'Cortex-M4'      # generic; the core is really a STAR-MC1
SPEED       = 4000

WRITER_BIN  = os.path.join(os.path.dirname(__file__),
                           'flashwriter', 'build', 'writer.bin')
LOAD_ADDR   = 0x20010000
MBOX        = 0x2001E000
BUF         = 0x20020000
BUF_SIZE    = 0x10000
MAGIC       = 0xF1A54EE5

CMD_JEDEC, CMD_ERASE_4K, CMD_PROGRAM, CMD_READ, CMD_EXIT = 1, 2, 3, 4, 5

SECTOR = 0x1000
FLASH_BASE = 0x08000000        # XIP window; chip offsets are relative to it


# --------------------------------------------------------------------- #

def connect():
    jl = pylink.JLink()
    jl.open()
    jl.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jl.connect(DEVICE, speed=SPEED)
    if not jl.target_connected():
        sys.exit("No target connected.")
    return jl


def reg_index(jl, *names):
    """Find a register index by name - the numbering is not the same
    across J-Link versions."""
    idxs = jl.register_list()
    table = {}
    for i in idxs:
        try:
            table[jl.register_name(i).upper()] = i
        except Exception:
            pass
    for n in names:
        if n.upper() in table:
            return table[n.upper()]
    sys.exit(f"Register {names} not found. Available: {sorted(table)}")


BOOT_VTOR_HOOK = 0x4584     # bootloader: the only place that writes VTOR


def boot_until_flash_ready(jl):
    """Let the bootloader run until the flash controller is set up.

    After a hard reset the MPI controller sits in its default state: XIP
    returns nothing but 0xFFFFFFFF, and manual mode hangs waiting for
    "TX done". Only the bootloader configures its clock and timing.

    We break at 0x4584, where the bootloader writes VTOR - i.e. right
    before it jumps into the application. At that point the flash is
    fully initialised but the stock app has not started yet.
    """
    jl.reset(halt=True)
    jl.breakpoint_clear_all()
    bp = jl.breakpoint_set(BOOT_VTOR_HOOK, thumb=True)
    jl.restart()

    for _ in range(300):
        time.sleep(0.01)
        if jl.halted():
            break
    else:
        jl.halt()
        jl.breakpoint_clear(bp)
        sys.exit("Bootloader never reached 0x4584 - is the console powered?")

    jl.breakpoint_clear(bp)


def load_writer(jl):
    if not os.path.exists(WRITER_BIN):
        sys.exit("Writer not built. Run:  make -C "
                 + os.path.dirname(WRITER_BIN).replace(os.sep, '/').rsplit('/build', 1)[0])
    with open(WRITER_BIN, 'rb') as f:
        img = f.read()
    sp, pc = struct.unpack_from('<II', img, 0)

    boot_until_flash_ready(jl)
    jl.memory_write8(LOAD_ADDR, list(img))

    # Clear the mailbox so stale contents cannot be read as "ready"
    jl.memory_write32(MBOX, [0] * 6)

    i_sp = reg_index(jl, 'MSP', 'R13 (SP)', 'SP', 'R13')
    i_pc = reg_index(jl, 'R15 (PC)', 'PC', 'R15')
    jl.register_write(i_sp, sp)
    jl.register_write(i_pc, pc & ~1)
    jl.restart()

    for _ in range(200):
        time.sleep(0.01)
        if jl.memory_read32(MBOX + 0x14, 1)[0] == MAGIC:
            return
    sys.exit("Writer did not report in (magic missing). Is the console powered?")


def cmd(jl, c, addr=0, length=0, timeout=20.0):
    jl.memory_write32(MBOX + 0x04, [addr, length])
    jl.memory_write32(MBOX + 0x00, [c])
    t0 = time.time()
    while time.time() - t0 < timeout:
        if jl.memory_read32(MBOX, 1)[0] == 0:
            st, res = jl.memory_read32(MBOX + 0x0C, 2)
            if st:
                sys.exit(f"Writer reported error {st} for command {c}, "
                         f"address 0x{addr:08X}")
            return res
        time.sleep(0.001)
    sys.exit(f"Timeout on command {c}")


def to_offset(addr):
    """Address in the XIP window -> offset inside the flash chip."""
    return addr - FLASH_BASE if addr >= FLASH_BASE else addr


# --------------------------------------------------------------------- #

def do_id(jl):
    jid = cmd(jl, CMD_JEDEC)
    mfr, typ, cap = (jid >> 16) & 0xFF, (jid >> 8) & 0xFF, jid & 0xFF
    size = 1 << cap if 0x10 <= cap <= 0x20 else 0
    print(f"JEDEC id: 0x{jid:06X}  manufacturer 0x{mfr:02X}, type 0x{typ:02X}, "
          f"capacity 0x{cap:02X}" + (f" = {size // (1024*1024)} MB" if size else ""))
    known = {0xEF: "Winbond", 0x5E: "Zbit", 0xC8: "GigaDevice",
             0x1C: "Eon", 0xA1: "Fudan", 0x68: "Boya"}
    if mfr in known:
        print(f"  -> {known[mfr]}")
    if jid == 0x5E4016:
        print("     Zbit ZB25VQ32, 4 MB - the chip fitted here (W25Q compatible)")
    return jid


def do_read(jl, path, addr, length):
    off = to_offset(addr)
    out = bytearray()
    t0 = time.time()
    while len(out) < length:
        n = min(BUF_SIZE, length - len(out))
        cmd(jl, CMD_READ, off + len(out), n)
        out += bytes(jl.memory_read8(BUF, n))
        print(f"\r  read {len(out)}/{length}", end='', flush=True)
    print(f"   ({time.time() - t0:.1f} s)")
    with open(path, 'wb') as f:
        f.write(out)
    print(f"-> {path}")


def do_write(jl, path, addr, verify=True):
    with open(path, 'rb') as f:
        img = f.read()
    off = to_offset(addr)

    if off % SECTOR:
        sys.exit(f"Start address must be 4 KB aligned "
                 f"(0x{addr:08X} -> offset 0x{off:X})")

    nsec = (len(img) + SECTOR - 1) // SECTOR
    print(f"{len(img)} bytes to 0x{addr:08X} (offset 0x{off:X}), "
          f"{nsec} sectors")

    t0 = time.time()
    for i in range(nsec):
        cmd(jl, CMD_ERASE_4K, off + i * SECTOR, timeout=10.0)
        print(f"\r  erasing {i + 1}/{nsec}", end='', flush=True)
    print(f"   ({time.time() - t0:.1f} s)")

    t0 = time.time()
    done = 0
    while done < len(img):
        n = min(BUF_SIZE, len(img) - done)
        jl.memory_write8(BUF, list(img[done:done + n]))
        cmd(jl, CMD_PROGRAM, off + done, n, timeout=60.0)
        done += n
        print(f"\r  writing {done}/{len(img)}", end='', flush=True)
    print(f"   ({time.time() - t0:.1f} s)")

    if verify:
        do_verify(jl, path, addr)


def do_verify(jl, path, addr):
    with open(path, 'rb') as f:
        img = f.read()
    off = to_offset(addr)
    bad = 0
    done = 0
    while done < len(img):
        n = min(BUF_SIZE, len(img) - done)
        cmd(jl, CMD_READ, off + done, n)
        got = bytes(jl.memory_read8(BUF, n))
        want = img[done:done + n]
        if got != want:
            for k in range(n):
                if got[k] != want[k]:
                    if bad < 8:
                        print(f"\n  mismatch at 0x{addr + done + k:08X}: "
                              f"read 0x{got[k]:02X}, expected 0x{want[k]:02X}")
                    bad += 1
        done += n
        print(f"\r  verifying {done}/{len(img)}", end='', flush=True)
    print()
    if bad:
        sys.exit(f"FAILED: {bad} bytes differ")
    print("Verify OK.")


# --------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='action', required=True)
    sub.add_parser('id')
    p = sub.add_parser('read');   p.add_argument('file'); p.add_argument('addr'); p.add_argument('len')
    p = sub.add_parser('write');  p.add_argument('file'); p.add_argument('addr')
    p.add_argument('--no-verify', action='store_true')
    p.add_argument('--leave-halted', action='store_true',
                   help='do not resume after write (boot without the probe)')
    p = sub.add_parser('verify'); p.add_argument('file'); p.add_argument('addr')
    args = ap.parse_args()

    jl = connect()
    print(f"Connected. Loading writer to 0x{LOAD_ADDR:08X} ...")
    load_writer(jl)
    print("Writer running, controller in manual mode.")

    try:
        if args.action == 'id':
            do_id(jl)
        elif args.action == 'read':
            do_read(jl, args.file, int(args.addr, 0), int(args.len, 0))
        elif args.action == 'write':
            do_write(jl, args.file, int(args.addr, 0), not args.no_verify)
        elif args.action == 'verify':
            do_verify(jl, args.file, int(args.addr, 0))
    finally:
        print("Restoring XIP mode ...")
        try:
            jl.memory_write32(MBOX + 0x00, [CMD_EXIT])
            time.sleep(0.2)
        except Exception:
            pass
        if getattr(args, 'leave_halted', False):
            jl.reset(halt=True)
        else:
            jl.reset(halt=False)
        jl.close()
        if getattr(args, 'leave_halted', False):
            print("Done, core left halted. Power-cycle to boot without SWD.")
        else:
            print("Done, console restarted.")


if __name__ == '__main__':
    main()
