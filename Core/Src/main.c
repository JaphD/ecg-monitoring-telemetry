/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Integrated ADS1292R ECG + LIS3DH IMU logger to SD card
  *
  * Peripherals used
  * -----------------
  *  SPI1   - ADS1292R  (CPOL=0 CPHA=1, i.e. Mode 1, SPI_PHASE_2EDGE)
  *           CS      -> PA4  (active-LOW, software controlled)
  *           RESET   -> PC4  (active-LOW)
  *           DRDY    -> PB9  (active-LOW input, polled)
  *
  *  I2C1   - LIS3DH IMU  (7-bit addr 0x18, SA0 tied LOW)
  *
  *  SDMMC1 - microSD card via FatFS  (4-bit bus, ClockDiv=7)
  *
  *  RTC    - timestamp in filename and CSV rows
  *           First-boot time set via backup-register sentinel 0xBEEF.
  *           The sentinel is written ONCE and never cleared, so the RTC
  *           keeps running across resets/power-cycles (requires VBAT).
  *
  *  USART1 - 115200 8N1, debug UART
  *
  * CSV columns
  * ------------
  *  timestamp    - HH:MM:SS from RTC
  *  accel_x/y/z  - LIS3DH 12-bit signed counts (1 mg/LSB at +/-2g)
  *  ecg_ch1      - ADS1292R channel-1 24-bit signed value
  *  ecg_ch2      - ADS1292R channel-2 24-bit signed value
  *
  * File naming
  * -----------
  *  ECG_YYYYMMDD_HHMMSS.CSV            (normal case)
  *  ECG_YYYYMMDD_HHMMSS_1.CSV          (if base name already exists)
  *  ECG_YYYYMMDD_HHMMSS_2.CSV  ...     (increments until a free slot found)
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fatfs.h"

/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef  hi2c1;
RTC_HandleTypeDef  hrtc;
SD_HandleTypeDef   hsd1;
SPI_HandleTypeDef  hspi1;
UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
/* Live-expression watch variable - shows current state in the debugger */
volatile char test_status[128] = "Initializing...";
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_SDMMC1_SD_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_RTC_Init(void);

/* USER CODE BEGIN PFP */

/* ===========================================================================
 *  ADS1292R SPI HELPER FUNCTIONS
 * ===========================================================================*/

/**
 * @brief Assert (pull LOW) the ADS1292R chip-select line.
 *        1 ms delay satisfies t_CSS >= 6 SCLK cycles requirement.
 */
static inline void ADS_CS_Low(void)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_Delay(1);
}

/**
 * @brief De-assert (pull HIGH) the ADS1292R chip-select line.
 */
static inline void ADS_CS_High(void)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_Delay(1);
}

/**
 * @brief Send a single-byte opcode command to the ADS1292R.
 *        e.g. 0x11=SDATAC, 0x10=RDATAC, 0x08=START, 0x06=RESET
 */
static void ADS_SendCommand(uint8_t cmd)
{
    ADS_CS_Low();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, 10);
    ADS_CS_High();
    HAL_Delay(2);  /* CS must stay HIGH >= 4 SCLK cycles between commands */
}

/**
 * @brief Write one byte to an ADS1292R register.
 *        WREG opcode = 0x40 | reg_addr, followed by count byte 0x00 (1 reg).
 */
static void ADS_WriteReg(uint8_t reg, uint8_t value)
{
    uint8_t cmd[2] = { (uint8_t)(0x40 | (reg & 0x1F)), 0x00 };
    ADS_CS_Low();
    HAL_SPI_Transmit(&hspi1, cmd,    2, 10);
    HAL_SPI_Transmit(&hspi1, &value, 1, 10);
    ADS_CS_High();
    HAL_Delay(2);
}

/**
 * @brief Read one byte from an ADS1292R register.
 *        RREG opcode = 0x20 | reg_addr, followed by count byte 0x00 (1 reg),
 *        then clock out one dummy byte to receive the register value.
 */
static uint8_t ADS_ReadReg(uint8_t reg)
{
    uint8_t cmd[2] = { (uint8_t)(0x20 | (reg & 0x1F)), 0x00 };
    uint8_t dummy  = 0x00;
    uint8_t result = 0x00;
    ADS_CS_Low();
    HAL_SPI_Transmit(&hspi1, cmd, 2, 10);
    HAL_SPI_TransmitReceive(&hspi1, &dummy, &result, 1, 10);
    ADS_CS_High();
    HAL_Delay(2);
    return result;
}

