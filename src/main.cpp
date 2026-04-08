// main.cpp
// ESP32-C3 Chromecast Physical Remote
//
// Passive controller — connects to existing Chromecast sessions
// (e.g. Spotify) without interrupting playback.
//
// Controls:
//   HUD mode:  encoder rotate = volume, encoder click = open menu,
//              long press = mute toggle
//   Menu mode: encoder rotate = navigate, encoder click = select,
//              GPIO 9 = back

#include <Arduino.h>
#include <WiFi.h>

// ── Pin assignments (ESP32-C3, GPIO 0–21) ────────────────────────────────────
#define PIN_ENC_CLK 2
#define PIN_ENC_DT 3
#define PIN_ENC_SW 4
#define PIN_LED 8
#define PIN_BTN 9 // back button

// ── Includes (after pin defines) ─────────────────────────────────────────────
#include "chromecast_client.h"
#include "config.h"
#include "debug_log.h"
#include "display.h"
#include "menu.h"
#include "rotary_encoder.h"
#include "web_server.h"

// ── App state machine (boot flow only — no auto cast connect) ────────────────
enum class AppState {
  INIT,
  AP_MODE,
  WIFI_CONNECTING,
  WIFI_CONNECTED,
  READY, // HUD + menu + web server, no auto-cast-connect
};

// ── Global instances ────────────────────────────────────────────────────────
DebugLog dbgLog;
static ConfigStore config;
static RotaryEncoder encoder;
static ChromecastClient cast;
static Display display;
static CastWebServer webServer;
static Menu menu;
static AppState state = AppState::INIT;
static uint32_t stateEnteredMs = 0;
static bool webServerStarted = false;
static int infoScrollPos = 0;

// ── Volume throttle ─────────────────────────────────────────────────────────
static float pendingVolume = -1.0f;
static uint32_t lastVolSendMs = 0;
static constexpr uint32_t VOL_SEND_INTERVAL_MS = 150;

// ── Inactivity timers ────────────────────────────────────────────────────────
static uint32_t lastActivityMs = 0;
static bool displayOn = true;

static void resetActivity() {
  lastActivityMs = millis();
  if (!displayOn) {
    display.setPower(true);
    displayOn = true;
  }
}

// ── Back button (GPIO 9) debounce ────────────────────────────────────────────
static bool backBtnLast = true;
static uint32_t backBtnDebounceMs = 0;

static bool backButtonPressed() {
  bool btn = digitalRead(PIN_BTN);
  uint32_t now = millis();
  bool pressed = false;

  if (now - backBtnDebounceMs >= 200) {
    if (btn == LOW && backBtnLast == HIGH) {
      backBtnDebounceMs = now;
      pressed = true;
    }
  }
  backBtnLast = btn;
  return pressed;
}

// ── Always-run services ──────────────────────────────────────────────────────
static void serviceAlways() {
  if (webServerStarted) {
    webServer.loop();
    if (webServer.consumeConnectEvent()) {
      dbgLog.log("[Web] Cast connected via web UI");
    }
  }
  if (cast.isConnected()) {
    cast.loop();
    // Flush pending throttled volume
    if (pendingVolume >= 0.0f &&
        millis() - lastVolSendMs >= VOL_SEND_INTERVAL_MS) {
      cast.setVolume(pendingVolume);
      dbgLog.log("[Remote] Volume → %.0f%%\n", pendingVolume * 100.0f);
      pendingVolume = -1.0f;
      lastVolSendMs = millis();
    }
  }
}

// ── Build device labels (appends play state for connected device) ────────────
static char deviceLabels[MAX_CAST_DEVICES][64];

