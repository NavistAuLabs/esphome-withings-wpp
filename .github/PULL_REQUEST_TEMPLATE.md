<!--
Branch from `main` and target `main`. See CONTRIBUTING.md.

Nothing in this PR should contain a real `kl`, a real BLE address, or real
measurements.
-->

## What this changes

<!-- One or two sentences. What behaviour is different after this merge? -->

## Why

<!-- The problem this solves. Link the issue if there is one. -->

Closes #

## How it was run

<!--
Required for anything that changes behaviour; delete this section for a
docs-only change. "Compiles clean" is not evidence — see CONTRIBUTING.md.
-->

- Board and ESPHome version:
- What you observed (log lines, decoded values, the session file being
  promoted out of `.partial/`):
- Was the scale still usable afterwards?

## Checklist

- [ ] Run against a real scale, with the result recorded above.
- [ ] `CHANGELOG.md` has an entry under `## [Unreleased]`.
- [ ] Documentation updated if an option, sensor, default or on-card layout
      changed.
- [ ] Comments that justify a non-obvious choice still match the code.
- [ ] No real secrets, addresses or measurements anywhere in the diff or the
      description.

## Protocol constraints

<!--
Delete if the change does not touch the connection or the sync. Otherwise
confirm all four:

- every terminal path still releases the BLE link (a held link makes the
  scale unusable);
- encryption is still requested on every connect;
- received frames are still written to the session file before being decoded,
  and promotion still waits for the scale's CMD_SYNC_OK;
- nothing added to an event handler blocks.
-->
