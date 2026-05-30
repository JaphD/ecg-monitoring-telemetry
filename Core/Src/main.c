/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : ADS1292R ECG + LIS3DH IMU logger → SD card → A7670G upload
  *
  * Peripherals
  * -----------
  *  SPI1   - ADS1292R  (Mode 1: CPOL=0 CPHA=1, SPI_PHASE_2EDGE)
  *           CS      -> PA4  (active-LOW, software NSS)
  *           RESET   -> PC4  (active-LOW)
  *           DRDY    -> PB9  (active-LOW, polled)
  *
  *  I2C1   - LIS3DH IMU  (7-bit addr 0x18, SA0 tied LOW)
  *
  *  SDMMC1 - microSD via FatFS  (4-bit bus, ClockDiv=7)
  *
  *  RTC    - timestamps in filenames and CSV rows
  *           First-boot time set via backup-register sentinel 0xBEEF.
  *
  *  USART1 - A7670G modem (115200 8N1)
  *

  *  PB2    - PWRKEY via NPN (MCU HIGH = PWRKEY pulled to GND)
  *  PB1    - RESET  via NPN (MCU HIGH = RESET  pulled to GND)
  *
  * Operational loop (runs forever)
  * --------------------------------
  *  1. Shut modem down via PWRKEY (if running)
  *  2. Log ECG + IMU to SD card for LOG_DURATION_MS (3 min)
  *  3. Close and mark CSV file with .RDY extension
  *  4. Boot modem via PWRKEY pulse, UART sync
  *  5. Upload CSV in ≤256 KB chunks via AT+HTTP*
  *     - On success  : rename file to .DONE
  *     - On failure  : retry up to UPLOAD_MAX_RETRIES times,
  *                     then rename to .FAIL and continue
  *  6. Shut modem down, go to step 2
  *
  * CSV columns
  * -----------
  *  timestamp, accel_x, accel_y, accel_z, ecg_ch1, ecg_ch2
  *
  * File naming
  * -----------
  *  ECG_YYYYMMDD_HHMMSS.RDY   (logging complete, awaiting upload)
  *  ECG_YYYYMMDD_HHMMSS.DONE  (uploaded successfully)
  *  ECG_YYYYMMDD_HHMMSS.FAIL  (all retries exhausted)
  *
  *  Collision avoidance: if the base name already exists a _N suffix
  *  is appended (N = 1..99) before the extension.
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
/* Live-expression watch variable */
volatile char test_status[128]        = "Initializing...";
volatile char last_modem_response[256] = {0};
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_SDMMC1_SD_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_RTC_Init(void);
uint8_t Send_AT_Command(const char* cmd, const char* expected_response, uint32_t timeout);

/* USER CODE BEGIN PFP */

/* ===========================================================================
 *  COMPILE-TIME CONFIGURATION
 * ===========================================================================*/

/** Total logging window per session (milliseconds). 3 minutes = 180 000 ms */
#define LOG_DURATION_MS         180000UL

/** Maximum bytes declared per AT+HTTPDATA call (logical POST limit).
 *  A7670G hard limit is 319 488; keep well below for safety.
 *  No RAM buffer of this size is allocated — data is streamed from SD. */
#define UPLOAD_CHUNK_SIZE       (256UL * 1024UL)  /* 256 KB */

/** Strip buffer: bytes read from SD and written to UART in one go.
 *  Must fit in RAM alongside FatFS buffers. 512 = one SD sector.    */
#define UPLOAD_STRIP_SIZE       512U

/** Number of full upload attempts before flagging a file as failed. */
#define UPLOAD_MAX_RETRIES      3

/** HTTP POST target.  Replace with your real endpoint. */
#define UPLOAD_URL              "http://httpbin.org/post"

/* ===========================================================================
 *  ADS1292R SPI HELPERS
 * ===========================================================================*/

static inline void ADS_CS_Low(void)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_Delay(1);
}

static inline void ADS_CS_High(void)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_Delay(1);
}

