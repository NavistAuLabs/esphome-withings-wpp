# Contributing

Outside contributions are welcome. This document covers the branch model, the
evidence a change needs to carry, what is in scope for this component, and the
constraints that shape almost every decision in the code.

## Branch model

- Fork the repository, branch from `main` (`feat/…`, `fix/…`, `docs/…`), and
  open your pull request against `main`.
- Releases are git tags. There are no build artifacts — ESPHome consumes the
  source directly from `github://…@<tag>`.
- **A published tag is never moved or deleted.** People's builds are pinned to
  it, and repointing a tag silently changes what their next compile produces.
  A mistake in a release is fixed by a new tag.

## Never paste your own data

Everything this component touches is either a health measurement or a
credential. Before you attach a log, a capture or a configuration to an issue
or a pull request:

- **Never post your `kl`.** It is the account's association secret. If you
  believe you have leaked one, it can be rotated by re-associating the device
  through the vendor app.
- **Redact weights, pulse rates and measurement timestamps.** A synthetic
  value proves a decode path just as well as your real one.
- **Redact BLE addresses**, both the rotating advertised one and the scale's
  identity MAC. `AA:BB:CC:DD:EE:FF` reads fine in a bug report.
- `.wpp` capture files contain full measurement history. They are gitignored
  for a reason; do not attach one without reading it first.

## Your change has to have been run

This is firmware talking to a device that is only reachable for about 55
seconds after somebody physically stands on it. There is no test suite that
can tell you a change is correct, and the interesting failures — a link held
open, a security escalation that has not landed yet, a length field read from
the wrong byte — all look fine on review.

So a pull request that changes behaviour should say, in its description:

- The board you ran it on and the ESPHome version.
- What you actually observed: the log lines, the decoded values being
  plausible, the session file being promoted out of `.partial/`.
- Whether the scale was still usable afterwards. A held link makes it
  unusable, which is the worst failure this component can cause.

"Compiles clean" is not evidence for this project. A documentation-only change
obviously does not need any of this.

If you have a `.wpp` capture that exercises something the decoder gets wrong,
that is the single most useful thing you can bring — described, with the
values redacted, not attached raw.

## Scope

The component has one job: read what the scale already stored, and get it off
the device intact. Things that are out of scope, and why:

- **Anything that writes to the scale beyond the protocol's own required
  acknowledgements.** In particular `DELALL`, which clears the scale's store.
  Its value is deliberately not defined anywhere in this source so it cannot
  be reached for by mistake. The store is what the cloud path depends on, and
  it is also what makes a missed sync self-heal.
- **Anything that grows `scripts/get_kl.py` past retrieval.** It exists to read
  one credential out of your own account, read-only, and print it. It is not
  the place for account management, measurement downloads, or anything that
  writes to the vendor's API.
- **Anything with a runtime cloud dependency.** Once `kl` is on the device,
  the device is done with the internet.
- **Person attribution from the record's identity fields.** `uid` and
  `userId[]` are demonstrably unstable across syncs; a PR that keys anything
  on them needs to first show that instability was measurement error.

New configuration options are not free — each one is documented, supported,
and kept working indefinitely. Bring a concrete case the current schema cannot
express. If you are unsure, open an issue before writing the PR.

## Four constraints that are not style preferences

**Release the link on every terminal path.** Success, protocol error, reply
timeout — all of them. A connected BLE peripheral stops advertising, and this
scale cannot be weighed on *at all* while a link is held open. Every terminal
path routes through `finish_()` specifically so that none of them can skip the
disconnect. Do not add a path that returns without going through it.

**Request encryption on every connect, not just at pairing.** A bond is key
material, not an encrypted link. Reconnecting to an already-bonded scale comes
up plaintext unless encryption is requested again, and subscribing does not
escalate security on its own.

**Write the raw frame before you decode it.** Every received frame is appended
to the session file verbatim, ahead of any parsing, and the file is promoted
out of `.partial/` only on the scale's own `CMD_SYNC_OK` acknowledgement. The
capture window is seconds long and cannot be reproduced on demand; a decoder
bug must never be able to destroy the bytes needed to fix it.

**Nothing in the event path may block.** The sequence is advanced entirely by
`gattc_event_handler()` and `set_timeout()` callbacks. No delays, no busy
waits, no long work inside a handler.

## Style

Match the surrounding code. It follows ESPHome core's conventions: 2-space
indent, a 120-column limit, `snake_case_` trailing-underscore members, and
`ESP_LOGx` with the file's `TAG`. Log at `ESP_LOGV`, never `ESP_LOGVV` — a
device logger at `VERBOSE` compiles `VV` statements out entirely, producing
silence that is indistinguishable from code that never ran.

Comments in this component explain *why*, especially where the obvious
implementation is the wrong one. If you change something a comment justifies,
update the comment in the same commit.

Protocol claims carry their evidence. The existing comments say what was
verified, how, and what is still assumed; a new claim should arrive the same
way, and "the reference says so" is an assumption until a capture agrees with
it.

## Changelog

This project follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Add your entry to [CHANGELOG.md](CHANGELOG.md) under `## [Unreleased]` in the
same pull request that makes the change, not at release time. Anything that
changes a default, a configuration key, a published sensor, or the on-card
file layout gets an entry regardless of how small the diff is — those are the
changes that break a consumer silently.

## Code of conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). Reports go
to `foss+conduct@navist.com.au`.
