/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : ADS1292R SPI ID test + register config + ECG data capture
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include <string.h>
#include <stdio.h>

/* ── Peripheral handles ─────────────────────────────────────────────────── */
I2C_HandleTypeDef  hi2c1;
SD_HandleTypeDef   hsd1;
SPI_HandleTypeDef  hspi1;
UART_HandleTypeDef huart1;

/* ── Debugger-visible globals ───────────────────────────────────────────── */
volatile char     test_status[128]  = "Initializing...";
volatile int32_t  ecg_ch1           = 0;   /* CH1 raw 24-bit signed (RA path) */
volatile int32_t  ecg_ch2           = 0;   /* CH2 raw 24-bit signed (LA path) */
volatile uint32_t sample_count      = 0;   /* increments every captured sample */
volatile uint8_t  new_sample_ready  = 0;   /* flag set when fresh data is in   */

/* ── ADS1292R SPI helpers ───────────────────────────────────────────────── */
/* CS  = PA4,  RESET/PWDN = PC4,  DRDY# = PA0 (active LOW, polled) */

static inline void CS_LOW(void)  { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET); }
static inline void CS_HIGH(void) { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);   }

/* Send a single opcode byte */
static void ADS_SendCmd(uint8_t cmd)
{
    CS_LOW();
    HAL_Delay(2);
    HAL_SPI_Transmit(&hspi1, &cmd, 1, 10);
    HAL_Delay(2);
    CS_HIGH();
    HAL_Delay(2);
}

/* Write one register:  WREG | addr, 0x00 (count=1), value */
static void ADS_WriteReg(uint8_t addr, uint8_t value)
{
    uint8_t buf[3] = { (uint8_t)(0x40 | (addr & 0x1F)), 0x00, value };
    CS_LOW();
    HAL_Delay(2);
    HAL_SPI_Transmit(&hspi1, buf, 3, 10);
    HAL_Delay(2);
    CS_HIGH();
    HAL_Delay(2);
}

/* Read one register: RREG | addr, 0x00, dummy */
static uint8_t ADS_ReadReg(uint8_t addr)
{
    uint8_t tx[2] = { (uint8_t)(0x20 | (addr & 0x1F)), 0x00 };
    uint8_t dummy = 0x00, rx = 0x00;
    CS_LOW();
    HAL_Delay(2);
    HAL_SPI_Transmit(&hspi1, tx, 2, 10);
    HAL_Delay(2);
    HAL_SPI_TransmitReceive(&hspi1, &dummy, &rx, 1, 10);
    HAL_Delay(2);
    CS_HIGH();
    HAL_Delay(2);
    return rx;
}

/* ── ADS1292R register map (relevant subset) ────────────────────────────── */
#define REG_ID      0x00
#define REG_CONFIG1 0x01
#define REG_CONFIG2 0x02
#define REG_LOFF    0x03
#define REG_CH1SET  0x04
#define REG_CH2SET  0x05
#define REG_RLD_SENS  0x06
#define REG_LOFF_SENS 0x07
#define REG_LOFF_STAT 0x08
#define REG_RESP1   0x09
#define REG_RESP2   0x0A
#define REG_GPIO    0x0B

/* Opcodes */
#define CMD_WAKEUP  0x02
#define CMD_STANDBY 0x04
#define CMD_RESET   0x06
#define CMD_START   0x08
#define CMD_STOP    0x0A
#define CMD_RDATAC  0x10
#define CMD_SDATAC  0x11
#define CMD_RDATA   0x12

/* ── Function prototypes ────────────────────────────────────────────────── */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_SDMMC1_SD_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);

