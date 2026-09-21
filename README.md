# esp32-cc1101-rf-caprep

Control **Pacific ceiling fans** (433.92 MHz RF remotes) from Home Assistant
with an ESP32 and a CC1101. The fan, light, dimmer, direction and timers all
become Home Assistant entities, and the original remotes keep working: the
controller listens to them and keeps Home Assistant in sync.

<p style="text-align: center;">
    <img src="resources/module.jpeg" width="50%"/>
</p>

## What you get, per fan

| Entity | What it does |
|---|---|
| `fan` | On/off, 6 speeds, direction |
| `light` | On/off and a brightness slider mapped onto the dimmer steps |
| `button` × 4 | Breeze, Timer 1H, Timer 4H, Light colour |
| `sensor` | Minutes left on the fan's own timer |

Plus, for the whole controller: **Learn Mode** and **Last Heard** (find a
remote's address), and **Sync Only** (re-align the tracked state without
transmitting).

## Hardware

- ESP32 dev board
- CC1101 module with a 433 MHz antenna

```
ESP32 GPIO  ->  CC1101
18          ->  SCK
19          ->  MISO (SO)
23          ->  MOSI (SI)
5           ->  CSN
25          ->  GDO0   (TX data)
26          ->  GDO2   (RX data)
3V3 / GND   ->  VCC / GND
```

Any pins can be used; set them in the config (below).

## Quick start

1. Copy [examples/fan-controller.yaml](examples/fan-controller.yaml) and create
   a `secrets.yaml` next to it with `wifi_ssid`, `wifi_password`, `api_key`
   (generate one with `openssl rand -base64 32`) and `ota_password`. Leave
   `fans:` empty (`fans: []`) if you don't know your addresses yet.
2. Flash it: `esphome run fan-controller.yaml`, and add the device in Home
   Assistant (Settings → Devices & Services → ESPHome).
3. **Find each remote's address**: on the device's page in Home Assistant,
   turn on **Learn Mode** and press any button on the remote. The **Last
   Heard** sensor shows exactly what to put in the config:
   ```
   address: 0xE5D7C, parity: even  (Power)
   ```
   Repeat for each remote. (The same thing is in the device log as a `LEARN`
   line.) Turn Learn Mode off when done.
4. Add one entry per fan and flash again:
   ```yaml
   pacific_fan:
     fans:
       - name: Bedroom
         address: 0xE5D7C
         parity: even
   ```
5. **Calibrate once**: the controller cannot ask a fan what it is doing, so
   tell it. Turn on **Sync Only**, set the fan and light entities to match the
   room, turn Sync Only off.

The component is loaded straight from this repository:

```yaml
external_components:
  - source: github://meirhalachmi/esp32-cc1101-rf-caprep
    components: [pacific_fan]
```

## Configuration

```yaml
pacific_fan:
  id: fan_radio          # optional, for id(fan_radio).send(address, command)
  sck_pin: 18            # all pins optional, defaults shown
  miso_pin: 19
  mosi_pin: 23
  cs_pin: 5
  gdo0_pin: 25
  gdo2_pin: 26
  learn_mode:            # optional, rename or hide the switches
    name: Learn Mode
  sync_only:
    name: Sync Only (no RF)
  last_heard:
    name: Last Heard
  fans:
    - name: Bedroom      # prefix for every entity of this fan
      address: 0xE5D7C   # 20-bit remote address, from Learn Mode
      parity: even       # even | odd, from Learn Mode
      dim_steps: 8       # optional, how many steps the dimmer has
      # Every entity can be customised, e.g.:
      # fan:   { name: Bedroom Ceiling Fan, icon: mdi:ceiling-fan }
      # light: { name: Bedroom Ceiling Light }
      # breeze / timer_1h / timer_4h / colour / timer_remaining: { ... }
```

## Dashboard

[examples/dashboard-room-view.yaml](examples/dashboard-room-view.yaml) is a
ready-made Home Assistant view for one room (light, fan with quick actions,
optionally the AC). Replace the entity prefix and paste it under `views:` in
the dashboard's raw configuration editor, once per room.

## How it works

