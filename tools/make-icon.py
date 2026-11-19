"""Regenerate src/app.ico (headphone glyph on indigo->violet rounded square).
Usage: python tools/make-icon.py [preview_dir]   (requires Pillow)"""
import io, os, struct, sys
from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SS = 8  # supersample factor
# per-size tuning: (band stroke, band half-width, band top, cup w, cup h, cup top, cup inset) as fractions of size
TUNE = {
    "small": dict(stroke=.14, hw=.30, top=.17, cw=.20, ch=.34, ctop=.50, rad=.22),
    "mid":   dict(stroke=.105, hw=.30, top=.17, cw=.17, ch=.31, ctop=.50, rad=.22),
    "big":   dict(stroke=.085, hw=.30, top=.17, cw=.155, ch=.30, ctop=.50, rad=.22),
}

def tune(n):
    return TUNE["small" if n <= 24 else "mid" if n <= 40 else "big"]

def render(n):
    s = n * SS
    t = tune(n)
    # gradient #4F46E5 -> #7C3AED (diagonal)
    a, b = (0x4F, 0x46, 0xE5), (0x7C, 0x3A, 0xED)
    g = Image.linear_gradient("L").resize((s, s))  # top->bottom
    gx = Image.linear_gradient("L").rotate(90).resize((s, s))
    m = Image.blend(g, gx, .5)
    bg = Image.merge("RGB", [Image.eval(m, lambda v, i=i: round(a[i] + (b[i] - a[i]) * v / 255)) for i in range(3)])
    mask = Image.new("L", (s, s), 0)
    pad = 0 if n <= 24 else s * .02
    ImageDraw.Draw(mask).rounded_rectangle([pad, pad, s - 1 - pad, s - 1 - pad], radius=s * t["rad"], fill=255)
    img = bg.convert("RGBA")
    img.putalpha(mask)

    gl = Image.new("L", (s, s), 0)
    d = ImageDraw.Draw(gl)
    cx = s / 2
    w = t["stroke"] * s
    hw = t["hw"] * s
    top = t["top"] * s
    ctop = t["ctop"] * s
    cw, ch = t["cw"] * s, t["ch"] * s
    # headband: semicircle-ish arc, outer box width 2*(hw+w/2)
    ro = hw + w / 2
    d.pieslice([cx - ro, top, cx + ro, top + 2 * ro], 180, 360, fill=255)
    ri = hw - w / 2
    d.pieslice([cx - ri, top + w, cx + ri, top + w + 2 * ri], 180, 360, fill=0)
    # band legs down to cup top
    arc_mid_y = top + ro
    for sx in (-1, 1):
        x0 = cx + sx * hw - w / 2
        d.rectangle([x0, arc_mid_y - 1, x0 + w, ctop + ch * .25], fill=255)
    # ear cups (rounded rects), centred under band legs
    r = cw * .55
    for sx in (-1, 1):
        x = cx + sx * hw
        d.rounded_rectangle([x - cw * .5 - (cw * .15 if sx < 0 else -cw * .15) * 0 - w * .15 * sx * -1 * 0, ctop, x + cw * .5, ctop + ch], radius=r, fill=255)
    gl = gl.resize((n, n), Image.LANCZOS)
    big = img.resize((n, n), Image.LANCZOS)
    white = Image.new("RGBA", (n, n), (255, 255, 255, 255))
    white.putalpha(gl)
    out = Image.alpha_composite(big, white)
    return out

def build_ico(images, path):
    blobs = []
    for im in images:
        n = im.width
        if n >= 256:
            buf = io.BytesIO(); im.save(buf, "PNG"); blobs.append(buf.getvalue()); continue
        # BMP DIB: BGRA bottom-up + 1bpp AND mask
        hdr = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
        px = im.tobytes("raw", "BGRA")
        rows = b"".join(px[y * n * 4:(y + 1) * n * 4] for y in range(n - 1, -1, -1))
        mrow = ((n + 31) // 32) * 4
        blobs.append(hdr + rows + b"\0" * (mrow * n))
    out = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    for im, blob in zip(images, blobs):
        n = im.width
        out += struct.pack("<BBBBHHII", 0 if n >= 256 else n, 0 if n >= 256 else n, 0, 0, 1, 32, len(blob), off)
        off += len(blob)
    with open(path, "wb") as f:
        f.write(out + b"".join(blobs))

if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    imgs = [render(n) for n in SIZES]
    build_ico(imgs, os.path.join(root, "src", "app.ico"))
    if len(sys.argv) > 1:
        os.makedirs(sys.argv[1], exist_ok=True)
        for im in imgs:
            im.save(os.path.join(sys.argv[1], f"icon_{im.width}.png"))
        sheet = Image.new("RGBA", (sum(i.width for i in imgs[:-1]) + 20 * 8, 280), (40, 40, 40, 255))
        x = 10
        for im in imgs[:-1]:
            sheet.alpha_composite(im, (x, 10)); x += im.width + 10
        z = Image.new("RGBA", (256 * 1, 256), (0, 0, 0, 0))
        sheet.save(os.path.join(sys.argv[1], "sheet.png"))
        # zoomed 16/20/24/32 at 8x nearest
        zs = [im.resize((im.width * 8, im.height * 8), Image.NEAREST) for im in imgs[:4]]
        zsheet = Image.new("RGBA", (sum(i.width for i in zs) + 50, 280), (40, 40, 40, 255))
        x = 10
        for im in zs:
            zsheet.alpha_composite(im, (x, 10)); x += im.width + 10
        zsheet.save(os.path.join(sys.argv[1], "zoom_small.png"))
