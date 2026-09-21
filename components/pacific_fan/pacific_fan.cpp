#include "pacific_fan.h"

#include <cmath>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "soc/gpio_struct.h"

namespace esphome::pacific_fan {

static const char *const TAG = "pacific_fan";

// ── Measured timings (averages over ~45 captured bursts) ────
static const int SHORT_US = 375;
static const int LONG_US = 1090;
static const int GAP_US = 5600;
static const int TX_REPS = 10;  // the remote sends ~9 per keying

// ── RX decoder tolerances ───────────────────────────────────
static const uint16_t MARK_MIN = 150;
static const uint16_t MARK_SPLIT = 700;  // below = '0', above = '1'
static const uint16_t MARK_MAX = 1800;
static const uint16_t GAP_MIN = 3500;
static const uint32_t PRESS_GAP_MS = 500;  // same code closer than this = same press

// ── Behaviour ───────────────────────────────────────────────
static const int DIM_OVERSHOOT = 2;           // extra presses at either end of the dimmer
static const uint32_t LIGHT_MIN_GAP_MS = 3000;  // a fast off/on changes the light's colour

const char *command_name(uint16_t cmd) {
  switch (cmd) {
    case CMD_TOGGLE: return "Power";
    case CMD_BREEZE: return "Breeze";
    case CMD_REVERSE: return "F/R";
    case CMD_TIMER_1H: return "Timer 1H";
    case CMD_TIMER_4H: return "Timer 4H";
    case CMD_LIGHT: return "Light";
    case CMD_COLOUR: return "Light colour";
    case CMD_DIM_DOWN: return "LED-";
    case CMD_DIM_UP: return "LED+";
  }
  static const char *const SPEEDS[] = {"Speed 1", "Speed 2", "Speed 3", "Speed 4", "Speed 5", "Speed 6"};
  int s = speed_of(cmd);
  return s ? SPEEDS[s - 1] : "?";
}

int speed_of(uint16_t cmd) {
  for (int i = 0; i < 6; i++)
    if (CMD_SPEED[i] == cmd) return i + 1;
  return 0;
}

// ============================================================
// ISR: edge durations off GDO2 into a ring buffer
// ============================================================
// A press is ~1100 edges and the loop may be busy (our own TX, an API
// call), so the ring holds several presses.
static const uint32_t RING = 4096;  // power of two
static volatile uint16_t ring_buf[RING];  // bit15 set = the pulse that ended was a mark
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_last_us = 0;
static uint8_t ring_pin = 0;

// digitalRead() lives in flash and is not safe from an IRAM ISR.
static inline bool IRAM_ATTR read_pin(uint8_t pin) {
  if (pin < 32) return (GPIO.in >> pin) & 0x1;
  return (GPIO.in1.val >> (pin - 32)) & 0x1;
}

static void IRAM_ATTR rx_isr() {
  uint32_t now = micros();
  uint32_t dt = now - ring_last_us;
  ring_last_us = now;
  if (dt > 0x7FFF) dt = 0x7FFF;
  uint16_t v = (uint16_t) dt;
  if (!read_pin(ring_pin)) v |= 0x8000;  // now LOW -> it was a HIGH mark
  ring_buf[ring_head & (RING - 1)] = v;
  ring_head++;
}

// ============================================================
// PacificFanRadio
// ============================================================
void PacificFanRadio::setup() {
  this->radio_ = new CC1101(this->sck_, this->miso_, this->mosi_, this->cs_, this->gdo0_, this->gdo2_);  // NOLINT
  this->radio_->init();
  this->radio_->setMHZ(433.92);
  this->radio_->setTXPwr(TX_PLUS_10_DBM);
  this->radio_->setDataRate(2400);
  this->radio_->setRxBW(RX_BW_162_KHZ);
  this->radio_->setModulation(ASK_OOK);

  uint8_t version = this->radio_->getVersion();
  if (version == 0x00 || version == 0xFF) {
    ESP_LOGE(TAG, "CC1101 not detected (version 0x%02X) - check the wiring", version);
    this->mark_failed();
    return;
  }

  // The library only drives GDO0 as an output when built without GDO2.
  pinMode(this->gdo0_, OUTPUT);
  digitalWrite(this->gdo0_, LOW);
  ring_pin = this->gdo2_;
  pinMode(this->gdo2_, INPUT);

  this->ready_ = true;
  this->start_rx_();
}

void PacificFanRadio::dump_config() {
  ESP_LOGCONFIG(TAG, "Pacific fan radio (CC1101 @ 433.92 MHz OOK)");
  ESP_LOGCONFIG(TAG, "  SCK %d  MISO %d  MOSI %d  CS %d  GDO0 %d  GDO2 %d", this->sck_, this->miso_, this->mosi_,
                this->cs_, this->gdo0_, this->gdo2_);
  if (this->is_failed())
    ESP_LOGE(TAG, "  CC1101 not detected");
}

void PacificFanRadio::start_rx_() {
  this->radio_->setRx();
  ring_last_us = micros();
  this->tail_ = ring_head;
  this->state_ = WAIT_GAP;
  attachInterrupt(digitalPinToInterrupt(this->gdo2_), rx_isr, CHANGE);
}

void PacificFanRadio::send(uint32_t address, uint16_t command) {
  if (!this->ready_) {
    ESP_LOGW(TAG, "Radio not ready - dropping %s", command_name(command));
    return;
  }
  if ((uint8_t) (this->txq_head_ - this->txq_tail_) >= TXQ) {
    ESP_LOGW(TAG, "TX queue full - dropping %s", command_name(command));
    return;
  }
  this->txq_[this->txq_head_++ % TXQ] = {address, command};
}

void PacificFanRadio::transmit_(uint32_t address, uint16_t command) {
  PacificRemote *remote = this->find_(address);
  bool even = remote ? remote->even_parity() : true;

  uint32_t msg = ((address & 0xFFFFF) << 9) | (command & 0x1FF);
  bool odd_ones = __builtin_popcount(msg) & 1;
  bool check = even ? odd_ones : !odd_ones;
  uint32_t frame = (msg << 1) | (check ? 1 : 0);

  // Stop listening so we do not decode our own transmission.
  detachInterrupt(digitalPinToInterrupt(this->gdo2_));
  this->radio_->setTx();
  digitalWrite(this->gdo0_, LOW);
  delayMicroseconds(GAP_US);

  for (int rep = 0; rep < TX_REPS; rep++) {
    {
      InterruptLock lock;
      for (int b = 29; b >= 0; b--) {
        bool one = (frame >> b) & 1;
        digitalWrite(this->gdo0_, HIGH);
        delayMicroseconds(one ? LONG_US : SHORT_US);
        digitalWrite(this->gdo0_, LOW);
        delayMicroseconds(one ? SHORT_US : LONG_US);
      }
    }
    delayMicroseconds(GAP_US);
    App.feed_wdt();
  }

  digitalWrite(this->gdo0_, LOW);
  this->start_rx_();
  ESP_LOGI(TAG, "TX  0x%05X  %s", (unsigned) address, command_name(command));
}

void PacificFanRadio::loop() {
  if (!this->ready_)
    return;
  uint32_t h = ring_head;
  if (h - this->tail_ > RING) {  // overrun: resync on the next gap
    this->tail_ = h - RING;
    this->state_ = WAIT_GAP;
  }
  while (this->tail_ != h) {
    uint16_t v = ring_buf[this->tail_ & (RING - 1)];
    this->tail_++;
    this->feed_(v & 0x7FFF, (v & 0x8000) != 0);
  }
  if (this->txq_tail_ != this->txq_head_) {
    TxItem it = this->txq_[this->txq_tail_++ % TXQ];
    this->transmit_(it.address, it.command);
  }
}

void PacificFanRadio::feed_(uint16_t dur, bool mark) {
  switch (this->state_) {
    case WAIT_GAP:
      if (!mark && dur >= GAP_MIN) {
        this->bits_ = 0;
        this->frame_ = 0;
        this->state_ = EXPECT_MARK;
      }
      return;
    case EXPECT_MARK:
      if (!mark || dur < MARK_MIN || dur > MARK_MAX) {
        this->state_ = WAIT_GAP;
        return;
      }
      this->mark_ = dur;
      this->state_ = EXPECT_SPACE;
      return;
    case EXPECT_SPACE:
      if (mark) {
        this->state_ = WAIT_GAP;
        return;
      }
      this->frame_ = (this->frame_ << 1) | (this->mark_ > MARK_SPLIT ? 1 : 0);
      this->bits_++;
      if (dur >= GAP_MIN) {
        if (this->bits_ == 30)
          this->on_frame_(this->frame_);
        this->bits_ = 0;  // the next repeat starts right here
        this->frame_ = 0;
      } else if (this->bits_ > 30) {
        this->state_ = WAIT_GAP;
        return;
      }
      this->state_ = EXPECT_MARK;
      return;
  }
}

PacificRemote *PacificFanRadio::find_(uint32_t address) {
  for (auto *r : this->remotes_)
    if (r->address() == (address & 0xFFFFF))
      return r;
  return nullptr;
}

void PacificFanRadio::on_frame_(uint32_t f) {
  uint32_t address = (f >> 10) & 0xFFFFF;
  uint16_t cmd = (f >> 1) & 0x1FF;
  bool total_even = !(__builtin_popcount(f) & 1);
  PacificRemote *remote = this->find_(address);
  bool valid = remote != nullptr && total_even == remote->even_parity();
  uint32_t now = millis();

  if (this->learn_mode) {
    bool repeat = f == this->learn_last_ && now - this->learn_last_ms_ < PRESS_GAP_MS;
    this->learn_last_ = f;
    this->learn_last_ms_ = now;
    if (!repeat)
      ESP_LOGI(TAG, "LEARN address=0x%05X parity=%s command=0x%03X (%s)%s", (unsigned) address,
               total_even ? "even" : "odd", cmd, command_name(cmd),
               remote ? (valid ? "" : "  [known address, wrong parity]") : "  [unknown address]");
  }
  // Only valid frames take part in press detection: a corrupted frame, or
  // another fan's remote, must not reset a press that is still arriving.
  if (!valid)
    return;

  if (f == this->cand_ && now - this->cand_last_ms_ < PRESS_GAP_MS) {
    this->cand_n_++;
  } else {
    this->cand_ = f;
    this->cand_n_ = 1;
  }
  this->cand_last_ms_ = now;
  if (this->cand_n_ != 2)
    return;

  ESP_LOGI(TAG, "RX  0x%05X  %s", (unsigned) address, command_name(cmd));
  remote->on_rx(cmd);
}

// ============================================================
// PacificRemote
// ============================================================
void PacificRemote::setup() {
  this->pref_ = global_preferences->make_preference<Phys>(fnv1_hash("pacific_fan_" + to_string(this->address_)));
  Phys saved;
  if (this->pref_.load(&saved)) {
    this->phys_ = saved;
    if (this->phys_.speed < 1 || this->phys_.speed > 6) this->phys_.speed = 1;
    if (this->phys_.level < 1 || this->phys_.level > this->dim_steps_) this->phys_.level = this->dim_steps_;
  }
  // The physical state is the truth: show it, whatever the entities restored.
  this->ready_ = true;
  this->publish_fan_();
  this->publish_light_();

  if (this->timer_sensor_ != nullptr)
    this->set_interval("timer_sensor", 30000, [this]() {
      if (this->timer_end_ms_ == 0) {
        this->timer_sensor_->publish_state(0);
        return;
      }
      int32_t left = (int32_t) (this->timer_end_ms_ - millis());
      this->timer_sensor_->publish_state(left > 0 ? (left + 59999) / 60000 : 0);
    });
}

void PacificRemote::dump_config() {
  ESP_LOGCONFIG(TAG, "Pacific fan remote 0x%05X (%s parity), %d dimmer steps", (unsigned) this->address_,
                this->even_parity_ ? "even" : "odd", this->dim_steps_);
}

void PacificRemote::loop() {
  // The fan's own timer switches it off without a word on the air.
  if (this->timer_end_ms_ != 0 && (int32_t) (millis() - this->timer_end_ms_) >= 0) {
    this->timer_end_ms_ = 0;
    this->phys_.fan_on = false;
    this->save_();
    this->publish_fan_();
    if (this->timer_sensor_ != nullptr)
      this->timer_sensor_->publish_state(0);
  }
}

void PacificRemote::publish_fan_() {
  if (this->fan_ == nullptr)
    return;
  this->fan_->state = this->phys_.fan_on;
  this->fan_->speed = this->phys_.speed;
  this->fan_->direction = this->phys_.reverse ? fan::FanDirection::REVERSE : fan::FanDirection::FORWARD;
  this->fan_->publish_state();
}

void PacificRemote::publish_light_() {
  if (this->light_ == nullptr)
    return;
  // Record it as what we want too, so the write it triggers sends nothing.
  this->want_light_on_ = this->phys_.light_on;
  this->want_brightness_ = (float) this->phys_.level / this->dim_steps_;
  auto call = this->light_->make_call();
  call.set_state(this->phys_.light_on);
  if (this->phys_.light_on)
    call.set_brightness(this->want_brightness_);
  call.set_transition_length(0);
  call.perform();
}

void PacificRemote::start_timer_(int hours) {
  this->timer_end_ms_ = millis() + (uint32_t) hours * 3600000UL;
  if (this->timer_sensor_ != nullptr)
    this->timer_sensor_->publish_state(hours * 60);
}

void PacificRemote::on_rx(uint16_t cmd) {
  int speed = speed_of(cmd);
  bool fan_changed = true;
  bool light_changed = true;

  if (speed) {
    this->phys_.fan_on = true;
    this->phys_.speed = speed;
  } else if (cmd == CMD_TOGGLE) {
    this->phys_.fan_on = !this->phys_.fan_on;
  } else if (cmd == CMD_BREEZE) {
    this->phys_.fan_on = true;
  } else if (cmd == CMD_REVERSE) {
    this->phys_.reverse = !this->phys_.reverse;
  } else {
    fan_changed = false;
  }

  if (cmd == CMD_LIGHT) {
    this->phys_.light_on = !this->phys_.light_on;
  } else if (cmd == CMD_DIM_UP && this->phys_.light_on) {
    this->phys_.level = std::min<int>(this->dim_steps_, this->phys_.level + 1);
  } else if (cmd == CMD_DIM_DOWN && this->phys_.light_on) {
    this->phys_.level = std::max<int>(1, this->phys_.level - 1);
  } else {
    light_changed = false;
  }

  if (cmd == CMD_TIMER_1H)
    this->start_timer_(1);
  else if (cmd == CMD_TIMER_4H)
    this->start_timer_(4);
  if (!this->phys_.fan_on)
    this->timer_end_ms_ = 0;

  this->save_();
  if (fan_changed)
    this->publish_fan_();
  if (light_changed)
    this->publish_light_();
}

void PacificRemote::press(uint16_t cmd) {
  this->send_(cmd);
  if (cmd == CMD_BREEZE) {
    this->phys_.fan_on = true;
    this->save_();
    this->publish_fan_();
  } else if (cmd == CMD_TIMER_1H) {
    this->start_timer_(1);
  } else if (cmd == CMD_TIMER_4H) {
    this->start_timer_(4);
  }
}

void PacificRemote::fan_control(bool on, int speed, bool reverse) {
  if (!this->ready_)
    return;
  if (speed < 1 || speed > 6)
    speed = this->phys_.speed;
  if (!on)
    this->timer_end_ms_ = 0;

  if (!this->radio_->sync_only) {
    if (reverse != this->phys_.reverse)
      this->send_(CMD_REVERSE);
    if (on && (!this->phys_.fan_on || speed != this->phys_.speed)) {
      // A speed command also switches the fan on, so it covers both "turn
      // on" and "change speed" without relying on the toggle.
      this->send_(CMD_SPEED[speed - 1]);
    } else if (!on && this->phys_.fan_on) {
      this->send_(CMD_TOGGLE);
    }
  }
  this->phys_.fan_on = on;
  this->phys_.speed = speed;
  this->phys_.reverse = reverse;
  this->save_();
  this->publish_fan_();
}

void PacificRemote::light_control(bool on, float brightness) {
  if (!this->ready_)
    return;
  this->want_light_on_ = on;
  this->want_brightness_ = brightness;
  // Keep light toggles apart: a later request replaces a pending one, so a
  // quick off-then-on collapses instead of flicking the colour.
  uint32_t since = millis() - this->light_tx_ms_;
  uint32_t wait = this->light_tx_ms_ && since < LIGHT_MIN_GAP_MS ? LIGHT_MIN_GAP_MS - since : 0;
  this->set_timeout("light", wait, [this]() { this->light_apply_(); });
}

void PacificRemote::light_apply_() {
  bool on = this->want_light_on_;
  int step = std::max(1, std::min(this->dim_steps_, (int) lroundf(this->want_brightness_ * this->dim_steps_)));

  if (this->radio_->sync_only) {
    this->phys_.light_on = on;
    if (on)
      this->phys_.level = step;
    this->save_();
    return;
  }
  if (on != this->phys_.light_on) {
    this->send_(CMD_LIGHT);
    this->phys_.light_on = on;
    this->light_tx_ms_ = millis();
  }
  if (on && step != this->phys_.level) {
    int cur = this->phys_.level;
    // At either end, overshoot so the estimate re-anchors to reality.
    int n = step == this->dim_steps_ ? this->dim_steps_ - cur + DIM_OVERSHOOT
            : step == 1              ? cur - 1 + DIM_OVERSHOOT
                                     : std::abs(step - cur);
    uint16_t cmd = step > cur ? CMD_DIM_UP : CMD_DIM_DOWN;
    for (int i = 0; i < n; i++)
      this->send_(cmd);
    this->phys_.level = step;
  }
  this->save_();
}

// ============================================================
// Entities
// ============================================================
void PacificFan::control(const fan::FanCall &call) {
  bool on = call.get_state().value_or(this->state);
  int speed = call.get_speed().value_or(this->speed);
  auto dir = call.get_direction().value_or(this->direction);
  this->remote_->fan_control(on, speed, dir == fan::FanDirection::REVERSE);
}

light::LightTraits PacificLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
  return traits;
}

void PacificLight::write_state(light::LightState *state) {
  bool on = state->current_values.is_on();
  float brightness = state->current_values.get_brightness();
  this->remote_->light_control(on, brightness);
}

void PacificSwitch::write_state(bool state) {
  if (this->kind_ == LEARN_MODE)
    this->radio_->learn_mode = state;
  else
    this->radio_->sync_only = state;
  this->publish_state(state);
}

}  // namespace esphome::pacific_fan
