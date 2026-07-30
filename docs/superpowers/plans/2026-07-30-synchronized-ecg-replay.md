# Synchronized ECG Replay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replay one known 500-sample PhysioNet segment repeatedly, save its quantized reference CSV, and freeze the corresponding ADS1292R CH2 capture for cross-analysis.

**Architecture:** The Python producer builds one cycle from a 16-sample analog marker, a 32-sample midpoint quiet interval, and the fixed source segment. The ESP32 continues to request one DAC pair every 4 ms. STM32 detects the marker from CH2, skips the quiet frames, copies 500 CH2 values into a linear `volatile` buffer, and stops overwriting it.

**Tech Stack:** Python 3 with NumPy/pySerial/wfdb, ESP32 Arduino DAC driver, STM32L452 HAL/C11, PowerShell source-contract test, GNU Arm Embedded build.

## Global Constraints

- Preserve ADS1292R CH1 internally shorted, CH2 on RA/LA, SPI1 `/256`, and 250 SPS.
- Do not add a physical synchronization wire or alter the ESP32 two-byte serial response protocol.
- The marker is four 4-sample plateaus at logical DAC deviations `+60, -60, +60, -60`; the quiet interval is 32 midpoint samples.
- Each repeated ECG payload contains exactly 500 samples selected by `CAPTURE_START_INDEX`.
- The comparison CSV must contain `sample_index,source_mv,ra_dac,la_dac,expected_differential_mv` for precisely the 500 payload samples.
- Preserve the existing continuous external diagnostics and never overwrite a frozen capture.
- Treat results as bench replay validation, not clinical validation.

---

### Task 1: Define the executable synchronization contract

**Files:**
- Create: `tests/synchronized_ecg_replay_contract_test.ps1`
- Test: `tests/synchronized_ecg_replay_contract_test.ps1`

**Interfaces:**
- Consumes: the three existing source files by their absolute paths.
- Produces: exit code 0 only when producer timing, ESP32 protocol, STM32 capture globals, and freeze behavior use the agreed symbols.

- [ ] **Step 1: Write the failing source-contract test**

Create a PowerShell test that requires `SYNC_MARKER_PLATEAU_SAMPLES = 4`,
`SYNC_QUIET_SAMPLES = 32`, `CAPTURE_SAMPLE_COUNT = 500`,
`ads_capture_ch2[ADS_CAPTURE_LENGTH]`, and a terminal
`ads_capture_frozen = 1U`. It must also require `stream_ecg.py` to call
`csv.writer` and write the five required CSV headers, and require the ESP32
to request and apply one DAC pair per sample as it does today.

- [ ] **Step 2: Run the contract test to verify it fails**

Run: `powershell -ExecutionPolicy Bypass -File tests/synchronized_ecg_replay_contract_test.ps1`

Expected: failure identifying absent synchronization constants and frozen
capture symbols; no production source is changed yet.

- [ ] **Step 3: Keep the test as a narrow structural regression check**

Use direct file-content assertions; avoid importing `stream_ecg.py`, because
its current module-level record loading and serial dependencies make import a
hardware/environment coupling rather than a unit of synchronization behavior.

### Task 2: Make the Python producer emit and record a fixed replay cycle

**Files:**
- Modify: `C:/Users/Yafet/Desktop/Projects/Altium/Thesis/ECG/Testing/ESP32 ECG Test/stream_ecg.py`
- Test: `tests/synchronized_ecg_replay_contract_test.ps1`

**Interfaces:**
- Consumes: `build_differential_dac_waveform()` output.
- Produces: `build_synchronized_replay_cycle()` returning marker/quiet/payload
  RA and LA DAC arrays; `write_capture_reference_csv()` saving only the
  payload reference; the existing stream loop sends the cycle continuously.

- [ ] **Step 1: Implement the smallest producer change required by the test**

Add named constants for the 4-by-4 marker, 32 quiet samples, 500 payload
samples, start index, and CSV path. Slice exactly 500 resampled samples,
construct the marker at `DAC_CENTER ± MAX_DAC_DEVIATION`, construct quiet
samples at `DAC_CENTER`, concatenate the three phases, and write one CSV with
the quantized payload fields before streaming starts.

- [ ] **Step 2: Preserve serial cadence and live plot behavior**

Change `stream_thread()` to index the constructed cycle, send exactly one
RA/LA pair for every `R`, and append to the plot only during the 500-sample
payload. Do not emit serial text in the binary ESP32 protocol.

