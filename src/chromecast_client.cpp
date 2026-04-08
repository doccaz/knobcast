// chromecast_client.cpp
// Passive Cast client — attaches to existing sessions, never launches apps.
#include "chromecast_client.h"
#include "debug_log.h"

// ─── Protobuf decode helpers ─────────────────────────────────────────────────
static uint64_t readVarint(const uint8_t *buf, size_t len, size_t &pos) {
  uint64_t result = 0;
  int shift = 0;
  while (pos < len) {
    uint8_t b = buf[pos++];
    result |= (uint64_t)(b & 0x7F) << shift;
    if (!(b & 0x80))
      break;
    shift += 7;
  }
  return result;
}

static bool parseCastMessage(const uint8_t *buf, size_t len, char *nsOut,
                             size_t nsMax, char *srcOut, size_t srcMax,
                             char *dstOut, size_t dstMax, char *payOut,
                             size_t payMax) {
  nsOut[0] = srcOut[0] = dstOut[0] = payOut[0] = '\0';
  size_t pos = 0;

  auto readLenDelim = [&](char *out, size_t outMax) {
    size_t slen = (size_t)readVarint(buf, len, pos);
    size_t copy = (slen < outMax - 1) ? slen : outMax - 1;
    memcpy(out, buf + pos, copy);
    out[copy] = '\0';
    pos += slen;
  };

  while (pos < len) {
    uint64_t tag = readVarint(buf, len, pos);
    int fieldNum = (int)(tag >> 3);
    int wireType = (int)(tag & 7);

    if (wireType == 0) {
      readVarint(buf, len, pos);
    } else if (wireType == 2) {
      switch (fieldNum) {
      case 2:
        readLenDelim(srcOut, srcMax);
        break;
      case 3:
        readLenDelim(dstOut, dstMax);
        break;
      case 4:
        readLenDelim(nsOut, nsMax);
        break;
      case 6:
        readLenDelim(payOut, payMax);
        break;
      default: {
        size_t skip = (size_t)readVarint(buf, len, pos);
        pos += skip;
        break;
      }
      }
    } else if (wireType == 1) {
      pos += 8;
    } else if (wireType == 5) {
      pos += 4;
    } else
      break;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────

ChromecastClient::ChromecastClient()
    : _port(CAST_PORT), _requestId(0), _volume(0.5f), _volumeSynced(false),
      _muted(false), _playing(false), _currentTime(0), _duration(0),
      _mediaSessionId(-1), _mediaConnected(false), _rxLen(0), _lastPingMs(0),
      _lastMediaPollMs(0) {
  _host[0] = '\0';
  _sessionId[0] = '\0';
  _appName[0] = '\0';
  _friendlyName[0] = '\0';
  _statusText[0] = '\0';
}

// ─── Discovery
// ────────────────────────────────────────────────────────────────
int ChromecastClient::discoverAll(uint32_t timeoutMs) {
  dbgLog.log("[Cast] Scanning for Chromecast devices…");

  deviceCount = 0;

  // The ESP32 mDNS library can miss multicast responses and returns
  // duplicate entries (IPv4+IPv6, multiple interfaces).  We do multiple
  // rounds, re-init mDNS each time, and deduplicate by friendly name
  // (not just IP) to handle all Cast device types including groups and
  // Chromecast Audio.
  uint32_t start = millis();
  int rounds = 0;
  while (millis() - start < timeoutMs && deviceCount < MAX_CAST_DEVICES) {
    MDNS.end();
    if (!MDNS.begin("knobcast")) {
      dbgLog.log("[Cast] mDNS init failed");
      break;
    }

    int n = MDNS.queryService("googlecast", "tcp");
    dbgLog.log("[Cast] Round %d: %d raw response(s)\n", rounds + 1, n);

    for (int i = 0; i < n && deviceCount < MAX_CAST_DEVICES; i++) {
      uint16_t port = MDNS.port(i);
      String hostname = MDNS.hostname(i);
      String ip = MDNS.IP(i).toString();
      const char* h = hostname.c_str() ? hostname.c_str() : "???";
      const char* a = ip.c_str() ? ip.c_str() : "0.0.0.0";
      dbgLog.log("[Cast] Record %d: %s (%s:%u)\n", i, h, a, port);

      int numTxt = MDNS.numTxt(i);
      for (int j = 0; j < numTxt; j++) {
        String key = MDNS.txtKey(i, j);
        String val = MDNS.txt(i, j);
        const char* k = key.c_str() ? key.c_str() : "?";
        const char* v = val.c_str() ? val.c_str() : "";
        dbgLog.log("[Cast]   txt[%d] %s = %s\n", j, k, v);
      }

      // Skip invalid / IPv6 responses (ESP32 WiFi is IPv4-only)
      if (ip == "0.0.0.0" || ip.length() == 0 || ip.indexOf(':') >= 0)
        continue;

      String fname = MDNS.txt(i, "fn");
      if (fname.length() == 0)
        fname = MDNS.hostname(i);

      String md = MDNS.txt(i, "md");
      bool isGroup = (md == "Google Cast Group") || (port != 8009);

      // Deduplicate: same name OR (same IP AND same Port)
      bool dup = false;
      for (int j = 0; j < deviceCount; j++) {
        const char* dIP = ip.c_str() ? ip.c_str() : "";
        const char* dName = fname.c_str() ? fname.c_str() : "";
        if (strcmp(devices[j].name, dName) == 0 ||
            (strcmp(devices[j].ip, dIP) == 0 && devices[j].port == port)) {
          dup = true;
          break;
        }
      }
      if (dup)
        continue;

      const char* finalIP = ip.c_str() ? ip.c_str() : "0.0.0.0";
      strncpy(devices[deviceCount].ip, finalIP,
              sizeof(devices[deviceCount].ip) - 1);
      devices[deviceCount].ip[sizeof(devices[deviceCount].ip) - 1] = '\0';
      devices[deviceCount].port = port;
      devices[deviceCount].isGroup = isGroup;
      const char* finalName = fname.c_str() ? fname.c_str() : "unknown";
      strncpy(devices[deviceCount].name, finalName,
              sizeof(devices[deviceCount].name) - 1);
      devices[deviceCount].name[sizeof(devices[deviceCount].name) - 1] = '\0';

      dbgLog.log("[Cast]  + %s (%s:%u) %s\n", devices[deviceCount].name,
                 devices[deviceCount].ip, devices[deviceCount].port,
                 isGroup ? "[Group]" : "");
      deviceCount++;
    }
    rounds++;
    if (millis() - start + 2000 < timeoutMs) {
      delay(2000); // gap between rounds for slow responders
    } else {
      break;
    }
  }

  if (deviceCount == 0) {
    dbgLog.log("[Cast] No devices found");
  } else {
    dbgLog.log("[Cast] Scan done: %d device(s) in %d round(s)\n", deviceCount,
               rounds);
  }
  return deviceCount;
}

bool ChromecastClient::discover(uint32_t timeoutMs) {
  int n = discoverAll(timeoutMs);
  if (n == 0)
    return false;
  strncpy(_host, devices[0].ip, sizeof(_host) - 1);
  _host[sizeof(_host) - 1] = '\0';
  _port = devices[0].port;
  return true;
}

// ─── TLS Connection
// ───────────────────────────────────────────────────────────
bool ChromecastClient::connect(const char *host, uint16_t port) {
  const char *h = host ? host : _host;
  if (h[0] == '\0') {
    dbgLog.log("[Cast] No host – call discover() first or pass IP");
    return false;
  }
  if (host) {
    strncpy(_host, host, sizeof(_host) - 1);
    _host[sizeof(_host) - 1] = '\0';
  }
  _port = port;

  _tls.setInsecure();
  dbgLog.log("[Cast] Connecting to %s:%u …\n", _host, _port);
  if (!_tls.connect(_host, _port)) {
    dbgLog.log("[Cast] TLS connect failed");
    return false;
  }
  dbgLog.log("[Cast] TLS connected");

  _mediaConnected = false;
  _volumeSynced = false;
  _sessionId[0] = '\0';
  _appName[0] = '\0';

  // CONNECT on platform channel
  if (!sendConnect(PLATFORM_DEST))
    return false;

  // GET_STATUS to learn about existing session (passive — no LAUNCH)
  JsonDocument doc;
  doc["type"] = "GET_STATUS";
  doc["requestId"] = nextRequestId();
  String json;
  serializeJson(doc, json);
  sendRaw(SOURCE_ID, PLATFORM_DEST, NS_RECEIVER, json.c_str());

  _lastPingMs = millis();
  return true;
}

void ChromecastClient::disconnect() {
  if (_tls.connected()) {
    JsonDocument doc;
    doc["type"] = "CLOSE";
    String json;
    serializeJson(doc, json);
    if (_mediaConnected && _sessionId[0] != '\0') {
      sendRaw(SOURCE_ID, _sessionId, NS_CONNECTION, json.c_str());
    }
    sendRaw(SOURCE_ID, PLATFORM_DEST, NS_CONNECTION, json.c_str());
    _tls.stop();
  }
  // Always clear metadata so we don't have "phantom" connections
  _mediaConnected = false;
  _mediaSessionId = -1;
  _host[0] = '\0';
  _port = CAST_PORT;
  _sessionId[0] = '\0';
  _appName[0] = '\0';
  _friendlyName[0] = '\0';
  _statusText[0] = '\0';
  _playing = false;
  _volumeSynced = false;
  dbgLog.log("[Cast] Disconnected & state cleared\n");
}

bool ChromecastClient::isConnected() { return _tls.connected(); }

// ─── Probe: connect, check for active session, disconnect if idle ────────────
bool ChromecastClient::probeForActiveSession(const char *host, uint16_t port,
                                             uint32_t waitMs) {
  if (!connect(host, port))
    return false;

  // Poll for RECEIVER_STATUS to arrive
  uint32_t start = millis();
  while (millis() - start < waitMs) {
    readMessages();
    if (hasSession()) {
      dbgLog.log("[Cast] Probe: active session on %s (%s)\n", host, _appName);
      return true;
    }
    delay(100);
  }

  // No active session — disconnect and report
  dbgLog.log("[Cast] Probe: no active session on %s\n", host);
  disconnect();
  return false;
}

// ─── loop() ──────────────────────────────────────────────────────────────────
void ChromecastClient::loop() {
  if (!_tls.connected())
    return;

  uint32_t now = millis();

  if (now - _lastPingMs > 5000) {
    sendPing();
    _lastPingMs = now;
  }

  // Periodically poll media status to keep _playing/_currentTime fresh
  if (_mediaConnected && now - _lastMediaPollMs > 3000) {
    JsonDocument doc;
    doc["type"] = "GET_STATUS";
    doc["requestId"] = nextRequestId();
    String json;
    serializeJson(doc, json);
    sendRaw(SOURCE_ID, _sessionId, NS_MEDIA, json.c_str());
    _lastMediaPollMs = now;
  }

  readMessages();
}

// ─── Low-level send ──────────────────────────────────────────────────────────
bool ChromecastClient::sendRaw(const char *src, const char *dst, const char *ns,
                               const char *payload) {
  if (!_tls.connected())
    return false;

  size_t msgLen = CastProto::encode(_txBuf + 4, sizeof(_txBuf) - 4, src, dst,
                                    ns, false, payload, nullptr, 0);

  _txBuf[0] = (msgLen >> 24) & 0xFF;
  _txBuf[1] = (msgLen >> 16) & 0xFF;
  _txBuf[2] = (msgLen >> 8) & 0xFF;
  _txBuf[3] = (msgLen) & 0xFF;

  size_t written = _tls.write(_txBuf, msgLen + 4);
  return (written == msgLen + 4);
}

bool ChromecastClient::sendConnect(const char *dst) {
  const char *payload =
      "{\"type\":\"CONNECT\",\"origin\":{}"
      ",\"userAgent\":\"ESP32CastClient/1.0\""
      ",\"senderInfo\":{\"sdkType\":2,\"version\":\"15.605.1.3\""
      ",\"browserVersion\":\"44.0.2403.30\",\"platform\":4"
      ",\"connectionType\":1}}";
  return sendRaw(SOURCE_ID, dst, NS_CONNECTION, payload);
}

bool ChromecastClient::sendPing() {
  return sendRaw(SOURCE_ID, PLATFORM_DEST, NS_HEARTBEAT, "{\"type\":\"PING\"}");
}

bool ChromecastClient::sendPong() {
  return sendRaw(SOURCE_ID, PLATFORM_DEST, NS_HEARTBEAT, "{\"type\":\"PONG\"}");
}

// ─── Connect to existing media transport ─────────────────────────────────────
bool ChromecastClient::ensureMediaConnection() {
  if (_sessionId[0] == '\0')
    return false;
  if (_mediaConnected)
    return true;
  _mediaConnected = sendConnect(_sessionId);
  if (_mediaConnected) {
    // Request media status to learn mediaSessionId
    // Give the device a tiny moment to finish the "JOIN" on its end
    delay(50);
    JsonDocument doc;
    doc["type"] = "GET_STATUS";
    doc["requestId"] = nextRequestId();
    String json;
    serializeJson(doc, json);
    sendRaw(SOURCE_ID, _sessionId, NS_MEDIA, json.c_str());
  }
  return _mediaConnected;
}

// ─── Volume & mute (receiver-level — works with any app) ────────────────────
bool ChromecastClient::setVolume(float level) {
  if (level < 0)
    level = 0;
  if (level > 1)
    level = 1;
  JsonDocument doc;
  doc["type"] = "SET_VOLUME";
  doc["volume"]["level"] = level;
  doc["requestId"] = nextRequestId();
  String json;
  serializeJson(doc, json);
  return sendRaw(SOURCE_ID, PLATFORM_DEST, NS_RECEIVER, json.c_str());
}

bool ChromecastClient::setMute(bool muted) {
  JsonDocument doc;
  doc["type"] = "SET_VOLUME";
  doc["volume"]["muted"] = muted;
  doc["requestId"] = nextRequestId();
  String json;
  serializeJson(doc, json);
  return sendRaw(SOURCE_ID, PLATFORM_DEST, NS_RECEIVER, json.c_str());
}

// ─── Media controls (sent to existing transport session) ─────────────────────

// Helper: send a media command, connecting to transport if needed.
// If mediaSessionId is unknown, re-requests media status and retries once.
bool ChromecastClient::_sendMediaCmd(const char *type, JsonDocument &doc) {
  if (!ensureMediaConnection()) {
    dbgLog.log("[Cast] Media cmd '%s' failed: no transport\n", type);
    return false;
  }
  if (_mediaSessionId < 0) {
    // If we don't have a session ID, try one quick status poll first
    dbgLog.log("[Cast] No msId for '%s'; reg & status sync...\n", type);
    
    // Drainage: clear the socket buffer proactively to clear any stale messages
    uint32_t drainStart = millis();
    while (_tls.available() && millis() - drainStart < 200) {
        readMessages();
    }

    // Send a status request to learn the mediaSessionId
    JsonDocument req;
    req["type"] = "GET_STATUS";
    req["requestId"] = nextRequestId();
    String rj;
    serializeJson(req, rj);
    sendRaw(SOURCE_ID, _sessionId, NS_MEDIA, rj.c_str());

    // Wait for the status reply with the mediaSessionId
    uint32_t start = millis();
    // 2.5s timeout for Spotify/Large apps to respond
    while (_mediaSessionId < 0 && millis() - start < 2500) {
      if (_tls.available()) {
          readMessages();
      } else {
          delay(50);
      }
    }
    
    if (_mediaSessionId < 0) {
      dbgLog.log("[Cast] Media cmd '%s' failed: still no mediaSessionId\n", type);
      return false;
    }
  }
  doc["type"] = type;
  doc["mediaSessionId"] = _mediaSessionId;
  doc["requestId"] = nextRequestId();
  String json;
  serializeJson(doc, json);
  dbgLog.log("[Cast] Sending %s to %s (msId=%d): %s\n", type, _sessionId, _mediaSessionId, json.c_str());
  return sendRaw(SOURCE_ID, _sessionId, NS_MEDIA, json.c_str());
}

bool ChromecastClient::play() {
  _playing = true; // Optimistic update
  JsonDocument doc;
  return _sendMediaCmd("PLAY", doc);
}

bool ChromecastClient::pause() {
  _playing = false; // Optimistic update
  JsonDocument doc;
  return _sendMediaCmd("PAUSE", doc);
}

bool ChromecastClient::stop() {
  JsonDocument doc;
  return _sendMediaCmd("STOP", doc);
}

bool ChromecastClient::next() {
  JsonDocument doc;
  // Send both types just in case — some apps only support one
  bool s1 = _sendMediaCmd("QUEUE_NEXT", doc);
  JsonDocument doc2;
  bool s2 = _sendMediaCmd("NEXT", doc2);
  return s1 || s2;
}

bool ChromecastClient::previous() {
  JsonDocument doc;
  bool s1 = _sendMediaCmd("QUEUE_PREV", doc);
  JsonDocument doc2;
  bool s2 = _sendMediaCmd("PREV", doc2);
  return s1 || s2;
}

bool ChromecastClient::seek(float positionSec) {
  JsonDocument doc;
  doc["currentTime"] = positionSec;
  return _sendMediaCmd("SEEK", doc);
}

// ─── Receive & parse ─────────────────────────────────────────────────────────
void ChromecastClient::readMessages() {
  while (_tls.available() && _rxLen < sizeof(_rxBuf)) {
    _rxBuf[_rxLen++] = (uint8_t)_tls.read();
  }

  while (_rxLen >= 4) {
    uint32_t msgLen = ((uint32_t)_rxBuf[0] << 24) |
                      ((uint32_t)_rxBuf[1] << 16) | ((uint32_t)_rxBuf[2] << 8) |
                      ((uint32_t)_rxBuf[3]);

    if (msgLen > sizeof(_rxBuf) - 4) {
      dbgLog.log("[Cast] Oversized msg %u, flushing\n", msgLen);
      _rxLen = 0;
      return;
    }
    if (_rxLen < 4 + msgLen)
      break;

    char ns[128], src[64], dst[64];
    parseCastMessage(_rxBuf + 4, msgLen, ns, sizeof(ns), src, sizeof(src), dst,
                     sizeof(dst), _payloadBuf, sizeof(_payloadBuf));

    handleMessage(ns, src, dst, _payloadBuf);

    size_t consumed = 4 + msgLen;
    memmove(_rxBuf, _rxBuf + consumed, _rxLen - consumed);
    _rxLen -= consumed;
  }
}

void ChromecastClient::handleMessage(const char *ns, const char *src,
                                     const char *dst, const char *payload) {
  if (strcmp(ns, NS_CONNECTION) == 0) {
    if (strstr(payload, "\"CLOSE\"")) {
      dbgLog.log("[Cast] Session closed by receiver (%s)\n", src);
      _mediaConnected = false;
      _mediaSessionId = -1;
    }
    return;
  }

  if (strcmp(ns, NS_HEARTBEAT) == 0) {
    if (strstr(payload, "\"PING\""))
      sendPong();
    return;
  }

  if (strcmp(ns, NS_RECEIVER) == 0) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, DeserializationOption::NestingLimit(64));
    if (error) {
      dbgLog.log("[Cast] JsonErr (Receiver): %s (len: %u bytes)\n", error.c_str(), strlen(payload));
      return;
    }
    const char *type = doc["type"];
    if (type && strcmp(type, "RECEIVER_STATUS") == 0) {
      JsonObject status = doc["status"];
      handleReceiverStatus(status);
    }
    return;
  }

  if (strcmp(ns, NS_MEDIA) == 0) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, DeserializationOption::NestingLimit(64));
    if (error) {
      dbgLog.log("[Cast] JsonErr (Media): %s (len: %u bytes)\n", error.c_str(), strlen(payload));
      return;
    }
    const char *type = doc["type"];
    if (type && strcmp(type, "MEDIA_STATUS") == 0) {
      JsonArray arr = doc["status"].as<JsonArray>();
      handleMediaStatus(arr);
    }
    return;
  }

  // Diagnostic: Log unknown namespaces to see if custom app protocols are used
  if (strcmp(ns, "urn:x-cast:com.google.cast.tp.heartbeat") != 0) {
      dbgLog.log("[Cast] Rcv NS: %s pay: %.50s...\n", ns, payload);
  }
}

