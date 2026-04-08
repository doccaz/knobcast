#pragma once
// rotary_encoder.h
// Interrupt-driven rotary encoder with push-button, debounce, and acceleration.
//
// Wiring (KY-040 or equivalent):
//   CLK (A)  → PIN_ENC_CLK   (with 10k pull-up or internal pull-up)
//   DT  (B)  → PIN_ENC_DT
//   SW       → PIN_ENC_SW    (active-low; internal pull-up)
//   +        → 3.3V
//   GND      → GND

#include <Arduino.h>

// ── Configurable pins (override in main.cpp before #include) ─────────────────
#ifndef PIN_ENC_CLK
#define PIN_ENC_CLK  2    // interrupt-capable on ESP32-C3
#endif
#ifndef PIN_ENC_DT
#define PIN_ENC_DT   3
#endif
#ifndef PIN_ENC_SW
#define PIN_ENC_SW   4
#endif

// ── Event types returned by poll() ───────────────────────────────────────────
enum class EncEvent : uint8_t {
    NONE        = 0,
    CW          = 1,   // clockwise step
    CCW         = 2,   // counter-clockwise step
    PRESS       = 3,   // short press (< LONG_PRESS_MS)
    LONG_PRESS  = 4,   // held ≥ LONG_PRESS_MS
};

class RotaryEncoder {
public:
    static constexpr uint32_t DEBOUNCE_MS     = 5;
    static constexpr uint32_t LONG_PRESS_MS   = 800;
    // Acceleration: if steps arrive faster than this, multiply them
    static constexpr uint32_t ACCEL_WINDOW_MS = 60;  // ms between steps
    static constexpr int      ACCEL_FACTOR    = 5;   // jump multiplier

    void begin() {
        pinMode(PIN_ENC_CLK, INPUT_PULLUP);
        pinMode(PIN_ENC_DT,  INPUT_PULLUP);
        pinMode(PIN_ENC_SW,  INPUT_PULLUP);

        _lastClk    = digitalRead(PIN_ENC_CLK);
        _lastSw     = HIGH;
        _swDownMs   = 0;
        _pending    = EncEvent::NONE;
        _pendingDelta = 0;
        _lastStepMs = 0;

        attachInterruptArg(digitalPinToInterrupt(PIN_ENC_CLK),
                           _isrClk, this, CHANGE);
    }

    // Call from loop() – returns pending event (NONE if nothing new).
    // When CW/CCW is returned, delta() gives the signed step count (with accel).
    EncEvent poll() {
        // ── Encoder steps ─────────────────────────────────────────────────
        if (_rawDelta != 0) {
            noInterrupts();
            int delta = _rawDelta;
            _rawDelta = 0;
            interrupts();

            uint32_t now = millis();
            uint32_t gap = now - _lastStepMs;
            _lastStepMs  = now;

            int accel = (gap < ACCEL_WINDOW_MS) ? ACCEL_FACTOR : 1;
            _pendingDelta = delta * accel;
            _pending = (_pendingDelta > 0) ? EncEvent::CW : EncEvent::CCW;
            return _pending;
        }

        // ── Button ────────────────────────────────────────────────────────
        bool sw = digitalRead(PIN_ENC_SW);
        uint32_t now = millis();

        if (sw == LOW && _lastSw == HIGH) {
            // press begin
            _swDownMs = now;
        } else if (sw == HIGH && _lastSw == LOW) {
            // release
            uint32_t held = now - _swDownMs;
            _lastSw = HIGH;
            _pending = (held >= LONG_PRESS_MS)
                       ? EncEvent::LONG_PRESS : EncEvent::PRESS;
            return _pending;
        }
        _lastSw = sw;
        return EncEvent::NONE;
    }

    // Signed step count from last CW/CCW event (includes acceleration)
    int delta() const { return _pendingDelta; }

private:
    volatile int _rawDelta  = 0;
    int          _lastClk;
    bool         _lastSw;
    uint32_t     _swDownMs;
    EncEvent     _pending;
    int          _pendingDelta;
    uint32_t     _lastStepMs;

    static void IRAM_ATTR _isrClk(void* arg) {
        RotaryEncoder* self = (RotaryEncoder*)arg;
        uint32_t now = micros();
        static uint32_t lastUs = 0;
        if (now - lastUs < 2000) return;  // debounce 2 ms
        lastUs = now;

        int clk = digitalRead(PIN_ENC_CLK);
        int dt  = digitalRead(PIN_ENC_DT);
        if (clk != self->_lastClk) {
            self->_lastClk = clk;
            if (clk == LOW) {
                // rising edge of CLK; DT tells direction
                self->_rawDelta += (dt != clk) ? +1 : -1;
            }
        }
    }
};