static void ADS_SendCommand(uint8_t cmd)
{
    ADS_CS_Low();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, 10);
    ADS_CS_High();
    HAL_Delay(2);
}

static void ADS_WriteReg(uint8_t reg, uint8_t value)
{
    uint8_t cmd[2] = { (uint8_t)(0x40 | (reg & 0x1F)), 0x00 };
    ADS_CS_Low();
    HAL_SPI_Transmit(&hspi1, cmd,    2, 10);
    HAL_SPI_Transmit(&hspi1, &value, 1, 10);
    ADS_CS_High();
    HAL_Delay(2);
}

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
 * @return HAL_OK if device ID is valid, HAL_ERROR otherwise.
 */
static HAL_StatusTypeDef ADS_Init(void)
{
    ADS_CS_High();
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_Delay(2000);

    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_Delay(200);

    snprintf((char*)test_status, sizeof(test_status), "ADS1292R: SDATAC...");
    ADS_SendCommand(0x11);
    HAL_Delay(200);

    snprintf((char*)test_status, sizeof(test_status), "ADS1292R: Reading ID...");
    uint8_t id = ADS_ReadReg(0x00);

    if (id == 0x73)
        snprintf((char*)test_status, sizeof(test_status), "ADS1292R ID=0x%02X OK", id);
    else if (id == 0x72)
        snprintf((char*)test_status, sizeof(test_status), "ADS1292 (non-R) ID=0x%02X", id);
    else
    {
        snprintf((char*)test_status, sizeof(test_status),
                 "FAIL: ADS ID=0x%02X (expected 0x73)", id);
        return HAL_ERROR;
    }

    ADS_WriteReg(0x01, 0x02); /* CONFIG1  : 500 SPS                  */
    ADS_WriteReg(0x02, 0xE0); /* CONFIG2  : ref buf on, 2.42 V ref   */
    ADS_WriteReg(0x03, 0x10); /* LOFF     : DC lead-off, 95% thr     */
    ADS_WriteReg(0x04, 0x00); /* CH1SET   : gain=6, normal input      */
    ADS_WriteReg(0x05, 0x00); /* CH2SET   : same as CH1               */
    ADS_WriteReg(0x06, 0x2C); /* RLD_SENS : RLD from CH1+CH2          */
    ADS_WriteReg(0x07, 0x0F); /* LOFF_SENS: all inputs                */
    ADS_WriteReg(0x09, 0xEA); /* RESP1    : resp demod+mod on         */
    ADS_WriteReg(0x0A, 0x03); /* RESP2    : RLDREF internal           */
    HAL_Delay(10);

    ADS_SendCommand(0x08); /* START  */
    HAL_Delay(10);
    ADS_SendCommand(0x10); /* RDATAC */
    HAL_Delay(10);

    snprintf((char*)test_status, sizeof(test_status), "ADS1292R: Init OK, streaming");
    return HAL_OK;
}

/**
 * @brief Read one 9-byte frame from ADS1292R (RDATAC mode).
 *        Polls DRDY (PB9 active-LOW) with a 10 ms timeout.
 */
static HAL_StatusTypeDef ADS_ReadData(int32_t *ch1, int32_t *ch2)
{
    uint32_t t_start = HAL_GetTick();
    while (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_SET)
    {
        if ((HAL_GetTick() - t_start) > 10)
            return HAL_TIMEOUT;
    }

    uint8_t rx[9] = {0};
    uint8_t tx[9] = {0};
    ADS_CS_Low();
    HAL_SPI_TransmitReceive(&hspi1, tx, rx, 9, 20);
    ADS_CS_High();

    int32_t raw_ch1 = ((int32_t)rx[3] << 16) | ((int32_t)rx[4] << 8) | rx[5];
    if (raw_ch1 & 0x00800000) raw_ch1 |= 0xFF000000;

    int32_t raw_ch2 = ((int32_t)rx[6] << 16) | ((int32_t)rx[7] << 8) | rx[8];
    if (raw_ch2 & 0x00800000) raw_ch2 |= 0xFF000000;

    *ch1 = raw_ch1;
    *ch2 = raw_ch2;
    return HAL_OK;
}

