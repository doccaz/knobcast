#pragma once
// display.h
// OLED display manager for 72x40 SSD1306 (I2C) via U8g2.
// Supports: HUD, menu lists with scrolling items, overlays.

#include "menu.h"
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

#ifndef PIN_OLED_SDA
#define PIN_OLED_SDA 5
#endif
#ifndef PIN_OLED_SCL
#define PIN_OLED_SCL 6
#endif

static constexpr int DISPLAY_W = 72;
static constexpr int DISPLAY_H = 40;
static constexpr int SCROLL_SPEED_MS = 50;
static constexpr int SCROLL_PAUSE_MS = 1500;
static constexpr int SCROLL_GAP_PX = 20;
static constexpr int ROW_H = 11;           // pixel height per text row
static constexpr int MAX_VISIBLE_ROWS = 3; // rows visible on 40px display

class Display {
public:
  void begin() {
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    _u8g2.begin();
    _u8g2.setContrast(128);
  }

  // ── Boot splash ──────────────────────────────────────────────────────
  void showSplash(const char *ip) {
    _u8g2.clearBuffer();
    _u8g2.setFont(u8g2_font_6x10_tr);
    _drawCentered(2, "KnobCast");
    _u8g2.setFont(u8g2_font_5x7_tr);
    _drawCentered(18, ip);
    _u8g2.sendBuffer();
  }

  // ── AP mode screen ───────────────────────────────────────────────────
  void showAPMode(const char *ssid, const char *ip) {
    _u8g2.clearBuffer();
    _u8g2.setFont(u8g2_font_5x7_tr);
    _drawCentered(2, "AP Mode");
    _drawCentered(14, ssid);
    _drawCentered(26, ip);
    _u8g2.sendBuffer();
  }

  // ── Connecting screen ────────────────────────────────────────────────
  void showConnecting(const char *label) {
    _u8g2.clearBuffer();
    _u8g2.setFont(u8g2_font_5x7_tr);
    _drawCentered(12, "Connecting");
    _drawCentered(24, label);
    _u8g2.sendBuffer();
  }

  // ── Brief overlay (volume change, etc) ───────────────────────────────
  void showOverlay(const char *line1, const char *line2 = nullptr) {
    strncpy(_ovLine1, line1 ? line1 : "", sizeof(_ovLine1) - 1);
    _ovLine1[sizeof(_ovLine1) - 1] = '\0';
    if (line2) {
      strncpy(_ovLine2, line2, sizeof(_ovLine2) - 1);
      _ovLine2[sizeof(_ovLine2) - 1] = '\0';
      _hasOvLine2 = true;
    } else {
      _hasOvLine2 = false;
    }
    _overlayUntil = millis() + 1200;
  }

  bool isOverlayActive() const { return millis() < _overlayUntil; }

