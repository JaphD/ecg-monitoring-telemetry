# ADS1292 Stream Startup Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore continuous ADS1292R sampling so SD batches are created and passed to the already-working A7670G upload path.

**Architecture:** Keep the SD-first ring-buffer architecture, but restore the ADS1292R startup order validated by commit `b3feab2` and the sensor branches: issue `START`, wait, then issue `RDATAC`. Arm PB9 EXTI only after the device is streaming, and bootstrap one frame when DRDY is already low so acquisition cannot depend on observing the first edge.

**Tech Stack:** STM32L4 HAL, ADS1292R SPI Mode 1, PB9 EXTI, PowerShell source-contract tests, GNU Arm Embedded build.

---

### Task 1: Restore and verify ADS1292R streaming startup

**Files:**
- Create: `tests/ads_stream_startup_regression_test.ps1`
- Modify: `Core/Src/main.c`

- [ ] **Step 1: Write the failing regression test**

The test extracts `ADS_Init`, checks that `ADS_Command(0x08U)` occurs before `ADS_Command(0x10U)`, and requires post-RDATAC EXTI arming plus a low-DRDY bootstrap capture.

- [ ] **Step 2: Run the regression test and verify RED**

Run: `powershell -ExecutionPolicy Bypass -File tests/ads_stream_startup_regression_test.ps1`

Expected: FAIL because the current rewrite issues RDATAC before START.

- [ ] **Step 3: Apply the minimal startup fix**

In `ADS_Init`, preserve the validated register values, then use this ordering:

```c
ADS_Command(0x08U); /* START */
HAL_Delay(10U);
ADS_Command(0x10U); /* RDATAC */
HAL_Delay(10U);
__HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_9);
HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
acquisition_enabled = 1U;
if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_RESET)
    ADS_CaptureFromISR();
```

Expose stream stage, DRDY pin state, and CONFIG1 readback as Live Expressions so a hardware run distinguishes command/configuration failure from EXTI failure.

- [ ] **Step 4: Verify GREEN and run the full contract suite**

Run every `tests/*.ps1` script. Expected: PASS.

- [ ] **Step 5: Build the Debug firmware**

Run `make -j4 all` from `Debug` with the STM32 GNU toolchain on `PATH`. Expected: `Firmware.elf` links without warnings or errors.

