# V1 JSON Telemetry Design

## Goal

Extend V1 telemetry to preserve SD-first recording while uploading validated,
bounded JSON batches to the local Node.js dashboard server. V2 `main` remains
unchanged.

## Wire Contract

The V1 client posts `application/json` to `/api/data`:

```json
{
  "device_id": "stm32l452-0123456789abcdef01234567",
  "readings": [
    {
      "timestamp": 1721495400000,
      "accel_x": 0.05,
      "accel_y": -1.2,
      "accel_z": 9.81,
      "ecg_ch1": 0.15,
      "ecg_ch2": -0.04
    }
  ]
}
```

`device_id` is derived from STM32L452's 96-bit hardware UID. `readings` is
split into JSON bodies smaller than the modem's 100,000-byte HTTPDATA limit.

## Time

At V1 boot, the modem rail is enabled only long enough to register and query
`AT+CCLK?`. Firmware parses the network UTC clock and records an epoch-ms
reference paired with `HAL_GetTick()`. Samples use the reference plus elapsed
ticks. If time sync fails, recording continues with a visible unsynchronized
state and the queued data is retained for a later retry.

## Storage and Upload

SD CSV remains the durable source of truth. During upload the firmware reads a
closed `.RDY` file, converts rows to bounded JSON batches, and posts every
batch. The `.RDY` file is removed only after every batch receives HTTP 200.

## Server and Dashboard

`Web-Dashboard/server.js` accepts both the existing raw CSV V2 payload and
V1 JSON. JSON payloads are schema-validated, normalized into rows carrying
`device_id`, and forwarded in existing SSE `sensor-batch` events. The dashboard
uses the same charts and displays the most recently received device ID.
