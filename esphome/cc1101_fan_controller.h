#pragma once
#include "esphome.h"
#include "CC1101_ESP_Arduino.h"
#include "soc/gpio_struct.h"
#include <functional>

// ============================================================
// Pacific ceiling fans - CC1101 @ 433.92 MHz PWM-OOK
// ============================================================
// Frame: 30 bits = 20-bit address | 9-bit command | 1 parity bit, MSB first.
// Each bit is one mark + one space: '1' = long mark + short space,
// '0' = short mark + long space.  Frames are separated by a ~5.5ms gap.
//
// Parity is per remote, not global: the girls' remote makes the total number
// of ones even, while the office remote was seen doing the opposite.  So each
// fan is registered with its own rule, and RX uses that rule to reject
// corrupted frames.
//
// Everything below was measured with esphome_rf_capture.yaml against the
// girls' room remote (address 0xE5D7C), every button pressed twice.
// ============================================================

// ── Command table (all verified) ────────────────────────────
static const uint16_t FAN_CMD_TOGGLE   = 0x191;  // power: toggles, no discrete off
static const uint16_t FAN_CMD_BREEZE   = 0x10B;
static const uint16_t FAN_CMD_SPEED1   = 0x1E8;
static const uint16_t FAN_CMD_SPEED2   = 0x1C8;
static const uint16_t FAN_CMD_SPEED3   = 0x1A9;
static const uint16_t FAN_CMD_SPEED4   = 0x189;
static const uint16_t FAN_CMD_SPEED5   = 0x16A;
static const uint16_t FAN_CMD_SPEED6   = 0x14A;
static const uint16_t FAN_CMD_REVERSE  = 0x12B;  // F/R: same code every press
static const uint16_t FAN_CMD_TIMER_1H = 0x095;
static const uint16_t FAN_CMD_TIMER_4H = 0x152;
static const uint16_t FAN_CMD_LIGHT    = 0x1B1;  // toggles the light
static const uint16_t FAN_CMD_COLOUR   = 0x1D0;  // a real command, not a double light
static const uint16_t FAN_CMD_DIM_DOWN = 0x0F4;
static const uint16_t FAN_CMD_DIM_UP   = 0x133;

static const uint16_t FAN_SPEED_CMDS[6] = {
    FAN_CMD_SPEED1, FAN_CMD_SPEED2, FAN_CMD_SPEED3,
    FAN_CMD_SPEED4, FAN_CMD_SPEED5, FAN_CMD_SPEED6,
};

// 1..6 for a speed command, 0 for anything else.
static inline int fan_speed_of(uint16_t cmd) {
    for (int i = 0; i < 6; i++)
        if (FAN_SPEED_CMDS[i] == cmd) return i + 1;
    return 0;
}

static const char *fan_cmd_name(uint16_t cmd) {
    switch (cmd) {
        case FAN_CMD_TOGGLE:   return "Power";
        case FAN_CMD_BREEZE:   return "Breeze";
        case FAN_CMD_SPEED1:   return "Speed 1";
        case FAN_CMD_SPEED2:   return "Speed 2";
        case FAN_CMD_SPEED3:   return "Speed 3";
        case FAN_CMD_SPEED4:   return "Speed 4";
        case FAN_CMD_SPEED5:   return "Speed 5";
        case FAN_CMD_SPEED6:   return "Speed 6";
        case FAN_CMD_REVERSE:  return "F/R";
        case FAN_CMD_TIMER_1H: return "Timer 1H";
        case FAN_CMD_TIMER_4H: return "Timer 4H";
        case FAN_CMD_LIGHT:    return "Light";
        case FAN_CMD_COLOUR:   return "Light colour";
        case FAN_CMD_DIM_DOWN: return "LED-";
        case FAN_CMD_DIM_UP:   return "LED+";
        default:               return "?";
    }
}

// ── Measured timings (averages over ~45 captured bursts) ────
static const int OOK_SHORT_US = 375;
static const int OOK_LONG_US  = 1090;
static const int OOK_GAP_US   = 5600;
static const int OOK_TX_REPS  = 10;   // the remote sends ~9 per keying