static void buildDeviceLabels() {
  for (int i = 0; i < cast.deviceCount; i++) {
    char baseName[72];
    if (cast.devices[i].isGroup) {
      snprintf(baseName, sizeof(baseName), "[G] %s", cast.devices[i].name);
    } else {
      strncpy(baseName, cast.devices[i].name, sizeof(baseName) - 1);
      baseName[sizeof(baseName) - 1] = '\0';
    }

    if (cast.isConnected() && strcmp(cast.devices[i].ip, cast.getHost()) == 0 &&
        cast.getAppName()[0]) {
      snprintf(deviceLabels[i], sizeof(deviceLabels[i]), "%s (%s%s)", baseName,
               cast.isPlaying() ? "> " : "", cast.getAppName());
    } else {
      strncpy(deviceLabels[i], baseName, sizeof(deviceLabels[i]) - 1);
      deviceLabels[i][sizeof(deviceLabels[i]) - 1] = '\0';
    }
  }
}

static void showDeviceListMenu() {
  buildDeviceLabels();
  const char *names[MAX_CAST_DEVICES];
  for (int i = 0; i < cast.deviceCount; i++)
    names[i] = deviceLabels[i];
  menu.showDeviceList(names, cast.deviceCount);
}

// ── Menu action: scan network ────────────────────────────────────────────────
static void menuDoScan() {
  menu.showScanning();
  display.drawMenu(menu); // show "Scanning..." immediately

  cast.discoverAll(5000);
  showDeviceListMenu();
}

// ── Menu action: connect to device at index ──────────────────────────────────
static void menuDoConnect(int idx) {
  if (idx < 0 || idx >= cast.deviceCount)
    return;
  char buf[64];
  snprintf(buf, sizeof(buf), "Connecting to %s...", cast.devices[idx].name);
  display.showOverlay(buf);

  if (cast.isConnected())
    cast.disconnect();

  // Pass the discovered port (e.g., 32234 for groups) instead of defaulting to
  // 8009
  if (cast.connect(cast.devices[idx].ip, cast.devices[idx].port)) {
    cast.setFriendlyName(cast.devices[idx].name);
    config.saveLastDevice(cast.devices[idx].ip, cast.devices[idx].name,
                          cast.devices[idx].port);
    display.showOverlay("Connected!", cast.devices[idx].name);
    dbgLog.log("[Menu] Connected to %s\n", cast.devices[idx].name);
  } else {
    display.showOverlay("Failed", cast.devices[idx].name);
    dbgLog.log("[Menu] Failed to connect to %s\n", cast.devices[idx].name);
  }
  menu.close();
}