/* ===========================================================================
 *  A7670G MODEM HELPERS
 * ===========================================================================*/

/**
 * @brief Wake the A7670G by pulsing PWRKEY (board is USB-C powered;
 *        VBAT is always present — no load switch exists).
 *
 *        Sequence:
 *          1. Ensure PWRKEY and RESET lines are released (NPN OFF)
 *          2. Pulse PWRKEY LOW for 600 ms via NPN (MCU HIGH = PWRKEY GND)
 *          3. Wait ~5 s for the module OS to boot
 *          4. Auto-baud sync on USART1
 *
 * @return 1 if UART link established, 0 on failure.
 */
static uint8_t Modem_PowerOn(void)
{
    snprintf((char*)test_status, sizeof(test_status), "Modem: Releasing control lines...");
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET); /* RESET NPN OFF  */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_RESET); /* PWRKEY NPN OFF */
    HAL_Delay(200);

    snprintf((char*)test_status, sizeof(test_status), "Modem: PWRKEY pulse...");
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_SET);   /* NPN ON  -> PWRKEY LOW */
    HAL_Delay(600);                                        /* >500 ms required      */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_RESET); /* NPN OFF -> PWRKEY float */

    snprintf((char*)test_status, sizeof(test_status), "Modem: Waiting for boot (~5s)...");
    HAL_Delay(5000);

    snprintf((char*)test_status, sizeof(test_status), "Modem: UART sync...");
    Send_AT_Command("AT\r\n", "OK", 1000); /* Prime auto-baud */
    HAL_Delay(100);
    if (!Send_AT_Command("AT\r\n", "OK", 2000))
    {
        snprintf((char*)test_status, sizeof(test_status), "FAIL: Modem UART link lost");
        return 0;
    }

    snprintf((char*)test_status, sizeof(test_status), "Modem: Online");
    return 1;
}

/**
 * @brief Shut down the A7670G cleanly via a long PWRKEY pulse (≥2 s).
 *        VBAT remains powered (USB-C supply); only the module is turned off.
 */
static void Modem_PowerOff(void)
{
    snprintf((char*)test_status, sizeof(test_status), "Modem: Shutting down...");
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_SET);   /* NPN ON  -> PWRKEY LOW */
    HAL_Delay(2500);                                       /* >2 s triggers power-off */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_RESET); /* NPN OFF -> release      */
    HAL_Delay(1000);                                       /* Allow module to settle  */
    snprintf((char*)test_status, sizeof(test_status), "Modem: Off");
}

/**
 * @brief Transmit one AT command and poll for expected_response within timeout.
 * @return 1 if response found, 0 on timeout.
 */
uint8_t Send_AT_Command(const char* cmd, const char* expected_response, uint32_t timeout)
{
    uint32_t tickstart = HAL_GetTick();
    uint16_t idx = 0;
    uint8_t  ch;

    memset((char*)last_modem_response, 0, sizeof(last_modem_response));
    __HAL_UART_CLEAR_FLAG(&huart1, UART_CLEAR_OREF | UART_CLEAR_NEF |
                                   UART_CLEAR_FEF  | UART_CLEAR_PEF);
    HAL_UART_Transmit(&huart1, (uint8_t*)cmd, strlen(cmd), 1000);

    while ((HAL_GetTick() - tickstart) < timeout)
    {
        if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE))
            __HAL_UART_CLEAR_FLAG(&huart1, UART_CLEAR_OREF);

        if (HAL_UART_Receive(&huart1, &ch, 1, 10) == HAL_OK)
        {
            if (idx < (sizeof(last_modem_response) - 2))
            {
                last_modem_response[idx++] = (char)ch;
                last_modem_response[idx]   = '\0';
                if (strstr((char*)last_modem_response, expected_response))
                    return 1;
            }
        }
    }
    return 0;
}

/**
 * @brief Send a small buffer of raw bytes to the modem UART.
 *        Called repeatedly with UPLOAD_STRIP_SIZE-sized slices.
 */
static void Modem_SendRaw(const uint8_t *data, uint16_t len)
{
    HAL_UART_Transmit(&huart1, (uint8_t*)data, len, 2000);
}

/* ===========================================================================
 *  UPLOAD LOGIC
 * ===========================================================================*/

/**
 * @brief Upload one .RDY file to the server, streaming SD → UART directly.
 *
 *        RAM usage: one 512-byte strip buffer (one SD sector).
 *        No large intermediate buffer is allocated.
 *
 *        The file is split into logical POST chunks of up to UPLOAD_CHUNK_SIZE
 *        bytes. For each chunk:
 *          AT+HTTPINIT
 *          AT+HTTPPARA="CID",1
 *          AT+HTTPPARA="URL","<UPLOAD_URL>"
 *          AT+HTTPPARA="CONTENT","text/csv"
 *          AT+HTTPDATA=<chunk_bytes>,30000  -> wait for "DOWNLOAD" prompt
 *          <stream chunk_bytes from SD in UPLOAD_STRIP_SIZE strips>
 *          AT+HTTPACTION=1                  -> POST, wait for +HTTPACTION:
 *          AT+HTTPTERM
 *
 * @param  filename  FatFS path to the .RDY file.
 * @return 1 on complete success, 0 on any failure.
 */
