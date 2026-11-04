// See ui.h for which spec sections this implements.
#include "ui.h"
#include "station.h"
#include <TFT_eSPI.h>
#include <SPI.h>

static TFT_eSPI tft;

// Off-screen buffer for the countdown. The clock is the only thing that repaints every
// second, and clearing-then-drawing it left a ~1.7 ms black gap that was catchable if
// you were looking straight at it. Rendering into RAM and pushing the finished block
// means the panel goes from old value straight to new value with no blank state.
// Costs TIME_BOX_W * TIME_BOX_H * 2 bytes = ~5.6 kB, once, shared by all seven rows.
static TFT_eSprite timeSprite = TFT_eSprite(&tft);

// Measured on this unit 2026-07-31 with Coding/Test_touch_calibrate/.
// Valid for setRotation(1) ONLY — the fifth value encodes orientation.
static uint16_t touchCalibration[5] = { 182, 3586, 349, 3346, 4 };

// --- palette, taken from epoxy_curing_station_UI.html ---------------------------
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static constexpr uint16_t C_BG        = rgb(11, 15, 11);    // #0b0f0b
static constexpr uint16_t C_GREEN     = rgb(74, 222, 128);  // #4ade80
static constexpr uint16_t C_ORANGE    = rgb(251, 146, 60);  // #fb923c
static constexpr uint16_t C_RED       = rgb(239, 68, 68);   // #ef4444
static constexpr uint16_t C_BORDER    = rgb(26, 46, 26);    // #1a2e1a
static constexpr uint16_t C_TEXT      = rgb(226, 232, 240); // #e2e8f0
static constexpr uint16_t C_DIM       = rgb(100, 116, 139); // #64748b
static constexpr uint16_t C_DIMGREEN  = rgb(58, 90, 58);    // #3a5a3a
static constexpr uint16_t C_ACTIVE_BG = rgb(13, 26, 13);    // #0d1a0d
static constexpr uint16_t C_DONE_BG   = rgb(26, 8, 8);      // #1a0808
static constexpr uint16_t C_BTN_BG    = rgb(17, 26, 17);    // #111a11
static constexpr uint16_t C_AVATAR    = rgb(22, 101, 52);   // #166534
static constexpr uint16_t C_AVATAR_D  = rgb(58, 8, 8);      // #3a0808
static constexpr uint16_t C_EMPTY     = rgb(20, 20, 20);    // #141414
static constexpr uint16_t C_HEATOFF   = rgb(26, 26, 26);    // #1a1a1a

// --- layout, 480x320 landscape ---------------------------------------------------
static constexpr int16_t SCR_W = 480;
static constexpr int16_t SCR_H = 320;

static constexpr int16_t TOP_H      = 38;
// 7 rows since 2026-09-15 (was 6 at 38 px / pitch 40): 42 + 7 x 34 = 280 = BOTTOM_Y.
static constexpr int16_t SLOT_Y0    = 42;
static constexpr int16_t SLOT_H     = 32;
static constexpr int16_t SLOT_PITCH = 34;
static constexpr int16_t BOTTOM_Y   = 280;
static constexpr int16_t BTN_Y      = 284;
static constexpr int16_t BTN_H      = 32;

// Inside a slot row.
static constexpr int16_t AVATAR_CX = 40;
static constexpr int16_t AVATAR_R  = 13;
static constexpr int16_t INFO_X    = 62;
static constexpr int16_t INFO_W    = 270;  // name/sub/bar column, stops short of the clock
static constexpr int16_t TIME_R    = 442;  // right edge of the countdown text
static constexpr int16_t CANCEL_X  = 450;  // the "X" column

// The countdown is repainted every second, so it gets its own box that can be cleared
// without touching the rest of the row. Sized for the widest string, "8:00:00" in
// font 4, and kept clear of the progress bar which ends at INFO_X + INFO_W.
static constexpr int16_t TIME_BOX_X = 336;
static constexpr int16_t TIME_BOX_W = 108;
static constexpr int16_t TIME_BOX_Y = 3;   // relative to the row top
static constexpr int16_t TIME_BOX_H = 26;

static inline int16_t slotY(uint8_t i) { return SLOT_Y0 + i * SLOT_PITCH; }

// --- what is currently on screen, so only real changes get repainted -------------
static Station::Slot shadow[Station::SLOT_COUNT];
static bool shadowValid[Station::SLOT_COUNT] = { false };
static float shadowTemp = -999.0f;
static bool shadowHeat = false;
static bool shadowTempValid = false;
static bool fullRedraw = true;

static bool sleeping = false;
static uint32_t lastTouchMs = 0;
static bool wasTouched = false;
static uint32_t sleepAfterMs = UI::SLEEP_AFTER_MS;

// --- drawing --------------------------------------------------------------------

static uint16_t barColor(uint32_t remaining, uint32_t total) {
  if (total == 0) return C_GREEN;
  uint32_t pct = (remaining * 100UL) / total;
  if (pct > 60) return C_GREEN;
  if (pct > 25) return C_ORANGE;
  return C_RED;
}

static void drawAvatar(int16_t cx, int16_t cy, const char* name, bool done) {
  char ini[3];
  Station::initialsOf(name, ini);
  tft.fillCircle(cx, cy, AVATAR_R, done ? C_AVATAR_D : C_AVATAR);
  tft.setTextColor(done ? C_RED : C_GREEN, done ? C_AVATAR_D : C_AVATAR);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(ini, cx, cy, 2);
  tft.setTextDatum(TL_DATUM);
}

static constexpr int16_t BADGE_X = 292, BADGE_W = 82, BADGE_Y = 8, BADGE_H = 22;
static constexpr int16_t TEMP_X  = 380, TEMP_W  = 98;

// The parts of the header that never change. Drawn once per full redraw.
static void drawHeaderStatic() {
  tft.fillRect(0, 0, SCR_W, TOP_H, C_BG);
  tft.drawFastHLine(0, TOP_H, SCR_W, C_BORDER);
  tft.setTextColor(C_GREEN, C_BG);
  tft.drawString("EPOXY CURE", 8, 11, 2);
}

