#!/usr/bin/env python3
"""
Dump live STAR-MC1 state over OpenOCD while stock firmware is running.

  python peek_openocd.py           # halt, dump, resume
  python peek_openocd.py --halt    # leave halted

Prints CPUID, MPI (flash) registers, DMA channel 0, and 32 bytes at
the stock audio descriptor 0x20044BA0.
"""
import argparse
import sys

import ocd

MPI = 0x52005000
RCC = 0x40021000
DMA_CH0 = 0x40031100
DMA_BASE = 0x40031000
AUDIO = 0x40012C00
DESC = 0x20044BA0
STUB = 0x20000000
CPUID = 0xE000ED00


def hexdump(addr, words):
    for i, w in enumerate(words):
        print("  0x%08X: 0x%08X" % (addr + 4 * i, w))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--halt', action='store_true',
                    help='leave the core halted')
    args = ap.parse_args()

    dev = ocd.connect_or_spawn()
    try:
        running = not dev.halted()
        if running:
            dev.halt()

        cpuid = dev.memory_read32(CPUID, 1)[0]
        print("CPUID  0x%08X%s" % (
            cpuid,
            "  (STAR-MC1 / 0x631F1320)" if cpuid == 0x631F1320 else ""))

        print("RCC @ 0x%08X" % RCC)
        hexdump(RCC, dev.memory_read32(RCC, 20))
        r1c = dev.memory_read32(RCC + 0x1C, 1)[0]
        r20 = dev.memory_read32(RCC + 0x20, 1)[0]
        print("  source=%d  R20=0x%08X  PLLCFG=0x%X" % (
            r1c & 7, r20, dev.memory_read32(RCC + 0x4C, 1)[0]))

        print("MPI @ 0x%08X" % MPI)
        hexdump(MPI, dev.memory_read32(MPI, 16))

        print("stock RAM stub @ 0x%08X" % STUB)
        hexdump(STUB, dev.memory_read32(STUB, 8))

        print("DMA global @ 0x%08X" % DMA_BASE)
        hexdump(DMA_BASE, dev.memory_read32(DMA_BASE, 16))

        print("DMA ch0 @ 0x%08X" % DMA_CH0)
        hexdump(DMA_CH0, dev.memory_read32(DMA_CH0, 8))

        print("DAC @ 0x%08X" % AUDIO)
        hexdump(AUDIO, dev.memory_read32(AUDIO, 20))

        print("audio descriptor @ 0x%08X" % DESC)
        hexdump(DESC, dev.memory_read32(DESC, 8))

        if running and not args.halt:
            dev.resume()
    finally:
        if not args.halt:
            # keep OpenOCD if we attached to an existing one
            if dev.proc:
                dev.close()
            elif dev.sock:
                try:
                    dev.sock.close()
                except OSError:
                    pass
        else:
            print("Core left halted.")
            if dev.proc:
                print("OpenOCD child still running (PID %d)." % dev.proc.pid)


if __name__ == '__main__':
    main()