static uint8_t Upload_File(const char *filename)
{
    FIL     fil;
    FRESULT fres;
    UINT    bytes_read;

    /* Single-sector strip buffer — only 512 bytes in RAM */
    static uint8_t strip[UPLOAD_STRIP_SIZE];

    fres = f_open(&fil, filename, FA_READ);
    if (fres != FR_OK)
    {
        snprintf((char*)test_status, sizeof(test_status),
                 "Upload: f_open failed (%d)", fres);
        return 0;
    }

    FSIZE_t  file_size      = f_size(&fil);
    FSIZE_t  file_pos       = 0;
    uint32_t chunk_num      = 0;
    char     httpdata_cmd[64];

    while (file_pos < file_size)
    {
        chunk_num++;

        /* How many bytes does this POST chunk cover? */
        uint32_t chunk_bytes = (uint32_t)(file_size - file_pos);
        if (chunk_bytes > UPLOAD_CHUNK_SIZE)
            chunk_bytes = (uint32_t)UPLOAD_CHUNK_SIZE;

        snprintf((char*)test_status, sizeof(test_status),
                 "Upload: chunk %lu (%lu B, pos %lu)...",
                 chunk_num, chunk_bytes, (uint32_t)file_pos);

        /* ---- HTTP session setup ---- */
        if (!Send_AT_Command("AT+HTTPINIT\r\n", "OK", 5000))
        {
            snprintf((char*)test_status, sizeof(test_status),
                     "Upload: HTTPINIT failed (chunk %lu)", chunk_num);
            f_close(&fil);
            return 0;
        }
        if (!Send_AT_Command("AT+HTTPPARA=\"CID\",1\r\n", "OK", 3000))
        {
            Send_AT_Command("AT+HTTPTERM\r\n", "OK", 3000);
            f_close(&fil);
            return 0;
        }
        if (!Send_AT_Command("AT+HTTPPARA=\"URL\",\"" UPLOAD_URL "\"\r\n", "OK", 3000))
        {
            Send_AT_Command("AT+HTTPTERM\r\n", "OK", 3000);
            f_close(&fil);
            return 0;
        }
        if (!Send_AT_Command("AT+HTTPPARA=\"CONTENT\",\"text/csv\"\r\n", "OK", 3000))
        {
            Send_AT_Command("AT+HTTPTERM\r\n", "OK", 3000);
            f_close(&fil);
            return 0;
        }

        /* AT+HTTPDATA tells the modem exactly how many bytes are coming.
         * 30 000 ms gives plenty of time to stream from slow SD cards.  */
        snprintf(httpdata_cmd, sizeof(httpdata_cmd),
                 "AT+HTTPDATA=%lu,30000\r\n", chunk_bytes);

        if (!Send_AT_Command(httpdata_cmd, "DOWNLOAD", 10000))
        {
            snprintf((char*)test_status, sizeof(test_status),
                     "Upload: HTTPDATA prompt timeout (chunk %lu)", chunk_num);
            Send_AT_Command("AT+HTTPTERM\r\n", "OK", 3000);
            f_close(&fil);
            return 0;
        }

        /* ---- Stream chunk_bytes from SD → UART in 512-byte strips ---- */
        uint32_t streamed = 0;
        while (streamed < chunk_bytes)
        {
            uint32_t want = chunk_bytes - streamed;
            if (want > UPLOAD_STRIP_SIZE) want = UPLOAD_STRIP_SIZE;

            fres = f_read(&fil, strip, (UINT)want, &bytes_read);
            if (fres != FR_OK || bytes_read == 0)
            {
                snprintf((char*)test_status, sizeof(test_status),
                         "Upload: f_read err=%d (chunk %lu, strip %lu B)",
                         fres, chunk_num, streamed);
                Send_AT_Command("AT+HTTPTERM\r\n", "OK", 3000);
                f_close(&fil);
                return 0;
            }
            Modem_SendRaw(strip, (uint16_t)bytes_read);
            streamed += bytes_read;
        }

        /* Modem signals it has received all declared bytes with "OK" */
        if (!Send_AT_Command("", "OK", 10000))
        {
            snprintf((char*)test_status, sizeof(test_status),
                     "Upload: data ACK timeout (chunk %lu)", chunk_num);
            Send_AT_Command("AT+HTTPTERM\r\n", "OK", 3000);
            f_close(&fil);
            return 0;
        }

        /* POST — server has up to 35 s to respond */
        if (!Send_AT_Command("AT+HTTPACTION=1\r\n", "+HTTPACTION:", 35000))
        {
            snprintf((char*)test_status, sizeof(test_status),
                     "Upload: POST timeout (chunk %lu)", chunk_num);
            Send_AT_Command("AT+HTTPTERM\r\n", "OK", 3000);
            f_close(&fil);
            return 0;
        }

        if (!strstr((char*)last_modem_response, ",200,"))
        {
            snprintf((char*)test_status, sizeof(test_status),
                     "Upload: non-200 (chunk %lu): %.60s",
                     chunk_num, (char*)last_modem_response);
            Send_AT_Command("AT+HTTPTERM\r\n", "OK", 3000);
            f_close(&fil);
            return 0;
        }

        Send_AT_Command("AT+HTTPTERM\r\n", "OK", 3000);
        file_pos += chunk_bytes;

        snprintf((char*)test_status, sizeof(test_status),
                 "Upload: chunk %lu OK (%lu/%lu B)",
                 chunk_num, (uint32_t)file_pos, (uint32_t)file_size);
    }

    f_close(&fil);
    snprintf((char*)test_status, sizeof(test_status),
             "Upload: DONE (%lu chunks)", chunk_num);
    return 1;
}

/**
 * @brief Rename a FatFS file by copying then deleting the original.
 *        FatFS f_rename cannot move across drives but works fine here.
 */
static void Rename_File(const char *old_name, const char *new_name)
{
    f_rename(old_name, new_name);
}

/* USER CODE END PFP */


