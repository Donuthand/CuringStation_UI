// See ui_fuji.h for what this is and which spec sections it implements.
#include "ui_fuji.h"
#include "ctp_touch.h"
#include "station.h"
#include "ui_fonts.h"
#include "ui_logo.h"
#include <TFT_eSPI.h>
#include <SPI.h>

static TFT_eSPI tft;

// Off-screen buffers for the three things on Home that repaint every second: the card
// countdowns, the header temperature and the heat off timer. Clearing-then-drawing the
// countdown left a ~1.7 ms blank gap that was catchable if you were looking straight at
// it; rendering into RAM and pushing the finished block means the panel goes from old
// value straight to new value. TFT_eSPI 2.5.43 cannot push part of a sprite, so each box
// has its own, and each keeps its smooth font loaded for good (fonts belong to the
// object) — the once-a-second path never reloads a font.
static TFT_eSprite timeSprite = TFT_eSprite(&tft);   // shared by all six cards
static TFT_eSprite tempSprite = TFT_eSprite(&tft);
static TFT_eSprite heatSprite = TFT_eSprite(&tft);

// Touch is the FT6336U on the capacitive module (ctp_touch.h), not TFT_eSPI's XPT2046, so
// there is no calibration table: Test_ctp_touch showed the chip's factory calibration
// lands every tap within ~11 px (Project_Status 2026-09-15).

static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Colour at `a`/255 opacity over `bg` — CSS opacity, pre-blended, since the panel has no
// alpha. Used for the disabled Clear finished button and the dimmed "--.-" reading.
static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
  uint8_t fr = (fg >> 11) & 0x1F, fg6 = (fg >> 5) & 0x3F, fb = fg & 0x1F;
  uint8_t br = (bg >> 11) & 0x1F, bg6 = (bg >> 5) & 0x3F, bb = bg & 0x1F;
  uint8_t r = (fr * a + br * (255 - a) + 127) / 255;
  uint8_t g = (fg6 * a + bg6 * (255 - a) + 127) / 255;
  uint8_t b = (fb * a + bb * (255 - a) + 127) / 255;
  return (r << 11) | (g << 5) | b;
}

// --- Home palette: new_ui_try/5_fujifilm_concept_B_bigtext_try.html (since 2026-10-09; the
// colours are Concept B's, new_ui_try/5_fujifilm_concept_B_2026-09-22.html) ---------------
// The design authority since 2026-09-23 (Concept B, dark). Values are the computed colours
// read out of the page in headless Chrome, and match TFT_UI_HANDOFF.md section 3. The
// subpages add their own P_ colours further down (stage 2).
static constexpr uint16_t B_BG            = rgb(0x10, 0x2A, 0x27);
static constexpr uint16_t B_TEXT          = rgb(0xEE, 0xF6, 0xF2);
static constexpr uint16_t B_RULE          = rgb(0x29, 0x42, 0x3E);   // header / footer borders
static constexpr uint16_t B_SERVICE       = rgb(0x94, 0xB1, 0xA2);
static constexpr uint16_t B_SUBTITLE      = rgb(0xA9, 0xBE, 0xB5);
static constexpr uint16_t B_TARGET        = rgb(0x9B, 0xB9, 0xAA);
static constexpr uint16_t B_HEAT_STATUS   = rgb(0xE7, 0xD7, 0xA4);
// "MAT OFF" in red (Dough, 2026-09-23; the HTML uses the amber above for it too). A light red,
// not the logo's #ED174C, which is too dark to read at 10 px on #102A27 (~5:1 contrast here).
static constexpr uint16_t B_MAT_OFF       = rgb(0xFF, 0x6B, 0x6B);
static constexpr uint16_t B_ON_BG         = rgb(0xFF, 0xD0, 0x78);
static constexpr uint16_t B_ON_BORDER     = rgb(0xFF, 0xE1, 0xA7);
static constexpr uint16_t B_ON_FG         = rgb(0x38, 0x24, 0x00);
static constexpr uint16_t B_OFF_BG        = rgb(0x26, 0x3E, 0x35);
static constexpr uint16_t B_OFF_BORDER    = rgb(0x50, 0x6B, 0x60);
static constexpr uint16_t B_OFF_FG        = rgb(0xB4, 0xC7, 0xBD);
static constexpr uint16_t B_RUN_BG        = rgb(0x1C, 0x38, 0x32);
static constexpr uint16_t B_RUN_BORDER    = rgb(0x35, 0x54, 0x49);
static constexpr uint16_t B_EMPTY_BG      = rgb(0x14, 0x2E, 0x29);
static constexpr uint16_t B_EMPTY_BORDER  = rgb(0x34, 0x50, 0x48);
static constexpr uint16_t B_DONE_BG       = rgb(0x16, 0x73, 0x47);
static constexpr uint16_t B_DONE_BORDER   = rgb(0x64, 0xBC, 0x8B);
static constexpr uint16_t B_DONE_TEXT     = rgb(0xE0, 0xF7, 0xE9);
static constexpr uint16_t B_WHITE         = rgb(0xFF, 0xFF, 0xFF);
static constexpr uint16_t B_SLOT_LABEL    = rgb(0xB1, 0xC5, 0xBB);
static constexpr uint16_t B_NAME          = rgb(0xDE, 0xEB, 0xE3);
// User names, everywhere they appear (Dough 2026-10-09: "highlight the name of user in every
// part"). The HEAT ON amber, as the trial HTML's .un class.
static constexpr uint16_t B_USER          = rgb(0xFF, 0xD0, 0x78);
static constexpr uint16_t B_META          = rgb(0xB9, 0xCF, 0xC1);
static constexpr uint16_t B_TRACK         = rgb(0x37, 0x52, 0x47);
static constexpr uint16_t B_FILL          = rgb(0x9A, 0xE6, 0xB7);
static constexpr uint16_t B_PLUS          = rgb(0x86, 0xAD, 0x98);
static constexpr uint16_t B_ASSIGN        = rgb(0x8D, 0xAB, 0x9D);
static constexpr uint16_t B_X_BG          = rgb(0x34, 0x56, 0x49);
static constexpr uint16_t B_X_BORDER      = rgb(0x64, 0x84, 0x72);
static constexpr uint16_t B_MUTED_BORDER  = rgb(0x35, 0x54, 0x47);
static constexpr uint16_t B_MUTED_FG      = rgb(0xC3, 0xD5, 0xCB);
static constexpr uint16_t B_DANGER_BG     = rgb(0x68, 0x37, 0x3B);
static constexpr uint16_t B_DANGER_BORDER = rgb(0xA6, 0x68, 0x6A);
static constexpr uint16_t B_DANGER_FG     = rgb(0xFF, 0xE2, 0xE2);
static constexpr uint16_t B_PRIMARY_BG    = rgb(0xA4, 0xEC, 0xC1);
static constexpr uint16_t B_PRIMARY_FG    = rgb(0x15, 0x3A, 0x29);
// Completion overlay.
static constexpr uint16_t B_ALERT_BG      = rgb(0x16, 0x73, 0x47);
static constexpr uint16_t B_ALERT_SOFT    = rgb(0xD9, 0xF4, 0xE5);
static constexpr uint16_t B_ALERT_CARD    = rgb(0x10, 0x57, 0x36);
static constexpr uint16_t B_ALERT_EDGE    = rgb(0x60, 0xA9, 0x80);
static constexpr uint16_t B_ALERT_X_BG    = rgb(0x28, 0x75, 0x4D);
static constexpr uint16_t B_ALERT_X_EDGE  = rgb(0x8B, 0xCE, 0xAA);

// --- layout, 480x320 landscape ---------------------------------------------------
static constexpr int16_t SCR_W = 480;
static constexpr int16_t SCR_H = 320;

// Geometry measured off the Concept B HTML in headless Chrome (element boxes and text
// baselines; the page is a native 480x320 layout, so 1 CSS px is 1 panel px). Baselines
// are the browser's, rounded half up — TFT_eSPI's L_BASELINE puts the glyph's last row
// just above the given y, which is where Chrome's fractional baseline lands.
static constexpr int16_t HEADER_H = 58;   // its 1 px rule is the last row
static constexpr int16_t FOOTER_Y = 274;  // its 1 px rule is the first row

// Header.
static constexpr int16_t LOGO_X = 12, LOGO_Y = 10;         // ui_logo.h bakes in the 10.5
static constexpr int16_t SERVICE_X = 119, SERVICE_BASE = 25;
static constexpr int16_t SUBTITLE_X = 12, SUBTITLE_BASE = 45;
static constexpr int16_t HCOL_R = 392;                      // right edge of badge/target/status
static constexpr int16_t BADGE_Y = 4, BADGE_H = 21, BADGE_BASE = 20, BADGE_PAD = 8;
static constexpr int16_t TARGET_BASE = 39;
// The status sprite starts below the "g" of "Target" (descender to ~y 42) so it never paints
// over it; no glyph in "Off in 0:00:00" / "MAT OFF" reaches that high. Ends above the rule (57).
// Text sizes x1.2 since 2026-10-09 (Dough, the bigtext trial HTML): every Home number below
// was re-measured off new_ui_try/5_fujifilm_concept_B_bigtext_try.html.
static constexpr int16_t HSTAT_Y = 43, HSTAT_H = 14, HSTAT_BASE = 53, HSTAT_W = 100;
static constexpr int16_t TEMP_R = 454, TEMP_BASE = 40, TEMP_BOX_Y = 12, TEMP_BOX_H = 32;
static constexpr int16_t UNIT_X = 454, UNIT_BASE = 30;
static constexpr int16_t HEAT_ZONE_X = 286;   // the whole .thermal button, x 292.6-468

// The six cards: 3 across then 3, fixed — empty slots are always shown (handoff 2, 5).
// Was 4 + 3 with seven slots until 2026-10-09; the columns are the old second row's.
struct CardRect { int16_t x, y, w, h; };
static const CardRect CARDS[6] = {
  { 8, 66, 151, 97 }, { 165, 66, 150, 97 }, { 321, 66, 151, 97 },
  { 8, 169, 151, 97 }, { 165, 169, 150, 97 }, { 321, 169, 151, 97 },
};
static constexpr int16_t CARD_R = 8;
// Inside a card, relative to its top-left.
static constexpr int16_t C_PAD = 8;                 // 7 px padding + 1 px border
static constexpr int16_t C_TOP_BASE = 19;           // "Slot N", AVAILABLE / COMPLETE
static constexpr int16_t C_X_W = 27, C_X_H = 25, C_X_Y = 4, C_X_R = 5;  // cancel box, R = right inset
static constexpr int16_t C_X_BASE = 25;
static constexpr int16_t C_NAME_BASE = 40;
static constexpr int16_t C_TIME_BASE = 68, C_META_BASE = 80, C_HINT_BASE = 84;
static constexpr int16_t C_TRACK_Y = 85, C_TRACK_H = 3;
// Position pill behind the meta line: between the countdown sprite (ends at 69) and the track.
static constexpr int16_t C_PILL_Y = 70, C_PILL_H = 14;
static constexpr int16_t C_PLUS_BASE = 52, C_ASSIGN_BASE = 68;
// The countdown sprite: 28 px digits (ink ~20 px above the baseline), rows 46-69, so it stays
// clear of the name's descenders above (to ~44) and the pill below (70). Wide enough for
// "8:00:00" (~92 px) and no wider than a card's content box (150 - 16).
static constexpr int16_t C_TIME_Y = 46, C_TIME_H = 24, C_TIME_W = 110;

// Footer buttons.
static constexpr int16_t BTN_Y = 280, BTN_H = 35, BTN_BASE = 303, BTN_R = 5;

// Header widths that depend on the fonts, worked out once in UIFuji::begin().
static int16_t tempBoxW = 0;

// --- what is currently on screen, so only real changes get repainted -------------
static Station::Slot shadow[Station::SLOT_COUNT];
static bool shadowValid[Station::SLOT_COUNT] = { false };
static float shadowTemp = -999.0f;
static bool shadowHeat = false;
static bool shadowTempValid = false;
static bool shadowAnyDone = false;
static bool fullRedraw = true;

static bool sleeping = false;
static uint32_t lastTouchMs = 0;
static uint8_t touchRun = 0;   // consecutive polls that saw a touch, capped at 3
static uint32_t sleepAfterMs = UIFuji::SLEEP_AFTER_MS;

// --- smooth fonts ------------------------------------------------------------------
// While a smooth font is loaded, drawString() IGNORES the font number and uses it. On the
// shared tft object a Segoe UI font is only loaded for one drawing pass, and every such
// pass ends with endFonts(). The sprites keep theirs loaded; nothing else uses them.
static const uint8_t* tftFont = nullptr;

static void useFont(const uint8_t* f) {
  if (f == tftFont) return;
  tft.loadFont(f);   // unloads the previous one itself
  tftFont = f;
}

static void endFonts() {
  if (!tftFont) return;
  tft.unloadFont();
  tftFont = nullptr;
}

// TFT_eSPI does not read a smooth font's space: it GUESSES (ascent+descent)*2/7, which
// is visibly too wide for Segoe UI. The fonts carry U+00A0 with the true advance, so
// spaces are swapped for it before drawing or measuring. Strings longer than the
// buffer are cut, which no string on the panel comes near.
static void withRealSpaces(const char* s, char* out, size_t outSize) {
  size_t n = 0;
  for (const char* p = s; *p && n + 3 < outSize; p++) {
    if (*p == ' ') { out[n++] = (char)0xC2; out[n++] = (char)0xA0; }
    else           { out[n++] = *p; }
  }
  out[n] = '\0';
}

static void drawText(TFT_eSPI& dst, const char* s, int16_t x, int16_t y, uint8_t datum) {
  char buf[128];   // the longest subpage note is ~70 characters, spaces cost 2 bytes each
  withRealSpaces(s, buf, sizeof(buf));
  dst.setTextDatum(datum);
  dst.drawString(buf, x, y);
  dst.setTextDatum(TL_DATUM);
}

static int16_t textW(TFT_eSPI& dst, const char* s) {
  char buf[128];
  withRealSpaces(s, buf, sizeof(buf));
  return dst.textWidth(buf);
}

