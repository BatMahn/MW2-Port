#!/usr/bin/env python3
"""NOTE (2026-10-20): packaging/macos/mw2port.icns is now the user-supplied MechWarrior-2.icns - do not regenerate it with
this script unless asked. Formerly: build the macOS app icon (packaging/macos/mw2port.icns) from the original MechWarrior 2 box art
(packaging/macos/mw2-boxart.jpg): the cover inset in a gunmetal macOS-style rounded square, so the rounded corners
don't clip the MECHWARRIOR 2 title. Developer tool only (needs Pillow); the build uses the committed .icns.
The Windows icon is separate: packaging/windows/mw2port.ico (the original game icon)."""
import io, struct, sys
from PIL import Image, ImageDraw, ImageFilter

S, BODY, RADIUS = 1024, 824, 185     # Apple's icon grid: 824 px rounded square on a 1024 canvas
INSET, ART_RADIUS = 58, 14           # inset >= RADIUS * (1 - 1/sqrt 2) keeps the art's corners inside the curve

def icon(boxart):
    off = (S - BODY) // 2
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # drop shadow
    shm = Image.new("L", (S, S), 0)
    ImageDraw.Draw(shm).rounded_rectangle([off, off + 12, off + BODY, off + BODY + 12], RADIUS, fill=110)
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0)); sh.putalpha(shm.filter(ImageFilter.GaussianBlur(14)))
    out = Image.alpha_composite(out, sh)
    # gunmetal frame
    frame = Image.new("RGBA", (BODY, BODY)); d = ImageDraw.Draw(frame)
    for y in range(BODY):
        c = int(78 - 38 * y / BODY)
        d.line([(0, y), (BODY, y)], fill=(c, c + 2, c + 6, 255))
    m = Image.new("L", (BODY, BODY), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, BODY - 1, BODY - 1], RADIUS, fill=255)
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0)); layer.paste(frame, (off, off), m)
    out = Image.alpha_composite(out, layer)
    # the cover, with a dark keyline
    a = BODY - 2 * INSET
    art = boxart.convert("RGB").resize((a, a), Image.LANCZOS)
    art = art.filter(ImageFilter.UnsharpMask(radius=2, percent=60, threshold=2)).convert("RGBA")
    ring = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(ring).rounded_rectangle([off + INSET - 3, off + INSET - 3, off + INSET + a + 2, off + INSET + a + 2],
                                           ART_RADIUS + 3, fill=(0, 0, 0, 200))
    out = Image.alpha_composite(out, ring)
    am = Image.new("L", (a, a), 0)
    ImageDraw.Draw(am).rounded_rectangle([0, 0, a - 1, a - 1], ART_RADIUS, fill=255)
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0)); layer.paste(art, (off + INSET, off + INSET), am)
    out = Image.alpha_composite(out, layer)
    # faint edge highlight
    edge = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle([off, off, off + BODY - 1, off + BODY - 1], RADIUS,
                                           outline=(255, 255, 255, 55), width=3)
    return Image.alpha_composite(out, edge)

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
    im = icon(Image.open(root + "/packaging/macos/mw2-boxart.jpg"))
    open(root + "/packaging/macos/mw2port.icns", "wb").write(icns(im))
    print("wrote packaging/macos/mw2port.icns")