// ── Light ───────────────────────────────────────────────────
// The dimmer has about 7-8 steps (hard to tell by eye); 8 keeps the ends
// reachable, and DIM_OVERSHOOT extra presses at either end absorb the doubt.
static const int DIM_STEPS = 8;
static const int DIM_OVERSHOOT = 2;
// Toggling the light off and on quickly changes its colour; keep HA-driven
// toggles at least this far apart.
static const uint32_t LIGHT_MIN_GAP_MS = 3000;

// ── RX decoder tolerances ───────────────────────────────────
static const uint16_t RX_MARK_MIN   = 150;
static const uint16_t RX_MARK_SPLIT = 700;    // below = '0', above = '1'
static const uint16_t RX_MARK_MAX   = 1800;
static const uint16_t RX_GAP_MIN    = 3500;
// A press is keyed twice ~200ms apart, ~9 frames each.  Frames of the same
// code closer together than this belong to one press.
static const uint32_t RX_PRESS_GAP_MS = 500;

// ============================================================
// Hardware pins - must match the wiring
// ============================================================
#ifndef CC1101_CS_PIN
#define CC1101_CS_PIN    5
#endif
#ifndef CC1101_GDO0_PIN
#define CC1101_GDO0_PIN  25
#endif
#ifndef CC1101_GDO2_PIN
#define CC1101_GDO2_PIN  26
#endif
#ifndef CC1101_SCK_PIN
#define CC1101_SCK_PIN   18
#endif
#ifndef CC1101_MISO_PIN
#define CC1101_MISO_PIN  19
#endif
#ifndef CC1101_MOSI_PIN
#define CC1101_MOSI_PIN  23
#endif

// ── ISR ring buffer: edge durations off GDO2 ────────────────
// A press is ~1100 edges (two keyings of ~9 frames), and the loop can be
// busy for a while (our own TX, an API call), so leave room for several.
#define FAN_RX_RING 4096                       // must stay a power of two

struct FanRxRing {
    volatile uint16_t buf[FAN_RX_RING];        // bit15 set = the pulse that ended was a mark
    volatile uint32_t head;
    volatile uint32_t last_us;
};
static FanRxRing fan_rx_ring;
static uint8_t fan_rx_pin = CC1101_GDO2_PIN;

// digitalRead() lives in flash and is not safe from an IRAM ISR.
static inline bool IRAM_ATTR fan_rx_read(uint8_t pin) {
    if (pin < 32) return (GPIO.in >> pin) & 0x1;
    return (GPIO.in1.val >> (pin - 32)) & 0x1;
}

static void IRAM_ATTR fan_rx_isr() {
    uint32_t now = micros();
    uint32_t dt = now - fan_rx_ring.last_us;
    fan_rx_ring.last_us = now;
    if (dt > 0x7FFF) dt = 0x7FFF;
    uint16_t v = (uint16_t) dt;
    if (!fan_rx_read(fan_rx_pin)) v |= 0x8000;   // now LOW -> it was a HIGH mark
    fan_rx_ring.buf[fan_rx_ring.head & (FAN_RX_RING - 1)] = v;
    fan_rx_ring.head++;
}

// ============================================================
// FanRadio: sits in RX, decodes remote presses, transmits on demand
// ============================================================
class FanRadio {
    struct Fan {
        uint32_t addr;
        bool even_parity;
        std::function<void(uint16_t cmd)> on_command;   // once per physical press
    };
    static const int MAX_FANS = 4;

    CC1101 radio_;
    int gdo0_, gdo2_;
    bool ready_ = false;
    portMUX_TYPE tx_mux_ = portMUX_INITIALIZER_UNLOCKED;

    Fan fans_[MAX_FANS];
    int nfans_ = 0;

    // Transmissions are queued and sent one per poll(): a burst takes ~0.5s,
    // and running several back to back from an API call starves the loop
    // until the task watchdog reboots the chip.
    struct TxItem { uint32_t addr; uint16_t cmd; };
    static const int TXQ = 16;
    TxItem txq_[TXQ];
    uint8_t txq_head_ = 0, txq_tail_ = 0;

    // decoder
    uint32_t tail_ = 0;
    enum { WAIT_GAP, EXPECT_MARK, EXPECT_SPACE } state_ = WAIT_GAP;
    uint16_t mark_ = 0;
    uint32_t frame_ = 0;
    int bits_ = 0;