/**
 * @brief Full ADS1292R power-up, reset, and configuration sequence.
 *
 *        Sequence (mirrors verified ADS1292.txt):
 *          1. Hold RESET LOW briefly, then release HIGH and wait 2 s
 *          2. Toggle RESET LOW->HIGH to latch hardware reset
 *          3. Send SDATAC (required before any RREG/WREG)
 *          4. Read ID register and verify
 *          5. Write config registers
 *          6. Send START then RDATAC to begin continuous streaming
 *
 * @return HAL_OK if device ID is valid, HAL_ERROR otherwise.
 */
static HAL_StatusTypeDef ADS_Init(void)
{
    /* Step 1 - CS idle high, pulse RESET low then release, wait for power */
    ADS_CS_High();
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET); /* RESET LOW  */
    HAL_Delay(10);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);   /* RESET HIGH */
    HAL_Delay(2000); /* >= 1s for power rails and oscillator to stabilise  */

    /* Step 2 - Hardware reset toggle: HIGH -> LOW -> HIGH */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_Delay(200); /* Device needs ~18 CLKs after reset released */

    /* Step 3 - SDATAC: stop continuous-data mode before register access */
    snprintf((char*)test_status, sizeof(test_status), "ADS1292R: Sending SDATAC...");
    ADS_SendCommand(0x11);
    HAL_Delay(200);

    /* Step 4 - Read ID register (0x00) and validate */
    snprintf((char*)test_status, sizeof(test_status), "ADS1292R: Reading ID...");
    uint8_t id = ADS_ReadReg(0x00);

    if (id == 0x73)
        snprintf((char*)test_status, sizeof(test_status), "ADS1292R found  ID=0x%02X", id);
    else if (id == 0x72)
        snprintf((char*)test_status, sizeof(test_status), "ADS1292 (non-R) ID=0x%02X", id);
    else
    {
        snprintf((char*)test_status, sizeof(test_status),
                 "FAIL: ADS ID=0x%02X (expected 0x73)", id);
        return HAL_ERROR;
    }

    /* Step 5 - Configure registers
     *
     *  CONFIG1 (0x01) = 0x02  -> 500 SPS, CLK output disabled
     *  CONFIG2 (0x02) = 0xE0  -> ref buffer on, 2.42V reference
     *  LOFF    (0x03) = 0x10  -> DC lead-off, 95% threshold
     *  CH1SET  (0x04) = 0x00  -> gain=6, normal electrode input
     *  CH2SET  (0x05) = 0x00  -> same as CH1
     *  RLD_SENS(0x06) = 0x2C  -> RLD driven from CH1+CH2
     *  LOFF_SENS(0x07)= 0x0F  -> lead-off detection on all inputs
     *  RESP1   (0x09) = 0xEA  -> respiration demod+mod enabled
     *  RESP2   (0x0A) = 0x03  -> RLDREF internal
     */
    ADS_WriteReg(0x01, 0x02);
    ADS_WriteReg(0x02, 0xE0);
    ADS_WriteReg(0x03, 0x10);
    ADS_WriteReg(0x04, 0x00);
    ADS_WriteReg(0x05, 0x00);
    ADS_WriteReg(0x06, 0x2C);
    ADS_WriteReg(0x07, 0x0F);
    ADS_WriteReg(0x09, 0xEA);
    ADS_WriteReg(0x0A, 0x03);
    HAL_Delay(10);

    /* Step 6 - START conversions, then enter continuous read mode */
    ADS_SendCommand(0x08); /* START  */
    HAL_Delay(10);
    ADS_SendCommand(0x10); /* RDATAC */
    HAL_Delay(10);

    snprintf((char*)test_status, sizeof(test_status), "ADS1292R: Init OK, streaming");
    return HAL_OK;
}

/**
 * @brief Read one data frame from the ADS1292R in RDATAC mode.
 *
 *        Each frame is 9 bytes clocked out when DRDY goes LOW:
 *          Bytes 0-2 : STATUS word (discarded)
 *          Bytes 3-5 : CH1 24-bit signed, MSB first
 *          Bytes 6-8 : CH2 24-bit signed, MSB first
 *
 *        Polls DRDY (PB9, active-LOW) with a 10 ms timeout.
 *
 * @param  ch1  Pointer to store channel-1 result
 * @param  ch2  Pointer to store channel-2 result
 * @return HAL_OK on success, HAL_TIMEOUT if DRDY does not assert in time
 */