// CSS letter-spacing, which TFT_eSPI has no notion of: glyphs are placed one at a time
// with `spacing` px added after each, accumulated as a float so -0.6 px over seven digits
// comes out as the browser's ~4 px rather than 0 or 7. Left-aligned on the baseline.
static uint8_t utf8Len(char lead) {
  uint8_t c = (uint8_t)lead;
  return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

static void nextGlyph(const char*& p, char* ch) {
  uint8_t n = utf8Len(*p);
  if (*p == ' ') { ch[0] = (char)0xC2; ch[1] = (char)0xA0; ch[2] = '\0'; }
  else           { memcpy(ch, p, n); ch[n] = '\0'; }
  p += n;
}

// The glyph's own advance. NOT textWidth(ch): for the LAST character of a string TFT_eSPI
// returns only its inked width (dX + width), so measured one at a time every glyph came out
// a little narrow and a space (no ink) came out as 0 — "NEW DURATION · SECONDS" drew as one
// word on the panel (Dough, 2026-09-23). gxAdvance / getUnicodeIndex are public in 2.5.43.
static float glyphAdvance(TFT_eSPI& dst, const char* ch) {
  uint8_t c0 = (uint8_t)ch[0];
  uint16_t code = c0;
  if (c0 >= 0xE0)      code = ((c0 & 0x0F) << 12) | ((ch[1] & 0x3F) << 6) | (ch[2] & 0x3F);
  else if (c0 >= 0xC0) code = ((c0 & 0x1F) << 6) | (ch[1] & 0x3F);
  uint16_t g = 0;
  if (dst.getUnicodeIndex(code, &g)) return dst.gxAdvance[g];
  return dst.textWidth(ch);   // not in the font: TFT_eSPI draws its hollow box
}

static float spacedW(TFT_eSPI& dst, const char* s, float spacing) {
  float w = 0;
  char ch[5];
  bool any = false;
  for (const char* p = s; *p;) { nextGlyph(p, ch); w += glyphAdvance(dst, ch) + spacing; any = true; }
  return any ? w - spacing : 0;
}

static void drawSpaced(TFT_eSPI& dst, const char* s, float x, int16_t base, float spacing) {
  char ch[5];
  dst.setTextDatum(L_BASELINE);
  for (const char* p = s; *p;) {
    nextGlyph(p, ch);
    dst.drawString(ch, (int16_t)lroundf(x), base);
    x += glyphAdvance(dst, ch) + spacing;
  }
  dst.setTextDatum(TL_DATUM);
}

// One line with an ellipsis when it does not fit (handoff 4: long names use one line and
// an ellipsis). Names are ASCII, so cutting byte by byte is safe. Needs the font loaded.
static void fitText(TFT_eSPI& dst, const char* s, int16_t maxW, char* out, size_t outSize) {
  strncpy(out, s, outSize - 1);
  out[outSize - 1] = '\0';
  if (textW(dst, out) <= maxW) return;
  for (size_t n = strlen(out); n > 0; n--) {
    size_t k = n - 1;
    while (k > 0 && s[k - 1] == ' ') k--;
    if (k + 4 > outSize) continue;
    memcpy(out, s, k);
    strcpy(out + k, "\xE2\x80\xA6");
    if (textW(dst, out) <= maxW) return;
  }
  strcpy(out, "\xE2\x80\xA6");
}

// One of the three once-a-second boxes: allocate it and give it its font for good.
static void makeSprite(TFT_eSprite& spr, int16_t w, int16_t h, const uint8_t* font,
                       const char* what) {
  spr.setColorDepth(16);
  if (spr.createSprite(w, h) == nullptr) {
    Serial.printf("[UI] WARNING: %s sprite allocation failed, it will not draw\n", what);
    return;
  }
  spr.loadFont(font);
}

// A 1 px border in the card's radius, filled. The corners outside the radius are painted
// with the page background first, since a card is redrawn over its old self.
static void drawBox(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t fill,
                    uint16_t border, uint16_t outside) {
  tft.fillRect(x, y, w, h, outside);
  tft.fillRoundRect(x, y, w, h, r, fill);
  tft.drawRoundRect(x, y, w, h, r, border);
}

// CSS "1px dashed": TFT_eSPI has no dashed line, so the solid border's straight runs get
// 3 px gaps every 6 px, the rounded corners stay solid — close to Chrome's 1 px dash.
static void dashBorder(const CardRect& c, uint16_t fill) {
  for (int16_t x = c.x + CARD_R + 3; x + 3 <= c.x + c.w - CARD_R; x += 6) {
    tft.drawFastHLine(x, c.y, 3, fill);
    tft.drawFastHLine(x, c.y + c.h - 1, 3, fill);
  }
  for (int16_t y = c.y + CARD_R + 3; y + 3 <= c.y + c.h - CARD_R; y += 6) {
    tft.drawFastVLine(c.x, y, 3, fill);
    tft.drawFastVLine(c.x + c.w - 1, y, 3, fill);
  }
}

// --- Home: header -------------------------------------------------------------------

// The parts of the header that only change with a full redraw: wordmark, "/ SERVICE",
// subtitle, the °C unit and the target (the setpoint is only edited on a subpage, and
// closing any subpage repaints Home in full).
static void drawHeaderStatic() {
  tft.fillRect(0, 0, SCR_W, HEADER_H, B_BG);
  tft.drawFastHLine(0, HEADER_H - 1, SCR_W, B_RULE);

  // Handoff 4: the vector wordmark, not a font approximation. tools/make_ui_logo.py
  // rasterises the HTML's SVG paths, pre-blended onto B_BG.
  bool swap = tft.getSwapBytes();
  tft.setSwapBytes(true);
  tft.pushImage(LOGO_X, LOGO_Y, UI_LOGO_W, UI_LOGO_H, UI_LOGO);
  tft.setSwapBytes(swap);

  useFont(UIB_R10);
  tft.setTextColor(B_SERVICE, B_BG);
  drawSpaced(tft, "/ SERVICE", SERVICE_X, SERVICE_BASE, 0.5f);
  useFont(UIB_R12);
  tft.setTextColor(B_SUBTITLE, B_BG);
  drawText(tft, "Endoscope curing station", SUBTITLE_X, SUBTITLE_BASE, L_BASELINE);

  char t[20];
  snprintf(t, sizeof(t), "Target %u\xC2\xB0" "C", Station::setpointC());
  useFont(UIB_R11);
  tft.setTextColor(B_TARGET, B_BG);
  drawText(tft, t, HCOL_R, TARGET_BASE, R_BASELINE);

  useFont(UIB_B12);
  tft.setTextColor(B_TEXT, B_BG);
  drawText(tft, "\xC2\xB0" "C", UNIT_X, UNIT_BASE, L_BASELINE);
  endFonts();
}

// ● HEAT ON / ○ HEAT OFF — the SSR output's actual state (handoff 5: the mat can be enabled
// and still show HEAT OFF at its target; MAT OFF is the status line's job, not this one's).
// Right-aligned on the column edge, so its left end moves between the two labels, as in
// the HTML.
static void drawHeaderBadge() {
  const char* label = shadowHeat ? "\xE2\x97\x8F HEAT ON" : "\xE2\x97\x8B HEAT OFF";
  uint16_t bg     = shadowHeat ? B_ON_BG : B_OFF_BG;
  uint16_t border = shadowHeat ? B_ON_BORDER : B_OFF_BORDER;

  useFont(UIB_B12);
  int16_t tw = (int16_t)lroundf(spacedW(tft, label, 0.3f));
  int16_t maxW = (int16_t)lroundf(spacedW(tft, "\xE2\x97\x8B HEAT OFF", 0.3f)) + 2 * BADGE_PAD;
  int16_t w = tw + 2 * BADGE_PAD;
  tft.fillRect(HCOL_R - maxW - 2, BADGE_Y, maxW + 2, BADGE_H, B_BG);
  drawBox(HCOL_R - w, BADGE_Y, w, BADGE_H, 4, bg, border, B_BG);
  tft.setTextColor(shadowHeat ? B_ON_FG : B_OFF_FG, bg);
  drawSpaced(tft, label, HCOL_R - w + BADGE_PAD, BADGE_BASE, 0.3f);
  endFonts();
}

// "--.-" rather than a stale number when the probe is not answering; a wrong temperature
// on a curing station is worse than an obviously absent one. Dimmed to the HTML's .45.
static void drawHeaderTemp() {
  char t[12];
  if (shadowTempValid) snprintf(t, sizeof(t), "%.1f", shadowTemp);
  else                 snprintf(t, sizeof(t), "--.-");

  tempSprite.fillSprite(B_BG);
  tempSprite.setTextColor(shadowTempValid ? B_TEXT : blend565(B_TEXT, B_BG, 115), B_BG);
  float w = spacedW(tempSprite, t, -1.0f);
  drawSpaced(tempSprite, t, tempBoxW - w, TEMP_BASE - TEMP_BOX_Y, -1.0f);
  tempSprite.pushSprite(TEMP_R - tempBoxW, TEMP_BOX_Y);
}

// Under the target: "Off in H:MM:SS" while an off timer runs, "MAT OFF" once the mat is
// disabled, nothing otherwise (handoff 5). Always H:MM:SS here, as the HTML's heatClock().
static bool shadowHeatEnabled = true;
static uint32_t shadowHeatSec = 0;

static void drawHeaderHeatTimer() {
  shadowHeatEnabled = Station::heatEnabled();
  shadowHeatSec = Station::heatOffRemainingSec();

  heatSprite.fillSprite(B_BG);
  heatSprite.setTextColor(shadowHeatEnabled ? B_HEAT_STATUS : B_MAT_OFF, B_BG);
  char t[24];
  t[0] = '\0';
  if (!shadowHeatEnabled) {
    snprintf(t, sizeof(t), "MAT OFF");
  } else if (shadowHeatSec) {
    snprintf(t, sizeof(t), "Off in %lu:%02lu:%02lu", (unsigned long)(shadowHeatSec / 3600),
             (unsigned long)(shadowHeatSec % 3600 / 60), (unsigned long)(shadowHeatSec % 60));
  }
  if (t[0]) drawText(heatSprite, t, HSTAT_W, HSTAT_BASE - HSTAT_Y, R_BASELINE);
  heatSprite.pushSprite(HCOL_R - HSTAT_W, HSTAT_Y);
}

// --- Home: cards --------------------------------------------------------------------

// The thin bar under a running countdown shows time REMAINING: full at the start,
// shrinking to nothing (handoff 5). Drawn WITHOUT clearing first — the filled and empty
// parts are two adjacent rectangles covering the whole track, so it never blanks.
static void drawCardTrack(uint8_t i) {
  const CardRect& c = CARDS[i];
  const Station::Slot& s = Station::slot(i);
  int16_t w = c.w - 2 * C_PAD;
  int16_t fill = s.totalSec ? (int16_t)((uint32_t)w * s.remainingSec / s.totalSec) : 0;
  int16_t x = c.x + C_PAD, y = c.y + C_TRACK_Y;
  if (fill > 0) tft.fillRect(x, y, fill, C_TRACK_H, B_FILL);
  if (fill < w) tft.fillRect(x + fill, y, w - fill, C_TRACK_H, B_TRACK);
}

// The countdown, via the sprite so the panel never shows a blank box. MM:SS under an
// hour, H:MM:SS from an hour up (Station::formatRemaining).
static void drawCardTime(uint8_t i) {
  const CardRect& c = CARDS[i];
  char t[9];
  Station::formatRemaining(Station::slot(i).remainingSec, t);
  timeSprite.fillSprite(B_RUN_BG);
  timeSprite.setTextColor(B_TEXT, B_RUN_BG);
  drawSpaced(timeSprite, t, 0, C_TIME_BASE - C_TIME_Y, -0.6f);
  timeSprite.pushSprite(c.x + C_PAD, c.y + C_TIME_Y);
}

// The user's name on one line, in the name colour, cut with an ellipsis at the card's content
// edge. The initials badge in front of it was removed 2026-10-09.
static void drawCardIdentity(const CardRect& c, const char* name, uint16_t bg) {
  int16_t nx = c.x + C_PAD;
  char shown[Station::USER_NAME_MAX + 4];
  useFont(UIB_R12);
  fitText(tft, name, c.x + c.w - C_PAD - nx, shown, sizeof(shown));
  tft.setTextColor(B_USER, bg);
  drawText(tft, shown, nx, c.y + C_NAME_BASE, L_BASELINE);
}

static void drawCard(uint8_t i) {
  const CardRect& c = CARDS[i];
  const Station::Slot& s = Station::slot(i);
  bool running = (s.state == Station::SlotState::Running);
  bool done    = (s.state == Station::SlotState::Done);
  uint16_t bg = running ? B_RUN_BG : (done ? B_DONE_BG : B_EMPTY_BG);
  uint16_t border = running ? B_RUN_BORDER : (done ? B_DONE_BORDER : B_EMPTY_BORDER);

  drawBox(c.x, c.y, c.w, c.h, CARD_R, bg, border, B_BG);
  if (!running && !done) dashBorder(c, bg);

  char label[8];
  snprintf(label, sizeof(label), "Slot %u", i + 1);
  // 13 px white rather than the HTML's 11 px #B1C5BB: Dough wanted the slot number to stand
  // out more (2026-09-23).
  useFont(UIB_R13);
  tft.setTextColor(done ? B_WHITE : B_TEXT, bg);
  drawSpaced(tft, label, c.x + C_PAD, c.y + C_TOP_BASE, 0.22f);

  if (running) {
    // The × is the card's only control — tapping the body of a running card does nothing
    // (handoff 5). 27 x 25, inset 4 from the right and 3 from the top inside the border.
    int16_t bx = c.x + c.w - C_X_R - C_X_W;
    drawBox(bx, c.y + C_X_Y, C_X_W, C_X_H, 4, B_X_BG, B_X_BORDER, bg);
    useFont(UIB_X18);
    tft.setTextColor(B_SLOT_LABEL, B_X_BG);
    drawText(tft, "\xC3\x97", bx + C_X_W / 2 + 1, c.y + C_X_BASE, C_BASELINE);

    drawCardIdentity(c, s.name, bg);

    // The curing position as a pale-green pill, then "105 min" (handoff 6 had "RBS · 105 min"
    // in grey; Dough, 2026-09-30: the part did not stand out). No position = just the minutes.
    // The minutes are dropped when they do not fit beside the pill ("Handle cover").
    char mins[12], shown[32];
    snprintf(mins, sizeof(mins), "%lu min", (unsigned long)(s.totalSec / 60));
    int16_t mx = c.x + C_PAD;
    useFont(UIB_R9);
    if (s.position) {
      fitText(tft, Station::POSITION_NAMES[s.position - 1], c.w - 2 * C_PAD - 8, shown, sizeof(shown));
      int16_t pw = textW(tft, shown) + 8;
      tft.fillRoundRect(mx, c.y + C_PILL_Y, pw, C_PILL_H, 3, B_PRIMARY_BG);
      tft.setTextColor(B_PRIMARY_FG, B_PRIMARY_BG);
      drawText(tft, shown, mx + 4, c.y + C_META_BASE, L_BASELINE);
      mx += pw + 4;
    }
    tft.setTextColor(B_META, bg);
    if (textW(tft, mins) <= c.x + c.w - C_PAD - mx) drawText(tft, mins, mx, c.y + C_META_BASE, L_BASELINE);
    endFonts();

    drawCardTime(i);
    drawCardTrack(i);
    return;
  }

  useFont(UIB_R9);
  tft.setTextColor(done ? B_DONE_TEXT : B_SLOT_LABEL, bg);
  drawText(tft, done ? "COMPLETE" : "AVAILABLE", c.x + c.w - C_PAD, c.y + C_TOP_BASE, R_BASELINE);

  if (!done) {
    useFont(UIB_PLUS24);
    tft.setTextColor(B_PLUS, bg);
    drawText(tft, "+", c.x + C_PAD, c.y + C_PLUS_BASE, L_BASELINE);
    useFont(UIB_R12);
    tft.setTextColor(B_ASSIGN, bg);
    drawText(tft, "Assign a cure", c.x + C_PAD, c.y + C_ASSIGN_BASE, L_BASELINE);
    endFonts();
    return;
  }

  // Completed: green, not red (handoff 5). Tapping it starts setup again for this slot.
  drawCardIdentity(c, s.name, bg);
  useFont(UIB_SB23);
  tft.setTextColor(B_WHITE, bg);
  drawSpaced(tft, "Ready \xE2\x9C\x93", c.x + C_PAD, c.y + C_TIME_BASE, -0.6f);

  // The position replaces the tone in the hint when one was chosen (handoff 6). The HTML
  // wraps a hint that is too long ("Handle cover · Restart ›" on a narrow card); here it is
  // cut with "…" so the card keeps its layout.
  char hint[40], shown[40];
  if (s.position) snprintf(hint, sizeof(hint), "%s \xC2\xB7 Restart \xE2\x80\xBA",
                           Station::POSITION_NAMES[s.position - 1]);
  else            snprintf(hint, sizeof(hint), "\xE2\x99\xAA %s \xC2\xB7 Restart \xE2\x80\xBA",
                           Station::SOUND_NAMES[s.sound]);
  useFont(UIB_R10);
  fitText(tft, hint, c.w - 2 * C_PAD, shown, sizeof(shown));
  tft.setTextColor(B_DONE_TEXT, bg);
  drawText(tft, shown, c.x + C_PAD, c.y + C_HINT_BASE, L_BASELINE);
  endFonts();
}

// --- Home: footer -------------------------------------------------------------------

// Settings (muted), Clear finished (danger), + New cure (primary), per the HTML — with the
// keypad key printed on each (Dough, 2026-09-23: a keyboard guide; a key nobody can discover
// is worse than no key). Widths = Bold label + the HTML's padding: Settings stays at the
// left edge, Clear finished keeps its centre, + New cure keeps its right edge.
// The key is drawn large beside the label — at the label's size "(*)" was too small to read (Dough,
// 2026-09-23). "0" and "#" at 24 px (baseline 305); "*" at 28 px (an asterisk is a small, high
// mark: ink rows -20..-10, so baseline 313). Both centre the ink on the button (y 297.5), and the
// 14 px label shares that centre line (Dough 2026-10-09, "make word in the middle"). Brackets
// back 2026-09-24, at the key's size. Widths re-measured off the bigtext trial HTML.
struct FooterButton {
  int16_t x, w;
  const char* label;
  const char* key;
  const uint8_t* keyFont;
  int16_t keyBase;
  uint16_t bg, border, fg;
};
static const FooterButton FOOTER[3] = {
  { 8,   118, "Settings",       "*", UIB_B28, 313, B_BG,         B_MUTED_BORDER,  B_MUTED_FG },
  { 151, 155, "Clear finished", "0", UIB_R20, 305, B_DANGER_BG,  B_DANGER_BORDER, B_DANGER_FG },
  { 330, 142, "+ New cure",     "#", UIB_R20, 305, B_PRIMARY_BG, B_PRIMARY_BG,    B_PRIMARY_FG },
};
static constexpr int16_t KEY_GAP = 6;
// Touch splits at the middle of each gap, so every pixel of the footer does something.
static constexpr int16_t FOOT_SPLIT_1 = 138, FOOT_SPLIT_2 = 318;
static constexpr int16_t BRACKET_BASE = 305;   // "(" ")" in UIB_R20, on the digit keys' baseline

static bool anyDone() {
  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
    if (Station::slot(i).state == Station::SlotState::Done) return true;
  }
  return false;
}

// Clear finished is dimmed to the HTML's disabled opacity (.4) while no slot is Done.
static void drawFooter() {
  shadowAnyDone = anyDone();
  tft.fillRect(0, FOOTER_Y, SCR_W, SCR_H - FOOTER_Y, B_BG);
  tft.drawFastHLine(0, FOOTER_Y, SCR_W, B_RULE);
  for (uint8_t k = 0; k < 3; k++) {
    const FooterButton& b = FOOTER[k];
    uint8_t a = (k == 1 && !shadowAnyDone) ? 102 : 255;
    uint16_t bg = blend565(b.bg, B_BG, a);
    drawBox(b.x, BTN_Y, b.w, BTN_H, BTN_R, bg, blend565(b.border, B_BG, a), B_BG);
    useFont(UIB_R12);
    int16_t lw = textW(tft, b.label);
    useFont(UIB_R20);
    int16_t ow = (int16_t)lroundf(glyphAdvance(tft, "("));
    int16_t cw = (int16_t)lroundf(glyphAdvance(tft, ")"));
    useFont(b.keyFont);
    int16_t kw = (int16_t)lroundf(glyphAdvance(tft, b.key));
    int16_t x = b.x + (b.w - (lw + KEY_GAP + ow + kw + cw)) / 2;
    int16_t kx = x + lw + KEY_GAP + ow;
    tft.setTextColor(blend565(b.fg, B_BG, a), bg);
    drawText(tft, b.key, kx, b.keyBase, L_BASELINE);
    useFont(UIB_R20);
    drawText(tft, "(", kx - ow, BRACKET_BASE, L_BASELINE);
    drawText(tft, ")", kx + kw, BRACKET_BASE, L_BASELINE);
    useFont(UIB_R12);
    drawText(tft, b.label, x, BTN_BASE, L_BASELINE);
  }
  endFonts();
}

// --- touch ----------------------------------------------------------------------

enum class Hit : uint8_t { None, Menu, Start, ClearDone, Slot, SlotCancel, Heat };

// --- subpages --------------------------------------------------------------------
//
// One screen at a time, full panel, over Home. Concept B flow (handoff 6): + New cure ->
// Choose slot -> Select user -> Curing position (optional) -> Set cure time -> Home. Tapping
// an empty or finished card skips Choose slot. Stage 2 of the port (2026-09-23): every
// subpage now follows the Concept B HTML; geometry and colours measured off it in headless
// Chrome, baselines rounded half up as on Home.
//
// Deliberate differences from the HTML, all small:
//  - Settings footer says what the firmware does ("Saved on the station ...") instead of the
//    HTML's "HTML simulation · Saved locally every 30 sec".
//  - Recovery list: all six rows fit, so no scroll box; its right column ends at the
//    content edge (the HTML's is 3 px short, room for a scrollbar).
//  - Long texts that the HTML wraps or lets overflow are cut with "…" on one line (the cure
//    time hint, the Choose slot detail is split at " · " instead of anywhere).
//  - The temperature log's time axis spans the samples, not "now" (the HTML's last point can
//    sit up to 90 s short of the right edge); the CURRENT figure is the live header reading.
//  - The typed name has no caret, like the unfocused HTML input.
//  - Set cure time shows "Still warming: x of 45 °C" under the presets while the air is below
//    the cure-ready point (2026-10-02). It warns only; Start still works.

enum class Screen : uint8_t { Main, ChooseSlot, ChooseUser, ChoosePosition, EnterTime,
                              ConfirmCancel, Alarm,
                              Menu, SetTemp, TempLog, SetAlarm, ManageUsers,
                              EditSound, AddUserName, AddUserSound,
                              UserActions, PowerLoss,
                              HeatMenu, HeatTimer, HeatWarn, SetVolume, OverTemp, SensorFault };

static Screen screen = Screen::Main;
static int8_t selSlot = -1;
// Over-temperature warning state (see ui_fuji.h). otReturn: where the warning goes back to.
static bool otActive = false, otSilenced = false;
static float otTempC = 0.0f;
static Screen otReturn = Screen::Main;
// Sensor-fault warning state, the same three as above (see ui_fuji.h).
static bool sfActive = false, sfSilenced = false;
static Screen sfReturn = Screen::Main;
static int8_t selUser = -1;
static uint8_t selPosition = 0;      // 0 = skipped, else 1..Station::POSITION_COUNT
static bool slotWasPreset = false;   // entered by tapping a card, so Back from users exits
static uint8_t userPage = 0;

