# ECG Firmware — V1 Li-Po Modem-Gate Board

This branch supports the first portable ECG board: an STM32L452RET6 with an ADS1292R ECG front end, LIS3DH accelerometer, microSD storage, and an A7670G modem powered from the Li-Po rail through an MCU-controlled load switch. It is an engineering prototype and is not a certified medical device.

## Current baseline

- **Branch:** `v1-lipo-modem-gate`
- **Firmware baseline:** `c485627`
- **Server endpoint:** `https://ecg-dashboard-1h8s.onrender.com/api/ingest`
- **Last combined test:** 21 recordings and 21 successful HTTP 200 uploads, with no upload, acquisition, or SD errors observed during that run.

The modem power gate, PWRKEY sequence, registration delays, and retry behavior are specific to V1. Do not replace them with assumptions from the externally powered V2 board without a new hardware test.

## Runtime sequence

1. Initialize the STM32 unique device ID, peripherals, microSD, LIS3DH, and ADS1292R.
2. Recover any completed `.RDY` files left on the SD card from an earlier reset or failed connection.
3. Enable and initialize the A7670G, wait for LTE registration, and establish a PDP context.
4. Drain previously queued files before starting another recording.
5. Capture 2,500 ADS1292R samples over approximately 10 seconds at 250 samples per second. LIS3DH XYZ readings are included in the same CSV rows.
6. Write the recording to `ACTIVE.TMP`, synchronize it, then rename the completed file to `.RDY`.
7. Upload queued `.RDY` files over HTTPS. The modem retries transient failures, including TLS-related HTTP 715 responses.
8. Delete a queued file only after HTTP 200. Otherwise retain it on the SD card and retry later.
9. Repeat the record-and-upload cycle.

## Telemetry format

Each file starts with the STM32 device identity followed by:

```text
timestamp,accel_x,accel_y,accel_z,ecg_ch1,ecg_ch2
```

CH2 carries the RA–LA ECG measurement. CH1 is internally shorted for diagnostic use. Motion is sampled at a lower effective rate and repeated across ECG rows.

## Reliability behavior

- SD storage is the source of truth until the server acknowledges an upload.
- Startup queue recovery preserves completed recordings across resets.
- ADS1292R start, frame, timing, and SPI failures are exposed through debugger diagnostics.
- SD write, synchronization, finalization, and queue failures remain visible and trigger recovery instead of silent deletion.
- The modem remains powered during normal repeated uploads to avoid unnecessary reconnect cycles.

## Known limitation

The firmware can query `AT+CBC` and attach `X-Battery-Millivolts`, but the tested board returned no usable reading: `battery_voltage_mv` remained zero and every query failed. Battery percentage is therefore not currently available or validated. Capture `last_modem_response` and `battery_last_response` during a future `AT+CBC` test before changing the implementation.

## Important files

- `Core/Src/main.c` — acquisition, storage, modem, and application state flow.
- `Core/Src/stm32l4xx_it.c` — ADS1292R data-ready interrupt handling.
- `FATFS/Target/` — SDMMC/FatFS integration.
- `ecg monitoring telemetry.ioc` — CubeMX hardware configuration.
- `docs/SD_FIRST_TELEMETRY.md` — detailed storage and queue behavior.

Preserve the known-good ADS1292R setup, SPI timing, SDMMC clock divider, and modem power sequence unless a change is supported by hardware evidence.