    // press detection: a code must be seen twice in a row before it counts,
    // then further copies are swallowed until the press goes quiet.
    uint32_t cand_ = 0;
    int cand_n_ = 0;
    uint32_t cand_last_ms_ = 0;

public:
    bool learn_mode = false;

    FanRadio(int sck, int miso, int mosi, int cs, int gdo0, int gdo2)
        : radio_(sck, miso, mosi, cs, gdo0, gdo2), gdo0_(gdo0), gdo2_(gdo2) {}

    bool ready() const { return ready_; }

    void add_fan(uint32_t addr, bool even_parity, std::function<void(uint16_t)> on_command) {
        if (nfans_ < MAX_FANS) fans_[nfans_++] = {addr & 0xFFFFF, even_parity, on_command};
    }

    bool begin() {
        radio_.init();
        radio_.setMHZ(433.92);
        radio_.setTXPwr(TX_PLUS_10_DBM);
        radio_.setDataRate(2400);
        radio_.setRxBW(RX_BW_162_KHZ);
        radio_.setModulation(ASK_OOK);

        uint8_t version = radio_.getVersion();
        if (version == 0x00 || version == 0xFF) {
            ESP_LOGE("fan_rf", "CC1101 not detected (ver=0x%02X). Check wiring.", version);
            return false;
        }

        // The library only drives GDO0 as an output when built without GDO2.
        pinMode(gdo0_, OUTPUT);
        digitalWrite(gdo0_, LOW);
        fan_rx_pin = gdo2_;
        pinMode(gdo2_, INPUT);

        ready_ = true;
        start_rx_();
        ESP_LOGI("fan_rf", "CC1101 ready (ver=0x%02X), listening on GDO2", version);
        return true;
    }

    void send(uint32_t address, uint16_t command) {
        if (!ready_) {
            ESP_LOGW("fan_rf", "Not initialised - skipping TX");
            return;
        }
        if ((uint8_t)(txq_head_ - txq_tail_) >= TXQ) {
            ESP_LOGW("fan_rf", "TX queue full - dropping cmd=0x%03X", command);
            return;
        }
        txq_[txq_head_++ % TXQ] = {address, command};
    }

private:
    void transmit_(uint32_t address, uint16_t command) {
        const Fan *fan = find_(address);
        bool even = fan ? fan->even_parity : true;

        uint32_t msg = ((address & 0xFFFFF) << 9) | (command & 0x1FF);
        bool odd_ones = __builtin_popcount(msg) & 1;
        bool parity = even ? odd_ones : !odd_ones;
        uint32_t frame = (msg << 1) | (parity ? 1 : 0);

        // Stop listening so we do not decode our own transmission.
        detachInterrupt(digitalPinToInterrupt(gdo2_));
        radio_.setTx();
        digitalWrite(gdo0_, LOW);
        delayMicroseconds(OOK_GAP_US);

        for (int rep = 0; rep < OOK_TX_REPS; rep++) {
            portENTER_CRITICAL(&tx_mux_);
            for (int b = 29; b >= 0; b--) {
                bool one = (frame >> b) & 1;
                digitalWrite(gdo0_, HIGH);
                delayMicroseconds(one ? OOK_LONG_US : OOK_SHORT_US);
                digitalWrite(gdo0_, LOW);
                delayMicroseconds(one ? OOK_SHORT_US : OOK_LONG_US);
            }
            portEXIT_CRITICAL(&tx_mux_);
            delayMicroseconds(OOK_GAP_US);
            App.feed_wdt();
        }

        digitalWrite(gdo0_, LOW);
        start_rx_();
        ESP_LOGI("fan_rf", "TX  addr=0x%05X  cmd=0x%03X (%s)", address, command, fan_cmd_name(command));
    }

public:
    // Drain the ISR ring; call from a short interval.
    void poll() {
        if (!ready_) return;
        uint32_t h = fan_rx_ring.head;
        if (h - tail_ > FAN_RX_RING) {           // overrun: resync on the next gap
            tail_ = h - FAN_RX_RING;
            state_ = WAIT_GAP;
        }
        while (tail_ != h) {
            uint16_t v = fan_rx_ring.buf[tail_ & (FAN_RX_RING - 1)];
            tail_++;
            feed_(v & 0x7FFF, (v & 0x8000) != 0);
        }
        if (txq_tail_ != txq_head_) {
            TxItem it = txq_[txq_tail_++ % TXQ];
            transmit_(it.addr, it.cmd);
        }
    }

private:
    const Fan *find_(uint32_t addr) const {
        for (int i = 0; i < nfans_; i++)
            if (fans_[i].addr == (addr & 0xFFFFF)) return &fans_[i];
        return nullptr;
    }

