"""Generate src/ui_logo.h / src/ui_logo.cpp — the FUJIFILM wordmark as an RGB565 bitmap.

The paths are copied verbatim from the header <svg> in
new_ui_try/5_fujifilm_concept_B_2026-09-22.html (Concept B, design authority since 2026-09-23).
The handoff says not to approximate the logo with a font, so it is rasterised from those paths:
8x supersampled with Pillow, box-filtered down to the displayed size, and blended onto the header
background here, since the panel has no alpha. Lettering white, the I's upper segment #ED174C.

Placement, as measured in headless Chrome: the <svg> box is 100 x 17 at (12, 10.5) with
preserveAspectRatio "xMidYMid meet". The half pixel is baked into the bitmap, so it is 100 x 18 and
drawn at (12, 10).

A second bitmap, UI_LOGO_SLEEP, is the same paths at 200 x 34 blended onto BLACK at 35 % brightness
(red segment dimmed alike): the idle sleep screen shows it drifting to a new spot every minute
(Dough, 2026-09-23). Dimmed so a sleeping panel is not the brightest thing in the room; separate
from UI_LOGO because that one is pre-blended onto the header green and would show as a green box.

Run from anywhere:
    python Coding/Firmware/tools/make_ui_logo.py            # write the two source files
    python Coding/Firmware/tools/make_ui_logo.py --check D  # also write an 8x preview PNG into D
"""
import pathlib
import re
import sys

from PIL import Image, ImageDraw

WHITE_PATH = ("M20 322V38Q20 0 60 0H238V34H134Q107 34 107 61V137H233V172H107V303Q107 322 88 322Z"
              "M285 15Q285 0 302 0H367V246Q367 300 422 300Q479 300 479 246V19Q479 0 499 0H560V239"
              "Q560 336 422 336Q285 336 285 239ZM713 19Q713 0 733 0H795V230Q795 337 643 337Q610 337 594 334"
              "V301Q626 303 649 300Q713 296 713 242ZM871 177H954V304Q954 324 934 324H871ZM1038 177H980"
              "L1038 130V38Q1038 0 1078 0H1256V35H1152Q1125 35 1125 63V139H1249V174H1123V304Q1123 324 1103 324"
              "H1038ZM1305 20Q1305 0 1325 0H1389V306Q1389 326 1369 326H1305ZM1466 20Q1466 0 1486 0H1549V261"
              "Q1549 290 1576 290H1661V326H1510Q1466 326 1466 281ZM1700 326V20Q1700 0 1720 0H1794L1847 249"
              "L1900 31Q1907 0 1938 0H2028V303Q2028 325 2006 325H1947V61H1942L1880 305Q1875 326 1854 326"
              "H1811L1743 62H1739V326Z")
RED_PATH = "M873 176V19Q873 0 892 0H954V105Q954 110 950 113Z"

VIEWBOX = (20, 0, 2008, 338)       # x, y, w, h
SS = 8                             # supersampling factor

WHITE = (255, 255, 255)
RED = (0xED, 0x17, 0x4C)
SLEEP_LEVEL = 0.35                 # sleep-screen brightness

# (C name, CSS box w, h, y fraction baked in, bitmap w, h, background, white, red, what)
LOGOS = [
    ("UI_LOGO", 100, 17, 0.5, 100, 18, (0x10, 0x2A, 0x27), WHITE, RED, "header, on #102A27"),
    ("UI_LOGO_SLEEP", 200, 34, 0.0, 200, 34, (0, 0, 0),
     tuple(round(c * SLEEP_LEVEL) for c in WHITE), tuple(round(c * SLEEP_LEVEL) for c in RED),
     "sleep screen, on black at 35 %"),
]

OUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "src"


