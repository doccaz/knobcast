#pragma once
// debug_log.h
// Ring buffer that captures log messages for display in the web UI.
// Usage: call debugLog("format", ...) instead of Serial.printf().
// It prints to Serial AND stores in the ring buffer.

#include <Arduino.h>

static constexpr int LOG_LINES     = 60;
static constexpr int LOG_LINE_LEN  = 256;

class DebugLog {
public:
    bool enabled = false;

    void log(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!fmt) return;

        // Use static buffers to save ~0.5KB of stack space per call
        static char buf[LOG_LINE_LEN];
        static char prefixLine[LOG_LINE_LEN];

        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        // Always print to Serial
        Serial.print(buf);

        if (!enabled) return;

        // Strip trailing newline for cleaner web display
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (len == 0) return;

        // Add 10-char timestamp prefix: [ssss.mmm]
        uint32_t ms = millis();
        snprintf(prefixLine, sizeof(prefixLine), "[%u.%03u] %s",
                 (unsigned)(ms / 1000), (unsigned)(ms % 1000), buf);

        strncpy(_lines[_head], prefixLine, LOG_LINE_LEN - 1);
        _lines[_head][LOG_LINE_LEN - 1] = '\0';
        _head = (_head + 1) % LOG_LINES;
        if (_count < LOG_LINES) _count++;
    }

    // Build a JSON array of log lines (oldest first)
    String toJson() const {
        String json = "[";
        int start = (_count < LOG_LINES) ? 0 : _head;
        for (int i = 0; i < _count; i++) {
            int idx = (start + i) % LOG_LINES;
            if (i > 0) json += ",";
            json += "\"";
            // Escape quotes and backslashes
            for (const char* p = _lines[idx]; *p; p++) {
                if (*p == '"') json += "\\\"";
                else if (*p == '\\') json += "\\\\";
                else json += *p;
            }
            json += "\"";
        }
        json += "]";
        return json;
    }

    void clear() { _head = 0; _count = 0; }

private:
    char _lines[LOG_LINES][LOG_LINE_LEN] = {};
    int  _head = 0;
    int  _count = 0;
};

// Global instance
extern DebugLog dbgLog;
