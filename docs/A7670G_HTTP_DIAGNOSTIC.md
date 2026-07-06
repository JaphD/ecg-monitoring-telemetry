# A7670G HTTP Diagnostic: Board Test Guide

This firmware build performs one modem-to-dashboard test and then stops in a stable loop for STM32CubeIDE Live Expressions.

## Before starting

1. Start the Node.js server on port 3333.
2. Start ngrok and confirm it forwards `https://carton-cupping-modify.ngrok-free.dev` to `http://localhost:3333`.
3. Open the dashboard and confirm its event log says the SSE stream is connected.
4. Flash and debug the firmware with ST-Link V2. Do not single-step through modem timing; resume normal execution and watch Live Expressions.

## Add these Live Expressions

```text
test_status
diag_stage
diag_result
diag_last_command
diag_last_response
diag_firmware_response
diag_ifc_response
diag_https_support_response
diag_https_enable_response
diag_httpinit_response
diag_url_response
diag_content_response
diag_download_response
diag_data_ack_response
diag_httpaction_response
diag_rx_log
diag_rx_count
diag_uart_error
diag_uart_ore_count
diag_tx_result
diag_http_status
diag_http_response_length
diag_download_tick
diag_tx_end_tick
diag_first_rx_tick
```

`diag_stage` reaches `1000` when all retry attempts have finished and all values are ready to record.

For the retry diagnostic, also add:

```text
diag_attempt_count
diag_attempt_result
diag_attempt_http_status
diag_attempt_response_length
diag_attempt_httpaction
diag_attempt_cereg
diag_attempt_cgatt
diag_attempt_cgact
diag_attempt_cgpaddr
diag_attempt_csq
diag_csslcfg_response
```

## Result codes

| `diag_result` | Meaning | Most useful evidence |
|---:|---|---|
| 1 | End-to-end success | `diag_http_status` should be 200 |
| 2 | No `DOWNLOAD` prompt | `diag_download_response` |
| 3 | No acknowledgement after payload | `diag_data_ack_response`, RX timing variables |
| 4 | Modem returned `ERROR` after payload | `diag_data_ack_response` |
| 5 | UART TX/RX or overrun error | `diag_uart_error`, `diag_uart_ore_count`, `diag_tx_result` |
| 6 | `HTTPACTION` failed or timed out | `diag_httpaction_response` |
| 7 | HTTP completed with a non-200 status | `diag_http_status`, `diag_httpaction_response` |
| 8 | Modem, network, or HTTP setup failed | `diag_last_command`, `diag_last_response`, `test_status` |

## Expected successful result

The dashboard packet count increases to 1 and displays acceleration values `10`, `20`, `30` and ECG values `1000`, `2000`. Ngrok records a POST to `/api/data`, and the modem reports `+HTTPACTION: 1,200,<length>`.

## Reporting a failed result

Wait for `diag_stage == 100`, then copy all expressions above exactly. Also record the ngrok connection table and Node.js console output. Do not reset the board until the values are captured.