def subpaths(d):
    """Absolute M/L/H/V/Q/Z only — all these paths use. Returns lists of (x, y) points."""
    tokens = re.findall(r"[MLHVQZ]|-?\d+(?:\.\d+)?", d)
    out, cur, pos, i, cmd = [], [], (0.0, 0.0), 0, None
    while i < len(tokens):
        t = tokens[i]
        if t.isalpha():
            cmd = t
            i += 1
            if cmd == "Z":
                if cur:
                    out.append(cur)
                cur = []
            continue
        if cmd == "M":
            pos = (float(tokens[i]), float(tokens[i + 1])); i += 2
            cur = [pos]
            cmd = "L"          # implicit lineto after the first pair
        elif cmd == "L":
            pos = (float(tokens[i]), float(tokens[i + 1])); i += 2
            cur.append(pos)
        elif cmd == "H":
            pos = (float(tokens[i]), pos[1]); i += 1
            cur.append(pos)
        elif cmd == "V":
            pos = (pos[0], float(tokens[i])); i += 1
            cur.append(pos)
        elif cmd == "Q":
            c = (float(tokens[i]), float(tokens[i + 1]))
            e = (float(tokens[i + 2]), float(tokens[i + 3])); i += 4
            p0 = pos
            for k in range(1, 17):
                u = k / 16
                cur.append(((1 - u) ** 2 * p0[0] + 2 * (1 - u) * u * c[0] + u * u * e[0],
                            (1 - u) ** 2 * p0[1] + 2 * (1 - u) * u * c[1] + u * u * e[1]))
            pos = e
        else:
            raise ValueError(f"unsupported path command {cmd}")
    if cur:
        out.append(cur)
    return out


def coverage(d, box_w, box_h, y_frac, out_w, out_h):
    vx, vy, vw, vh = VIEWBOX
    scale = min(box_w / vw, box_h / vh)                    # "meet"
    ox = (box_w - vw * scale) / 2
    oy = (box_h - vh * scale) / 2 + y_frac
    img = Image.new("L", (out_w * SS, out_h * SS), 0)
    draw = ImageDraw.Draw(img)
    for pts in subpaths(d):
        draw.polygon([((x - vx) * scale * SS + ox * SS, (y - vy) * scale * SS + oy * SS)
                      for x, y in pts], fill=255)
    return img.resize((out_w, out_h), Image.BOX)


def blend(a, fg, bg):
    return tuple((f * a + b * (255 - a) + 127) // 255 for f, b in zip(fg, bg))


def render(box_w, box_h, y_frac, out_w, out_h, bg, fg_white, fg_red):
    white = coverage(WHITE_PATH, box_w, box_h, y_frac, out_w, out_h)
    red = coverage(RED_PATH, box_w, box_h, y_frac, out_w, out_h)
    rgb = Image.new("RGB", (out_w, out_h))
    px = rgb.load()
    for y in range(out_h):
        for x in range(out_w):
            c = blend(white.getpixel((x, y)), fg_white, bg)
            px[x, y] = blend(red.getpixel((x, y)), fg_red, c)
    return rgb


def main():
    hdr = ["// GENERATED by tools/make_ui_logo.py - do not edit by hand, re-run the script.",
           "// FUJIFILM wordmark from the Concept B HTML's SVG paths, pre-blended (the panel has no alpha).",
           "#pragma once", "", "#include <Arduino.h>", "",
           "// RGB565, row-major. Push with tft.setSwapBytes(true) + pushImage()."]
    cpp = ["// GENERATED by tools/make_ui_logo.py - do not edit by hand, re-run the script.",
           '#include "ui_logo.h"', ""]
    total = 0
    for name, bw, bh, yf, ow, oh, bg, fw, fr, what in LOGOS:
        rgb = render(bw, bh, yf, ow, oh, bg, fw, fr)
        px = rgb.load()
        words = []
        for y in range(oh):
            for x in range(ow):
                r, g, b = px[x, y]
                words.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
        total += len(words) * 2
        hdr += ["", f"// {what}", f"constexpr int16_t {name}_W = {ow};", f"constexpr int16_t {name}_H = {oh};",
                f"extern const uint16_t {name}[];"]
        body = ",".join(f"0x{w:04X}" for w in words)
        cpp.append(f"const uint16_t {name}[] PROGMEM = {{{body}}};")
        if len(sys.argv) >= 3 and sys.argv[1] == "--check":
            out = pathlib.Path(sys.argv[2]) / f"{name.lower()}_check.png"
            rgb.resize((ow * 4, oh * 4), Image.NEAREST).save(out)
            print("check image:", out)
    (OUT_DIR / "ui_logo.h").write_text("\n".join(hdr) + "\n", encoding="utf-8", newline="\n")
    (OUT_DIR / "ui_logo.cpp").write_text("\n".join(cpp) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {OUT_DIR / 'ui_logo.h'} and ui_logo.cpp, {total} bytes")


if __name__ == "__main__":
    main()
