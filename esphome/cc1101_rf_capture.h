#pragma once
#include "esphome.h"
#include "cc1101_esp_arduino.h"
#include "soc/gpio_struct.h"

// ============================================================
// CC1101 RF capture / protocol-mapping firmware
// ============================================================
// This is a *measurement* tool, not a controller.  It parks the CC1101
// in RX and continuously decodes PWM-OOK frames off GDO2, then prints
// everything needed to pin the protocol down:
//
//   - the raw bit string (so the 20/9/1 split can be re-checked)
//   - the 20-bit address and 9-bit command
//   - the parity bit AND the parity of the 29 data bits, so the parity
//     scheme is *derived* from real frames instead of assumed
//   - pulse-width statistics and a histogram, so OOK_SHORT_US /
//     OOK_LONG_US / OOK_GAP_US come from measurement
//   - how many times the remote repeats each frame
//
// Nothing is filtered by address here: every valid-looking frame is
// reported, which is the point - we do not yet know our addresses.
//
// See CAPTURE_PROCEDURE.md for the button-press procedure.
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

// ── Decoder tolerances ──────────────────────────────────────
// Deliberately wide.  We are measuring the timings, not asserting them.
static const uint16_t RFC_MARK_MIN   = 120;   // shorter than this is noise
static const uint16_t RFC_MARK_SPLIT = 700;   // below = '0', above = '1'
static const uint16_t RFC_MARK_MAX   = 2200;  // longer than this ends a frame
static const uint16_t RFC_GAP_MIN    = 2200;  // a LOW this long separates frames
static const int      RFC_MIN_BITS   = 24;    // reject anything shorter
static const int      RFC_MAX_BITS   = 40;
static const uint32_t RFC_BURST_MS   = 400;   // silence that ends a burst
// Repeats inside one transmission come ~50ms apart (frame + gap).  A longer
// pause before an identical frame means the remote keyed up a second time -
// which is how a button that "double-taps" an existing command shows itself.
static const uint32_t RFC_TX_SPLIT_MS = 150;

// ── ISR ring buffer ─────────────────────────────────────────
#define RFC_RING 2048                          // must stay a power of two

struct RfcRing {
    volatile uint16_t buf[RFC_RING];           // bit15 = level of the pulse that ended
    volatile uint32_t head;                    // monotonic write counter
    volatile uint32_t last_us;
};
static RfcRing rfc_ring;
static uint8_t rfc_gdo2_pin = CC1101_GDO2_PIN;

// Read a GPIO straight off the register: digitalRead() lives in flash and
// is not safe to call from an IRAM ISR.
static inline bool IRAM_ATTR rfc_fast_read(uint8_t pin) {
    if (pin < 32) return (GPIO.in >> pin) & 0x1;
    return (GPIO.in1.val >> (pin - 32)) & 0x1;
}

static void IRAM_ATTR rfc_isr() {
    uint32_t now = micros();
    uint32_t dt = now - rfc_ring.last_us;
    rfc_ring.last_us = now;
    if (dt > 0x7FFF) dt = 0x7FFF;
    // The pulse that just ended held the level opposite to the one we read now.
    uint16_t v = (uint16_t) dt;
    if (!rfc_fast_read(rfc_gdo2_pin)) v |= 0x8000;   // now LOW  -> it was a HIGH mark
    rfc_ring.buf[rfc_ring.head & (RFC_RING - 1)] = v;
    rfc_ring.head++;
}

// ── Capture procedure: the remote's buttons, in press order ─
static const char *RFC_STEPS[] = {
    "POWER  (power symbol, top left)",
    "BREEZE (wave symbol, top right)",
    "SPEED 1",
    "SPEED 2",
    "SPEED 3",
    "SPEED 4",
    "SPEED 5",
    "SPEED 6",
    "F/R    (direction, centre of the dial)",
    "TIMER 1H",
    "TIMER 4H",
    "LIGHT  (bulb)",
    "LIGHT COLOUR (three dots)",
    "LED-   (dim down)",
    "LED+   (dim up)",
};
static const int RFC_NSTEPS = sizeof(RFC_STEPS) / sizeof(RFC_STEPS[0]);