  // ── Main HUD — redrawn every frame ───────────────────────────────────
  void drawHUD(const char *deviceName, const char *appName, float volume,
               bool muted, bool playing, bool connected,
               int barMode = 0, float currentTime = 0, float duration = 0) {
    _u8g2.clearBuffer();

    if (isOverlayActive()) {
      _drawOverlay();
      _u8g2.sendBuffer();
      return;
    }

    _u8g2.setFont(u8g2_font_5x7_tr);

    // Row 0: device name
    if (!connected) {
      _drawScrolling(0, 0, "No device");
    } else if (deviceName && deviceName[0]) {
      _drawScrolling(0, 0, deviceName);
    } else {
      _drawScrolling(0, 0, "Chromecast");
    }

    // Row 1: progress bar (volume or elapsed time)
    if (muted) {
      _drawCentered(13, "MUTED");
    } else if (barMode == 1 && playing && duration > 0) {
      // Elapsed time bar
      int barX = 2, barY = 14, barW = 48, barH = 7;
      _u8g2.drawFrame(barX, barY, barW, barH);
      float progress = currentTime / duration;
      if (progress > 1.0f) progress = 1.0f;
      int fillW = (int)(progress * (barW - 2));
      if (fillW > 0)
        _u8g2.drawBox(barX + 1, barY + 1, fillW, barH - 2);
      char timeStr[12];
      int mins = (int)currentTime / 60;
      int secs = (int)currentTime % 60;
      snprintf(timeStr, sizeof(timeStr), "%d:%02d", mins, secs);
      _u8g2.drawStr(barX + barW + 3, 20, timeStr);
    } else {
      // Volume bar (default, or elapsed time mode with nothing playing)
      int barX = 2, barY = 14, barW = 48, barH = 7;
      _u8g2.drawFrame(barX, barY, barW, barH);
      int fillW = (int)(volume * (barW - 2));
      if (fillW > 0)
        _u8g2.drawBox(barX + 1, barY + 1, fillW, barH - 2);
      char pct[6];
      snprintf(pct, sizeof(pct), "%d%%", (int)(volume * 100.0f + 0.5f));
      _u8g2.drawStr(barX + barW + 3, 20, pct);
    }

    // Row 2: play state + app
    if (!connected) {
      _drawScrolling(1, 26, "Press knob for menu");
    } else {
      char row2[80];
      const char *icon = playing ? "> " : "|| ";
      const char *app = (appName && appName[0]) ? appName : "Idle";
      snprintf(row2, sizeof(row2), "%s%s", icon, app);
      _drawScrolling(1, 26, row2);
    }

    _u8g2.sendBuffer();
  }

  // ── Menu rendering — redrawn every frame ─────────────────────────────
  void drawMenu(const Menu &menu) {
    _u8g2.clearBuffer();

    if (isOverlayActive()) {
      _drawOverlay();
      _u8g2.sendBuffer();
      return;
    }

    switch (menu.screen) {
    case MenuScreen::MAIN:
      _drawList("Menu", menu);
      break;
    case MenuScreen::DEVICE_LIST:
      _drawList("Devices", menu);
      break;
    case MenuScreen::DEVICE_ACTIONS:
      _drawList("Actions", menu);
      break;
    case MenuScreen::INFO:
      _drawList("Info", menu);
      break;
    case MenuScreen::SETTINGS:
      _drawList("Settings", menu);
      break;
    case MenuScreen::MENU_TIMEOUT:
      _drawList("Menu Timeout", menu);
      break;
    case MenuScreen::SCREEN_TIMEOUT:
      _drawList("Screen Timeout", menu);
      break;
    case MenuScreen::BAR_MODE:
      _drawList("Progress Bar", menu);
      break;
    case MenuScreen::SCANNING:
      _u8g2.setFont(u8g2_font_5x7_tr);
      _drawCentered(12, "Scanning...");
      break;
    case MenuScreen::CONNECTING:
      _u8g2.setFont(u8g2_font_5x7_tr);
      _drawCentered(6, "Connecting");
      _drawCentered(20, menu.items[0].label);
      break;
    default:
      break;
    }

    _u8g2.sendBuffer();
  }

  // ── Power Management ──────────────────────────────────────────────────
  void setPower(bool on) { _u8g2.setPowerSave(on ? 0 : 1); }

  U8G2_SSD1306_72X40_ER_F_HW_I2C &getU8g2() { return _u8g2; }

private:
  U8G2_SSD1306_72X40_ER_F_HW_I2C _u8g2{U8G2_R0, /* reset=*/U8X8_PIN_NONE};
  uint32_t _overlayUntil = 0;
  char _ovLine1[32] = {};
  char _ovLine2[32] = {};
  bool _hasOvLine2 = false;

  // ── Scroll state: 0=HUD row0, 1=HUD row2, 2..N=menu selected item ──
  static constexpr int NUM_SCROLLS = 4;
  struct ScrollState {
    char text[80] = {};
    int textW = 0;
    int offset = 0;
    uint32_t lastMs = 0;
    bool paused = true;
    uint32_t pauseUntil = 0;
  };
  ScrollState _scroll[NUM_SCROLLS];

