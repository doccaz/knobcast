#pragma once
// config.h
// NVS-backed persistent configuration for KnobCast.
// Stores WiFi credentials, Chromecast target, and preferences.

#include <Arduino.h>
#include <Preferences.h>

struct Config {
    char     wifiSsid[64];
    char     wifiPass[64];
    char     castIp[40];       // empty = use mDNS discovery
    float    volumeStep;       // 0.0–1.0, default 0.02 (2%)
    int      menuTimeout;      // menu timeout in seconds
    int      screenTimeout;    // screen timeout in seconds
    bool     scanOnBoot;       // scan for devices on boot
    int      barMode;          // 0 = volume, 1 = elapsed time
    bool     autoConnect;      // auto-connect to last device on boot
    char     lastDeviceIp[40]; // IP of last connected device
    char     lastDeviceName[64]; // friendly name of last connected device
    uint16_t lastDevicePort;   // port of last connected device
    bool     configured;       // true once WiFi has been saved at least once

    void setDefaults() {
        wifiSsid[0] = '\0';
        wifiPass[0] = '\0';
        castIp[0]   = '\0';
        volumeStep  = 0.02f;
        menuTimeout = 15;
        screenTimeout = 600;
        scanOnBoot  = true;
        barMode     = 0;
        autoConnect = true;
        lastDeviceIp[0]   = '\0';
        lastDeviceName[0] = '\0';
        lastDevicePort    = 8009;
        configured  = false;
    }
};

class ConfigStore {
public:
    Config cfg;

    void begin() {
        cfg.setDefaults();
        _prefs.begin("knobcast", false);
        load();
    }

    void load() {
        cfg.configured = _prefs.getBool("configured", false);
        _prefs.getString("wifiSsid", cfg.wifiSsid, sizeof(cfg.wifiSsid));
        _prefs.getString("wifiPass", cfg.wifiPass, sizeof(cfg.wifiPass));
        _prefs.getString("castIp",   cfg.castIp,   sizeof(cfg.castIp));
        cfg.volumeStep = _prefs.getFloat("volStep", 0.02f);
        cfg.menuTimeout = _prefs.getInt("menuTimeout", 15);
        cfg.screenTimeout = _prefs.getInt("screenTimeout", 600);
        cfg.scanOnBoot = _prefs.getBool("scanOnBoot", true);
        cfg.barMode = _prefs.getInt("barMode", 0);
        cfg.autoConnect = _prefs.getBool("autoConnect", true);
        _prefs.getString("lastDevIp", cfg.lastDeviceIp, sizeof(cfg.lastDeviceIp));
        _prefs.getString("lastDevName", cfg.lastDeviceName, sizeof(cfg.lastDeviceName));
        cfg.lastDevicePort = _prefs.getUShort("lastDevPort", 8009);
    }

    void save() {
        _prefs.putBool("configured", cfg.configured);
        _prefs.putString("wifiSsid", cfg.wifiSsid);
        _prefs.putString("wifiPass", cfg.wifiPass);
        _prefs.putString("castIp",   cfg.castIp);
        _prefs.putFloat("volStep",   cfg.volumeStep);
        _prefs.putInt("menuTimeout", cfg.menuTimeout);
        _prefs.putInt("screenTimeout", cfg.screenTimeout);
        _prefs.putBool("scanOnBoot", cfg.scanOnBoot);
        _prefs.putInt("barMode", cfg.barMode);
        _prefs.putBool("autoConnect", cfg.autoConnect);
        _prefs.putString("lastDevIp", cfg.lastDeviceIp);
        _prefs.putString("lastDevName", cfg.lastDeviceName);
        _prefs.putUShort("lastDevPort", cfg.lastDevicePort);
    }

    void saveLastDevice(const char* ip, const char* name, uint16_t port) {
        strncpy(cfg.lastDeviceIp, ip, sizeof(cfg.lastDeviceIp) - 1);
        cfg.lastDeviceIp[sizeof(cfg.lastDeviceIp) - 1] = '\0';
        strncpy(cfg.lastDeviceName, name, sizeof(cfg.lastDeviceName) - 1);
        cfg.lastDeviceName[sizeof(cfg.lastDeviceName) - 1] = '\0';
        cfg.lastDevicePort = port;
        _prefs.putString("lastDevIp", cfg.lastDeviceIp);
        _prefs.putString("lastDevName", cfg.lastDeviceName);
        _prefs.putUShort("lastDevPort", cfg.lastDevicePort);
    }

    void reset() {
        cfg.setDefaults();
        _prefs.clear();
    }

private:
    Preferences _prefs;
};
