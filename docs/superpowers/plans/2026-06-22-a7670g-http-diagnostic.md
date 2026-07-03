# A7670G HTTP Diagnostic Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a buildable STM32 diagnostic that captures the A7670G HTTP data acknowledgement without a polling RX gap and posts one valid CSV row to the ngrok-backed Node.js endpoint.

**Architecture:** Retain the generated STM32 peripheral setup and validated modem boot routine. Add a small interrupt-driven diagnostic receiver, a one-shot HTTP transaction, and volatile debugger variables; then stop execution in a stable observation loop.

**Tech Stack:** STM32L452RE, STM32 HAL, USART1 interrupts, A7670G AT commands, STM32CubeIDE Debug makefile.

---

### Task 1: Define the diagnostic contract

**Files:**
- Modify: `Core/Src/main.c`
- Create: `docs/A7670G_HTTP_DIAGNOSTIC.md`

- [x] Add volatile stage, result, response, timing, UART-error, and HTTP-status variables.
- [x] Document all values and the expected dashboard result.

### Task 2: Add interrupt-driven diagnostic reception

**Files:**
- Modify: `Core/Src/main.c`
- Modify: `Core/Src/stm32l4xx_hal_msp.c`
- Modify: `Core/Src/stm32l4xx_it.c`
- Modify: `Core/Inc/stm32l4xx_it.h`

- [x] Enable `USART1_IRQn` in the UART MSP initialization.
- [x] Route `USART1_IRQHandler` to `HAL_UART_IRQHandler`.
- [x] Add one-byte re-armed HAL RX callbacks that append to a bounded diagnostic buffer.
- [x] Capture first-byte timing and UART error flags before clearing or recovery.

### Task 3: Implement the one-shot HTTP transaction

**Files:**
- Modify: `Core/Src/main.c`

- [x] Query firmware, flow-control, and HTTPS capability responses.
- [x] Configure the ngrok `/api/data` URL and `text/csv` content.
- [x] Wait for `DOWNLOAD`, arm RX, and send the exact fixed CSV payload.
- [x] Classify `OK`, `ERROR`, timeout, and UART-error outcomes.
- [x] Execute `HTTPACTION=1`, parse the HTTP status, terminate HTTP, and halt.

### Task 4: Verify the artifact

**Files:**
- Verify: `Core/Src/main.c`
- Verify: `Debug/Firmware.elf`

- [x] Run static checks for declared diagnostic variables, IRQ routing, payload length, and URL path.
- [x] Build the existing Debug makefile and require a zero exit status.
- [x] Inspect the final diff to ensure staged user work remains staged and unrelated source files are untouched.
