# V1 JSON UART Integrity Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce valid JSON timestamps and deliver each HTTPDATA body to the A7670G without UART corruption.

**Architecture:** Convert the `uint64_t` timestamp to decimal with a small firmware helper instead of relying on newlib-nano `%llu`. Send the already-built JSON body in paced 512-byte blocking UART chunks while maintaining Live Expressions counters for progress and failures.

**Tech Stack:** STM32L4 HAL C, A7670G AT commands, PowerShell source-contract tests, GNU Arm Embedded build.

## Global Constraints

- Work only on `v1-lipo-modem-gate`.
- Preserve the server-time fallback when `+CCLK` is invalid.
- Preserve the SD-first retry and modem PB8 power-gate lifecycle.
- Do not modify CubeIDE-generated build artifacts as source changes.

---

### Task 1: Add JSON and UART regression contract

**Files:**
- Create: `tests/json_uart_integrity_regression_test.ps1`

**Interfaces:**
- Consumes: `Core/Src/main.c`
- Produces: a source contract that rejects `%llu` timestamps and uninterrupted full-payload UART transmission

- [ ] Add checks for a dedicated `UInt64_ToDecimal` helper, `%s` timestamp insertion, a 512-byte transmit limit, chunk progress accounting, and background service between chunks.
- [ ] Run `powershell -ExecutionPolicy Bypass -File tests/json_uart_integrity_regression_test.ps1`.
- [ ] Confirm it fails against the current implementation.

### Task 2: Implement timestamp conversion and paced HTTPDATA transmission

**Files:**
- Modify: `Core/Src/main.c`

**Interfaces:**
- Produces: `UInt64_ToDecimal(uint64_t, char *, size_t)` and paced transmission inside `HTTP_PostJsonBatch`

- [ ] Add the decimal conversion helper and use its result with `%s` in each JSON row.
- [ ] Send HTTPDATA in chunks no larger than 512 bytes.
- [ ] Call `Background_Service()` and apply a short pacing delay after each non-final chunk.
- [ ] Add Live Expressions counters for chunk count, current chunk size, and chunk transmit failures.
- [ ] Run the new regression test and the complete PowerShell test suite.

### Task 3: Build and review

**Files:**
- Verify: `Core/Src/main.c`
- Verify: `tests/json_uart_integrity_regression_test.ps1`

**Interfaces:**
- Produces: a flashable `Debug/Firmware.elf`

- [ ] Run `make -C Debug all -j2`.
- [ ] Inspect `git diff` and confirm only the intended source, test, and plan changed.
- [ ] Report the Live Expressions and HTTP outcomes to inspect after flashing.
