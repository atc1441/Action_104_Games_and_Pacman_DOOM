#!/usr/bin/env python3
"""
OpenOCD TCL-RPC helper for the Action 104 STAR-MC1.

Talks to an already-running OpenOCD on tcl_port (default 6666), or
spawns one with tools/openocd/star_mc1.cfg. Used by flash_openocd.py
and rescue_openocd.py; J-Link/pylink is not required.
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = os.path.join(HERE, 'openocd', 'star_mc1.cfg')
TCL_PORT = 6666
TELNET_PORT = 4444
GDB_PORT = 3333
EOM = b'\x1a'


class OpenOcdError(RuntimeError):
    pass


class OpenOcd:
    def __init__(self, host='127.0.0.1', tcl_port=TCL_PORT, proc=None):
        self.host = host
        self.tcl_port = tcl_port
        self.proc = proc
        self.sock = None

    def connect(self, timeout=8.0):
        t0 = time.time()
        last = None
        while time.time() - t0 < timeout:
            try:
                s = socket.create_connection((self.host, self.tcl_port), 1.0)
                s.settimeout(30.0)
                self.sock = s
                return
            except OSError as e:
                last = e
                time.sleep(0.05)
        raise OpenOcdError('OpenOCD TCL port %s:%d not reachable (%s)'
                           % (self.host, self.tcl_port, last))

    def close(self):
        proc = self.proc
        if self.sock:
            try:
                if proc:
                    try:
                        self.cmd('shutdown', ignore_error=True)
                    except Exception:
                        pass
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        if proc:
            try:
                proc.wait(timeout=3)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
            log = getattr(proc, '_log', None)
            if log:
                try:
                    log.close()
                except Exception:
                    pass
            self.proc = None

    def cmd(self, command, ignore_error=False):
        if self.sock is None:
            raise OpenOcdError('not connected')
        self.sock.sendall(command.encode('utf-8') + EOM)
        buf = b''
        while True:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            buf += chunk
            if EOM in buf:
                break
        text = buf.split(EOM, 1)[0].decode('utf-8', 'replace').strip()
        if (not ignore_error) and text.lower().startswith('error'):
            raise OpenOcdError('%s -> %s' % (command, text))
        return text

    def halt(self):
        self.cmd('halt')

    def resume(self):
        self.cmd('resume')

    def reset_halt(self):
        # nRESET is not on the board; SYSRESETREQ via the core.
        try:
            self.cmd('reset halt')
        except OpenOcdError:
            self.cmd('halt')

    def mdw(self, addr, count=1):
        """Read `count` 32-bit words. Returns a list of ints."""
        out = []
        # OpenOCD read_memory: address width count
        raw = self.cmd('read_memory 0x%x 32 %d' % (addr, count))
        for tok in raw.replace(',', ' ').split():
            tok = tok.strip()
            if tok.startswith('0x') or tok.startswith('0X'):
                out.append(int(tok, 16))
            elif tok:
                try:
                    out.append(int(tok, 16))
                except ValueError:
                    pass
        if len(out) < count:
            # Fallback: mdw-style one word at a time
            out = []
            for i in range(count):
                line = self.cmd('mdw 0x%x' % (addr + 4 * i))
                # "0x20000000: 00000000"
                parts = line.replace(':', ' ').split()
                val = None
                for p in reversed(parts):
                    try:
                        val = int(p, 16)
                        break
                    except ValueError:
                        continue
                if val is None:
                    raise OpenOcdError('mdw parse failed: %r' % line)
                out.append(val)
        return out[:count]

    def mww(self, addr, value):
        self.cmd('mww 0x%x 0x%x' % (addr, value & 0xFFFFFFFF))

    def memory_write32(self, addr, words):
        for i, w in enumerate(words):
            self.mww(addr + 4 * i, w)

    def memory_read32(self, addr, count):
        return self.mdw(addr, count)

    def memory_write8(self, addr, data):
        if isinstance(data, list):
            data = bytes(data)
        path = os.path.join('/tmp', 'ocd_wr_%d.bin' % os.getpid())
        with open(path, 'wb') as f:
            f.write(data)
        try:
            self.cmd('load_image {%s} 0x%x bin' % (path, addr))
        finally:
            try:
                os.remove(path)
            except OSError:
                pass

    def memory_read8(self, addr, length):
        path = os.path.join('/tmp', 'ocd_rd_%d.bin' % os.getpid())
        try:
            self.cmd('dump_image {%s} 0x%x %d' % (path, addr, length))
            with open(path, 'rb') as f:
                return list(f.read())
        finally:
            try:
                os.remove(path)
            except OSError:
                pass

    def reg_write(self, name, value):
        self.cmd('reg %s 0x%x' % (name, value & 0xFFFFFFFF))

    def reg_read(self, name):
        line = self.cmd('reg %s' % name)
        # "pc (/32): 0x08004118"
        for tok in line.replace(':', ' ').split():
            if tok.startswith('0x') or tok.startswith('0X'):
                return int(tok, 16)
        raise OpenOcdError('reg parse failed: %r' % line)

    def bp_set(self, addr, size=2):
        self.cmd('bp 0x%x %d hw' % (addr, size))

    def bp_clear(self, addr):
        try:
            self.cmd('rbp 0x%x' % addr)
        except OpenOcdError:
            pass

    def halted(self):
        t = self.cmd('targets').lower()
        return 'halted' in t


def spawn(cfg=CFG, extra_args=None):
    """Start OpenOCD as a child process and connect to its TCL port."""
    args = ['openocd', '-f', cfg,
            '-c', 'gdb_port disabled',
            '-c', 'telnet_port disabled']
    if extra_args:
        args.extend(extra_args)
    # PIPE without a reader deadlocks once OpenOCD prints DAP errors
    # (exactly what happens across a PLL source switch).
    log = open('/tmp/openocd-spawn.log', 'ab', buffering=0)
    proc = subprocess.Popen(
        args,
        stdout=log,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
    )
    proc._log = log
    ocd = OpenOcd(proc=proc)
    try:
        ocd.connect(timeout=10.0)
        try:
            ocd.cmd('init')
        except OpenOcdError:
            pass
        try:
            ocd.cmd('adapter speed 1000')
        except OpenOcdError:
            pass
        return ocd
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass
        raise


def connect_or_spawn(cfg=CFG):
    """Attach to an existing OpenOCD, otherwise start one."""
    existing = OpenOcd()
    try:
        existing.connect(timeout=0.4)
        return existing
    except OpenOcdError:
        pass
    if not os.path.isfile(cfg):
        sys.exit('OpenOCD config missing: %s' % cfg)
    print('Starting OpenOCD with %s ...' % cfg, flush=True)
    return spawn(cfg)