// The one numeric entry, shared by every number screen as the HTML's numeric() is: cure time
// and heat off timer (3 digits), setpoint and alarm duration (2).
static char numBuf[4] = "";
static const char* numErr = nullptr;
static char nameBuf[Station::USER_NAME_MAX + 1] = "";  // new user being typed
static const char* nameErr = nullptr;
static int8_t editUser = -1;         // user whose tone is being changed
static int8_t tonePreview = -1;      // play button tapped, waiting for the sketch; -1 = none
static bool toneFromActions = false; // tone list opened from User options: Back returns there
static bool volFromMenu = false;     // volume opened from Settings row 5: Back/save return there
static uint8_t managePage = 0;

// --- subpage palette (Concept B computed colours) ---
static constexpr uint16_t P_BACK_BG     = rgb(0x28, 0x48, 0x3D);
static constexpr uint16_t P_TAG         = rgb(0x9D, 0xB8, 0xAA);
// Over-temperature warning (2026-10-01): deep red full screen, white text.
static constexpr uint16_t OT_BG         = rgb(0x9B, 0x1C, 0x1C);
static constexpr uint16_t OT_SOFT       = rgb(0xFF, 0xD6, 0xD6);
static constexpr uint16_t P_SUMMARY     = rgb(0xA7, 0xC0, 0xB2);
static constexpr uint16_t P_ROW_BG      = rgb(0x20, 0x3E, 0x33);
static constexpr uint16_t P_ROW_BORDER  = rgb(0x36, 0x58, 0x49);
static constexpr uint16_t P_ROW_INDEX   = rgb(0x8D, 0xAE, 0x9B);
static constexpr uint16_t P_ROW_VALUE   = rgb(0xAF, 0xCC, 0xBA);
static constexpr uint16_t P_TONE        = rgb(0x9B, 0xE2, 0xB6);
static constexpr uint16_t P_CHOICE_BG   = rgb(0x21, 0x3F, 0x35);
static constexpr uint16_t P_CHOICE_EDGE = rgb(0x3C, 0x59, 0x4B);
static constexpr uint16_t P_SEL_BG      = rgb(0xC2, 0xEF, 0xD1);
static constexpr uint16_t P_SEL_FG      = rgb(0x16, 0x3B, 0x29);
static constexpr uint16_t P_OPT_EDGE    = rgb(0x45, 0x6B, 0x57);
static constexpr uint16_t P_NOTE        = rgb(0x99, 0xB5, 0xA4);
static constexpr uint16_t P_LABEL       = rgb(0xA2, 0xBE, 0xB0);
static constexpr uint16_t P_FIELD_BG    = rgb(0x1C, 0x38, 0x2F);
static constexpr uint16_t P_FIELD_EDGE  = rgb(0x50, 0x70, 0x5B);
static constexpr uint16_t P_HINT        = rgb(0xFF, 0xD1, 0xCD);   // .error: hint and error alike
static constexpr uint16_t P_PAD_KEY     = rgb(0x28, 0x48, 0x3A);
static constexpr uint16_t P_WARM        = rgb(0xE6, 0xD2, 0x9C);   // cancel countdown, recovery text
static constexpr uint16_t P_DEL_BG      = rgb(0x58, 0x33, 0x36);
static constexpr uint16_t P_KB_KEY      = rgb(0x29, 0x48, 0x3B);
static constexpr uint16_t P_HEAT_STATE  = rgb(0xBB, 0xEF, 0xCB);
static constexpr uint16_t P_LIST_RULE   = rgb(0x29, 0x4A, 0x3B);
static constexpr uint16_t P_LOG_LABEL   = rgb(0xAD, 0xC3, 0xB5);
static constexpr uint16_t P_LOG_MIN     = rgb(0x9F, 0xC9, 0xF3);
static constexpr uint16_t P_LOG_CUR     = rgb(0xF2, 0xBF, 0x72);   // also the trace
static constexpr uint16_t P_LOG_MAX     = rgb(0xED, 0xAA, 0xA5);
static constexpr uint16_t P_CHART_TEXT  = rgb(0xA9, 0xC5, 0xB6);
static constexpr uint16_t P_GRID        = rgb(0x36, 0x54, 0x46);
static constexpr uint16_t P_BAND        = rgb(0x24, 0x4A, 0x37);

// --- subpage frame: .page-head (48 px, rule on its last row) / .page-foot (47 px) ---
static constexpr int16_t PAGE_HEAD_H = 48;
static constexpr int16_t PAGE_FOOT_Y = 273;                     // its rule is the first row
// Baselines below re-measured 2026-10-09 off the bigtext trial HTML (all text x1.2).
static constexpr int16_t BACK_X = 10, BACK_Y = 7, BACK_W = 36, BACK_H = 34, BACK_BASE = 34;
static constexpr int16_t TITLE_X = 56, TITLE_BASE = 32, TAG_R = 470, TAG_BASE = 29;
static constexpr int16_t SUMMARY_X = 10, FOOT_BASE = 303;
static constexpr int16_t FOOT_BTN_Y = 280, FOOT_BTN_H = 35;
static constexpr int16_t PRIMARY_FOOT_X = 356, PRIMARY_FOOT_W = 114;   // Skip / Add user / Next
static constexpr int16_t CX = 12, CW = 456;                            // content box
static constexpr int16_t DETAIL_BASE = 80;                             // .detail-user line

// Rows (.tone-list / .setting-list buttons): 34 high, 5 px apart.
static constexpr int16_t ROW_Y0 = 60, ROW_H = 34, ROW_PITCH = 39;
static constexpr uint8_t USER_ROWS = 5;       // Select user, per page
static constexpr uint8_t MANAGE_ROWS = 4;     // User names & tones, per page
static inline int16_t rowY(uint8_t r) { return ROW_Y0 + r * ROW_PITCH; }

// .safe-actions buttons: 39 high.
static constexpr int16_t ACT_H = 39;

// Pager (.pager in the footer): ‹ 7, "n/N", 9 ›.
static constexpr int16_t PREV_X = 10, NEXT_X = 86, PAGER_W = 39, PAGE_NUM_X = 57;

// Numeric page (.entry): left column 220 wide, keypad 3 x 70 on the right.
static constexpr int16_t NUM_X = 12, NUM_Y = 84, NUM_W = 220, NUM_H = 58, NUM_BASE = 126;
static constexpr int16_t NUM_LABEL_BASE = 74, NUM_HINT_BASE = 164;
static constexpr int16_t PAD_Y0 = 60, PAD_PITCH = 48, PAD_W = 70, PAD_H = 43;
static const int16_t PAD_X[3] = { 248, 323, 398 };
static const CardRect PRESETS[4] = { { 12, 177, 108, 31 }, { 125, 177, 107, 31 },
                                     { 12, 213, 108, 31 }, { 125, 213, 107, 31 } };
static const uint16_t PRESET_MIN[4] = { 60, 75, 90, 105 };            // handoff 6, exactly
// As wide as the longer label ("Alarm volume 20 ›" + padding), as the HTML's buttons are.
static constexpr int16_t TONES_BTN_Y = 177, TONES_BTN_W = 150, TONES_BTN_H = 35;
// Alarm volume button under it (2026-09-24), same size, the HTML's 6 px gap between them.
static constexpr int16_t VOL_BTN_Y = TONES_BTN_Y + TONES_BTN_H + 6;

// Smooth-font text on a subpage. Callers end their pass with endFonts().
static void pText(const uint8_t* font, const char* s, int16_t x, int16_t base, uint16_t fg,
                  uint16_t bg, uint8_t datum = L_BASELINE) {
  useFont(font);
  tft.setTextColor(fg, bg);
  drawText(tft, s, x, base, datum);
}

// Width of s in the loaded font, summing true advances (textW() undercounts a trailing space).
static int16_t runW(const char* s) { return (int16_t)lroundf(spacedW(tft, s, 0.0f)); }

// One piece of a line drawn in pieces (a user's name in B_USER between plain text). Returns the
// x where the next piece starts.
static int16_t drawRun(const uint8_t* font, const char* s, int16_t x, int16_t base, uint16_t fg,
                       uint16_t bg) {
  pText(font, s, x, base, fg, bg);
  return x + runW(s);
}

// prefix + name + suffix on one line, the name in B_USER and cut with "…" when the whole does
// not fit maxW. Left-aligned at x, or right-aligned on x when `right`.
static void drawNameLine(const uint8_t* font, const char* prefix, const char* name,
                         const char* suffix, int16_t x, int16_t base, int16_t maxW, uint16_t fg,
                         uint16_t userFg, uint16_t bg, bool right = false) {
  useFont(font);
  char shown[Station::USER_NAME_MAX + 4];
  int16_t fixedW = runW(prefix) + runW(suffix);
  fitText(tft, name, maxW - fixedW, shown, sizeof(shown));
  if (right) x -= fixedW + runW(shown);
  x = drawRun(font, prefix, x, base, fg, bg);
  x = drawRun(font, shown, x, base, userFg, bg);
  drawRun(font, suffix, x, base, fg, bg);
}

// Text that repaints in place once a second (cancel countdown, heating mat state). The smooth
// font's background fill paints each glyph cell solid, so the old value is overwritten rather
// than cleared first — no blank frame; whatever a longer previous string left to the right is
// erased after. Nothing else shares these rows.
static void liveText(const uint8_t* font, const char* s, int16_t x, int16_t base, int16_t maxW,
                     uint16_t fg) {
  useFont(font);
  tft.setTextColor(fg, B_BG, true);
  drawText(tft, s, x, base, L_BASELINE);
  tft.setTextColor(fg, B_BG, false);
  int16_t w = textW(tft, s), h = tft.fontHeight();
  if (w < maxW) tft.fillRect(x + w, base - h, maxW - w, h + 8, B_BG);
  endFonts();
}

// The page header and the two rules. goToScreen() has already cleared the panel. `back`
// false (Power was interrupted) leaves no back button: that page must be answered.
static void drawPageFrame(const char* title, const char* tag, bool back = true,
                          bool footRule = true) {
  tft.drawFastHLine(0, PAGE_HEAD_H - 1, SCR_W, B_RULE);
  if (footRule) tft.drawFastHLine(0, PAGE_FOOT_Y, SCR_W, B_RULE);
  int16_t tx = 10;
  if (back) {
    drawBox(BACK_X, BACK_Y, BACK_W, BACK_H, 5, P_BACK_BG, P_BACK_BG, B_BG);
    pText(UIB_R20, "\xE2\x80\xB9", BACK_X + BACK_W / 2, BACK_BASE, B_TEXT, P_BACK_BG, C_BASELINE);
    tx = TITLE_X;
  }
  pText(UIB_B18, title, tx, TITLE_BASE, B_TEXT, B_BG);
  if (tag) pText(UIB_R11, tag, TAG_R, TAG_BASE, P_TAG, B_BG, R_BASELINE);
  endFonts();
}

// The footer's grey explanation line (.summary). `maxW` clears the old text first when it is
// repainted in place (the name error).
static void drawSummary(const char* s, int16_t maxW = 0) {
  if (maxW) tft.fillRect(SUMMARY_X, FOOT_BASE - 15, maxW, 20, B_BG);
  pText(UIB_R11, s, SUMMARY_X, FOOT_BASE, P_SUMMARY, B_BG);
  endFonts();
}

// .action buttons: primary (pale green), danger (red), muted (outline). Label left-aligned
// with the HTML's 13 px padding (+1 border), or centred for the fixed-width footer buttons.
// Disabled = the HTML's opacity .4, pre-blended.
enum class Btn : uint8_t { Primary, Danger, Muted };

static void drawAction(int16_t x, int16_t y, int16_t w, int16_t h, const char* label, Btn k,
                       bool centre, bool enabled = true) {
  uint16_t bg = B_BG, edge = B_MUTED_BORDER, fg = B_MUTED_FG;
  if (k == Btn::Primary) { bg = B_PRIMARY_BG; edge = B_PRIMARY_BG;    fg = B_PRIMARY_FG; }
  if (k == Btn::Danger)  { bg = B_DANGER_BG;  edge = B_DANGER_BORDER; fg = B_DANGER_FG; }
  uint8_t a = enabled ? 255 : 102;
  bg = blend565(bg, B_BG, a);
  drawBox(x, y, w, h, 5, bg, blend565(edge, B_BG, a), B_BG);
  pText(UIB_R12, label, centre ? x + w / 2 : x + 14, y + (h + 12) / 2, blend565(fg, B_BG, a), bg,
        centre ? C_BASELINE : L_BASELINE);
  endFonts();
}

// A full-width list row: optional two-digit index (Settings), the label, and a right-hand
// value (tone, chevron). Settings rows put the label after the index.
// `name`, when given, follows `left` in B_USER (Select user: "1 · " + the name).
static void drawRow(int16_t y, const char* left, const char* right, uint16_t rightCol,
                    const char* index = nullptr, int16_t w = CW, int16_t x = CX,
                    const char* name = nullptr) {
  drawBox(x, y, w, ROW_H, 5, P_ROW_BG, P_ROW_BORDER, B_BG);
  int16_t lx = x + 13;
  if (index) {
    pText(UIB_R12, index, lx, y + 23, P_ROW_INDEX, P_ROW_BG);
    lx = x + 41;
  }
  useFont(UIB_R12);
  int16_t rw = right ? textW(tft, right) + 12 : 0;
  int16_t maxW = x + w - 13 - rw - lx;
  if (name) {
    drawNameLine(UIB_R14, left, name, "", lx, y + 24, maxW, B_TEXT, B_USER, P_ROW_BG);
  } else {
    char shown[48];
    useFont(UIB_R14);
    fitText(tft, left, maxW, shown, sizeof(shown));
    pText(UIB_R14, shown, lx, y + 24, B_TEXT, P_ROW_BG);
  }
  if (right) pText(UIB_R12, right, x + w - 13, y + 23, rightCol, P_ROW_BG, R_BASELINE);
  endFonts();
}

// The footer pager: ‹ 7, "n/N", 9 ›. Drawn dimmed (and inert) when there is only one page,
// as the HTML disables them.
static void drawPager(uint8_t page, uint8_t count) {
  bool on = count > 1;
  drawAction(PREV_X, FOOT_BTN_Y, PAGER_W, FOOT_BTN_H, "\xE2\x80\xB9 7", Btn::Muted, true, on);
  drawAction(NEXT_X, FOOT_BTN_Y, PAGER_W, FOOT_BTN_H, "9 \xE2\x80\xBA", Btn::Muted, true, on);
  char t[8];
  snprintf(t, sizeof(t), "%u/%u", page + 1, count);
  pText(UIB_R11, t, PAGE_NUM_X, FOOT_BASE, B_TEXT, B_BG);
  endFonts();
}

static bool inRect(int16_t x, int16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static bool tappedBack(int16_t x, int16_t y) { return y < PAGE_HEAD_H && x < TITLE_X; }

// Returns -1 (previous), +1 (next) or 0 for a tap on the pager.
static int8_t tappedPager(int16_t x, int16_t y) {
  if (inRect(x, y, PREV_X, FOOT_BTN_Y, PAGER_W, FOOT_BTN_H)) return -1;
  if (inRect(x, y, NEXT_X, FOOT_BTN_Y, PAGER_W, FOOT_BTN_H)) return 1;
  return 0;
}

static bool tappedFootPrimary(int16_t x, int16_t y) {
  return inRect(x, y, PRIMARY_FOOT_X, FOOT_BTN_Y, PRIMARY_FOOT_W, FOOT_BTN_H);
}

// Row r of a list, or -1.
static int8_t tappedRow(int16_t y, uint8_t rows) {
  for (uint8_t r = 0; r < rows; r++) {
    if (y >= rowY(r) && y < rowY(r) + ROW_H) return (int8_t)r;
  }
  return -1;
}

// The step tag in the header, "SLOT 4 · STEP 2/4". Direct-card flow has one step fewer.
static void stepTag(char* out, size_t n, uint8_t step) {
  unsigned total = slotWasPreset ? 3 : 4;
  unsigned shown = slotWasPreset ? step - 1u : step;
  snprintf(out, n, "SLOT %d \xC2\xB7 STEP %u/%u", selSlot + 1, shown, total);
}

// Steps 3 and 4 show who and what instead of the step count (Dough, 2026-09-30):
// "SLOT 4 · User B · RBS", without the part until one is chosen or when it is skipped.
// The name is in B_USER and is what gets cut with "…" to the space right of the longest title
// ("Curing position", which ends near x 216 at 22 px). Drawn after the page frame.
static constexpr int16_t WHO_TAG_W = 240;
static void drawWhoTag() {
  const char* pos = selPosition ? Station::POSITION_NAMES[selPosition - 1] : nullptr;
  char prefix[16], suffix[24];
  snprintf(prefix, sizeof(prefix), "SLOT %d \xC2\xB7 ", selSlot + 1);
  snprintf(suffix, sizeof(suffix), "%s%s", pos ? " \xC2\xB7 " : "", pos ? pos : "");
  drawNameLine(UIB_R11, prefix, selUser >= 0 ? Station::user(selUser).name : "", suffix, TAG_R,
               TAG_BASE, WHO_TAG_W, P_TAG, B_USER, B_BG, /*right=*/true);
  endFonts();
}

// --- Choose slot: 3 + 3, running slots disabled -------------------------------------
static const CardRect PICK[6] = {
  { 12, 60, 147, 82 }, { 167, 60, 146, 82 }, { 321, 60, 147, 82 },
  { 12, 148, 147, 82 }, { 167, 148, 146, 82 }, { 321, 148, 147, 82 },
};

static void drawChooseSlot() {
  drawPageFrame("Choose slot", nullptr);
  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
    const Station::Slot& s = Station::slot(i);
    const CardRect& c = PICK[i];
    bool running = (s.state == Station::SlotState::Running);
    uint8_t a = running ? 102 : 255;
    // Disabled = the whole button at opacity .4 over the page, so text blends with the page too.
    uint16_t bg = blend565(P_CHOICE_BG, B_BG, a), fg = blend565(B_TEXT, B_BG, a);
    drawBox(c.x, c.y, c.w, c.h, 5, bg, blend565(P_CHOICE_EDGE, B_BG, a), B_BG);

    // The user's name in B_USER, dimmed with the rest of a disabled (running) button.
    uint16_t ufg = blend565(B_USER, B_BG, a);
    char label[8], detail[40];
    snprintf(label, sizeof(label), "Slot %u", i + 1);
    const char* state = running ? " \xC2\xB7 in use" : " \xC2\xB7 done, restart";
    if (s.state != Station::SlotState::Empty) snprintf(detail, sizeof(detail), "%s%s", s.name, state);

    int16_t tx = c.x + 8, tw = c.w - 16;
    useFont(UIB_R11);
    if (s.state == Station::SlotState::Empty) {
      pText(UIB_R13, label, tx, c.y + 36, fg, bg);
      pText(UIB_R11, "Empty", tx, c.y + 61, fg, bg);
    } else if (runW(detail) <= tw) {
      pText(UIB_R13, label, tx, c.y + 36, fg, bg);
      drawNameLine(UIB_R11, "", s.name, state, tx, c.y + 61, tw, fg, ufg, bg);
    } else {
      // Two lines, broken after the name — the HTML breaks wherever the width runs out.
      pText(UIB_R13, label, tx, c.y + 28, fg, bg);
      drawNameLine(UIB_R11, "", s.name, " \xC2\xB7", tx, c.y + 52, tw, fg, ufg, bg);
      pText(UIB_R11, running ? "in use" : "done, restart", tx, c.y + 69, fg, bg);
    }
    endFonts();
  }
  drawSummary("Step 1 of 4 \xC2\xB7 Keypad 1\xE2\x80\x93" "6 selects a slot");
}

