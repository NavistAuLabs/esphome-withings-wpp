"""BLE client for the Withings Body Smart scale's WPP (Withings Proprietary
Protocol): association handshake, then a full stored-measurement sync.

Frame/TLV layout and the auth sequence (CMD_PROBE -> CMD_PROBE_CHALLENGE ->
answer with SHA1(challenge || mac || kl) -> CMD_PROBE reply) are verified
against a real captured exchange with a real scale: the capture decodes
byte-for-byte against this exact framing, and the SHA1 answer it contains
reproduces the device's own captured bytes when recomputed with the real `kl`
-- see withings_wpp.cpp's header comment for specifics. The component README
has the wire format, the error codes, and the traps this protocol sets (BLE not
Classic, bonding vs encryption, where `kl` comes from).

Once authenticated: CMD_TIME_SET, CMD_CONNECT_REASON, then
CMD_STORED_MEASURE{GETSTATE, GETALL} -- read-only, never DELALL, so the
scale's own store and its cloud sync are untouched. Every raw WPP frame
received during that sync is appended verbatim to the SD card before decode
and promoted into place only once the scale acknowledges the mandatory
CMD_SYNC_OK -- see withings_wpp.cpp's
write_session_frame_()/promote_session_file_(). Every terminal state (success,
protocol error, reply timeout) disconnects; see withings_wpp.cpp's finish_().

Takes an existing `ble_client` rather than defining its own, because the
passkey pairing automations that bond the scale are config-side and belong on
that entry. The component drives the link itself from there: it listens for
the scale's advertisement (the scale only advertises after a weigh-in, so the
advertisement IS the event), calls connect on the absent->present edge, and
speaks WPP from ESP_GATTC_SEARCH_CMPL_EVT onward. A `ble_client.connect`
action fired from elsewhere -- a button, say -- starts a run just as well.
"""

import esphome.codegen as cg
from esphome.components import ble_client, ble_device_base, sensor, text_sensor
from esphome.components import time as time_
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_TIME_ID,
    DEVICE_CLASS_WEIGHT,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_KILOGRAM,
    UNIT_SECOND,
)

DEPENDENCIES = ["ble_client", "time"]

CONF_ASSOCIATION_KEY = "association_key"
CONF_WEIGHT = "weight"
CONF_FAT_MASS = "fat_mass"
CONF_MUSCLE_MASS = "muscle_mass"
CONF_HYDRATION = "hydration"
CONF_BONE_MASS = "bone_mass"
CONF_DRIFT = "drift"
CONF_CONNECT_REASON = "connect_reason"
CONF_LAST_SYNC = "last_sync"

withings_wpp_ns = cg.esphome_ns.namespace("withings_wpp")
WithingsWpp = withings_wpp_ns.class_(
    "WithingsWpp", cg.Component, ble_client.BLEClientNode
)


def validate_association_key(value):
    # `kl` is a verified protocol fact (32 ASCII chars) -- catching a
    # mis-pasted secret here is a compile error with a clear message instead
    # of a confusing runtime AUTH_ERR (-6) days later.
    value = cv.string_strict(value)
    if len(value) != 32:
        raise cv.Invalid(
            f"{CONF_ASSOCIATION_KEY} (the account's WPP 'kl' secret) must be exactly "
            f"32 ASCII characters, got {len(value)}"
        )
    return value


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WithingsWpp),
            # The scale's per-account association secret ("kl" in Withings/WPP
            # terminology and throughout the protocol docs/comments here).
            # Supplied via !secret from whatever secret store you use.
            cv.Required(CONF_ASSOCIATION_KEY): validate_association_key,
            # Required, not GenerateID-autodetected: a device that configures
            # more than one time platform (homeassistant + sntp, say) would
            # make autodetection ambiguous -- and CMD_TIME_SET/the session
            # filename both need real time, so silently defaulting to utc=0 is
            # the wrong failure mode for a missing config.
            cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_WEIGHT): sensor.sensor_schema(
                unit_of_measurement=UNIT_KILOGRAM,
                device_class=DEVICE_CLASS_WEIGHT,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=2,
            ),
            cv.Optional(CONF_FAT_MASS): sensor.sensor_schema(
                unit_of_measurement=UNIT_KILOGRAM,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=2,
            ),
            cv.Optional(CONF_MUSCLE_MASS): sensor.sensor_schema(
                unit_of_measurement=UNIT_KILOGRAM,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=2,
            ),
            cv.Optional(CONF_HYDRATION): sensor.sensor_schema(
                unit_of_measurement=UNIT_KILOGRAM,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=2,
            ),
            cv.Optional(CONF_BONE_MASS): sensor.sensor_schema(
                unit_of_measurement=UNIT_KILOGRAM,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=2,
            ),
            # How far the scale's own clock was off at TimeSetReply -- bounds
            # how far each measurement's `time` field can be trusted.
            cv.Optional(CONF_DRIFT): sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:clock-alert-outline",
            ),
            cv.Optional(CONF_CONNECT_REASON): text_sensor.text_sensor_schema(
                icon="mdi:gesture-tap-button",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # session-utc of the last sync that completed with a scale-side
            # CMD_SYNC_OK ack -- device-side signal that a sync actually
            # finished, not just that a connection happened.
            cv.Optional(CONF_LAST_SYNC): text_sensor.text_sensor_schema(
                icon="mdi:clock-check-outline",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    # Also a BLE_DEVICE, not just a BLE client: the component listens for the
    # scale's advertisement (which is the weigh-in event) as well as talking
    # GATT to it. register_ble_device() needs ble_hub_id from this schema.
    .extend(ble_device_base.BLE_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    # Advertisement listener as well as GATT client: the scale only advertises
    # after a weigh-in, so being armed is the only way to catch one.
    await ble_device_base.register_ble_device(var, config)
    cg.add(var.set_association_key(config[CONF_ASSOCIATION_KEY]))

    time_var = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time_id(time_var))

    if weight_config := config.get(CONF_WEIGHT):
        sens = await sensor.new_sensor(weight_config)
        cg.add(var.set_weight_sensor(sens))
    if fat_mass_config := config.get(CONF_FAT_MASS):
        sens = await sensor.new_sensor(fat_mass_config)
        cg.add(var.set_fat_mass_sensor(sens))
    if muscle_mass_config := config.get(CONF_MUSCLE_MASS):
        sens = await sensor.new_sensor(muscle_mass_config)
        cg.add(var.set_muscle_mass_sensor(sens))
    if hydration_config := config.get(CONF_HYDRATION):
        sens = await sensor.new_sensor(hydration_config)
        cg.add(var.set_hydration_sensor(sens))
    if bone_mass_config := config.get(CONF_BONE_MASS):
        sens = await sensor.new_sensor(bone_mass_config)
        cg.add(var.set_bone_mass_sensor(sens))
    if drift_config := config.get(CONF_DRIFT):
        sens = await sensor.new_sensor(drift_config)
        cg.add(var.set_drift_sensor(sens))
    if connect_reason_config := config.get(CONF_CONNECT_REASON):
        ts = await text_sensor.new_text_sensor(connect_reason_config)
        cg.add(var.set_connect_reason_text_sensor(ts))
    if last_sync_config := config.get(CONF_LAST_SYNC):
        ts = await text_sensor.new_text_sensor(last_sync_config)
        cg.add(var.set_last_sync_text_sensor(ts))