// The parts that change with temperature. Repainted on their own so the title is not
// wiped and redrawn every time the reading moves a tenth of a degree.
static void drawHeaderStatus() {
  const char* badge = shadowHeat ? "HEAT ON" : "HEAT OFF";
  uint16_t badgeBg = shadowHeat ? C_AVATAR : C_HEATOFF;
  tft.fillRoundRect(BADGE_X, BADGE_Y, BADGE_W, BADGE_H, 8, badgeBg);
  tft.setTextColor(shadowHeat ? C_GREEN : C_DIM, badgeBg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(badge, BADGE_X + BADGE_W / 2, BADGE_Y + BADGE_H / 2, 2);

  // "--.-" rather than a stale number when the probe is not answering; a wrong
  // temperature on a curing station is worse than an obviously absent one.
  char t[12];
  if (shadowTempValid) snprintf(t, sizeof(t), "%.1fC", shadowTemp);
  else                 snprintf(t, sizeof(t), "--.-C");

  tft.fillRect(TEMP_X, BADGE_Y, TEMP_W, BADGE_H, C_BG);
  tft.setTextColor(shadowTempValid ? C_ORANGE : C_DIM, C_BG);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(t, 474, BADGE_Y + BADGE_H / 2, 4);
  tft.setTextDatum(TL_DATUM);
}

// The heat off timer, in the free space between the title and the HEAT badge:
// "Off in 1:23:45" while a timer runs, "MAT SWITCHED OFF" once it has run out, and
// nothing otherwise. Ticks once a second, so padded rather than cleared first.
static bool shadowHeatEnabled = true;
static uint32_t shadowHeatSec = 0;
static constexpr int16_t HT_X = 100;   // "EPOXY CURE" in font 2 ends well before this

static void drawHeaderHeatTimer() {
  shadowHeatEnabled = Station::heatEnabled();
  shadowHeatSec = Station::heatOffRemainingSec();

  char t[24], rem[9];
  uint16_t col;
  if (!shadowHeatEnabled)  { snprintf(t, sizeof(t), "MAT SWITCHED OFF"); col = C_RED; }
  else if (shadowHeatSec)  { Station::formatRemaining(shadowHeatSec, rem);
                             snprintf(t, sizeof(t), "Off in %s", rem); col = C_ORANGE; }
  else {
    tft.fillRect(HT_X, BADGE_Y, BADGE_X - 6 - HT_X, BADGE_H, C_BG);
    return;
  }
  tft.setTextColor(col, C_BG);
  tft.setTextDatum(MR_DATUM);
  tft.setTextPadding(BADGE_X - 6 - HT_X);
  tft.drawString(t, BADGE_X - 6, BADGE_Y + BADGE_H / 2, 2);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

// The progress bar, drawn WITHOUT clearing first: the filled part and the empty part
// are painted as two adjacent rectangles that together cover the whole bar, so there
// is never a moment where the bar is blank. Blanking is what the eye reads as flicker.
static void drawSlotBar(uint8_t i) {
  const Station::Slot& s = Station::slot(i);
  int16_t y = slotY(i);
  int16_t barY = y + SLOT_H - 5;
  uint16_t col = barColor(s.remainingSec, s.totalSec);

  int16_t fill = 0;
  if (s.totalSec > 0) {
    fill = (int16_t)((uint32_t)INFO_W * s.remainingSec / s.totalSec);
  }
  if (fill > 0) tft.fillRect(INFO_X, barY, fill, 3, col);
  if (fill < INFO_W) tft.fillRect(INFO_X + fill, barY, INFO_W - fill, 3, C_HEATOFF);
}

// The countdown text, drawn via the sprite so the panel never shows a blank box.
// The clear still happens — but in RAM, where nothing can see it.
static void drawSlotTime(uint8_t i) {
  const Station::Slot& s = Station::slot(i);
  int16_t y = slotY(i);
  uint16_t bg = (s.state == Station::SlotState::Done) ? C_DONE_BG : C_ACTIVE_BG;
  uint16_t col = (s.state == Station::SlotState::Done)
                 ? C_RED : barColor(s.remainingSec, s.totalSec);

  char t[9];
  if (s.state == Station::SlotState::Done) strcpy(t, "DONE");
  else Station::formatRemaining(s.remainingSec, t);

  timeSprite.fillSprite(bg);
  timeSprite.setTextColor(col, bg);
  timeSprite.setTextDatum(MR_DATUM);
  // TIME_R is a screen coordinate; inside the sprite the right edge is relative.
  timeSprite.drawString(t, TIME_R - TIME_BOX_X, TIME_BOX_H / 2, 4);
  timeSprite.pushSprite(TIME_BOX_X, y + TIME_BOX_Y);
}

static void drawSlot(uint8_t i) {
  const Station::Slot& s = Station::slot(i);
  int16_t y = slotY(i);

  uint16_t bg, border;
  switch (s.state) {
    case Station::SlotState::Running: bg = C_ACTIVE_BG; border = C_GREEN;  break;
    case Station::SlotState::Done:    bg = C_DONE_BG;   border = C_RED;    break;
    default:                          bg = C_BG;        border = C_EMPTY;  break;
  }

  tft.fillRoundRect(0, y, SCR_W, SLOT_H, 5, bg);
  tft.drawRoundRect(0, y, SCR_W, SLOT_H, 5, border);

  char num[3];
  snprintf(num, sizeof(num), "%u", i + 1);

  if (s.state == Station::SlotState::Empty) {
    tft.setTextColor(C_DIMGREEN, bg);
    tft.drawString(num, 10, y + 8, 2);
    tft.drawString("Empty - tap to start", INFO_X, y + 8, 2);
    return;
  }

  bool done = (s.state == Station::SlotState::Done);
  tft.setTextColor(done ? C_RED : C_DIMGREEN, bg);
  tft.drawString(num, 10, y + 8, 2);

  drawAvatar(AVATAR_CX, y + SLOT_H / 2, s.name, done);

  tft.setTextColor(done ? C_RED : C_TEXT, bg);
  tft.drawString(s.name, INFO_X, y + 1, 2);

  if (done) {
    char sub[40];
    snprintf(sub, sizeof(sub), "%s - sounds for %us",
             Station::SOUND_NAMES[s.sound], Station::alarmDurationSec());
    tft.setTextColor(C_RED, bg);
    tft.drawString(sub, INFO_X, y + 20, 1);
    drawSlotTime(i);
    return;
  }

  char sub[40];
  snprintf(sub, sizeof(sub), "%lu min cure - %uC",
           (unsigned long)(s.totalSec / 60), Station::setpointC());
  tft.setTextColor(C_GREEN, bg);
  tft.drawString(sub, INFO_X, y + 18, 1);

  drawSlotBar(i);
  drawSlotTime(i);

  // cancel "X"
  tft.setTextColor(rgb(122, 58, 58), bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("X", CANCEL_X + 12, y + SLOT_H / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

static void drawButton(int16_t x, int16_t w, const char* label, bool red) {
  tft.fillRoundRect(x, BTN_Y, w, BTN_H, 4, red ? C_DONE_BG : C_BTN_BG);
  tft.drawRoundRect(x, BTN_Y, w, BTN_H, 4, red ? rgb(58, 20, 20) : C_BORDER);
  tft.setTextColor(red ? C_RED : C_GREEN, red ? C_DONE_BG : C_BTN_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, BTN_Y + BTN_H / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

static void drawBottomBar() {
  tft.fillRect(0, BOTTOM_Y, SCR_W, SCR_H - BOTTOM_Y, C_BG);
  tft.drawFastHLine(0, BOTTOM_Y, SCR_W, C_BORDER);
  // "+ Start" and "Clear done" carry the keypad key that does the same thing, so the
  // physical shortcut is visible rather than hidden. Menu has no key: '*' is back
  // everywhere else and people press it on the main screen out of habit, so binding
  // it here would fire the wrong thing on a reflex press.
  drawButton(6,   152, "Menu",           false);
  drawButton(164, 152, "+ Start (#)",    false);
  drawButton(322, 152, "Clear done (0)", true);
}

// --- touch ----------------------------------------------------------------------

enum class Hit : uint8_t { None, Menu, Start, ClearDone, Slot, SlotCancel };

// --- overlay screens -------------------------------------------------------------
//
// One screen is shown at a time, full-panel, over the main view. Following the
// mockup's flow: + Start -> choose slot -> choose user -> enter time -> running.
// Tapping an empty or finished slot row jumps straight in with that slot chosen,
// which is the mockup's slotPreset path.

enum class Screen : uint8_t { Main, ChooseSlot, ChooseUser, EnterTime,
                              ConfirmCancel, Alarm,
                              Menu, SetTemp, TempLog, SetAlarm, ManageUsers,
                              EditSound, AddUserName, AddUserSound,
                              UserActions, PowerLoss,
                              HeatMenu, HeatTimer, HeatWarn };

static Screen screen = Screen::Main;
static int8_t selSlot = -1;
static int8_t selUser = -1;
static bool slotWasPreset = false;   // entered by tapping a slot, so "back" exits
static char timeBuf[4] = "";         // up to 3 digits, "480"
static const char* timeErr = nullptr;
static uint8_t userPage = 0;

// Step D state.
static char numBuf[4] = "";          // setpoint / alarm entry (2 digits), heat off timer (3)
static const char* numErr = nullptr;
static char nameBuf[Station::USER_NAME_MAX + 1] = "";  // new user being typed
static int8_t editUser = -1;         // user whose tone is being changed
static uint8_t managePage = 0;

// Panel geometry.
static constexpr int16_t PN_X = 8,  PN_Y = 8, PN_W = 464, PN_H = 304;
static constexpr int16_t PN_TITLE_H = 32;
static constexpr int16_t BODY_X = 16, BODY_W = 448;
static constexpr int16_t CLOSE_X = PN_X + PN_W - 34;   // the X in the title bar

// List rows, shared by the slot picker and the user picker.
static constexpr int16_t LIST_Y0 = 58, LIST_ROW_H = 38, LIST_PITCH = 41;
static constexpr uint8_t SLOT_ROWS = Station::SLOT_COUNT;
static constexpr uint8_t USER_ROWS = 5;   // 6th row is the pager
static constexpr int16_t PAGER_Y = LIST_Y0 + USER_ROWS * LIST_PITCH;

// Time entry.
static constexpr int16_t FIELD_Y = 68, FIELD_H = 36;
static constexpr int16_t ERR_Y = 106;
static constexpr int16_t PRESET_Y = 118, PRESET_H = 30, PRESET_W = 108;
static constexpr int16_t KEY_Y0 = 154, KEY_H = 34, KEY_PITCH = 39;
static constexpr int16_t KEY_W = 145, KEY_PITCH_X = 151;
static const uint16_t PRESET_MIN[4] = { 45, 60, 150, 240 };
static const char* const PRESET_LABEL[4] = { "45 min", "1 h", "2.5 h", "4 h" };

static inline int16_t listRowY(uint8_t r) { return LIST_Y0 + r * LIST_PITCH; }

// The slot picker lists every slot on one screen, and 7 rows at the normal 41 px pitch
// run off the panel, so its rows are shorter: 58 + 6 x 36 + 33 = 307, inside the panel.
static constexpr int16_t SLOT_LIST_PITCH = 36, SLOT_LIST_H = 33;
static inline int16_t slotListY(uint8_t r) { return LIST_Y0 + r * SLOT_LIST_PITCH; }

// `accent` colours the border and title — red for the alarm, per the mockup.
// `closable` false omits the X entirely: the alarm popup must be acknowledged, so
// there is deliberately nothing to close it with.
static void drawPanel(const char* title, uint16_t accent = rgb(42, 74, 42),
                      bool closable = true) {
  tft.fillRoundRect(PN_X, PN_Y, PN_W, PN_H, 8, rgb(13, 20, 13)); // #0d140d
  tft.drawRoundRect(PN_X, PN_Y, PN_W, PN_H, 8, accent);
  tft.drawFastHLine(PN_X, PN_Y + PN_TITLE_H, PN_W, C_BORDER);

  tft.setTextColor(accent == rgb(42, 74, 42) ? C_GREEN : accent, rgb(13, 20, 13));
  tft.drawString(title, BODY_X, PN_Y + 9, 2);

  if (!closable) return;

  // Back/close. Drawn as a bordered button in full-brightness text rather than the
  // mockup's dim grey — at arm's length on this panel the dim version was effectively
  // invisible. It is also the control people reach for when lost, so it should be the
  // easiest thing on the screen to find.
  int16_t bx = CLOSE_X - 4, by = PN_Y + 4, bw = 34, bh = 24;
  tft.fillRoundRect(bx, by, bw, bh, 4, C_BTN_BG);
  tft.drawRoundRect(bx, by, bw, bh, 4, C_DIMGREEN);
  tft.setTextColor(C_TEXT, C_BTN_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("X", bx + bw / 2, by + bh / 2, 4);
  tft.setTextDatum(TL_DATUM);
}

// One selectable row. `enabled` false draws it dimmed and it does not respond.
// `h` defaults to the normal row; the slot picker passes its shorter one.
static void drawListRow(int16_t y, const char* left, const char* right,
                        uint16_t leftCol, bool enabled, int16_t h = LIST_ROW_H) {
  uint16_t bg = enabled ? rgb(13, 26, 13) : rgb(10, 12, 10);
  tft.fillRoundRect(BODY_X, y, BODY_W, h, 4, bg);
  tft.drawRoundRect(BODY_X, y, BODY_W, h, 4, enabled ? C_BORDER : C_EMPTY);
  tft.setTextColor(enabled ? leftCol : C_DIMGREEN, bg);
  tft.drawString(left, BODY_X + 12, y + (h - 16) / 2, 2);   // font 2 is 16 px tall
  if (right) {
    tft.setTextColor(enabled ? C_DIMGREEN : C_EMPTY, bg);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(right, BODY_X + BODY_W - 12, y + h / 2, 2);
    tft.setTextDatum(TL_DATUM);
  }
}

static void drawChooseSlot() {
  drawPanel("Choose slot");
  tft.setTextColor(C_DIM, rgb(13, 20, 13));
  tft.drawString("Step 1 of 3 - which slot is this scope going in?", BODY_X, 44, 1);

  for (uint8_t i = 0; i < SLOT_ROWS; i++) {
    const Station::Slot& s = Station::slot(i);
    char left[40];
    switch (s.state) {
      case Station::SlotState::Empty:
        snprintf(left, sizeof(left), "%u   Empty", i + 1);
        drawListRow(slotListY(i), left, ">", C_TEXT, true, SLOT_LIST_H);
        break;
      case Station::SlotState::Done:
        snprintf(left, sizeof(left), "%u   %s - done, will restart", i + 1, s.name);
        drawListRow(slotListY(i), left, ">", C_RED, true, SLOT_LIST_H);
        break;
      default: // Running — cannot be taken over without cancelling first
        snprintf(left, sizeof(left), "%u   %s - in use", i + 1, s.name);
        drawListRow(slotListY(i), left, nullptr, C_TEXT, false, SLOT_LIST_H);
        break;
    }
  }
}

static uint8_t userPageCount() {
  uint8_t n = Station::userCount();
  if (n == 0) return 1;
  return (n + USER_ROWS - 1) / USER_ROWS;
}

static void drawChooseUser() {
  drawPanel("Select user");
  char step[64];
  snprintf(step, sizeof(step), "Step 2 of 3 - who is putting the scope in?  (Slot %d)",
           selSlot + 1);
  tft.setTextColor(C_DIM, rgb(13, 20, 13));
  tft.drawString(step, BODY_X, 44, 1);

  uint8_t total = Station::userCount();
  if (userPage >= userPageCount()) userPage = 0;
  uint8_t first = userPage * USER_ROWS;

  for (uint8_t r = 0; r < USER_ROWS; r++) {
    uint8_t idx = first + r;
    int16_t y = listRowY(r);
    if (idx >= total) {
      tft.fillRect(BODY_X, y, BODY_W, LIST_ROW_H, rgb(13, 20, 13)); // clear unused row
      continue;
    }
    const Station::User& u = Station::user(idx);
    char left[48];
    // Numbered so the keypad digit that selects this row is visible rather than
    // guessed. r+1, not idx+1: the numbering is per page, which is what handleKey()
    // uses — on page 2 the first row is still "1".
    snprintf(left, sizeof(left), "%u   %s", r + 1, u.name);
    drawListRow(y, left, Station::SOUND_NAMES[u.sound], C_TEXT, true);
  }

  if (total == 0) {
    tft.setTextColor(C_DIM, rgb(13, 20, 13));
    tft.drawString("No users saved yet - add one from the Menu (step D)",
                   BODY_X + 12, LIST_Y0 + 12, 2);
  }

  // Pager, only when there is more than one page.
  tft.fillRect(BODY_X, PAGER_Y, BODY_W, LIST_ROW_H, rgb(13, 20, 13));
  if (userPageCount() > 1) {
    tft.fillRoundRect(BODY_X, PAGER_Y, 120, 32, 4, C_BTN_BG);
    tft.drawRoundRect(BODY_X, PAGER_Y, 120, 32, 4, C_BORDER);
    tft.setTextColor(C_GREEN, C_BTN_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("< Prev (7)", BODY_X + 60, PAGER_Y + 16, 2);

    tft.fillRoundRect(BODY_X + BODY_W - 120, PAGER_Y, 120, 32, 4, C_BTN_BG);
    tft.drawRoundRect(BODY_X + BODY_W - 120, PAGER_Y, 120, 32, 4, C_BORDER);
    tft.drawString("Next (9) >", BODY_X + BODY_W - 60, PAGER_Y + 16, 2);

    char pg[16];
    snprintf(pg, sizeof(pg), "%u / %u", userPage + 1, userPageCount());
    tft.setTextColor(C_DIM, rgb(13, 20, 13));
    tft.drawString(pg, BODY_X + BODY_W / 2, PAGER_Y + 16, 2);
    tft.setTextDatum(TL_DATUM);
  }
}

// Just the input field and the error line — redrawn on every keypress.
static void drawTimeField() {
  tft.fillRoundRect(BODY_X, FIELD_Y, BODY_W, FIELD_H, 4, rgb(6, 15, 6));
  tft.drawRoundRect(BODY_X, FIELD_Y, BODY_W, FIELD_H, 4, rgb(42, 74, 42));
  tft.setTextColor(C_GREEN, rgb(6, 15, 6));
  tft.drawString(timeBuf[0] ? timeBuf : "_", BODY_X + 12, FIELD_Y + 8, 4);

  tft.fillRect(BODY_X, ERR_Y, BODY_W, 10, rgb(13, 20, 13));
  if (timeErr) {
    tft.setTextColor(C_RED, rgb(13, 20, 13));
    tft.drawString(timeErr, BODY_X, ERR_Y, 1);
  }
}

static void drawKey(int16_t x, int16_t y, int16_t w, int16_t h,
                    const char* label, uint16_t fg, uint16_t bg, uint16_t border) {
  tft.fillRoundRect(x, y, w, h, 4, bg);
  tft.drawRoundRect(x, y, w, h, 4, border);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

static void drawEnterTime() {
  drawPanel("Set cure time");

  char step[64];
  snprintf(step, sizeof(step), "Step 3 of 3  -  Slot %d  -  %s",
           selSlot + 1, selUser >= 0 ? Station::user(selUser).name : "");
  tft.setTextColor(C_DIM, rgb(13, 20, 13));
  tft.drawString(step, BODY_X, 44, 1);
  tft.drawString("CURE TIME (MINUTES)", BODY_X, 56, 1);

  drawTimeField();

  for (uint8_t i = 0; i < 4; i++) {
    drawKey(BODY_X + i * (PRESET_W + 5), PRESET_Y, PRESET_W, PRESET_H,
            PRESET_LABEL[i], C_GREEN, C_ACTIVE_BG, rgb(42, 74, 42));
  }

  // 7 8 9 / 4 5 6 / 1 2 3 / del 0 Start
  static const char* const keys[4][3] = {
    { "7", "8", "9" },
    { "4", "5", "6" },
    { "1", "2", "3" },
    { "<-", "0", "Start" }
  };
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t c = 0; c < 3; c++) {
      int16_t x = BODY_X + c * KEY_PITCH_X;
      int16_t y = KEY_Y0 + r * KEY_PITCH;
      bool isDel   = (r == 3 && c == 0);
      bool isStart = (r == 3 && c == 2);
      uint16_t fg = isDel ? C_RED : (isStart ? C_GREEN : C_ORANGE);
      uint16_t bg = isDel ? C_DONE_BG : (isStart ? C_AVATAR : C_BTN_BG);
      uint16_t bd = isDel ? rgb(58, 20, 20) : (isStart ? rgb(42, 90, 42) : C_BORDER);
      drawKey(x, y, KEY_W, KEY_H, keys[r][c], fg, bg, bd);
    }
  }
}

// --- step C: cancel confirmation and the finished-cure alarm ----------------------
// Both follow epoxy_curing_station_UI.html (confirmCancelSlot / showAlarmPopup) and
// share the panel frame and list rows above.

static constexpr int16_t POP_AVATAR_Y = 74;   // avatar centre
static constexpr int16_t POP_NAME_Y   = 96;
static constexpr int16_t POP_SUB_Y    = 120;
static constexpr int16_t POP_WARN_Y   = 142;
static constexpr int16_t POP_ROW1_Y   = 172;
static constexpr int16_t POP_ROW2_Y   = POP_ROW1_Y + LIST_PITCH;

// Name + slot, then a coloured sub-line.
static void drawPopupIdentity(uint8_t i, bool done, const char* sub, uint16_t subCol) {
  const Station::Slot& s = Station::slot(i);
  char line[48];

  drawAvatar(PN_X + PN_W / 2, POP_AVATAR_Y, s.name, done);

  snprintf(line, sizeof(line), "%s - Slot %u", s.name, i + 1);
  tft.setTextColor(C_TEXT, rgb(13, 20, 13));
  tft.setTextDatum(TC_DATUM);
  tft.drawString(line, PN_X + PN_W / 2, POP_NAME_Y, 2);

  tft.setTextColor(subCol, rgb(13, 20, 13));
  tft.drawString(sub, PN_X + PN_W / 2, POP_SUB_Y, 2);
  tft.setTextDatum(TL_DATUM);
}

static void drawConfirmCancel() {
  if (selSlot < 0) return;
  drawPanel("Cancel cure?");

  char sub[24];
  char rem[9];
  Station::formatRemaining(Station::slot(selSlot).remainingSec, rem);
  snprintf(sub, sizeof(sub), "%s remaining", rem);
  drawPopupIdentity((uint8_t)selSlot, false, sub, C_ORANGE);

  tft.setTextColor(C_RED, rgb(13, 20, 13));
  tft.setTextDatum(TC_DATUM);
  tft.drawString("This stops the cure and clears the slot - cannot be undone.",
                 PN_X + PN_W / 2, POP_WARN_Y, 1);
  tft.setTextDatum(TL_DATUM);

  // Safe option first and on top, so the reflexive tap keeps the cure running.
  drawListRow(POP_ROW1_Y, "No, keep running", nullptr, C_TEXT, true);
  drawListRow(POP_ROW2_Y, "Yes, cancel this cure", nullptr, C_RED, true);
}

// Every unacknowledged cure is a card with its own Stop, all on one screen — the
// mockup's showAlarmPopups(). Stopping one re-renders with the rest still listed;
// the last one closes the popup. Seven slots means at most seven cards: 46 + 6 x 38 + 34 =
// 308, inside the panel (they were 38 high at pitch 44 for six).
static constexpr int16_t ALARM_Y0 = 46, ALARM_CARD_H = 34, ALARM_PITCH = 38;

static inline int16_t alarmCardY(uint8_t k) { return ALARM_Y0 + k * ALARM_PITCH; }

static void drawAlarmCard(uint8_t k, uint8_t slotIdx) {
  const Station::Slot& s = Station::slot(slotIdx);
  int16_t y = alarmCardY(k);
  uint16_t bg = rgb(13, 26, 13);

  tft.fillRoundRect(BODY_X, y, BODY_W, ALARM_CARD_H, 4, bg);
  tft.drawRoundRect(BODY_X, y, BODY_W, ALARM_CARD_H, 4, rgb(58, 20, 20)); // #3a1414

  drawAvatar(BODY_X + 12 + AVATAR_R, y + ALARM_CARD_H / 2, s.name, true);

  char line[48];
  snprintf(line, sizeof(line), "%s - Slot %u", s.name, slotIdx + 1);
  tft.setTextColor(C_TEXT, bg);
  tft.drawString(line, BODY_X + 52, y + 2, 2);

  snprintf(line, sizeof(line), "%s - sounding for %us",
           Station::SOUND_NAMES[s.sound], Station::alarmDurationSec());
  tft.setTextColor(C_ORANGE, bg);
  tft.drawString(line, BODY_X + 52, y + 22, 1);

  // Each card is labelled with a key that stops THAT card. '#' always takes the top
  // one, so the top card advertises '#' and the rest advertise their slot digit —
  // both are true, and after the top card is stopped the next one inherits the '#'.
  // The digit is the SLOT number (matching the line above and the main screen), not
  // the card's position: position shifts as others are acknowledged, the slot does not.
  if (k == 0) snprintf(line, sizeof(line), "Stop (#)");
  else        snprintf(line, sizeof(line), "Stop (%u)", slotIdx + 1);
  tft.setTextColor(C_GREEN, bg);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(line, BODY_X + BODY_W - 12, y + ALARM_CARD_H / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

// What the popup is currently showing, so a cure finishing while it is open adds a
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

static void drawAlarm() {
  uint8_t n = Station::pendingAlarmCount();
  alarmSigN = n;
  for (uint8_t k = 0; k < n; k++) alarmSig[k] = (uint8_t)Station::pendingAlarmAt(k);
  if (n == 0) return;

  char title[32];
  if (n > 1) snprintf(title, sizeof(title), "%u cures complete", n);
  else       snprintf(title, sizeof(title), "Cure complete");
  drawPanel(title, C_RED, false);   // no X — one of them has to be acknowledged

  for (uint8_t k = 0; k < n; k++) {
    int8_t slotIdx = Station::pendingAlarmAt(k);
    if (slotIdx < 0) break;
    drawAlarmCard(k, (uint8_t)slotIdx);
  }
}

// --- step D: menu, settings, user management -------------------------------------
// Follows openMenu / openTempMenu / openAlarmMenu / openManageUsers / openAddUser in
// epoxy_curing_station_UI.html.

// "Start new slot" and "Clear done slots" were dropped 2026-08-02 — both are already
// buttons on the main screen, one tap away, so having them here too was a second route
// to the same thing and made the menu longer to read. The menu is now only the things
// that have no other home.
static constexpr uint8_t MENU_COUNT = 5;
static const char* const MENU_ITEMS[MENU_COUNT] = {
  "Manage user names",
  "Temperature setting",
  "Temp log",
  "Alarm settings",
  "Heat off timer",
};

static void drawMenu() {
  drawPanel("Main menu");
  for (uint8_t i = 0; i < MENU_COUNT; i++) {
    // Numbered so the keypad digit that opens each item is visible, same as the slot
    // and user lists.
    char left[40];
    snprintf(left, sizeof(left), "%u   %s", i + 1, MENU_ITEMS[i]);
    drawListRow(listRowY(i), left, ">", C_TEXT, true);
  }
}

// --- temp log chart ---
// Follows renderTempLogChart() in the mockup: MIN/CURRENT/MAX above a fixed-scale
// plot with the 40-50 C target band shaded. The scale is fixed at 20-55 C on purpose
// — an auto-scaling Y axis makes a flat hold look like wild swings.
static constexpr int16_t TL_STAT_Y = 46;
static constexpr int16_t TL_X = BODY_X, TL_Y = 84, TL_W = BODY_W, TL_H = 180;
static constexpr int16_t TL_PAD_L = 26, TL_PAD_R = 6, TL_PAD_T = 6, TL_PAD_B = 6;
static constexpr float TL_LO = 20.0f, TL_HI = 55.0f;

static inline int16_t tlPlotX0() { return TL_X + TL_PAD_L; }
static inline int16_t tlPlotW()  { return TL_W - TL_PAD_L - TL_PAD_R; }
static inline int16_t tlPlotY0() { return TL_Y + TL_PAD_T; }
static inline int16_t tlPlotH()  { return TL_H - TL_PAD_T - TL_PAD_B; }

static int16_t tlY(float c) {
  if (c < TL_LO) c = TL_LO;
  if (c > TL_HI) c = TL_HI;
  return tlPlotY0() + tlPlotH() - (int16_t)((c - TL_LO) / (TL_HI - TL_LO) * tlPlotH());
}

static int16_t tlX(uint16_t k, uint16_t n) {
  if (n <= 1) return tlPlotX0();
  return tlPlotX0() + (int16_t)((int32_t)k * tlPlotW() / (n - 1));
}

static void drawStat(int16_t cx, const char* label, float v, uint16_t col) {
  char s[12];
  snprintf(s, sizeof(s), "%.1f", v);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_DIM, rgb(13, 20, 13));
  tft.drawString(label, cx, TL_STAT_Y, 1);
  tft.setTextColor(col, rgb(13, 20, 13));
  tft.drawString(s, cx, TL_STAT_Y + 10, 2);
  tft.setTextDatum(TL_DATUM);
}

// What the chart is currently showing, so a new sample redraws it while it is open.
// Once the buffer is full the count stops changing, so the newest value is tracked too.
static uint16_t tlShownN = 0xFFFF;
static float tlShownLast = 0.0f;

static bool tempLogChanged() {
  uint16_t n = Station::tempLogCount();
  if (n != tlShownN) return true;
  return n > 0 && Station::tempLogAt(n - 1) != tlShownLast;
}

static void drawTempLog() {
  drawPanel("Temp log");

  uint16_t n = Station::tempLogCount();
  tlShownN = n;
  tlShownLast = n > 0 ? Station::tempLogAt(n - 1) : 0.0f;

  float mn = 0, mx = 0, cur = 0;
  if (n > 0) {
    mn = mx = Station::tempLogAt(0);
    for (uint16_t k = 1; k < n; k++) {
      float v = Station::tempLogAt(k);
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
    cur = Station::tempLogAt(n - 1);
  }
  drawStat(BODY_X + BODY_W / 6,     "MIN",     mn,  rgb(96, 165, 250));
  drawStat(BODY_X + BODY_W / 2,     "CURRENT", cur, C_ORANGE);
  drawStat(BODY_X + BODY_W * 5 / 6, "MAX",     mx,  rgb(248, 113, 113));

  tft.fillRoundRect(TL_X, TL_Y, TL_W, TL_H, 6, rgb(6, 15, 6));
  tft.drawRoundRect(TL_X, TL_Y, TL_W, TL_H, 6, C_BORDER);

  // The 40-50 C target band. No alpha on this panel, so it is a flat dark green
  // rather than the mockup's 18% overlay.
  int16_t bandTop = tlY(50.0f), bandBot = tlY(40.0f);
  tft.fillRect(tlPlotX0(), bandTop, tlPlotW(), bandBot - bandTop, rgb(12, 34, 12));
  tft.setTextColor(C_DIMGREEN, rgb(12, 34, 12));
  tft.drawString("50C", tlPlotX0() + 2, bandTop + 2, 1);
  tft.drawString("40C", tlPlotX0() + 2, bandBot - 10, 1);

  // Y axis labels, in the left padding.
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(C_DIMGREEN, rgb(6, 15, 6));
  for (uint8_t v = 20; v <= 50; v += 10) {
    char lbl[4];
    snprintf(lbl, sizeof(lbl), "%u", v);
    tft.drawString(lbl, TL_X + TL_PAD_L - 3, tlY((float)v), 1);
  }
  tft.setTextDatum(TL_DATUM);

  if (n < 2) {
    tft.setTextColor(C_DIM, rgb(6, 15, 6));
    tft.setTextDatum(MC_DATUM);
    tft.drawString("collecting - first points take a few minutes",
                   TL_X + TL_W / 2, TL_Y + TL_H / 2, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  for (uint16_t k = 0; k + 1 < n; k++) {
    tft.drawLine(tlX(k, n), tlY(Station::tempLogAt(k)),
                 tlX(k + 1, n), tlY(Station::tempLogAt(k + 1)), C_ORANGE);
  }
  tft.fillCircle(tlX(n - 1, n), tlY(cur), 2, C_ORANGE);

  // Past two hours "~478 min ago" stops being readable at a glance, so it switches
  // to hours — the buffer now spans 8 h, not the 90 min it started at.
  char span[32];
  uint16_t mins = (uint16_t)((uint32_t)(n - 1) * Station::tempLogIntervalSec() / 60);
  if (mins >= 120) snprintf(span, sizeof(span), "~%u h %02u min ago", mins / 60, mins % 60);
  else             snprintf(span, sizeof(span), "~%u min ago", mins);
  tft.setTextColor(C_DIMGREEN, rgb(13, 20, 13));
  tft.drawString(span, BODY_X, TL_Y + TL_H + 6, 1);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("now", BODY_X + BODY_W, TL_Y + TL_H + 6, 1);
  tft.setTextDatum(TL_DATUM);
}

// Both number-entry screens share this layout: current value, hint, field, error,
// then the same 3-wide keypad as the cure-time screen so the muscle memory carries.
// The field is 36 high, matching the cure-time screen. At 30 it was too short: font 4
// is 26 px, and 5 px of top padding put the digits' bottoms past the box edge.
static constexpr int16_t SET_VAL_Y = 44, SET_HINT_Y = 72;
static constexpr int16_t SET_FIELD_Y = 82, SET_FIELD_H = 36, SET_ERR_Y = 120;
static constexpr int16_t SET_KEY_Y0 = 134, SET_KEY_H = 32, SET_KEY_PITCH = 38;
// The keypad's bottom row ends at 282, so the footer starts below that.
static constexpr int16_t SET_FOOT_Y = 286;

// Field and error only — redrawn on every keypress.
static void drawNumField() {
  tft.fillRoundRect(BODY_X, SET_FIELD_Y, BODY_W, SET_FIELD_H, 4, rgb(6, 15, 6));
  tft.drawRoundRect(BODY_X, SET_FIELD_Y, BODY_W, SET_FIELD_H, 4, rgb(42, 74, 42));
  tft.setTextColor(C_GREEN, rgb(6, 15, 6));
  tft.drawString(numBuf[0] ? numBuf : "_", BODY_X + 12, SET_FIELD_Y + 8, 4);

  tft.fillRect(BODY_X, SET_ERR_Y, BODY_W, 10, rgb(13, 20, 13));
  if (numErr) {
    tft.setTextColor(C_RED, rgb(13, 20, 13));
    tft.drawString(numErr, BODY_X, SET_ERR_Y, 1);
  }
}

static void drawNumKeypad(const char* okLabel) {
  static const char* const keys[4][3] = {
    { "7", "8", "9" },
    { "4", "5", "6" },
    { "1", "2", "3" },
    { "<-", "0", "" }        // the OK label is supplied by the caller
  };
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t c = 0; c < 3; c++) {
      bool isDel = (r == 3 && c == 0);
      bool isOk  = (r == 3 && c == 2);
      uint16_t fg = isDel ? C_RED : (isOk ? C_GREEN : C_ORANGE);
      uint16_t bg = isDel ? C_DONE_BG : (isOk ? C_AVATAR : C_BTN_BG);
      drawKey(BODY_X + c * KEY_PITCH_X, SET_KEY_Y0 + r * SET_KEY_PITCH,
              KEY_W, SET_KEY_H, isOk ? okLabel : keys[r][c],
              fg, bg, isOk ? C_GREEN : rgb(42, 74, 42));
    }
  }
}

static void drawSetTemp() {
  drawPanel("Temperature setpoint");

  char big[16];
  snprintf(big, sizeof(big), "%u C", Station::setpointC());
  tft.setTextColor(C_GREEN, rgb(13, 20, 13));
  tft.drawString(big, BODY_X, SET_VAL_Y, 4);

  tft.setTextColor(C_DIM, rgb(13, 20, 13));
  tft.drawString("Current setpoint  -  range 40-50 C", BODY_X, SET_HINT_Y, 1);

  drawNumField();
  drawNumKeypad("Set");
}

static void drawSetAlarm() {
  drawPanel("Alarm settings");

  char big[16];
  snprintf(big, sizeof(big), "%u sec", Station::alarmDurationSec());
  tft.setTextColor(C_GREEN, rgb(13, 20, 13));
  tft.drawString(big, BODY_X, SET_VAL_Y, 4);

  tft.setTextColor(C_DIM, rgb(13, 20, 13));
  tft.drawString("How long the alarm sounds each time a slot finishes  -  range 5-60 sec",
                 BODY_X, SET_HINT_Y, 1);

  drawNumField();
  drawNumKeypad("Set");

  // Shortcut from the mockup — the tone is per-worker, which is not obvious from a
  // screen that only sets a duration.
  tft.drawFastHLine(BODY_X, SET_FOOT_Y - 2, BODY_W, C_BORDER);
  tft.setTextColor(C_GREEN, rgb(13, 20, 13));
  tft.drawString("Set each worker's alarm tone  >", BODY_X, SET_FOOT_Y + 2, 2);
}

// --- user management ---

static constexpr uint8_t MANAGE_ROWS = 4;   // leaves room for "+ Add user" and a pager
static constexpr int16_t ADD_ROW_Y  = LIST_Y0 + MANAGE_ROWS * LIST_PITCH;
static constexpr int16_t MANAGE_PAGER_Y = ADD_ROW_Y + LIST_PITCH;
static constexpr int16_t DEL_X = BODY_X + BODY_W - 62;  // the "Del" hit column

// Prev/Next are drawn as real buttons and hit-tested against the same rectangles.
// They used to be words inside one centred string with the touch zones guessed at
// fixed offsets — the words did not sit where the zones were, so taps did nothing.
static constexpr int16_t PAGER_H = 30, PAGER_BTN_W = 120;
static constexpr int16_t PREV_X = BODY_X;
static constexpr int16_t NEXT_X = BODY_X + BODY_W - PAGER_BTN_W;

static uint8_t managePageCount() {
  uint8_t n = Station::userCount();
  if (n == 0) return 1;
  return (n + MANAGE_ROWS - 1) / MANAGE_ROWS;
}

static void drawManageUsers() {
  drawPanel("User names");

  uint8_t total = Station::userCount();
  if (managePage >= managePageCount()) managePage = managePageCount() - 1;

  for (uint8_t r = 0; r < MANAGE_ROWS; r++) {
    uint8_t idx = managePage * MANAGE_ROWS + r;
    int16_t y = listRowY(r);
    if (idx >= total) {
      tft.fillRect(BODY_X, y, BODY_W, LIST_ROW_H, rgb(13, 20, 13));
      continue;
    }
    const Station::User& u = Station::user(idx);
    uint16_t bg = rgb(13, 26, 13);
    tft.fillRoundRect(BODY_X, y, BODY_W, LIST_ROW_H, 4, bg);
    tft.drawRoundRect(BODY_X, y, BODY_W, LIST_ROW_H, 4, C_BORDER);

    drawAvatar(BODY_X + 12 + AVATAR_R, y + LIST_ROW_H / 2, u.name, false);

    // Numbered per page, like the user list — r+1, not idx+1.
    char nameLine[48];
    snprintf(nameLine, sizeof(nameLine), "%u   %s", r + 1, u.name);
    tft.setTextColor(C_TEXT, bg);
    tft.drawString(nameLine, BODY_X + 52, y + 4, 2);
    tft.setTextColor(C_GREEN, bg);
    tft.drawString(Station::SOUND_NAMES[u.sound], BODY_X + 52, y + 23, 1);

    tft.setTextColor(C_RED, bg);
    tft.setTextDatum(MR_DATUM);
    tft.drawString("Del", BODY_X + BODY_W - 12, y + LIST_ROW_H / 2, 2);
    tft.setTextDatum(TL_DATUM);
  }

  drawListRow(ADD_ROW_Y, "+   Add user (#)", nullptr, C_GREEN, true);

  tft.fillRect(BODY_X, MANAGE_PAGER_Y, BODY_W, PAGER_H, rgb(13, 20, 13));
  if (managePageCount() > 1) {
    // 7/9 are the keypad keys, printed on the buttons — they sit left and right the
    // way the buttons do. Without a page key the keypad could not reach page 2 at all.
    drawKey(PREV_X, MANAGE_PAGER_Y, PAGER_BTN_W, PAGER_H, "< Prev (7)",
            C_GREEN, C_BTN_BG, C_GREEN);
    drawKey(NEXT_X, MANAGE_PAGER_Y, PAGER_BTN_W, PAGER_H, "Next (9) >",
            C_GREEN, C_BTN_BG, C_GREEN);

    char pg[16];
    snprintf(pg, sizeof(pg), "%u / %u", managePage + 1, managePageCount());
    tft.setTextColor(C_TEXT, rgb(13, 20, 13));
    tft.setTextDatum(MC_DATUM);
    tft.drawString(pg, BODY_X + BODY_W / 2, MANAGE_PAGER_Y + PAGER_H / 2, 2);
    tft.setTextDatum(TL_DATUM);
  }
}

// The dark UI's tone list has room for five rows only. Station::SOUND_COUNT went to 10 for
// Concept B (2026-09-25); this UI keeps offering the first five rather than overflowing.
static constexpr uint8_t DARK_TONES = Station::SOUND_COUNT < 5 ? Station::SOUND_COUNT : 5;

// The five tones. Used both for an existing user (with a tick on the current one) and
// as step 2 of adding a user, where nothing is selected yet.
static void drawSoundList(const char* title, const char* step, int8_t currentSound) {
  drawPanel(title);
  tft.setTextColor(C_DIM, rgb(13, 20, 13));
  tft.drawString(step, BODY_X, 44, 1);

  for (uint8_t i = 0; i < DARK_TONES; i++) {
    bool sel = (currentSound >= 0 && (uint8_t)currentSound == i);
    drawListRow(listRowY(i), Station::SOUND_NAMES[i], sel ? "in use" : ">",
                sel ? C_GREEN : C_TEXT, true);
  }
}

static void drawEditSound() {
  if (editUser < 0 || editUser >= (int8_t)Station::userCount()) return;
  const Station::User& u = Station::user(editUser);
  char title[40];
  snprintf(title, sizeof(title), "Alarm tone - %s", u.name);
  drawSoundList(title, "Pick the tone that sounds when this worker's cure finishes",
                (int8_t)u.sound);
}

static void drawAddUserSound() {
  char step[64];
  snprintf(step, sizeof(step), "Step 2 of 2  -  %s's alarm tone", nameBuf);
  drawSoundList("Choose alarm tone", step, -1);
}

// Keypad route into a user row. Touch has two targets on that row (the name changes
// the tone, "Del" deletes), but a digit cannot say WHICH part of the row was meant,
// so it opens this instead. Touch is unchanged and still goes straight to either.
//
// Side benefit: delete has no confirmation on the touch path (copied from the mockup,
// and safe there because a running cure already owns a copy of the name) — coming
// through here makes it two deliberate presses.
static void drawUserActions() {
  if (editUser < 0 || editUser >= (int8_t)Station::userCount()) return;
  const Station::User& u = Station::user(editUser);

  drawPanel("User");

  char line[48];
  snprintf(line, sizeof(line), "%s", u.name);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_TEXT, rgb(13, 20, 13));
  tft.drawString(line, PN_X + PN_W / 2, POP_NAME_Y, 4);

  snprintf(line, sizeof(line), "Alarm tone: %s", Station::SOUND_NAMES[u.sound]);
  tft.setTextColor(C_GREEN, rgb(13, 20, 13));
  tft.drawString(line, PN_X + PN_W / 2, POP_SUB_Y, 2);
  tft.setTextDatum(TL_DATUM);

  // Safe option first and on top, as on the cancel confirmation.
  drawListRow(POP_ROW1_Y, "1   Change alarm tone", nullptr, C_TEXT, true);
  drawListRow(POP_ROW2_Y, "2   Delete this user", nullptr, C_RED, true);
}

// --- QWERTY name entry ---
//
// The mockup puts the digits, apostrophe, SPACE and Next all on one bottom row. That
// is 13 keys across 448 px = 34 px each, too small to hit reliably with a finger on
// this panel, so the digits get their own row and the layout is five rows instead of
// four. Same keys, same order, just not crammed.
static constexpr int16_t KB_Y0 = 92, KB_H = 34, KB_PITCH = 40;
static constexpr int16_t KB_W = 40, KB_PITCH_X = 44, KB_X0 = BODY_X + 4;
// 36 high for the same reason as the settings field — 30 clipped the typed name.
static constexpr int16_t KB_FIELD_Y = 52, KB_FIELD_H = 36;
static const char* const KB_ROWS[4] = {
  "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
};
static constexpr int16_t KB_DEL_X = KB_X0 + 7 * KB_PITCH_X + 12, KB_DEL_W = 116;
static constexpr int16_t KB_APOS_X = KB_X0, KB_APOS_W = 60;
static constexpr int16_t KB_SPACE_X = KB_X0 + 68, KB_SPACE_W = 240;
static constexpr int16_t KB_NEXT_X = KB_X0 + 316, KB_NEXT_W = 120;

static inline int16_t kbRowY(uint8_t r) { return KB_Y0 + r * KB_PITCH; }

// Row 2 (ASDFGHJKL) is one key short, so it sits half a key in — as on a real keyboard.
static inline int16_t kbRowX0(uint8_t r) { return (r == 2) ? KB_X0 + KB_PITCH_X / 2 : KB_X0; }

static void drawNameField() {
  tft.fillRoundRect(BODY_X, KB_FIELD_Y, BODY_W, KB_FIELD_H, 4, rgb(6, 15, 6));
  tft.drawRoundRect(BODY_X, KB_FIELD_Y, BODY_W, KB_FIELD_H, 4, rgb(42, 74, 42));
  tft.setTextColor(C_GREEN, rgb(6, 15, 6));
  tft.drawString(nameBuf[0] ? nameBuf : "_", BODY_X + 12, KB_FIELD_Y + 8, 4);
}

static void drawAddUserName() {
  drawPanel("Add user name");
  tft.setTextColor(C_DIM, rgb(13, 20, 13));
  tft.drawString("Step 1 of 2  -  type name", BODY_X, 42, 1);

  drawNameField();

  char label[2] = { 0, 0 };
  for (uint8_t r = 0; r < 4; r++) {
    int16_t y = kbRowY(r);
    int16_t x0 = kbRowX0(r);
    for (uint8_t c = 0; KB_ROWS[r][c]; c++) {
      label[0] = KB_ROWS[r][c];
      drawKey(x0 + c * KB_PITCH_X, y, KB_W, KB_H, label,
              C_TEXT, C_BTN_BG, rgb(42, 74, 42));
    }
  }
  drawKey(KB_DEL_X, kbRowY(3), KB_DEL_W, KB_H, "<-", C_RED, C_DONE_BG, rgb(42, 74, 42));

  drawKey(KB_APOS_X,  kbRowY(4), KB_APOS_W,  KB_H, "'",     C_TEXT,  C_BTN_BG, rgb(42, 74, 42));
  drawKey(KB_SPACE_X, kbRowY(4), KB_SPACE_W, KB_H, "SPACE", C_TEXT,  C_BTN_BG, rgb(42, 74, 42));
  drawKey(KB_NEXT_X,  kbRowY(4), KB_NEXT_W,  KB_H, "Next",  C_GREEN, C_AVATAR, C_GREEN);
}

// --- step E: the power-loss warning ----------------------------------------------
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

// Everything here is font 2. The explanation started out as font 1 to fit on two
// lines; at arm's length on this panel that was unreadable, which defeats the point
// of a warning. Re-wrapped to three shorter lines instead of shrinking the text.
static constexpr int16_t PL_MSG_Y       = 42;
static constexpr int16_t PL_MSG_PITCH   = 20;
static constexpr int16_t PL_LIST_Y      = 106;
static constexpr int16_t PL_LIST_PITCH  = 16;    // font 2 height; 19 fitted six, not seven
static constexpr int16_t PL_ROW1_Y      = 221;   // seven restored cures end at 218
static constexpr int16_t PL_ROW2_Y      = PL_ROW1_Y + LIST_PITCH;

static void drawPowerLoss() {
  // No X. This is the one thing that must be answered rather than dismissed — the
  // same reasoning as the alarm popup.
  drawPanel("Power was interrupted", C_ORANGE, false);

  tft.setTextColor(C_TEXT, rgb(13, 20, 13));
  tft.drawString("These cures were restored from before the power cut.",
                 BODY_X, PL_MSG_Y, 2);
  tft.setTextColor(C_ORANGE, rgb(13, 20, 13));
  tft.drawString("Nothing was curing while the power was off.",
                 BODY_X, PL_MSG_Y + PL_MSG_PITCH, 2);
  tft.drawString("Check the resin before you trust these times.",
                 BODY_X, PL_MSG_Y + 2 * PL_MSG_PITCH, 2);

  uint8_t shown = 0;
  char line[64], rem[9];
  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
    const Station::Slot& s = Station::slot(i);
    if (s.state != Station::SlotState::Running) continue;
    Station::formatRemaining(s.remainingSec, rem);
    snprintf(line, sizeof(line), "Slot %u  -  %s  -  %s left", i + 1, s.name, rem);
    tft.setTextColor(C_GREEN, rgb(13, 20, 13));
    tft.drawString(line, BODY_X + 8, PL_LIST_Y + shown * PL_LIST_PITCH, 2);
    shown++;
  }

  // Keep is on top and in plain text: it is the reflexive tap, and it is the option
  // that loses nothing. Clearing seven people's timers is the destructive one.
  drawListRow(PL_ROW1_Y, "Keep these times", nullptr, C_TEXT, true);
  drawListRow(PL_ROW2_Y, "Clear all and start over", nullptr, C_RED, true);
}

// --- heating mat off timer (menu item 5) -----------------------------------------
// Follows openHeatMenu / openHeatTimer / openHeatWarn in the mockup. Added 2026-09-15.
// Three screens: the mat's state with what can be done about it, a minutes entry, and
// a warning when the timer would cut the heat before a running cure finishes.

static uint16_t heatPendingMin = 0;  // minutes typed, held while HeatWarn asks
static int8_t heatWarnSlot = -1;     // the cure HeatWarn is warning about

// What HeatMenu is showing, so a second ticking by repaints only the countdown and a
// timer running out while the screen is open redraws the rows (they change).
static bool heatShownOn = true;
static uint32_t heatShownSec = 0;

// The status line alone — repainted every second while a timer runs. Padded rather
// than cleared first, so the text never blanks (same reason as the slot clock).
static void drawHeatMenuValue() {
  heatShownOn = Station::heatEnabled();
  heatShownSec = Station::heatOffRemainingSec();

  char big[32], rem[9];
  uint16_t col;
  if (!heatShownOn)       { snprintf(big, sizeof(big), "Mat is OFF"); col = C_RED; }
  else if (heatShownSec)  { Station::formatRemaining(heatShownSec, rem);
                            snprintf(big, sizeof(big), "Off in %s", rem); col = C_ORANGE; }
  else                    { snprintf(big, sizeof(big), "Mat is ON - no off timer"); col = C_GREEN; }

  tft.setTextColor(col, rgb(13, 20, 13));
  tft.setTextPadding(BODY_W);
  tft.drawString(big, BODY_X, SET_VAL_Y, 4);
  tft.setTextPadding(0);
}

static void drawHeatMenu() {
  drawPanel("Heating mat");
  drawHeatMenuValue();

  tft.setTextColor(C_DIM, rgb(13, 20, 13));
  if (!heatShownOn) {
    tft.drawString("Switched off by the timer. It stays off until you turn it on.",
                   BODY_X, SET_HINT_Y + 8, 1);
    drawListRow(POP_ROW1_Y, "1   Turn heat ON", nullptr, C_GREEN, true);
  } else if (heatShownSec) {
    tft.drawString("The mat switches off when this reaches zero, and stays off.",
                   BODY_X, SET_HINT_Y + 8, 1);
    drawListRow(POP_ROW1_Y, "1   Change off timer", nullptr, C_TEXT, true);
    drawListRow(POP_ROW2_Y, "2   Cancel timer", nullptr, C_TEXT, true);
  } else {
    tft.drawString("The mat holds the setpoint. Set a timer to switch it off later.",
                   BODY_X, SET_HINT_Y + 8, 1);
    drawListRow(POP_ROW1_Y, "1   Set off timer", nullptr, C_TEXT, true);
  }
}

static void drawHeatTimer() {
  drawPanel("Heat off timer");

  tft.setTextColor(C_GREEN, rgb(13, 20, 13));
  tft.drawString("Switch the mat off in...", BODY_X, SET_VAL_Y, 4);

  tft.setTextColor(C_DIM, rgb(13, 20, 13));
  tft.drawString("Minutes from now  -  range 1-480 min  -  stays off until you turn it on",
                 BODY_X, SET_HINT_Y, 1);

  drawNumField();
  drawNumKeypad("Start");
}

static void drawHeatWarn() {
  if (heatWarnSlot < 0) return;
  drawPanel("Heat off before a cure ends?", C_ORANGE);

  char sub[32], rem[9];
  Station::formatRemaining(Station::slot(heatWarnSlot).remainingSec, rem);
  snprintf(sub, sizeof(sub), "%s left on this cure", rem);
  drawPopupIdentity((uint8_t)heatWarnSlot, false, sub, C_ORANGE);

  char line[64];
  snprintf(line, sizeof(line), "The mat switches off in %u min - before this cure finishes.",
           heatPendingMin);
  tft.setTextColor(C_RED, rgb(13, 20, 13));
  tft.setTextDatum(TC_DATUM);
  tft.drawString(line, PN_X + PN_W / 2, POP_WARN_Y, 1);
  tft.setTextDatum(TL_DATUM);

  // Safe option first and on top, as on the cancel confirmation.
  snprintf(line, sizeof(line), "2   Yes, switch off in %u min", heatPendingMin);
  drawListRow(POP_ROW1_Y, "1   No, change the time", nullptr, C_TEXT, true);
  drawListRow(POP_ROW2_Y, line, nullptr, C_RED, true);
}

static void drawCurrentScreen() {
  switch (screen) {
    case Screen::ChooseSlot:    drawChooseSlot();    break;
    case Screen::ChooseUser:    drawChooseUser();    break;
    case Screen::EnterTime:     drawEnterTime();     break;
    case Screen::ConfirmCancel: drawConfirmCancel(); break;
    case Screen::Alarm:         drawAlarm();         break;
    case Screen::Menu:          drawMenu();          break;
    case Screen::SetTemp:       drawSetTemp();       break;
    case Screen::TempLog:       drawTempLog();       break;
    case Screen::SetAlarm:      drawSetAlarm();      break;
    case Screen::ManageUsers:   drawManageUsers();   break;
    case Screen::EditSound:     drawEditSound();     break;
    case Screen::AddUserName:   drawAddUserName();   break;
    case Screen::AddUserSound:  drawAddUserSound();  break;
    case Screen::UserActions:   drawUserActions();   break;
    case Screen::PowerLoss:     drawPowerLoss();     break;
    case Screen::HeatMenu:      drawHeatMenu();      break;
    case Screen::HeatTimer:     drawHeatTimer();     break;
    case Screen::HeatWarn:      drawHeatWarn();      break;
    default: break;
  }
}

static void closeOverlay();  // defined with the public section

static void goToScreen(Screen s) {
  screen = s;
  if (s == Screen::Main) { closeOverlay(); return; }
  tft.fillScreen(C_BG);
  drawCurrentScreen();
}

static void openStartFlow(int8_t presetSlot) {
  selSlot = presetSlot;
  selUser = -1;
  userPage = 0;
  timeBuf[0] = '\0';
  timeErr = nullptr;
  slotWasPreset = (presetSlot >= 0);
  goToScreen(slotWasPreset ? Screen::ChooseUser : Screen::ChooseSlot);
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

// --- overlay touch ---------------------------------------------------------------

// Returns true if the tap was the title-bar X. The target is the whole right end of
// the title bar, wider than the drawn button, so a near miss still counts.
static bool tappedClose(int16_t x, int16_t y) {
  return (y >= PN_Y && y < PN_Y + PN_TITLE_H && x >= CLOSE_X - 12);
}

static void onChooseSlotTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::Main); return; }
  for (uint8_t i = 0; i < SLOT_ROWS; i++) {
    int16_t top = slotListY(i);
    if (y < top || y >= top + SLOT_LIST_H) continue;
    // Running slots are not selectable — they must be cancelled first.
    if (Station::slot(i).state == Station::SlotState::Running) return;
    selSlot = (int8_t)i;
    goToScreen(Screen::ChooseUser);
    return;
  }
}

static void onChooseUserTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) {
    // Back behaviour depends on how the flow was entered, matching the mockup.
    goToScreen(slotWasPreset ? Screen::Main : Screen::ChooseSlot);
    return;
  }

  if (userPageCount() > 1 && y >= PAGER_Y && y < PAGER_Y + 32) {
    if (x < BODY_X + 120) {
      userPage = (userPage == 0) ? userPageCount() - 1 : userPage - 1;
      drawChooseUser();
    } else if (x >= BODY_X + BODY_W - 120) {
      userPage = (userPage + 1) % userPageCount();
      drawChooseUser();
    }
    return;
  }

  for (uint8_t r = 0; r < USER_ROWS; r++) {
    int16_t top = listRowY(r);
    if (y < top || y >= top + LIST_ROW_H) continue;
    uint8_t idx = userPage * USER_ROWS + r;
    if (idx >= Station::userCount()) return;
    selUser = (int8_t)idx;
    goToScreen(Screen::EnterTime);
    return;
  }
}

static void timeAppend(char c) {
  uint8_t n = strlen(timeBuf);
  if (n >= 3) return;              // 480 is the maximum, 3 digits is enough
  if (n == 0 && c == '0') return;  // no leading zero
  timeBuf[n] = c;
  timeBuf[n + 1] = '\0';
  timeErr = nullptr;
}

static void timeBackspace() {
  uint8_t n = strlen(timeBuf);
  if (n > 0) timeBuf[n - 1] = '\0';
  timeErr = nullptr;
}

static void timeConfirm() {
  int n = atoi(timeBuf);
  if (timeBuf[0] == '\0' || n < Station::TIME_MIN_MIN || n > Station::TIME_MAX_MIN) {
    timeErr = "Enter 1-480 min";
    timeBuf[0] = '\0';
    drawTimeField();
    return;
  }
  const Station::User& u = Station::user(selUser);
  if (!Station::startSlot((uint8_t)selSlot, u.name, u.sound, (uint16_t)n)) {
    timeErr = "Could not start - check the slot";
    drawTimeField();
    return;
  }
  goToScreen(Screen::Main);
}

static void onEnterTimeTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::ChooseUser); return; }

  if (y >= PRESET_Y && y < PRESET_Y + PRESET_H) {
    for (uint8_t i = 0; i < 4; i++) {
      int16_t px = BODY_X + i * (PRESET_W + 5);
      if (x < px || x >= px + PRESET_W) continue;
      snprintf(timeBuf, sizeof(timeBuf), "%u", PRESET_MIN[i]);
      timeErr = nullptr;
      drawTimeField();
      return;
    }
    return;
  }

  if (y < KEY_Y0) return;
  for (uint8_t r = 0; r < 4; r++) {
    int16_t ky = KEY_Y0 + r * KEY_PITCH;
    if (y < ky || y >= ky + KEY_H) continue;
    for (uint8_t c = 0; c < 3; c++) {
      int16_t kx = BODY_X + c * KEY_PITCH_X;
      if (x < kx || x >= kx + KEY_W) continue;

      if (r == 3 && c == 0) { timeBackspace(); drawTimeField(); return; }
      if (r == 3 && c == 2) { timeConfirm(); return; }
      if (r == 3 && c == 1) { timeAppend('0'); drawTimeField(); return; }
      timeAppend((char)('7' - r * 3 + c));
      drawTimeField();
      return;
    }
    return;
  }
}

static void onConfirmCancelTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::Main); return; }

  if (y >= POP_ROW1_Y && y < POP_ROW1_Y + LIST_ROW_H) {   // No, keep running
    goToScreen(Screen::Main);
    return;
  }
  if (y >= POP_ROW2_Y && y < POP_ROW2_Y + LIST_ROW_H) {   // Yes, cancel
    // cancelSlot also drops the slot from the alarm queue, so a cure cancelled in
    // its final second cannot leave an alarm pending for a slot that is now empty.
    Station::cancelSlot((uint8_t)selSlot);
    goToScreen(Screen::Main);
    return;
  }
}