// --- Select user: five per page with their tones -------------------------------------
static uint8_t pageCount(uint8_t perPage) {
  uint8_t n = Station::userCount();
  return n == 0 ? 1 : (n + perPage - 1) / perPage;
}

static void drawChooseUser() {
  char tag[24];
  stepTag(tag, sizeof(tag), 2);
  drawPageFrame("Select user", tag);

  uint8_t count = pageCount(USER_ROWS);
  if (userPage >= count) userPage = 0;
  uint8_t total = Station::userCount();
  for (uint8_t r = 0; r < USER_ROWS; r++) {
    uint8_t idx = userPage * USER_ROWS + r;
    if (idx >= total) break;
    const Station::User& u = Station::user(idx);
    // Numbered per page: the digit that picks the row is r + 1 on every page.
    char left[8], tone[16];
    snprintf(left, sizeof(left), "%u \xC2\xB7 ", r + 1);
    snprintf(tone, sizeof(tone), "\xE2\x99\xAA %s", Station::SOUND_NAMES[u.sound]);
    drawRow(rowY(r), left, tone, P_TONE, nullptr, CW, CX, u.name);
  }
  if (total == 0) {
    pText(UIB_R11, "No users saved. Add a user from Settings \xE2\x86\x92 User names & tones.",
          CX, 86, P_NOTE, B_BG);
    endFonts();
  }
  drawPager(userPage, count);
}

// --- Curing position (optional): 3 + 2 buttons, Skip in the footer -------------------
static const CardRect POS_RECT[Station::POSITION_COUNT] = {
  { 12, 94, 147, 62 }, { 167, 94, 146, 62 }, { 321, 94, 147, 62 },
  { 12, 164, 224, 62 }, { 244, 164, 224, 62 },
};

static void drawChoosePosition() {
  drawPageFrame("Curing position", nullptr);
  drawWhoTag();

  pText(UIB_R14, "Which part is being cured?", CX, 81, B_NAME, B_BG);
  // The pill is sized to its Bold text (the HTML's 58 px holds the regular weight only).
  useFont(UIB_R11);
  int16_t ow = textW(tft, "Optional") + 16;
  drawBox(CX + CW - ow, 62, ow, 25, 4, B_BG, P_OPT_EDGE, B_BG);
  pText(UIB_R11, "Optional", CX + CW - ow + 8, 80, B_PRIMARY_BG, B_BG);
  for (uint8_t p = 0; p < Station::POSITION_COUNT; p++) {
    const CardRect& c = POS_RECT[p];
    bool sel = (selPosition == p + 1);   // Back from the time entry shows the previous choice
    uint16_t bg = sel ? P_SEL_BG : P_CHOICE_BG, fg = sel ? P_SEL_FG : B_TEXT;
    drawBox(c.x, c.y, c.w, c.h, 5, bg, sel ? P_SEL_BG : P_CHOICE_EDGE, B_BG);
    char num[4];
    snprintf(num, sizeof(num), "0%u", p + 1);
    pText(UIB_R11, num, c.x + 11, c.y + 24, blend565(fg, bg, 179), bg);   // opacity .7
    pText(UIB_B17, Station::POSITION_NAMES[p], c.x + 11, c.y + 51, fg, bg);
  }
  pText(UIB_R10, "Choose a position, or skip if it is not needed.", CX, 249, P_NOTE, B_BG);
  endFonts();

  drawSummary("1\xE2\x80\x93" "5 to choose \xC2\xB7 # to skip");
  drawAction(PRIMARY_FOOT_X, FOOT_BTN_Y, PRIMARY_FOOT_W, FOOT_BTN_H, "Skip \xE2\x86\x92",
             Btn::Primary, true);
}

// --- numeric pages (.entry): cure time, setpoint, alarm duration, heat off timer --------

static uint8_t numMaxDigits() {
  return (screen == Screen::SetTemp || screen == Screen::SetAlarm || screen == Screen::SetVolume)
             ? 2 : 3;
}

// The line under the value: this screen's hint, or the error after a rejected entry. Both
// in the same colour, as the HTML's .error div carries both.
static void numHint(char* out, size_t n) {
  switch (screen) {
    case Screen::EnterTime: {
      const char* pos = selPosition ? Station::POSITION_NAMES[selPosition - 1] : nullptr;
      char full[64];
      snprintf(full, sizeof(full), "%s%s%s \xC2\xB7 1\xE2\x80\x93" "480 min",
               selUser >= 0 ? Station::user(selUser).name : "", pos ? " \xC2\xB7 " : "", pos ? pos : "");
      useFont(UIB_R11);
      fitText(tft, full, NUM_W, out, n);
      return;
    }
    case Screen::SetTemp:
      snprintf(out, n, "Current %u\xC2\xB0" "C \xC2\xB7 Range 40\xE2\x80\x93" "50\xC2\xB0" "C",
               Station::setpointC());
      return;
    case Screen::SetAlarm:
      snprintf(out, n, "Current %u sec \xC2\xB7 Range 5\xE2\x80\x93" "60 sec", Station::alarmDurationSec());
      return;
    case Screen::SetVolume:
      snprintf(out, n, "Current %u \xC2\xB7 Range 10\xE2\x80\x93" "20", Station::alarmVolume());
      return;
    default:   // HeatTimer (handoff 5: with the mat disabled, say that a timer turns it on)
      snprintf(out, n, Station::heatEnabled() ? "Range 1\xE2\x80\x93" "480 minutes"
                                              : "Setting a timer turns the mat ON");
      return;
  }
}

// The value box and the hint line — repainted on every key. Empty shows an em dash.
static void drawNumValue() {
  drawBox(NUM_X, NUM_Y, NUM_W, NUM_H, 6, P_FIELD_BG, P_FIELD_EDGE, B_BG);
  pText(UIB_B30, numBuf[0] ? numBuf : "\xE2\x80\x94", NUM_X + 9, NUM_BASE, B_WHITE, P_FIELD_BG);
  tft.fillRect(NUM_X, NUM_HINT_BASE - 15, NUM_W, 20, B_BG);
  if (!numErr && screen == Screen::EnterTime && selUser >= 0) {
    // "Dough · RBS · 1–480 min" with the name in B_USER.
    const char* pos = selPosition ? Station::POSITION_NAMES[selPosition - 1] : nullptr;
    char suffix[48];
    snprintf(suffix, sizeof(suffix), "%s%s \xC2\xB7 1\xE2\x80\x93" "480 min", pos ? " \xC2\xB7 " : "",
             pos ? pos : "");
    drawNameLine(UIB_R11, "", Station::user(selUser).name, suffix, NUM_X, NUM_HINT_BASE, NUM_W,
                 P_HINT, B_USER, B_BG);
  } else {
    char hint[64];
    if (numErr) snprintf(hint, sizeof(hint), "%s", numErr);
    else        numHint(hint, sizeof(hint));
    pText(UIB_R11, hint, NUM_X, NUM_HINT_BASE, P_HINT, B_BG);
  }
  endFonts();
}

// 1 2 3 / 4 5 6 / 7 8 9 / ⌫ 0 OK, the OK key in primary green. Phone order, so it matches the
// physical 4x3 keypad beside the screen (was calculator order 7 8 9 on top until 2026-10-07).
static void drawPad(const char* okLabel) {
  static const char* const keys[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9",
                                        "\xE2\x8C\xAB", "0", nullptr };
  for (uint8_t k = 0; k < 12; k++) {
    int16_t x = PAD_X[k % 3], y = PAD_Y0 + (k / 3) * PAD_PITCH;
    if (k == 11) {
      drawBox(x, y, PAD_W, PAD_H, 5, B_PRIMARY_BG, B_PRIMARY_BG, B_BG);
      pText(UIB_R13, okLabel, x + PAD_W / 2, y + 28, B_PRIMARY_FG, B_PRIMARY_BG, C_BASELINE);
    } else {
      drawBox(x, y, PAD_W, PAD_H, 5, P_PAD_KEY, P_PAD_KEY, B_BG);
      pText(UIB_R20, keys[k], x + PAD_W / 2, y + 32, B_TEXT, P_PAD_KEY, C_BASELINE);
    }
  }
  endFonts();
}

// '0'-'9', 'D' (backspace), 'K' (Set / Start) or 0 for a tap on the pad.
static char padKeyAt(int16_t x, int16_t y) {
  static const char keys[12] = { '1', '2', '3', '4', '5', '6', '7', '8', '9', 'D', '0', 'K' };
  for (uint8_t k = 0; k < 12; k++) {
    if (inRect(x, y, PAD_X[k % 3], PAD_Y0 + (k / 3) * PAD_PITCH, PAD_W, PAD_H)) return keys[k];
  }
  return 0;
}

static void drawNumPage(const char* title, const char* tag, const char* label, const char* ok) {
  drawPageFrame(title, tag);
  useFont(UIB_R11);
  tft.setTextColor(P_LABEL, B_BG);
  drawSpaced(tft, label, CX, NUM_LABEL_BASE, 0.77f);   // uppercase, letter-spacing .07em
  endFonts();
  drawNumValue();
  drawPad(ok);
  drawSummary("Digits to enter \xC2\xB7 # to confirm \xC2\xB7 * to go back");
}

// Warm-up warning (Dough, 2026-10-02: "only show warning"). A cure counts from the first time
// the air reaches the cure-sheet column below the setpoint (45 C at SP 49 — the "cure-ready
// point" in Paper/Report_Plan.md). Starting earlier is still allowed; this line only says so.
// No reading -> no line: the header already shows the fault.
static constexpr int16_t WARM_BASE = 265;              // under the presets, above the foot rule
static constexpr int16_t WARM_NONE = -32768;
static int16_t warmShownTenths = WARM_NONE;            // what the line shows; WARM_NONE = blank

static uint8_t cureReadyC() { return (uint8_t)((Station::setpointC() - 1) / 5 * 5); }

static void drawWarmWarning() {
  bool warn = shadowTempValid && shadowTemp < cureReadyC();
  int16_t tenths = warn ? (int16_t)lroundf(shadowTemp * 10.0f) : WARM_NONE;
  if (tenths == warmShownTenths) return;
  warmShownTenths = tenths;
  tft.fillRect(NUM_X, WARM_BASE - 15, NUM_W, 20, B_BG);
  if (!warn) return;
  char t[40];
  snprintf(t, sizeof(t), "Still warming: %.1f of %u \xC2\xB0" "C", shadowTemp, cureReadyC());
  pText(UIB_R11, t, NUM_X, WARM_BASE, P_WARM, B_BG);
  endFonts();
}

static void drawEnterTime() {
  drawNumPage("Set cure time", nullptr, "CURE TIME \xC2\xB7 MINUTES", "Start #");
  drawWhoTag();
  // Presets fill the entry only; Start still has to be pressed (handoff 6).
  for (uint8_t i = 0; i < 4; i++) {
    const CardRect& c = PRESETS[i];
    char t[12];
    snprintf(t, sizeof(t), "%u min", PRESET_MIN[i]);
    drawBox(c.x, c.y, c.w, c.h, 5, P_CHOICE_BG, P_CHOICE_EDGE, B_BG);
    pText(UIB_R12, t, c.x + c.w / 2, c.y + 22, B_TEXT, P_CHOICE_BG, C_BASELINE);
  }
  endFonts();
  warmShownTenths = WARM_NONE;   // the page was just repainted blank there
  drawWarmWarning();
}

static void drawSetTemp() {
  drawNumPage("Temperature setpoint", nullptr, "NEW TEMPERATURE \xC2\xB7 \xC2\xB0" "C", "Set #");
}

static void drawSetAlarm() {
  drawNumPage("Alarm duration", nullptr, "NEW DURATION \xC2\xB7 SECONDS", "Set #");
  // Tones are per user, which a screen that only sets a duration does not make obvious.
  drawAction(CX, TONES_BTN_Y, TONES_BTN_W, TONES_BTN_H, "User alarm tones \xE2\x80\xBA", Btn::Muted,
             false);
  char vol[24];
  snprintf(vol, sizeof(vol), "Alarm volume %u \xE2\x80\xBA", Station::alarmVolume());
  drawAction(CX, VOL_BTN_Y, TONES_BTN_W, TONES_BTN_H, vol, Btn::Muted, false);
}

// Opened from Alarm settings, and returns there (Set or Back) so the new value shows on
// its button. The sketch sends it to the DFPlayer, never alongside a play command.
static void drawSetVolume() {
  drawNumPage("Alarm volume", nullptr, "NEW VOLUME \xC2\xB7 10\xE2\x80\x93" "20", "Set #");
}

static void drawHeatTimer() {
  drawNumPage("Set heat off timer", nullptr, "SWITCH MAT OFF IN \xC2\xB7 MINUTES", "Set #");
}

// --- Cancel cure? ------------------------------------------------------------------
static uint32_t cancelShownSec = 0;

static void drawCancelTime() {
  cancelShownSec = Station::slot(selSlot).remainingSec;
  char rem[9], t[24];
  Station::formatRemaining(cancelShownSec, rem);
  snprintf(t, sizeof(t), "%s remaining", rem);
  liveText(UIB_B22, t, CX, 120, CW, P_WARM);
}

static void drawDetail(uint8_t slotIdx) {
  char prefix[16];
  snprintf(prefix, sizeof(prefix), "Slot %u \xC2\xB7 ", slotIdx + 1);
  drawNameLine(UIB_B16, prefix, Station::slot(slotIdx).name, "", CX, DETAIL_BASE, CW, B_TEXT,
               B_USER, B_BG);
  endFonts();
}

static void drawConfirmCancel() {
  if (selSlot < 0) return;
  drawPageFrame("Cancel cure?", nullptr);
  drawDetail((uint8_t)selSlot);
  drawCancelTime();
  pText(UIB_R11, "This stops the cure and clears the slot \xE2\x80\x94 cannot be undone.", CX, 155,
        P_NOTE, B_BG);
  endFonts();
  // Safe option first and on top, so the reflexive tap keeps the cure running.
  drawAction(CX, 174, CW, ACT_H, "1 \xC2\xB7 No, keep running", Btn::Primary, false);
  drawAction(CX, 222, CW, ACT_H, "2 \xC2\xB7 Yes, cancel this cure", Btn::Danger, false);
  drawSummary("# does nothing here");
}

// The completion overlay (Concept B, handoff 9): full screen, green, every unacknowledged
// cure as a card in completion order. One card fills the grid; more use two columns and
// ceil(n/2) rows with 6 px gaps. Tapping a card or its × acknowledges that one alarm —
// the slot stays Ready until Clear finished. There is no dismiss-all and no Back.
// Geometry measured in headless Chrome with 1, 3 and 7 cards.
static constexpr int16_t AL_X = 12, AL_Y = 85, AL_W = 456, AL_H = 223, AL_GAP = 6;
static constexpr int16_t AL_TOP_BASE = 25, AL_H1_BASE = 57, AL_CAPTION_BASE = 75;
static constexpr int16_t AL_X_SIZE = 32, AL_X_INSET = 5, AL_X_BASE = 27;  // × box, inset incl. border

static CardRect alarmCardRect(uint8_t k, uint8_t n) {
  if (n <= 1) return { AL_X, AL_Y, AL_W, AL_H };
  uint8_t rows = (n + 1) / 2;
  int16_t colW = (AL_W - AL_GAP) / 2;                          // 225
  float rowH = (float)(AL_H - AL_GAP * (rows - 1)) / rows;     // 51.25 at seven, 71.67 at six
  int16_t top = AL_Y + (int16_t)lroundf((k / 2) * (rowH + AL_GAP));
  int16_t bot = AL_Y + (int16_t)lroundf((k / 2) * (rowH + AL_GAP) + rowH);
  return { (int16_t)(AL_X + (k % 2) * (colW + AL_GAP)), top, colW, (int16_t)(bot - top) };
}

static void drawAlarmCard(uint8_t k, uint8_t n, uint8_t slotIdx) {
  const Station::Slot& s = Station::slot(slotIdx);
  CardRect c = alarmCardRect(k, n);
  bool single = (n == 1);
  drawBox(c.x, c.y, c.w, c.h, 6, B_ALERT_CARD, B_ALERT_EDGE, B_ALERT_BG);

  // Text is centred between 40 px side paddings (46 for a single card), which keeps it
  // clear of the × box. Offsets from the card's vertical centre, as the browser lays out
  // the three lines.
  float cy = c.y + c.h / 2.0f;
  int16_t cx = c.x + c.w / 2;
  int16_t maxW = c.w - 2 - 2 * (single ? 46 : 40);
  char line[48], shown[48];

  // "Slot 7 · RBS" when a position was chosen, "Slot 7 · Ready" otherwise (handoff 6).
  snprintf(line, sizeof(line), "Slot %u \xC2\xB7 %s", slotIdx + 1,
           s.position ? Station::POSITION_NAMES[s.position - 1] : "Ready");
  useFont(single ? UIB_R20 : UIB_R13);
  tft.setTextColor(B_ALERT_SOFT, B_ALERT_CARD);
  // Offsets re-set for the x1.2 text (2026-10-09): the three lines still fit a 71.7 px card.
  drawText(tft, line, cx, (int16_t)lroundf(cy + (single ? -23.0f : -14.0f)), C_BASELINE);

  useFont(single ? UIB_B32 : UIB_B18);
  fitText(tft, s.name, maxW, shown, sizeof(shown));
  tft.setTextColor(B_USER, B_ALERT_CARD);
  drawText(tft, shown, cx, (int16_t)lroundf(cy + (single ? 16.0f : 9.5f)), C_BASELINE);

  // The key that stops this card. With several, the SLOT number, which never changes as
  // other cards are acknowledged. With one, '#' (it takes the oldest, i.e. this one) —
  // Dough, 2026-09-23; the slot digit still works too.
  char key[4];
  if (single) snprintf(key, sizeof(key), "#");
  else        snprintf(key, sizeof(key), "%u", slotIdx + 1);
  snprintf(line, sizeof(line), "\xE2\x99\xAA %s \xC2\xB7 %us \xC2\xB7 Stop (%s)",
           Station::SOUND_NAMES[s.sound], Station::alarmDurationSec(), key);
  useFont(single ? UIB_R14 : UIB_R10);
  fitText(tft, line, maxW, shown, sizeof(shown));
  tft.setTextColor(B_ALERT_SOFT, B_ALERT_CARD);
  drawText(tft, shown, cx, (int16_t)lroundf(cy + (single ? 43.5f : 26.0f)), C_BASELINE);

  int16_t bx = c.x + c.w - AL_X_INSET - AL_X_SIZE;
  drawBox(bx, c.y + AL_X_INSET, AL_X_SIZE, AL_X_SIZE, 4, B_ALERT_X_BG, B_ALERT_X_EDGE,
          B_ALERT_CARD);
  useFont(UIB_X22);
  tft.setTextColor(B_WHITE, B_ALERT_X_BG);
  drawText(tft, "\xC3\x97", bx + AL_X_SIZE / 2, c.y + AL_X_BASE, C_BASELINE);
  endFonts();
}

