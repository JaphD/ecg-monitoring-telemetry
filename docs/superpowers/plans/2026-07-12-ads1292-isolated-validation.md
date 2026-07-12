# ADS1292 Isolated Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build debugger-only STM32 firmware that proves ADS1292R SPI, register, conversion, internal-short, internal-test, and ESP32 channel-2 acquisition behavior without SD, IMU, modem, or telemetry code.

**Architecture:** Keep CubeMX clock/SPI/GPIO setup in `main.c`, place ADS transport and the staged validation state machine in a dedicated validation module, and place frame/statistics arithmetic in a HAL-independent module that can be compiled and executed on the Windows host. The firmware automatically advances through digital, internal-short, and internal-square-wave tests before continuously buffering the externally applied channel-2 waveform for Live Expressions.

**Tech Stack:** STM32L452 HAL, SPI1 mode 1, ADS1292R at 250 SPS, PB9 DRDY polling, C11 host tests with MinGW GCC, STM32CubeIDE/ST-Link V2.

## Global Constraints

- Work only on branch `feature/SPI-Test-ADS1292`.
- Do not initialize or service SDMMC, FatFs, I2C, LIS3DH, UART, A7670G, HTTP, or dashboard code.
- Use PB9 for ADS1292R DRDY; do not poll PA0.
- Use RA as `IN2P`, LA as `IN2N`, and channel 2 as the external ECG channel.
- Keep channel 1 internally shorted during external capture.
- Configure both channels for gain x1 during validation.
- Discard the first four DRDY frames after START or a MUX change.
- Preserve all final stage results in volatile Live Expression globals.
- Use a 500-sample external circular buffer per channel, retaining two seconds at 250 SPS.
- Do not connect the bench stimulus circuit to a person.

---

## File Structure

- Create `Core/Inc/ads1292_analysis.h`: HAL-independent frame parsing, statistics types, and pass-criteria interfaces.
- Create `Core/Src/ads1292_analysis.c`: pure C implementations used by firmware and host tests.
- Create `Tests/ads1292_analysis_test.c`: executable unit tests for the pure C logic.
- Create `Core/Inc/ads1292_validation.h`: staged test API and Live Expression declarations.
- Create `Core/Src/ads1292_validation.c`: ADS SPI transport, register verification, DRDY acquisition, staged tests, and external buffers.
- Modify `Core/Src/main.c`: retain only HAL, clock, SPI1, GPIO initialization and call the validation runner.
- Modify `Core/Src/stm32l4xx_hal_msp.c` only if SPI1 GPIO configuration is missing or inconsistent with PA5/PA6/PA7.
- Update `docs/superpowers/specs/2026-07-12-ads1292-isolated-validation-design.md` only if implementation evidence forces a documented decision change.

---

### Task 1: Implement and test HAL-independent ADS frame analysis

**Files:**
- Create: `Core/Inc/ads1292_analysis.h`
- Create: `Core/Src/ads1292_analysis.c`
- Create: `Tests/ads1292_analysis_test.c`

**Interfaces:**
- Produces: `ADS1292_Stats`, `ADS1292_SignExtend24`, `ADS1292_StatusWord`, `ADS1292_StatusValid`, `ADS1292_StatsReset`, `ADS1292_StatsPush`, `ADS1292_StatsMean`, and `ADS1292_InternalTestPass`.
- Consumes: only `<stdint.h>` and `<stdbool.h>`; no STM32 HAL headers.

- [ ] **Step 1: Write the failing host test**

Create `Tests/ads1292_analysis_test.c` with assertions covering zero, positive full-scale, negative full-scale, signed negative conversion, valid/invalid status prefixes, min/max/mean/peak-to-peak accumulation, saturation counting, and internal-test thresholds:

