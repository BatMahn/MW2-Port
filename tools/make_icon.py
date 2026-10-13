#!/usr/bin/env python3
"""Draw the port's app icon (an original design: HUD target brackets around a generic walker) and write
packaging/macos/mw2port.icns + packaging/mw2port.png. Developer tool only (needs Pillow); the build uses the
committed files. Nothing here comes from the game's data."""
import io, struct, sys
from PIL import Image, ImageDraw, ImageFilter

S = 1024
G = (64, 224, 96)        # HUD green
def icon():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # macOS-style rounded square with a dark gunmetal gradient
    bg = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(bg)
    for y in range(S):
        t = y / S
        d.line([(0, y), (S, y)], fill=(int(38 - 20 * t), int(46 - 22 * t), int(52 - 24 * t), 255))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([100, 100, S - 100, S - 100], radius=185, fill=255)
    im.paste(bg, (0, 0), mask)
    d = ImageDraw.Draw(im)
    # faint radar rings and grid
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    cx, cy = S // 2, S // 2 + 10
    for r in (330, 230, 130):
        g.ellipse([cx - r, cy - r, cx + r, cy + r], outline=G + (60,), width=6)
    g.line([(cx, 170), (cx, S - 170)], fill=G + (45,), width=5)
    g.line([(170, cy), (S - 170, cy)], fill=G + (45,), width=5)
    glow = Image.composite(glow, Image.new("RGBA", (S, S), (0, 0, 0, 0)), mask)
    im.alpha_composite(glow)
    # generic two-legged walker silhouette (original): box torso, side pods, digitigrade legs
    m = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    w = ImageDraw.Draw(m)
    body = (14, 18, 22, 255)
    w.polygon([(420, 360), (604, 360), (640, 420), (620, 500), (404, 500), (384, 420)], fill=body)   # torso
    w.rectangle([470, 330, 554, 366], fill=body)                                                   # cockpit
    w.polygon([(476, 340), (548, 340), (540, 360), (484, 360)], fill=(200, 60, 40, 255))           # canopy glow
    for sx in (-1, 1):
        x0 = cx + sx * 118
        w.rectangle([min(x0, x0 + sx * 30), 395, max(x0, x0 + sx * 30), 430], fill=body)                 # shoulder
        x1 = x0 + sx * 30
        w.rounded_rectangle([min(x1, x1 + sx * 62), 370, max(x1, x1 + sx * 62), 470], radius=12, fill=body)  # pods
        w.rectangle([min(x1, x1 + sx * 62) + 18, 470, max(x1, x1 + sx * 62) - 18, 500], fill=body)          # barrel
        hip = (cx + sx * 62, 500)
        knee = (cx + sx * 96, 588)
        ank = (cx + sx * 72, 690)
        w.line([hip, knee, ank], fill=body, width=46, joint="curve")
        w.polygon([(ank[0] - 50, 720), (ank[0] + 50, 720), (ank[0] + 30, 690), (ank[0] - 30, 690)], fill=body)
    edge = m.split()[3].filter(ImageFilter.MaxFilter(13))
    rim = Image.new("RGBA", (S, S), G + (255,))
    rim.putalpha(edge.point(lambda a: 150 if a else 0))
    im.alpha_composite(rim)
    im.alpha_composite(m)
    # target brackets (red, locked) and a centre pip
    R = (235, 60, 40, 255)
    a, b, L, T = 245, S - 245, 100, 24
    for (x, y, dx, dy) in ((a, a, 1, 1), (b, a, -1, 1), (a, b, 1, -1), (b, b, -1, -1)):
        d.rectangle([min(x, x + dx * L), min(y, y + dy * T), max(x, x + dx * L), max(y, y + dy * T)], fill=R)
        d.rectangle([min(x, x + dx * T), min(y, y + dy * L), max(x, x + dx * T), max(y, y + dy * L)], fill=R)
    return im

def icns(im):
    chunks = []
    for typ, px in ((b"ic07", 128), (b"ic08", 256), (b"ic09", 512), (b"ic10", 1024),
                    (b"ic11", 32), (b"ic12", 64), (b"ic13", 256), (b"ic14", 512)):
        buf = io.BytesIO()
        im.resize((px, px), Image.LANCZOS).save(buf, "PNG")
        data = buf.getvalue()
        chunks.append(typ + struct.pack(">I", len(data) + 8) + data)
    body = b"".join(chunks)
    return b"icns" + struct.pack(">I", len(body) + 8) + body

if __name__ == "__main__":
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    im = icon()
    im.save(root + "/packaging/mw2port.png")
    open(root + "/packaging/macos/mw2port.icns", "wb").write(icns(im))
    print("wrote packaging/mw2port.png, packaging/macos/mw2port.icns")