// What the overlay is currently showing, so a cure finishing while it is open adds a
// card instead of going unnoticed — and so an unchanged queue is not repainted every
// loop, which would flicker the whole panel.
static uint8_t alarmSig[Station::SLOT_COUNT];
static uint8_t alarmSigN = 0xFF;

static bool alarmSigDiffers() {
  uint8_t n = Station::pendingAlarmCount();
  if (n != alarmSigN) return true;
  for (uint8_t k = 0; k < n; k++) {
    if (alarmSig[k] != (uint8_t)Station::pendingAlarmAt(k)) return true;
  }
  return false;
}

// Paints the whole overlay, background included (callers do not clear first).
static void drawAlarm() {
  uint8_t n = Station::pendingAlarmCount();
  alarmSigN = n;
  for (uint8_t k = 0; k < n; k++) alarmSig[k] = (uint8_t)Station::pendingAlarmAt(k);
  if (n == 0) return;

  tft.fillScreen(B_ALERT_BG);
  char t[40];
  useFont(UIB_R10);
  tft.setTextColor(B_ALERT_SOFT, B_ALERT_BG);
  drawText(tft, "FUJIFILM \xC2\xB7 ENDOSCOPE SERVICE", AL_X, AL_TOP_BASE, L_BASELINE);
  snprintf(t, sizeof(t), "%u FINISHED", n);
  drawText(tft, t, AL_X + AL_W, AL_TOP_BASE, R_BASELINE);

  if (n > 1) snprintf(t, sizeof(t), "%u cures complete", n);
  else       snprintf(t, sizeof(t), "Cure complete");
  useFont(UIB_B28);
  tft.setTextColor(B_WHITE, B_ALERT_BG);
  drawText(tft, t, SCR_W / 2, AL_H1_BASE, C_BASELINE);

  useFont(UIB_R11);
  tft.setTextColor(B_ALERT_SOFT, B_ALERT_BG);
  drawText(tft, "Tap a card or \xC3\x97 to stop its alarm. The slot stays Ready.", SCR_W / 2,
           AL_CAPTION_BASE, C_BASELINE);
  endFonts();

  for (uint8_t k = 0; k < n; k++) {
    int8_t slotIdx = Station::pendingAlarmAt(k);
    if (slotIdx < 0) break;
    drawAlarmCard(k, n, (uint8_t)slotIdx);
  }
}

// --- Station settings -----------------------------------------------------------------
// "Alarm volume" is row 5 since 2026-09-25 so the keypad can reach it (* then 5): its
// only other way in is a button on the Alarm settings page, whose digits are all taken by the
// seconds entry. Heat off timer moved 5 -> 6. Six 34 px rows reach y 289, past the footer rule
// (273), so this page has no footer: the "Saved on the station" line was dropped (option A)
// rather than thinning every row.
static constexpr uint8_t MENU_COUNT = 6;
static const char* const MENU_ITEMS[MENU_COUNT] = {
  "User names & tones",
  "Temperature setting",
  "Temperature log",
  "Alarm settings",
  "Alarm volume",
  "Heat off timer",
};

// Maker's credit under the last row (Dough, 2026-10-27): who built the station and how to reach
// him once the co-op is over. A credit, not a copyright notice — that needs the company's OK.
// ~330 px in R11, centred; the rows end at y 289, the HTML's .credit line sits at baseline ~311.
static const char* const STATION_CREDIT = "By Phatcharapon Putyuen \xC2\xB7 KMITL Co-op 2026";
static constexpr int16_t CREDIT_BASE = 311;

static void drawMenu() {
  drawPageFrame("Station settings", nullptr, true, /*footRule=*/false);
  pText(UIB_R11, STATION_CREDIT, SCR_W / 2, CREDIT_BASE, P_SUMMARY, B_BG, C_BASELINE);
  endFonts();
  for (uint8_t i = 0; i < MENU_COUNT; i++) {
    char index[4], value[16];
    snprintf(index, sizeof(index), "0%u", i + 1);
    switch (i) {
      case 1:  snprintf(value, sizeof(value), "%u\xC2\xB0" "C \xE2\x80\xBA", Station::setpointC()); break;
      case 3:  snprintf(value, sizeof(value), "%u sec \xE2\x80\xBA", Station::alarmDurationSec()); break;
      case 4:  snprintf(value, sizeof(value), "%u \xE2\x80\xBA", Station::alarmVolume()); break;
      case 5:  snprintf(value, sizeof(value), "%s \xE2\x80\xBA", Station::heatEnabled() ? "ON" : "OFF"); break;
      default: snprintf(value, sizeof(value), "\xE2\x80\xBA"); break;
    }
    // Row 6 reads "OFF ›" in red while the mat is switched off, like the header's MAT OFF.
    bool off = (i == 5 && !Station::heatEnabled());
    drawRow(rowY(i), MENU_ITEMS[i], value, off ? B_MAT_OFF : P_ROW_VALUE, index);
  }
}

// --- Temperature log ------------------------------------------------------------------
// The HTML's SVG: viewBox 450 x 154 centred in the 456 px content box (x 15, y 102), fixed
// 20-55 °C scale with the 40-50 band shaded — an auto-scaling axis would make a flat hold
// look like wild swings. Gaps (NAN samples, probe not answering) break the trace.
static constexpr int16_t LOG_X = 15, LOG_Y = 102, LOG_H = 154;
static constexpr int16_t LOG_PLOT_X0 = LOG_X + 36, LOG_PLOT_W = 396;
static const int16_t LOG_STAT_X[3] = { 67, 218, 371 };

static int16_t logY(float c) {
  float y = LOG_Y + 132.0f - (c - 20.0f) / 35.0f * 112.0f;
  if (y < LOG_Y) y = LOG_Y;                       // the SVG clips at its own box
  if (y > LOG_Y + LOG_H - 1) y = LOG_Y + LOG_H - 1;
  return (int16_t)lroundf(y);
}

// What the chart is currently showing, so a new sample redraws it while it is open. Once
// the buffer is full the count stops changing, so the newest value is tracked too (a NAN
// never equals itself, hence the isnan() pair).
static uint16_t tlShownN = 0xFFFF;
static float tlShownLast = 0.0f;

static bool tempLogChanged() {
  uint16_t n = Station::tempLogCount();
  if (n != tlShownN) return true;
  if (n == 0) return false;
  float v = Station::tempLogAt(n - 1);
  if (isnan(v) || isnan(tlShownLast)) return isnan(v) != isnan(tlShownLast);
  return v != tlShownLast;
}

static void drawTempLog() {
  drawPageFrame("Temperature log", nullptr);
  char t[40];
  snprintf(t, sizeof(t), "Every %u sec \xC2\xB7 Up to %u h \xC2\xB7 RAM only",
           Station::tempLogIntervalSec(),
           (unsigned)((uint32_t)Station::TEMP_LOG_MAX * Station::tempLogIntervalSec() / 3600));
  drawSummary(t);

  uint16_t n = Station::tempLogCount();
  tlShownN = n;
  tlShownLast = n > 0 ? Station::tempLogAt(n - 1) : 0.0f;

  float mn = 0, mx = 0;
  uint16_t valid = 0;
  for (uint16_t k = 0; k < n; k++) {
    float v = Station::tempLogAt(k);
    if (isnan(v)) continue;
    if (valid == 0 || v < mn) mn = v;
    if (valid == 0 || v > mx) mx = v;
    valid++;
  }
  if (valid < 2) {
    pText(UIB_R11, "Collecting \xE2\x80\x94 first points take a few minutes.", CX, 89, P_NOTE, B_BG);
    endFonts();
    return;
  }

  // MIN / CURRENT / MAX. CURRENT is the live reading, as the HTML's is.
  static const char* const labels[3] = { "MIN", "CURRENT", "MAX" };
  static const uint16_t cols[3] = { P_LOG_MIN, P_LOG_CUR, P_LOG_MAX };
  for (uint8_t i = 0; i < 3; i++) {
    float v = (i == 0) ? mn : (i == 2) ? mx : shadowTemp;
    if (i == 1 && !shadowTempValid) snprintf(t, sizeof(t), "--.-\xC2\xB0");
    else                            snprintf(t, sizeof(t), "%.1f\xC2\xB0", v);
    pText(UIB_R10, labels[i], LOG_STAT_X[i], 72, P_LOG_LABEL, B_BG);
    pText(UIB_B18, t, LOG_STAT_X[i], 94, cols[i], B_BG);
  }

  tft.fillRect(LOG_PLOT_X0, logY(50.0f), LOG_PLOT_W, logY(40.0f) - logY(50.0f), P_BAND);
  static const uint8_t marks[4] = { 20, 40, 50, 55 };
  for (uint8_t m : marks) {
    snprintf(t, sizeof(t), "%u\xC2\xB0", m);
    pText(UIB_R10, t, LOG_X + 3, logY(m) + 4, P_CHART_TEXT, B_BG);
    tft.drawFastHLine(LOG_PLOT_X0, logY(m), LOG_PLOT_W, P_GRID);
  }

  // The trace, 2 px wide like the SVG's stroke: every segment drawn twice, one row apart.
  // A NAN on either end of a segment leaves it out, which is the gap.
  for (uint16_t k = 0; k + 1 < n; k++) {
    float a = Station::tempLogAt(k), b = Station::tempLogAt(k + 1);
    if (isnan(a) || isnan(b)) continue;
    int16_t x0 = LOG_PLOT_X0 + (int16_t)((int32_t)k * LOG_PLOT_W / (n - 1));
    int16_t x1 = LOG_PLOT_X0 + (int16_t)((int32_t)(k + 1) * LOG_PLOT_W / (n - 1));
    tft.drawLine(x0, logY(a), x1, logY(b), P_LOG_CUR);
    tft.drawLine(x0, logY(a) + 1, x1, logY(b) + 1, P_LOG_CUR);
  }

  uint32_t mins = (uint32_t)(n - 1) * Station::tempLogIntervalSec() / 60;
  if (mins >= 60) snprintf(t, sizeof(t), "~%lu h %lu min ago", (unsigned long)(mins / 60),
                           (unsigned long)(mins % 60));
  else            snprintf(t, sizeof(t), "~%lu min ago", (unsigned long)mins);
  pText(UIB_R10, t, LOG_PLOT_X0, LOG_Y + 150, P_CHART_TEXT, B_BG);
  pText(UIB_R10, "Now", LOG_X + 412, LOG_Y + 150, P_CHART_TEXT, B_BG);
  endFonts();
}

// --- User names & tones -----------------------------------------------------------------
// Four per page: the name + tone button and a separate Del. Touching the name changes the
// tone; Del deletes at once, as in the HTML (a running cure keeps its own copy of the name).
// A keypad digit opens User options instead, since a digit cannot say which of the two
// targets was meant.
static constexpr int16_t UROW_Y0 = 60, UROW_H = 46, UROW_PITCH = 50;
static constexpr int16_t UNAME_W = 409, DEL_X = 428, DEL_W = 40;

static void drawManageUsers() {
  uint8_t total = Station::userCount();
  uint8_t count = pageCount(MANAGE_ROWS);
  if (managePage >= count) managePage = count - 1;
  char tag[16];
  snprintf(tag, sizeof(tag), "%u/%u USERS", total, Station::MAX_USERS);
  drawPageFrame("User names & tones", tag);

  for (uint8_t r = 0; r < MANAGE_ROWS; r++) {
    uint8_t idx = managePage * MANAGE_ROWS + r;
    if (idx >= total) break;
    const Station::User& u = Station::user(idx);
    int16_t y = UROW_Y0 + r * UROW_PITCH;
    drawBox(CX, y, UNAME_W, UROW_H, 5, P_ROW_BG, P_ROW_BG, B_BG);
    drawBox(DEL_X, y, DEL_W, UROW_H, 5, P_DEL_BG, P_DEL_BG, B_BG);
    char left[8], tone[16];
    snprintf(left, sizeof(left), "%u \xC2\xB7 ", r + 1);   // per page, as on Select user
    snprintf(tone, sizeof(tone), "\xE2\x99\xAA %s", Station::SOUND_NAMES[u.sound]);
    useFont(UIB_R11);
    int16_t toneW = textW(tft, tone);
    drawNameLine(UIB_R13, left, u.name, "", CX + 10, y + 29, UNAME_W - 30 - toneW, B_TEXT, B_USER,
                 P_ROW_BG);
    pText(UIB_R11, tone, CX + UNAME_W - 10, y + 29, P_TONE, P_ROW_BG, R_BASELINE);
    pText(UIB_R11, "Del", DEL_X + DEL_W / 2, y + 29, B_TEXT, P_DEL_BG, C_BASELINE);
  }
  if (total == 0) {
    pText(UIB_R11, "No users yet. Add a name to start a cure.", CX, 89, P_NOTE, B_BG);
  }
  endFonts();

  drawPager(managePage, count);
  drawAction(PRIMARY_FOOT_X, FOOT_BTN_Y, PRIMARY_FOOT_W, FOOT_BTN_H, "+ Add user #", Btn::Primary,
             true, total < Station::MAX_USERS);
}

// Keypad route into a user row: 1 change tone, 2 delete. Safe option first, as everywhere.
static void drawUserActions() {
  if (editUser < 0 || editUser >= (int8_t)Station::userCount()) return;
  drawPageFrame("User options", nullptr);
  pText(UIB_B16, Station::user(editUser).name, CX, DETAIL_BASE, B_USER, B_BG);
  endFonts();
  drawAction(CX, 91, CW, ACT_H, "1 \xC2\xB7 Change alarm tone", Btn::Primary, false);
  drawAction(CX, 139, CW, ACT_H, "2 \xC2\xB7 Delete this user", Btn::Danger, false);
}

// The tones: changing an existing user's (✓ on the current one) or picking the new
// user's (step 2 of adding one).
// Each tone is a row with a 44 px play button beside it (Dough, 2026-09-24): tap the row to
// choose, tap the play button to hear the tone first.
// 10 tones since 2026-09-25: two columns of 5 (1-5 left, 6-10 right) at the normal
// row height, so taps stay as accurate as before — a single column only fits 5 above the
// footer. The right column's play button sits where the old single-column one was (x 424).
// The printed number is the keypad key, so the tenth tone shows 0.
static constexpr uint8_t TONE_ROWS = 5;
static constexpr int16_t TONE_COL_W = 224, TONE_COL_GAP = 8;               // 2 x 224 + 8 = CW
static constexpr int16_t PLAY_W = 44, TONE_ROW_W = TONE_COL_W - 6 - PLAY_W;  // 174

static int16_t toneColX(uint8_t i) { return CX + (i / TONE_ROWS) * (TONE_COL_W + TONE_COL_GAP); }
static int16_t tonePlayX(uint8_t i) { return toneColX(i) + TONE_COL_W - PLAY_W; }

static void drawPlayButton(int16_t x, int16_t y) {
  drawBox(x, y, PLAY_W, ROW_H, 5, P_ROW_BG, P_ROW_BORDER, B_BG);
  int16_t cx = x + PLAY_W / 2, cy = y + ROW_H / 2;   // the HTML's ▶, ~9 px wide
  tft.fillTriangle(cx - 4, cy - 5, cx - 4, cy + 5, cx + 5, cy, B_PRIMARY_BG);
}

static void drawToneList(const char* title, const char* who, int8_t current) {
  drawPageFrame(title, nullptr);
  for (uint8_t i = 0; i < Station::SOUND_COUNT; i++) {
    char left[24];
    snprintf(left, sizeof(left), "%u \xC2\xB7 %s", (i + 1) % 10, Station::SOUND_NAMES[i]);
    int16_t y = rowY(i % TONE_ROWS);
    drawRow(y, left, current == (int8_t)i ? "\xE2\x9C\x93 In use" : "\xE2\x80\xBA", P_TONE,
            nullptr, TONE_ROW_W, toneColX(i));
    drawPlayButton(tonePlayX(i), y);
  }
  char t[24];
  snprintf(t, sizeof(t), " \xC2\xB7 Alarm %u sec", Station::alarmDurationSec());
  drawNameLine(UIB_R11, "", who, t, SUMMARY_X, FOOT_BASE, 330, P_SUMMARY, B_USER, B_BG);
  endFonts();
}

static void drawEditSound() {
  if (editUser < 0 || editUser >= (int8_t)Station::userCount()) return;
  const Station::User& u = Station::user(editUser);
  drawToneList("Change alarm tone", u.name, (int8_t)u.sound);
}

static void drawAddUserSound() { drawToneList("Choose alarm tone", nameBuf, -1); }

// --- Add user name: touch keyboard ---------------------------------------------------------
// Number row, QWERTY rows, then ’ / Space / ⌫ — the HTML's rows and widths. The physical
// keypad cannot type letters, so this page stays touch-only ('*' still goes back).
static constexpr int16_t KB_FIELD_Y = 60, KB_FIELD_H = 32, KB_Y0 = 98, KB_PITCH = 31, KB_H = 27;
static const char* const KB_ROWS[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
static const CardRect KB_BOTTOM[3] = { { 12, 222, 81, 27 }, { 97, 222, 286, 27 }, { 387, 222, 81, 27 } };

// Key c of letter row r: the row's keys share the 456 px width with 4 px gaps, edges rounded
// the way the browser lays them out.
static CardRect kbKey(uint8_t r, uint8_t c) {
  uint8_t n = strlen(KB_ROWS[r]);
  float w = (CW - 4.0f * (n - 1)) / n;
  int16_t l = (int16_t)lroundf(CX + c * (w + 4)), rgt = (int16_t)lroundf(CX + c * (w + 4) + w);
  return { l, (int16_t)(KB_Y0 + r * KB_PITCH), (int16_t)(rgt - l), KB_H };
}

static void drawNameField() {
  drawBox(CX, KB_FIELD_Y, CW, KB_FIELD_H, 5, P_FIELD_BG, P_FIELD_EDGE, B_BG);
  pText(UIB_B16, nameBuf, CX + 11, KB_FIELD_Y + 24, B_WHITE, P_FIELD_BG);
  endFonts();
}

static void drawKbKey(const CardRect& k, const char* label) {
  drawBox(k.x, k.y, k.w, k.h, 3, P_KB_KEY, P_KB_KEY, B_BG);
  pText(UIB_R12, label, k.x + k.w / 2, k.y + 20, B_TEXT, P_KB_KEY, C_BASELINE);
}

static void drawNameSummary() {
  drawSummary(nameErr ? nameErr : "Maximum 14 characters \xC2\xB7 Touch keyboard", 330);
}

static void drawAddUserName() {
  drawPageFrame("Add user name", nullptr);
  drawNameField();
  char label[2] = { 0, 0 };
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t c = 0; KB_ROWS[r][c]; c++) {
      label[0] = KB_ROWS[r][c];
      drawKbKey(kbKey(r, c), label);
    }
  }
  drawKbKey(KB_BOTTOM[0], "\xE2\x80\x99");
  drawKbKey(KB_BOTTOM[1], "Space");
  drawKbKey(KB_BOTTOM[2], "\xE2\x8C\xAB");
  endFonts();
  drawNameSummary();
  drawAction(PRIMARY_FOOT_X, FOOT_BTN_Y, PRIMARY_FOOT_W, FOOT_BTN_H, "Next \xE2\x86\x92", Btn::Primary,
             true);
}

