# esphome-withings-wpp

[![License: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/license-PolyForm%20Noncommercial%201.0.0-blue.svg)](LICENSE)

`withings_wpp` — an ESPHome external component that reads weigh-ins off a
**Withings Body Smart** scale over Bluetooth Low Energy, on your own hardware,
with no cloud round trip.

It speaks WPP (Withings Proprietary Protocol) against the scale's single GATT
characteristic: it authenticates with the association secret your account
already holds, drives the scale's own stored-measurement read, publishes the
latest weigh-in as ESPHome sensors, and writes every raw protocol frame to an
SD card so a decode bug never costs you the bytes.

**It is non-destructive.** Nothing is reset, re-associated or deleted. The
scale stays on its cloud account, keeps uploading over Wi-Fi, and the Home
Assistant Withings integration keeps working. This is a second reader of data
the scale is already storing — the `DELALL` command that would clear that
store is deliberately not even given a constant in the source, so it cannot be
reached for by mistake.

## Installation

### Requirements

- **A Withings account that logs in with an email address and a password.**
  The `kl` association secret this component needs is only served by the legacy
  account API, and that API authenticates with email and password. **A
  Sign-in-with-Apple or Google account cannot retrieve it** until you give the
  account a password. Converting one takes a few minutes and disturbs neither
  the scale nor an existing Home Assistant integration:
  1. In your Withings account settings, set the account email address to one
     you can receive mail at.
  2. Log out of Health Mate.
  3. Run the password-reset flow on that address and choose a password.
  4. The account now has an email-and-password login. OAuth refresh tokens
     survive the change, so anything already connected keeps working.
- **`esp32_ble` with `io_capability: keyboard_display`.** The scale
  authenticates BLE pairing with a six-digit passkey shown on its own display.
  The default `none` can only negotiate Just Works, and pairing then fails in
  about four milliseconds.
- **`esp32_ble_tracker`, and a `ble_client` entry for the scale.** The
  advertisement is the trigger, so the component has to be listening as well as
  connecting.
- **A `time` source.** The scale is told the time on every connect, and the
  capture filename is a UTC timestamp. `time_id` is a required option rather
  than auto-detected: silently defaulting to `utc=0` is the wrong failure mode.
- **A recent ESPHome.** Older versions fail the build on APIs this component
  uses; there is no compatibility shim.
- **An SD card, if you want the raw captures.** Optional in practice: with no
  card, the component logs a warning and syncs anyway, so you lose the captures
  and keep the sensors.

### 1. Retrieve your account's `kl` secret

`kl` is the association secret shared between the scale and its Withings
account — without it the scale will not answer a BLE peer at all. It is
issued to your own account, so retrieving it is an account holder reading
their own credential. It is fetched exactly **once**; after that the device
has no cloud dependency at all.

```sh
python3 scripts/get_kl.py --email you@example.com
python3 scripts/get_kl.py --email you@example.com --mac AA:BB:CC:DD:EE:FF
```

The script logs in to the legacy account API, lists your own device
associations, and prints the 32-character `kl` for each. It is read-only: it
writes nothing to the account and sends nothing to the scale. The password can
come from `--password`, `$WITHINGS_PASSWORD`, or the interactive prompt; only
its MD5 digest leaves the machine, because that is what the API expects.

Put the value in your ESPHome `secrets.yaml`:

```yaml
withings_scale_kl: "0123456789abcdef0123456789abcdef"
```

This puts `kl` at rest in device flash. Its blast radius is small — read
access to one scale's stored measurements, not to your account — and the
component validates the length at compile time, so a mis-paste is a build
error, not a confusing auth failure days later.

### 2. Add the component to your device configuration