/* ===========================================================================
 *  APPLICATION ENTRY POINT
 * ===========================================================================*/
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_I2C1_Init();
    MX_SDMMC1_SD_Init();
    MX_SPI1_Init();
    MX_USART1_UART_Init();
    MX_RTC_Init();
    MX_FATFS_Init();

    /* USER CODE BEGIN 2 */

    /* Locals declared before any goto */
    FATFS           fs;
    FIL             fil;
    FRESULT         fres;
    char            base_name[32];
    char            filename_rdy[48];   /* ECG_....RDY  */
    char            filename_done[48];  /* ECG_....DONE */
    char            filename_fail[48];  /* ECG_....FAIL */
    char            csv_line[160];
    RTC_TimeTypeDef sTime        = {0};
    RTC_DateTypeDef sDate        = {0};
    uint32_t        sample_count = 0;
    uint8_t         file_is_open = 0;

    /* LIS3DH */
    uint8_t  accel_buf[6];
    int16_t  raw_x, raw_y, raw_z;
    uint8_t  reg_val;

    /* ADS1292R */
    int32_t  ecg_ch1 = 0, ecg_ch2 = 0;

    /* Mount filesystem once — stays mounted for the lifetime of the program */
    snprintf((char*)test_status, sizeof(test_status), "FatFS: Mounting...");
    fres = f_mount(&fs, "", 1);
    if (fres != FR_OK)
    {
        snprintf((char*)test_status, sizeof(test_status),
                 "FAIL: f_mount err=%d", fres);
        goto fatal_error;
    }

    /* ======================================================================
     *  MAIN OPERATIONAL LOOP  (log → upload → repeat)
     * ====================================================================== */
    while (1)
    {
        /* ------------------------------------------------------------------
         *  PHASE 0 — Ensure modem is OFF before logging starts
         * ------------------------------------------------------------------ */
        Modem_PowerOff();

        /* ------------------------------------------------------------------
         *  PHASE 1 — Initialise sensors
         * ------------------------------------------------------------------ */
        snprintf((char*)test_status, sizeof(test_status), "Init: ADS1292R...");
        if (ADS_Init() != HAL_OK)
            goto fatal_error;   /* test_status already set by ADS_Init */

        snprintf((char*)test_status, sizeof(test_status), "Init: LIS3DH...");
        reg_val = 0x57; /* ODR=100 Hz, XYZ on */
        HAL_I2C_Mem_Write(&hi2c1, 0x18 << 1, 0x20,
                          I2C_MEMADD_SIZE_8BIT, &reg_val, 1, 10);
        reg_val = 0x88; /* BDU=1, +/-2 g, HR=1 */
        HAL_I2C_Mem_Write(&hi2c1, 0x18 << 1, 0x23,
                          I2C_MEMADD_SIZE_8BIT, &reg_val, 1, 10);
        HAL_Delay(100);
        snprintf((char*)test_status, sizeof(test_status), "Init: LIS3DH OK");

        /* ------------------------------------------------------------------
         *  PHASE 2 — Build timestamped filename
         * ------------------------------------------------------------------ */
        HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
        HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

        snprintf(base_name, sizeof(base_name),
                 "ECG_%04d%02d%02d_%02d%02d%02d",
                 2000 + sDate.Year, sDate.Month,   sDate.Date,
                 sTime.Hours,       sTime.Minutes, sTime.Seconds);

        /* Try base.RDY; if it exists, append _1, _2 ... _99 */
        snprintf(filename_rdy,  sizeof(filename_rdy),  "%s.RDY",  base_name);
        snprintf(filename_done, sizeof(filename_done), "%s.DONE", base_name);
        snprintf(filename_fail, sizeof(filename_fail), "%s.FAIL", base_name);

        if (f_stat(filename_rdy, NULL) == FR_OK)
        {
            uint8_t suffix = 1;
            do {
                snprintf(filename_rdy,  sizeof(filename_rdy),
                         "%s_%d.RDY",  base_name, suffix);
                snprintf(filename_done, sizeof(filename_done),
                         "%s_%d.DONE", base_name, suffix);
                snprintf(filename_fail, sizeof(filename_fail),
                         "%s_%d.FAIL", base_name, suffix);
                suffix++;
            } while ((f_stat(filename_rdy, NULL) == FR_OK) && (suffix <= 99));

            if (suffix > 99)
            {
                snprintf((char*)test_status, sizeof(test_status),
                         "FAIL: No free filename slot");
                goto fatal_error;
            }
        }

        /* ------------------------------------------------------------------
         *  PHASE 3 — Open CSV file and write header
         * ------------------------------------------------------------------ */
        snprintf((char*)test_status, sizeof(test_status),
                 "FatFS: Creating %s", filename_rdy);
        HAL_Delay(200);

        fres = f_open(&fil, filename_rdy, FA_CREATE_NEW | FA_WRITE);
        if (fres != FR_OK)
        {
            snprintf((char*)test_status, sizeof(test_status),
                     "FAIL: f_open err=%d", fres);
            goto fatal_error;
        }
        file_is_open  = 1;
        sample_count  = 0;

        f_puts("timestamp,accel_x,accel_y,accel_z,ecg_ch1,ecg_ch2\r\n", &fil);

        /* ------------------------------------------------------------------
         *  PHASE 4 — Logging loop (runs for LOG_DURATION_MS)
         * ------------------------------------------------------------------ */
        uint32_t log_start = HAL_GetTick();
        snprintf((char*)test_status, sizeof(test_status),
                 "Logging: %s (3 min)", filename_rdy);

        while ((HAL_GetTick() - log_start) < LOG_DURATION_MS)
        {
            /* ADS1292R: wait for DRDY then read frame */
            if (ADS_ReadData(&ecg_ch1, &ecg_ch2) != HAL_OK)
            {
                /* DRDY timeout — skip sample, keep logging */
                continue;
            }

            /* LIS3DH: 6-byte burst read starting at OUT_X_L (auto-increment) */
            HAL_I2C_Mem_Read(&hi2c1, 0x18 << 1, 0x28 | 0x80,
                             I2C_MEMADD_SIZE_8BIT, accel_buf, 6, 10);

            raw_x = (int16_t)((accel_buf[1] << 8) | accel_buf[0]) >> 4;
            raw_y = (int16_t)((accel_buf[3] << 8) | accel_buf[2]) >> 4;
            raw_z = (int16_t)((accel_buf[5] << 8) | accel_buf[4]) >> 4;

            HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
            HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

            snprintf(csv_line, sizeof(csv_line),
                     "%02d:%02d:%02d,%d,%d,%d,%ld,%ld\r\n",
                     sTime.Hours, sTime.Minutes, sTime.Seconds,
                     raw_x, raw_y, raw_z,
                     ecg_ch1, ecg_ch2);
            f_puts(csv_line, &fil);
            sample_count++;

            /* Flush every 500 samples (~1 s at 500 SPS) */
            if (sample_count % 500 == 0)
            {
                f_sync(&fil);
                snprintf((char*)test_status, sizeof(test_status),
                         "Logging: %lu samples", sample_count);
            }
        }

        /* ------------------------------------------------------------------
         *  PHASE 5 — Close file (logging complete)
         * ------------------------------------------------------------------ */
        f_sync(&fil);
        f_close(&fil);
        file_is_open = 0;

        snprintf((char*)test_status, sizeof(test_status),
                 "Log done: %lu samples -> %s", sample_count, filename_rdy);
        HAL_Delay(500);

        /* ------------------------------------------------------------------
         *  PHASE 6 — Boot modem
         * ------------------------------------------------------------------ */
        if (!Modem_PowerOn())
        {
            /* Modem failed to come online — flag file and continue */
            snprintf((char*)test_status, sizeof(test_status),
                     "FAIL: Modem offline, flagging %s", filename_rdy);
            Rename_File(filename_rdy, filename_fail);
            continue; /* Back to top of main loop */
        }

        /* Check signal quality before attempting upload */
        if (!Send_AT_Command("AT+CSQ\r\n", "OK", 2000))
        {
            snprintf((char*)test_status, sizeof(test_status),
                     "FAIL: No signal, flagging %s", filename_rdy);
            Modem_PowerOff();
            Rename_File(filename_rdy, filename_fail);
            continue;
        }

        /* ------------------------------------------------------------------
         *  PHASE 7 — Upload with retries
         * ------------------------------------------------------------------ */
        uint8_t upload_ok = 0;

        for (uint8_t attempt = 1; attempt <= UPLOAD_MAX_RETRIES; attempt++)
        {
            snprintf((char*)test_status, sizeof(test_status),
                     "Upload: attempt %d/%d for %s",
                     attempt, UPLOAD_MAX_RETRIES, filename_rdy);

            if (Upload_File(filename_rdy))
            {
                upload_ok = 1;
                break;
            }

            /* Upload failed — show status and wait before retry */
            snprintf((char*)test_status, sizeof(test_status),
                     "Upload: attempt %d FAILED, retrying...", attempt);
            HAL_Delay(3000);
        }

        /* ------------------------------------------------------------------
         *  PHASE 8 — Mark file and power down modem
         * ------------------------------------------------------------------ */
        Modem_PowerOff();

        if (upload_ok)
        {
            Rename_File(filename_rdy, filename_done);
            snprintf((char*)test_status, sizeof(test_status),
                     "DONE: %s uploaded OK", filename_done);
        }
        else
        {
            Rename_File(filename_rdy, filename_fail);
            snprintf((char*)test_status, sizeof(test_status),
                     "FAIL: %s — all retries exhausted", filename_fail);
        }

        HAL_Delay(1000);
        /* Loop back → new logging session */
    }

    /* ======================================================================
     *  FATAL ERROR — only reached on unrecoverable init failures
     * ====================================================================== */
    fatal_error:
        if (file_is_open) { f_close(&fil); }
        f_mount(NULL, "", 0);
        /* USER CODE END 2 */
        while (1) { HAL_Delay(100); }
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
    hi2c1.Init.Timing           = 0x00D09BE3;
    hi2c1.Init.OwnAddress1      = 0;
    hi2c1.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2      = 0;
    hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    hi2c1.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1)                                          != HAL_OK) Error_Handler();
    if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK) Error_Handler();
    if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0)                      != HAL_OK) Error_Handler();
}