static void onAlarmTouch(int16_t x, int16_t y) {
  (void)x;
  // No close button and no dismiss-by-tapping-outside: the only way out is Stop, and
  // each card has to be stopped on its own.
  uint8_t n = Station::pendingAlarmCount();
  for (uint8_t k = 0; k < n; k++) {
    int16_t top = alarmCardY(k);
    if (y < top || y >= top + ALARM_CARD_H) continue;

    int8_t slotIdx = Station::pendingAlarmAt(k);
    if (slotIdx < 0) return;
    // The slot stays Done and on the main screen until "Clear done" — stopping the
    // alarm acknowledges the noise, it does not tidy the slot away.
    Station::acknowledgeAlarm((uint8_t)slotIdx);

    // Last one acknowledged closes the popup; otherwise redraw with the rest still
    // listed, exactly as the mockup re-renders in place.
    if (Station::hasPendingAlarm()) { tft.fillScreen(C_BG); drawAlarm(); }
    else                            { goToScreen(Screen::Main); }
    return;
  }
}

// --- step D touch ----------------------------------------------------------------

static void openMenu() {
  goToScreen(Screen::Menu);
}

static void openSetTemp() {
  numBuf[0] = '\0';
  numErr = nullptr;
  goToScreen(Screen::SetTemp);
}