    void start_rx_() {
        radio_.setRx();
        fan_rx_ring.last_us = micros();
        tail_ = fan_rx_ring.head;
        state_ = WAIT_GAP;
        attachInterrupt(digitalPinToInterrupt(gdo2_), fan_rx_isr, CHANGE);
    }

    void feed_(uint16_t dur, bool high) {
        switch (state_) {
            case WAIT_GAP:
                if (!high && dur >= RX_GAP_MIN) { bits_ = 0; frame_ = 0; state_ = EXPECT_MARK; }
                return;
            case EXPECT_MARK:
                if (!high || dur < RX_MARK_MIN || dur > RX_MARK_MAX) { state_ = WAIT_GAP; return; }
                mark_ = dur;
                state_ = EXPECT_SPACE;
                return;
            case EXPECT_SPACE:
                if (high) { state_ = WAIT_GAP; return; }
                frame_ = (frame_ << 1) | (mark_ > RX_MARK_SPLIT ? 1 : 0);
                bits_++;
                if (dur >= RX_GAP_MIN) {
                    if (bits_ == 30) on_frame_(frame_);
                    bits_ = 0; frame_ = 0;            // the next repeat starts right here
                } else if (bits_ > 30) {
                    state_ = WAIT_GAP;
                    return;
                }
                state_ = EXPECT_MARK;
                return;
        }
    }

    // Learn mode reports each distinct frame once per press, valid or not.
    uint32_t learn_last_ = 0;
    uint32_t learn_last_ms_ = 0;
    void log_learn_(uint32_t f, uint32_t addr, uint16_t cmd, bool total_even,
                    const Fan *fan, bool valid) {
        uint32_t now = millis();
        bool repeat = f == learn_last_ && now - learn_last_ms_ < RX_PRESS_GAP_MS;
        learn_last_ = f;
        learn_last_ms_ = now;
        if (repeat) return;
        ESP_LOGI("fan_rf", "LEARN addr=0x%05X cmd=0x%03X (%s) parity=%s%s", addr, cmd,
                 fan_cmd_name(cmd), total_even ? "even" : "odd",
                 fan ? (valid ? "" : "  [known address, WRONG parity]") : "  [unknown address]");
    }

    void on_frame_(uint32_t f) {
        uint32_t addr = (f >> 10) & 0xFFFFF;
        uint16_t cmd = (f >> 1) & 0x1FF;
        bool total_even = !(__builtin_popcount(f) & 1);
        const Fan *fan = find_(addr);
        bool valid = fan && (total_even == fan->even_parity);

        if (learn_mode) log_learn_(f, addr, cmd, total_even, fan, valid);
        // Only frames that pass the whitelist and parity take part in press
        // detection: a corrupted frame, or another fan's remote, must not
        // reset the count of a press that is still arriving.
        if (!valid) return;

        uint32_t now = millis();
        if (f == cand_ && now - cand_last_ms_ < RX_PRESS_GAP_MS) {
            cand_n_++;
        } else {
            cand_ = f;
            cand_n_ = 1;
        }
        cand_last_ms_ = now;
        if (cand_n_ != 2) return;                 // not confirmed yet, or already reported

        ESP_LOGI("fan_rf", "RX  addr=0x%05X  cmd=0x%03X (%s)", addr, cmd, fan_cmd_name(cmd));
        if (fan->on_command) fan->on_command(cmd);
    }
};

static FanRadio fan_rf(
    CC1101_SCK_PIN,
    CC1101_MISO_PIN,
    CC1101_MOSI_PIN,
    CC1101_CS_PIN,
    CC1101_GDO0_PIN,
    CC1101_GDO2_PIN
);