// --- Power was interrupted ---------------------------------------------------------------
// Shown once at boot when cures came back out of NVS still running.
//
// What the restored countdown actually is, measured on hardware 2026-08-02: it is
// the value at the last 30 s save, so it is up to 30 s LONGER than the cure really
// had left. Not shorter — the error is toward over-curing, which is the safe side.
//
// So the risk is not the clock. The board has no battery-backed clock and cannot
// tell how long the power was off, and during that time the heater was off and the
// resin was not curing. A short blip is nothing; hours mean the batch itself may be
// no good, and no timer can tell the operator which happened. That is what this
// screen is for, and it is why the wording asks them to check the resin rather than
// the numbers. See the power-loss section of Project_Status.md.
//
// Countdowns (and the heat off timer) are HELD while this is up — UIFuji::holdingCountdowns()
// — and the heater keeps holding the setpoint (Dough, 2026-09-23).
static constexpr int16_t PL_LIST_Y = 120, PL_LIST_PITCH = 18;
// The two answers sit in the footer, sized to their labels as the HTML's flex buttons are.
static constexpr int16_t PL_KEEP_X = 10;
static const char* const PL_KEEP  = "1 \xC2\xB7 Keep these times";
static const char* const PL_CLEAR = "2 \xC2\xB7 Clear all & start over";

static int16_t plBtnW(const char* s) {
  useFont(UIB_R12);
  int16_t w = textW(tft, s) + 28;
  endFonts();
  return w;
}
static int16_t plKeepW()  { return plBtnW(PL_KEEP); }
static int16_t plClearW() { return plBtnW(PL_CLEAR); }

static void drawPowerLoss() {
  drawPageFrame("Power was interrupted", nullptr, false);   // no Back: answer 1 or 2

  pText(UIB_R11, "These cures were restored from before the power cut.", CX, 74, P_WARM, B_BG);
  pText(UIB_R11, "Nothing was curing while the power was off.", CX, 91, P_WARM, B_BG);
  pText(UIB_R11, "Check the resin before you trust these times.", CX, 108, P_WARM, B_BG);

  uint8_t shown = 0;
  char line[40], rem[9];
  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
    const Station::Slot& s = Station::slot(i);
    if (s.state != Station::SlotState::Running) continue;
    int16_t y = PL_LIST_Y + shown * PL_LIST_PITCH;
    snprintf(line, sizeof(line), "Slot %u \xC2\xB7 ", i + 1);
    drawNameLine(UIB_R11, line, s.name, "", CX, y + 13, 300, B_TEXT, B_USER, B_BG);
    Station::formatRemaining(s.remainingSec, rem);
    snprintf(line, sizeof(line), "%s left", rem);
    pText(UIB_R11, line, CX + CW, y + 13, B_TEXT, B_BG, R_BASELINE);
    tft.drawFastHLine(CX, y + 17, CW, P_LIST_RULE);
    shown++;
  }
  endFonts();

  // Keep is first and green: it is the reflexive tap, and the option that loses nothing.
  drawAction(PL_KEEP_X, FOOT_BTN_Y, plKeepW(), FOOT_BTN_H, PL_KEEP, Btn::Primary, false);
  drawAction(SCR_W - 10 - plClearW(), FOOT_BTN_Y, plClearW(), FOOT_BTN_H, PL_CLEAR, Btn::Danger, false);
}

// --- Heating mat and its off timer ---------------------------------------------------------
// Handoff 8. Settings row 6 (row 5 until 2026-09-25) opens the minutes entry directly while the mat is ON, and this
// state page while it is OFF; Back from the entry opens this page, whose Back returns to
// Settings. Home's heating area opens the entry with Back / success returning Home.

static uint16_t heatPendingMin = 0;  // minutes typed, held while HeatWarn asks
static bool heatFromBadge = false;   // HeatTimer opened from Home's heating area: back = Main
static int8_t heatWarnSlot = -1;     // the cure HeatWarn is warning about

// What HeatMenu is showing, so a second ticking by repaints only the countdown and a
// timer running out while the screen is open redraws the rows (they change).
static bool heatShownOn = true;
static uint32_t heatShownSec = 0;

static void drawHeatMenuValue() {
  heatShownOn = Station::heatEnabled();
  heatShownSec = Station::heatOffRemainingSec();
  char t[32];
  if (!heatShownOn)      snprintf(t, sizeof(t), "Mat is OFF");
  else if (heatShownSec) snprintf(t, sizeof(t), "Off in %lu:%02lu:%02lu", (unsigned long)(heatShownSec / 3600),
                                  (unsigned long)(heatShownSec % 3600 / 60), (unsigned long)(heatShownSec % 60));
  else                   snprintf(t, sizeof(t), "Mat is ON \xC2\xB7 no off timer");
  liveText(UIB_B23, t, CX, 93, CW, heatShownOn ? P_HEAT_STATE : B_MAT_OFF);   // "Mat is OFF" red
}

static void drawHeatMenu() {
  drawPageFrame("Heating mat", nullptr);
  drawHeatMenuValue();
  int16_t noteBase = 186;
  if (!heatShownOn) {
    drawAction(CX, 118, CW, ACT_H, "1 \xC2\xB7 Turn heat ON", Btn::Primary, false);
  } else {
    drawAction(CX, 118, CW, ACT_H, heatShownSec ? "1 \xC2\xB7 Change off timer" : "1 \xC2\xB7 Set off timer",
               Btn::Primary, false);
    if (heatShownSec) {
      drawAction(CX, 166, CW, ACT_H, "2 \xC2\xB7 Cancel timer", Btn::Muted, false);
      noteBase = 234;
    }
  }
  pText(UIB_R11, "The mat stays off after the timer expires until you turn it back on.", CX, noteBase,
        P_NOTE, B_BG);
  endFonts();
  drawSummary("Turning heat off does not cancel cure timers.");
}

static void drawHeatWarn() {
  if (heatWarnSlot < 0) return;
  drawPageFrame("Heat would stop early", nullptr);
  drawDetail((uint8_t)heatWarnSlot);

  char line[48], rem[9];
  Station::formatRemaining(Station::slot(heatWarnSlot).remainingSec, rem);
  snprintf(line, sizeof(line), "%s left on this cure.", rem);
  pText(UIB_R11, line, CX, 111, P_NOTE, B_BG);
  snprintf(line, sizeof(line), "The mat would switch off in %u minute%s.", heatPendingMin,
           heatPendingMin == 1 ? "" : "s");
  pText(UIB_R11, line, CX, 129, P_NOTE, B_BG);
  endFonts();

  // Safe option first and on top, as on the cancel confirmation.
  snprintf(line, sizeof(line), "2 \xC2\xB7 Yes, switch off in %u min", heatPendingMin);
  drawAction(CX, 146, CW, ACT_H, "1 \xC2\xB7 No, change the time", Btn::Primary, false);
  drawAction(CX, 194, CW, ACT_H, line, Btn::Danger, false);
  drawSummary("# does nothing here");
}

// --- Over-temperature warning (2026-10-01) ----------------------------------------------
// Two live lines repaint in place (temperature, sound state); the rest is drawn once.
static constexpr int16_t OT_TEMP_BASE = 132, OT_STATUS_BASE = 270;

static void drawOverTempLive() {
  char t[56];
  snprintf(t, sizeof(t), "Air %.1f\xC2\xB0" "C \xC2\xB7 above %.0f\xC2\xB0" "C for %lu min", otTempC,
           UIFuji::OVERTEMP_C, (unsigned long)(UIFuji::OVERTEMP_HOLD_MS / 60000UL));
  tft.fillRect(0, OT_TEMP_BASE - 24, SCR_W, 32, OT_BG);
  pText(UIB_B18, t, SCR_W / 2, OT_TEMP_BASE, B_WHITE, OT_BG, C_BASELINE);
  snprintf(t, sizeof(t), otSilenced ? "Sound silenced \xC2\xB7 warning stays until below %.0f\xC2\xB0" "C"
                                    : "Alarm sounding \xC2\xB7 press any key or tap to silence", UIFuji::OVERTEMP_C);
  tft.fillRect(0, OT_STATUS_BASE - 17, SCR_W, 24, OT_BG);
  pText(UIB_R12, t, SCR_W / 2, OT_STATUS_BASE, OT_SOFT, OT_BG, C_BASELINE);
  endFonts();
}

static void drawOverTemp() {
  tft.fillScreen(OT_BG);
  pText(UIB_B28, "OVER-TEMPERATURE", SCR_W / 2, 66, B_WHITE, OT_BG, C_BASELINE);
  pText(UIB_R12, "The controller could not hold the temperature down.", SCR_W / 2, 98, OT_SOFT, OT_BG,
        C_BASELINE);
  pText(UIB_B18, "1  Unplug the heating mat now", 44, 182, B_WHITE, OT_BG);
  pText(UIB_B18, "2  Have the heater relay (SSR) checked", 44, 214, B_WHITE, OT_BG);
  pText(UIB_R12, "Cure timers keep counting behind this warning.", SCR_W / 2, 300, OT_SOFT, OT_BG, C_BASELINE);
  endFonts();
  drawOverTempLive();
}

static void silenceOverTemp() {
  if (!otActive || otSilenced) return;
  otSilenced = true;
  if (screen == Screen::OverTemp) drawOverTempLive();
}

// --- Sensor-fault warning (2026-10-06) ----------------------------------------------------
// Same red page as the over-temperature warning; only the sound line repaints.
static void drawSensorFaultLive() {
  const char* t = sfSilenced ? "Sound silenced \xC2\xB7 warning stays until the reading returns"
                             : "Alarm sounding \xC2\xB7 press any key or tap to silence";
  tft.fillRect(0, OT_STATUS_BASE - 17, SCR_W, 24, OT_BG);
  pText(UIB_R12, t, SCR_W / 2, OT_STATUS_BASE, OT_SOFT, OT_BG, C_BASELINE);
  endFonts();
}

static void drawSensorFault() {
  tft.fillScreen(OT_BG);
  pText(UIB_B28, "SENSOR FAULT", SCR_W / 2, 66, B_WHITE, OT_BG, C_BASELINE);
  pText(UIB_R12, "No temperature reading. The heater is OFF.", SCR_W / 2, 98, OT_SOFT, OT_BG, C_BASELINE);
  pText(UIB_B18, "Cures are NOT being heated", SCR_W / 2, OT_TEMP_BASE, B_WHITE, OT_BG, C_BASELINE);
  pText(UIB_B18, "1  Unplug, then check the probe wire", 44, 182, B_WHITE, OT_BG);
  pText(UIB_B18, "2  If it does not come back, restart", 44, 214, B_WHITE, OT_BG);
  pText(UIB_R12, "Cure timers keep counting. Check the parts before use.", SCR_W / 2, 300, OT_SOFT, OT_BG,
        C_BASELINE);
  endFonts();
  drawSensorFaultLive();
}

static void silenceSensorFault() {
  if (!sfActive || sfSilenced) return;
  sfSilenced = true;
  if (screen == Screen::SensorFault) drawSensorFaultLive();
}

static void drawCurrentScreen() {
  switch (screen) {
    case Screen::ChooseSlot:     drawChooseSlot();     break;
    case Screen::ChooseUser:     drawChooseUser();     break;
    case Screen::ChoosePosition: drawChoosePosition(); break;
    case Screen::EnterTime:      drawEnterTime();      break;
    case Screen::ConfirmCancel:  drawConfirmCancel();  break;
    case Screen::Alarm:          drawAlarm();          break;
    case Screen::Menu:           drawMenu();           break;
    case Screen::SetTemp:        drawSetTemp();        break;
    case Screen::TempLog:        drawTempLog();        break;
    case Screen::SetAlarm:       drawSetAlarm();       break;
    case Screen::ManageUsers:    drawManageUsers();    break;
    case Screen::EditSound:      drawEditSound();      break;
    case Screen::AddUserName:    drawAddUserName();    break;
    case Screen::AddUserSound:   drawAddUserSound();   break;
    case Screen::UserActions:    drawUserActions();    break;
    case Screen::PowerLoss:      drawPowerLoss();      break;
    case Screen::HeatMenu:       drawHeatMenu();       break;
    case Screen::HeatTimer:      drawHeatTimer();      break;
    case Screen::HeatWarn:       drawHeatWarn();       break;
    case Screen::SetVolume:      drawSetVolume();      break;
    case Screen::OverTemp:       drawOverTemp();       break;
    case Screen::SensorFault:    drawSensorFault();    break;
    default: break;
  }
}

static void closeOverlay();  // defined with the public section

static void goToScreen(Screen s) {
  screen = s;
  if (s == Screen::Main) { closeOverlay(); return; }
  // The completion overlay and the two red warnings paint their own background.
  if (s != Screen::Alarm && s != Screen::OverTemp && s != Screen::SensorFault) tft.fillScreen(B_BG);
  drawCurrentScreen();
}

// --- navigation: the same functions serve touch and the keypad ------------------------

static void openStartFlow(int8_t presetSlot) {
  selSlot = presetSlot;
  selUser = -1;
  selPosition = 0;
  userPage = 0;
  slotWasPreset = (presetSlot >= 0);
  goToScreen(slotWasPreset ? Screen::ChooseUser : Screen::ChooseSlot);
}

static void pickSlot(uint8_t i) {
  if (i >= Station::SLOT_COUNT) return;
  if (Station::slot(i).state == Station::SlotState::Running) return;   // cancel it first
  selSlot = (int8_t)i;
  userPage = 0;
  goToScreen(Screen::ChooseUser);
}

// Row r (0-4) of the visible page. Choosing a user resets the position (handoff 6).
static void pickUser(uint8_t r) {
  uint8_t idx = userPage * USER_ROWS + r;
  if (r >= USER_ROWS || idx >= Station::userCount()) return;
  selUser = (int8_t)idx;
  selPosition = 0;
  goToScreen(Screen::ChoosePosition);
}

static void openEnterTime() {
  numBuf[0] = '\0';
  numErr = nullptr;
  goToScreen(Screen::EnterTime);
}

// 1..POSITION_COUNT, or 0 for Skip.
static void pickPosition(uint8_t p) {
  if (p > Station::POSITION_COUNT) return;
  selPosition = p;
  openEnterTime();
}

static void openConfirmCancel(uint8_t i) {
  selSlot = (int8_t)i;
  goToScreen(Screen::ConfirmCancel);
}

// Shows every unacknowledged alarm at once. Only ever called from the main screen, so
// it cannot interrupt someone mid-way through starting a cure.
static void openAlarm() {
  if (!Station::hasPendingAlarm()) return;
  goToScreen(Screen::Alarm);
}

static void openMenu() { goToScreen(Screen::Menu); }

static void openNumber(Screen s) {
  numBuf[0] = '\0';
  numErr = nullptr;
  goToScreen(s);
}

static void openHeatTimer() { openNumber(Screen::HeatTimer); }

static void openManageUsers() {
  managePage = 0;
  goToScreen(Screen::ManageUsers);
}

static void openAddUser() {
  if (Station::userCount() >= Station::MAX_USERS) return;
  nameBuf[0] = '\0';
  nameErr = nullptr;
  goToScreen(Screen::AddUserName);
}

static void openEditSound(uint8_t idx, bool fromActions) {
  editUser = (int8_t)idx;
  toneFromActions = fromActions;
  goToScreen(Screen::EditSound);
}

// The Heating mat state page — except in the one state where it has nothing to offer (mat ON,
// no off timer: only "Set off timer", which Settings row 6 already opens directly). Dough asked
// for that "Mat is ON · no off timer" page to go (2026-09-23), a deliberate difference from the
// HTML: in that state every route that led to it lands on Settings instead.
static void openHeatState() {
  if (Station::heatEnabled() && Station::heatOffRemainingSec() == 0) goToScreen(Screen::Menu);
  else                                                               goToScreen(Screen::HeatMenu);
}

static void menuPick(uint8_t i) {
  switch (i) {
    case 0: openManageUsers();             break;
    case 1: openNumber(Screen::SetTemp);   break;
    case 2: goToScreen(Screen::TempLog);   break;
    case 3: openNumber(Screen::SetAlarm);  break;
    case 4: volFromMenu = true; openNumber(Screen::SetVolume); break;
    case 5:
      // Handoff 8: straight to the minutes while the mat is on; the state page while it is off.
      // While the off timer counts down, open the Heating mat page so "Change off timer" /
      // "Cancel timer" are offered; going straight to the minutes had hidden Cancel.
      heatFromBadge = false;
      if (Station::heatEnabled() && Station::heatOffRemainingSec() == 0) openHeatTimer();
      else                                                               openHeatState();
      break;
  }
}

// After a timer is set: Home when it was opened from Home, else the Heating mat page, which
// then shows the countdown with Change / Cancel (handoff 8).
static void heatDone() {
  if (heatFromBadge) goToScreen(Screen::Main);
  else               openHeatState();
}

static void heatWarnAccept() {
  Station::startHeatOffTimer(heatPendingMin);   // turns a disabled mat back on
  heatDone();
}

// '*' and every Back button. Each page goes back one level, as the HTML's backAction.
static void goBack() {
  switch (screen) {
    case Screen::ChooseSlot:     goToScreen(Screen::Main); break;
    case Screen::ChooseUser:     goToScreen(slotWasPreset ? Screen::Main : Screen::ChooseSlot); break;
    case Screen::ChoosePosition: goToScreen(Screen::ChooseUser); break;
    case Screen::EnterTime:      goToScreen(Screen::ChoosePosition); break;  // keeps the choice
    case Screen::ConfirmCancel:  goToScreen(Screen::Main); break;
    case Screen::Menu:           goToScreen(Screen::Main); break;
    case Screen::SetTemp:
    case Screen::SetAlarm:
    case Screen::TempLog:
    case Screen::ManageUsers:    goToScreen(Screen::Menu); break;
    case Screen::HeatMenu:       goToScreen(heatFromBadge ? Screen::Main : Screen::Menu); break;
    case Screen::UserActions:    goToScreen(Screen::ManageUsers); break;
    case Screen::EditSound:      goToScreen(toneFromActions ? Screen::UserActions : Screen::ManageUsers); break;
    case Screen::AddUserName:    goToScreen(Screen::ManageUsers); break;
    case Screen::AddUserSound:   goToScreen(Screen::AddUserName); break;   // the typed name is kept
    case Screen::HeatTimer:
      // A running timer: back to the page that opened "Change off timer" (it is the one with Cancel).
      if (Station::heatOffRemainingSec() > 0) goToScreen(Screen::HeatMenu);
      else if (heatFromBadge)                 goToScreen(Screen::Main);
      else                                    openHeatState();
      break;
    case Screen::HeatWarn:       openHeatTimer(); break;
    case Screen::SetVolume:      if (volFromMenu) goToScreen(Screen::Menu); else openNumber(Screen::SetAlarm); break;
    default: break;   // Main, Alarm, PowerLoss: nothing to go back to
  }
}