static void openSetAlarm() {
  numBuf[0] = '\0';
  numErr = nullptr;
  goToScreen(Screen::SetAlarm);
}

static void openManageUsers() {
  managePage = 0;
  goToScreen(Screen::ManageUsers);
}

static void openHeatMenu() {
  goToScreen(Screen::HeatMenu);
}

static void openHeatTimer() {
  numBuf[0] = '\0';
  numErr = nullptr;
  goToScreen(Screen::HeatTimer);
}

// '#' / Start on the minutes screen. Goes to Main on success, so the countdown that
// just appeared in the header is the confirmation.
static void heatConfirm() {
  int n = atoi(numBuf);
  if (numBuf[0] == '\0' || n < Station::TIME_MIN_MIN || n > Station::TIME_MAX_MIN) {
    numErr = "Enter 1-480 min";
    numBuf[0] = '\0';
    drawNumField();
    return;
  }
  // The old timer can run out while this screen is open; then there is nothing left
  // to switch off, and the heat menu shows the mat as OFF.
  if (!Station::heatEnabled()) { openHeatMenu(); return; }

  heatPendingMin = (uint16_t)n;
  int8_t longest = Station::longestRunningSlot();
  if (longest >= 0 && Station::slot(longest).remainingSec > (uint32_t)n * 60UL) {
    heatWarnSlot = longest;
    goToScreen(Screen::HeatWarn);
    return;
  }
  Station::startHeatOffTimer(heatPendingMin);
  goToScreen(Screen::Main);
}

