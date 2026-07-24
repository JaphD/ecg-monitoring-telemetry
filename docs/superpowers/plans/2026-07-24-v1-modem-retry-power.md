# V1 Modem Retry Power Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep the V1 A7670G rail powered across queued-file retries and turn it off only after the queue drains.

**Architecture:** Retain the existing upload state machine and 60-second cooldown. Remove only the three intermediate calls that disable PB8 on boot failure, deferred HTTP upload, and retry idle; preserve the successful queue-drained shutdown.

**Tech Stack:** STM32L452 C firmware, PowerShell source-contract tests, GNU Arm Embedded build

## Global Constraints

- Preserve the existing modem command sequence and upload payload.
- Preserve the 60-second retry interval.
- Preserve `ModemPower_Disable("upload complete")`.
- Do not modify unrelated CubeIDE-generated files.

---

### Task 1: Enforce the retry power contract

**Files:**
- Modify: `tests/v1_lipo_modem_power_gate_test.ps1`
- Modify: `Core/Src/main.c`

**Interfaces:**
- Consumes: `ModemPower_BootForUpload()`, `Run_UploadPhase()`, and `Drain_UploadQueueBeforeNextRecord()`
- Produces: PB8 remains enabled until the upload queue drains

- [ ] **Step 1: Write the failing test**

Require the source not to contain intermediate `modem boot failed`, `upload deferred`, or `upload retry` power-off calls, while still requiring the cooldown and `upload complete` shutdown.

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -ExecutionPolicy Bypass -File tests/v1_lipo_modem_power_gate_test.ps1`

Expected: FAIL because the current source still disables PB8 during retry.

- [ ] **Step 3: Write minimal implementation**

Remove the three intermediate `ModemPower_Disable(...)` calls. Do not change modem commands, delays, queue handling, or the successful shutdown.

- [ ] **Step 4: Run verification**

Run the targeted test, all `tests/*.ps1`, and `make -C Debug all -j2`.

Expected: all tests pass and the build exits with status 0.

- [ ] **Step 5: Commit intended files**

Commit only `Core/Src/main.c`, the updated test, this plan, and the corresponding design document.
