"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");

const parserPath = path.resolve(
  __dirname,
  "../../Web-Dashboard/telemetry-parser.js"
);
const { createUploadParser } = require(parserPath);

const uid = "STM32-003A002B123456789ABC91C2";
const header = "timestamp,accel_x,accel_y,accel_z,ecg_ch1,ecg_ch2";

function makeParser(uploadId) {
  return createUploadParser({
    receivedAt: "2026-07-31T19:00:00.000Z",
    uploadId,
  });
}

test("uses a valid STM32 UID as the device identity and preserves all six sample fields", () => {
  const parser = makeParser("u1");
  parser.pushLine(`device_id,${uid}`);
  parser.pushLine(header);
  parser.pushLine("1000,30,-5,1024,-942,-3729");

  const result = parser.finish();

  assert.equal(result.ok, true);
  assert.deepEqual(result.batch.device, {
    id: uid,
    name: uid,
    legacy: false,
  });
  assert.deepEqual(result.batch.rows[0], {
    timestamp: 1000,
    accel_x: 30,
    accel_y: -5,
    accel_z: 1024,
    ecg_ch1: -942,
    ecg_ch2: -3729,
  });
});

test("accepts a legacy queue file with explicit legacy identity", () => {
  const parser = makeParser("u2");
  parser.pushLine(header);
  parser.pushLine("1000,30,-5,1024,-942,-3729");

  const result = parser.finish();

  assert.equal(result.ok, true);
  assert.deepEqual(result.batch.device, {
    id: null,
    name: "Legacy ECG Monitor",
    legacy: true,
  });
});

test("rejects malformed identity instead of treating it as legacy", () => {
  const parser = makeParser("u3");
  parser.pushLine("device_id,STM32-BAD");
  parser.pushLine(header);
  parser.pushLine("1000,30,-5,1024,-942,-3729");

  const result = parser.finish();

  assert.equal(result.ok, false);
  assert.equal(result.status, 422);
});

test("rejects a new-format upload with no valid rows", () => {
  const parser = makeParser("u4");
  parser.pushLine(`device_id,${uid}`);
  parser.pushLine(header);

  const result = parser.finish();

  assert.equal(result.ok, false);
  assert.equal(result.status, 422);
});

test("rejects a UID change within one request", () => {
  const parser = makeParser("u5");
  parser.pushLine(`device_id,${uid}`);
  parser.pushLine("device_id,STM32-FFFFFFFFFFFFFFFFFFFFFFFF");
  parser.pushLine(header);
  parser.pushLine("1000,30,-5,1024,-942,-3729");

  const result = parser.finish();

  assert.equal(result.ok, false);
  assert.equal(result.status, 422);
});

test("rejects identity metadata that appears after the sample header", () => {
  const parser = makeParser("u7");
  parser.pushLine(header);
  parser.pushLine(`device_id,${uid}`);
  parser.pushLine("1000,30,-5,1024,-942,-3729");

  const result = parser.finish();

  assert.equal(result.ok, false);
  assert.equal(result.status, 422);
});

test("counts malformed sample rows without discarding valid rows", () => {
  const parser = makeParser("u6");
  parser.pushLine(`device_id,${uid}`);
  parser.pushLine(header);
  parser.pushLine("not,a,valid,row");
  parser.pushLine("1000,30,-5,1024,-942,-3729");

  const result = parser.finish();

  assert.equal(result.ok, true);
  assert.equal(result.batch.upload.accepted, 1);
  assert.equal(result.batch.upload.invalid, 1);
});
