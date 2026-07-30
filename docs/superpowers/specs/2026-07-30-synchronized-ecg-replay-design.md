# Synchronized ECG Replay Design

## Goal

Produce one debugger-exportable, linear 500-sample ADS1292R CH2 capture that
corresponds to a known, repeatable 500-sample PhysioNet replay segment.

## Scope and constraints

- This is controlled bench replay validation only; do not connect the stimulus
  setup to a person.
- Preserve the verified ADS1292R external configuration: CH1 internally
  shorted, CH2 on RA/LA, SPI1 prescaler `/256`, and 250 SPS.
- Do not add a physical synchronization wire. Synchronization is detected from
  the existing differential RA/LA stimulus.
- Retain the existing continuous external statistics and circular buffers for
  live diagnostics.
- Keep the ESP32's one-request/one-two-byte-response serial protocol and its
  250 Hz cadence.

## Design choices

Three synchronization options were considered:

1. A new ESP32-to-STM32 GPIO sync wire provides the most direct timing edge,
   but expands the validated hardware setup.
2. A single large analog pulse needs no wiring but can be confused with an ECG
   transient or analog settling.
3. A short multi-step analog signature followed by quiet settling needs no
   wiring and is distinct from the replay morphology. This is the selected
   design.

## Data flow

Each ESP32 cycle is sent at 250 Hz in this order:

1. Sixteen marker samples: four samples each at positive, negative, positive,
   and negative maximum permitted DAC differential deviation.
2. Thirty-two midpoint DAC samples, providing a 128 ms quiet settling period.
3. The fixed 500-sample source segment.

`stream_ecg.py` will select the source segment with an explicit configurable
start index, encode the exact quantized RA/LA values, and write a companion
CSV. The CSV columns are `sample_index`, `source_mv`, `ra_dac`, `la_dac`, and
`expected_differential_mv`; the final field is the quantized, polarity-aware
differential actually requested at the ADS input.

The STM32 external-capture loop keeps its existing evidence counters and adds
a small state machine. It recognizes the four marker plateaus using a
conservative absolute CH2 threshold and sign sequence, discards the 32 quiet
frames, copies the next 500 CH2 samples linearly into
`ads_capture_ch2[500]`, and freezes that array. The state variables
`ads_sync_detected`, `ads_capture_active`, `ads_capture_frozen`,
`ads_capture_count`, and `ads_capture_sequence` remain `volatile` for
CubeIDE Live Expressions. CH1 remains available only through the existing
continuous diagnostic buffer.

The marker establishes a repeatable cycle boundary, not a false claim of
perfect clock phase. The later analysis removes DC offset and determines the
remaining sample lag by cross-correlation before reporting correlation, gain,
offset, RMSE, and QRS timing error.

## Failure behavior and evidence

- If marker progression breaks, STM32 resets only the synchronization detector
  and continues external acquisition; it does not overwrite a frozen capture.
- SPI, frame, DRDY, and saturation counters retain their current semantics.
- A frozen capture remains intact until reset, so suspending in CubeIDE cannot
  race a circular-buffer writer.
- Success requires the prior external-capture health checks plus a frozen
  500-sample CH2 capture. Fidelity metrics are calculated off-target from the
  exported capture and CSV, not inferred from the marker alone.

## Verification

- Add source-contract tests that assert the marker/quiet/frame lengths,
  protocol framing, CSV fields, and STM32 freeze-state contract.
- Run those tests before and after implementation, then run `make -C Debug
  all -j2`.
- On hardware, verify a marker detection, count of 500, frozen array, zero
  new SPI/frame/DRDY errors, and zero saturation before exporting the buffer.
