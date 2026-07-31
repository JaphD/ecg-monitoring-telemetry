# Validated ADS1292R Production Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the simplified production ADS1292R SPI/startup path with the complete protocol proven on `feature/SPI-Test-ADS1292`, while preserving the production sample ring, IMU, SD logger, and modem uploader.

**Architecture:** Keep `Core/Src/main.c` as the production integration point. Port the validated ADS command transport, command-mode recovery, register write/readback verification, and settling-frame startup into the existing retry wrapper; once startup succeeds, accepted CH1/CH2 samples continue into the existing ring unchanged.

**Tech Stack:** STM32L452 HAL, ADS1292R over SPI1, PowerShell source-contract regression tests, STM32CubeIDE generated Make build.

## Global Constraints

- Preserve ADS1292R 250 SPS and SPI1 prescaler `/256`.
- Preserve PA0 as the ADS1292R DRDY input.
- Preserve CH1 internal short (`0x11`) and CH2 normal electrode input (`0x00`).
- Preserve the production 2,500-sample ECG/IMU logging window and upload flow.
- Do not restore the synchronized 10-second bench-capture buffer.
- Do not commit before the user completes hardware validation.

---

### Task 1: Lock Down the Validated ADS Transport Contract

**Files:**
- Create: `tests/ads_validated_transport_port_test.ps1`
- Test: `tests/ads_validated_transport_port_test.ps1`

**Interfaces:**
- Consumes: `Core/Src/main.c`
- Produces: a regression contract for guarded commands, command-mode entry, register verification, settling frames, and SPI configuration

- [ ] **Step 1: Write the failing test**

Create a PowerShell test that isolates the ADS functions and verifies:

- WREG and RREG bytes are transmitted individually with `ADS_SPI_GUARD_MS`.
- command mode performs `SDATAC`, ID probe, `STOP`, and a second ID probe.
- all production configuration registers use verified writes.
- stream startup discards exactly `ADS_SETTLING_FRAMES` frames.
- SPI1 uses `SPI_NSS_PULSE_DISABLE`.

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/ads_validated_transport_port_test.ps1
```

Expected: FAIL because the simplified production transport sends WREG/RREG as bursts and lacks the validated command-mode and settling-frame functions.

### Task 2: Port the Validated ADS Protocol

**Files:**
- Modify: `Core/Src/main.c`
- Test: `tests/ads_validated_transport_port_test.ps1`

**Interfaces:**
- Consumes: SPI1, PA4 CS, PC4 RESET/PWDN, PA0 DRDY
- Produces: `ADS_ConfigureAndStartAttempt()` that returns `HAL_OK` only after verified configuration and valid settling-frame acquisition

- [ ] **Step 1: Implement the minimal validated transport**

Add HAL-status-returning guarded `ADS_Command`, `ADS_WriteRegister`, and `ADS_ReadRegister` functions matching the tested branch.

- [ ] **Step 2: Implement deterministic command-mode entry**

Add ID probing plus `SDATAC -> probe -> STOP -> probe`, with two attempts and a hardware-reset recovery.

- [ ] **Step 3: Verify the production register set**

Write and read back CONFIG1, CONFIG2, LOFF, CH1SET, CH2SET, RLD_SENS, LOFF_SENS, RESP1, and RESP2. Fail startup on a mismatch and expose the register/expected/readback through stable diagnostics.

- [ ] **Step 4: Start and settle**

Send `START`, then `RDATAC`, discard four complete DRDY frames, and only then enable the production acquisition ring.

- [ ] **Step 5: Align SPI1 NSS behavior**

Set `hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE`.

- [ ] **Step 6: Run the focused test**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/ads_validated_transport_port_test.ps1
```

Expected: PASS.

### Task 3: Verify Production Integration

**Files:**
- Verify: `Core/Src/main.c`
- Verify: `tests/*.ps1`
- Verify: `Debug/makefile`

**Interfaces:**
- Consumes: the completed ADS port
- Produces: a buildable image retaining SD-first recording and upload behavior

- [ ] **Step 1: Run all PowerShell contracts**

Run every `tests/*.ps1` file and stop on the first failure.

- [ ] **Step 2: Build the firmware**

Run:

```powershell
make -C Debug all -j2
```

Expected: successful link of `Debug/Firmware.elf`.

- [ ] **Step 3: Report hardware validation probes**

Provide the exact Live Expressions that distinguish command-mode, register-verification, settling-frame, DRDY, frame-status, sampling-rate, and ring/logging success.
