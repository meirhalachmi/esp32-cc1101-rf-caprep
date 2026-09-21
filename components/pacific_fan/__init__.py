"""Pacific ceiling fans (433.92 MHz RF remotes) through a CC1101.

One `pacific_fan:` block describes the radio and every fan. Each fan
becomes a fan entity (6 speeds + direction), a dimmable light, buttons for
Timer 1H / Timer 4H / Light colour, and a timer countdown sensor. Breeze is
a preset mode of the fan entity.
"""

import esphome.codegen as cg
from esphome.components import button, fan, light, sensor, switch, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    CONF_NAME,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome import pins

CODEOWNERS = ["@meirhalachmi"]
AUTO_LOAD = ["button", "fan", "light", "sensor", "switch", "text_sensor"]

CONF_SCK_PIN = "sck_pin"
CONF_MISO_PIN = "miso_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_CS_PIN = "cs_pin"
CONF_GDO0_PIN = "gdo0_pin"
CONF_GDO2_PIN = "gdo2_pin"
CONF_FANS = "fans"
CONF_PARITY = "parity"
CONF_DIM_STEPS = "dim_steps"
CONF_LEARN_MODE = "learn_mode"
CONF_SYNC_ONLY = "sync_only"
CONF_LAST_HEARD = "last_heard"
CONF_FAN = "fan"
CONF_LIGHT = "light"
CONF_TIMER_1H = "timer_1h"
CONF_TIMER_4H = "timer_4h"
CONF_COLOUR = "colour"
CONF_TIMER_REMAINING = "timer_remaining"

pacific_fan_ns = cg.esphome_ns.namespace("pacific_fan")
PacificFanRadio = pacific_fan_ns.class_("PacificFanRadio", cg.Component)
PacificRemote = pacific_fan_ns.class_("PacificRemote", cg.Component)
PacificFan = pacific_fan_ns.class_("PacificFan", cg.Component, fan.Fan)
PacificLight = pacific_fan_ns.class_("PacificLight", light.LightOutput)
PacificButton = pacific_fan_ns.class_("PacificButton", button.Button)
PacificSwitch = pacific_fan_ns.class_("PacificSwitch", switch.Switch, cg.Component)

# (config key, default entity name, command, icon)
BUTTONS = [
    (CONF_TIMER_1H, "{} Fan Timer 1H", 0x095, "mdi:timer-outline"),
    (CONF_TIMER_4H, "{} Fan Timer 4H", 0x152, "mdi:timer-outline"),
    (CONF_COLOUR, "{} Light Colour", 0x1D0, "mdi:palette"),
]
DEFAULT_NAMES = {
    CONF_FAN: "{} Fan",
    CONF_LIGHT: "{} Light",
    CONF_TIMER_REMAINING: "{} Fan Timer",
    **{key: name for key, name, _, _ in BUTTONS},
}


def _fill_entity_defaults(value):
    """Give every entity of a fan a name derived from the fan's name."""
    value = cv.Schema({cv.Required(CONF_NAME): cv.string}, extra=cv.ALLOW_EXTRA)(value)
    room = value[CONF_NAME]
    for key, name in DEFAULT_NAMES.items():
        sub = dict(value.get(key) or {})
        sub.setdefault(CONF_NAME, name.format(room))
        value[key] = sub
    # Dimmer steps are driven one press at a time: map brightness linearly
    # onto them and never animate.
    value[CONF_LIGHT].setdefault("gamma_correct", 1.0)
    value[CONF_LIGHT].setdefault("default_transition_length", "0s")
    return value


