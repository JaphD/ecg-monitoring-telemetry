# Record/Upload State Machine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace simultaneous SD logging/uploading with explicit 30-second recording and queued upload phases while preserving every 500 Hz sample.

**Architecture:** The main loop runs a record session, stops ADS acquisition, drains and closes the logger, uploads all ready files, then starts the next session. Boot recovery queues interrupted active data before the first upload phase.

**Tech Stack:** STM32L4 HAL, ADS1292R SPI, LIS3DH I2C, FatFs/SDMMC, A7670G HTTP AT commands, PowerShell source-contract tests.

---

### Task 1: Define phase behavior

**Files:**
- Create: `tests/record_upload_state_machine_test.ps1`
- Modify: `Core/Src/main.c`

- [ ] Write a failing contract requiring 30-second sessions, 2,500-row files, explicit phase values, ADS stop/start functions, boot recovery without opening an active logger, and upload only while acquisition is disabled.
- [ ] Run the test and confirm it fails against the simultaneous architecture.

### Task 2: Implement recording lifecycle

**Files:**
- Modify: `Core/Src/main.c`

- [ ] Split active-file recovery from active-file creation.
- [ ] Add `ADS_StartAcquisition` and `ADS_StopAcquisition` lifecycle functions.
- [ ] Record for 30 seconds, rotating every 2,500 samples.
- [ ] Stop ADS, drain the ring, and finalize the partial active file without reopening it.

### Task 3: Implement upload lifecycle

**Files:**
- Modify: `Core/Src/main.c`

- [ ] Upload ready files only after the logger is closed and acquisition is disabled.
- [ ] Drain the boot queue before the first recording session.
- [ ] Preserve failed files and return to recording after the configured HTTP attempts.
- [ ] Open a new active file only when recording begins.

### Task 4: Verify

**Files:**
- Test: `tests/*.ps1`

- [ ] Run the new state-machine contract.
- [ ] Run all existing production contracts and update stale simultaneous-mode expectations.
- [ ] Build the STM32 Debug target with warnings enabled.
- [ ] Document Live Expression expectations for the board test.