// Pulse statistics for one frame or one burst of repeats.
struct RfcStats {
    uint32_t short_n, short_sum; uint16_t short_min, short_max;
    uint32_t long_n,  long_sum;  uint16_t long_min,  long_max;
    uint32_t gap_n,   gap_sum;   uint16_t gap_min,   gap_max;
    uint16_t hist[44];                       // 50us bins covering 0..2200us

    void reset() {
        short_n = short_sum = 0; short_min = 0xFFFF; short_max = 0;
        long_n  = long_sum  = 0; long_min  = 0xFFFF; long_max  = 0;
        gap_n   = gap_sum   = 0; gap_min   = 0xFFFF; gap_max   = 0;
        memset(hist, 0, sizeof(hist));
    }

    void add_mark(uint16_t d, bool is_long) {
        if (is_long) {
            long_n++; long_sum += d;
            if (d < long_min) long_min = d;
            if (d > long_max) long_max = d;
        } else {
            short_n++; short_sum += d;
            if (d < short_min) short_min = d;
            if (d > short_max) short_max = d;
        }
        int bin = d / 50;
        if (bin < 44) hist[bin]++;
    }

    void add_gap(uint16_t d) {
        gap_n++; gap_sum += d;
        if (d < gap_min) gap_min = d;
        if (d > gap_max) gap_max = d;
    }

    void merge(const RfcStats &o) {
        short_n += o.short_n; short_sum += o.short_sum;
        if (o.short_min < short_min) short_min = o.short_min;
        if (o.short_max > short_max) short_max = o.short_max;
        long_n += o.long_n; long_sum += o.long_sum;
        if (o.long_min < long_min) long_min = o.long_min;
        if (o.long_max > long_max) long_max = o.long_max;
        gap_n += o.gap_n; gap_sum += o.gap_sum;
        if (o.gap_min < gap_min) gap_min = o.gap_min;
        if (o.gap_max > gap_max) gap_max = o.gap_max;
        for (int i = 0; i < 44; i++) hist[i] += o.hist[i];
    }
};

class RfCapture {
    static const int RAW_MAX = 160;

    CC1101 radio_;
    int gdo0_, gdo2_;
    bool ready_ = false;

    uint32_t tail_ = 0;
    uint32_t dropped_ = 0;

    // frame decoder state
    enum { WAIT_GAP, EXPECT_MARK, EXPECT_SPACE } state_ = WAIT_GAP;
    uint16_t pending_mark_ = 0;
    uint64_t frame_ = 0;
    int bits_ = 0;

    // stats/raw for the frame currently being decoded
    RfcStats cur_;
    uint16_t cur_raw_[RAW_MAX];
    int cur_raw_n_ = 0;

    // the open burst (a run of identical repeated frames)
    bool burst_open_ = false;
    uint64_t burst_frame_ = 0;
    int burst_bits_ = 0;
    int burst_reps_ = 0;
    uint32_t burst_last_ms_ = 0;
    int burst_step_ = 0;
    int burst_txs_ = 0;                 // separate keyings of the same code
    uint16_t burst_tx_gaps_[8];         // ms between those keyings
    int burst_tx_gap_n_ = 0;
    uint32_t prev_burst_end_ms_ = 0;
    RfcStats burst_;
    uint16_t burst_raw_[RAW_MAX];
    int burst_raw_n_ = 0;

    int step_ = 0;
    int burst_seq_ = 0;

public:
    bool dump_raw = false;