```yaml
external_components:
  - source: github://NavistAuLabs/esphome-withings-wpp@v0.1.0
    components: [withings_wpp]
  # SD card mount for the raw captures. Pinned at the commit upstream's v0.2.0
  # tag points at: a branch moves, and a tag can be repointed.
  - source: github://n-serrette/esphome_sd_card@889073e052275aeb9e826b697efe3bbbe09f935d
    components: [sd_mmc_card]

esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      # FATFS long filenames are off by default, which caps every path at 8.3
      # and fails the 14-character session filenames with EINVAL.
      CONFIG_FATFS_LFN_HEAP: "y"
      CONFIG_FATFS_MAX_LFN: "255"
    advanced:
      # Since ESPHome 2026.2.0 the built-in ESP-IDF components are excluded
      # from the build, and sd_mmc_card needs fatfs plus these VFS features.
      include_builtin_idf_components: [fatfs]
      disable_vfs_support_termios: false
      disable_vfs_support_select: false
      disable_vfs_support_dir: false

esp32_ble:
  # The scale shows a six-digit passkey on its own display; the default
  # io_capability of "none" can only offer Just Works and pairing fails.
  io_capability: keyboard_display

esp32_ble_tracker:
  scan_parameters:
    # window MUST be shorter than interval. At window == interval the radio
    # scans at 100% duty and never yields, leaving no airtime to establish an
    # outbound connection -- connects then fail intermittently with
    # ESP_GATT_CONN_CONN_CANCEL (0x100) followed by OPEN status=133.
    interval: 1100ms
    window: 300ms
    active: true

time:
  - platform: sntp
    id: my_time

# The pairing rig below is driven from the device's own web UI.
web_server:
  version: 2
  auth:
    username: !secret web_username
    password: !secret web_password
    type: digest

sd_mmc_card:
  id: sd_card
  # Pins are board-specific -- these are an Olimex ESP32-POE-ISO wired for
  # 1-bit mode. Use your own board's SD pinout.
  mode_1bit: true
  clk_pin: GPIO14
  cmd_pin: GPIO15
  data0_pin: GPIO2
  # Auto-formatting on a mount glitch would destroy captured data.
  format_if_mount_failed: false

ble_client:
  - mac_address: "AA:BB:CC:DD:EE:FF"   # placeholder -- the component re-points it
    id: withings_scale
    auto_connect: false                # the component drives the connection
    on_passkey_request:
      then:
        # The scale renders its code only once this request arrives, so the
        # reply must not fire immediately -- it would send whatever stale value
        # the number holds (0 on a fresh boot) and fail instantly. Hold the
        # request open and wait for the code to be set. SMP times out at about
        # 30s, so the window is short but workable.
        - logger.log: "PASSKEY REQUESTED - read the code off the scale now"
        - number.set:
            id: withings_passkey
            value: 0
        - wait_until:
            condition:
              lambda: "return id(withings_passkey).state > 0;"
            timeout: 28s
        - ble_client.passkey_reply:
            id: withings_scale
            passkey: !lambda "return (uint32_t) id(withings_passkey).state;"
    on_numeric_comparison_request:
      then:
        - ble_client.numeric_comparison_reply:
            id: withings_scale
            accept: true

number:
  - platform: template
    name: "Withings Passkey"
    id: withings_passkey
    optimistic: true
    restore_value: true
    initial_value: 0
    min_value: 0
    max_value: 999999
    step: 1
    mode: box
    entity_category: config

button:
  - platform: template
    name: "Withings Pair Now"
    on_press:
      - ble_client.disconnect: withings_scale
      - delay: 2s
      - ble_client.connect: withings_scale

withings_wpp:
  ble_client_id: withings_scale
  association_key: !secret withings_scale_kl
  time_id: my_time
  weight:         { name: "Withings Weight" }
  fat_mass:       { name: "Withings Fat Mass" }
  muscle_mass:    { name: "Withings Muscle Mass" }
  hydration:      { name: "Withings Hydration" }
  bone_mass:      { name: "Withings Bone Mass" }
  drift:          { name: "Withings Clock Drift" }
  connect_reason: { name: "Withings Connect Reason" }
  last_sync:      { name: "Withings Last Sync" }
```

The `mac_address` on `ble_client` is a formality — ESPHome requires one, but
the component identifies the scale by its service UUID and re-points the
client at the observed address on sight, first boot included. Any placeholder
works; there is nothing to look up and nothing to keep updated.

The pairing rig is **not temporary**. Leave it in place: it is the
re-onboarding path if the bond is ever lost.

### 3. Pair the scale

The scale requires an encrypted, authenticated link, which means a one-time BLE
bond with a passkey it renders on its own display — and it renders that code
*only* once a peer asks to pair.

1. **Put the scale in setup mode**: hold the button on the back for 3 seconds.
2. **Open the ESPHome device's web interface** (the `web_server` page — the
   scale itself has no UI beyond its display) and **press "Withings Pair
   Now"**. That is the ESP asking to pair; watch for the `PASSKEY REQUESTED`
   log line.
3. **Read the six-digit code** off the scale's display.
4. **Enter it into "Withings Passkey"** on the same page. Setting the number
   releases the held passkey request and completes the bond. If the 28-second
   window lapsed first, press "Withings Pair Now" again and repeat from step 3.
5. **Let the scale return to normal mode.** Weigh-in syncs only work in normal
   mode — setup mode advertises a zero identity node, so identity recovery is
   disabled there.