**The protocol.** Each press sends a 30-bit PWM-OOK frame, most significant
bit first: 20-bit address, 9-bit command, 1 check bit. A `1` is a ~1090 µs mark
and a ~375 µs space, a `0` the reverse; frames repeat ~9 times with a ~5.6 ms
gap, and each press is keyed twice ~200 ms apart. The check bit makes the count
of ones even on some remotes and odd on others, hence `parity:` per fan.

| Button | Command | Button | Command |
|---|---|---|---|
| Power (toggle) | `0x191` | F/R (toggle) | `0x12B` |
| Breeze | `0x10B` | Timer 1H | `0x095` |
| Speed 1 | `0x1E8` | Timer 4H | `0x152` |
| Speed 2 | `0x1C8` | Light (toggle) | `0x1B1` |
| Speed 3 | `0x1A9` | Light colour | `0x1D0` |
| Speed 4 | `0x189` | LED- | `0x0F4` |
| Speed 5 | `0x16A` | LED+ | `0x133` |
| Speed 6 | `0x14A` | | |

**Tracked state.** Power, F/R and the light are toggles and the dimmer only
steps, so the controller keeps what it believes each fan is doing (saved to
flash) and sends only what is needed to reach what Home Assistant asks for.
Turning a fan on is done with a speed command, which also switches it on, so
it never depends on guessing the toggle.

**Listening.** The CC1101 stays in receive mode. A press counts once two
identical frames agree, with the right address and check bit; everything else
is dropped. Presses on the original remotes update the entities, and the
controller stops listening while it transmits so it never hears itself.

**Details worth knowing:**
- A quick off/on of the light changes its colour on these fans, so light
  toggles from Home Assistant are kept at least 3 s apart.
- The dimmer has no absolute command. Brightness maps to an estimated step;
  going to 100% or the minimum sends a couple of extra presses so the estimate
  re-anchors.
- The fan's own timer switches it off silently; the controller follows the
  countdown and marks the fan off when it ends (not across a controller reboot).

## Other fans

If a fan does not respond, its remote may use different timings or codes.
[esphome/esphome_rf_capture.yaml](esphome/esphome_rf_capture.yaml) is a
receive-only build that logs every frame with its pulse timings; follow
[CAPTURE_PROCEDURE.md](esphome/CAPTURE_PROCEDURE.md) to map a remote.

## Troubleshooting

- **`CC1101 not detected`** in the log: check the SPI wiring and 3.3 V power.
- **Presses on the remote are not picked up**: turn on Learn Mode and check
  the frames arrive; move the antenna or the controller closer.
- **Home Assistant shows the wrong state**: recalibrate with Sync Only.
- **WiFi fails**: the device opens a fallback access point if you add `ap:`
  under `wifi:` together with `captive_portal:`.

## Credits

- CC1101 driver: [CC1101-ESP-Arduino](https://github.com/wladimir-computin/CC1101-ESP-Arduino)
  (MIT), vendored in `components/pacific_fan` because ESPHome's ESP-IDF build
  rejects its library manifest.

---

## Legacy Arduino capture/replay tool (`server.ino`)

The original general-purpose RF recorder is still included.

### Setup

### Dependencies

1. [CC1101-ESP-Arduino](https://github.com/wladimir-computin/CC1101-ESP-Arduino) — must be installed manually
2. ArduinoJson
3. SPIFFS
4. ESP32 core libraries (WiFi, WebServer, ESPmDNS)

### Installation

1. Install the Arduino IDE and add ESP32 board support
2. Install the required libraries:
   - Clone or download the [CC1101-ESP-Arduino](https://github.com/wladimir-computin/CC1101-ESP-Arduino) library into your Arduino libraries folder
   - Install ArduinoJson through the Arduino Library Manager
3. Open `server.ino`, update WiFi credentials, and upload to ESP32
4. Access the web interface at `http://esp32-rf.local`

### Home Assistant integration

The legacy Arduino sketch exposes a REST API for signal replay. See [homeassistant.yaml](/homeassistant.yaml) for an example configuration.

<p style="text-align: center;">
    <img src="resources/ha.jpeg" width="30%"/>
</p>