/* ═══════════════════════════════════════════════════════════════════════════
   main
   ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    /* ── HAL + peripheral init ────────────────────────────────────────── */
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_I2C1_Init();
    MX_SDMMC1_SD_Init();
    MX_SPI1_Init();
    MX_USART1_UART_Init();

    /* ══════════════════════════════════════════════════════════════════
       STEP 1 — Power-up sequence
       ══════════════════════════════════════════════════════════════════ */
    snprintf((char *)test_status, sizeof(test_status), "ADS1292R: Power-up sequence...");
    CS_HIGH();
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);   /* RESET/PWDN HIGH */
    HAL_Delay(2000);

    /* Toggle RESET: HIGH → LOW → HIGH */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_Delay(200);

    /* ══════════════════════════════════════════════════════════════════
       STEP 2 — Send SDATAC, then read ID
       ══════════════════════════════════════════════════════════════════ */
    snprintf((char *)test_status, sizeof(test_status), "ADS1292R: Sending SDATAC...");
    ADS_SendCmd(CMD_SDATAC);
    HAL_Delay(100);

    snprintf((char *)test_status, sizeof(test_status), "ADS1292R: Reading ID...");
    uint8_t id_val = ADS_ReadReg(REG_ID);

    if (id_val == 0x73)
        snprintf((char *)test_status, sizeof(test_status), "PASS: ADS1292R  | ID=0x%02X", id_val);
    else if (id_val == 0x72)
        snprintf((char *)test_status, sizeof(test_status), "PASS: ADS1292   | ID=0x%02X", id_val);
    else
    {
        snprintf((char *)test_status, sizeof(test_status),
                 "FAIL: ID=0x%02X expected 0x73 — halting", id_val);
        while (1) {}   /* stop here so you can inspect in debugger */
    }

    HAL_Delay(500);

    /* ══════════════════════════════════════════════════════════════════
       STEP 3 — Configure registers
       ══════════════════════════════════════════════════════════════════
       All writes happen while SDATAC is active (continuous-read is OFF).

       CONFIG1 (0x01):
         [7]   = 0  — single-shot off (continuous conversion)
         [2:0] = 001 — 500 SPS  (matches MIT-BIH 360 Hz → nearest higher)
                 000 = 125 SPS
                 001 = 250 SPS   ← use this if you prefer
                 010 = 500 SPS
                 011 = 1000 SPS
                 (Use 0x01 for 250 SPS to be close to the 360 Hz source)

       CONFIG2 (0x02):
         [7]   = 1  — PDB_LOFF_COMP off  (bit is "power-down" when 1 = powered)
         [6]   = 1  — PDB_REFBUF  on (internal 2.42 V reference enabled)
         [5]   = 1  — VREF_4V     0  (2.42 V ref, not 4 V)
         [4]   = 1  — CLK_EN      on (oscillator output enabled on CLK pin)
         [1]   = 0  — INT_TEST    off
         [0]   = 0  — TEST_FREQ   —
         → 0b11100000 = 0xE0  (common safe value for external-signal measurement)

       CH1SET (0x04):
         [7]   = 0  — channel powered ON
         [6:4] = 001 — PGA gain x2  (adjust to taste: 000=6, 001=1, 010=2,
                                      011=3, 100=4, 101=8, 110=12)
         [3:0] = 0000 — normal electrode input
         → 0x10  (gain x2, normal input)

       CH2SET (0x05):  same as CH1SET
         → 0x10

       RLD_SENS (0x06): leave at 0x23 (PGA chop disabled, both channels in RLD)
       RESP1/RESP2: leave at defaults (respiration off)
    ══════════════════════════════════════════════════════════════════ */
    snprintf((char *)test_status, sizeof(test_status), "ADS1292R: Configuring registers...");

    ADS_WriteReg(REG_CONFIG1, 0x01);  /* 250 SPS continuous */
    ADS_WriteReg(REG_CONFIG2, 0xE0);  /* internal ref + clock enabled */
    ADS_WriteReg(REG_CH1SET,  0x10);  /* PGA gain x2, normal input      */
    ADS_WriteReg(REG_CH2SET,  0x10);  /* PGA gain x2, normal input      */
    ADS_WriteReg(REG_RLD_SENS, 0x23); /* RLD: both channels              */
    ADS_WriteReg(REG_RESP1,   0x02);  /* respiration off                 */
    ADS_WriteReg(REG_RESP2,   0x03);  /* RLDREF internal (AVDD/2)        */

    HAL_Delay(10);

    /* Verify at least CONFIG1 wrote correctly */
    uint8_t cfg1_rb = ADS_ReadReg(REG_CONFIG1);
    if (cfg1_rb != 0x01)
        snprintf((char *)test_status, sizeof(test_status),
                 "WARN: CONFIG1 readback=0x%02X (expected 0x01)", cfg1_rb);

    /* ══════════════════════════════════════════════════════════════════
       STEP 4 — Start conversions + enter continuous-read mode
       ══════════════════════════════════════════════════════════════════ */
    snprintf((char *)test_status, sizeof(test_status), "ADS1292R: Starting conversions...");
    ADS_SendCmd(CMD_START);   /* begin ADC conversions */
    HAL_Delay(10);
    ADS_SendCmd(CMD_RDATAC);  /* enter read-data-continuous mode */
    HAL_Delay(10);

    snprintf((char *)test_status, sizeof(test_status), "ADS1292R: Waiting for DRDY#...");

    /* ══════════════════════════════════════════════════════════════════
       STEP 5 — Acquisition loop
       In RDATAC mode the device pulls DRDY# LOW when a new 9-byte frame
       is ready.  Frame layout (MSB first):
         Bytes 0-2  : 24-bit status word
         Bytes 3-5  : CH1  (24-bit signed, 2's complement)
         Bytes 6-8  : CH2  (24-bit signed, 2's complement)
       ══════════════════════════════════════════════════════════════════ */
    while (1)
    {
        /* Poll PA0 (DRDY#) — wait for it to go LOW (active low) */
        if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_RESET)
        {
            uint8_t frame[9] = {0};

            /* Clock out 9 bytes while CS is held LOW */
            CS_LOW();
            HAL_SPI_Receive(&hspi1, frame, 9, 50);
            CS_HIGH();

            /* ── Parse CH1 (bytes 3-5) ── */
            int32_t ch1_raw = ((int32_t)frame[3] << 16)
                            | ((int32_t)frame[4] <<  8)
                            |  (int32_t)frame[5];
            /* Sign-extend from 24-bit */
            if (ch1_raw & 0x00800000) ch1_raw |= (int32_t)0xFF000000;

            /* ── Parse CH2 (bytes 6-8) ── */
            int32_t ch2_raw = ((int32_t)frame[6] << 16)
                            | ((int32_t)frame[7] <<  8)
                            |  (int32_t)frame[8];
            if (ch2_raw & 0x00800000) ch2_raw |= (int32_t)0xFF000000;

            /* ── Store into debugger-visible globals ── */
            ecg_ch1 = ch1_raw;
            ecg_ch2 = ch2_raw;
            sample_count++;
            new_sample_ready = 1;

            /* Update status string every 250 samples (~1 s at 250 SPS) */
            if (sample_count % 250 == 0)
                snprintf((char *)test_status, sizeof(test_status),
                         "OK | n=%lu | CH1=%ld | CH2=%ld",
                         (unsigned long)sample_count,
                         (long)ecg_ch1,
                         (long)ecg_ch2);

            new_sample_ready = 0;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Peripheral init functions (unchanged from CubeMX output)
   ═══════════════════════════════════════════════════════════════════════════ */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) Error_Handler();

    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_LSE | RCC_OSCILLATORTYPE_MSI;
    RCC_OscInitStruct.LSEState            = RCC_LSE_ON;
    RCC_OscInitStruct.MSIState            = RCC_MSI_ON;
    RCC_OscInitStruct.MSICalibrationValue = 0;
    RCC_OscInitStruct.MSIClockRange       = RCC_MSIRANGE_6;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_MSI;
    RCC_OscInitStruct.PLL.PLLM           = 1;
    RCC_OscInitStruct.PLL.PLLN           = 40;
    RCC_OscInitStruct.PLL.PLLP           = RCC_PLLP_DIV7;
    RCC_OscInitStruct.PLL.PLLQ           = RCC_PLLQ_DIV2;
    RCC_OscInitStruct.PLL.PLLR           = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK) Error_Handler();

    HAL_RCCEx_EnableMSIPLLMode();
}

