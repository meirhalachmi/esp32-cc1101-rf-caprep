# Handoff - ceiling fan RF controller

Three Pacific ceiling fans (rooms: **office**, **girls**, **ofek**) on 433.92 MHz
OOK, driven by one ESP32 + CC1101 through ESPHome.

## Settled

**Protocol.** 30-bit frame: 20-bit address, 9-bit command, 1 parity bit.
Parity is **odd** over the whole frame; `send_command()` defaults to
`odd_parity = true`. Timings 300us short / 1150us long / 6000us gap, 20 repeats.
Light is `0x1B1`. All applied in `esphome/cc1101_fan_controller.h`.

**Verified commands.** Only Speed 1 (`0x1E8`), Speed 2 (`0x1C8`), Toggle
(`0x191`) and Light (`0x1B1`). Speeds 3-6, Invert and everything else in the
table came from the upstream library, which was already wrong about Light -
treat them as unverified.

**There is no discrete off.** Power is a toggle, so "turn off" from Home
Assistant means sending the toggle conditionally on tracked state. That is why
RX state tracking matters rather than being a nicety.

**The remote has 15 buttons**, not the 9 currently in the table: power, breeze,
speeds 1-6, F/R, 1H, 4H, light, light colour, LED-, LED+.

**Light colour is probably not a command.** The colour temperature changes when
the light is switched off and back on quickly, so the colour button may just key
the light command twice. The capture firmware detects and reports this. If
confirmed, Home Assistant reproduces it by sending the light command twice - and
the light entity then needs a minimum interval between consecutive light
commands, so an ordinary off/on automation does not change the colour by
accident.

## Decided, not yet built

1. **Entities.** Replace the select-plus-buttons layout with a real `fan` entity
   (on/off plus speeds) and a separate `light` entity per room.
2. **RX state tracking.** CC1101 sits in RX by default, interrupt on GDO2.
   Whitelist the three addresses and drop everything else. `Speed N` from a
   physical remote sets the speed **and** marks the fan on; `Toggle` flips
   on/off; `Light` flips the light. Detach the interrupt around TX, reattach
   after the gap window, so the ESP32 does not receive its own transmission.
3. **State survives reboot** - restore from flash. Without it the toggle has
   nothing to reason about.
4. **Raw command sender**: one code box plus a target selector (office / girls /
   ofek) plus one send button, rather than three duplicated buttons.
5. **Learn Mode switch**: while on, log every frame heard including unknown
   addresses, for mapping new buttons; while off, the whitelist filters silently.

## Blocked on capture data

Tasks 1 and 2 cannot be written until the capture runs: the three 20-bit
addresses are unknown, and the command table is mostly unverified. Run
`esphome/CAPTURE_PROCEDURE.md` against `esphome_rf_capture.yaml` and keep the
full log.

## Untouched on purpose

`server.ino` and `homeassistant.yaml` are the legacy Arduino build. Leave them.

## Not compiled

The protocol fixes and the capture firmware have not been through an ESPHome
build - only YAML validation and a read of the CC1101 library source. First
compile happens locally.
