# ADS1292 Isolated Validation Design

Date: 2026-07-12  
Branch: `feature/SPI-Test-ADS1292`

## Objective

Create an isolated STM32 firmware test that proves, through STM32CubeIDE Live Expressions, that the ADS1292R:

1. communicates reliably over SPI;
2. accepts and retains the intended register configuration;
3. produces correctly framed samples at 250 SPS;
4. measures its internally shorted inputs without saturation;
5. reproduces the internal 1-Hz test square wave with the expected amplitude; and
6. captures a correctly biased differential ECG stimulus from an ESP32 DAC on channel 2.

The test excludes SD, FatFs, LIS3DH, UART telemetry, the A7670G modem, HTTP, and the web dashboard. The debugger is the only observation interface.

## Evidence and Constraints

### Board signal routing

The `Sensors_AFE.pdf` schematic establishes the following routing:

- P300 pin 1 is LA.
- P300 pin 2 is RA.
- P300 pin 3 is RL.
- RA connects through R305 (0 ohm) to ADS1292R `IN2P`.
- LA connects through R308 (0 ohm) to ADS1292R `IN2N`.
- RA and LA also pass through the respiration modulation network, but remain one differential ECG lead on ADC channel 2.
- Channel 1 has a separate mid-supply bias network made from two 10-megaohm dividers.
- RL connects to `RLDOUT` through R304 (100 kilohms).

Therefore, GPIO25 and GPIO26 do not stimulate ADS channel 1 and channel 2 independently. They generate the positive and negative halves of the single channel-2 differential stimulus.

### Previous external-stimulus problem

The previous ESP32 interface attenuated each DAC output with a 1-megaohm:1-kilohm divider to ground and then AC-coupled the result. This reduced the approximately 1.65-V DAC common-mode to approximately 1.6 mV and then removed the remaining DC component. Channel 2 consequently lacked a defined valid common-mode voltage.

This can produce full-scale ADS codes even when the differential waveform looks reasonable. The repeated `8388607` value seen previously is the positive 24-bit full-scale saturation code.

### Firmware discrepancies in the existing test branch

The existing branch must not be treated as a validated external-signal test because:

- it polls PA0 for DRDY, while the production firmware that completed multi-hour tests uses PB9;
- it labels `CHnSET = 0x10` as gain x2, although this value selects gain x1;
- it describes `RLD_SENS = 0x23` as deriving RLD from both channels, although it selects channel-1 inputs only;
- it describes `CONFIG2 = 0xE0` as enabling CLK output, although `CLK_EN` is bit 3 and remains clear; and
- it validates only CONFIG1 readback instead of the complete writable register set.

## Selected Approach

Use an automatic staged self-test. Each stage leaves its final results in dedicated volatile globals so Live Expressions can be inspected after the firmware advances.

The alternatives were rejected as follows:

- A debugger-selected manual mode would be flexible but less repeatable and could disturb timing when variables are changed while conversions run.
- An external-input-only test would not distinguish an ADS/SPI/configuration failure from an analog common-mode or ESP32 stimulus failure.

## Firmware Architecture

### Enabled peripherals

The isolated firmware initializes only:

- HAL and the system clock;
- SPI1 in mode 1, MSB first;
- GPIO for ADS CS and RESET/PWDN;
- PB9 as the ADS DRDY input; and
- the PB9 falling-edge interrupt or an edge-qualified polling fallback.

The firmware does not initialize or service SDMMC, FatFs, I2C, LIS3DH, UART, modem, HTTP, or telemetry code.

### Acquisition rules

- CS remains low for the complete 9-byte frame.
- Each frame contains three status bytes, three channel-1 bytes, and three channel-2 bytes.
- Samples are sign-extended from 24 bits to signed 32-bit values.
- The status prefix is validated against the ADS1292R `0xCxxxxx` format.
- The first four DRDY frames after START or a multiplexer change are discarded to allow the digital filter to settle.
- A frame is read once per PB9 falling edge.
- DRDY rate is measured over a timed observation window and compared with 250 SPS.
- SPI HAL return values are checked and counted.

## Test Stages

### Stage 0 - reset and identification

1. Hold CS high.
2. Execute the datasheet-compliant RESET/PWDN sequence.
3. issue `SDATAC` before any register access because RDATAC is the power-up default;
4. read the ID register; and
5. require ADS1292R ID `0x73`.

Failure stops the sequence and retains the observed ID and failure stage.

### Stage 1 - complete register verification

Write a deterministic base configuration for:

- CONFIG1: continuous conversion, 250 SPS;
- CONFIG2: internal 2.42-V reference enabled, internal test disabled;
- LOFF: default/no active lead-off test;
- CH1SET and CH2SET: gain x1 with the stage-specific MUX selection;
- RLD_SENS: RLD disabled for the initial isolated tests;
- LOFF_SENS: disabled;
- RESP1 and RESP2: respiration disabled while preserving required fixed bits; and
- GPIO: known/default configuration.

Read all writable registers back into a debugger-visible array. Create a mismatch bit mask that identifies each register whose value differs from the expected value.

### Stage 2 - internally shorted-input test

Configure both channels with `MUX = 0001`, which connects each input pair to the device's internal common-mode voltage.

After discarding four frames, capture a fixed block and calculate independently for each channel:

- sample count;
- minimum;
- maximum;
- signed mean;
- peak-to-peak code spread;
- saturation count; and
- status/frame error count.

Acceptance criteria:

- no full-scale positive or negative samples;
- no frame-prefix errors;
- sample rate within a conservative tolerance of 250 SPS; and
- mean and peak-to-peak values recorded for comparison with the datasheet and between the two physical boards.

The first implementation records noise metrics without enforcing an unrealistically tight noise threshold. A threshold can be added after baseline measurements from both boards are available.

