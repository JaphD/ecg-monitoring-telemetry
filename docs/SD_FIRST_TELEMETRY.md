# SD-First Telemetry Board Test

The firmware continuously samples ECG at 500 SPS, reads LIS3DH acceleration at 50 Hz, writes exact six-column CSV batches to SD, and uploads the oldest closed batch to the Node.js endpoint.

## Live Expressions

```text
(char *)system_status
total_samples_acquired
total_samples_logged
sample_ring_overflows
sd_write_errors
sd_files_queued
uploads_ok
uploads_failed
last_http_status
last_upload_attempts
last_upload_file_size
uart_error_flags
uart_overruns
ads_id_value
ads_drdy_irq_count
ads_spi_errors
(char *)upload_failure_step
(char *)upload_failure_response
modem_boot_stage
(char *)modem_boot_failure
(char *)modem_boot_last_response
(char *)current_log_filename
(char *)current_upload_filename
(char *)last_modem_response
modem_power_requested
modem_power_state
modem_power_stage
modem_power_last_transition_tick
modem_power_enables
modem_power_disables
modem_power_cycles
(char *)modem_power_last_on_reason
(char *)modem_power_last_off_reason
```

## V1 LiPo Modem Power Gate

For V1, PB8 controls the TPS22969DNYR rail to the A7670G. `modem_power_state`
is `0` when the rail is off and `1` after the 100 ms rail-settle interval.
`modem_power_stage` is `10` while the rail is settling, `20` while the modem
boot sequence runs, `30` when the modem is ready for upload, and `0` when it
is off. The reason strings show why the last transition occurred. During ECG
recording and the 60-second retry idle interval the expected state is OFF.

Healthy operation has a nonzero ADS ID, continuously increasing DRDY/acquired/logged counts, zero SPI/ring/SD errors, and periodic `uploads_ok` increments. A transient modem status 715 is retried as a complete HTTP session after five seconds. The `.RDY` file remains on SD unless HTTP 200 is received.

The public ngrok URL is compiled into `UPLOAD_URL` in `Core/Src/main.c`. Update it whenever the free ngrok hostname changes.