```c
#include "ads1292_analysis.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const uint8_t zero[3] = {0x00, 0x00, 0x00};
    const uint8_t pos_fs[3] = {0x7F, 0xFF, 0xFF};
    const uint8_t neg_fs[3] = {0x80, 0x00, 0x00};
    const uint8_t minus_two[3] = {0xFF, 0xFF, 0xFE};
    const uint8_t valid_frame[9] = {0xC0, 0x00, 0x00, 0,0,0, 0,0,0};
    const uint8_t bad_frame[9] = {0x40, 0x00, 0x00, 0,0,0, 0,0,0};

    assert(ADS1292_SignExtend24(zero) == 0);
    assert(ADS1292_SignExtend24(pos_fs) == 8388607);
    assert(ADS1292_SignExtend24(neg_fs) == -8388608);
    assert(ADS1292_SignExtend24(minus_two) == -2);
    assert(ADS1292_StatusValid(ADS1292_StatusWord(valid_frame)));
    assert(!ADS1292_StatusValid(ADS1292_StatusWord(bad_frame)));

    ADS1292_Stats stats;
    ADS1292_StatsReset(&stats);
    ADS1292_StatsPush(&stats, -3500);
    ADS1292_StatsPush(&stats, 3500);
    assert(stats.count == 2U);
    assert(stats.minimum == -3500);
    assert(stats.maximum == 3500);
    assert(ADS1292_StatsMean(&stats) == 0);
    assert((stats.maximum - stats.minimum) == 7000);
    assert(stats.saturation_count == 0U);
    assert(ADS1292_InternalTestPass(&stats, 6U));

    ADS1292_StatsPush(&stats, 8388607);
    assert(stats.saturation_count == 1U);
    assert(!ADS1292_InternalTestPass(&stats, 6U));

    puts("ads1292_analysis_test: PASS");
    return 0;
}
```

- [ ] **Step 2: Run the test and verify it fails because the module does not exist**

Run:

```powershell
gcc -std=c11 -Wall -Wextra -Werror -ICore/Inc Core/Src/ads1292_analysis.c Tests/ads1292_analysis_test.c -o Tests/ads1292_analysis_test.exe
```

Expected: compilation fails because `ads1292_analysis.h` or the declared functions do not exist.

- [ ] **Step 3: Add the analysis header**

Create `Core/Inc/ads1292_analysis.h`:

```c
#ifndef ADS1292_ANALYSIS_H
#define ADS1292_ANALYSIS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t count;
    int32_t minimum;
    int32_t maximum;
    int64_t sum;
    uint32_t saturation_count;
} ADS1292_Stats;

int32_t ADS1292_SignExtend24(const uint8_t bytes[3]);
uint32_t ADS1292_StatusWord(const uint8_t frame[9]);
bool ADS1292_StatusValid(uint32_t status_word);
void ADS1292_StatsReset(ADS1292_Stats *stats);
void ADS1292_StatsPush(ADS1292_Stats *stats, int32_t sample);
int32_t ADS1292_StatsMean(const ADS1292_Stats *stats);
bool ADS1292_InternalTestPass(const ADS1292_Stats *stats,
                              uint32_t transition_count);

#endif
```

- [ ] **Step 4: Implement the analysis functions**

Create `Core/Src/ads1292_analysis.c`:

```c
#include "ads1292_analysis.h"
#include <limits.h>

int32_t ADS1292_SignExtend24(const uint8_t bytes[3])
{
    int32_t value = ((int32_t)bytes[0] << 16) |
                    ((int32_t)bytes[1] << 8) |
                    (int32_t)bytes[2];
    if ((value & 0x00800000L) != 0) value |= (int32_t)0xFF000000L;
    return value;
}

uint32_t ADS1292_StatusWord(const uint8_t frame[9])
{
    return ((uint32_t)frame[0] << 16) |
           ((uint32_t)frame[1] << 8) |
           (uint32_t)frame[2];
}

bool ADS1292_StatusValid(uint32_t status_word)
{
    return (status_word & 0xF00000UL) == 0xC00000UL;
}

void ADS1292_StatsReset(ADS1292_Stats *stats)
{
    stats->count = 0U;
    stats->minimum = INT32_MAX;
    stats->maximum = INT32_MIN;
    stats->sum = 0;
    stats->saturation_count = 0U;
}

void ADS1292_StatsPush(ADS1292_Stats *stats, int32_t sample)
{
    if (sample < stats->minimum) stats->minimum = sample;
    if (sample > stats->maximum) stats->maximum = sample;
    stats->sum += sample;
    stats->count++;
    if ((sample == 8388607) || (sample == -8388608))
        stats->saturation_count++;
}

int32_t ADS1292_StatsMean(const ADS1292_Stats *stats)
{
    return stats->count == 0U ? 0 : (int32_t)(stats->sum / stats->count);
}

bool ADS1292_InternalTestPass(const ADS1292_Stats *stats,
                              uint32_t transition_count)
{
    if ((stats->count == 0U) || (stats->saturation_count != 0U)) return false;
    if ((stats->minimum > -2800) || (stats->minimum < -4200)) return false;
    if ((stats->maximum < 2800) || (stats->maximum > 4200)) return false;
    return (transition_count >= 4U) && (transition_count <= 8U);
}
```

