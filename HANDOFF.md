# Handoff - ceiling fan RF controller

Three Pacific ceiling fans (rooms: **office**, **girls**, **ofek**) on 433.92 MHz
OOK, driven by one ESP32 + CC1101 through ESPHome.

## Status (2026-09-21)

All three rooms are in `esphome_fan_controller.yaml` (OTA at 192.168.1.202,
device `fan-controller`), each one `fan_room.yaml` package:

| Room | Address | Parity (total ones) |
|---|---|---|
| girls | 0xE5D7C | even |
| office | 0xAACAC | odd |
| ofek | 0xB19BC | even |

Girls' room is verified end to end (TX moves the real fan and light, remote
presses update HA without being echoed).  Office and ofek were mapped with
Learn Mode; their command codes match the girls' remote.  A new room is one
more package entry.

## Measured (capture of the girls' remote, every button pressed twice)

- Address `0xE5D7C`.  **Parity: total ones even** for all ~45 frames.  The
  office remote uses odd, so parity is configured per fan, not globally.
- Timings: short mark 375us, long mark 1090us, inter-frame gap ~5.5ms, ~9
  frames per keying.  Every press keys twice, ~200ms apart.
- All 15 buttons are stateless (same code on every press):

  | Button | Cmd | Button | Cmd |
  |---|---|---|---|
  | Power (toggle) | 0x191 | F/R (toggle) | 0x12B |
  | Breeze | 0x10B | Timer 1H | 0x095 |
  | Speed 1-6 | 0x1E8 0x1C8 0x1A9 0x189 0x16A 0x14A | Timer 4H | 0x152 |
  | Light (toggle) | 0x1B1 | Light colour | 0x1D0 |
  | LED- | 0x0F4 | LED+ | 0x133 |

- The colour button is a real command, not a double light press.  A fast
  off/on of the light still changes colour, so HA should not do that.

## Design as built

- Physical state (`*_phys` globals, restored from flash every 5s) is kept
  apart from the HA entities; HA changes are reconciled against it.  Power and
  F/R are toggles, so off and direction changes are only sent when they differ.
  A speed command both sets speed and turns the fan on.
- RX: CC1101 always in RX, ISR on GDO2 into a ring buffer, decoded in a 20ms
  interval.  Frames that fail the address whitelist or the per-fan parity are
  dropped before press detection; a press counts once two identical valid
  frames agree.  RX is detached during TX.
- TX is queued and sent one burst per poll - back-to-back sends from the API
  handler previously starved the loop and tripped the task watchdog.
- **Sync Only (no RF)** switch: HA changes only re-align the physical state.
  Used to calibrate when the tracked state drifts.
- Learn Mode logs every frame, including unknown addresses.

## Open

- Dimmer as a brightness slider with an estimated step (7-8 steps, count not
  certain; resync by overshooting at 0%/100%).
- Minimum interval (~3s) between light commands from HA, so a quick off/on does
  not change the colour.
- Timers turn the fan off without us hearing it; state drifts until the next
  press or a Sync Only calibration.

## Toolchain notes

- ESPHome 2026.9 builds ESP32 with the ESP-IDF toolchain, which rejects the
  CC1101 library's manifest, so the library is vendored in `esphome/cc1101_lib/`.
- WiFi credentials live in `esphome/secrets.yaml` (gitignored).

## Untouched on purpose

`server.ino` and `homeassistant.yaml` are the legacy Arduino build. Leave them.
