# ECG Device Identity and Clinical-Focus Dashboard Design

Date: 2026-07-31

## Objective

Add a stable hardware identity to each STM32 telemetry upload and redesign the dashboard so ADS1292R Channel 2 is the primary waveform while Channel 1, IMU data, and pipeline health remain accessible.

The first registered board is displayed as **ECG Monitor 01**. Its immutable identity comes from the STM32L452 factory-programmed 96-bit unique device ID (UID).

## Scope

This change covers:

- STM32 UID acquisition and formatting in firmware.
- UID metadata in SD-backed CSV uploads.
- Server-side UID parsing and friendly-name mapping.
- Backward-compatible handling of already queued six-column CSV files.
- SSE messages that carry device and upload context.
- An ECG-first dashboard based on the approved Clinical Focus layout.
- Parser, browser, source-contract, and firmware build verification.

This change does not add multi-device selection, authentication, a persistent database, clinical ECG interpretation, alarm generation, or remote configuration.

## Architecture

The data path remains SD-first:

1. The STM32 acquires ADS1292R and LIS3DH samples.
2. The logger writes a 2,500-row, 10-second CSV file to the SD card.
3. The firmware closes and renames the file to the existing ready-queue format.
4. The A7670G uploads the complete file as `text/csv`.
5. The Node.js server parses the upload, resolves its friendly device name, and sends upload metadata plus sample rows over SSE.
6. The browser renders the complete Channel 2 window and compact supporting telemetry.

No identity state is inferred from the modem, SD filename, network address, or browser connection.

## Firmware Device Identity

At startup, the firmware reads:

- `HAL_GetUIDw0()`
- `HAL_GetUIDw1()`
- `HAL_GetUIDw2()`

The three words are formatted in word order as uppercase, zero-padded hexadecimal:

```text
STM32-W0W1W2
```

where each word contributes exactly eight hexadecimal characters. The resulting identifier therefore has the form:

```text
STM32-003A002B123456789ABC91C2
```

The firmware keeps this string in a stable global `volatile` character buffer named `device_id` so it can be inspected in STM32CubeIDE Live Expressions.

The UID is immutable hardware identity. The friendly name is not stored in firmware and changing the friendly name must not require reflashing the board.

## CSV Contract

New files begin with one device metadata line, followed by the existing header and sample rows:

```csv
device_id,STM32-003A002B123456789ABC91C2
timestamp,accel_x,accel_y,accel_z,ecg_ch1,ecg_ch2
1234,30,-5,1024,-942,-3729
```

Requirements:

- The metadata line appears exactly once per file.
- The identifier is not repeated in sample rows.
- Sample rows retain the established six-column order and numeric representation.
- The additional metadata remains far below the A7670G `HTTPDATA` size limit.
- The active file, queued `.RDY` file, and uploaded bytes contain identical identity metadata.

The firmware upload mechanism and `Content-Type: text/csv` remain unchanged.

## Friendly-Name Mapping

The server owns the alias mapping. The initial mapping resolves the detected board UID to:

```text
ECG Monitor 01
```

The mapping is isolated in one server-side configuration object so another UID/name pair can be added later without changing the parser or dashboard.

Because the exact hardware UID is only known at runtime, an unknown well-formed UID is displayed as `Unregistered ECG Monitor` and logged by the server. After the first firmware run reveals `device_id`, that exact UID is entered into the alias map as `ECG Monitor 01`.

The dashboard shows the friendly name prominently and a shortened UID below it. The complete UID is available through the device element's tooltip.

## Legacy Queue Compatibility

Queued files created before this change contain only the existing header and six-column sample rows. The server accepts these files during migration and labels them:

```text
ECG Monitor 01 · Legacy
```

Legacy status applies only when the upload has no `device_id` metadata line. A malformed line that starts with `device_id` is not treated as legacy.

This compatibility prevents valid data already stored on the SD card from being discarded. It can be removed in a later version after all legacy queue files have drained.

## Server Parsing and HTTP Semantics

Each HTTP request has independent parser state containing:

- Device ID and resolved friendly name.
- Whether the upload is new-format or legacy.
- Accepted and invalid sample counts.
- First and last sample timestamps.

Parsing rules:

1. A valid `device_id` line is consumed as metadata, not a sample.
2. The established sample header is skipped.
3. A valid sample contains exactly six finite numeric values.
4. New-format data received before valid identity metadata is not broadcast.
5. A UID change within one request invalidates the request.
6. Sample rows from different HTTP requests are never merged into one upload batch.

The server returns HTTP 200 only when the upload is accepted under either the new-format or explicit legacy path and contains at least one valid sample. A malformed identity, zero valid samples, or structurally invalid new-format upload receives a non-200 response. This ensures the firmware does not treat rejected data as successfully uploaded and delete the queued `.RDY` file.

## SSE Contract

The server sends one upload envelope rather than making the dashboard infer batch boundaries from a continuous row stream:

```json
{
  "type": "telemetry_batch",
  "device": {
    "id": "STM32-003A002B123456789ABC91C2",
    "name": "ECG Monitor 01",
    "legacy": false
  },
  "upload": {
    "accepted": 2500,
    "invalid": 0,
    "received_at": "2026-07-31T19:00:00.000Z"
  },
  "rows": []
}
```

`rows` contains the established sample objects with `timestamp`, `accel_x`, `accel_y`, `accel_z`, `ecg_ch1`, and `ecg_ch2`.

The server may internally chunk SSE transmission to avoid oversized writes, but every chunk includes the same device identity and an upload identifier. The dashboard assembles only chunks belonging to that upload before replacing the displayed 10-second window.

## Clinical-Focus Dashboard

### Header

The header displays:

- Friendly name: `ECG Monitor 01`.
- Short UID with the complete UID in a tooltip.
- Live, disconnected, legacy, or identity-warning state.
- Time of the most recently accepted upload.

### Primary Channel 2 Region

ADS1292R Channel 2 occupies approximately 70 to 75 percent of the usable dashboard area.

The chart:

- Displays one complete 2,500-sample, 10-second upload window.
- Uses a linear, unsmoothed line with zero-radius points.
- Disables chart animation and automatic decimation.
- Preserves all samples for hover inspection.
- Labels the horizontal axis in seconds and the vertical axis in raw ADS counts.
- Does not draw a line between separate recording windows.
- Replaces the previous complete window atomically after the next upload has been assembled.

The adjacent compact metrics rail shows:

- Current Channel 2 value.
- Minimum, maximum, mean, and peak-to-peak for the displayed window.
- Observed sample rate calculated from timestamps within the displayed window.
- Valid samples in the upload.
- Total accepted uploads and readings since the server process started.

### Supporting Telemetry

- ADS Channel 1 is a short reference waveform strip below the main trace.
- IMU data is a compact panel with X, Y, Z, vector magnitude, and a small motion trend.
- Pipeline health shows accepted uploads, received rows, invalid rows, connection state, and legacy status.
- Supporting panels never reduce the primary Channel 2 region below its dominant visual role on desktop layouts.

### Responsive Behaviour

On narrow screens, Channel 2 remains first and full-width. The metrics rail moves below it, followed by Channel 1 and IMU panels. The waveform is not replaced by summary cards at any supported width.

## Error and State Handling

- **Unknown valid UID:** Display `Unregistered ECG Monitor`, show the UID, and log it for alias configuration.
- **Malformed UID:** Reject the upload and do not broadcast its rows.
- **Legacy upload:** Accept and display it with an explicit legacy badge.
- **Different UID while one device is displayed:** Do not merge it into the active waveform; show an identity warning.
- **Invalid rows:** Skip them, report the count, and accept the upload only if valid rows remain and identity rules pass.
- **Interrupted SSE:** Keep the latest complete waveform visible and switch the connection indicator to disconnected.
- **Partial upload batch:** Keep the previous complete waveform visible until the new batch is complete.
- **Server restart:** Runtime counters reset; device identity continues to come from each upload.

## Verification

### Firmware

- Verify `device_id` contains `STM32-` plus 24 uppercase hexadecimal digits.
- Verify all three HAL UID words appear in the documented order.
- Verify every new active and ready file starts with the metadata line and existing sample header.
- Verify 2,500 sample rows are still logged per completed recording window.
- Verify the resulting file remains within `MAX_HTTPDATA_BYTES`.
- Run the full STM32 firmware build.

### Server

- Parse a valid new-format upload and resolve the configured alias.
- Accept a valid legacy six-column upload with a legacy designation.
- Reject malformed and changing UIDs.
- Reject a request with no valid sample rows.
- Preserve request boundaries and upload identity through SSE chunking.
- Confirm the HTTP result prevents firmware deletion of rejected queue files.

### Dashboard

- Render exactly 2,500 Channel 2 samples without smoothing or decimation.
- Replace complete windows without connecting separate batches.
- Show the friendly name and matching UID.
- Keep Channel 1 and IMU visible in compact form.
- Preserve the last complete waveform during SSE interruption.
- Verify desktop and narrow responsive layouts.

### End-to-End Bench Test

Run the existing replay pipeline for at least two upload cycles and confirm:

- The firmware Live Expression UID matches the uploaded metadata.
- The server resolves the UID to `ECG Monitor 01`.
- Each dashboard update contains one complete 10-second Channel 2 window.
- SD, ADS, UART, parsing, and upload error counters remain zero.
- No queued file is deleted after a rejected server response.

## Acceptance Criteria

The design is complete when one identifiable STM32 board can record, queue, upload, and display complete 10-second telemetry batches under the friendly name `ECG Monitor 01`; Channel 2 is the dominant faithful waveform; supporting ADS and IMU data remain visible; legacy queued files remain usable; and malformed or cross-device data cannot silently contaminate the displayed trace.