- [ ] **Step 5: Compile and run the host test**

Run:

```powershell
gcc -std=c11 -Wall -Wextra -Werror -ICore/Inc Core/Src/ads1292_analysis.c Tests/ads1292_analysis_test.c -o Tests/ads1292_analysis_test.exe
./Tests/ads1292_analysis_test.exe
```

Expected: `ads1292_analysis_test: PASS` and exit code 0.

- [ ] **Step 6: Commit the pure analysis unit**

```powershell
git add Core/Inc/ads1292_analysis.h Core/Src/ads1292_analysis.c Tests/ads1292_analysis_test.c
git commit -m "test: add host-tested ADS1292 frame analysis"
```

---

### Task 2: Add ADS transport, register verification, and debugger contract

**Files:**
- Create: `Core/Inc/ads1292_validation.h`
- Create: `Core/Src/ads1292_validation.c`

**Interfaces:**
- Consumes: `SPI_HandleTypeDef`, HAL GPIO/tick functions, and all analysis functions from Task 1.
- Produces: `ADS1292_ValidationInit(SPI_HandleTypeDef *spi)` and non-returning `ADS1292_ValidationRun(void)` plus all volatile Live Expression globals named in the design.

- [ ] **Step 1: Add compile-time register and pin definitions**

Define the ADS register addresses, commands, expected ID, PA4 CS, PC4 RESET/PWDN, PB9 DRDY, stage/result enums, and failure codes in `ads1292_validation.c`. Use `CONFIG1=0x01`, base `CONFIG2=0xA0`, `CHnSET=0x11` for gain-x1 input-short, `RLD_SENS=0x00`, `LOFF_SENS=0x00`, `RESP1=0x02`, `RESP2=0x03`, and `GPIO=0x0C`.

- [ ] **Step 2: Define every Live Expression global**

Add exact volatile definitions matching the specification, including 12-byte expected/readback arrays, short/test statistics, timing counters, 500-element channel buffers, failure code, result, status string, and buffer sequence/index. Initialize stage/result/failure to boot/pending/none.

- [ ] **Step 3: Implement checked SPI transport**

Implement:

```c
static HAL_StatusTypeDef ADS_Command(uint8_t command);
static HAL_StatusTypeDef ADS_WriteRegister(uint8_t address, uint8_t value);
static HAL_StatusTypeDef ADS_ReadRegister(uint8_t address, uint8_t *value);
static HAL_StatusTypeDef ADS_ReadFrame(uint8_t frame[9]);
```

Every helper must hold PA4 low for the complete transaction, check the HAL result, restore CS high on all exits, and increment `ads_spi_error_count` on failure. Command/register delays must meet at least four ADS clock cycles; the existing 2-ms delay is conservative and may be retained.

- [ ] **Step 4: Implement reset, ID, and complete register readback**

Implement the sequence:

```text
CS high -> RESET/PWDN high 2 s -> low 100 ms -> high 200 ms
-> SDATAC -> read ID -> require 0x73
-> write addresses 0x01 through 0x0B where writable
-> read all 12 addresses into ads_register_readback[]
-> compare writable addresses and set ads_register_mismatch_mask
```

Do not compare read-only ID or LOFF_STAT against writable expected values. On failure, set a specific failure code, set `ads_test_result` to failed, preserve the captured arrays, and stop before conversion stages.

- [ ] **Step 5: Compile the firmware translation units**

Run the CubeIDE Debug build or:

```powershell
make -C Debug -j12 all
```

Expected: `ads1292_analysis.c` and `ads1292_validation.c` compile without warnings or undefined references. If CubeIDE has not regenerated managed build files for the new sources, refresh the project and run Project > Build Project before interpreting the result as a source failure.

- [ ] **Step 6: Commit the transport and register stage**

```powershell
git add Core/Inc/ads1292_validation.h Core/Src/ads1292_validation.c
git commit -m "feat: add isolated ADS1292 register validation"
```

---

### Task 3: Implement PB9 acquisition and automatic internal stages

