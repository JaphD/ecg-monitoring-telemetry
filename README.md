# ECG Monitoring and Telemetry Firmware

Firmware for a wearable ECG monitoring system based on an STM32L452RET6. The platform combines an ADS1292R ECG analog front end, LIS3DH accelerometer, microSD storage, and A7670G LTE modem for local capture and remote telemetry.

This is an engineering and bring-up project. The ECG acquisition and telemetry paths have been exercised during development, but this repository does not represent a certified medical device or clinical diagnostic system.

## Hardware targets and baseline branches

Two physical board variants are supported. Select the branch that matches the board before building, flashing, or changing board-specific behavior.

| Branch | Hardware target | Purpose |
| --- | --- | --- |
| [`main`](../../tree/main) | **Rev 2 — USB-C / external-power version** | Baseline for the revised board, which uses downstream regulation and incorporates improved A7670G power distribution and grounding. |
| [`v1-lipo-modem-gate`](../../tree/v1-lipo-modem-gate) | **Rev 1 — Li-Po battery version** | Baseline for the portable battery-powered board, including its TP4056 charging and MCU-controlled modem power-gating assumptions. |

> **Important:** Rev 1 modem power sequencing and delays are intentional. Do not transfer Rev 1 power-control assumptions to Rev 2, or change the modem enable/startup sequence without hardware-specific validation.

## Peripheral bring-up branches

The following branches preserve focused development and validation work. They are useful reference points when working on an individual subsystem; they are not substitutes for the appropriate Rev 1 or Rev 2 baseline.

| Branch | Focus |
| --- | --- |
| [`board_init`](../../tree/board_init) | Early STM32CubeMX foundation: pin/peripheral setup, clock configuration, and SPI1 8-bit data configuration. This is a historical firmware bring-up branch, not a board variant. |
| [`feature/SWD-Test-ST_Link_V2`](../../tree/feature/SWD-Test-ST_Link_V2) | SWD connectivity and serial-debug validation. |
| [`feature/SPI-Test-ADS1292`](../../tree/feature/SPI-Test-ADS1292) | ADS1292R SPI communication, ECG acquisition, and synchronized replay-capture hardening. |
| [`feature/I2C-Test-LIS3DH`](../../tree/feature/I2C-Test-LIS3DH) | LIS3DH I2C communication and `WHO_AM_I` verification. |
| [`feat/lis3dh-xyz-readout`](../../tree/feat/lis3dh-xyz-readout) | LIS3DH three-axis motion-data readout. |
| [`feature/SDMMC-Test-MicroSD`](../../tree/feature/SDMMC-Test-MicroSD) | SDMMC/FatFS storage and the SD-first telemetry/logging baseline. |
| [`feature/UART-Test-A7670G`](../../tree/feature/UART-Test-A7670G) | A7670G power-up, UART/AT-command verification, and telemetry upload work. |

## Implemented subsystems

- ECG capture through the ADS1292R.
- Motion acquisition through the LIS3DH.
- microSD logging through SDMMC and FatFS.
- A7670G LTE network registration and server uplink.
- STM32 unique-device identity included in telemetry.

## Repository layout

- `Core/` — application code, STM32 startup code, and hardware-abstraction configuration.
- `Drivers/` — STM32 HAL and CMSIS dependencies.
- `FATFS/` and `Middlewares/` — filesystem and storage support.
- `tests/` / `Tests/` — targeted regression and bench-test artifacts.
- `ecg monitoring telemetry.ioc` — STM32CubeMX hardware/project configuration; treat GPIO and peripheral settings as hardware definition.

## Working safely

1. Confirm whether the connected board is Rev 1 or Rev 2 before modifying GPIO, power, modem, or initialization code.
2. Preserve known-good ADS1292R, LIS3DH, microSD, and A7670G behavior unless the task specifically requires a change.
3. Validate changes incrementally: power rails and boot, peripheral communication, acquisition/logging, modem registration, then uplink.
4. Do not treat a build or static test as proof of hardware validation; record the target board and bench result for meaningful firmware milestones.
