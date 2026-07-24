# V1 Modem Retry Power Design

## Goal

Keep the A7670G rail enabled while a queued `.RDY` file is being retried, avoiding a fresh cold boot and LTE attachment after every failed attempt.

## Behavior

- PB8 is enabled when an upload queue first requires the modem.
- A modem boot, registration, PDP, or HTTP failure leaves PB8 enabled.
- The existing 60-second retry interval remains unchanged.
- The same queued file remains on the SD card until a successful HTTP upload.
- PB8 is disabled only after the upload queue reaches zero.
- Existing Live Expressions continue to expose modem boot, upload, and power state.

## Scope

Only the V1 modem power policy and its source-contract test change. The validated modem commands, HTTP upload logic, CSV format, SD queue handling, and 2,500-reading rotation remain unchanged.

## Verification

The power-gate test must reject intermediate retry shutdowns while requiring the successful `upload complete` shutdown. All PowerShell tests and the embedded Debug build must pass.