**Files:**
- Modify: `Core/Src/ads1292_validation.c`
- Modify: `Core/Inc/ads1292_validation.h`

**Interfaces:**
- Consumes: Task 1 statistics and Task 2 checked SPI/register transport.
- Produces: completed short-test and internal-test results, measured rate, frame counters, and transition counts.

- [ ] **Step 1: Implement edge-qualified PB9 frame waiting**

Implement `ADS_WaitAndReadFrame(frame, timeout_ms)` so it waits for PB9 to become low, reads exactly one 9-byte frame, and then waits for PB9 to return high before accepting another frame. Do not reference PA0. Record the last 24-bit status word and reject/increment frame errors when its upper nibble is not `0xC`.

- [ ] **Step 2: Implement settling-frame discard**

Implement a helper that reads and discards exactly four valid DRDY frames after START or a channel MUX change. A timeout sets the DRDY-timeout failure code and prevents the next stage from running.

- [ ] **Step 3: Implement the input-short collection stage**

Write both CHnSET registers to `0x11`, verify them, start conversion, enter RDATAC, discard four frames, and collect 500 valid samples. Populate per-channel stats and measured sample rate. Pass only when there are no saturation or frame errors and measured rate is between 240000 and 260000 millihertz. Preserve mean and peak-to-peak values even if the stage fails.

- [ ] **Step 4: Implement the internal-square-wave stage**

Issue SDATAC, set CONFIG2 to `0xA3`, set both CHnSET registers to `0x15`, verify readback, restart conversion, discard four frames, and collect 750 valid samples. Count a transition whenever sample sign differs from the preceding nonzero sample. Use `ADS1292_InternalTestPass` independently for each channel and require both to pass.

- [ ] **Step 5: Extend the host test for boundary conditions**

Add assertions proving that internal-test results fail for amplitude 2000, amplitude 5000, fewer than four transitions, more than eight transitions, and saturation. Recompile and run the host test; expect `PASS`.

- [ ] **Step 6: Build and commit the automatic internal stages**

Run the host test and CubeIDE build. Commit only after both succeed:

```powershell
git add Core/Src/ads1292_validation.c Core/Inc/ads1292_validation.h Tests/ads1292_analysis_test.c
git commit -m "feat: add ADS1292 short and internal signal tests"
```

---

### Task 4: Implement continuous external channel-2 capture

**Files:**
- Modify: `Core/Src/ads1292_validation.c`
- Modify: `Core/Inc/ads1292_validation.h`

**Interfaces:**
- Consumes: validated register/frame/acquisition functions from Tasks 2-3.
- Produces: continuously updated two-second channel buffers and external-stage statistics.

- [ ] **Step 1: Configure control and external channels**

Issue SDATAC, disable the internal test source with CONFIG2 `0xA0`, configure CH1SET `0x11` for gain-x1 input-short, configure CH2SET `0x10` for gain-x1 normal input, keep RLD_SENS `0x00`, verify readback, restart conversion, and discard four frames.

- [ ] **Step 2: Fill debugger-visible circular buffers**

For every valid frame:

```c
ads_external_ch1_latest = channel1;
ads_external_ch2_latest = channel2;
ads_external_ch1_buffer[index] = channel1;
ads_external_ch2_buffer[index] = channel2;
index++;
if (index == 500U) {
    index = 0U;
    ads_external_buffer_sequence++;
}
ads_external_buffer_index = index;
```

Update running min/max and increment `ads_external_saturation_count` for either full-scale code. Keep stage/result set to external-active/internal-tests-passed rather than marking the entire test failed solely because the external circuit saturates.

- [ ] **Step 3: Make external failures diagnostically distinct**

Use separate failure codes for SPI error, DRDY timeout, bad status prefix, register mismatch, and external saturation. Preserve the last valid samples and buffers after a failure. Do not reset or erase earlier internal-stage results.

- [ ] **Step 4: Build and commit external capture**

Run host tests and CubeIDE build. Commit:

```powershell
git add Core/Src/ads1292_validation.c Core/Inc/ads1292_validation.h
git commit -m "feat: add debugger-visible ADS1292 external capture"
```

---

### Task 5: Reduce `main.c` to isolated-test orchestration

**Files:**
- Modify: `Core/Src/main.c`
- Inspect/modify if necessary: `Core/Src/stm32l4xx_hal_msp.c`