static HAL_StatusTypeDef ADS_ReadData(int32_t *ch1, int32_t *ch2)
{
    /* Wait for DRDY to go LOW (active-low on PB9).
     * At 500 SPS one frame arrives every 2 ms, so 10 ms is generous. */
    uint32_t t_start = HAL_GetTick();
    while (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_SET)
    {
        if ((HAL_GetTick() - t_start) > 10)
            return HAL_TIMEOUT;
    }

    /* Clock out all 9 bytes in a single transaction */
    uint8_t rx[9] = {0};
    uint8_t tx[9] = {0}; /* dummy TX */
    ADS_CS_Low();
    HAL_SPI_TransmitReceive(&hspi1, tx, rx, 9, 20);
    ADS_CS_High();

    /* Reconstruct signed 24-bit values and sign-extend to 32 bits */
    int32_t raw_ch1 = ((int32_t)rx[3] << 16) | ((int32_t)rx[4] << 8) | rx[5];
    if (raw_ch1 & 0x00800000) raw_ch1 |= 0xFF000000;

    int32_t raw_ch2 = ((int32_t)rx[6] << 16) | ((int32_t)rx[7] << 8) | rx[8];
    if (raw_ch2 & 0x00800000) raw_ch2 |= 0xFF000000;

    *ch1 = raw_ch1;
    *ch2 = raw_ch2;
    return HAL_OK;
}

/* USER CODE END PFP */


/* ===========================================================================
 *  APPLICATION ENTRY POINT
 * ===========================================================================*/