static void heatWarnAccept() {
  Station::startHeatOffTimer(heatPendingMin);   // false only if the mat went off meanwhile
  goToScreen(Screen::Main);
}

static void onMenuTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::Main); return; }

  for (uint8_t i = 0; i < MENU_COUNT; i++) {
    int16_t top = listRowY(i);
    if (y < top || y >= top + LIST_ROW_H) continue;
    switch (i) {
      case 0: openManageUsers();           break;
      case 1: openSetTemp();               break;
      case 2: goToScreen(Screen::TempLog); break;
      case 3: openSetAlarm();              break;
      case 4: openHeatMenu();              break;
    }
    return;
  }
}

static void numAppend(char c) {
  uint8_t n = strlen(numBuf);
  // 50 C and 60 s both fit in two digits; the heat off timer goes to 480 min.
  if (n >= (screen == Screen::HeatTimer ? 3 : 2)) return;
  numBuf[n] = c;
  numBuf[n + 1] = '\0';
  numErr = nullptr;
}

static void numBackspace() {
  uint8_t n = strlen(numBuf);
  if (n > 0) numBuf[n - 1] = '\0';
  numErr = nullptr;
}

// Shared by both settings screens. Returns true if the value was accepted.
static bool numConfirm(bool isTemp) {
  int n = atoi(numBuf);
  int lo = isTemp ? Station::SETPOINT_MIN_C : Station::ALARM_MIN_SEC;
  int hi = isTemp ? Station::SETPOINT_MAX_C : Station::ALARM_MAX_SEC;
  if (numBuf[0] == '\0' || n < lo || n > hi) {
    numErr = isTemp ? "Must be 40-50 C" : "Must be 5-60 sec";
    numBuf[0] = '\0';
    drawNumField();
    return false;
  }
  if (isTemp) Station::setSetpointC((uint8_t)n);
  else        Station::setAlarmDurationSec((uint8_t)n);
  return true;
}

