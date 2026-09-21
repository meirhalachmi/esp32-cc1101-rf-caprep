#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/button/button.h"
#include "esphome/components/fan/fan.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "cc1101_esp_arduino.h"

// ============================================================
// Pacific ceiling fans over a CC1101 at 433.92 MHz PWM-OOK.
//
// Frame: 30 bits, MSB first = 20-bit address | 9-bit command | 1 check bit.
// Each bit is one mark + one space: '1' = long mark + short space,
// '0' = short mark + long space.  Frames repeat with a ~5.5ms gap.
//
// The last bit makes the count of ones even on some remotes and odd on
// others, so it is configured per remote and used to reject corrupt frames.
// ============================================================

namespace esphome::pacific_fan {

// ── Command table, measured on the original remote ──────────
static const uint16_t CMD_TOGGLE = 0x191;  // power: toggles, no discrete off
static const uint16_t CMD_BREEZE = 0x10B;
static const uint16_t CMD_SPEED[6] = {0x1E8, 0x1C8, 0x1A9, 0x189, 0x16A, 0x14A};
static const uint16_t CMD_REVERSE = 0x12B;  // F/R: toggles
static const uint16_t CMD_TIMER_1H = 0x095;
static const uint16_t CMD_TIMER_4H = 0x152;
static const uint16_t CMD_LIGHT = 0x1B1;  // toggles the light
static const uint16_t CMD_COLOUR = 0x1D0;
static const uint16_t CMD_DIM_DOWN = 0x0F4;
static const uint16_t CMD_DIM_UP = 0x133;

const char *command_name(uint16_t cmd);
int speed_of(uint16_t cmd);  // 1..6 for a speed command, else 0

class PacificRemote;

// ============================================================
// The CC1101: always listening, transmits from a queue.
// ============================================================
class PacificFanRadio : public Component {
 public:
  void set_pins(int sck, int miso, int mosi, int cs, int gdo0, int gdo2) {
    sck_ = sck; miso_ = miso; mosi_ = mosi; cs_ = cs; gdo0_ = gdo0; gdo2_ = gdo2;
  }
  void add_remote(PacificRemote *remote) { remotes_.push_back(remote); }
  void set_last_heard(text_sensor::TextSensor *sensor) { last_heard_ = sensor; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  bool is_ready() const { return ready_; }
  // Queue a burst; sent one per loop so a batch cannot starve the loop.
  void send(uint32_t address, uint16_t command);

  bool learn_mode{false};
  bool sync_only{false};

 protected:
  void transmit_(uint32_t address, uint16_t command);
  void start_rx_();
  void feed_(uint16_t duration, bool mark);
  void on_frame_(uint32_t frame);
  PacificRemote *find_(uint32_t address);

  int sck_, miso_, mosi_, cs_, gdo0_, gdo2_;
  CC1101 *radio_{nullptr};
  bool ready_{false};
  std::vector<PacificRemote *> remotes_;
  text_sensor::TextSensor *last_heard_{nullptr};

  struct TxItem { uint32_t address; uint16_t command; };
  static const uint8_t TXQ = 32;
  TxItem txq_[TXQ];
  uint8_t txq_head_{0}, txq_tail_{0};

  uint32_t tail_{0};
  enum { WAIT_GAP, EXPECT_MARK, EXPECT_SPACE } state_{WAIT_GAP};
  uint16_t mark_{0};
  uint32_t frame_{0};
  int bits_{0};

  // A press is two keyings of ~9 identical frames.  A code counts once two
  // valid frames agree, then its copies are swallowed until it goes quiet.
  uint32_t cand_{0};
  int cand_n_{0};
  uint32_t cand_last_ms_{0};
  uint32_t learn_last_{0};
  uint32_t learn_last_ms_{0};
};

class PacificFan;

// ============================================================
// One remote = one fan + its light.  Keeps what the physical fan is
// believed to be doing, since power, F/R and the light are toggles.
// ============================================================
class PacificRemote : public Component {
 public:
  PacificRemote(PacificFanRadio *radio, uint32_t address, bool even_parity)
      : radio_(radio), address_(address), even_parity_(even_parity) {}

  void set_fan(PacificFan *fan) { fan_ = fan; }
  void set_light(light::LightState *light) { light_ = light; }
  void set_timer_sensor(sensor::Sensor *sensor) { timer_sensor_ = sensor; }
  void set_dim_steps(int steps) { dim_steps_ = steps; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  uint32_t address() const { return address_; }
  bool even_parity() const { return even_parity_; }

  // A press heard from this remote.
  void on_rx(uint16_t cmd);
  // Home Assistant asked for this fan / light state.
  void fan_control(bool on, int speed, bool reverse);
  void light_control(bool on, float brightness);
  // A button entity: send the command and track what it implies.
  void press(uint16_t cmd);

 protected:
  struct Phys {
    bool fan_on;
    uint8_t speed;
    bool reverse;
    bool light_on;
    uint8_t level;
  } __attribute__((packed));

  void send_(uint16_t cmd) { radio_->send(address_, cmd); }
  void light_apply_();
  void start_timer_(int hours);
  void publish_fan_();
  void publish_light_();
  void save_() { pref_.save(&phys_); }

  PacificFanRadio *radio_;
  uint32_t address_;
  bool even_parity_;
  PacificFan *fan_{nullptr};
  light::LightState *light_{nullptr};
  sensor::Sensor *timer_sensor_{nullptr};
  int dim_steps_{8};

  Phys phys_{false, 1, false, false, 8};
  ESPPreferenceObject pref_;
  bool ready_{false};

  bool want_light_on_{false};
  float want_brightness_{1.0f};
  uint32_t light_tx_ms_{0};
  uint32_t timer_end_ms_{0};  // 0 = no timer running
};

class PacificFan : public Component, public fan::Fan {
 public:
  explicit PacificFan(PacificRemote *remote) : remote_(remote) {}
  fan::FanTraits get_traits() override { return fan::FanTraits(false, true, true, 6); }

 protected:
  void control(const fan::FanCall &call) override;
  PacificRemote *remote_;
};

class PacificLight : public light::LightOutput {
 public:
  explicit PacificLight(PacificRemote *remote) : remote_(remote) {}
  light::LightTraits get_traits() override;
  void write_state(light::LightState *state) override;

 protected:
  PacificRemote *remote_;
};

class PacificButton : public button::Button {
 public:
  PacificButton(PacificRemote *remote, uint16_t cmd) : remote_(remote), cmd_(cmd) {}

 protected:
  void press_action() override { remote_->press(cmd_); }
  PacificRemote *remote_;
  uint16_t cmd_;
};

class PacificSwitch : public switch_::Switch, public Component {
 public:
  enum Kind { LEARN_MODE, SYNC_ONLY };
  PacificSwitch(PacificFanRadio *radio, int kind) : radio_(radio), kind_(kind) {}
  void setup() override { this->publish_state(false); }

 protected:
  void write_state(bool state) override;
  PacificFanRadio *radio_;
  int kind_;
};

}  // namespace esphome::pacific_fan
