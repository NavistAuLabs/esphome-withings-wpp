# Security policy

## Supported versions

Security fixes go onto the most recent tag. There are no long-term support
branches.

| Version | Supported |
| ------- | --------- |
| 0.1.x   | Yes       |
| < 0.1   | No        |

## Report a vulnerability

Report vulnerabilities privately through
[GitHub Security Advisories](https://github.com/NavistAuLabs/esphome-withings-wpp/security/advisories/new).
Do not open a public issue for a vulnerability.

Include the ESPHome version, the board, a configuration that shows the problem
and the effect you can demonstrate. **Do not include your `kl`, real
measurements or real BLE addresses** — a report never needs them. Expect an
acknowledgement within seven days.

## Before you report: two things are true by design

**The `kl` secret is at rest in device flash.** It arrives through `!secret`,
which compiles it into the firmware image. Anyone who can read flash off the
board can read it. That is a property of how ESPHome handles secrets, not a
defect in this component, and it is a deliberate trade: `kl` grants read access
to one scale's stored measurements, not to the Withings account. If your threat
model includes someone holding your hardware, this component is not for you.

**Measurements land on an SD card in the clear.** The `.wpp` session files are
raw protocol frames, and whoever holds the card can decode them. The component
has no encryption-at-rest and is not going to grow one; it is a capture path
for hardware inside your own home.

Reports amounting to "the secret is in the firmware" or "the card is not
encrypted" will be closed with a pointer to this section.

## What is in scope

The component authenticates with a long-lived shared secret, talks to a peer
it selects from the air, and parses attacker-influenceable length fields on a
device with no memory protection. That shape defines the interesting surface:

- **Anything that leaks the association key.** `kl` is used to compute one
  SHA-1 and is never logged, never published to a sensor, and never written to
  the card. A code path that exposes it — a log line, a dump, an error
  message, a file — is the most serious report this project can receive.
- **Peer selection.** The component connects to whatever advertises the WPP
  service UUID, because the scale's own advertised address rotates and cannot
  be trusted as identity. A hostile advertiser within radio range can
  therefore attract the connection and issue a challenge of its own choosing,
  receiving `SHA1(challenge || mac || kl)` in return. That is an oracle
  against a 32-character secret rather than a disclosure of it, and it costs
  the attacker nothing but proximity — if you have a way to bind the peer to a
  real identity before the challenge is answered, that is a very welcome
  report. A report that the component can be made to connect to the *wrong*
  device, or to be kept from the right one, is also in scope.
- **Memory safety in the frame and TLV parsers.** Every length in a WPP frame
  comes off the wire. The parsers bound-check before reading and stop rather
  than overrun; an input that gets past that, corrupts the heap, or faults the
  device is in scope. A remotely triggerable crash counts — the board is
  typically unattended, and the weigh-in it would have captured is gone.
- **The file path built from peer data.** The session path is assembled from
  the peer's address with colons stripped. A way to influence that into
  writing outside `/sdcard/withings/` is in scope.
- **Anything that makes the component hold the BLE link open.** A held link
  makes the scale unusable — it cannot be weighed on at all — so a denial of
  service here is physical, not just digital.
- **Anything that sends `DELALL`.** The component must never be able to clear
  the scale's store. A path that reaches it is a data-destruction bug and will
  be treated as a security issue.
- **`scripts/get_kl.py` mishandling a credential.** It takes an account
  password and prints a `kl`. A path that sends either anywhere other than the
  account API, or writes either to disk, is in scope. Passing `--password` on
  the command line puts it in your shell history and process list — that is why
  the environment variable and the interactive prompt exist, and it is not
  itself a defect in the script.

## What is out of scope

- **The secret at rest, and the unencrypted card.** See above.
- **BLE pairing itself** — the passkey model, bond storage in NVS, the ESP-IDF
  SMP implementation. That is ESP-IDF and ESPHome code; report it there.
- **Vulnerabilities in ESPHome, ESP-IDF, bluedroid, FATFS, or whatever
  component mounts the card.** Report those to their projects.
- **Withings' own cloud API, the Health Mate app, or the scale's firmware.**
  Reports about these belong with the vendor. The only thing here that touches
  the cloud API is `scripts/get_kl.py`, which runs on your own machine rather
  than on the device; bugs in that script are in scope, the API's own behaviour
  is not. If you have found something in the device firmware, disclose it to
  Withings, not here.
- **Physical access to the board, the card, or the scale.**
- **That the protocol uses SHA-1.** The device defines the handshake; this
  component implements what the device requires. There is no version of this
  that gets to choose a better primitive.
