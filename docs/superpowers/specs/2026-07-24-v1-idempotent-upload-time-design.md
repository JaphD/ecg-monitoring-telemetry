# V1 Idempotent Upload and Server-Time Design

## Goal

Make V1 uploads deterministic and retry-safe while preserving the SD-first
recording lifecycle and PB8 modem power gate.

## Time synchronization

Firmware first checks `AT+CCLK?`. If the returned date is invalid, it performs
an HTTP GET to the dashboard server's `/api/time` endpoint and reads the short
JSON response using the validated A7670G `AT+HTTPREAD` sequence. The server
epoch is anchored to the midpoint of the request round trip so stored sample
ticks can be converted to acquisition-time epoch milliseconds.

`network_time_failures` increments only if both sources fail. Separate Live
Expressions expose modem-clock failures, server-time successes/failures, the
selected source, and the last server response.

## Retry-safe batching

Each POST carries exactly 250 readings, except malformed or legacy short files.
Every body also carries:

- `file_id`: an FNV-1a hash of the complete `.RDY` contents plus its size.
- `batch_index`: zero-based position within that file.

Firmware retries the current JSON batch before abandoning the file. A later
file-level retry reconstructs the same identifiers.

## Server behavior

Node validates `file_id` and `batch_index`, remembers accepted batch keys, and
returns HTTP 200 with `duplicate: true` for retransmissions without rebroadcasting
their readings. The bounded in-memory cache expires old keys. Legacy JSON
without batch identity remains accepted but cannot be deduplicated.

The dashboard counter is relabeled from packets to readings because it counts
individual sample rows received over SSE, not HTTP requests.

## Validation

Source-contract tests verify fixed batch size, deterministic identity,
batch-scoped retry, server-time fallback, server deduplication, and dashboard
labels. The complete PowerShell suite, Node syntax checks, and CubeIDE Debug
build must pass.
