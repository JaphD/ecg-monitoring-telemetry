# Synchronized ECG Replay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Repeatedly replay one known 500-sample PhysioNet segment, save its exact quantized reference CSV, and freeze the corresponding ADS1292R CH2 samples for cross-analysis.

**Architecture:** A pure Python helper builds and records one 548-sample cycle: 16 analog marker samples, 32 midpoint quiet samples, and the 500-sample payload. A header-only, HAL-independent C state machine recognizes the marker relative to its tracked CH2 baseline, skips the quiet interval, and freezes the subsequent payload. `stream_ecg.py` and `main.c` are thin integrations of these testable units.

**Tech Stack:** Python 3/NumPy/unittest, ESP32 Arduino DAC request-response protocol, STM32L452 HAL/C11, host GCC, GNU Arm Embedded build.

## Global Constraints

- Preserve CH1 internally shorted, CH2 on RA/LA, SPI1 `/256`, and 250 SPS.
- Do not add a physical synchronization wire or alter ESP32's two-byte serial protocol.
- Marker: four plateaus of four samples at logical DAC deviations `+60,-60,+60,-60`; quiet: 32 midpoint samples.
- Payload: exactly 500 samples selected with `CAPTURE_START_INDEX`.
- CSV header: `sample_index,source_mv,ra_dac,la_dac,expected_differential_mv` for exactly the payload samples.
- Retain continuous external diagnostics; never overwrite the frozen capture.
- This remains bench replay validation, not clinical validation.

---

### Task 1: Test the replay-cycle producer before adding it

**Files:**
- Create: `C:/Users/Yafet/Desktop/Projects/Altium/Thesis/ECG/Testing/ESP32 ECG Test/test_sync_replay.py`
- Create: `C:/Users/Yafet/Desktop/Projects/Altium/Thesis/ECG/Testing/ESP32 ECG Test/sync_replay.py`

**Interfaces:**
- Produces: `build_synchronized_replay_cycle(...)` and `write_capture_reference_csv(...)`.
- Consumes: payload arrays of source millivolts, transmitted millivolts, and RA/LA DAC values.

- [ ] Write a failing `unittest` that supplies a hand-checked 500-element fixture and asserts a 548-element cycle, marker plateau values, 32 midpoint values, unchanged payload values, and a five-column 500-row CSV.
- [ ] Run `python -m unittest test_sync_replay.py -v`; expect module-import failure because `sync_replay.py` does not exist.
- [ ] Implement the pure helper with literal phase lengths and no serial, WFDB, GUI, or hardware dependencies.
- [ ] Re-run the same unittest; expect all tests to pass.

### Task 2: Test the STM32 sync and freeze state machine before integration

**Files:**
- Create: `Tests/ads_sync_capture_test.c`
- Create: `Core/Inc/ads_sync_capture.h`

**Interfaces:**
- Produces: `ADS_SyncCapture_Init()` and `ADS_SyncCapture_Push()`.
- Consumes: one signed CH2 sample at a time and caller-owned 500-element storage.

- [ ] Write a failing host-C test that establishes a `120000`-count baseline, feeds `+5000,-5000,+5000,-5000` marker plateaus, 32 quiet samples, and 500 numbered payload values. Assert capture count 500, frozen true, correct ordered buffer, and immutable buffer after additional samples.
- [ ] Compile with `gcc -std=c11 -Wall -Wextra -Werror -ICore/Inc Tests/ads_sync_capture_test.c -o Tests/ads_sync_capture_test.exe`; expect missing-header failure.
- [ ] Implement a header-only state machine. Marker qualification compares sample delta against a slow baseline, then requires the exact signed four-plateau sequence. Baseline updates only while no marker plateau is active.
- [ ] Compile and execute the host test; expect `ads_sync_capture_test: PASS`.

### Task 3: Integrate the proven Python cycle into the existing producer

**Files:**
- Modify: `C:/Users/Yafet/Desktop/Projects/Altium/Thesis/ECG/Testing/ESP32 ECG Test/stream_ecg.py`
- Modify: `C:/Users/Yafet/Desktop/Projects/Altium/Thesis/ECG/Testing/ESP32 ECG Test/esp32_dac/esp32_dac.ino`
- Test: `test_sync_replay.py`

**Interfaces:**
- Consumes: current `build_differential_dac_waveform()` output and the pure replay helper.
- Produces: a repeated 548-sample send cycle and a reference CSV beside `stream_ecg.py`.

- [ ] Add explicit `CAPTURE_START_INDEX`, call the helper after waveform quantization, and save the payload CSV before opening serial.
- [ ] Change `stream_thread()` to send the cycle one RA/LA pair per ESP32 `R`; append only payload values to the live plot.
- [ ] Keep ESP32 transport implementation unchanged; add a comment that marker, quiet, and payload DAC pairs are forwarded unchanged.
- [ ] Re-run `python -m unittest test_sync_replay.py -v`; expect all tests to pass.

### Task 4: Integrate the proven capture state machine into ADS external capture

**Files:**
- Modify: `Core/Src/main.c`
- Modify: `Core/Inc/ads_sync_capture.h`
- Test: `Tests/ads_sync_capture_test.c`

**Interfaces:**
- Consumes: each valid CH2 sample from `ADS_ExternalCapture()`.
- Produces: `volatile` `ads_sync_detected`, `ads_capture_active`, `ads_capture_frozen`, `ads_capture_count`, `ads_capture_sequence`, and `ads_capture_ch2[500]`.

- [ ] Include the header, define one static `ADS_SyncCapture` instance, and define the Live Expression globals without altering existing external buffers or counters.
- [ ] Initialize the state machine before entering continuous external capture.
- [ ] Push each valid CH2 value into the state machine only while not frozen; mirror its observable state into the volatile globals and assign `ads_capture_sequence` last on freeze.
- [ ] Re-run the host test, then run `make -C Debug all -j2`; expect both exit 0. Do not stage generated Debug or IDE artifacts.

### Task 5: Review, commit, and hand off hardware validation

**Files:**
- Modify: `Core/Src/main.c`
- Create: `Core/Inc/ads_sync_capture.h`, `Tests/ads_sync_capture_test.c`
- Externally modify: `stream_ecg.py`, `sync_replay.py`, `test_sync_replay.py`, `esp32_dac.ino`

- [ ] Run Python unit tests, host-C test, `git diff --check`, and the Debug build.
- [ ] Commit only repository-owned source and host test files as `feat: freeze synchronized ECG replay capture`.
- [ ] Report the required hardware export: payload CSV plus `&ads_capture_ch2[0]` for 2,000 bytes. State that measured correlation and QRS timing remain pending the real capture.