// ── Handle input when menu is open ───────────────────────────────────────────
static void handleMenuInput() {
  EncEvent ev = encoder.poll();
  bool back = backButtonPressed();

  // Back button
  if (back) {
    resetActivity();
    switch (menu.screen) {
    case MenuScreen::DEVICE_LIST:
    case MenuScreen::INFO:
      menu.open(cast.isConnected(), cast.deviceCount > 0); // back to main menu
      break;
    case MenuScreen::DEVICE_ACTIONS:
      showDeviceListMenu();
      break;
    case MenuScreen::SETTINGS:
    case MenuScreen::MENU_TIMEOUT:
    case MenuScreen::SCREEN_TIMEOUT:
    case MenuScreen::BAR_MODE:
      menu.open(cast.isConnected(), cast.deviceCount > 0);
      break;
    default:
      menu.close(); // close menu entirely
      break;
    }
    return;
  }

  // Encoder rotation = navigate (or scroll info screen)
  if (ev == EncEvent::CW || ev == EncEvent::CCW) {
    resetActivity();
    menu.navigate(encoder.delta());
    return;
  }

  // Encoder press = select
  if (ev != EncEvent::PRESS)
    return;

  switch (menu.screen) {
  case MenuScreen::MAIN: {
    const char *sel = menu.selectedLabel();
    if (strcmp(sel, "Actions") == 0) {
      // Pre-flight sync to ensure fresh status
      cast.loop();
      int connIdx = cast.getConnectedDeviceIdx();
      if (connIdx >= 0) {
        menu.showDeviceActions(connIdx, cast.devices[connIdx].name, true,
                               cast.isPlaying());
      } else {
        // Fallback if not in discovered list: use current session name
        menu.showDeviceActions(-1, cast.getFriendlyName(), true,
                               cast.isPlaying());
      }
    } else if (strcmp(sel, "Devices") == 0) {
      showDeviceListMenu();
    } else if (strcmp(sel, "Scan network") == 0) {
      menuDoScan();
    } else if (strcmp(sel, "Connection info") == 0) {
      static char ipRow[32], ssidRow[32], devRow[64], appRow[64], verRow[32];
      snprintf(ipRow, sizeof(ipRow), "IP: %s",
               WiFi.localIP().toString().c_str());
      snprintf(ssidRow, sizeof(ssidRow), "SSID: %s", WiFi.SSID().c_str());
      snprintf(devRow, sizeof(devRow), "Dev: %s", cast.getFriendlyName());
      snprintf(appRow, sizeof(appRow), "App: %s", cast.getAppName());
      snprintf(verRow, sizeof(verRow), "Ver: 1.0.6");

      const char *rows[] = {ipRow, ssidRow, devRow, appRow, verRow};
      menu.showInfo(rows, 5);
    } else if (strcmp(sel, "Disconnect") == 0) {
      cast.disconnect();
      display.showOverlay("Disconnected");
      menu.open(false, cast.deviceCount > 0);
    } else if (strcmp(sel, "About") == 0) {
      const char *rows[] = {"KnobCast", "Firmware v1.0.6",
                            "by Erico Mendonca",
                            "github.com/doccaz/knobcast"
                            "Built: " __DATE__,
                            "ESP32-C3 DevKit"};
      menu.showInfo(rows, 5);
    } else if (strcmp(sel, "Settings") == 0) {
      menu.showSettingsMenu(config.cfg.scanOnBoot, config.cfg.barMode, config.cfg.autoConnect);
    } else if (strcmp(sel, "Reboot") == 0) {
      display.showOverlay("Rebooting...");
      delay(500);
      ESP.restart();
    } else if (strcmp(sel, "<- exit") == 0) {
      menu.close();
    }
    break;
  }

  case MenuScreen::SETTINGS: {
    const char *sel = menu.selectedLabel();
    if (strcmp(sel, "Menu Timeout") == 0) {
      menu.showMenuTimeoutSettings(config.cfg.menuTimeout);
    } else if (strcmp(sel, "Screen Timeout") == 0) {
      menu.showScreenTimeoutSettings(config.cfg.screenTimeout);
    } else if (strcmp(sel, "Progress Bar") == 0) {
      menu.showBarModeSettings(config.cfg.barMode);
    } else if (strncmp(sel, "Scan on boot", 12) == 0) {
      config.cfg.scanOnBoot = !config.cfg.scanOnBoot;
      config.save();
      display.showOverlay(config.cfg.scanOnBoot ? "Scan: ON" : "Scan: OFF");
      menu.showSettingsMenu(config.cfg.scanOnBoot, config.cfg.barMode, config.cfg.autoConnect);
    } else if (strncmp(sel, "Auto-connect", 12) == 0) {
      config.cfg.autoConnect = !config.cfg.autoConnect;
      config.save();
      display.showOverlay(config.cfg.autoConnect ? "AutoConn: ON" : "AutoConn: OFF");
      menu.showSettingsMenu(config.cfg.scanOnBoot, config.cfg.barMode, config.cfg.autoConnect);
    } else if (strcmp(sel, "<- back") == 0) {
      menu.open(cast.isConnected(), cast.deviceCount > 0);
    }
    break;
  }

  case MenuScreen::MENU_TIMEOUT: {
    const char *sel = menu.selectedLabel();
    int val = -1;
    if (strcmp(sel, "5s") == 0)
      val = 5;
    else if (strcmp(sel, "15s") == 0)
      val = 15;
    else if (strcmp(sel, "30s") == 0)
      val = 30;
    else if (strcmp(sel, "60s") == 0)
      val = 60;
    if (val > 0) {
      config.cfg.menuTimeout = val;
      config.save();
      char msg[16];
      snprintf(msg, sizeof(msg), "Menu: %ds", val);
      display.showOverlay(msg);
      menu.showSettingsMenu(config.cfg.scanOnBoot, config.cfg.barMode, config.cfg.autoConnect);
    } else if (strcmp(sel, "<- back") == 0) {
      menu.showSettingsMenu(config.cfg.scanOnBoot, config.cfg.barMode, config.cfg.autoConnect);
    }
    break;
  }

  case MenuScreen::SCREEN_TIMEOUT: {
    const char *sel = menu.selectedLabel();
    int val = -1;
    if (strcmp(sel, "30s") == 0)
      val = 30;
    else if (strcmp(sel, "1m") == 0)
      val = 60;
    else if (strcmp(sel, "5m") == 0)
      val = 300;
    else if (strcmp(sel, "10m") == 0)
      val = 600;
    else if (strcmp(sel, "Never") == 0)
      val = 0;
    if (val >= 0) {
      config.cfg.screenTimeout = val;
      config.save();
      display.showOverlay(val == 0 ? "Screen: Never" : "Screen saved");
      menu.showSettingsMenu(config.cfg.scanOnBoot, config.cfg.barMode, config.cfg.autoConnect);
    } else if (strcmp(sel, "<- back") == 0) {
      menu.showSettingsMenu(config.cfg.scanOnBoot, config.cfg.barMode, config.cfg.autoConnect);
    }
    break;
  }

  case MenuScreen::BAR_MODE: {
    const char *sel = menu.selectedLabel();
    int val = -1;
    if (strncmp(sel, "Volume", 6) == 0)
      val = 0;
    else if (strncmp(sel, "Elapsed time", 12) == 0)
      val = 1;
    if (val >= 0) {
      config.cfg.barMode = val;
      config.save();
      display.showOverlay(val == 0 ? "Bar: Volume" : "Bar: Elapsed");
      menu.showSettingsMenu(config.cfg.scanOnBoot, config.cfg.barMode, config.cfg.autoConnect);
    } else if (strcmp(sel, "<- back") == 0) {
      menu.showSettingsMenu(config.cfg.scanOnBoot, config.cfg.barMode, config.cfg.autoConnect);
    }
    break;
  }

  case MenuScreen::INFO: {
    const char *sel = menu.selectedLabel();
    if (strcmp(sel, "<- back") == 0) {
      menu.open(cast.isConnected(), cast.deviceCount > 0);
    }
    break;
  }

  case MenuScreen::DEVICE_LIST: {
    const char *sel = menu.selectedLabel();
    if (strcmp(sel, "Scan network") == 0) {
      menuDoScan();
    } else if (strcmp(sel, "<- back") == 0) {
      menu.open(cast.isConnected(), cast.deviceCount > 0);
    } else {
      // Device items start at the top (index 0)
      int devIdx = menu.cursor;
      if (devIdx >= 0 && devIdx < cast.deviceCount) {
        // Determine if this device is the one we're currently connected to
        bool isTcpUp = cast.isConnected();
        const char *curHost = cast.getHost();

        dbgLog.log("[Menu] Sel devIdx=%d ip=%s isTcp=%d curHost=%s\n", devIdx,
                   cast.devices[devIdx].ip, isTcpUp,
                   curHost ? curHost : "none");

        bool hostMatches = (curHost && curHost[0] != '\0' &&
                            strcmp(cast.devices[devIdx].ip, curHost) == 0);

        // We only skip the "Connect" button if TCP is alive AND the IP matches
        // perfectly
        bool devConnected = isTcpUp && hostMatches;

        menu.showDeviceActions(devIdx, cast.devices[devIdx].name, devConnected,
                               devConnected && cast.isPlaying());
      }
    }
    break;
  }

  case MenuScreen::DEVICE_ACTIONS: {
    int idx = menu.selectedDevice;
    const char *sel = menu.selectedLabel();
    if (strcmp(sel, "Connect") == 0) {
      if (idx >= 0 && idx < cast.deviceCount)
        menuDoConnect(idx);
    } else if (strcmp(sel, "Disconnect") == 0) {
      cast.disconnect();
      display.showOverlay("Disconnected");
      dbgLog.log("[Menu] Disconnected");
      menu.open(false, cast.deviceCount > 0);
    } else if (strcmp(sel, "Play") == 0 || strcmp(sel, "Pause") == 0) {
      bool isPauseReq = (strcmp(sel, "Pause") == 0);
      if (isPauseReq ? cast.pause() : cast.play()) {
        // Optimistic UI: flip for instant feedback
        bool newPlaying = !isPauseReq;
        display.showOverlay(newPlaying ? "PLAYING" : "PAUSED");
        menu.updateActionState(cast.getFriendlyName(), true, newPlaying);
      } else {
        display.showOverlay("Failed");
      }
    } else if (strcmp(sel, "Next") == 0) {
      display.showOverlay(cast.next() ? "NEXT" : "Failed");
    } else if (strcmp(sel, "Previous") == 0) {
      display.showOverlay(cast.previous() ? "PREVIOUS" : "Failed");
    } else if (strcmp(sel, "Connection info") == 0) {
      static char ipRow[32], ssidRow[32], devRow[64], appRow[64], verRow[32];
      snprintf(ipRow, sizeof(ipRow), "IP: %s",
               WiFi.localIP().toString().c_str());
      snprintf(ssidRow, sizeof(ssidRow), "SSID: %s", WiFi.SSID().c_str());
      snprintf(devRow, sizeof(devRow), "Dev: %s", cast.getFriendlyName());
      snprintf(appRow, sizeof(appRow), "App: %s", cast.getAppName());
      snprintf(verRow, sizeof(verRow), "Ver: 1.0.6");
      const char *rows[] = {ipRow, ssidRow, devRow, appRow, verRow};
      menu.showInfo(rows, 5);
    } else if (strcmp(sel, "<- back") == 0) {
      if (idx >= 0)
        showDeviceListMenu();
      else
        menu.open(cast.isConnected(), cast.deviceCount > 0);
    }
    break;
  }

  default:
    break;
  }
}