- [ ] **Step 3: Run the contract test to verify producer requirements pass**

Run: `powershell -ExecutionPolicy Bypass -File tests/synchronized_ecg_replay_contract_test.ps1`

Expected: producer assertions pass; STM32 assertions still fail until Task 3.

### Task 3: Preserve ESP32 timing while documenting synchronized cycles

**Files:**
- Modify: `C:/Users/Yafet/Desktop/Projects/Altium/Thesis/ECG/Testing/ESP32 ECG Test/esp32_dac/esp32_dac.ino`
- Test: `tests/synchronized_ecg_replay_contract_test.ps1`

**Interfaces:**
- Consumes: one `R` request and two bytes from Python.
- Produces: the same byte-for-byte protocol at 250 Hz, explicitly described
  as forwarding marker, quiet, and payload DAC pairs unchanged.

- [ ] **Step 1: Keep the request/response transport behavior unchanged**

Do not add marker synthesis to ESP32: Python owns frame construction. Add
only a narrow comment documenting that every received pair, including marker
and quiet values, is sent to GPIO25/GPIO26 without transformation.

- [ ] **Step 2: Run the contract test**

Run: `powershell -ExecutionPolicy Bypass -File tests/synchronized_ecg_replay_contract_test.ps1`

Expected: ESP32 protocol assertions pass; STM32 assertions still fail until
the freeze state machine is implemented.

### Task 4: Add marker detection and frozen STM32 capture

**Files:**
- Modify: `Core/Src/main.c`
- Test: `tests/synchronized_ecg_replay_contract_test.ps1`

**Interfaces:**
- Consumes: each valid `ch2` sample in `ADS_ExternalCapture()`.
- Produces: volatile `ads_sync_detected`, `ads_capture_active`,
  `ads_capture_frozen`, `ads_capture_count`, `ads_capture_sequence`, and
  `ads_capture_ch2[500]`.

- [ ] **Step 1: Add capture constants and debugger-visible state**

Define `ADS_CAPTURE_LENGTH 500U`, `ADS_SYNC_MARKER_PLATEAU_SAMPLES 4U`,
`ADS_SYNC_QUIET_SAMPLES 32U`, a conservative marker threshold in ADC counts,
and a private sync-state enum. Initialize all new Live Expression globals to
zero.

- [ ] **Step 2: Implement the minimal marker/quiet/capture state machine**

After each successful frame read, keep the existing external diagnostics.
When capture is not frozen, recognize four threshold-qualified CH2 plateaus
with signs `+,-,+,-`, reset detection on any invalid plateau, consume exactly
32 quiet frames, then linearly store the next 500 CH2 samples. Set
`ads_capture_sequence` last, after the 500th array element and count are
visible; set `ads_capture_frozen = 1U` immediately after it. Do not modify
the linear buffer again until reset.

- [ ] **Step 3: Run the complete source-contract test**

Run: `powershell -ExecutionPolicy Bypass -File tests/synchronized_ecg_replay_contract_test.ps1`

Expected: all producer, ESP32, and STM32 contract assertions pass.

- [ ] **Step 4: Build the firmware**

Run: `make -C Debug all -j2`

Expected: exit code 0 and an updated `Debug/Firmware.elf`; do not stage
generated Debug or CubeIDE metadata files.

### Task 5: Review and commit source changes

**Files:**
- Modify: `Core/Src/main.c`
- Create: `tests/synchronized_ecg_replay_contract_test.ps1`
- Modify externally: `stream_ecg.py`, `esp32_dac.ino`

**Interfaces:**
- Consumes: test and build outputs from Tasks 1-4.
- Produces: an intentional firmware repository commit plus externally edited
  test-stimulus sources left visible for the user.

- [ ] **Step 1: Inspect diffs and verify source cleanliness**

Run `git diff --check`, the full source-contract test, and the Debug build.
Inspect the diff to confirm it contains no accidental generated artifacts.

- [ ] **Step 2: Commit repository-owned source and test files**

Stage only `Core/Src/main.c` and
`tests/synchronized_ecg_replay_contract_test.ps1`; commit with
`feat: freeze synchronized ECG replay capture`.

- [ ] **Step 3: Give the hardware validation procedure**

Report the required Live Expressions and the exact CSV/buffer export inputs:
`&ads_capture_ch2[0]` for 2,000 bytes and the generated payload CSV. State
that correlation and timing figures remain pending real hardware capture.