    RfCapture(int sck, int miso, int mosi, int cs, int gdo0, int gdo2)
        : radio_(sck, miso, mosi, cs, gdo0, gdo2), gdo0_(gdo0), gdo2_(gdo2) {
        cur_.reset();
        burst_.reset();
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
            ESP_LOGE("rfcap", "CC1101 not detected (ver=0x%02X). Check SPI wiring.", version);
            return false;
        }

        // GDO0 is the async TX data input; keep it quiet while we listen.
        pinMode(gdo0_, OUTPUT);
        digitalWrite(gdo0_, LOW);

        rfc_gdo2_pin = gdo2_;
        pinMode(gdo2_, INPUT);
        rfc_ring.head = 0;
        rfc_ring.last_us = micros();
        tail_ = 0;

        radio_.setRx();
        attachInterrupt(digitalPinToInterrupt(gdo2_), rfc_isr, CHANGE);

        ready_ = true;
        ESP_LOGI("rfcap", "=========================================================");
        ESP_LOGI("rfcap", " RF CAPTURE READY  -  CC1101 ver=0x%02X  433.92 MHz OOK", version);
        ESP_LOGI("rfcap", " Listening continuously on GDO2 (GPIO %d).", gdo2_);
        ESP_LOGI("rfcap", " Press 'Next Button' in the web UI, then press that");
        ESP_LOGI("rfcap", " button on the remote twice, ~2s apart.");
        ESP_LOGI("rfcap", "=========================================================");
        return true;
    }

    // Called from a short ESPHome interval; drains the ISR ring and decodes.
    void poll() {
        if (!ready_) return;

        uint32_t h = rfc_ring.head;
        if (h - tail_ > RFC_RING) {
            dropped_ += (h - tail_ - RFC_RING);
            tail_ = h - RFC_RING;
            state_ = WAIT_GAP;           // we lost continuity; resync on the next gap
        }
        while (tail_ != h) {
            uint16_t v = rfc_ring.buf[tail_ & (RFC_RING - 1)];
            tail_++;
            feed_(v & 0x7FFF, (v & 0x8000) != 0);
        }

        if (burst_open_ && (millis() - burst_last_ms_) > RFC_BURST_MS) close_burst_();
    }

    // ── Procedure control ───────────────────────────────────
    void next_step() { if (step_ < RFC_NSTEPS) step_++; announce_(); }
    void prev_step() { if (step_ > 0) step_--; announce_(); }
    void reset_steps() {
        step_ = 0;
        ESP_LOGI("rfcap", "");
        ESP_LOGI("rfcap", "########## PROCEDURE RESET ##########");
        announce_();
    }
    std::string step_label() {
        if (step_ < 1 || step_ > RFC_NSTEPS) return "(not started)";
        char b[96];
        snprintf(b, sizeof(b), "%02d/%02d  %s", step_, RFC_NSTEPS, RFC_STEPS[step_ - 1]);
        return std::string(b);
    }