static void MX_I2C1_Init(void)
{
    hi2c1.Instance              = I2C1;
    hi2c1.Init.Timing           = 0x00D09BE3;
    hi2c1.Init.OwnAddress1      = 0;
    hi2c1.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2      = 0;
    hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    hi2c1.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) Error_Handler();
    if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK) Error_Handler();
    if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK) Error_Handler();
}

static void MX_SDMMC1_SD_Init(void)
{
    hsd1.Instance                 = SDMMC1;
    hsd1.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
    hsd1.Init.ClockBypass         = SDMMC_CLOCK_BYPASS_DISABLE;
    hsd1.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd1.Init.BusWide             = SDMMC_BUS_WIDE_4B;
    hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd1.Init.ClockDiv            = 0;
    if (HAL_SD_Init(&hsd1) != HAL_OK) Error_Handler();
    if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK) Error_Handler();
}

static void MX_SPI1_Init(void)
{
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_2EDGE;   /* CPOL=0, CPHA=1 → Mode 1 */
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial     = 7;
    hspi1.Init.CRCLength         = SPI_CRC_LENGTH_DATASIZE;
    hspi1.Init.NSSPMode          = SPI_NSS_PULSE_ENABLE;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) Error_Handler();
}

static void MX_USART1_UART_Init(void)
{
    huart1.Instance            = USART1;
    huart1.Init.BaudRate       = 115200;
    huart1.Init.WordLength     = UART_WORDLENGTH_8B;
    huart1.Init.StopBits       = UART_STOPBITS_1;
    huart1.Init.Parity         = UART_PARITY_NONE;
    huart1.Init.Mode           = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl      = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling   = UART_OVERSAMPLING_16;
    huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* Output levels */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13 | GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4,               GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_8, GPIO_PIN_RESET);

    /* PC13, PC4 — outputs (LED, RESET/PWDN) */
    GPIO_InitStruct.Pin   = GPIO_PIN_13 | GPIO_PIN_4;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* PA0 — DRDY# input (active LOW, polled) */
    GPIO_InitStruct.Pin  = GPIO_PIN_0;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* PA4 — CS output */
    GPIO_InitStruct.Pin   = GPIO_PIN_4;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* PB1, PB2, PB8 — outputs (other peripherals, untouched) */
    GPIO_InitStruct.Pin   = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_8;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* PB5, PB9 — other peripheral interrupts, left as-is */
    GPIO_InitStruct.Pin  = GPIO_PIN_5 | GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif
