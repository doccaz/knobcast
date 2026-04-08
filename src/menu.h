#pragma once
// menu.h
// On-screen menu system for KnobCast.
// Activated by encoder press from HUD. GPIO 9 = back.
// Encoder rotation = navigate, encoder press = select.

#include <Arduino.h>

static constexpr int MENU_MAX_ITEMS = 12;

enum class MenuScreen {
    CLOSED,         // HUD mode — menu not visible
    MAIN,           // Main menu
    DEVICE_LIST,    // List of discovered Chromecast devices
    DEVICE_ACTIONS, // Sub-menu for selected device (connect, disconnect, info)
    INFO,           // Connection info (IP, SSID)
    SCANNING,       // mDNS scan in progress (transient)
    CONNECTING,     // Connecting to selected device (transient)
    SETTINGS,       // Settings main menu
    MENU_TIMEOUT,   // Menu timeout selection
    SCREEN_TIMEOUT, // Screen timeout selection
    BAR_MODE,       // Progress bar mode selection
};

struct MenuItem {
    char label[64];
};

class Menu {
public:
    MenuScreen screen       = MenuScreen::CLOSED;
    int        cursor       = 0;
    int        count        = 0;
    int        selectedDevice = -1;  // device index for DEVICE_ACTIONS screen
    MenuItem   items[MENU_MAX_ITEMS];

    bool isOpen() const { return screen != MenuScreen::CLOSED; }

    void open(bool connected, bool hasDevices) {
        screen = MenuScreen::MAIN;
        cursor = 0;
        count  = 0;

        if (connected) {
            _setItem(count++, "Actions");
            _setItem(count++, "Connection info");
            _setItem(count++, "Disconnect");
        } else {
            if (hasDevices) {
                _setItem(count++, "Devices");
            }
            _setItem(count++, "Scan network");
        }
        _setItem(count++, "About");
        _setItem(count++, "Settings");
        _setItem(count++, "Reboot");
        _setItem(count++, "<- exit");
    }

    void close() {
        screen = MenuScreen::CLOSED;
        cursor = 0;
        count  = 0;
    }

    // Navigate: delta from encoder (+1 = CW, -1 = CCW)
    void navigate(int delta) {
        if (count == 0) return;
        cursor += delta;
        if (cursor < 0) cursor = 0;
        if (cursor >= count) cursor = count - 1;
    }

    // Populate device list from discovery results.
    // First item is always "Scan network"; devices follow at index 1+.
    void showDeviceList(const char* names[], const int n) {
        screen = MenuScreen::DEVICE_LIST;
        cursor = 0;
        count  = 0;
        
        // List of discovered devices first
        int maxDev = MENU_MAX_ITEMS - 2; // leave room for Scan network and back
        int devN = (n > maxDev) ? maxDev : n;
        for (int i = 0; i < devN; i++) {
            _setItem(count++, names[i]);
        }
        if (devN == 0) {
            _setItem(count++, "(no devices found)");
        }

        _setItem(count++, "Scan network");
        _setItem(count++, "<- back");
    }

    void showDeviceActions(int deviceIdx, const char* deviceName,
                           bool isConnected, bool isPlaying = false) {
        screen = MenuScreen::DEVICE_ACTIONS;
        selectedDevice = deviceIdx;
        cursor = 0;
        count  = 0;
        if (isConnected) {
            _setItem(count++, isPlaying ? "Pause" : "Play");
            _setItem(count++, "Previous");
            _setItem(count++, "Next");
            _setItem(count++, "Connection info");
            _setItem(count++, "Disconnect");
        } else {
            _setItem(count++, "Connect");
            _setItem(count++, "Device info");
        }
        _setItem(count++, "<- back");
    }

    void updateActionState(const char* deviceName, bool isConnected, bool isPlaying) {
        if (screen != MenuScreen::DEVICE_ACTIONS) return;
        int oldCursor = cursor;
        showDeviceActions(selectedDevice, deviceName, isConnected, isPlaying);
        cursor = oldCursor;
        if (cursor >= count) cursor = count - 1;
    }

    void showInfo(const char* rows[], int n) {
        screen = MenuScreen::INFO;
        cursor = 0;
        count  = 0;
        int maxRows = MENU_MAX_ITEMS - 1;
        int rowN = (n > maxRows) ? maxRows : n;
        for (int i = 0; i < rowN; i++) {
            _setItem(count++, rows[i]);
        }
        _setItem(count++, "<- back");
    }

    void showScanning() {
        screen = MenuScreen::SCANNING;
        cursor = 0;
        count  = 0;
    }

    void showConnecting(const char* name) {
        screen = MenuScreen::CONNECTING;
        cursor = 0;
        count  = 0;
        _setItem(0, name);
    }

    void showSettingsMenu(bool scanOnBoot = true, int barMode = 0,
                          bool autoConnect = true) {
        screen = MenuScreen::SETTINGS;
        cursor = 0;
        count  = 0;
        _setItem(count++, "Menu Timeout");
        _setItem(count++, "Screen Timeout");
        _setItem(count++, "Progress Bar");
        _setItemMark(count++, "Scan on boot", scanOnBoot);
        _setItemMark(count++, "Auto-connect", autoConnect);
        _setItem(count++, "<- back");
    }

    void showBarModeSettings(int currentMode = 0) {
        screen = MenuScreen::BAR_MODE;
        cursor = 0;
        count  = 0;
        _setItemMark(count++, "Volume", currentMode == 0);
        _setItemMark(count++, "Elapsed time", currentMode == 1);
        _setItem(count++, "<- back");
    }

    void showMenuTimeoutSettings(int currentSecs = -1) {
        screen = MenuScreen::MENU_TIMEOUT;
        cursor = 0;
        count  = 0;
        _setItemMark(count++, "5s",  currentSecs ==  5);
        _setItemMark(count++, "15s", currentSecs == 15);
        _setItemMark(count++, "30s", currentSecs == 30);
        _setItemMark(count++, "60s", currentSecs == 60);
        _setItem(count++, "<- back");
    }

    void showScreenTimeoutSettings(int currentSecs = -1) {
        screen = MenuScreen::SCREEN_TIMEOUT;
        cursor = 0;
        count  = 0;
        _setItemMark(count++, "30s",   currentSecs ==  30);
        _setItemMark(count++, "1m",    currentSecs ==  60);
        _setItemMark(count++, "5m",    currentSecs == 300);
        _setItemMark(count++, "10m",   currentSecs == 600);
        _setItemMark(count++, "Never", currentSecs ==   0);
        _setItem(count++, "<- back");
    }

    const char* selectedLabel() const {
        if (cursor >= 0 && cursor < count) return items[cursor].label;
        return "";
    }

private:
    void _setItem(int idx, const char* label) {
        strncpy(items[idx].label, label, sizeof(items[idx].label) - 1);
        items[idx].label[sizeof(items[idx].label) - 1] = '\0';
    }

    void _setItemMark(int idx, const char* label, bool active) {
        if (active) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s *", label);
            _setItem(idx, buf);
        } else {
            _setItem(idx, label);
        }
    }
};
