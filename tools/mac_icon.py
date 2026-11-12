#!/usr/bin/env python3
"""mac_icon.py - the classic Mac application icon from the Windows one.

    mac_icon.py ICON.ico|ICON32.png [ICON16.png] > platform/carbon/icon.r

Writes Rez source for the Finder's icon family and the bundle that binds it to the application (creator MW2P):
ICN# / icl4 / icl8 (32 x 32: black-and-white + mask, the Mac's 16 colours, the Mac's 256 colours) and ics# / ics4 / ics8
(16 x 16), BNDL 128, FREF 128 and the 'MW2P' owner resource. The 32 x 32 image comes from the .ico's 32 x 32 entry (or
the PNG); the 16 x 16 from the .ico's 16 x 16 entry, a second PNG, or the 32 x 32 halved. Colours map to the nearest
entry of the Mac's standard palettes (the 4-bit one for icl4/ics4, the 256-colour system palette for icl8/ics8);
the mask is every pixel the icon's alpha (or the .ico's AND mask) leaves opaque. Needs Pillow."""
import io, struct, sys
from PIL import Image

CREATOR = b'MW2P'

def mac_palette8():
    """The Mac's 256-colour system palette (clut 8): the 6x6x6 cube from white to just above black, ramps of red,
    green, blue and grey without the cube's values, black."""
    pal, lv = [], [0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00]
    for r in lv:
        for g in lv:
            for b in lv:
                pal.append((r, g, b))
    pal = pal[:215]
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    pal += [(v, 0, 0) for v in ramp] + [(0, v, 0) for v in ramp] + [(0, 0, v) for v in ramp] + [(v, v, v) for v in ramp]
    pal.append((0, 0, 0))
    return pal

MAC4 = [(0xFF, 0xFF, 0xFF), (0xFC, 0xF3, 0x05), (0xFF, 0x64, 0x02), (0xDD, 0x08, 0x06), (0xF2, 0x08, 0x84), (0x46, 0x00, 0xA5),
        (0x00, 0x00, 0xD4), (0x02, 0xAB, 0xEA), (0x1F, 0xB7, 0x14), (0x00, 0x64, 0x11), (0x56, 0x2C, 0x05), (0x90, 0x71, 0x3A),
        (0xC0, 0xC0, 0xC0), (0x80, 0x80, 0x80), (0x40, 0x40, 0x40), (0x00, 0x00, 0x00)]
MAC8 = mac_palette8()

def nearest(c, pal):
    r, g, b = c
    return min(range(len(pal)), key=lambda i: (pal[i][0] - r) ** 2 * 3 + (pal[i][1] - g) ** 2 * 4 + (pal[i][2] - b) ** 2 * 2)