You pair once: the bond survives reflashing (a full chip erase is the
exception). Everything else about the link — encryption on every connect —
the component handles itself.

### 4. Confirm a weigh-in arrives

Stand on the scale. It advertises for roughly 55 seconds after a weigh-in, the
component connects on sight, and a full authenticate-read-release cycle takes
about three seconds. You should see the sensors update, and — if a card is
mounted — a session file promoted out of `.partial/`.

Nothing here polls, and nothing can wake the scale remotely: if nobody stands
on it, there is nothing to connect to. A weigh-in that is missed is not lost,
though — the scale re-serves its whole store on the next connect.

## Configuration

| Option | Type | Default | Notes |
| --- | --- | --- | --- |
| `association_key` | string | **required** | The account's `kl`. Exactly 32 ASCII characters, validated at compile time. Use `!secret`. |
| `time_id` | ID | **required** | A `time` platform. Used for `CMD_TIME_SET` and the capture filename. Required rather than auto-detected, because a device with two time platforms would make detection ambiguous. |
| `ble_client_id` | ID | generated | The `ble_client` entry for the scale. Name it explicitly if the device has more than one. |
| `id` | ID | generated | Only needed if something else refers to the instance. |
| `weight` | sensor | — | kg, 2 dp, device class `weight`. |
| `fat_mass` | sensor | — | kg, 2 dp. |
| `muscle_mass` | sensor | — | kg, 2 dp. |
| `hydration` | sensor | — | kg, 2 dp. |
| `bone_mass` | sensor | — | kg, 2 dp. |
| `drift` | sensor | — | Seconds. How far the scale's own clock was off, from its `TimeSetReply`. Diagnostic; bounds how far each measurement's timestamp can be trusted. |
| `connect_reason` | text sensor | — | Why the scale accepted this connection: `USER_REQ (button)` or `DEVICE_REQ (weigh-in)`. Diagnostic. |
| `last_sync` | text sensor | — | The session timestamp of the last sync the scale actually acknowledged — a device-side signal that a sync *finished*, not merely that a connection happened. Diagnostic. |

Every sensor is optional; configure the ones you want.

The published values come from the measurement with the **largest timestamp**
in the batch, not the last one in the list — `GETALL`'s ordering is not
documented, so list position is not trusted to mean "latest".

Five measurement types map to sensors: weight, fat mass, muscle mass, hydration
and bone mass. Others the scale sends — pulse, fat-free mass, and several
unidentified types in the impedance range — are decoded, logged and kept in the
raw capture, but have no sensor. Nothing received is ever discarded for being
unrecognised.

## What lands on the SD card

Every WPP frame received during a sync is appended to a file **verbatim, before
it is decoded**, so a parser bug never costs you the bytes needed to fix it and
replay.

```
/sdcard/.partial/withings/<mac>/<YYYYMMDDHHMMSS>.wpp   during a sync
/sdcard/withings/<mac>/<YYYYMMDDHHMMSS>.wpp            after the scale's SYNC_OK ack
```

`<mac>` is the scale's address with the colons stripped; the timestamp is
UTC, taken once at session start.

The promotion out of `.partial/` happens only when the scale acknowledges
`CMD_SYNC_OK` — not merely when the measurement list terminates. A session that
was cut short stays in `.partial/` indefinitely: never promoted, never deleted.

Files are raw WPP frames in wire order. The component's own
[README](components/withings_wpp/README.md) documents the framing, object
types and measurement type table — everything needed to parse one.

Two things matter to whatever ingests them:

- **Dedup is mandatory.** Acknowledging a sync does not mark anything as read,
  so every connect re-serves the scale's entire store. That is what makes a
  missed weigh-in self-heal — but it also means you get everything again, every
  time.
- **Dedup must key on the measurement's own `time`** and nothing else. The
  identity fields in the record (`uid`, `userId[]`) are *not stable*: decoding
  two captures of the same measurement a day apart gives different values for
  both, on an otherwise byte-identical record. They look like uninitialised
  device memory.

How large the store grows before the scale starts dropping old measurements has
not been measured here; `GETSTATE` reports a count and the oldest stored
timestamp if you want to watch it.

## Troubleshooting

**Nothing happens when someone weighs themselves.** The scale is only on the
air for about 55 seconds after a weigh-in and cannot be woken, so first make
sure the window had not already closed. Then check for the component's
per-advertisement log line, which is at `VERBOSE` — if your logger is above
that level you cannot tell a listener that is never called from one that is
working, because `esp32_ble_tracker` logs nothing per advertisement of its own.
Then check the connect: a weigh-in seen while the client is not `IDLE` is
logged and dropped, since an edge is a one-shot. Nothing is permanently lost
either way; the next sync re-serves everything.

