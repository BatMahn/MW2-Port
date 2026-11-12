#!/usr/bin/env python3
"""macbin_bundle.py FILE.bin - set the Finder's "has bundle" flag in a MacBinary file (and clear "inited", so the Finder
reads the bundle afresh), fixing the MacBinary II header CRC. Retro68 writes the Finder flags as 0, and without the
bundle flag the Mac OS 9 Finder shows the generic application icon instead of the one in the resources."""
import sys

def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc

def main(path):
    d = bytearray(open(path, 'rb').read())
    if len(d) < 128 or d[0] != 0 or not 1 <= d[1] <= 63:
        raise SystemExit('%s: not a MacBinary file' % path)
    flags = d[73]
    flags |= 0x20                      # kHasBundle (0x2000 of fdFlags)
    flags &= ~0x01 & 0xFF              # kHasBeenInited (0x0100): let the Finder pick the bundle up
    d[73] = flags
    if d[122] >= 129:                  # MacBinary II / III: the header's CRC over bytes 0-123
        crc = crc16_xmodem(bytes(d[:124]))
        d[124] = crc >> 8; d[125] = crc & 0xFF
    open(path, 'wb').write(d)
    print('%s: has bundle' % path)

if __name__ == '__main__':
    for p in sys.argv[1:]: main(p)