def ico_images(data):
    """{size: RGBA image} from a .ico (BMP entries with their AND masks, or PNG entries)"""
    out = {}
    n = struct.unpack('<H', data[4:6])[0]
    for i in range(n):
        w, h, cc, _, planes, bpp, size, off = struct.unpack('<BBBBHHII', data[6 + 16 * i:22 + 16 * i])
        w = w or 256; h = h or 256
        blob = data[off:off + size]
        if blob[:8] == b'\x89PNG\r\n\x1a\n':
            im = Image.open(io.BytesIO(blob)).convert('RGBA')
        else:   # a BMP without its file header: XOR image of height 2h, then the 1-bit AND mask
            hdr = struct.unpack('<IiiHHIIiiII', blob[:40])
            bits = hdr[4]
            ncol = hdr[9] or (1 << bits if bits <= 8 else 0)
            pal_off = hdr[0]
            pal = [tuple(blob[pal_off + 4 * k:pal_off + 4 * k + 3][::-1]) for k in range(ncol)]
            px_off = pal_off + 4 * ncol
            stride = ((w * bits + 31) // 32) * 4
            mstride = ((w + 31) // 32) * 4
            moff = px_off + stride * h
            im = Image.new('RGBA', (w, h))
            for y in range(h):
                row = blob[px_off + (h - 1 - y) * stride:]
                mrow = blob[moff + (h - 1 - y) * mstride:]
                for x in range(w):
                    if bits == 4: idx = (row[x >> 1] >> (4 if x % 2 == 0 else 0)) & 15; c = pal[idx]; a = 255
                    elif bits == 8: c = pal[row[x]]; a = 255
                    elif bits == 1: c = pal[(row[x >> 3] >> (7 - x % 8)) & 1]; a = 255
                    elif bits == 24: c = tuple(row[3 * x:3 * x + 3][::-1]); a = 255
                    elif bits == 32: c = tuple(row[4 * x:4 * x + 3][::-1]); a = row[4 * x + 3]
                    else: raise SystemExit('unsupported .ico depth %d' % bits)
                    if bits != 32 and (mrow[x >> 3] >> (7 - x % 8)) & 1: a = 0
                    im.putpixel((x, y), c + (a,))
        if w not in out or bpp > out[w][1]: out[w] = (im, bpp)
    return {k: v[0] for k, v in out.items()}

def bits1(im, fn):
    """packed 1-bit rows (MSB first) of fn(rgba)"""
    w, h = im.size; out = bytearray()
    for y in range(h):
        for x0 in range(0, w, 8):
            v = 0
            for k in range(8): v = v << 1 | (1 if fn(im.getpixel((x0 + k, y))) else 0)
            out.append(v)
    return bytes(out)

def family(im):
    """(icn#, icl4, icl8) bytes of one size: the black-and-white icon is the darker half of the opaque pixels"""
    opaque = lambda p: p[3] >= 128
    w, h = im.size
    # the black-and-white icon: black solid, mid tones a 50% checkerboard, light tones white (the classic 1-bit look)
    bwi = bytearray()
    for y in range(h):
        for x0 in range(0, w, 8):
            v = 0
            for k in range(8):
                p = im.getpixel((x0 + k, y)); x = x0 + k
                lum = (p[0] * 3 + p[1] * 6 + p[2]) / 10
                on = p[3] >= 128 and (lum < 40 or (lum < 150 and (x + y) % 2 == 0))
                v = v << 1 | (1 if on else 0)
            bwi.append(v)
    bw = bytes(bwi) + bits1(im, opaque)
    c4, c8 = bytearray(), bytearray()
    for y in range(h):
        for x in range(0, w, 2):
            p0, p1 = im.getpixel((x, y)), im.getpixel((x + 1, y))
            i0 = nearest(p0[:3], MAC4) if p0[3] >= 128 else 0
            i1 = nearest(p1[:3], MAC4) if p1[3] >= 128 else 0
            c4.append(i0 << 4 | i1)
        for x in range(w):
            p = im.getpixel((x, y))
            c8.append(nearest(p[:3], MAC8) if p[3] >= 128 else 0)
    return bw, bytes(c4), bytes(c8)

def rez(kind, rid, data, name=''):
    hexs = data.hex().upper()
    lines = ['    $"%s"' % hexs[i:i + 64] for i in range(0, len(hexs), 64)]
    return 'data \'%s\' (%d%s) {\n%s\n};\n' % (kind, rid, (', "%s"' % name) if name else '', '\n'.join(lines))

def main():
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    src = sys.argv[1]
    if src.lower().endswith('.ico'):
        imgs = ico_images(open(src, 'rb').read())
        if 32 not in imgs: raise SystemExit('the .ico has no 32 x 32 image (has %s)' % sorted(imgs))
        i32 = imgs[32]; i16 = imgs.get(16)
    else:
        i32 = Image.open(src).convert('RGBA')
        if i32.size != (32, 32): i32 = i32.resize((32, 32), Image.LANCZOS)
        i16 = None
    if len(sys.argv) > 2: i16 = Image.open(sys.argv[2]).convert('RGBA')
    if i16 is None: i16 = i32.resize((16, 16), Image.LANCZOS)
    if i16.size != (16, 16): i16 = i16.resize((16, 16), Image.LANCZOS)
    l_bw, l4, l8 = family(i32)
    s_bw, s4, s8 = family(i16)
    out = ['/* icon.r - the application icon (tools/mac_icon.py from %s): the Finder icon family + the bundle */\n' % src.split('/')[-1]]
    out += [rez('ICN#', 128, l_bw), rez('icl4', 128, l4), rez('icl8', 128, l8),
            rez('ics#', 128, s_bw), rez('ics4', 128, s4), rez('ics8', 128, s8)]
    # BNDL: signature, signature resource 0, 2 types - ICN# (local 0 -> 128) and FREF (local 0 -> 128)
    bndl = CREATOR + struct.pack('>hh', 0, 1) + b'ICN#' + struct.pack('>hhh', 0, 0, 128) + b'FREF' + struct.pack('>hhh', 0, 0, 128)
    fref = b'APPL' + struct.pack('>h', 0) + b'\x00'
    out += [rez('BNDL', 128, bndl), rez('FREF', 128, fref), rez(CREATOR.decode(), 0, b'\x00', 'Owner signature')]
    sys.stdout.write('\n'.join(out))

if __name__ == '__main__':
    main()