  void _drawOverlay() {
    _u8g2.setFont(u8g2_font_6x10_tr);
    _drawCentered(8, _ovLine1);
    if (_hasOvLine2) {
      _u8g2.setFont(u8g2_font_5x7_tr);
      _drawCentered(24, _ovLine2);
    }
  }

  // ── Draw a scrollable list with title and cursor ─────────────────────
  void _drawList(const char *title, const Menu &menu) {
    _u8g2.setFont(u8g2_font_5x7_tr);
    int fontH = _u8g2.getAscent() + 1;

    // Title bar (row 0)
    _u8g2.drawBox(0, 0, DISPLAY_W, ROW_H);
    _u8g2.setDrawColor(0);
    _drawCentered(1, title);
    _u8g2.setDrawColor(1);

    // Visible window: up to 3 items below title
    int startIdx = 0;
    if (menu.cursor >= MAX_VISIBLE_ROWS) {
      startIdx = menu.cursor - MAX_VISIBLE_ROWS + 1;
    }

    for (int vi = 0; vi < MAX_VISIBLE_ROWS; vi++) {
      int idx = startIdx + vi;
      if (idx >= menu.count)
        break;

      int y = ROW_H + vi * ROW_H;
      bool selected = (idx == menu.cursor);

      if (selected) {
        // Highlight bar
        _u8g2.drawBox(0, y, DISPLAY_W, ROW_H - 1);
        _u8g2.setDrawColor(0);
      }

      // If selected, scroll long text. Otherwise truncate.
      if (selected) {
        _drawScrolling(2, y, menu.items[idx].label);
        // Scrolling sets draw color back, fix if needed
        _u8g2.setDrawColor(1);
      } else {
        // Truncate to fit
        char buf[15];
        strncpy(buf, menu.items[idx].label, 14);
        buf[14] = '\0';
        _u8g2.drawStr(2, y + fontH, buf);
      }

      if (selected)
        _u8g2.setDrawColor(1);
    }
  }

  void _drawScrolling(int scrollIdx, int y, const char *text) {
    ScrollState &s = _scroll[scrollIdx];
    _u8g2.setFont(u8g2_font_5x7_tr);
    int textW = _u8g2.getStrWidth(text);
    int drawY = y + _u8g2.getAscent() + 1;

    if (textW <= DISPLAY_W) {
      if (strcmp(s.text, text) != 0) {
        strncpy(s.text, text, sizeof(s.text) - 1);
        s.text[sizeof(s.text) - 1] = '\0';
        s.textW = textW;
        s.offset = 0;
        s.paused = true;
      }
      _u8g2.drawStr(2, drawY, text);
      return;
    }

    if (strcmp(s.text, text) != 0) {
      strncpy(s.text, text, sizeof(s.text) - 1);
      s.text[sizeof(s.text) - 1] = '\0';
      s.textW = textW;
      s.offset = 0;
      s.paused = true;
      s.pauseUntil = millis() + SCROLL_PAUSE_MS;
      s.lastMs = millis();
    }

    uint32_t now = millis();
    if (s.paused) {
      if (now >= s.pauseUntil) {
        s.paused = false;
        s.lastMs = now;
      }
    } else if (now - s.lastMs >= (uint32_t)SCROLL_SPEED_MS) {
      s.offset++;
      s.lastMs = now;
      if (s.offset >= s.textW + SCROLL_GAP_PX) {
        s.offset = 0;
        s.paused = true;
        s.pauseUntil = now + SCROLL_PAUSE_MS;
      }
    }

    int x1 = -s.offset;
    int x2 = x1 + s.textW + SCROLL_GAP_PX;
    _u8g2.setClipWindow(0, y, DISPLAY_W, y + ROW_H);
    _u8g2.drawStr(x1, drawY, s.text);
    _u8g2.drawStr(x2, drawY, s.text);
    _u8g2.setMaxClipWindow();
  }

  void _drawCentered(int y, const char *text) {
    int w = _u8g2.getStrWidth(text);
    int x = (DISPLAY_W - w) / 2;
    if (x < 0)
      x = 0;
    _u8g2.drawStr(x, y + _u8g2.getAscent() + 1, text);
  }
};