void ChromecastClient::handleReceiverStatus(JsonObject &status) {
  // Volume (receiver-level)
  if (status["volume"]["level"].is<float>()) {
    _volume = status["volume"]["level"].as<float>();
    _volumeSynced = true;
  }
  if (status["volume"]["muted"].is<bool>()) {
    _muted = status["volume"]["muted"].as<bool>();
  }

  // Detect existing running application (passive — we just observe)
  // Note: some RECEIVER_STATUS responses are partial (volume-only, no
  // "applications" key). Only touch session state when the key is present.
  if (!status["applications"].isNull()) {
    JsonArray apps = status["applications"].as<JsonArray>();
    if (apps.size() > 0) {
      JsonObject app = apps[0];
      const char *tid = app["transportId"];
      const char *st = app["statusText"];
      const char *dn = app["displayName"];

      bool sessionChanged = false;
      if (tid) {
        if (strcmp(_sessionId, tid) != 0)
          sessionChanged = true;
        strncpy(_sessionId, tid, sizeof(_sessionId) - 1);
        _sessionId[sizeof(_sessionId) - 1] = '\0';
      }
      if (st) {
        strncpy(_statusText, st, sizeof(_statusText) - 1);
        _statusText[sizeof(_statusText) - 1] = '\0';
      }
      if (dn) {
        strncpy(_appName, dn, sizeof(_appName) - 1);
        _appName[sizeof(_appName) - 1] = '\0';
      }

      // If session changed, we need to reconnect to the media transport
      if (sessionChanged) {
        _mediaConnected = false;
        _mediaSessionId = -1;
        dbgLog.log("[Cast] Session detected: %s (%s)\n", _appName, _sessionId);
        ensureMediaConnection();
      }
    } else {
      // applications key present but empty — app was closed
      _sessionId[0] = '\0';
      _appName[0] = '\0';
      _mediaSessionId = -1;
      _mediaConnected = false;
      _playing = false;
    }
  }
}

void ChromecastClient::handleMediaStatus(JsonArray &arr) {
  if (arr.isNull() || arr.size() == 0)
    return;
  JsonObject ms = arr[0];

  // Only update if the field is present, making the ID "sticky"
  if (!ms["mediaSessionId"].isNull()) {
    _mediaSessionId = ms["mediaSessionId"];
  }
  if (!ms["currentTime"].isNull()) {
    _currentTime = ms["currentTime"];
  }
  if (!ms["media"].isNull() && !ms["media"]["duration"].isNull()) {
    _duration = ms["media"]["duration"];
  }

  if (!ms["playerState"].isNull()) {
    const char *pstate = ms["playerState"] | "IDLE";
    _playing = (strcmp(pstate, "PLAYING") == 0);
  }

  dbgLog.log("[Cast] MS: msId=%d t=%.1f/%.1f playing=%d sessionId=%s\n", 
             _mediaSessionId, _currentTime, _duration, _playing, _sessionId);
}