// --- the numeric entry ---

static void numAppend(char c) {
  uint8_t n = strlen(numBuf);
  if (n >= numMaxDigits()) return;
  if (n == 0 && c == '0') return;   // a leading zero is ignored (handoff 6)
  numBuf[n] = c;
  numBuf[n + 1] = '\0';
  numErr = nullptr;
}

// Out-of-range or empty: clear the number and show this screen's error (handoff 6, 8).
static bool numRead(int lo, int hi, const char* err, int& out) {
  int n = atoi(numBuf);
  if (numBuf[0] == '\0' || n < lo || n > hi) {
    numErr = err;
    numBuf[0] = '\0';
    drawNumValue();
    return false;
  }
  out = n;
  return true;
}

static void numConfirm() {
  int n = 0;
  switch (screen) {
    case Screen::EnterTime: {
      if (!numRead(Station::TIME_MIN_MIN, Station::TIME_MAX_MIN, "Enter 1-480 min", n)) return;
      const Station::User& u = Station::user(selUser);
      if (!Station::startSlot((uint8_t)selSlot, u.name, u.sound, (uint16_t)n, selPosition)) {
        numErr = "Could not start - check the slot";
        drawNumValue();
        return;
      }
      goToScreen(Screen::Main);
      return;
    }
    case Screen::SetTemp:
      if (!numRead(Station::SETPOINT_MIN_C, Station::SETPOINT_MAX_C, "Must be 40-50 C", n)) return;
      Station::setSetpointC((uint8_t)n);
      goToScreen(Screen::Menu);
      return;
    case Screen::SetAlarm:
      if (!numRead(Station::ALARM_MIN_SEC, Station::ALARM_MAX_SEC, "Must be 5-60 sec", n)) return;
      Station::setAlarmDurationSec((uint8_t)n);
      goToScreen(Screen::Menu);
      return;
    case Screen::SetVolume:
      if (!numRead(Station::ALARM_VOL_MIN, Station::ALARM_VOL_MAX, "Must be 10-20", n)) return;
      Station::setAlarmVolume((uint8_t)n);
      if (volFromMenu) goToScreen(Screen::Menu);   // back where it was opened from
      else             openNumber(Screen::SetAlarm);
      return;
    case Screen::HeatTimer: {
      if (!numRead(Station::TIME_MIN_MIN, Station::TIME_MAX_MIN, "Enter 1-480 min", n)) return;
      // Warn if the mat would go off before the running cure with the most time left.
      heatPendingMin = (uint16_t)n;
      int8_t longest = Station::longestRunningSlot();
      if (longest >= 0 && Station::slot(longest).remainingSec > (uint32_t)n * 60UL) {
        heatWarnSlot = longest;
        goToScreen(Screen::HeatWarn);
        return;
      }
      Station::startHeatOffTimer(heatPendingMin);   // turns a disabled mat back on
      heatDone();
      return;
    }
    default:
      return;
  }
}

// A digit, 'D' (backspace) or 'K' / '#' (Set / Start) — from the pad or the keypad.
static void numKey(char k) {
  if (k == 'K' || k == '#') { numConfirm(); return; }
  if (k == 'D') {
    uint8_t n = strlen(numBuf);
    if (n > 0) numBuf[n - 1] = '\0';
    numErr = nullptr;
  } else {
    numAppend(k);
  }
  drawNumValue();
}

static bool isNumScreen() {
  return screen == Screen::EnterTime || screen == Screen::SetTemp || screen == Screen::SetAlarm ||
         screen == Screen::HeatTimer || screen == Screen::SetVolume;
}

// --- the rest of the per-page actions ---

static void manageRowKey(uint8_t r) {   // keypad: open User options for row r
  uint8_t idx = managePage * MANAGE_ROWS + r;
  if (r >= MANAGE_ROWS || idx >= Station::userCount()) return;
  editUser = (int8_t)idx;
  goToScreen(Screen::UserActions);
}

static void deleteEditUser() {
  if (editUser >= 0) Station::deleteUser((uint8_t)editUser);
  editUser = -1;
  goToScreen(Screen::ManageUsers);
}

static void tonePick(uint8_t i) {
  if (i >= Station::SOUND_COUNT) return;
  if (screen == Screen::AddUserSound) {
    if (Station::addUser(nameBuf, i)) managePage = (Station::userCount() - 1) / MANAGE_ROWS;
    else Serial.println("[UI] user list is full, name not added");
  } else if (editUser >= 0) {
    Station::setUserSound((uint8_t)editUser, i);
  }
  goToScreen(Screen::ManageUsers);
}

static void heatMenuRow(uint8_t r) {
  if (!Station::heatEnabled()) {
    // Turn heat ON clears any off timer; the page would now read "Mat is ON · no off timer",
    // which Dough removed, so this lands on Settings (row 6 reads ON).
    // From Home's heating area it goes straight back Home (Dough, 2026-09-23).
    if (r == 0) {
      Station::setHeatEnabled(true);
      if (heatFromBadge) goToScreen(Screen::Main);
      else               openHeatState();
    }
    return;
  }
  if (r == 0) { openHeatTimer(); return; }
  if (r == 1 && Station::heatOffRemainingSec() > 0) {
    Station::cancelHeatOffTimer();   // the timer only — the mat stays on, cures keep running
    if (heatFromBadge) goToScreen(Screen::Main);   // opened from Home: back Home
    else               openHeatState();            // mat ON, no timer: back to Settings
  }
}

static void powerLossAnswer(bool clearAll) {
  // Everything, not just the running slots: a cure that finished during the outage is in
  // the same boat and its alarm would fire the moment this closes.
  if (clearAll) for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) Station::cancelSlot(i);
  goToScreen(Screen::Main);
}

static void nameAppend(char c) {
  uint8_t n = strlen(nameBuf);
  if (n >= Station::USER_NAME_MAX) return;
  nameBuf[n] = c;
  nameBuf[n + 1] = '\0';
}

// Next: the name is trimmed; nothing left is refused with the HTML's message.
static void nameNext() {
  char* p = nameBuf;
  while (*p == ' ') p++;
  memmove(nameBuf, p, strlen(p) + 1);
  for (size_t n = strlen(nameBuf); n > 0 && nameBuf[n - 1] == ' '; n--) nameBuf[n - 1] = '\0';
  if (nameBuf[0] == '\0') {
    nameErr = "Enter a name, 1\xE2\x80\x93" "14 characters.";
    drawNameField();
    drawNameSummary();
    return;
  }
  goToScreen(Screen::AddUserSound);
}

// --- touch, per page -------------------------------------------------------------------

static void onPageTouch(int16_t x, int16_t y) {
  if (screen != Screen::PowerLoss && tappedBack(x, y)) { goBack(); return; }

  if (isNumScreen()) {
    char k = padKeyAt(x, y);
    if (k) { numKey(k); return; }
    if (screen == Screen::EnterTime) {
      for (uint8_t i = 0; i < 4; i++) {
        const CardRect& c = PRESETS[i];
        if (!inRect(x, y, c.x, c.y, c.w, c.h)) continue;
        snprintf(numBuf, sizeof(numBuf), "%u", PRESET_MIN[i]);   // fills only, Start still needed
        numErr = nullptr;
        drawNumValue();
        return;
      }
    }
    if (screen == Screen::SetAlarm && inRect(x, y, CX, TONES_BTN_Y, TONES_BTN_W, TONES_BTN_H)) {
      openManageUsers();
    }
    if (screen == Screen::SetAlarm && inRect(x, y, CX, VOL_BTN_Y, TONES_BTN_W, TONES_BTN_H)) {
      volFromMenu = false;
      openNumber(Screen::SetVolume);
    }
    return;
  }

  int8_t r;
  switch (screen) {
    case Screen::ChooseSlot:
      for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
        if (inRect(x, y, PICK[i].x, PICK[i].y, PICK[i].w, PICK[i].h)) { pickSlot(i); return; }
      }
      return;

    case Screen::ChooseUser:
      if (int8_t d = tappedPager(x, y)) {
        uint8_t count = pageCount(USER_ROWS);
        if (count < 2) return;
        userPage = (userPage + count + d) % count;   // wraps both ways
        goToScreen(Screen::ChooseUser);
        return;
      }
      if ((r = tappedRow(y, USER_ROWS)) >= 0) pickUser((uint8_t)r);
      return;

    case Screen::ChoosePosition:
      if (tappedFootPrimary(x, y)) { pickPosition(0); return; }
      for (uint8_t p = 0; p < Station::POSITION_COUNT; p++) {
        const CardRect& c = POS_RECT[p];
        if (inRect(x, y, c.x, c.y, c.w, c.h)) { pickPosition(p + 1); return; }
      }
      return;

    case Screen::ConfirmCancel:
      if (inRect(x, y, CX, 161, CW, ACT_H)) { goToScreen(Screen::Main); return; }
      if (inRect(x, y, CX, 209, CW, ACT_H)) {
        // cancelSlot also drops the slot from the alarm queue, so a cure cancelled in
        // its final second cannot leave an alarm pending for a slot that is now empty.
        Station::cancelSlot((uint8_t)selSlot);
        goToScreen(Screen::Main);
      }
      return;

    case Screen::Menu:
      if ((r = tappedRow(y, MENU_COUNT)) >= 0) menuPick((uint8_t)r);
      return;

    case Screen::ManageUsers: {
      if (tappedFootPrimary(x, y)) { openAddUser(); return; }
      if (int8_t d = tappedPager(x, y)) {
        uint8_t count = pageCount(MANAGE_ROWS);
        if (count < 2) return;
        managePage = (managePage + count + d) % count;
        goToScreen(Screen::ManageUsers);
        return;
      }
      for (uint8_t row = 0; row < MANAGE_ROWS; row++) {
        int16_t top = UROW_Y0 + row * UROW_PITCH;
        uint8_t idx = managePage * MANAGE_ROWS + row;
        if (y < top || y >= top + UROW_H || idx >= Station::userCount()) continue;
        if (inRect(x, y, DEL_X, top, DEL_W, UROW_H)) {
          // No confirmation, as in the HTML. A running cure copied the name into the
          // slot when it started, so deleting the user cannot disturb work in progress.
          Station::deleteUser(idx);
          goToScreen(Screen::ManageUsers);
        } else if (x < CX + UNAME_W) {
          openEditSound(idx, false);
        }
        return;
      }
      return;
    }

    case Screen::UserActions:
      if (inRect(x, y, CX, 86, CW, ACT_H)) { openEditSound((uint8_t)editUser, true); return; }
      if (inRect(x, y, CX, 134, CW, ACT_H)) deleteEditUser();
      return;

    case Screen::EditSound:
    case Screen::AddUserSound:
      if ((r = tappedRow(y, TONE_ROWS)) >= 0) {
        uint8_t i = (x < CX + TONE_COL_W + TONE_COL_GAP / 2 ? 0 : TONE_ROWS) + r;
        if (i >= Station::SOUND_COUNT) return;
        if (x >= tonePlayX(i)) tonePreview = i;    // the sketch plays it (takeTonePreview)
        else                   tonePick(i);
      }
      return;

    case Screen::AddUserName: {
      if (tappedFootPrimary(x, y)) { nameNext(); return; }
      bool typed = false;
      if (inRect(x, y, KB_BOTTOM[0].x, KB_BOTTOM[0].y, KB_BOTTOM[0].w, KB_BOTTOM[0].h)) {
        nameAppend('\''); typed = true;
      } else if (inRect(x, y, KB_BOTTOM[1].x, KB_BOTTOM[1].y, KB_BOTTOM[1].w, KB_BOTTOM[1].h)) {
        nameAppend(' '); typed = true;
      } else if (inRect(x, y, KB_BOTTOM[2].x, KB_BOTTOM[2].y, KB_BOTTOM[2].w, KB_BOTTOM[2].h)) {
        uint8_t n = strlen(nameBuf);
        if (n > 0) nameBuf[n - 1] = '\0';
        typed = true;
      } else {
        for (uint8_t kr = 0; kr < 4 && !typed; kr++) {
          for (uint8_t c = 0; KB_ROWS[kr][c]; c++) {
            CardRect k = kbKey(kr, c);
            if (!inRect(x, y, k.x, k.y, k.w, k.h)) continue;
            nameAppend(KB_ROWS[kr][c]);
            typed = true;
            break;
          }
        }
      }
      if (!typed) return;
      drawNameField();
      if (nameErr) { nameErr = nullptr; drawNameSummary(); }
      return;
    }

    case Screen::PowerLoss:
      if (inRect(x, y, PL_KEEP_X, FOOT_BTN_Y, plKeepW(), FOOT_BTN_H)) { powerLossAnswer(false); return; }
      if (inRect(x, y, SCR_W - 10 - plClearW(), FOOT_BTN_Y, plClearW(), FOOT_BTN_H)) powerLossAnswer(true);
      return;

    case Screen::HeatMenu:
      if (inRect(x, y, CX, 112, CW, ACT_H)) { heatMenuRow(0); return; }
      if (inRect(x, y, CX, 160, CW, ACT_H)) heatMenuRow(1);
      return;

    case Screen::HeatWarn:
      if (inRect(x, y, CX, 142, CW, ACT_H)) { openHeatTimer(); return; }
      if (inRect(x, y, CX, 190, CW, ACT_H)) heatWarnAccept();
      return;

    default:
      return;
  }
}

static void onAlarmTouch(int16_t x, int16_t y) {
  // Nothing outside the cards does anything: each alarm is acknowledged on its own, by
  // tapping its card or the × on it (both do the same thing).
  uint8_t n = Station::pendingAlarmCount();
  for (uint8_t k = 0; k < n; k++) {
    CardRect c = alarmCardRect(k, n);
    if (x < c.x || x >= c.x + c.w || y < c.y || y >= c.y + c.h) continue;

    int8_t slotIdx = Station::pendingAlarmAt(k);
    if (slotIdx < 0) return;
    // The slot stays Done and on Home until "Clear finished" — stopping the
    // alarm acknowledges the noise, it does not tidy the slot away.
    Station::acknowledgeAlarm((uint8_t)slotIdx);

    // Last one acknowledged closes the popup; otherwise redraw with the rest still
    // listed, exactly as the mockup re-renders in place.
    if (Station::hasPendingAlarm()) { drawAlarm(); }
    else                            { goToScreen(Screen::Main); }
    return;
  }
}

static Hit hitTest(int16_t x, int16_t y, uint8_t& slotOut) {
  if (y >= FOOTER_Y) {
    if (x < FOOT_SPLIT_1) return Hit::Menu;
    if (x < FOOT_SPLIT_2) return Hit::ClearDone;
    return Hit::Start;
  }
  if (y < HEADER_H) {
    // Handoff 5: the whole temperature/heating area opens the heat off timer. Nothing
    // else in the header is tappable.
    return (x >= HEAT_ZONE_X) ? Hit::Heat : Hit::None;
  }

  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
    const CardRect& c = CARDS[i];
    if (x < c.x || x >= c.x + c.w || y < c.y || y >= c.y + c.h) continue;
    slotOut = i;
    if (Station::slot(i).state != Station::SlotState::Running) return Hit::Slot;
    // A running card's only control is its ×. The zone is the drawn 27 x 25 box plus
    // 4 px of slack towards the card body; the rest of a running card does nothing.
    int16_t bx = c.x + c.w - C_X_R - C_X_W;
    if (x >= bx - 4 && y < c.y + C_X_Y + C_X_H + 4) return Hit::SlotCancel;
    return Hit::None;
  }
  return Hit::None;
}

static void handleHit(Hit h, uint8_t i) {
  switch (h) {
    case Hit::ClearDone:
      Station::clearAllDone();
      break;

    case Hit::Start:
      openStartFlow(-1); // no slot chosen yet, so the flow starts at step 1
      break;

    case Hit::Slot:
      // Tapping an empty or finished card jumps straight in with that slot chosen.
      // hitTest() never returns this for a running card.
      if (Station::slot(i).state != Station::SlotState::Running) {
        openStartFlow((int8_t)i);
      }
      break;

    case Hit::SlotCancel:
      openConfirmCancel(i);
      break;

    case Hit::Menu:
      openMenu();
      break;

    case Hit::Heat:
      // Mat ON: straight to the minutes (handoff 5). Mat OFF: the Heating mat page with
      // "Turn heat ON", which returns straight Home (Dough, 2026-09-23 — the handoff opened
      // the minutes entry here too). Back from either returns Home.
      // Timer counting down: the Heating mat page with Change / Cancel.
      heatFromBadge = true;
      if (Station::heatEnabled() && Station::heatOffRemainingSec() == 0) openHeatTimer();
      else                                                               goToScreen(Screen::HeatMenu);
      break;
    default:
      break;
  }
}

// --- public ---------------------------------------------------------------------

