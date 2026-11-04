"""Generate src/ui_fonts.h / src/ui_fonts.cpp — Segoe UI as TFT_eSPI smooth (anti-aliased) fonts.

Each font is a .vlw file image compiled in as a byte array, loaded with tft.loadFont(array).
No LittleFS upload and no change to TFT_eSPI's User_Setup.h (SMOOTH_FONT is already on).

Run from anywhere on the Windows PC (reads C:\\Windows\\Fonts):
    python Coding/Firmware/tools/make_ui_fonts.py            # write the two source files
    python Coding/Firmware/tools/make_ui_fonts.py --check D  # also render a comparison PNG into D

LICENCE — READ BEFORE SHIPPING: Segoe UI and Segoe UI Symbol are Microsoft fonts under the Windows
licence, which does not permit embedding them in another product. Dough chose them knowingly on
2026-09-15. Selawik (Microsoft, SIL OFL) is the drop-in replacement: point FONT_FILES at it and
re-run; nothing else in the firmware changes.

Sizes in SPECS are CSS px from Concept B (5_fujifilm_concept_B_2026-09-22.html); scaled() turns them
into the design authority's since 2026-10-09 (5_fujifilm_concept_B_bigtext_try.html, every size x1.2).
That page is a native 480x320 layout, so 1 CSS px = 1 panel px and the sizes carry over unchanged.

Charsets are kept to what the screens actually draw, to save flash. A character missing from
a font is drawn by TFT_eSPI as a small hollow box — visible, not a crash. Add it here and re-run.

The no-break space U+00A0 is always included with the real space advance. TFT_eSPI ignores a font's
own space and GUESSES its width as (ascent+descent)*2/7 — too wide for Segoe UI — so the firmware's
text helper swaps ' ' for U+00A0 before drawing.
"""
import pathlib
import string
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

FONTS_DIR = pathlib.Path(r"C:\Windows\Fonts")
FONT_FILES = {"L": "segoeuil.ttf", "R": "segoeui.ttf", "SB": "seguisb.ttf", "B": "segoeuib.ttf"}
SYMBOL_FILE = "seguisym.ttf"
# Not in Segoe UI, so taken from Segoe UI Symbol the way a browser falls back:
# check mark, menu bars, close x, backspace, music note (menu bars / close x: unused since stage 2).
SYMBOLS = {"\u2713", "\u2630", "\u2715", "\u232b", "\u266a"}

OUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "src"

ASCII = "".join(chr(c) for c in range(0x21, 0x7F))
UPPER = string.ascii_uppercase

# (C name, weight, px, charset, what uses it) — weights: CSS 400/500 -> R, 600/650 -> SB, 700/750 -> B
NAME_CHARS = string.ascii_letters + string.digits + "'…"   # names: keyboard + seeds, ellipsis