// The 3-wide keypad shared by both settings screens.
static void onNumKeypadTouch(int16_t x, int16_t y, bool isTemp) {
  if (y < SET_KEY_Y0) return;
  for (uint8_t r = 0; r < 4; r++) {
    int16_t ky = SET_KEY_Y0 + r * SET_KEY_PITCH;
    if (y < ky || y >= ky + SET_KEY_H) continue;
    for (uint8_t c = 0; c < 3; c++) {
      int16_t kx = BODY_X + c * KEY_PITCH_X;
      if (x < kx || x >= kx + KEY_W) continue;

      if (r == 3 && c == 0) { numBackspace(); drawNumField(); return; }
      if (r == 3 && c == 1) { numAppend('0'); drawNumField(); return; }
      if (r == 3 && c == 2) {
        if (screen == Screen::HeatTimer)  heatConfirm();   // navigates on its own
        else if (numConfirm(isTemp))      goToScreen(Screen::Menu);
        return;
      }
      numAppend((char)('7' - r * 3 + c));
      drawNumField();
      return;
    }
    return;
  }
}

static void onSetTempTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::Menu); return; }
  onNumKeypadTouch(x, y, true);
}

// Read-only screen — the X is the only control.
static void onTempLogTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) goToScreen(Screen::Menu);
}

static void onSetAlarmTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::Menu); return; }
  if (y >= SET_FOOT_Y) { openManageUsers(); return; }
  onNumKeypadTouch(x, y, false);
}

