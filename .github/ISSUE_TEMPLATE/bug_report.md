---
name: Bug report
about: Report a problem with withings_wpp
title: ""
labels: bug
assignees: ""
---

**Before you paste anything:** everything this component handles is either a
health measurement or a credential.

- **Never post your `kl` / `association_key`**, or a firmware image containing
  one.
- Replace real weights, pulse rates and measurement timestamps with synthetic
  values — they prove a decode path just as well.
- Replace BLE addresses with `AA:BB:CC:DD:EE:FF`, both the advertised one and
  the scale's identity MAC.
- Do not attach a `.wpp` capture without reading it first; it is a full
  measurement history.

If this is a security issue, do not open an issue — see SECURITY.md.

## Environment

- ESPHome version:
- Board:
- withings_wpp version (the tag in your `external_components` source):
- Scale model:
- What mounts the SD card, and where (or "no card"):
- Is the scale currently bonded (did pairing succeed at some point)?

## What happened

## What you expected to happen

## Steps to reproduce

<!--
Say what the scale was doing: a fresh weigh-in, a button press, a manual
`ble_client.connect`. The scale advertises for only about 55 seconds after a
weigh-in, so "nothing happened" often means the window had closed.
-->

## Relevant configuration

```yaml
# your withings_wpp, ble_client, esp32_ble and esp32_ble_tracker blocks,
# redacted
```

## Relevant log output

```
# Redact addresses and measured values first. Set the logger to VERBOSE if
# the question is whether the advertisement listener is being called --
# the component logs each advertisement at that level and nothing above it.
```
