#pragma once
// rotary_encoder.h
// Interrupt-driven rotary encoder with push-button.
// Uses full gray-code quadrature state machine with divisor=2
// (one step per two transitions — one per half-detent).
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
    static constexpr uint32_t LONG_PRESS_MS = 800;
    static constexpr int DIVISOR = 2;  // transitions per step

    void begin() {
        pinMode(PIN_ENC_CLK, INPUT_PULLUP);
        pinMode(PIN_ENC_DT,  INPUT_PULLUP);
        pinMode(PIN_ENC_SW,  INPUT_PULLUP);

        _lastSw     = HIGH;
        _swDownMs   = 0;
        _pending    = EncEvent::NONE;
        _pendingDelta = 0;

        // Initialize quadrature state
        _quadState = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);

        attachInterruptArg(digitalPinToInterrupt(PIN_ENC_CLK),
                           _isrQuad, this, CHANGE);
        attachInterruptArg(digitalPinToInterrupt(PIN_ENC_DT),
                           _isrQuad, this, CHANGE);
    }

    // Call from loop() – returns pending event (NONE if nothing new).
    // When CW/CCW is returned, delta() gives the signed step count.
    EncEvent poll() {
        // ── Encoder steps ─────────────────────────────────────────────────
        if (_rawDelta != 0) {
            noInterrupts();
            int delta = _rawDelta;
            _rawDelta = 0;
            interrupts();

            // Reset accumulator on direction change to prevent
            // old-direction remainder causing wrong-direction steps
            if ((_accumulator > 0 && delta < 0) || (_accumulator < 0 && delta > 0)) {
                _accumulator = 0;
            }
            _accumulator += delta;
            int steps = _accumulator / DIVISOR;
            if (steps == 0) return EncEvent::NONE;
            _accumulator %= DIVISOR;

            _pendingDelta = steps;
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

    // Signed step count from last CW/CCW event
    int delta() const { return _pendingDelta; }

private:
    volatile int _rawDelta    = 0;
    volatile uint8_t _quadState = 0;
    int          _accumulator = 0;
    bool         _lastSw;
    uint32_t     _swDownMs;
    EncEvent     _pending;
    int          _pendingDelta;

    static void IRAM_ATTR _isrQuad(void* arg) {
        // Gray-code quadrature state table.
        // Index = (oldState << 2) | newState, value = direction (+1, -1, or 0).
        static const int8_t QEM[16] = {
        //  new:  00  01  10  11    old:
                 0, -1, +1,  0,  // 00
                +1,  0,  0, -1,  // 01
                -1,  0,  0, +1,  // 10
                 0, +1, -1,  0,  // 11
        };
        RotaryEncoder* self = (RotaryEncoder*)arg;
        uint8_t newState = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
        uint8_t idx = (self->_quadState << 2) | newState;
        int8_t dir = QEM[idx];
        self->_quadState = newState;
        if (dir) self->_rawDelta += dir;
    }
};