static void onManageUsersTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::Menu); return; }

  if (y >= ADD_ROW_Y && y < ADD_ROW_Y + LIST_ROW_H) {
    nameBuf[0] = '\0';
    goToScreen(Screen::AddUserName);
    return;
  }

  if (managePageCount() > 1 && y >= MANAGE_PAGER_Y && y < MANAGE_PAGER_Y + PAGER_H) {
    if (x >= PREV_X && x < PREV_X + PAGER_BTN_W) {
      managePage = (managePage == 0) ? managePageCount() - 1 : managePage - 1;
      drawManageUsers();
    } else if (x >= NEXT_X && x < NEXT_X + PAGER_BTN_W) {
      managePage = (managePage + 1) % managePageCount();
      drawManageUsers();
    }
    return;
  }

  for (uint8_t r = 0; r < MANAGE_ROWS; r++) {
    int16_t top = listRowY(r);
    if (y < top || y >= top + LIST_ROW_H) continue;
    uint8_t idx = managePage * MANAGE_ROWS + r;
    if (idx >= Station::userCount()) return;

    if (x >= DEL_X) {
      // No confirmation, as in the mockup. A running cure copied the name into the
      // slot when it started, so deleting the user cannot disturb work in progress.
      Station::deleteUser(idx);
      drawManageUsers();
      return;
    }
    editUser = (int8_t)idx;
    goToScreen(Screen::EditSound);
    return;
  }
}

static void onEditSoundTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::ManageUsers); return; }
  for (uint8_t i = 0; i < DARK_TONES; i++) {
    int16_t top = listRowY(i);
    if (y < top || y >= top + LIST_ROW_H) continue;
    if (editUser >= 0) Station::setUserSound((uint8_t)editUser, i);
    goToScreen(Screen::ManageUsers);
    return;
  }
}

static void nameAppend(char c) {
  uint8_t n = strlen(nameBuf);
  if (n >= Station::USER_NAME_MAX) return;
  nameBuf[n] = c;
  nameBuf[n + 1] = '\0';
}

static void onAddUserNameTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::ManageUsers); return; }
  if (y < KB_Y0) return;

  for (uint8_t r = 0; r < 5; r++) {
    int16_t ky = kbRowY(r);
    if (y < ky || y >= ky + KB_H) continue;

    if (r == 4) {
      if (x >= KB_APOS_X && x < KB_APOS_X + KB_APOS_W)   { nameAppend('\''); drawNameField(); }
      else if (x >= KB_SPACE_X && x < KB_SPACE_X + KB_SPACE_W) { nameAppend(' '); drawNameField(); }
      else if (x >= KB_NEXT_X && x < KB_NEXT_X + KB_NEXT_W) {
        // A name of nothing but spaces is not a name.
        bool hasChar = false;
        for (const char* p = nameBuf; *p; p++) if (*p != ' ') hasChar = true;
        if (hasChar) goToScreen(Screen::AddUserSound);
      }
      return;
    }

    if (r == 3 && x >= KB_DEL_X && x < KB_DEL_X + KB_DEL_W) {
      uint8_t n = strlen(nameBuf);
      if (n > 0) nameBuf[n - 1] = '\0';
      drawNameField();
      return;
    }

    int16_t x0 = kbRowX0(r);
    for (uint8_t c = 0; KB_ROWS[r][c]; c++) {
      int16_t kx = x0 + c * KB_PITCH_X;
      if (x < kx || x >= kx + KB_W) continue;
      nameAppend(KB_ROWS[r][c]);
      drawNameField();
      return;
    }
    return;
  }
}

static void onAddUserSoundTouch(int16_t x, int16_t y) {
  // Back goes to the name step, not out of the flow — the typed name is kept.
  if (tappedClose(x, y)) { goToScreen(Screen::AddUserName); return; }

  for (uint8_t i = 0; i < DARK_TONES; i++) {
    int16_t top = listRowY(i);
    if (y < top || y >= top + LIST_ROW_H) continue;
    if (!Station::addUser(nameBuf, i)) {
      // Only reachable at MAX_USERS. Silently dropping it would look like a bug.
      Serial.println("[UI] user list is full, name not added");
    }
    goToScreen(Screen::ManageUsers);
    return;
  }
}

static void onUserActionsTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::ManageUsers); return; }
  (void)x;   // both rows span the full body width

  if (y >= POP_ROW1_Y && y < POP_ROW1_Y + LIST_ROW_H) {
    goToScreen(Screen::EditSound);
    return;
  }
  if (y >= POP_ROW2_Y && y < POP_ROW2_Y + LIST_ROW_H) {
    if (editUser >= 0) Station::deleteUser((uint8_t)editUser);
    editUser = -1;
    goToScreen(Screen::ManageUsers);
  }
}

static void onPowerLossTouch(int16_t x, int16_t y) {
  (void)x;   // both rows span the full body width, as on the cancel confirmation

  if (y >= PL_ROW1_Y && y < PL_ROW1_Y + LIST_ROW_H) {
    goToScreen(Screen::Main);
    return;
  }
  if (y >= PL_ROW2_Y && y < PL_ROW2_Y + LIST_ROW_H) {
    // Everything, not just the running slots: a cure that finished during the
    // outage is in the same boat and its alarm would fire the moment this closes.
    for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) Station::cancelSlot(i);
    goToScreen(Screen::Main);
  }
}

// Rows are drawn from the mat's state, so the touch has to read the same state.
static void onHeatMenuTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::Menu); return; }
  bool row1 = (y >= POP_ROW1_Y && y < POP_ROW1_Y + LIST_ROW_H);
  bool row2 = (y >= POP_ROW2_Y && y < POP_ROW2_Y + LIST_ROW_H);

  if (!Station::heatEnabled()) {
    if (row1) { Station::setHeatEnabled(true); goToScreen(Screen::Main); }
    return;
  }
  if (row1) { openHeatTimer(); return; }
  if (row2 && Station::heatOffRemainingSec() > 0) {
    Station::cancelHeatOffTimer();
    goToScreen(Screen::HeatMenu);
  }
}

static void onHeatTimerTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { goToScreen(Screen::HeatMenu); return; }
  onNumKeypadTouch(x, y, false);   // isTemp unused here — Start goes to heatConfirm()
}

static void onHeatWarnTouch(int16_t x, int16_t y) {
  if (tappedClose(x, y)) { openHeatTimer(); return; }
  if (y >= POP_ROW1_Y && y < POP_ROW1_Y + LIST_ROW_H) { openHeatTimer(); return; }
  if (y >= POP_ROW2_Y && y < POP_ROW2_Y + LIST_ROW_H) heatWarnAccept();
}

static Hit hitTest(int16_t x, int16_t y, uint8_t& slotOut) {
  if (y >= BOTTOM_Y) {
    if (x < 158) return Hit::Menu;
    if (x < 316) return Hit::Start;
    return Hit::ClearDone;
  }
  if (y < SLOT_Y0) return Hit::None; // header is not interactive

  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
    int16_t top = slotY(i);
    if (y < top || y >= top + SLOT_H) continue;
    slotOut = i;
    // The X column only means anything on a slot that is actually running.
    if (x >= CANCEL_X && Station::slot(i).state == Station::SlotState::Running) {
      return Hit::SlotCancel;
    }
    return Hit::Slot;
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
      // Tapping an empty or finished row jumps straight in with that slot chosen.
      // A running row is not a shortcut to anything — it has to be cancelled first.
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
    default:
      break;
  }
}

// --- public ---------------------------------------------------------------------

namespace UI {

void begin() {
  tft.init();
  tft.setRotation(1); // landscape; the touch calibration above is tied to this
  tft.setTouch(touchCalibration);

  // Allocated once and reused by every row. If this ever fails (no RAM), the clock
  // simply would not draw — worth knowing rather than silently losing the countdown.
  timeSprite.setColorDepth(16);
  if (timeSprite.createSprite(TIME_BOX_W, TIME_BOX_H) == nullptr) {
    Serial.println("[UI] WARNING: clock sprite allocation failed, countdown will not draw");
  }
  tft.fillScreen(C_BG);
  lastTouchMs = millis();
  requestFullRedraw();
}

void setHeader(float currentTempC, bool heatOn, bool tempValid) {
  // Only a change of a tenth or more is worth a repaint; the raw float jitters.
  bool moved = (fabsf(currentTempC - shadowTemp) >= 0.05f);
  if (!moved && heatOn == shadowHeat && tempValid == shadowTempValid) return;
  shadowTemp = currentTempC;
  shadowHeat = heatOn;
  shadowTempValid = tempValid;

  // Only the main screen has a header. An overlay covers the whole panel, and the
  // temperature sits at x=380..478 — directly on top of the panel's X button, which
  // it was painting over once a second on every overlay screen. The new values are
  // still recorded above, so the header is correct as soon as the overlay closes.
  if (!sleeping && screen == Screen::Main) drawHeaderStatus();
}

void showPowerLossWarning() { goToScreen(Screen::PowerLoss); }

bool asleep() { return sleeping; }

void setSleepTimeout(uint32_t ms) { sleepAfterMs = ms; }

void requestFullRedraw() {
  fullRedraw = true;
  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) shadowValid[i] = false;
}

} // namespace UI

// Returns to the main screen and forces a repaint, since the overlay covered it.
static void closeOverlay() {
  screen = Screen::Main;
  UI::requestFullRedraw();
}