private:
    void announce_() {
        if (step_ < 1 || step_ > RFC_NSTEPS) {
            ESP_LOGI("rfcap", "Procedure not started. Press 'Next Button' to begin.");
            return;
        }
        ESP_LOGI("rfcap", "");
        ESP_LOGI("rfcap", "===== STEP %02d/%02d : %s =====", step_, RFC_NSTEPS, RFC_STEPS[step_ - 1]);
        ESP_LOGI("rfcap", "      now press that button on the remote, twice, ~2s apart");
    }

    void drop_frame_() {
        state_ = WAIT_GAP;
        bits_ = 0;
        frame_ = 0;
        cur_.reset();
        cur_raw_n_ = 0;
    }

    void feed_(uint16_t dur, bool high) {
        switch (state_) {
            case WAIT_GAP:
                if (!high && dur >= RFC_GAP_MIN) {
                    bits_ = 0; frame_ = 0;
                    cur_.reset(); cur_raw_n_ = 0;
                    state_ = EXPECT_MARK;
                }
                return;

            case EXPECT_MARK:
                if (!high) { drop_frame_(); return; }
                if (dur < RFC_MARK_MIN || dur > RFC_MARK_MAX) { drop_frame_(); return; }
                pending_mark_ = dur;
                state_ = EXPECT_SPACE;
                return;

            case EXPECT_SPACE: {
                if (high) { drop_frame_(); return; }
                bool one = pending_mark_ > RFC_MARK_SPLIT;
                frame_ = (frame_ << 1) | (one ? 1 : 0);
                bits_++;
                cur_.add_mark(pending_mark_, one);
                if (cur_raw_n_ < RAW_MAX - 1) {
                    cur_raw_[cur_raw_n_++] = pending_mark_;
                    cur_raw_[cur_raw_n_++] = dur;
                }

                if (bits_ > RFC_MAX_BITS) { drop_frame_(); return; }

                if (dur >= RFC_GAP_MIN) {
                    cur_.add_gap(dur);
                    if (bits_ >= RFC_MIN_BITS) {
                        on_frame_(frame_, bits_);
                    }
                    bits_ = 0; frame_ = 0;
                    cur_.reset(); cur_raw_n_ = 0;
                    state_ = EXPECT_MARK;        // the next repeat starts right here
                } else {
                    state_ = EXPECT_MARK;
                }
                return;
            }
        }
    }

    void on_frame_(uint64_t f, int bits) {
        uint32_t now = millis();

        if (burst_open_ && f == burst_frame_ && bits == burst_bits_ &&
            (now - burst_last_ms_) <= RFC_BURST_MS) {
            uint32_t dt = now - burst_last_ms_;
            if (dt > RFC_TX_SPLIT_MS) {
                // Same code, but too late to be a repeat: a second keying.
                burst_txs_++;
                if (burst_tx_gap_n_ < 8) burst_tx_gaps_[burst_tx_gap_n_++] = (uint16_t) dt;
            }
            burst_.merge(cur_);
            burst_reps_++;
            burst_last_ms_ = now;
            return;
        }

        // A different frame (or a long silence) ends the previous burst.  Close
        // it first, then adopt this frame's stats as the new burst's.
        if (burst_open_) close_burst_();

        burst_open_ = true;
        burst_frame_ = f;
        burst_bits_ = bits;
        burst_reps_ = 1;
        burst_last_ms_ = now;
        burst_step_ = step_;
        burst_txs_ = 1;
        burst_tx_gap_n_ = 0;
        burst_ = cur_;
        burst_raw_n_ = cur_raw_n_;
        memcpy(burst_raw_, cur_raw_, sizeof(uint16_t) * cur_raw_n_);
    }

    void close_burst_() {
        burst_open_ = false;
        burst_seq_++;

        uint64_t f = burst_frame_;
        int bits = burst_bits_;

        // bit string, split 20 | 9 | 1
        char bs[RFC_MAX_BITS + 4];
        int n = 0;
        for (int b = bits - 1; b >= 0; b--) {
            bs[n++] = ((f >> b) & 1) ? '1' : '0';
            int printed = bits - b;
            if (bits == 30 && (printed == 20 || printed == 29)) bs[n++] = '|';
        }
        bs[n] = '\0';

        ESP_LOGI("rfcap", "");
        ESP_LOGI("rfcap", "-------- BURST #%03d   step %02d: %s --------", burst_seq_,
                 burst_step_,
                 (burst_step_ >= 1 && burst_step_ <= RFC_NSTEPS) ? RFC_STEPS[burst_step_ - 1]
                                                                 : "(no step set)");
        if (prev_burst_end_ms_)
            ESP_LOGI("rfcap", "  bits=%d  reps=%d  rssi=%ddBm  (+%ums since previous burst)",
                     bits, burst_reps_, radio_.getRSSI(), burst_last_ms_ - prev_burst_end_ms_);
        else
            ESP_LOGI("rfcap", "  bits=%d  reps=%d  rssi=%ddBm", bits, burst_reps_, radio_.getRSSI());
        prev_burst_end_ms_ = burst_last_ms_;

        if (burst_txs_ > 1) {
            std::string g;
            for (int i = 0; i < burst_tx_gap_n_; i++) {
                char b[16];
                snprintf(b, sizeof(b), "%ums ", burst_tx_gaps_[i]);
                g += b;
            }
            ESP_LOGI("rfcap", "  ** this single press keyed the SAME code %d times, %s apart **",
                     burst_txs_, g.c_str());
        }
        ESP_LOGI("rfcap", "  raw   = %s", bs);
        ESP_LOGI("rfcap", "  hex   = 0x%08X%08X", (uint32_t)(f >> 32), (uint32_t)(f & 0xFFFFFFFF));

        if (bits == 30) {
            uint32_t msg29 = (uint32_t)(f >> 1);
            uint32_t addr  = (msg29 >> 9) & 0xFFFFF;
            uint16_t cmd   = (uint16_t)(msg29 & 0x1FF);
            int p          = (int)(f & 1);
            int ones29     = __builtin_popcount(msg29);
            int total      = ones29 + p;
            ESP_LOGI("rfcap", "  addr  = 0x%05X      cmd = 0x%03X", addr, cmd);
            ESP_LOGI("rfcap", "  parity: ones(29)=%d p=%d total=%d -> %s   [odd:%s even:%s]",
                     ones29, p, total, (total & 1) ? "ODD" : "EVEN",
                     (total & 1) ? "OK" : "FAIL", (total & 1) ? "FAIL" : "OK");
        } else {
            ESP_LOGW("rfcap", "  frame is %d bits, not 30 - the 20/9/1 split does not apply", bits);
        }

        if (burst_.short_n)
            ESP_LOGI("rfcap", "  short : n=%u avg=%u min=%u max=%u", burst_.short_n,
                     burst_.short_sum / burst_.short_n, burst_.short_min, burst_.short_max);
        if (burst_.long_n)
            ESP_LOGI("rfcap", "  long  : n=%u avg=%u min=%u max=%u", burst_.long_n,
                     burst_.long_sum / burst_.long_n, burst_.long_min, burst_.long_max);
        if (burst_.gap_n)
            ESP_LOGI("rfcap", "  gap   : n=%u avg=%u min=%u max=%u", burst_.gap_n,
                     burst_.gap_sum / burst_.gap_n, burst_.gap_min, burst_.gap_max);

        // Non-empty 50us bins: shows at a glance whether the marks really are bimodal.
        std::string hist;
        for (int i = 0; i < 44; i++) {
            if (!burst_.hist[i]) continue;
            char b[24];
            snprintf(b, sizeof(b), "%d:%u ", i * 50, burst_.hist[i]);
            hist += b;
        }
        if (!hist.empty()) ESP_LOGI("rfcap", "  marks : %s", hist.c_str());

        if (dropped_) {
            ESP_LOGW("rfcap", "  (dropped %u edges since the last burst - RF noise)", dropped_);
            dropped_ = 0;
        }

        if (dump_raw && burst_raw_n_) {
            ESP_LOGI("rfcap", "  first frame, raw pulses (mark,space alternating, us):");
            std::string line;
            for (int i = 0; i < burst_raw_n_; i++) {
                char b[12];
                snprintf(b, sizeof(b), "%u,", burst_raw_[i]);
                line += b;
                if (line.length() > 180 || i == burst_raw_n_ - 1) {
                    ESP_LOGI("rfcap", "    %s", line.c_str());
                    line.clear();
                }
            }
        }
    }
};

static RfCapture rf_cap(
    CC1101_SCK_PIN,
    CC1101_MISO_PIN,
    CC1101_MOSI_PIN,
    CC1101_CS_PIN,
    CC1101_GDO0_PIN,
    CC1101_GDO2_PIN
);
