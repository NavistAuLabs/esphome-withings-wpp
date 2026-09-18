# withings_wpp — how it works

Protocol and implementation reference for the `withings_wpp` component: WPP
(Withings Proprietary Protocol) over BLE against a **Withings Body Smart**,
authenticated with the account's `kl` association secret. For installing and
using the component — full device configuration, `kl` retrieval, pairing, SD
mount, troubleshooting — see the root [README](../../README.md); nothing here
is needed to run it.

It is non-destructive by design (an additional reader, never a takeover —
see the root README), and captured sessions land at
`/sdcard/withings/<mac>/<session-utc>.wpp`, promoted from `.partial/` only
once the scale acknowledges `CMD_SYNC_OK`.

## The capture window

**The scale is silent when idle.** It advertises only for roughly 55 seconds
after someone stands on it, and cannot be woken remotely. There is no
polling, no scheduling, no "sync now" from cold.

So the component is an **advertisement listener** as well as a GATT client:
an advertisement *is* the weigh-in event, and it connects on sight. A
full authenticate-read-release cycle takes about 3 seconds, comfortably
inside the window.

It matches on the **`WITH` service UUID** (`00000020-5749-5448`, "5749 5448"
being ASCII "WITH"), not on the MAC. The scale advertises a *static random*
address that changes whenever it reboots, so a hardcoded MAC stops matching
silently — the weigh-in is missed and nothing anywhere says why. When the
observed address differs from the client's, the client is re-pointed before
connecting.

The scale's stable **identity** MAC (`11:22:33:44:55:66`, say) is a third
thing again — not the rotating address you put in `ble_client`. It is
embedded in the advertised service UUID's node and comes back in the auth
challenge, and it is what the SHA1 answer is computed over. **Setup mode
advertises a zero node instead**, so identity recovery works in normal mode
only.

## Protocol

Service `00000020-5749-5448-…`, characteristic `00000024-5749-5448-0010-…`.
One characteristic carries everything.

```
frame:  01 | cmd(u16 BE) | payload_len(u16 BE) | objects
object: type(u16 BE) | size(u16 BE) | data     (strings/arrays: 1-byte length prefix)
list terminator: Null TLV (type 256, len 0)
0x4000 on a cmd = device-initiated; 0x8000 = notification
```

**The scale is app-driven — it never volunteers data.** Listening after
authentication yields silence; you must drive the whole sequence:

```
TX  CMD_PROBE (257)            AppProbe(298) + AppProbeOsVersion(2344)
RX  CMD_PROBE_CHALLENGE (296)  ProbeChallenge(290){mac str, challenge[16]}
TX  CMD_PROBE_CHALLENGE (296)  ProbeChallengeResponse(291){SHA1(challenge || mac_lowercase_ascii || kl)}
RX  CMD_PROBE (257)            their answer + ProbeReply  -- AUTHENTICATED
TX  CMD_TIME_SET (1281)        TimeSet{utc, gmtOffset, dstChangeTime, nextGmtOffset}
RX  TimeSetReply (1282)        {drift i32}
TX  CMD_CONNECT_REASON (273)
RX  ConnectReason (280)        1 USER_REQ (button), 2 DEVICE_REQ (weigh-in)
TX  CMD_STORED_MEASURE (271)   StoredMeasureAction(276){cmd = 0 GETSTATE}
RX  StoredMeasureStatus (277)  {cnt i16, oldestMeasTime i32, wifiConfigured i8}
TX  CMD_STORED_MEASURE (271)   StoredMeasureAction{cmd = 1 GETALL}
RX  per measurement            Meta(278) / MetaExtend(299) / Data(279)* ... Null
TX  CMD_SYNC_OK (277)          MANDATORY -- skip it and the scale ignores you next connect
```

The MAC in the SHA1 input is the **ASCII lowercase string form**, not raw
bytes.

**`DELALL` (cmd = 2) is never sent** and has no named constant in this
component, so it cannot be reached for by mistake.

Errors (`Cmderror`, cmd 256, `{cmd u16, err i32}`) mean quite different
things and are logged separately:

| code | meaning |
|---|---|
| `-3` ERR_VAL | bad or missing object. `CMD_STORED_MEASURE` with an empty payload gives this — it needs the `StoredMeasureAction` object. It does *not* mean the command itself is unsupported |
| `-5` NOT_AUTH | no association — the probe sequence is wrong |
| `-6` AUTH_ERR | wrong `kl` |

## `StoredMeasureMeta` — TWO count bytes

The easiest thing in this protocol to get wrong. The reference writes the
layout as:

```
uid u32, userIdCnt u8, userId: u8 count + u32[], attrib u8, time u32
```

