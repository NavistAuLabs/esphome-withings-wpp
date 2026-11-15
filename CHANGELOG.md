# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Anything that changes a default, a configuration key, a published sensor, or
the layout of what lands on the SD card is recorded here regardless of size —
those are the changes that break a consumer without breaking a build.

## [Unreleased]

## [0.2.0] - 2026-11-15

### Changed

- The advertisement trigger now connects once per weigh-in: on the first
  advertisement seen after five minutes with none, rather than on the
  absent-to-present edge of a 60-second presence window. A slow or retried
  connect inside one ~55-second advertising burst no longer re-triggers a
  second session against the same weigh-in. The five minutes is fixed, not
  configurable.
- Connect, connect failure, idle disconnect, the encrypted-link confirmation,
  the first advertisement of each appearance and the end of the quiet period
  are logged at `DEBUG`.

### Fixed

- First-time pairing no longer aborts. When the notify subscribe is refused
  for insufficient authentication while the passkey pairing is still in
  flight, the component now waits for the pairing to finish (up to 45
  seconds) and retries the subscribe once, instead of releasing the link and
  cancelling the pairing (auth fail reason `0x66`).
- A session with a scale in setup mode is closed the way Health Mate closes
  it, with `CMD_SYNC_OK` then `CMD_SETUP_OK`, instead of failing on the
  unexpected probe reply and leaving the scale showing "Setup failed". No
  keys are written.

## [0.1.0] - 2026-09-18

First public release.

### Added

- `withings_wpp`: an ESPHome external component that reads stored weigh-ins
  from a Withings Body Smart scale over BLE, speaking WPP and authenticating
  with the account's `kl` association secret.
- Advertisement-driven sync. The scale advertises only for about 55 seconds
  after a weigh-in and cannot be woken, so the component listens as an
  `esp32_ble_tracker` device and connects on the absent-to-present edge. A
  full authenticate-read-release cycle takes about three seconds.
- Peer matching on the WPP service UUID rather than the address, with the
  `ble_client` re-pointed when the scale's static-random address rotates.
- An encrypted link requested on every connect, not only at pairing, because
  a bond is key material rather than an encrypted link.
- The full read sequence — `CMD_TIME_SET`, `CMD_CONNECT_REASON`,
  `CMD_STORED_MEASURE{GETSTATE}`, `{GETALL}`, and the mandatory `CMD_SYNC_OK`
  acknowledgement, which the scale requires before it will serve a subsequent
  connection.
- `DELALL` is never sent, and its command value is not defined anywhere in the
  source. The scale's store, and therefore its cloud sync, is untouched.
- Optional sensors for weight, fat mass, muscle mass, hydration and bone mass,
  published from the measurement with the latest timestamp in the batch;
  diagnostic sensors for scale clock drift, connect reason and last
  acknowledged sync.
- Raw capture: every received frame is appended verbatim to
  `/sdcard/.partial/withings/<mac>/<utc>.wpp` before any decode, and promoted
  to `/sdcard/withings/<mac>/<utc>.wpp` only once the scale acknowledges the
  sync. A cut-short session is never promoted and never deleted.
- Compile-time validation that `association_key` is exactly 32 ASCII
  characters, so a mis-pasted secret is a build error rather than a runtime
  `-6 AUTH_ERR` days later.
- A self-contained SHA-1 (RFC 3174), verified against known test vectors —
  including the padding-boundary cases — and against a real captured challenge
  answer, rather than a pin to one of ESP-IDF's two incompatible mbedtls SHA-1
  surfaces.
- Unrecognised measurement types are decoded, logged and kept rather than
  dropped.
- `scripts/get_kl.py`: a read-only helper that logs in to the legacy Withings
  account API with your own email and password, lists that account's device
  associations, and prints each device's `kl`. Nothing is written to the
  account, and nothing is sent to the scale.