SPECS = [
    # Concept B (new_ui_try/5_fujifilm_concept_B_2026-09-22.html), every screen, sizes read off
    # headless Chrome's computed styles. ALL BOLD, whatever the HTML's weight (Dough 2026-09-23,
    # "i want the text to bold"; one step heavier - SemiBold - was tried first and looked the
    # same on the panel at 9-12 px). The C names of the stage-1 fonts keep the HTML weight
    # (UIB_R9 is Bold now) so ui_fuji.cpp did not change; the second column is the truth.
    # The white UIF_* fonts went with stage 2 of the port (2026-09-23), as did UIB_SB12, which
    # had become a byte-identical copy of UIB_R12 once everything was Bold, and UIB_B11 when the
    # card "Slot N" moved up to 13 px (UIB_R13).
    ("UIB_R9",        "B",   9, ASCII + "·…",     "AVAILABLE/COMPLETE, initials, card meta line"),
    ("UIB_R10",       "B",  10, ASCII + "·♪›…°", "/ SERVICE, heat status, done hint, alert strip, multi detail, log labels, notes"),
    ("UIB_R11",       "B",  11, ASCII + "°·×–—→›♪…", "Target, captions, page tags, footers, hints, notes"),
    ("UIB_R12",       "B",  12, ASCII + "…·♪✓›‹°’⌫→", "subtitle, card names, buttons, row values, keyboard"),
    ("UIB_B12",       "B",  12, UPPER + "°●○", "HEAT ON/OFF badge, temperature unit"),
    ("UIB_SB23",      "B", 23, "0123456789:Ready✓", "card countdown, Ready check"),
    ("UIB_B28",       "B",  28, "0123456789.-Curecomplets*OVERTMPAUSNFL", "header temperature, completion heading, footer * key, OVER-TEMPERATURE, SENSOR FAULT"),
    ("UIB_X18",       "B",  18, "×",            "card cancel x"),
    ("UIB_X22",       "B",  22, "×",            "completion card x"),
    ("UIB_PLUS24",    "B",  24, "+",                  "empty-card plus (CSS weight 300)"),
    ("UIB_R13",       "B",  13, ASCII + "·♪…", "completion slot line (multi), slot chooser, user rows, Start #"),
    ("UIB_R14",       "B",  14, ASCII + "·♪…", "completion detail (single), list rows, position intro"),
    ("UIB_R20",       "B",  20, ASCII + "·…‹⌫", "completion slot line (single), keypad digits, back"),
    ("UIB_B16",       "B",  16, ASCII + "·…",     "page detail line, typed name"),
    ("UIB_B17",       "B",  17, "LensRBSTubeCapHandle cover", "curing position buttons"),
    ("UIB_B18",       "B",  18, ASCII + "°·…",   "completion name (multi), page titles, log values"),
    ("UIB_B22",       "B",  22, "0123456789: remaing",  "cancel screen countdown"),
    ("UIB_B23",       "B",  23, "Mat is OFF on timer Off in 0123456789:·N", "heating mat state"),
    ("UIB_B30",       "B",  30, "0123456789—",       "numeric entry value"),
    ("UIB_B32",       "B",  32, NAME_CHARS,           "completion name (single)"),
]


def glyph_record(font, ch):
    """(unicode, w, h, xAdvance, dY, dX, bitmap bytes) the way TFT_eSPI's loadMetrics() reads it."""
    if ch == "\u00a0":
        return (0xA0, 0, 0, round(font.getlength(" ")), 0, 0, b"")
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")  # relative to the baseline origin
    w, h = max(0, x1 - x0), max(0, y1 - y0)
    img = Image.new("L", (max(w, 1), max(h, 1)), 0)
    ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
    data = img.tobytes()[: w * h] if w and h else b""
    # dY is distance from baseline UP to the bitmap top; y0 is negative above the baseline.
    return (ord(ch), w, h, round(font.getlength(ch)), -y0, x0, data)


# Text-size trial adopted 2026-10-09: every size below 28 px is 1.2x the Concept B value,
# exactly as new_ui_try/5_fujifilm_concept_B_bigtext_try.html does (round(px * 1.2)). The C names
# and the px in SPECS stay the ORIGINAL sizes so ui_fuji.cpp's font names did not change - the
# generated ui_fonts.h comments show the real size.
def scaled(px):
    return px if px >= 28 else round(px * 1.2)


def build_vlw(weight, px, charset):
    px = scaled(px)
    base = ImageFont.truetype(str(FONTS_DIR / FONT_FILES[weight]), px)
    sym = ImageFont.truetype(str(FONTS_DIR / SYMBOL_FILE), px)
    ascent, descent = base.getmetrics()
    chars = sorted(set(charset) | {"\u00a0"}, key=ord)
    glyphs = [glyph_record(sym if ch in SYMBOLS else base, ch) for ch in chars]
    for g in glyphs:
        assert g[1] <= 255 and g[2] <= 255 and -128 <= g[5] <= 127, g
    out = bytearray(struct.pack(">6i", len(glyphs), 11, px, 0, ascent, descent))
    for code, w, h, adv, dy, dx, _ in glyphs:
        out += struct.pack(">7i", code, h, w, adv, dy, dx, 0)
    for g in glyphs:
        out += g[6]
    return bytes(out), ascent, descent


# ---- self-check: decode the array exactly as TFT_eSPI does and draw it ---------------------------

def parse_vlw(buf):
    count, _, _, _, ascent, descent = struct.unpack_from(">6i", buf, 0)
    ptr, bmp = 24, 24 + 28 * count
    glyphs = {}
    for _ in range(count):
        code, h, w, adv, dy, dx, _pad = struct.unpack_from(">7i", buf, ptr)
        glyphs[code] = (w, h, adv, dy, dx, bmp)
        ptr += 28
        bmp += w * h
    return ascent, descent, glyphs


