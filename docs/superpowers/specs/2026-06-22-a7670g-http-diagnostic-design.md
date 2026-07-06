# A7670G HTTP Diagnostic Design

## Goal

Create a temporary STM32 firmware path that isolates the A7670G HTTP upload from ECG, IMU, RTC, SD-card, and buffering behavior. The test must expose every modem stage through STM32CubeIDE Live Expressions and, on success, deliver one valid CSV row through ngrok to the Node.js dashboard.

## Diagnostic flow

1. Initialize HAL, clocks, GPIO, and USART1 only.
2. Boot the A7670G and establish the already-validated packet-data connection.
3. Query and retain `AT+CGMR`, `AT+IFC?`, and `AT+HTTPSSL=?` responses.
4. Initialize HTTP, enable SSL when the modem reports `HTTPSSL` support, and configure `https://carton-cupping-modify.ngrok-free.dev/api/data` with `text/csv` content.
5. Request a data-entry window for one fixed, valid CSV payload.
6. Arm interrupt-driven UART reception before transmitting the payload so an immediate modem acknowledgement cannot be lost during the transmit-to-receive handoff.
7. If the modem acknowledges the data, execute `AT+HTTPACTION=1`, parse its status, and retain the full response.
8. Halt in a debugger-friendly loop with all diagnostic variables stable.

## Test payload

```csv
timestamp,accel_x,accel_y,accel_z,ecg_ch1,ecg_ch2
00:00:01,10,20,30,1000,2000
```

The Node.js server should skip the header, broadcast one sensor row through SSE, and return HTTP 200. The dashboard should show one packet with acceleration `10,20,30` and ECG values `1000,2000`.

## Result codes

- `1`: complete success with HTTP 200
- `2`: no `DOWNLOAD` prompt
- `3`: data acknowledgement timeout
- `4`: modem returned `ERROR` after payload
- `5`: UART interrupt receive error
- `6`: `HTTPACTION` failed or timed out
- `7`: HTTP action completed with a non-200 status
- `8`: network or HTTP setup failure

## Safety and scope

The diagnostic does not alter the staged version of `main.c`. It intentionally bypasses sensor acquisition and normal streaming. It does not modify the Node.js server.
