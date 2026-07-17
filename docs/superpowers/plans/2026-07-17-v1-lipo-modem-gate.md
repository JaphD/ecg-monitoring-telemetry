# V1 LiPo Modem Power Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make PB8 power-gate the A7670G for V1 while retaining SD-first telemetry and exposing the entire modem-power lifecycle in CubeIDE Live Expressions.

**Architecture:** Add a file-local modem-power controller in `main.c`. Recording stays unchanged with the rail off; queue uploads explicitly power the rail, boot the modem, upload, then power it off. Existing PowerShell source-contract tests verify the order without target hardware.

**Tech Stack:** STM32L4 HAL, FatFS, STM32CubeIDE, PowerShell source-contract tests.

## Global Constraints

- Retain ADS1292R 250 SPS and SPI1 `/256`.
- PB8 HIGH enables TPS22969DNYR; PB8 LOW disables the A7670G rail.
- Use 100 ms rail-on and rail-off settling delays.
- Never delete `.RDY` data unless modem HTTP status is 200.
- Preserve the 60,000 ms retry idle period with the modem rail off.
- Do not add TP4056, USB-C, PMOS-path, battery-gauge, or sleep-mode logic.

---

### Task 1: Specify and verify the V1 power-controller contract

**Files:**
- Create: `tests/v1_lipo_modem_power_gate_test.ps1`
- Modify: `Core/Src/main.c:30-175, 1478-1574`

**Interfaces:**
- Produces `ModemPower_Enable(const char *reason)`, `ModemPower_Disable(const char *reason)`, and `ModemPower_BootForUpload(void)`.
- Produces debugger variables `modem_power_requested`, `modem_power_state`, `modem_power_stage`, `modem_power_last_transition_tick`, `modem_power_enables`, `modem_power_disables`, `modem_power_cycles`, `modem_power_last_on_reason`, and `modem_power_last_off_reason`.

- [ ] **Step 1: Write the failing source-contract test**

```powershell
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')
$required = @(
  'MODEM_RAIL_SETTLE_MS       100U',
  'ModemPower_Enable', 'ModemPower_Disable', 'ModemPower_BootForUpload',
  'modem_power_requested', 'modem_power_state', 'modem_power_stage',
  'modem_power_enables', 'modem_power_disables', 'modem_power_cycles',
  'modem_power_last_on_reason', 'modem_power_last_off_reason'
)
foreach ($item in $required) { if ($source -notmatch [regex]::Escape($item)) { throw "Missing V1 modem power contract: $item" } }
$enable = $source.IndexOf('static void ModemPower_Enable')
$boot = $source.IndexOf('static uint8_t ModemPower_BootForUpload')
$upload = $source.IndexOf('static void Run_UploadPhase')
if (($enable -lt 0) -or ($boot -lt 0) -or ($upload -lt 0) -or ($boot -gt $upload)) { throw 'Power controller must be defined before the upload phase.' }
Write-Output 'V1 modem power-gate contract: PASS'
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `powershell -ExecutionPolicy Bypass -File tests/v1_lipo_modem_power_gate_test.ps1`

Expected: `Missing V1 modem power contract: MODEM_RAIL_SETTLE_MS       100U`.

- [ ] **Step 3: Implement the controller and Live Expressions**

```c
#define MODEM_RAIL_SETTLE_MS       100U

static void ModemPower_Enable(const char *reason)
{
    modem_power_requested = 1U;
    modem_power_stage = 10U;
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_Delay(MODEM_RAIL_SETTLE_MS);
    modem_power_state = 1U;
    modem_power_enables++;
    modem_power_last_transition_tick = HAL_GetTick();
    snprintf((char *)modem_power_last_on_reason, sizeof(modem_power_last_on_reason), "%s", reason);
}
```

Add the matching disable helper (LOW, 100 ms delay, state/counter/reason update) and boot helper (enable then call `Modem_Boot`; disable on failure). Replace the unconditional boot in `main()` with no modem boot. Invoke the boot helper only when the upload phase has queued files; disable the rail after success or deferred upload.

- [ ] **Step 4: Run the test to verify it passes**

Run: `powershell -ExecutionPolicy Bypass -File tests/v1_lipo_modem_power_gate_test.ps1`

Expected: `V1 modem power-gate contract: PASS`.

- [ ] **Step 5: Commit**

```powershell
git add Core/Src/main.c tests/v1_lipo_modem_power_gate_test.ps1
git commit -m "feat: gate V1 modem rail through PB8"
```

### Task 2: Verify retry isolation and debugger observability

**Files:**
- Modify: `tests/v1_lipo_modem_power_gate_test.ps1`
- Modify: `docs/SD_FIRST_TELEMETRY.md`

**Interfaces:**
- Consumes `ModemPower_Disable(const char *reason)` from Task 1.
- Produces a documented CubeIDE expression list for V1 rail state and retry behavior.

- [ ] **Step 1: Extend the failing test**

```powershell
$retryStart = $source.IndexOf('static void Drain_UploadQueueBeforeNextRecord')
$retryText = $source.Substring($retryStart, 2600)
if ($retryText -notmatch 'ModemPower_Disable\("upload retry"\)') { throw 'Retry idle must force the modem rail off.' }
if ($retryText -notmatch 'UPLOAD_RETRY_IDLE_MS') { throw 'Retry cooldown must remain intact.' }
if ($source -notmatch 'ModemPower_Disable\("upload complete"\)') { throw 'Successful upload must power off the modem rail.' }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `powershell -ExecutionPolicy Bypass -File tests/v1_lipo_modem_power_gate_test.ps1`

Expected: `Retry idle must force the modem rail off.`

- [ ] **Step 3: Complete retry path and documentation**

```c
ModemPower_Disable("upload retry");
while ((HAL_GetTick() - start) < UPLOAD_RETRY_IDLE_MS) {
    LIS3DH_Service();
    HAL_Delay(10U);
}
```

Add the nine V1 expressions to `docs/SD_FIRST_TELEMETRY.md`, with state meanings: OFF, settling, booting, upload, and retry idle.

- [ ] **Step 4: Run all relevant tests**

Run: `Get-ChildItem tests\*_test.ps1 | ForEach-Object { powershell -ExecutionPolicy Bypass -File $_.FullName }`

Expected: every script prints `PASS` and exits 0.

- [ ] **Step 5: Commit**

```powershell
git add Core/Src/main.c docs/SD_FIRST_TELEMETRY.md tests/v1_lipo_modem_power_gate_test.ps1
git commit -m "docs: expose V1 modem power diagnostics"
```