int main(void)
{
    /* HAL and clock */
    HAL_Init();
    SystemClock_Config();

    /* Peripherals - GPIO must come first */
    MX_GPIO_Init();
    MX_I2C1_Init();
    MX_SDMMC1_SD_Init();
    MX_SPI1_Init();
    MX_USART1_UART_Init();
    MX_RTC_Init();
    MX_FATFS_Init();

    /* USER CODE BEGIN 2 */

    /* ------------------------------------------------------------------
     * Declare ALL locals here, before the first goto.
     * C does not allow jumping over initialisations.
     * ------------------------------------------------------------------ */
    FATFS           fs;
    FIL             fil;
    FRESULT         fres;
    char            filename[40];   /* Extra space for _NN suffix */
    char            base_name[32];  /* Date+time portion without suffix */
    char            csv_line[160];
    RTC_TimeTypeDef sTime        = {0};
    RTC_DateTypeDef sDate        = {0};
    uint32_t        sample_count = 0;
    uint8_t         file_is_open = 0;   /* Guard: only call f_close if f_open succeeded */

    /* LIS3DH */
    uint8_t  accel_buf[6];
    int16_t  raw_x, raw_y, raw_z;
    uint8_t  reg_val;

    /* ADS1292R */
    int32_t  ecg_ch1 = 0, ecg_ch2 = 0;

    /* ==========================================================
     *  STEP 1 - Initialise ADS1292R
     * ========================================================== */
    snprintf((char*)test_status, sizeof(test_status), "Init: ADS1292R...");
    if (ADS_Init() != HAL_OK)
    {
        /* test_status already describes the failure; skip to cleanup */
        goto log_end;
    }

    /* ==========================================================
     *  STEP 2 - Initialise LIS3DH
     *  CTRL_REG1 (0x20): ODR=100Hz, all axes on  -> 0x57
     *  CTRL_REG4 (0x23): BDU=1, +/-2g, HR=1      -> 0x88
     * ========================================================== */
    snprintf((char*)test_status, sizeof(test_status), "Init: LIS3DH...");
    reg_val = 0x57;
    HAL_I2C_Mem_Write(&hi2c1, 0x18 << 1, 0x20, I2C_MEMADD_SIZE_8BIT, &reg_val, 1, 10);
    reg_val = 0x88;
    HAL_I2C_Mem_Write(&hi2c1, 0x18 << 1, 0x23, I2C_MEMADD_SIZE_8BIT, &reg_val, 1, 10);
    HAL_Delay(100); /* Wait for first conversion */
    snprintf((char*)test_status, sizeof(test_status), "Init: LIS3DH OK");

    /* ==========================================================
     *  STEP 3 - Mount SD card filesystem
     * ========================================================== */
    snprintf((char*)test_status, sizeof(test_status), "FatFS: Mounting...");
    fres = f_mount(&fs, "", 1);
    if (fres != FR_OK)
    {
        snprintf((char*)test_status, sizeof(test_status), "FAIL: f_mount err=%d", fres);
        goto log_end;
    }

    /* ==========================================================
     *  STEP 4 - Build timestamped filename from RTC
     *
     *  NOTE: GetDate MUST be called after GetTime to unlock the
     *        shadow registers on STM32.
     *
     *  Collision avoidance:
     *    Try "ECG_YYYYMMDD_HHMMSS.CSV" first.
     *    If that file already exists (board reset within the same
     *    second), try "ECG_YYYYMMDD_HHMMSS_1.CSV", _2, _3 ...
     *    up to _99 before giving up.
     * ========================================================== */
    HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

    /* Build the base name (no suffix, no extension) */
    snprintf(base_name, sizeof(base_name),
             "ECG_%04d%02d%02d_%02d%02d%02d",
             2000 + sDate.Year, sDate.Month,   sDate.Date,
             sTime.Hours,       sTime.Minutes, sTime.Seconds);

    /* First attempt: no suffix */
    snprintf(filename, sizeof(filename), "%s.CSV", base_name);

    if (f_stat(filename, NULL) == FR_OK)
    {
        /* File already exists — find the first free numbered slot */
        uint8_t suffix = 1;
        do {
            snprintf(filename, sizeof(filename), "%s_%d.CSV", base_name, suffix);
            suffix++;
        } while ((f_stat(filename, NULL) == FR_OK) && (suffix <= 99));

        if (suffix > 99)
        {
            snprintf((char*)test_status, sizeof(test_status),
                     "FAIL: No free filename slot for %s", base_name);
            goto log_end;
        }
    }

    snprintf((char*)test_status, sizeof(test_status), "FatFS: Creating %s", filename);
    HAL_Delay(200);

    /* ==========================================================
     *  STEP 5 - Open / create the CSV file
     *           FA_CREATE_NEW is used instead of FA_CREATE_ALWAYS
     *           so a logic error never silently overwrites data.
     * ========================================================== */
    fres = f_open(&fil, filename, FA_CREATE_NEW | FA_WRITE);
    if (fres != FR_OK)
    {
        snprintf((char*)test_status, sizeof(test_status), "FAIL: f_open err=%d", fres);
        goto log_end;
    }
    file_is_open = 1; /* Mark file as open so log_end knows to call f_close */

    /* Write CSV header */
    f_puts("timestamp,accel_x,accel_y,accel_z,ecg_ch1,ecg_ch2\r\n", &fil);
    snprintf((char*)test_status, sizeof(test_status), "Logging: %s", filename);

    /* ==========================================================
     *  STEP 6 - Main logging loop
     *
     *  Loop rate is governed by ADS1292R DRDY (~500 SPS = 2 ms/sample).
     *  LIS3DH is read every iteration; BDU=1 ensures you always get a
     *  complete, consistent sample even when reading faster than its ODR.
     *  f_sync is called every 500 samples (~1 s) to protect against
     *  data loss on unexpected power removal.
     * ========================================================== */
    while (1)
    {
        /* -- ADS1292R: wait for DRDY then read 9-byte frame -- */
        if (ADS_ReadData(&ecg_ch1, &ecg_ch2) != HAL_OK)
        {
            /* DRDY did not assert within timeout; skip this sample */
            snprintf((char*)test_status, sizeof(test_status),
                     "WARN: ADS DRDY timeout at sample %lu", sample_count);
            continue;
        }

        /* -- LIS3DH: read all 6 axis bytes in one auto-increment transaction --
         *    Register 0x28 | 0x80 = OUT_X_L with auto-increment bit set      */
        HAL_I2C_Mem_Read(&hi2c1, 0x18 << 1, 0x28 | 0x80,
                         I2C_MEMADD_SIZE_8BIT, accel_buf, 6, 10);

        /* High-res mode: data is left-aligned in 16-bit register.
         * Right-shift by 4 to obtain 12-bit signed counts.                   */
        raw_x = (int16_t)((accel_buf[1] << 8) | accel_buf[0]) >> 4;
        raw_y = (int16_t)((accel_buf[3] << 8) | accel_buf[2]) >> 4;
        raw_z = (int16_t)((accel_buf[5] << 8) | accel_buf[4]) >> 4;

        /* -- RTC timestamp (GetDate must follow GetTime) -- */
        HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
        HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

        /* -- Format and write one CSV row -- */
        snprintf(csv_line, sizeof(csv_line),
                 "%02d:%02d:%02d,%d,%d,%d,%ld,%ld\r\n",
                 sTime.Hours, sTime.Minutes, sTime.Seconds,
                 raw_x, raw_y, raw_z,
                 ecg_ch1, ecg_ch2);
        f_puts(csv_line, &fil);
        sample_count++;

        /* -- Flush to card every 500 samples to guard against power loss -- */
        if (sample_count % 500 == 0)
        {
            f_sync(&fil);
            snprintf((char*)test_status, sizeof(test_status),
                     "Logging: %lu samples", sample_count);
        }

        /* No HAL_Delay - loop rate is set by ADS1292R DRDY */
    }

    /* ==========================================================
     *  CLEANUP  (reached only via goto on initialisation failure)
     *  file_is_open guards against calling f_close on an
     *  uninitialised FIL struct, which would cause a HardFault.
     * ========================================================== */
    log_end:
        if (file_is_open) f_close(&fil);
        f_mount(NULL, "", 0); /* Unmount before power-down */

    /* USER CODE END 2 */

    /* Should not reach here during normal operation */
    while (1) { }
}