**`userId` is itself length-prefixed.** Its array count is a *separate byte*
after `userIdCnt`, not a value derived from it. Reading the array length from
`userIdCnt` collapses the array to empty and slides `attrib` and `time`
twelve bytes left, producing `users=0 attrib=3 time=0` on every measurement —
plausible-looking and completely wrong. Without `time` nothing can be dated;
without `userId[]` nothing can be attributed.

A real 23-byte capture, decoded:

```
00000003 00 03 00000000 00000003 2001c698 01 6aa04df3
uid=3    |  |  \___ userId[3] ___________/  |  time
         |  userId array count              attrib
         userIdCnt
```

`4 + 1 + 1 + (3 x 4) + 1 + 4 = 23`. Exact, no remainder.

`StoredMeasureMetaExtend(299).algo`: **every value observed on the wire is
`0`, on guest and profile weigh-ins alike** — one datapoint arrived with the
TLV absent entirely, and `algo=3`, which exists in the app's constants, has
never been observed. `algo` distinguishes nothing. The fields that do
discriminate, consistent across a dozen guest sessions and a
full-composition profile session: `attrib` (`1` guest, `0` profile) and
`userIdCnt` (`0` guest, `1` profile). Guest sessions carry a single
datapoint (weight); a profile weigh-in carried 17, including
impedance-derived body composition.

`StoredMeasureData(279)`: `value i32, type u16, exponent i16`; real value is
`value × 10^exponent`.

| type | meaning |
|---|---|
| 1 | weight kg |
| 5 | fat-free mass kg |
| 8 | fat mass kg |
| 11 | pulse bpm |
| 76 | muscle mass kg |
| 77 | hydration kg |
| 88 | bone mass kg |
| 170, 177, 178, 189, 201, 202, 203, 207, 226, 227 | unknown — observed but unidentified, values not in the public API |

On the full-composition profile session above: type 5 (fat-free mass) **is**
on the wire, and so is type 11 (pulse), carrying a plausible pulse rate —
neither is cloud-derived only.
Type 6 (fat ratio) has not been observed, which is no evidence either way.
The values in the plausible impedance range (370–500-ish) arrived as types
177, 178 and 189; types 78, 79, 86, 16 and 80, which other notes list as
impedance, did not appear at all. An unidentified type is recorded as
unknown, not guessed at. Every type received is stored regardless.

## Identifying who weighed in

The wire gives you one reliable bit: **profile or guest.** `attrib` (`0`
profile, `1` guest) and `userIdCnt` (`1` profile, `0` guest) have
discriminated correctly in every observed session. On a single-profile
scale that is full attribution: `attrib=0` is your user. Nothing observed
so far distinguishes *which* profile on a multi-profile scale — all
evidence to date is from a one-profile household, so treat multi-profile
attribution as unsolved.

The fields that look like identity are not. **Measured across two captures
pulled a day apart**, the *same* measurement carried different
`uid`/`userId[]` values on an otherwise identical record:

| field | first capture | same measurement, next day |
|---|---|---|
| `uid` | `3` | `0` |
| `userIds` | `[0, 3, 536987288]` | `[134544467, 134544466, 1627389952]` |
| `ts` | 2026-01-15 07:30:00Z | identical |
| weight | e.g. 70.000 kg | identical |
| `attrib` | 1 | 1 |

Same timestamp, same value, same layout — only the identity fields moved,
and the array is not even constant within a single session: one datapoint
of a 17-datapoint weigh-in carried a third distinct value set. The values
look like **uninitialised memory**: `0x0804FC52`/`0x0804FC53` are STM32
flash addresses, `0x2001C698` is STM32 SRAM. The scale appears to leak
buffer contents into `userId[]`.

**Consequence:** `uid`/`userId[]` cannot be used as any part of a dedup or
identity key. Whatever ingests these measurements has to key on
`(time, measure_type)` alone — anything that includes an identity field
will re-insert the same weigh-in on every sync.

## Every sync serves the complete store

`SYNC_OK` acknowledges receipt; only `DELALL` clears the store, and this
component never sends it. So a missed weigh-in self-heals — it arrives on
the next sync — and dedup on `time` is mandatory, since every sync returns
everything.

## Gotchas

**REQUEST ENCRYPTION ON EVERY CONNECT.** A bond is key material, not an
encrypted link. Reconnecting to an already-bonded scale comes up plaintext
unless `esp_ble_set_encryption(addr, ESP_BLE_SEC_ENCRYPT_MITM)` is called
again — which then silently re-encrypts from the stored LTK with no new
passkey. Subscribing does **not** escalate security on its own: the client
connects, holds, and the peer never asks for anything. Skip it and the first
subscribe fails with `Insufficient Authentication`, which looks exactly like
an unpaired device and sends you off to re-pair a perfectly good bond.

