"""Read an ARMSX1 deep trace (deeptrace-current.bin) and rebuild guest RAM at each snapshot.

usage:
  read_trace.py trace.bin summary                 list snapshots: field, pc, pad, changed words
  read_trace.py trace.bin dump OUTDIR              write ram-<field>.bin for every snapshot
  read_trace.py trace.bin watch ADDR [ADDR...]     print 32-bit values at addresses (hex, KSEG0) per snapshot
  read_trace.py trace.bin counters FROM TO         words that change at every snapshot between fields FROM..TO
"""
import struct, sys, zlib

RAM = 2 * 1024 * 1024


def records(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'ARMSXDT1', 'not a deep trace'
    at, ram = 8, None
    while at + 4 <= len(data):
        try:
            kind, field, clocks, pc = struct.unpack_from('<iqqi', data, at); at += 24
            regs = struct.unpack_from('<32i', data, at); at += 128
            pad, = struct.unpack_from('<i', data, at); at += 4
            if kind == 1:
                n, = struct.unpack_from('<i', data, at); at += 4
                ram = bytearray(zlib.decompress(data[at:at + n])); at += n; changed = RAM // 4
            else:
                count, = struct.unpack_from('<i', data, at); at += 4
                pairs = struct.unpack_from('<%di' % (count * 2), data, at); at += count * 8
                for i in range(0, len(pairs), 2): struct.pack_into('<i', ram, pairs[i] * 4, pairs[i + 1])
                changed = count
        except (struct.error, zlib.error):
            break  # a record still being written at pull time
        if ram is not None: yield field, clocks, pc & 0xFFFFFFFF, regs, pad, bytes(ram), changed


def main():
    path, cmd = sys.argv[1], sys.argv[2]
    if cmd == 'summary':
        for field, clocks, pc, regs, pad, ram, changed in records(path):
            print('field %8d  pc %08X  ra %08X  pad %04X  changed words %d' % (field, pc, regs[31] & 0xFFFFFFFF, pad, changed))
    elif cmd == 'dump':
        import os; os.makedirs(sys.argv[3], exist_ok=True)
        for field, *_rest, ram, _c in records(path): open('%s/ram-%08d.bin' % (sys.argv[3], field), 'wb').write(ram)
    elif cmd == 'watch':
        addrs = [int(a, 16) for a in sys.argv[3:]]
        for field, clocks, pc, regs, pad, ram, changed in records(path):
            print('%8d pad %04X  ' % (field, pad) + '  '.join('%08X=%08X' % (a, struct.unpack_from('<I', ram, a & 0x1FFFFF)[0]) for a in addrs))
    elif cmd == 'counters':
        lo, hi = int(sys.argv[3]), int(sys.argv[4]); prev = None; always = None
        for field, *_r, ram, _c in records(path):
            if not lo <= field <= hi: continue
            if prev is not None:
                diff = {i for i in range(0, RAM, 4) if ram[i:i + 4] != prev[i:i + 4]}
                always = diff if always is None else always & diff
            prev = ram
        for i in sorted(always or []): print('%08X %d' % (0x80000000 + i, struct.unpack_from('<i', prev, i)[0]))


main()