// ── Handle input when HUD is showing (menu closed) ──────────────────────────
static void handleHUDInput() {
  EncEvent ev = encoder.poll();
  if (backButtonPressed()) {
    resetActivity();
    config.cfg.barMode = (config.cfg.barMode == 0) ? 1 : 0;
    config.save();
    display.showOverlay(config.cfg.barMode == 0 ? "Bar: Volume" : "Bar: Elapsed");
  }

  if (ev == EncEvent::NONE)
    return;

  switch (ev) {
  case EncEvent::CW:
  case EncEvent::CCW: {
    if (!cast.isConnected() || !cast.isVolumeSynced())
      break;
    float step = config.cfg.volumeStep * encoder.delta();
    // Use pending volume as base if we haven't flushed yet
    float base = (pendingVolume >= 0.0f) ? pendingVolume : cast.getVolume();
    float newVol = base + step;
    if (newVol < 0.0f)
      newVol = 0.0f;
    if (newVol > 1.0f)
      newVol = 1.0f;
    pendingVolume = newVol;
    // Send immediately if enough time has passed, otherwise defer
    if (millis() - lastVolSendMs >= VOL_SEND_INTERVAL_MS) {
      cast.setVolume(newVol);
      dbgLog.log("[Remote] Volume → %.0f%%\n", newVol * 100.0f);
      pendingVolume = -1.0f;
      lastVolSendMs = millis();
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "VOL %d%%", (int)(newVol * 100.0f + 0.5f));
    display.showOverlay(buf);
    break;
  }

  case EncEvent::PRESS:
    resetActivity();
    menu.open(cast.isConnected(), cast.deviceCount > 0);
    break;

  case EncEvent::LONG_PRESS:
    resetActivity();
    if (cast.isConnected()) {
      bool newMute = !cast.isMuted();
      dbgLog.log("[Remote] → MUTE %s\n", newMute ? "ON" : "OFF");
      cast.setMute(newMute);
      display.showOverlay(newMute ? "MUTED" : "UNMUTED");
    }
    break;

  default:
    break;
  }
}