**RELEASE THE LINK on every terminal path** — success, failure, timeout, auth
rejection. A connected BLE peripheral stops advertising, so a held link makes
the scale invisible *and unusable*: it cannot be weighed on at all while the
link is open. Hold a connection only as long as the sync needs it.

**Log at `ESP_LOGV`, never `ESP_LOGVV`.** The device logger runs at `VERBOSE`
and compiles `VV` statements out entirely, producing silence indistinguishable
from code that never runs.

**`esp32_ble_tracker` logs nothing per advertisement.** Without this
component's own advertisement log, "is the listener even being called?" is
unanswerable from outside the board.

**Connects fail roughly half the time** with `ESP_GATT_CONN_CONN_CANCEL`
(reason `0x100`) then `OPEN_EVT status=133`, after a 20-second timeout —
cancelled locally, not refused. The presence timeout must exceed 20s or a
failed connect expires it and fakes a fresh wake edge.

**The bond survives reflashing.** It lives in ESP-IDF NVS keyed by peer
address, managed by bluedroid rather than by ESPHome — you pair once, not
once per flash. The exception is a full chip erase, which wipes NVS and the
bond with it; after one, pair again.

## Pairing

The passkey rig lives in your device YAML, not in this component, and it is
**not temporary** — it is the re-onboarding path if the bond is ever lost.
Leaving it out of the component is deliberate: reading a six-digit code off
the scale's display needs a human and a runtime-settable input, so it is
inherently config-side. The root [README](../../README.md) has the YAML.

Three things must all be true, and each fails differently when it is not:

1. **`io_capability: keyboard_display`** on `esp32_ble`. The default `none`
   can only negotiate Just Works, and pairing fails in about 4 ms with
   `SMP_PASSKEY_ENTRY_FAIL`.
2. **Request encryption explicitly** on connect (above).
3. **Hold the passkey request open ~28 s.** The scale renders its six-digit
   code only once a peer asks to pair. Replying immediately sends whatever
   the field holds (0 on a fresh boot) and the scale flashes "setup failed".
   SMP itself times out around 30 s.

Decoding the ESP-IDF pairing error number: `auth fail reason=N` is
`BTA_DM_AUTH_FAIL_BASE + SMP status`, where the base is `0x43 + 10 = 77`. So
`78` is `SMP_PASSKEY_ENTRY_FAIL` and `102` is `SMP_CONN_TOUT`.

## The `kl` secret

`kl` is the **association secret** the scale and its owner's Withings account
share. It is what the Health Mate app holds, and it is the only thing that
makes the scale answer a BLE peer at all: a peer that cannot produce
`SHA1(challenge || mac || kl)` gets `-6 AUTH_ERR` and nothing else. It is 32
ASCII characters, one per device.

**You get it from your own account.** The secret is issued to the account the
scale is already associated with, so retrieving it is an account holder
reading their own credential — it is not extracted from the device, and it is
not shared between accounts. It is fetched **once** and then never again:
after that the device has no cloud dependency at all, at runtime or otherwise.

Retrieving it needs Withings' **legacy** association API, which authenticates
with the account **email and password** and wants a legacy `sessionid`. An
OAuth bearer token is refused there (`Invalid Session: sessionid missing`),
and the modern `getdevice` call does not return the secret — verified against
the live API. A Sign-in-with-Apple or Google account has no password until you
set one through the reset flow.

`scripts/get_kl.py` in this repository performs that fetch: it logs in, lists
the account's own device associations, and prints each device's `kl`. It is
read-only — nothing is written to the account, and nothing is sent to the
scale. The root [README](../../README.md) has the steps and the account
requirement.

The protocol understanding here came from public prior art:

- [`totruok/openwithings`](https://github.com/totruok/openwithings) — talks to
  Withings scales over BLE from Linux, and includes the association/`kl`
  retrieval tooling.
- [`DavidVentura/withouthings`](https://github.com/DavidVentura/withouthings) —
  Rust, targets a ScanWatch 2; the source for the readopt state machine and the
  SHA1 challenge formula this component implements.

Once you have it, it reaches the device via `!secret`, which places it **at
rest in device flash**. Treat that as the trade it is rather than an
oversight: `kl` grants read access to one scale's stored measurements, not to
the Withings account, and anyone who can read flash off the board is already
holding your hardware.

### Interoperability

This is an independent interoperability implementation, for use with hardware
you own and an account you control. It is not affiliated with, authorised by,
or supported by Withings. Nothing here is required for the scale to work as
sold — the component is **non-destructive**, reading what the scale already
stores while the device stays associated with its cloud account and keeps
syncing over Wi-Fi.

