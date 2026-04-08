#pragma once
// chromecast_client.h
// Google Cast (CASTV2) sender — passive / non-invasive mode.
// Connects to an existing session (e.g. Spotify) without launching a new app.
//
// Protocol summary:
//   1. mDNS discovery  → _googlecast._tcp, port 8009
//   2. TLS TCP socket  (self-signed cert → InsecureSkipVerify)
//   3. Messages        = 4-byte BE length prefix + protobuf CastMessage
//   4. Channels:
//        urn:x-cast:com.google.cast.tp.connection  (CONNECT / CLOSE)
//        urn:x-cast:com.google.cast.tp.heartbeat   (PING / PONG)
//        urn:x-cast:com.google.cast.receiver       (GET_STATUS / SET_VOLUME)
//        urn:x-cast:com.google.cast.media          (PLAY / PAUSE / STOP / SEEK)

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "cast_message.h"

// ── Namespaces ───────────────────────────────────────────────────────────────
#define NS_CONNECTION  "urn:x-cast:com.google.cast.tp.connection"
#define NS_HEARTBEAT   "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_RECEIVER    "urn:x-cast:com.google.cast.receiver"
#define NS_MEDIA       "urn:x-cast:com.google.cast.media"

#define CAST_PORT       8009
#define SOURCE_ID       "sender-esp32"
#define PLATFORM_DEST   "receiver-0"
#define MSG_BUF_SIZE    2048
#define RX_BUF_SIZE     12288
#define MAX_CAST_DEVICES 8

// ── Discovered device info ───────────────────────────────────────────────────
struct CastDevice {
    char name[64];
    char ip[40];
    uint16_t port;
    bool isGroup;
};

class ChromecastClient {
public:
    ChromecastClient();

    // ── Discovery ──────────────────────────────────────────────────────────
    // Scans for all Chromecasts on the network. Returns count found.
    // Results stored in devices[], up to MAX_CAST_DEVICES.
    int discoverAll(uint32_t timeoutMs = 8000);

    // Simple single-device discover (fills _host/_port with first result)
    bool discover(uint32_t timeoutMs = 5000);

    CastDevice devices[MAX_CAST_DEVICES];
    int deviceCount = 0;

    // ── Connection ─────────────────────────────────────────────────────────
    bool connect(const char* host = nullptr, uint16_t port = CAST_PORT);
    void disconnect();
    bool isConnected();

    // Probe a device: connect, wait for status, return true if an app is active.
    // Disconnects automatically if no active session found.
    bool probeForActiveSession(const char* host, uint16_t port = CAST_PORT,
                               uint32_t waitMs = 3000);

    // ── Must be called in loop() ───────────────────────────────────────────
    void loop();

    // ── Volume & mute (receiver-level, works with any app) ─────────────────
    bool setVolume(float level);   // 0.0 – 1.0
    bool setMute(bool muted);

    // ── Media controls (sent to existing session, never launches) ──────────
    bool play();
    bool pause();
    bool stop();
    bool next();
    bool previous();
    bool seek(float positionSec);

    // ── State accessors ────────────────────────────────────────────────────
    float    getVolume()        const { return _volume; }
    bool     isVolumeSynced()   const { return _volumeSynced; }
    bool     isMuted()          const { return _muted; }
    bool     isPlaying()        const { return _playing; }
    bool     hasSession()       const { return _sessionId[0] != '\0'; }
    float    getCurrentTime()   const { return _currentTime; }
    float    getDuration()      const { return _duration; }
    const char* getStatusText() const { return _statusText; }
    const char* getAppName()      const { return _appName; }
    const char* getHost()         const { return _host; }
    const char* getFriendlyName() const { return _friendlyName; }
    uint16_t getPort() const { return _port; }
    int      getConnectedDeviceIdx() const {
        for (int i = 0; i < deviceCount; i++) {
            if (strcmp(devices[i].ip, _host) == 0 && devices[i].port == _port) return i;
        }
        return -1;
    }
    void setFriendlyName(const char* name) {
        strncpy(_friendlyName, name, sizeof(_friendlyName) - 1);
        _friendlyName[sizeof(_friendlyName) - 1] = '\0';
    }

private:
    WiFiClientSecure _tls;
    char   _host[64];
    uint16_t _port;

    // Session state (read from existing session, never launched by us)
    char   _sessionId[64];
    char   _appName[64];
    char   _friendlyName[64];  // mDNS friendly name of the device
    int    _requestId;
    float  _volume;
    bool   _volumeSynced;
    bool   _muted;
    bool   _playing;
    float  _currentTime;
    float  _duration;
    char   _statusText[128];
    int    _mediaSessionId;
    bool   _mediaConnected;    // true once we've sent CONNECT to the transport

    // Buffers
    uint8_t _txBuf[MSG_BUF_SIZE];
    uint8_t _rxBuf[RX_BUF_SIZE];
    char    _payloadBuf[RX_BUF_SIZE]; // Large buffer off the stack for parsed payloads
    size_t  _rxLen;

    // Timing
    uint32_t _lastPingMs;
    uint32_t _lastMediaPollMs;

    // ── Low-level send ─────────────────────────────────────────────────────
    bool sendRaw(const char* src, const char* dst,
                 const char* ns, const char* payload);

    // ── Platform-level helpers ─────────────────────────────────────────────
    bool sendConnect(const char* dst = PLATFORM_DEST);
    bool sendPing();
    bool sendPong();

    // ── Connect to the media transport (existing session) ──────────────────
    bool ensureMediaConnection();
    bool _sendMediaCmd(const char* type, JsonDocument& doc);

    // ── Receive / parse ────────────────────────────────────────────────────
    void readMessages();
    void handleMessage(const char* ns, const char* src, const char* dst,
                       const char* payload);
    void handleReceiverStatus(JsonObject& status);
    void handleMediaStatus(JsonArray& arr);

    int nextRequestId() { return ++_requestId; }
};