namespace UIFuji {

void begin() {
  tft.init();
  tft.setRotation(1); // landscape; CtpTouch::read() maps rotation 1 only
  // User_Setup.h has TFT_INVERSION_ON for the resistive panel; this panel shows every
  // colour as its exact complement with it. Turned off here, not in User_Setup.h, so
  // the resistive panel keeps working with Test_ui (same fix as Test_ctp_touch).
  tft.invertDisplay(false);
  // AFTER tft.init(): init() claims GPIO 15 (TOUCH_CS in User_Setup.h) as an output,
  // and this is what hands it back to I2C as SDA.
  if (!CtpTouch::begin()) {
    Serial.println("[UI] WARNING: FT6336 touch chip not found - touch will not work");
  }

  // The temperature box is as wide as its widest reading, right-aligned on TEMP_R, so the
  // badge/target column stays put as the number changes. The HTML lets that column
  // shift by a pixel or two with the reading; a fixed column is deliberate here.
  useFont(UIB_B28);
  tempBoxW = (int16_t)lroundf(max(spacedW(tft, "00.0", -1.0f), spacedW(tft, "--.-", -1.0f))) + 2;
  endFonts();

  // If any of these ever fails (no RAM), that box simply would not draw — worth knowing
  // rather than silently losing the countdown.
  makeSprite(timeSprite, C_TIME_W, C_TIME_H, UIB_SB23, "countdown");
  makeSprite(tempSprite, tempBoxW, TEMP_BOX_H, UIB_B28, "temperature");
  makeSprite(heatSprite, HSTAT_W, HSTAT_H, UIB_R10, "heat timer");
  tft.fillScreen(B_BG);
  lastTouchMs = millis();
  requestFullRedraw();
}

void setHeader(float currentTempC, bool heatOn, bool tempValid) {
  // Only a change of a tenth or more is worth a repaint; the raw float jitters.
  bool moved = (fabsf(currentTempC - shadowTemp) >= 0.05f);
  bool tempChanged = moved || tempValid != shadowTempValid;
  bool heatChanged = (heatOn != shadowHeat);
  if (!tempChanged && !heatChanged) return;
  shadowTemp = currentTempC;
  shadowHeat = heatOn;
  shadowTempValid = tempValid;

  // Only the main screen has a header. An overlay covers the whole panel, and the
  // temperature sits directly on top of the panel's X button, which it was painting
  // over once a second on every overlay screen. The new values are still recorded
  // above, so the header is correct as soon as the overlay closes. The badge and the
  // reading repaint separately, so a tenth of a degree does not flash the badge.
  if (!sleeping && screen == Screen::EnterTime && tempChanged) drawWarmWarning();
  if (sleeping || screen != Screen::Main) return;
  if (heatChanged) drawHeaderBadge();
  if (tempChanged) drawHeaderTemp();
}

void showPowerLossWarning() { goToScreen(Screen::PowerLoss); }

void setOverTemp(bool active, float tempC) {
  if (active && !otActive) {
    otActive = true;
    otSilenced = false;
    otTempC = tempC;
    // Interrupts any screen, half-typed entries included: a safety warning outranks them.
    // Only the power-loss question is worth coming back to, because it holds the countdowns.
    otReturn = (screen == Screen::PowerLoss) ? Screen::PowerLoss : Screen::Main;
    if (sleeping) wake();
    lastTouchMs = millis();
    goToScreen(Screen::OverTemp);
    return;
  }
  if (!active && otActive) {
    otActive = false;
    if (screen == Screen::OverTemp) goToScreen(otReturn);
    return;
  }
  if (active && screen == Screen::OverTemp && fabsf(tempC - otTempC) >= 0.05f) {
    otTempC = tempC;
    drawOverTempLive();
  }
}

bool overTempSilenced() { return otSilenced; }

void setSensorFault(bool active) {
  if (active && !sfActive) {
    sfActive = true;
    sfSilenced = false;
    sfReturn = (screen == Screen::PowerLoss) ? Screen::PowerLoss : Screen::Main;   // as setOverTemp
    if (sleeping) wake();
    lastTouchMs = millis();
    goToScreen(Screen::SensorFault);
    return;
  }
  if (!active && sfActive) {
    sfActive = false;
    if (screen == Screen::SensorFault) goToScreen(sfReturn);
  }
}

bool sensorFaultSilenced() { return sfSilenced; }

bool holdingCountdowns() { return screen == Screen::PowerLoss; }

bool asleep() { return sleeping; }

void setSleepTimeout(uint32_t ms) { sleepAfterMs = ms; }

void requestFullRedraw() {
  fullRedraw = true;
  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) shadowValid[i] = false;
}

} // namespace UIFuji

// Returns to the main screen and forces a repaint, since the overlay covered it.
static void closeOverlay() {
  screen = Screen::Main;
  UIFuji::requestFullRedraw();
}

namespace UIFuji {

// The sleep screen's wordmark (Dough, 2026-09-23): 200 x 34 at 35 % brightness on black
// (UI_LOGO_SLEEP, tools/make_ui_logo.py), moved to a new random spot every minute so no part
// of the LCD holds the same image for hours — avoiding that is what sleep is for.
// Only while the mat is ON: with the mat switched off the sleep screen is plain black (Dough,
// 2026-09-23) — the logo reads "on, resting", black reads "heat off".
static constexpr uint32_t SLEEP_LOGO_MOVE_MS = 60000;
static int16_t sleepLogoX = 0, sleepLogoY = 0;
static uint32_t sleepLogoMs = 0;
static bool sleepLogoShown = false;

static void eraseSleepLogo() {
  if (sleepLogoShown) tft.fillRect(sleepLogoX, sleepLogoY, UI_LOGO_SLEEP_W, UI_LOGO_SLEEP_H, TFT_BLACK);
  sleepLogoShown = false;
}

static void moveSleepLogo(bool erase) {
  if (erase) eraseSleepLogo();
  sleepLogoX = (int16_t)random(0, SCR_W - UI_LOGO_SLEEP_W + 1);
  sleepLogoY = (int16_t)random(0, SCR_H - UI_LOGO_SLEEP_H + 1);
  bool swap = tft.getSwapBytes();
  tft.setSwapBytes(true);
  tft.pushImage(sleepLogoX, sleepLogoY, UI_LOGO_SLEEP_W, UI_LOGO_SLEEP_H, UI_LOGO_SLEEP);
  tft.setSwapBytes(swap);
  sleepLogoShown = true;
  sleepLogoMs = millis();
}

static void goToSleep() {
  sleeping = true;
  // Black pixels only, NO DISPOFF (0x28): on this capacitive module "display off" shows
  // WHITE, not black (Dough, 2026-09-23), and the backlight is hardwired to 3V3 so nothing
  // hides it. A black frame under the lit backlight is the darkest this build can go.
  // ui.cpp (resistive panel, where DISPOFF gives black) keeps its DISPOFF.
  tft.fillScreen(TFT_BLACK);
  sleepLogoShown = false;
  if (Station::heatEnabled()) moveSleepLogo(false);
}

void wake() {
  if (!sleeping) { lastTouchMs = millis(); return; }
  sleeping = false;
  tft.fillScreen(B_BG);   // only ever asleep on Home; the display was never switched off
  requestFullRedraw();
  lastTouchMs = millis();
}

// Physical keypad input, following physKey() in the mockup. See ui.h for the three
// places this deliberately differs from it and why.
void handleKey(char k) {
  if (k == 0) return;
  Serial.printf("[UI] key %c screen=%u\n", k, (unsigned)screen);   // see the tap log in service()

  // Same rule as touch: the press that wakes the screen must not also act on whatever
  // is underneath it — you cannot aim at a screen you cannot see.
  if (sleeping) { wake(); return; }
  lastTouchMs = millis(); // keypad activity counts as activity, or it would sleep mid-entry

  // Any key only silences the over-temperature alarm; the warning itself cannot be dismissed.
  if (screen == Screen::OverTemp) { silenceOverTemp(); return; }
  if (screen == Screen::SensorFault) { silenceSensorFault(); return; }

  bool digit = (k >= '0' && k <= '9');
  uint8_t d = digit ? (uint8_t)(k - '0') : 0;

  switch (screen) {
    case Screen::Main:
      if (k == '#') { openStartFlow(-1); return; }
      // '0', not '*': '*' is back on every other screen, so it gets pressed here out
      // of habit and would clear the list on a reflex. '0' has no such habit and is
      // free — 1-6 are the slots. Only touches cures that already finished, as on the
      // touch path.
      if (k == '0') { Station::clearAllDone(); return; }
      // '*' is back everywhere else and the main screen is the root, so it is free.
      // Safe to bind here precisely because opening Settings destroys nothing and
      // '*' closes it again — it reads as a toggle. That is why Clear finished did NOT
      // go on this key.
      if (k == '*') { openMenu(); return; }
      if (k >= '1' && k <= '6') {
        uint8_t i = d - 1;
        // Same split as tapping the card: a running slot has to be cancelled first.
        if (Station::slot(i).state == Station::SlotState::Running) openConfirmCancel(i);
        else                                                       openStartFlow((int8_t)i);
      }
      return;

    case Screen::Alarm:
      // The digit is the SLOT number shown on the card ("Stop (3)"), not the card's
      // position — so it still has to be aimed at a specific finished cure, which is
      // what "acknowledged deliberately" means. There is deliberately no key that
      // silences everything at once, and no '*': the overlay has no Back either.
      {
        int8_t target = -1;
        if (k == '#') {
          // The top card, which is the oldest unacknowledged cure. Pressing # again
          // takes the next one, so a stack can be cleared without reading the digits.
          target = Station::pendingAlarmAt(0);
        } else if (k >= '1' && k <= '6') {
          uint8_t n = Station::pendingAlarmCount();
          for (uint8_t q = 0; q < n; q++) {
            if (Station::pendingAlarmAt(q) == (int8_t)(d - 1)) { target = (int8_t)(d - 1); break; }
          }
        }
        if (target < 0) return;   // no card for that key — ignore the press
        Station::acknowledgeAlarm((uint8_t)target);
        if (Station::hasPendingAlarm()) { drawAlarm(); }
        else                            { goToScreen(Screen::Main); }
      }
      return;

    case Screen::PowerLoss:
      // No '*' — like the alarm overlay this has to be answered, not dismissed. The two
      // digits are the two buttons, safe one first.
      if (k == '1') powerLossAnswer(false);
      if (k == '2') powerLossAnswer(true);
      return;

    default:
      break;
  }

  // Every subpage: '*' goes back one level (the HTML's key()).
  if (k == '*') { goBack(); return; }

  if (isNumScreen()) { if (digit || k == '#') numKey(k); return; }

  switch (screen) {
    case Screen::ChooseSlot:
      if (d >= 1 && d <= 6) pickSlot(d - 1);
      return;

    case Screen::ChooseUser:
      // 7 / 9 page (and wrap) when there is more than one page; otherwise they are rows
      // that do not exist on a 5-row page, so nothing happens.
      if ((k == '7' || k == '9') && pageCount(USER_ROWS) > 1) {
        uint8_t count = pageCount(USER_ROWS);
        userPage = (userPage + count + (k == '7' ? -1 : 1)) % count;
        goToScreen(Screen::ChooseUser);
        return;
      }
      if (d >= 1) pickUser(d - 1);   // numbering is per page, as drawn
      return;

    case Screen::ChoosePosition:
      if (k == '#') { pickPosition(0); return; }   // Skip
      if (d >= 1 && d <= Station::POSITION_COUNT) pickPosition(d);
      return;

    case Screen::ConfirmCancel:
      // '1' takes the safe choice. '#' deliberately does NOTHING here: the whole point of
      // putting "keep running" on top is that a stray confirm press must not destroy a
      // cure, and a blanket '#'=OK would undo that.
      if (k == '1') { goToScreen(Screen::Main); return; }
      if (k == '2') {
        Station::cancelSlot((uint8_t)selSlot);
        goToScreen(Screen::Main);
      }
      return;

    case Screen::Menu:
      if (d >= 1 && d <= MENU_COUNT) menuPick(d - 1);
      return;

    case Screen::ManageUsers:
      if (k == '#') { openAddUser(); return; }
      if ((k == '7' || k == '9') && pageCount(MANAGE_ROWS) > 1) {
        uint8_t count = pageCount(MANAGE_ROWS);
        managePage = (managePage + count + (k == '7' ? -1 : 1)) % count;
        goToScreen(Screen::ManageUsers);
        return;
      }
      // A digit opens User options for that row: touch has two targets on a row (name =
      // tone, Del = delete) and a digit cannot say which was meant.
      if (d >= 1) manageRowKey(d - 1);
      return;

    case Screen::UserActions:
      // No '#': the safe row is 1 and delete is 2, same rule as the cancel confirm.
      if (k == '1') { openEditSound((uint8_t)editUser, true); return; }
      if (k == '2') deleteEditUser();
      return;

    case Screen::EditSound:
    case Screen::AddUserSound:
      if (digit) tonePick(d == 0 ? 9 : d - 1);   // 1-9, then 0 = the tenth tone
      return;

    case Screen::HeatMenu:
      if (d == 1 || d == 2) heatMenuRow(d - 1);
      return;

    case Screen::HeatWarn:
      // Same rule as the cancel confirmation: '1' takes the safe row and '#' does nothing,
      // so a reflex confirm cannot cut the heat under a running cure.
      if (k == '1') { openHeatTimer(); return; }
      if (k == '2') heatWarnAccept();
      return;

    default:
      // TempLog: '*' only. AddUserName: letters cannot come from a numeric pad, so the
      // page stays touch-driven; '*' (handled above) is still Back.
      return;
  }
}

int8_t takeTonePreview() {
  int8_t t = tonePreview;
  tonePreview = -1;
  return t;
}

void remoteStopAlarm() {
  // The over-temperature alarm takes the press first, so one press never also acknowledges a cure.
  if (otActive && !otSilenced) { silenceOverTemp(); return; }
  if (sfActive && !sfSilenced) { silenceSensorFault(); return; }
  int8_t slot = Station::pendingAlarmAt(0);   // oldest, same target as the '#' key
  if (slot < 0) return;
  Station::acknowledgeAlarm((uint8_t)slot);
  if (screen == Screen::Alarm) {
    if (Station::hasPendingAlarm()) { drawAlarm(); }
    else                            { goToScreen(Screen::Main); }
  }
}

void service() {
  int16_t tx = 0, ty = 0;
  bool touched = CtpTouch::read(tx, ty);   // polled over I2C; false on no touch or error
  // A press counts only once it is seen on TWO polls in a row (a real finger is down for
  // tens of ms), so a single bad reading from the chip cannot tap anything — phantom taps
  // kept opening the heat off timer (2026-09-23). The second reading's position is used.
  if (!touched)          touchRun = 0;
  else if (touchRun < 3) touchRun++;
  bool newPress = (touchRun == 2);   // 2 on the second poll only; it then sits at 3 until release
  // Every accepted tap, so a phantom can be told apart from a real finger in the Serial
  // Monitor (the heat off timer was opening and closing by itself, 2026-09-23).
  if (newPress) Serial.printf("[UI] tap x=%d y=%d screen=%u\n", tx, ty, (unsigned)screen);

  if (newPress) {
    if (sleeping) {
      // The touch that wakes the screen must not also press whatever happens to be
      // under the finger — you cannot aim at a screen you cannot see.
      wake();
      return;
    }
    lastTouchMs = millis();
    if (screen == Screen::Main) {
      uint8_t slotIdx = 0;
      Hit h = hitTest((int16_t)tx, (int16_t)ty, slotIdx);
      handleHit(h, slotIdx);
    } else if (screen == Screen::OverTemp) {
      silenceOverTemp();   // any tap silences; nothing on this screen dismisses it
    } else if (screen == Screen::SensorFault) {
      silenceSensorFault();
    } else if (screen == Screen::Alarm) {
      onAlarmTouch((int16_t)tx, (int16_t)ty);
    } else {
      onPageTouch((int16_t)tx, (int16_t)ty);
    }
  }

  // A finished cure must never sit invisible behind a blank panel, so an alarm wakes
  // the screen. It only opens from the main screen: interrupting someone half-way
  // through starting a cure would lose what they had typed. The alarm stays queued
  // and comes up as soon as they are back.
  if (Station::hasPendingAlarm() && screen == Screen::Main) {
    if (sleeping) wake();
    openAlarm();
    return;
  }

  // A new sample while the chart is open extends the trace, as the mockup re-renders.
  // Reverted 2026-08-02 to a full repaint: the split static/data version was tried and
  // looked worse on the panel than this does.
  if (screen == Screen::TempLog && tempLogChanged()) {
    goToScreen(Screen::TempLog);
    return;
  }

  // The cancel confirmation's countdown is live, as the HTML's is.
  if (screen == Screen::ConfirmCancel && selSlot >= 0 &&
      Station::slot(selSlot).remainingSec != cancelShownSec) {
    drawCancelTime();
  }

  // A second cure finishing while the popup is already open adds its card to it.
  if (screen == Screen::Alarm && alarmSigDiffers()) {
    drawAlarm();
    return;
  }

  // The heat menu's countdown ticks; if the timer runs out (or the state otherwise
  // changes) while it is open, the rows change too, so the whole panel is redrawn.
  if (screen == Screen::HeatMenu) {
    bool on = Station::heatEnabled();
    uint32_t sec = Station::heatOffRemainingSec();
    if (on != heatShownOn || (sec == 0) != (heatShownSec == 0)) {
      openHeatState();
      return;
    }
    if (sec != heatShownSec) drawHeatMenuValue();
  }

  if (sleeping) {
    // The off timer can switch the mat off while the screen sleeps: the logo goes, black stays.
    if (!Station::heatEnabled())                          eraseSleepLogo();
    else if (!sleepLogoShown ||
             millis() - sleepLogoMs >= SLEEP_LOGO_MOVE_MS) moveSleepLogo(true);
    return;
  }

  // The countdowns keep running behind an overlay, but nothing on the main screen is
  // visible, so there is nothing to repaint until it closes.
  if (screen != Screen::Main) return;

  // Sleep only while nothing is curing (Dough, 2026-09-23). The backlight cannot be switched on
  // this build, so blanking saves nothing and only hides the status, and a cure counting down
  // is what people glance at. First version also required the mat switched off; dropped the
  // same day, because the mat is normally left on and the screen then never slept. The mat
  // may therefore be hot behind the sleep screen — any touch shows the header again. While a
  // cure runs the idle clock is held at zero, so the timeout starts when the last one ends.
  bool idle = true;
  for (uint8_t i = 0; idle && i < Station::SLOT_COUNT; i++) {
    if (Station::slot(i).state == Station::SlotState::Running) idle = false;
  }
  if (!idle) lastTouchMs = millis();
  if (millis() - lastTouchMs >= sleepAfterMs) {
    goToSleep();
    return;
  }

  if (fullRedraw) {
    tft.fillScreen(B_BG);
    drawHeaderStatic();
    drawHeaderBadge();
    drawHeaderTemp();
    drawHeaderHeatTimer();
    drawFooter();
    fullRedraw = false;
  } else if (Station::heatEnabled() != shadowHeatEnabled ||
             Station::heatOffRemainingSec() != shadowHeatSec) {
    drawHeaderHeatTimer();
  }

  // Repaint as little as possible, at two levels.
  //
  // Nothing changed          -> draw nothing.
  // Only the countdown moved -> repaint just the clock box and the bar. This is the
  //                             once-per-second case, and repainting the whole card
  //                             here is what made the display flicker.
  // Anything else changed    -> repaint the whole card (state, name, duration).
  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
    const Station::Slot& s = Station::slot(i);

    if (shadowValid[i] && memcmp(&shadow[i], &s, sizeof(Station::Slot)) == 0) continue;

    bool tickOnly = shadowValid[i]
                 && shadow[i].state == s.state
                 && s.state == Station::SlotState::Running
                 && shadow[i].totalSec == s.totalSec
                 && shadow[i].sound == s.sound
                 && shadow[i].position == s.position
                 && strcmp(shadow[i].name, s.name) == 0;

    if (tickOnly) {
      drawCardTrack(i);
      drawCardTime(i);
    } else {
      drawCard(i);
    }

    shadow[i] = s;
    shadowValid[i] = true;
  }

  // Clear finished dims and undims as the first slot finishes / the last is cleared.
  if (anyDone() != shadowAnyDone) drawFooter();
}

} // namespace UIFuji
