# V1 SD-first telemetry test

The `v1-lipo-modem-gate` firmware records ADS1292R ECG at 250 SPS and reads
LIS3DH acceleration at 50 Hz. Each closed SD file contains 2,500 ECG rows,
representing about 10 seconds. The six-column CSV remains the durable upload
payload, and a file is deleted only after HTTP 200.

The current upload endpoint is:

```text
https://ecg-dashboard-1h8s.onrender.com/api/ingest
```

## Essential Live Expressions

```text
(char *)system_status
record_sessions_completed
uploads_ok
uploads_failed
last_http_status
last_upload_attempts
sd_files_queued
ads_measured_rate_millihz
sample_ring_overflows
ads_spi_errors
sd_write_errors
(char *)upload_failure_step
(char *)upload_failure_response
battery_voltage_mv
battery_query_failures
battery_header_failures
(char *)battery_last_response
```

Healthy acquisition keeps `ads_measured_rate_millihz` near `250000`, advances
the recording and upload counters, and leaves the acquisition, ring, and SD
error counters at zero. A transient HTTP 715 is retried as a complete HTTP
session after five seconds; its `.RDY` file remains queued until HTTP 200.

## V1 Li-Po modem power and battery measurement

PB8 controls the TPS22969DNYR rail feeding the A7670G. The current
`MODEM_HOLD_POWER_AB_TEST = 1` setting retains modem power across normal record
and upload cycles to preserve the tested TLS behavior. The existing 715
recovery path may still cycle the modem rail.

After a successful modem boot, firmware issues `AT+CBC`. The A7670G reports its
supply voltage, which is the battery-fed modem rail on V1. A valid 2,500-5,000
mV result is exposed in `battery_voltage_mv` and sent with each upload as:

```text
X-Battery-Millivolts: 3749
```

The header is optional. A failed query or header command increments its
diagnostic counter but does not stop the ECG upload. The TirtaTrace server
stores the voltage with the recording and does not infer a Li-Po percentage.

## Current validation status

As of 2026-09-13, the battery metadata change builds with GNU Tools for STM32
14.3 and passes the firmware source-contract suite. It has not yet been
validated on the physical V1 board. During the first run, confirm a plausible
`battery_voltage_mv`, zero `battery_header_failures`, HTTP 200, and the same
voltage on the latest TirtaTrace recording.
