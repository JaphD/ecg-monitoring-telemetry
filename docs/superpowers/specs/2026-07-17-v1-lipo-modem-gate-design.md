# V1 LiPo Modem Power Gate Design

## Goal

Adapt the V2 SD-first telemetry firmware for the V1 LiPo-powered board while
keeping the ADS1292R acquisition, LIS3DH capture, microSD queue, and A7670G
HTTP upload behavior intact.  The V1-specific behavior is power-gating the
A7670G battery rail through TPS22969DNYR, controlled directly by STM32 PB8.

## Hardware Contract

- The board runs from a PKCELL LP402535 3.7 V, 330 mAh LiPo for initial tests;
  a compatible 1200 mAh pack may be used later.
- USB-C, TP4056 charging, and the PMOS power path are hardware-controlled and
  require no firmware action.  USB connection powers only charging circuitry;
  normal board operation is from the battery path.
- A TPS22969DNYR gates the raw LiPo rail to the A7670G.
- PB8 drives TPS22969DNYR `ON` directly: HIGH enables the A7670G rail and LOW
  disables it.
- A 10 kOhm pulldown on `ON` keeps the A7670G rail off while PB8 is floating
  during reset and protects against noise.
- SPI1 `/256` and ADS1292R 250 SPS are validated settings and are retained.

## Power and Boot Sequence

1. Reset state: PB8 is held LOW externally; the A7670G rail is OFF.
2. Firmware initializes HAL, 80 MHz clocks, GPIO, I2C1, SDMMC1, SPI1,
   USART1, FatFS, the SD queue, and LIS3DH.  PB8 remains LOW.
3. If an `.RDY` upload batch exists, firmware sets PB8 HIGH and waits 100 ms
   for the TPS22969 output to stabilize before using the modem UART.
4. The existing A7670G sequence probes with `AT`; if needed it holds PWRKEY
   for 600 ms, waits 8 s, and then follows its validated reset recovery path.
   SIM, LTE registration, PDP activation, and IP acquisition run unchanged.
5. The oldest queued CSV file is uploaded.  It is deleted only after HTTP 200.
6. Firmware terminates the HTTP session, waits 100 ms, sets PB8 LOW, and waits
   100 ms for rail discharge before accessing the next application phase.
7. New ECG recording runs with PB8 LOW.  Once a batch is finalized, repeat
   steps 3-6 to upload it.

## Failure and Recovery

- A modem boot or upload failure never discards an `.RDY` file.
- Before the existing 60-second upload retry idle interval, firmware forces
  PB8 LOW and confirms the application modem state is OFF.
- Every later retry begins from a true modem rail power-cycle: PB8 HIGH,
  100 ms settle, then modem boot.
- `Error_Handler` retains its existing terminal behavior; its GPIO reset-state
  and the external pulldown leave the modem rail OFF.

## Live Expressions

Existing V2 variables for ECG, IMU, SD, modem, and HTTP status remain
available.  V1 adds volatile observability for:

- requested PB8 state and confirmed software modem-rail state;
- power-controller stage and last transition tick;
- number of rail enables, disables, and forced retry power-cycles;
- most recent rail-on and rail-off reasons;
- upload retry countdown/state while the rail is intentionally OFF.

The values must let a CubeIDE debugger distinguish: recording with the modem
off, rail settling, modem booting, uploading, successful power-off, and
waiting to retry after failure.

## Scope Boundaries

- No change to TP4056 charging or USB-C PMOS path control is introduced.
- No battery fuel-gauge, USB-present sensing, low-battery shutdown, or MCU
  sleep mode is added in this branch.
- No changes are made to the ADS1292R acquisition format, 250 SPS setting,
  LIS3DH cadence, SD CSV schema, or server endpoint contract.
