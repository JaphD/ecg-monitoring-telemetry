# V1 TLS 715 Recovery Design

## Goal

Recover automatically when the A7670G reports HTTP status `715` (TLS handshake failure) without changing the SD-first CSV workflow.

## Behavior

- Configure SSL context 0 for TLS 1.2 and SNI before each HTTPS request.
- Associate HTTP with SSL context 0.
- Keep the existing three HTTP attempts and five-second attempt backoff.
- Remember whether any attempt in the completed round returned `715`.
- If the entire round fails after seeing `715`, mark TLS recovery pending.
- In the existing 60-second queue-retry path, cycle PB8 once and clear the pending flag.
- The next retry cold-boots, registers, restores PDP, and retries the retained `.RDY` file.
- Ordinary HTTP failures continue with the modem rail enabled.
- A successful queue drain still disables PB8 using the existing `upload complete` path.

## Diagnostics

Expose `tls_handshake_failures`, `tls_recovery_pending`, and `tls_recovery_cycles` as `volatile uint32_t` Live Expressions.

## Scope

Do not change the CSV format, 2,500-reading rotation, server endpoint, SD queue ownership, HTTP attempt count, or retry timing.
