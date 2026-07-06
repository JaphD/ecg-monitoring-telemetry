# SD-First ECG Telemetry Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Continuously acquire ADS1292R ECG and LIS3DH motion data, preserve it on SD, and reliably upload closed CSV batches to the Node.js endpoint.

**Architecture:** An EXTI-driven ECG producer writes compact samples into a RAM ring. The foreground drains the ring into an active SD CSV file, rotates files into a persistent `.RDY` queue, and uploads the oldest file with interrupt-driven modem RX and three full HTTP retries.

**Tech Stack:** STM32L452 HAL, FatFs/SDMMC, ADS1292R SPI, LIS3DH I2C, A7670G USART1, HTTP CSV.

---

- [x] Replace diagnostic `main.c` with the SD-first pipeline.
- [x] Configure ADS DRDY falling-edge EXTI and USART1 IRQ handlers.
- [x] Preserve the exact six-column Node.js CSV contract.
- [x] Recover unfinished SD files and retain failed uploads.
- [x] Retry complete HTTP transactions three times with five-second backoff.
- [x] Build with no compiler errors or warnings and document Live Expressions.