**Connects fail perhaps half the time**, with `ESP_GATT_CONN_CONN_CANCEL`
(reason `0x100`) and then `OPEN_EVT status=133`, after about 20 seconds. The
attempt was cancelled locally, not refused. The usual cause is a scan duty
cycle that leaves the radio no airtime — see `scan_parameters` above, where the
window must be shorter than the interval. The next weigh-in re-serves
everything anyway, so a failed connect costs nothing but the wait.

**The first subscribe fails with `Insufficient Authentication`.** This means
almost the opposite of what it looks like. The device is bonded; the link had
not finished escalating to encrypted when the subscribe was attempted. Do not
go and re-pair a perfectly good bond — the component logs this case with that
warning attached.

**Pairing fails immediately.** `auth fail reason=N` is
`BTA_DM_AUTH_FAIL_BASE + SMP status`, where the base is `0x43 + 10 = 77`. So
`78` is `SMP_PASSKEY_ENTRY_FAIL` — most often `io_capability` left at `none`,
or a passkey reply sent before the code was read — and `102` is
`SMP_CONN_TOUT`.

**`-6 AUTH_ERR`** is a wrong `kl`. **`-5 NOT_AUTH`** means the probe sequence
did not complete — the scale saw a command before authentication. **`-3
ERR_VAL`** is a malformed command, most often a `CMD_STORED_MEASURE` with no
`StoredMeasureAction` object.

**`scripts/get_kl.py` says login failed.** The account has no password login —
see Requirements for converting one. An OAuth bearer token is refused by this
endpoint (`Invalid Session: sessionid missing`), and the modern `getdevice`
call does not return the secret at all.

**The scale seems dead and cannot be weighed on.** A connected BLE peripheral
stops advertising, and this scale cannot be used at all while a link is held
open. Every terminal path in the component — success, protocol error, reply
timeout — releases the link for exactly this reason.

**No file on the card.** An `fopen` failure is logged with its path and
`errno`, and the sync continues without a capture. Check that the card is
mounted at `/sdcard`, and that FATFS long filename support is compiled in —
the session filenames are 14 characters before the extension, well past 8.3,
and the tell for LFN being off is `errno` 22 (`EINVAL`).

## Interoperability

This is an independent interoperability implementation, written from publicly
available protocol documentation and observation of a device the author owns.
It is for use with **hardware you own and an account you control**. It is not
affiliated with, authorised by, endorsed by or supported by Withings.

It does not circumvent anything or unlock anything: it presents the account's
own association secret, which the account holder supplies, and asks the device
for data it is already storing about the person operating it. The vendor's
service is untouched and keeps working.

The protocol understanding behind this component came from public prior art:

- [I Connected My Withings Body+ to Home Assistant with an ESP32](https://didac.dev/blog/i-made-my-withings-scale-sync-to-home-assistant-without-the-cloud)
  by Dídac Sabatés, and its
  [Hacker News thread](https://news.ycombinator.com/item?id=49550436) — the
  post that set this project off: a Body+ synced to Home Assistant over an
  ESP32, no cloud.
- [`totruok/openwithings`](https://github.com/totruok/openwithings) — talks to
  Withings scales over BLE from Linux; its protocol notes are the source for
  the scale's WPP flow, and it includes its own `kl` retrieval tooling.
- [`DavidVentura/withouthings`](https://github.com/DavidVentura/withouthings)
  — Rust, targets a ScanWatch 2; the source for the readopt state machine, the
  SHA1 challenge formula, and the command/object-type tables.

## Contributing

Outside contributions are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for
the branch model and what a change needs to show. Participation is governed by
the [Code of Conduct](CODE_OF_CONDUCT.md). To report a vulnerability, follow
[SECURITY.md](SECURITY.md) rather than opening an issue. Released changes are
recorded in [CHANGELOG.md](CHANGELOG.md).

The protocol reference — wire format, the full command sequence, object type
tables, the error codes and the implementation traps — lives next to the code
in
[components/withings_wpp/README.md](components/withings_wpp/README.md).

## License

[PolyForm Noncommercial 1.0.0](LICENSE). Copyright Joshua Hogendorn 2026.

This is **source-available, not open source**: use it for any noncommercial
purpose — personal projects, study, hobby work — and by charities, schools,
public research and government bodies, but not commercially. The full terms
are in [LICENSE](LICENSE) and are short enough to read.