**Interfaces:**
- Consumes: `ADS1292_ValidationInit(&hspi1)` and `ADS1292_ValidationRun()`.
- Produces: firmware entry point and SPI/GPIO initialization only.

- [ ] **Step 1: Remove unrelated peripheral initialization from `main()`**

The final entry point must be structurally equivalent to:

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_SPI1_Init();
    ADS1292_ValidationInit(&hspi1);
    ADS1292_ValidationRun();
    while (1) {}
}
```

Remove calls to I2C1, SDMMC1, USART1, FatFs, LIS3DH, modem, and telemetry initialization. Remove obsolete ADS helper functions and globals from `main.c` so there is only one ADS implementation.

- [ ] **Step 2: Correct GPIO configuration**

Configure:

- PA4 as push-pull output, initially high, for CS;
- PC4 as push-pull output, initially low, for RESET/PWDN; and
- PB9 as no-pull input for DRDY.

Do not configure or read PA0 as DRDY. An EXTI handler is not required because the selected isolated implementation uses edge-qualified polling.

- [ ] **Step 3: Verify SPI1 mode and pins**

Retain SPI mode 1 (`CPOL=0`, `CPHA=1`), 8-bit, MSB-first, software NSS, and a conservative prescaler. Confirm `stm32l4xx_hal_msp.c` configures PA5 SCLK, PA6 MISO, and PA7 MOSI for SPI1 alternate function.

- [ ] **Step 4: Build and inspect size**

Run:

```powershell
make -C Debug -j12 all
arm-none-eabi-size Debug/Firmware.elf
```

Expected: build exit code 0, no unresolved ADS symbols, and RAM use comfortably below STM32L452 capacity after adding two 500-element signed buffers.

- [ ] **Step 5: Commit isolated orchestration**

```powershell
git add Core/Src/main.c Core/Src/stm32l4xx_hal_msp.c
git commit -m "refactor: isolate ADS1292 debugger validation firmware"
```

---

### Task 6: Hardware validation and evidence capture

**Files:**
- Modify only if evidence requires correction: `docs/superpowers/specs/2026-07-12-ads1292-isolated-validation-design.md`

**Interfaces:**
- Consumes: flashed firmware, ST-Link V2, STM32CubeIDE Live Expressions, and later the corrected ESP32 bias circuit.
- Produces: stage-by-stage pass/fail evidence and external waveform samples.

- [ ] **Step 1: Flash with ESP32 disconnected**

Build, start a fresh debug session with hardware reset through NRST, run without breakpoints for at least 10 seconds, and inspect the overall, register, short, and internal-test globals.

Expected before external wiring:

- ID `0x73`;
- register mismatch mask `0`;
- no SPI/frame errors;
- approximately 250000 millihertz;
- short test without saturation;
- both internal-test channels around -3495 and +3495 counts;
- internal test pass set for both channels; and
- firmware waiting/running in external mode.

- [ ] **Step 2: Record the complete Live Expression snapshot**

Capture all variables in the order specified by the design. If any internal stage fails, stop external testing and diagnose that exact layer using the retained failure code and arrays.

- [ ] **Step 3: Build the corrected ESP32 bias network**

Use the documented VBIAS divider and decoupling, 1-megaohm DAC injection resistors, 1-kilohm RA/LA-to-VBIAS resistors, common grounds, no series coupling capacitors, and RL disconnected. Verify approximately 1.65 V DC at both RA and LA before connecting them to P300.

- [ ] **Step 4: Run constant-code external test before PhysioNet**

Command both DACs to midpoint. Expected: CH2 remains close to zero differential without saturation. Then offset GPIO25 and GPIO26 by equal and opposite small code steps. Expected: CH2 changes polarity when the offsets are swapped; CH1 remains near its internally shorted baseline.

- [ ] **Step 5: Run PhysioNet stream**

Start the 250-Hz Python/ESP32 stream, allow at least three buffer sequences, pause execution, and inspect both 500-sample buffers. Expected: repeated ECG morphology only in channel 2, no full-scale samples, advancing sequence/index counters, and sample rate near 250 SPS.

- [ ] **Step 6: Final verification commit**

If implementation behavior required no design changes, do not modify the specification. If a documented assumption changed, update the design with measured evidence and commit:

```powershell
git add docs/superpowers/specs/2026-07-12-ads1292-isolated-validation-design.md
git commit -m "docs: record ADS1292 isolated validation results"
```