/* ===========================================================================
 *  PERIPHERAL INITIALISERS
 * ===========================================================================*/

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
        Error_Handler();

    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_LSI
                                          | RCC_OSCILLATORTYPE_LSE
                                          | RCC_OSCILLATORTYPE_MSI;
    RCC_OscInitStruct.LSEState            = RCC_LSE_ON;
    RCC_OscInitStruct.LSIState            = RCC_LSI_ON;
    RCC_OscInitStruct.MSIState            = RCC_MSI_ON;
    RCC_OscInitStruct.MSICalibrationValue = 0;
    RCC_OscInitStruct.MSIClockRange       = RCC_MSIRANGE_6;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_MSI;
    RCC_OscInitStruct.PLL.PLLM            = 1;
    RCC_OscInitStruct.PLL.PLLN            = 40;
    RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV7;
    RCC_OscInitStruct.PLL.PLLQ            = RCC_PLLQ_DIV2;
    RCC_OscInitStruct.PLL.PLLR            = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
        Error_Handler();

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK  | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
        Error_Handler();

    HAL_RCCEx_EnableMSIPLLMode();
}

static void MX_I2C1_Init(void)
{
    hi2c1.Instance              = I2C1;
    hi2c1.Init.Timing           = 0x00D09BE3; /* ~100 kHz at 80 MHz PCLK */
    hi2c1.Init.OwnAddress1      = 0;
    hi2c1.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2      = 0;
    hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    hi2c1.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1)                                               != HAL_OK) Error_Handler();
    if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE)      != HAL_OK) Error_Handler();
    if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0)                           != HAL_OK) Error_Handler();
}

static void MX_RTC_Init(void)
{
    /* NOTE: The sentinel-clearing line has been REMOVED.
     *
     * The backup register (DR0 = 0xBEEF) is written only once on the very
     * first boot after flashing. From that point on, the RTC runs freely
     * off the LSE / VBAT and is NEVER reset again on subsequent boots or
     * power cycles. This means the filename timestamps and CSV timestamps
     * will always reflect real elapsed time, as long as VBAT is maintained.
     *
     * If you ever need to re-set the time, clear DR0 manually via the
     * debugger (write 0x0000 to RTC_BKP_DR0) before the next power-on,
     * update the time values below, and reflash.
     */

    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    hrtc.Instance            = RTC;
    hrtc.Init.HourFormat     = RTC_HOURFORMAT_24;
    hrtc.Init.AsynchPrediv   = 127;
    hrtc.Init.SynchPrediv    = 255;
    hrtc.Init.OutPut         = RTC_OUTPUT_DISABLE;
    hrtc.Init.OutPutRemap    = RTC_OUTPUT_REMAP_NONE;
    hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    hrtc.Init.OutPutType     = RTC_OUTPUT_TYPE_OPENDRAIN;
    if (HAL_RTC_Init(&hrtc) != HAL_OK)
        Error_Handler();

    /* CubeMX requires an unconditional SetTime/SetDate after HAL_RTC_Init.
     * These defaults are immediately overridden by the sentinel block below
     * on first boot, and skipped entirely on every subsequent boot.        */
    sTime.Hours          = 0x00;
    sTime.Minutes        = 0x00;
    sTime.Seconds        = 0x00;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;
    if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
        Error_Handler();

    sDate.WeekDay = RTC_WEEKDAY_MONDAY;
    sDate.Month   = RTC_MONTH_JANUARY;
    sDate.Date    = 0x01;
    sDate.Year    = 0x00;
    if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BCD) != HAL_OK)
        Error_Handler();

    /* First-boot guard: backup register DR0 survives soft resets and
     * power cycles (requires VBAT).
     *
     * On the FIRST boot after flashing (DR0 != 0xBEEF):
     *   - Sets the RTC to the values below
     *   - Writes 0xBEEF to DR0
     *
     * On ALL SUBSEQUENT boots (DR0 == 0xBEEF):
     *   - This entire block is skipped
     *   - The RTC keeps its current (running) time
     */
    if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != 0xBEEF)
    {
        /* Time set: 16:13:00  (4:13 PM)  on Friday 29 May 2026 */
        sTime.Hours          = 0x16; /* 16 BCD */
        sTime.Minutes        = 0x13; /* 13 BCD */
        sTime.Seconds        = 0x00;
        sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
        sTime.StoreOperation = RTC_STOREOPERATION_RESET;
        HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD);

        sDate.WeekDay = RTC_WEEKDAY_FRIDAY;
        sDate.Month   = RTC_MONTH_MAY;
        sDate.Date    = 0x29; /* 29 BCD */
        sDate.Year    = 0x26; /* 2026 -> 26 BCD */
        HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BCD);

        /* Stamp the sentinel so this block never runs again */
        HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, 0xBEEF);
    }
}