FAN_ENTRY_SCHEMA = cv.All(
    _fill_entity_defaults,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PacificRemote),
            cv.Required(CONF_NAME): cv.string,
            cv.Required(CONF_ADDRESS): cv.All(cv.hex_uint32_t, cv.Range(max=0xFFFFF)),
            cv.Required(CONF_PARITY): cv.one_of("even", "odd", lower=True),
            cv.Optional(CONF_DIM_STEPS, default=8): cv.int_range(min=2, max=20),
            cv.Required(CONF_FAN): fan.fan_schema(PacificFan),
            cv.Required(CONF_LIGHT): light.light_schema(
                PacificLight, light.LightType.BRIGHTNESS_ONLY
            ),
            cv.Required(CONF_TIMER_REMAINING): sensor.sensor_schema(
                unit_of_measurement="min",
                icon="mdi:timer-sand",
                accuracy_decimals=0,
            ),
            **{
                cv.Required(key): button.button_schema(PacificButton, icon=icon)
                for key, _, _, icon in BUTTONS
            },
        }
    ).extend(cv.COMPONENT_SCHEMA),
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PacificFanRadio),
            cv.Optional(CONF_SCK_PIN, default=18): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_MISO_PIN, default=19): pins.internal_gpio_input_pin_number,
            cv.Optional(CONF_MOSI_PIN, default=23): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_CS_PIN, default=5): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_GDO0_PIN, default=25): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_GDO2_PIN, default=26): pins.internal_gpio_input_pin_number,
            cv.Optional(
                CONF_LEARN_MODE, default={CONF_NAME: "Learn Mode"}
            ): switch.switch_schema(
                PacificSwitch, icon="mdi:school", entity_category=ENTITY_CATEGORY_CONFIG
            ),
            cv.Optional(
                CONF_SYNC_ONLY, default={CONF_NAME: "Sync Only (no RF)"}
            ): switch.switch_schema(
                PacificSwitch, icon="mdi:sync", entity_category=ENTITY_CATEGORY_CONFIG
            ),
            # Learn Mode reports each remote heard here, ready to copy into
            # `fans:`, so finding an address does not need the logs.
            cv.Optional(
                CONF_LAST_HEARD, default={CONF_NAME: "Last Heard"}
            ): text_sensor.text_sensor_schema(
                icon="mdi:access-point", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Optional(CONF_FANS, default=[]): cv.ensure_list(FAN_ENTRY_SCHEMA),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_arduino,
)


async def to_code(config):
    cg.add_library("SPI", None)

    radio = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(radio, config)
    cg.add(
        radio.set_pins(
            config[CONF_SCK_PIN],
            config[CONF_MISO_PIN],
            config[CONF_MOSI_PIN],
            config[CONF_CS_PIN],
            config[CONF_GDO0_PIN],
            config[CONF_GDO2_PIN],
        )
    )

    for kind, key in enumerate((CONF_LEARN_MODE, CONF_SYNC_ONLY)):
        sw = await switch.new_switch(config[key], radio, kind)
        await cg.register_component(sw, config[key])

    last_heard = await text_sensor.new_text_sensor(config[CONF_LAST_HEARD])
    cg.add(radio.set_last_heard(last_heard))

    for entry in config[CONF_FANS]:
        remote = cg.new_Pvariable(
            entry[CONF_ID], radio, entry[CONF_ADDRESS], entry[CONF_PARITY] == "even"
        )
        await cg.register_component(remote, entry)
        cg.add(radio.add_remote(remote))
        cg.add(remote.set_dim_steps(entry[CONF_DIM_STEPS]))

        fan_var = await fan.new_fan(entry[CONF_FAN], remote)
        await cg.register_component(fan_var, entry[CONF_FAN])
        cg.add(remote.set_fan(fan_var))

        await light.new_light(entry[CONF_LIGHT], remote)
        light_state = await cg.get_variable(entry[CONF_LIGHT][CONF_ID])
        cg.add(remote.set_light(light_state))

        timer = await sensor.new_sensor(entry[CONF_TIMER_REMAINING])
        cg.add(remote.set_timer_sensor(timer))

        for key, _, cmd, _ in BUTTONS:
            await button.new_button(entry[key], remote, cmd)