### Stage 3 - internal 1-Hz square-wave test

Configure both channels with:

- gain x1;
- `MUX = 0101` for the internal test source;
- CONFIG2 internal test enabled; and
- TEST_FREQ set to the 1-Hz square wave.

With the 2.42-V reference, the internal test level is:

`+/- VREF / 2400 = approximately +/-1.008 mV`

At gain x1, the expected signed output level is approximately:

`1.008 mV / 2.42 V * 8388607 = approximately 3495 counts`

The expected peak-to-peak result is approximately 6990 counts.

Acceptance criteria:

- both positive and negative plateaus are observed;
- plateau magnitude is within a conservative 20 percent tolerance of 3495 counts;
- positive and negative magnitudes are reasonably symmetric;
- transitions are consistent with a 1-Hz square wave;
- no saturation occurs; and
- both channels report comparable results.

### Stage 4 - external ESP32 channel-2 capture

Configure:

- channel 1 as internally shorted at gain x1, providing a control channel;
- channel 2 for normal electrode input at gain x1;
- RLD disabled; and
- respiration modulation disabled.

Capture channel 2 continuously into a volatile circular sample buffer. Maintain channel-1 and channel-2 statistics, status-error counts, saturation counts, sample rate, and buffer sequence number.

The test must identify channel 2 as the external ECG channel. Channel 1 is not expected to reproduce the ESP32 waveform.

## Live Expression Contract

The implementation will expose clearly named volatile values in this order:

### Overall state

- `ads_test_stage`
- `ads_test_result`
- `ads_test_status`
- `ads_failure_code`

### SPI and register validation

- `ads_id_value`
- `ads_register_expected[12]`
- `ads_register_readback[12]`
- `ads_register_mismatch_mask`
- `ads_spi_error_count`
- `ads_frame_error_count`
- `ads_last_status_word`

### Timing

- `ads_drdy_count`
- `ads_sample_count`
- `ads_measured_rate_millihz`
- `ads_drdy_interval_min_us`
- `ads_drdy_interval_max_us`

### Input-short results

- `ads_short_pass`
- `ads_short_ch1_min`, `ads_short_ch1_max`, `ads_short_ch1_mean`, `ads_short_ch1_pp`
- `ads_short_ch2_min`, `ads_short_ch2_max`, `ads_short_ch2_mean`, `ads_short_ch2_pp`

### Internal-test results

- `ads_internal_test_pass`
- `ads_test_ch1_min`, `ads_test_ch1_max`, `ads_test_ch1_pp`
- `ads_test_ch2_min`, `ads_test_ch2_max`, `ads_test_ch2_pp`
- `ads_test_ch1_transitions`
- `ads_test_ch2_transitions`

### External capture

- `ads_external_active`
- `ads_external_ch1_latest`
- `ads_external_ch2_latest`
- `ads_external_ch1_min`, `ads_external_ch1_max`
- `ads_external_ch2_min`, `ads_external_ch2_max`
- `ads_external_saturation_count`
- `ads_external_buffer_index`
- `ads_external_buffer_sequence`
- `ads_external_ch1_buffer[]`
- `ads_external_ch2_buffer[]`

The rolling buffers must be large enough to retain at least two seconds of data at 250 SPS without consuming excessive STM32 RAM.

## ESP32 Bench Interface

The external validation circuit must preserve a nominal 1.65-V common-mode while attenuating the ESP32 DAC differential signal.

Create VBIAS:

```text
ESP32 3.3 V -- 10 kOhm --+-- 10 kOhm -- GND
                         |
                       VBIAS
                         |
                    10 uF || 100 nF
                         |
                        GND
```

Connect the stimulus:

```text
GPIO25 -- 1 MOhm --+---- RA / IN2P
                   |
                  1 kOhm
                   |
                  VBIAS

GPIO26 -- 1 MOhm --+---- LA / IN2N
                   |
                  1 kOhm
                   |
                  VBIAS

ESP32 GND -------------- ECG board GND
RL left disconnected during this test
```

Do not place a series coupling capacitor in either DAC injection path. With 1-megaohm injection and 1-kilohm bias resistors, the approximate attenuation is 1:1000 while RA and LA remain near 1.65 V.

The Python/ESP32 stream must drive GPIO25 and GPIO26 complementarily around DAC midpoint. The sign convention is:

`channel 2 differential input = RA - LA = IN2P - IN2N`

Reversing GPIO25 and GPIO26 reverses ECG polarity but does not invalidate the ADC.

## Safety Boundary

This interface is for powered board-to-board bench testing only. It must never be connected to a person while either board is USB-connected or otherwise non-isolated.

## Expected Diagnostic Interpretation

- Stage 1 fails: SPI command sequencing, pin mapping, reset, clock, or register access problem.
- Stage 1 passes but Stage 2 fails: ADC conversion/frame alignment, reference, supply, or device problem.
- Stage 2 passes but Stage 3 fails: internal test configuration, channel MUX, gain, or sample parsing problem.
- Stages 1-3 pass but Stage 4 rails: external common-mode, grounding, attenuation, or ESP32 stimulus problem.
- Stage 4 shows the waveform only on channel 2: expected schematic behavior.

## Authoritative References

- Texas Instruments, ADS1291/ADS1292/ADS1292R datasheet, SBAS502C.
- TI E2E, ADS1292 SPI communication and internal test-signal recommendation.
- TI E2E, ADS1292 channel-read issue and input common-mode guidance.
- TI E2E, ADS1292 conversion settling and DRDY timing guidance.
- Project schematic: `C:/Users/Yafet/Desktop/Projects/Altium/Thesis/ECG/Sensors_AFE.pdf`.