// ── Display update ───────────────────────────────────────────────────────────
static void updateDisplay() {
  if (menu.isOpen()) {
    if (menu.screen == MenuScreen::INFO) {
      char rIP[24], rWifi[24], rCast[24], rVol[24], rState[24], rApp[24];
      snprintf(rIP, sizeof(rIP), "IP: %s", WiFi.localIP().toString().c_str());
      snprintf(rWifi, sizeof(rWifi), "WiFi: %s", config.cfg.wifiSsid);
      const char *ch = cast.isConnected() ? cast.getHost() : "none";
      snprintf(rCast, sizeof(rCast), "Cast: %s", ch);
      snprintf(rVol, sizeof(rVol), "Vol: %d%%%s",
               (int)(cast.getVolume() * 100.0f + 0.5f),
               cast.isMuted() ? " MUTE" : "");
      snprintf(rState, sizeof(rState), "State: %s",
               !cast.isConnected() ? "N/A"
               : cast.isPlaying()  ? "Playing"
                                   : "Idle");
      const char *an = cast.getAppName();
      display.drawMenu(menu);
    } else {
      display.drawMenu(menu);
    }
  } else {
    const char *devName =
        cast.getFriendlyName()[0] ? cast.getFriendlyName() : nullptr;
    const char *appName = cast.getAppName();
    display.drawHUD(devName, appName, cast.getVolume(), cast.isMuted(),
                    cast.isPlaying(), cast.isConnected(),
                    config.cfg.barMode, cast.getCurrentTime(),
                    cast.getDuration());
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  dbgLog.log("\n=== KnobCast ===\n");

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_BTN, INPUT_PULLUP);

  config.begin();
  display.begin();
  encoder.begin();

  display.showSplash("Starting...");
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  serviceAlways();

  switch (state) {

  case AppState::INIT:
    if (!config.cfg.configured || config.cfg.wifiSsid[0] == '\0') {
      state = AppState::AP_MODE;
    } else {
      state = AppState::WIFI_CONNECTING;
    }
    stateEnteredMs = millis();
    break;

  case AppState::AP_MODE:
    if (!webServerStarted) {
      webServer.startAP();
      webServer.begin(&config, &cast);
      webServerStarted = true;
      display.showAPMode(webServer.getAPSsid(),
                         webServer.getAPIP().toString().c_str());
      dbgLog.log("[App] AP mode active");
    }
    break;

  case AppState::WIFI_CONNECTING: {
    static bool wifiBegun = false;
    if (!wifiBegun) {
      dbgLog.log("[WiFi] Connecting to %s …\n", config.cfg.wifiSsid);
      display.showConnecting(config.cfg.wifiSsid);
      WiFi.mode(WIFI_STA);
      WiFi.begin(config.cfg.wifiSsid, config.cfg.wifiPass);
      wifiBegun = true;
      stateEnteredMs = millis();
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiBegun = false;
      dbgLog.log("\n[WiFi] Connected – IP: %s\n",
                 WiFi.localIP().toString().c_str());
      display.showSplash(WiFi.localIP().toString().c_str());
      state = AppState::WIFI_CONNECTED;
      stateEnteredMs = millis();
    } else if (millis() - stateEnteredMs > 20000) {
      wifiBegun = false;
      dbgLog.log("[WiFi] Timeout — falling back to AP mode");
      display.showOverlay("WiFi fail", "AP mode...");
      state = AppState::AP_MODE;
      stateEnteredMs = millis();
    }
    break;
  }

  case AppState::WIFI_CONNECTED:
    if (!webServerStarted) {
      MDNS.begin("knobcast");
      MDNS.addService("http", "tcp", 80);
      dbgLog.log("[mDNS] Registered knobcast.local\n");
      webServer.begin(&config, &cast);
      webServerStarted = true;
    }
    if (millis() - stateEnteredMs > 1500) {
      if (config.cfg.scanOnBoot) {
        display.showConnecting("Scanning...");
        cast.discoverAll(5000);
        dbgLog.log("[App] Boot scan found %d device(s)\n", cast.deviceCount);
      }

      // Auto-connect to last device if enabled and we have a saved IP
      if (config.cfg.autoConnect && config.cfg.lastDeviceIp[0] != '\0') {
        // Check if the device was found in scan results (or skip check if scan disabled)
        bool found = !config.cfg.scanOnBoot; // if no scan, just try directly
        for (int i = 0; i < cast.deviceCount; i++) {
          if (strcmp(cast.devices[i].ip, config.cfg.lastDeviceIp) == 0) {
            found = true;
            break;
          }
        }
        if (found) {
          dbgLog.log("[App] Auto-connecting to %s (%s:%u)\n",
                     config.cfg.lastDeviceName, config.cfg.lastDeviceIp,
                     config.cfg.lastDevicePort);
          display.showConnecting(config.cfg.lastDeviceName);
          if (cast.connect(config.cfg.lastDeviceIp, config.cfg.lastDevicePort)) {
            cast.setFriendlyName(config.cfg.lastDeviceName);
            dbgLog.log("[App] Auto-connected to %s\n", config.cfg.lastDeviceName);
          } else {
            dbgLog.log("[App] Auto-connect failed\n");
          }
        } else {
          dbgLog.log("[App] Last device %s not found in scan\n",
                     config.cfg.lastDeviceName);
        }
      }

      state = AppState::READY;
      stateEnteredMs = millis();
    }
    break;

  // ── READY: HUD + menu, no auto-connect ───────────────────────────────
  case AppState::READY:
    // Always run background tasks if connected
    cast.loop();

    if (menu.isOpen()) {
      handleMenuInput();

      // ── Menu inactivity timeout ─────────────────────────────────────
      if (config.cfg.menuTimeout > 0 &&
          millis() - lastActivityMs >
              (uint32_t)config.cfg.menuTimeout * 1000UL) {
        menu.close();
      }

      // Periodic menu sync (if Actions sub-menu is open)
      static uint32_t lastMenuSync = 0;
      if (menu.screen == MenuScreen::DEVICE_ACTIONS &&
          millis() - lastMenuSync > 1000) {
        bool isConnected = false;
        int idx = menu.selectedDevice;
        if (idx >= 0 && idx < cast.deviceCount) {
          const char *curHost = cast.getHost();
          bool hostMatches = (curHost && curHost[0] != '\0' &&
                              strcmp(cast.devices[idx].ip, curHost) == 0);
          isConnected = cast.isConnected() && hostMatches;
        } else if (idx == -1) {
          isConnected = cast.isConnected();
        }
        menu.updateActionState(cast.getFriendlyName(), isConnected,
                               isConnected && cast.isPlaying());
        lastMenuSync = millis();
      }
    } else {
      handleHUDInput();
    }

    // ── Screen inactivity timeout ─────────────────────────────────────
    if (displayOn && config.cfg.screenTimeout > 0 &&
        millis() - lastActivityMs >
            (uint32_t)config.cfg.screenTimeout * 1000UL) {
      display.setPower(false);
      displayOn = false;
    }

    updateDisplay();
    digitalWrite(PIN_LED, cast.isConnected() ? HIGH : LOW);
    break;
  }
}
