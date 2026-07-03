# Record/Upload State Machine Design

## Goal

Reliably retain and eventually deliver every 500 Hz ADS1292R sample, including the latest LIS3DH values, without simultaneous SD logging and SD-backed HTTP upload activity.

## Architecture

Firmware alternates between two mutually exclusive phases:

1. **Record:** sample ADS1292R at 500 Hz, service LIS3DH, and write CSV to SD. Rotate the active log every 2,500 rows (five seconds). Record for 30 seconds.
2. **Upload:** stop ADS conversions, drain the RAM ring, finalize `ACTIVE.TMP`, close the logger, then upload every `.RDY` file sequentially. Delete a file only after HTTP 200. After the queue is empty, open a new active log and begin the next recording session.

At boot, recover a non-empty `ACTIVE.TMP` as `.RDY` and drain the existing queue before beginning a new recording session. A failed file remains queued. After three HTTP attempts, firmware starts a new recording session and retries the queue after that session, preventing permanent loss of new measurements during a network outage.

## Data Format

The server-facing format remains unchanged:

```text
timestamp,accel_x,accel_y,accel_z,ecg_ch1,ecg_ch2
```

Each five-second file contains at most 2,500 data rows plus the header. The Node server continues to process each request as a streamed CSV body.

## SD Safety

- No `.RDY` file is open while `ACTIVE.TMP` is being written.
- `ACTIVE.TMP` is closed before the upload phase begins.
- SD file deletion occurs only after HTTP 200.
- Sync, close, rename, unlink, and physical SD recovery results remain debugger-visible.
- A write failure is throttled rather than retried in a tight loop.

## Observable Phases

`system_phase` values:

- `10`: boot queue recovery
- `20`: recording
- `30`: stopping and finalizing recording
- `40`: uploading queued files
- `50`: upload phase ended with files still queued
- `100`: upload queue drained
- `255`: fatal initialization failure

Additional counters expose completed record sessions, uploaded sessions, and queued files.

## Success Criteria

- During record phase, acquired/logged counts increase and SD errors remain stable.
- During upload phase, acquisition is disabled and sample counts remain intentionally unchanged.
- Each `.RDY` file produces HTTP 200 and is then deleted.
- On network failure, the file remains queued and is retried after the next recording session or reboot.
- No SD read/write overlap exists between recording and upload phases.

