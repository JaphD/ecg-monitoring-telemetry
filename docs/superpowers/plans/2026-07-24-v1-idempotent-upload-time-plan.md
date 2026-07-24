# V1 Idempotent Upload and Server-Time Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give V1 reliable acquisition timestamps and prevent dashboard duplicates during modem retries.

**Architecture:** Use the existing `+CCLK` path first and the validated A7670G `HTTPREAD` pattern against `/api/time` as fallback. Send deterministic 250-reading batches identified by file hash and batch index; deduplicate those identifiers in Node before SSE broadcast.

**Tech Stack:** STM32L4 HAL C, FatFs, A7670G AT HTTP commands, Node.js/Express, PowerShell source-contract tests.

## Global Constraints

- Work only on `v1-lipo-modem-gate`.
- Preserve 250 SPS acquisition and the SD-first `.RDY` deletion-after-success rule.
- Preserve PB8 TPS22969DNYR modem gating.
- Preserve the main-branch 512-byte UART transport pattern.
- Keep legacy server JSON compatible.

---

### Task 1: Regression contracts

**Files:**
- Create: `tests/v1_idempotent_upload_time_test.ps1`

**Interfaces:**
- Consumes: `Core/Src/main.c`, `Web-Dashboard/server.js`, `Web-Dashboard/dashboard.html`
- Produces: failing checks for server time, deterministic batching, deduplication, and reading labels

- [ ] Add exact source checks for `JSON_READINGS_PER_BATCH 250U`, `file_id`, `batch_index`, FNV-1a, current-batch retry, `/api/time`, `AT+HTTPREAD`, server deduplication, and `READINGS`.
- [ ] Run the test and confirm failure against the current implementation.

### Task 2: Firmware time and upload changes

**Files:**
- Modify: `Core/Src/main.c`

**Interfaces:**
- Produces: `ServerTime_Sync()`, `File_ComputeId()`, `HTTP_PostJsonBatchWithRetry()`

- [ ] Implement manual unsigned-decimal response parsing and `/api/time` HTTP GET fallback.
- [ ] Add Live Expressions for selected time source and server-time diagnostics.
- [ ] Hash the `.RDY` file deterministically, rewind it, and emit 250-reading JSON bodies with stable batch indexes.
- [ ] Retry only the current batch before returning file failure.

### Task 3: Node and dashboard changes

**Files:**
- Modify: `Web-Dashboard/server.js`
- Modify: `Web-Dashboard/dashboard.html`

**Interfaces:**
- Consumes: `file_id`, `batch_index`, `readings`
- Produces: `/api/time`, duplicate-suppressed SSE, accurate reading labels

- [ ] Add `/api/time` returning integer `epoch_ms`.
- [ ] Add a bounded expiring accepted-batch cache.
- [ ] Return HTTP 200 for duplicate keys without queuing their rows.
- [ ] Relabel packet displays and rate units as readings.

### Task 4: Verification and commit

**Files:**
- Verify all modified files.

**Interfaces:**
- Produces: flashable `Debug/Firmware.elf`

- [ ] Run the new regression test.
- [ ] Run all PowerShell tests.
- [ ] Run `node --check server.js`.
- [ ] Run `make -C Debug all -j2` with CubeIDE's bundled GNU Arm toolchain.
- [ ] Review intended diffs and commit source, tests, and design documents.