namespace UI {

static void goToSleep() {
  sleeping = true;
  tft.fillScreen(TFT_BLACK);
  tft.writecommand(0x28); // DISPOFF (MIPI DCS) — pixels off. The backlight is
                          // hardwired to 3V3 and cannot be switched here, see ui.h.
}

void wake() {
  if (!sleeping) { lastTouchMs = millis(); return; }
  sleeping = false;
  tft.writecommand(0x29); // DISPON
  tft.fillScreen(C_BG);
  requestFullRedraw();
  lastTouchMs = millis();
}

// Physical keypad input, following physKey() in the mockup. See ui.h for the three
// places this deliberately differs from it and why.
void handleKey(char k) {
  if (k == 0) return;

  // Same rule as touch: the press that wakes the screen must not also act on whatever
  // is underneath it — you cannot aim at a screen you cannot see.
  if (sleeping) { wake(); return; }
  lastTouchMs = millis(); // keypad activity counts as activity, or it would sleep mid-entry

  bool digit = (k >= '0' && k <= '9');

  switch (screen) {
    case Screen::Main:
      if (k == '#') { openStartFlow(-1); return; }
      // '0', not '*': '*' is back on every other screen, so it gets pressed here out
      // of habit and would clear the list on a reflex. '0' has no such habit and is
      // free — 1-6 are the slots. Labelled on the button, since a hidden key is worse
      // than no key. Only touches cures that already finished, as on the touch path.
      if (k == '0') { Station::clearAllDone(); return; }
      // '*' is back everywhere else and the main screen is the root, so it is free.
      // Safe to bind here precisely because opening the menu destroys nothing and
      // '*' closes it again — it reads as a toggle. That is why Clear done did NOT
      // go on this key.
      if (k == '*') { openMenu(); return; }
      if (k >= '1' && k <= '6') {
        uint8_t i = (uint8_t)(k - '1');
        // Same split as tapping the row: a running slot has to be cancelled first.
        if (Station::slot(i).state == Station::SlotState::Running) openConfirmCancel(i);
        else                                                       openStartFlow((int8_t)i);
      }
      return;

    case Screen::ChooseSlot:
      if (k == '*') { goToScreen(Screen::Main); return; }
      if (k >= '1' && k <= '6') {
        uint8_t i = (uint8_t)(k - '1');
        if (Station::slot(i).state == Station::SlotState::Running) return;
        selSlot = (int8_t)i;
        goToScreen(Screen::ChooseUser);
      }
      return;

    case Screen::ChooseUser:
      if (k == '*') { goToScreen(slotWasPreset ? Screen::Main : Screen::ChooseSlot); return; }
      // Paging, or the rows past the first page could not be reached at all. Checked
      // before the row digits because 7/9 are outside the 1-5 row range anyway.
      if (userPageCount() > 1 && (k == '7' || k == '9')) {
        if (k == '7') userPage = (userPage == 0) ? userPageCount() - 1 : userPage - 1;
        else          userPage = (userPage + 1) % userPageCount();
        drawChooseUser();
        return;
      }
      if (digit && k != '0') {
        uint8_t r = (uint8_t)(k - '1');
        if (r >= USER_ROWS) return;
        uint8_t idx = userPage * USER_ROWS + r;   // numbering is per page, not absolute
        if (idx >= Station::userCount()) return;
        selUser = (int8_t)idx;
        goToScreen(Screen::EnterTime);
      }
      return;

    case Screen::EnterTime:
      if (k == '*') { goToScreen(Screen::ChooseUser); return; }
      if (k == '#') { timeConfirm(); return; }
      if (digit) { timeAppend(k); drawTimeField(); }
      return;

    case Screen::SetTemp:
      if (k == '*') { goToScreen(Screen::Menu); return; }
      if (k == '#') { if (numConfirm(true)) goToScreen(Screen::Menu); return; }
      if (digit) { numAppend(k); drawNumField(); }
      return;

    case Screen::SetAlarm:
      if (k == '*') { goToScreen(Screen::Menu); return; }
      if (k == '#') { if (numConfirm(false)) goToScreen(Screen::Menu); return; }
      if (digit) { numAppend(k); drawNumField(); }
      return;

    case Screen::Menu:
      if (k == '*') { goToScreen(Screen::Main); return; }
      if (digit && k != '0') {
        uint8_t i = (uint8_t)(k - '1');
        if (i >= MENU_COUNT) return;
        switch (i) {
          case 0: openManageUsers();           break;
          case 1: openSetTemp();               break;
          case 2: goToScreen(Screen::TempLog); break;
          case 3: openSetAlarm();              break;
          case 4: openHeatMenu();              break;
        }
      }
      return;

    case Screen::HeatMenu:
      if (k == '*') { goToScreen(Screen::Menu); return; }
      if (!Station::heatEnabled()) {
        if (k == '1') { Station::setHeatEnabled(true); goToScreen(Screen::Main); }
        return;
      }
      if (k == '1') { openHeatTimer(); return; }
      if (k == '2' && Station::heatOffRemainingSec() > 0) {
        Station::cancelHeatOffTimer();
        goToScreen(Screen::HeatMenu);
      }
      return;

    case Screen::HeatTimer:
      if (k == '*') { goToScreen(Screen::HeatMenu); return; }
      if (k == '#') { heatConfirm(); return; }
      if (digit) { numAppend(k); drawNumField(); }
      return;

    case Screen::HeatWarn:
      // Same rule as the cancel confirmation: '*' and '1' take the safe row, and '#'
      // does nothing, so a reflex confirm cannot cut the heat under a running cure.
      if (k == '*' || k == '1') { openHeatTimer(); return; }
      if (k == '2') heatWarnAccept();
      return;

    case Screen::ConfirmCancel:
      // '*' and '1' both take the safe row. '#' deliberately does NOTHING here: the
      // whole point of putting "keep running" on top is that a stray confirm press
      // must not destroy a cure, and a blanket '#'=OK would undo that.
      if (k == '*' || k == '1') { goToScreen(Screen::Main); return; }
      if (k == '2') {
        Station::cancelSlot((uint8_t)selSlot);
        goToScreen(Screen::Main);
      }
      return;

    case Screen::PowerLoss:
      // No '*' — like the alarm popup this has to be answered, not dismissed. The two
      // digits are the two rows, safe one first, same as the cancel confirmation.
      if (k == '1') { goToScreen(Screen::Main); return; }
      if (k == '2') {
        for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) Station::cancelSlot(i);
        goToScreen(Screen::Main);
      }
      return;

    case Screen::ManageUsers:
      if (k == '*') { goToScreen(Screen::Menu); return; }
      if (k == '#') { nameBuf[0] = '\0'; goToScreen(Screen::AddUserName); return; }
      if (managePageCount() > 1 && (k == '7' || k == '9')) {
        if (k == '7') managePage = (managePage == 0) ? managePageCount() - 1 : managePage - 1;
        else          managePage = (managePage + 1) % managePageCount();
        drawManageUsers();
        return;
      }
      if (digit && k != '0') {
        uint8_t r = (uint8_t)(k - '1');
        if (r >= MANAGE_ROWS) return;
        uint8_t idx = managePage * MANAGE_ROWS + r;   // per page, as drawn
        if (idx >= Station::userCount()) return;
        editUser = (int8_t)idx;
        goToScreen(Screen::UserActions);
      }
      return;

    case Screen::UserActions:
      if (k == '*') { goToScreen(Screen::ManageUsers); return; }
      // No '#': the safe row is 1 and delete is 2, same rule as the cancel confirm.
      if (k == '1') { goToScreen(Screen::EditSound); return; }
      if (k == '2') {
        if (editUser >= 0) Station::deleteUser((uint8_t)editUser);
        editUser = -1;
        goToScreen(Screen::ManageUsers);
      }
      return;

    case Screen::TempLog:
      if (k == '*') goToScreen(Screen::Menu);
      return;

    case Screen::EditSound:
      // Back to the popup that opened it, not past it, so a mis-press costs one step.
      if (k == '*') { goToScreen(Screen::UserActions); return; }
      if (digit && k != '0') {
        uint8_t i = (uint8_t)(k - '1');
        if (i >= DARK_TONES) return;
        if (editUser >= 0) Station::setUserSound((uint8_t)editUser, i);
        goToScreen(Screen::ManageUsers);
      }
      return;

    case Screen::AddUserSound:
      // Back goes to the name step, not out of the flow — the typed name is kept.
      if (k == '*') { goToScreen(Screen::AddUserName); return; }
      if (digit && k != '0') {
        uint8_t i = (uint8_t)(k - '1');
        if (i >= DARK_TONES) return;
        if (!Station::addUser(nameBuf, i)) Serial.println("[UI] user list is full, name not added");
        goToScreen(Screen::ManageUsers);
      }
      return;

    case Screen::AddUserName:
      // Letters cannot come from a numeric pad, so this screen stays touch-driven.
      // '*' is still useful as back, matching its X.
      if (k == '*') goToScreen(Screen::ManageUsers);
      return;

    case Screen::Alarm:
      // The digit is the SLOT number shown on the card ("Stop (3)"), not the card's
      // position — so it still has to be aimed at a specific finished cure, which is
      // what "acknowledged deliberately" means. There is deliberately no key that
      // silences everything at once, and no '*': the popup has no X either.
      {
        int8_t target = -1;
        if (k == '#') {
          // The top card, which is the oldest unacknowledged cure. Pressing # again
          // takes the next one, so a stack can be cleared without reading the digits.
          target = Station::pendingAlarmAt(0);
        } else if (k >= '1' && k <= '6') {
          uint8_t i = (uint8_t)(k - '1');
          uint8_t n = Station::pendingAlarmCount();
          for (uint8_t q = 0; q < n; q++) {
            if (Station::pendingAlarmAt(q) == (int8_t)i) { target = (int8_t)i; break; }
          }
        }
        if (target < 0) return;   // no card for that key — ignore the press
        Station::acknowledgeAlarm((uint8_t)target);
        if (Station::hasPendingAlarm()) { tft.fillScreen(C_BG); drawAlarm(); }
        else                            { goToScreen(Screen::Main); }
      }
      return;
  }
}

void remoteStopAlarm() {
  int8_t slot = Station::pendingAlarmAt(0);   // oldest, same target as the '#' key
  if (slot < 0) return;
  Station::acknowledgeAlarm((uint8_t)slot);
  if (screen == Screen::Alarm) {
    if (Station::hasPendingAlarm()) { tft.fillScreen(C_BG); drawAlarm(); }
    else                            { goToScreen(Screen::Main); }
  }
}

void service() {
  uint16_t tx = 0, ty = 0;
  bool touched = tft.getTouch(&tx, &ty);
  bool newPress = touched && !wasTouched;
  wasTouched = touched;

  if (newPress) {
    if (sleeping) {
      // The touch that wakes the screen must not also press whatever happens to be
      // under the finger — you cannot aim at a screen you cannot see.
      wake();
      return;
    }
    lastTouchMs = millis();
    switch (screen) {
      case Screen::ChooseSlot:    onChooseSlotTouch((int16_t)tx, (int16_t)ty);    break;
      case Screen::ChooseUser:    onChooseUserTouch((int16_t)tx, (int16_t)ty);    break;
      case Screen::EnterTime:     onEnterTimeTouch((int16_t)tx, (int16_t)ty);     break;
      case Screen::ConfirmCancel: onConfirmCancelTouch((int16_t)tx, (int16_t)ty); break;
      case Screen::Alarm:         onAlarmTouch((int16_t)tx, (int16_t)ty);         break;
      case Screen::Menu:          onMenuTouch((int16_t)tx, (int16_t)ty);          break;
      case Screen::SetTemp:       onSetTempTouch((int16_t)tx, (int16_t)ty);       break;
      case Screen::TempLog:       onTempLogTouch((int16_t)tx, (int16_t)ty);       break;
      case Screen::SetAlarm:      onSetAlarmTouch((int16_t)tx, (int16_t)ty);      break;
      case Screen::ManageUsers:   onManageUsersTouch((int16_t)tx, (int16_t)ty);   break;
      case Screen::EditSound:     onEditSoundTouch((int16_t)tx, (int16_t)ty);     break;
      case Screen::AddUserName:   onAddUserNameTouch((int16_t)tx, (int16_t)ty);   break;
      case Screen::AddUserSound:  onAddUserSoundTouch((int16_t)tx, (int16_t)ty);  break;
      case Screen::UserActions:   onUserActionsTouch((int16_t)tx, (int16_t)ty);   break;
      case Screen::PowerLoss:     onPowerLossTouch((int16_t)tx, (int16_t)ty);     break;
      case Screen::HeatMenu:      onHeatMenuTouch((int16_t)tx, (int16_t)ty);      break;
      case Screen::HeatTimer:     onHeatTimerTouch((int16_t)tx, (int16_t)ty);     break;
      case Screen::HeatWarn:      onHeatWarnTouch((int16_t)tx, (int16_t)ty);      break;
      case Screen::Main: {
        uint8_t slotIdx = 0;
        Hit h = hitTest((int16_t)tx, (int16_t)ty, slotIdx);
        handleHit(h, slotIdx);
        break;
      }
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
    tft.fillScreen(C_BG);
    drawTempLog();
    return;
  }

  // A second cure finishing while the popup is already open adds its card to it.
  if (screen == Screen::Alarm && alarmSigDiffers()) {
    tft.fillScreen(C_BG);
    drawAlarm();
    return;
  }

  // The heat menu's countdown ticks; if the timer runs out (or the state otherwise
  // changes) while it is open, the rows change too, so the whole panel is redrawn.
  if (screen == Screen::HeatMenu) {
    bool on = Station::heatEnabled();
    uint32_t sec = Station::heatOffRemainingSec();
    if (on != heatShownOn || (sec == 0) != (heatShownSec == 0)) {
      tft.fillScreen(C_BG);
      drawHeatMenu();
      return;
    }
    if (sec != heatShownSec) drawHeatMenuValue();
  }

  if (sleeping) return;

  // The countdowns keep running behind an overlay, but nothing on the main screen is
  // visible, so there is nothing to repaint until it closes.
  if (screen != Screen::Main) return;

  if (millis() - lastTouchMs >= sleepAfterMs) {
    goToSleep();
    return;
  }

  if (fullRedraw) {
    tft.fillScreen(C_BG);
    drawHeaderStatic();
    drawHeaderStatus();
    drawHeaderHeatTimer();
    drawBottomBar();
    fullRedraw = false;
  } else if (Station::heatEnabled() != shadowHeatEnabled ||
             Station::heatOffRemainingSec() != shadowHeatSec) {
    drawHeaderHeatTimer();
  }

  // Repaint as little as possible, at two levels.
  //
  // Nothing changed          -> draw nothing.
  // Only the countdown moved -> repaint just the clock box and the bar. This is the
  //                             once-per-second case, and repainting the whole row
  //                             here is what made the display flicker.
  // Anything else changed    -> repaint the whole row (state, name, duration).
  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
    const Station::Slot& s = Station::slot(i);

    if (shadowValid[i] && memcmp(&shadow[i], &s, sizeof(Station::Slot)) == 0) continue;

    bool tickOnly = shadowValid[i]
                 && shadow[i].state == s.state
                 && s.state == Station::SlotState::Running
                 && shadow[i].totalSec == s.totalSec
                 && shadow[i].sound == s.sound
                 && strcmp(shadow[i].name, s.name) == 0;

    if (tickOnly) {
      drawSlotBar(i);
      drawSlotTime(i);
    } else {
      drawSlot(i);
    }

    shadow[i] = s;
    shadowValid[i] = true;
  }
}

} // namespace UI