def tft_draw(img, buf, text, x, baseline, fg):
    """Mirror of drawString(L_BASELINE) + drawGlyph(): cursor_y = baseline - maxAscent, glyph top =
    cursor_y + maxAscent - dY. Blends onto whatever is already in img (the firmware blends onto the
    text background colour, which is the same thing on a solid fill)."""
    ascent, _, glyphs = parse_vlw(buf)
    cursor_y = baseline - ascent
    px = img.load()
    for ch in text.replace(" ", "\u00a0"):
        w, h, adv, dy, dx, off = glyphs[ord(ch)]
        top = cursor_y + ascent - dy
        for yy in range(h):
            for xx in range(w):
                a = buf[off + yy * w + xx]
                if a:
                    bx, by = x + dx + xx, top + yy
                    r, g, b = px[bx, by]
                    px[bx, by] = tuple((fc * a + bc * (255 - a)) // 255 for fc, bc in zip(fg, (r, g, b)))
        x += adv


def check(arrays, out_png):
    samples = [
        ("UIB_R10", "B", 10, "/ SERVICE  Off in 1:02:03  ♪ Bell · Restart ›"),
        ("UIB_R12", "B", 12, "Endoscope curing station  User B"),
        ("UIB_B12", "B", 12, "● HEAT ON ○ HEAT OFF °C"),
        ("UIB_B28", "B", 28, "44.8 Cure complete"), ("UIB_SB23", "B", 23, "8:00:00 Ready ✓"),
        ("UIB_B32", "B", 32, "User C Dough"), ("UIB_R9", "B", 9, "AVAILABLE · Handle cover · 105 min"),
        ("UIB_R12", "B", 12, "♪ Chime ✓ In use Select › 45°C ’ ⌫ Next →"),
        ("UIB_B18", "B", 18, "Station settings 44.8°"), ("UIB_B30", "B", 30, "105 —"),
        ("UIB_R20", "B", 20, "‹ 7 8 9 ⌫"), ("UIB_B23", "B", 23, "Mat is ON · no off timer"),
    ]
    img = Image.new("RGB", (560, 40 * len(samples)), (255, 255, 255))
    draw = ImageDraw.Draw(img)
    for i, (name, weight, px, text) in enumerate(samples):
        base = 32 + i * 40
        tft_draw(img, arrays[name], text, 8, base, (23, 70, 55))           # left: decoded array
        font = ImageFont.truetype(str(FONTS_DIR / FONT_FILES[weight]), scaled(px))  # right: Pillow reference
        draw.text((268, base), text.replace("\u2713", "").replace("\u2630", ""), font=font,
                  fill=(23, 70, 55), anchor="ls")
    img.save(out_png)
    print("check image:", out_png)


def main():
    arrays, total = {}, 0
    hdr = [
        "// GENERATED by tools/make_ui_fonts.py - do not edit by hand, re-run the script.",
        "// Segoe UI smooth fonts for the Concept B UI (see the script for the licence note).",
        "#pragma once", "", "#include <Arduino.h>", "",
    ]
    cpp = ["// GENERATED by tools/make_ui_fonts.py - do not edit by hand, re-run the script.",
           '#include "ui_fonts.h"', ""]
    for name, weight, px, charset, use in SPECS:
        data, asc, desc = build_vlw(weight, px, charset)
        arrays[name] = data
        total += len(data)
        hdr.append(f"extern const uint8_t {name}[];  // {weight} {scaled(px)}px (was {px}), asc {asc} desc {desc}, "
                   f"{len(data)} B - {use}")
        body = ",".join(str(b) for b in data)
        cpp.append(f"const uint8_t {name}[] PROGMEM = {{{body}}};")
    hdr += ["", f"// Total font data: {total} bytes of flash.", ""]
    (OUT_DIR / "ui_fonts.h").write_text("\n".join(hdr), encoding="utf-8", newline="\n")
    (OUT_DIR / "ui_fonts.cpp").write_text("\n".join(cpp) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {OUT_DIR / 'ui_fonts.h'} and ui_fonts.cpp, {total} bytes of font data")
    if len(sys.argv) >= 3 and sys.argv[1] == "--check":
        check(arrays, pathlib.Path(sys.argv[2]) / "ui_fonts_check.png")


if __name__ == "__main__":
    main()