static void MX_RTC_Init(void)
{
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

    /* First-boot guard: only set the clock once after flashing.
     * Clear RTC_BKP_DR0 via debugger to re-set the time.         */
    if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != 0xBEEF)
    {
        sTime.Hours          = 0x16;
        sTime.Minutes        = 0x13;
        sTime.Seconds        = 0x00;
        sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
        sTime.StoreOperation = RTC_STOREOPERATION_RESET;
        HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD);

        sDate.WeekDay = RTC_WEEKDAY_FRIDAY;
        sDate.Month   = RTC_MONTH_MAY;
        sDate.Date    = 0x29;
        sDate.Year    = 0x26;
        HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BCD);

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
    hsd1.Init.ClockDiv            = 7;
    if (HAL_SD_Init(&hsd1) != HAL_OK)
        Error_Handler();
    if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK)
        Error_Handler();
}

static void MX_SPI1_Init(void)
{
    /* ADS1292R: SPI Mode 1 (CPOL=0, CPHA=1), ~312 kHz at 80 MHz */
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_2EDGE;
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

    /* Set safe idle levels before configuring directions */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13 | GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4,                GPIO_PIN_SET);   /* ADS CS idle HIGH */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_8, GPIO_PIN_RESET);

    /* PC13 (LED), PC4 (ADS RESET) */
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

    /* PA4 - ADS1292R chip-select */
    GPIO_InitStruct.Pin   = GPIO_PIN_4;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* PB1 (modem RESET NPN), PB2 (modem PWRKEY NPN), PB8 (spare output) */
    GPIO_InitStruct.Pin   = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_8;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* PB5 (spare), PB9 (ADS1292R DRDY active-LOW, polled) */
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