static void MX_SDMMC1_SD_Init(void)
{
    hsd1.Instance                 = SDMMC1;
    hsd1.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
    hsd1.Init.ClockBypass         = SDMMC_CLOCK_BYPASS_DISABLE;
    hsd1.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd1.Init.BusWide             = SDMMC_BUS_WIDE_4B;
    hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd1.Init.ClockDiv            = 7; /* Conservative; reliable with 10k pull-ups */
    if (HAL_SD_Init(&hsd1) != HAL_OK)
        Error_Handler();
    if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK)
        Error_Handler();
}

static void MX_SPI1_Init(void)
{
    /* ADS1292R requires SPI Mode 1: CPOL=0, CPHA=1 (SPI_PHASE_2EDGE).
     * Prescaler 256 -> ~312 kHz at 80 MHz, within ADS1292R's 1 MHz max.
     * These settings match the verified working ADS1292.txt reference.   */
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_2EDGE; /* Mode 1 */
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial     = 7;
    hspi1.Init.CRCLength         = SPI_CRC_LENGTH_DATASIZE;
    hspi1.Init.NSSPMode          = SPI_NSS_PULSE_ENABLE;
    if (HAL_SPI_Init(&hspi1) != HAL_OK)
        Error_Handler();
}

static void MX_USART1_UART_Init(void)
{
    huart1.Instance                    = USART1;
    huart1.Init.BaudRate               = 115200;
    huart1.Init.WordLength             = UART_WORDLENGTH_8B;
    huart1.Init.StopBits               = UART_STOPBITS_1;
    huart1.Init.Parity                 = UART_PARITY_NONE;
    huart1.Init.Mode                   = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart1.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart1) != HAL_OK)
        Error_Handler();
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* Set idle output levels BEFORE configuring pins as outputs:
     *   PC4  (ADS RESET/PWDN) -> LOW  (held in reset until ADS_Init runs)
     *   PA4  (ADS CS)         -> HIGH (de-asserted)
     *   PC13 (LED)            -> LOW  (off)
     *   PB1, PB2, PB8        -> LOW                                        */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13 | GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4,                GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_8, GPIO_PIN_RESET);

    /* PC13 (LED), PC4 (ADS RESET/PWDN) */
    GPIO_InitStruct.Pin   = GPIO_PIN_13 | GPIO_PIN_4;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* PA0 - general input */
    GPIO_InitStruct.Pin  = GPIO_PIN_0;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* PA4 - ADS1292R chip-select (software NSS) */
    GPIO_InitStruct.Pin   = GPIO_PIN_4;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* PB1, PB2, PB8 - general purpose outputs */
    GPIO_InitStruct.Pin   = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_8;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* PB5 - spare interrupt input
     * PB9 - ADS1292R DRDY (active-low, polled in ADS_ReadData)            */
    GPIO_InitStruct.Pin  = GPIO_PIN_5 | GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

void Error_Handler(void)
{
    __disable_irq();
    while (1) { }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file; (void)line;
}
#endif
