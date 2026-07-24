/*
 * SD-first ECG monitoring firmware
 *
 * Data flow:
 *   ADS1292R DRDY ISR -> RAM ring -> active SD CSV -> closed .RDY files
 *                                               -> A7670G HTTPS -> Node.js
 *
 * The SD card is the source of truth. A file is removed only after the modem
 * reports HTTP 200. Failed socket attempts leave the file queued for later.
 */

#include "main.h"
#include "fatfs.h"

#include <stdio.h>
#include <string.h>

I2C_HandleTypeDef  hi2c1;
SD_HandleTypeDef   hsd1;
SPI_HandleTypeDef  hspi1;
UART_HandleTypeDef huart1;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_SDMMC1_SD_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);

#define SAMPLE_RING_CAPACITY       2048U
#define SAMPLE_RING_MASK           (SAMPLE_RING_CAPACITY - 1U)
#define SAMPLES_PER_FILE           2500U
#define RECORD_SESSION_MAX_MS      12000U
#define RECORD_PROGRESS_TIMEOUT_MS 3000U
#define HTTP_MAX_ATTEMPTS          3U
#define HTTP_RETRY_BACKOFF_MS      5000U
#define UPLOAD_RETRY_IDLE_MS       60000U
#define SD_RECORD_START_RETRY_MS   5000U
#define ADS_START_RECOVERY_MS      5000U
#define ADS_START_MAX_ATTEMPTS     3U
#define ADS_RESTART_QUIET_MS       250U
#define ADS_ID_READ_MAX_ATTEMPTS   5U
#define ADS_ID_READ_RETRY_MS       75U
#define ADS_START_DRDY_TIMEOUT_MS  200U
#define ADS_CONFIG1_250_SPS        0x01U
#define ADS_DRDY_FALLBACK_MS       40U
#define ADS_POLL_INTERVAL_MS       4U
#define MODEM_RAIL_SETTLE_MS       100U
#define JSON_BATCH_CAPACITY        32768U
#define JSON_READINGS_PER_BATCH    250U
#define MODEM_HTTP_TX_CHUNK_SIZE   512U
#define MODEM_HTTP_TX_PACING_MS    2U
#define MODEM_APN                  "internet"
#define UPLOAD_URL                 "https://carton-cupping-modify.ngrok-free.dev/api/data"
#define SERVER_TIME_URL            "https://carton-cupping-modify.ngrok-free.dev/api/time"
#define ACTIVE_LOG_NAME            "ACTIVE.TMP"
#define CSV_HEADER                 "timestamp,accel_x,accel_y,accel_z,ecg_ch1,ecg_ch2\r\n"

typedef struct
{
    uint32_t tick_ms;
    int32_t  ecg_ch1;
    int32_t  ecg_ch2;
    int16_t  accel_x;
    int16_t  accel_y;
    int16_t  accel_z;
} SampleRecord;

static SampleRecord sample_ring[SAMPLE_RING_CAPACITY];
static volatile uint16_t ring_head = 0U;
static volatile uint16_t ring_tail = 0U;
static volatile int16_t latest_accel_x = 0;
static volatile int16_t latest_accel_y = 0;
static volatile int16_t latest_accel_z = 0;
static volatile uint8_t acquisition_enabled = 0U;
static uint8_t ads_has_started = 0U;

static FIL log_file;
static uint8_t log_open = 0U;
static uint32_t log_rows = 0U;
static uint32_t file_sequence = 1U;
static volatile uint8_t upload_in_progress = 0U;
static volatile uint8_t sd_upload_read_active = 0U;
static uint32_t last_imu_tick = 0U;
static uint32_t ads_last_poll_tick = 0U;

static volatile char modem_rx[512];
static volatile uint16_t modem_rx_len = 0U;
static volatile uint8_t modem_rx_byte = 0U;
static volatile uint8_t modem_rx_active = 0U;

/* Live Expressions */
volatile char system_status[128] = "Booting";
volatile uint32_t system_phase = 0U;
volatile uint32_t record_sessions_completed = 0U;
volatile uint32_t upload_phases_completed = 0U;
volatile uint32_t record_progress_timeouts = 0U;
volatile uint32_t record_last_progress_tick = 0U;
volatile uint32_t total_samples_acquired = 0U;
volatile uint32_t total_samples_logged = 0U;
volatile uint32_t sample_ring_overflows = 0U;
volatile uint32_t sd_write_errors = 0U;
volatile uint32_t sd_files_queued = 0U;
volatile uint32_t queue_reconcile_events = 0U;
volatile uint32_t sd_rotation_deferred = 0U;
volatile uint32_t sd_sync_result = 0U;
volatile uint32_t sd_sync_count = 0U;
volatile uint32_t sd_sync_failures = 0U;
volatile uint32_t sd_last_sync_tick = 0U;
volatile uint32_t sd_last_sync_duration_ms = 0U;
volatile uint32_t sd_close_result = 0U;
volatile uint32_t sd_rename_result = 0U;
volatile uint32_t sd_logger_write_fault = 0U;
volatile uint32_t sd_last_write_result = 0U;
volatile uint32_t sd_write_retry_tick = 0U;
volatile uint32_t sd_drain_failures = 0U;
volatile uint32_t sd_recovery_required = 0U;
volatile uint32_t sd_recovery_stage = 0U;
volatile uint32_t sd_record_stall_recoveries = 0U;
volatile uint32_t sd_active_abandoned = 0U;
volatile uint32_t logger_start_result = 0U;
volatile uint32_t logger_start_failures = 0U;
volatile uint32_t sd_remount_attempts = 0U;
volatile uint32_t sd_remount_result = 0U;
volatile uint32_t storage_prepare_result = 0U;
volatile uint32_t storage_prepare_count = 0U;
volatile uint32_t uploads_ok = 0U;
volatile uint32_t uploads_failed = 0U;
volatile uint32_t upload_stage = 0U;
volatile uint32_t upload_bytes_sent = 0U;
volatile uint32_t upload_uart_chunks = 0U;
volatile uint32_t upload_uart_last_chunk_size = 0U;
volatile uint32_t upload_uart_chunk_failures = 0U;
volatile uint32_t current_http_status = 0U;
volatile uint32_t sd_unlink_result = 0U;
volatile uint32_t upload_delete_failures = 0U;
volatile uint32_t last_http_status = 0U;
volatile uint32_t last_upload_attempts = 0U;
volatile uint32_t last_upload_file_size = 0U;
volatile uint32_t uart_error_flags = 0U;
volatile uint32_t uart_overruns = 0U;
volatile uint32_t ads_drdy_irq_count = 0U;
volatile uint32_t ads_spi_errors = 0U;
volatile uint32_t ads_id_value = 0U;
volatile uint32_t ads_config1_readback = 0U;
volatile uint32_t ads_stream_stage = 0U;
volatile uint32_t ads_drdy_pin_state = 1U;
volatile uint32_t ads_bootstrap_capture_count = 0U;
volatile uint32_t ads_capture_mode = 0U; /* 1=EXTI, 2=PB9 polling fallback */
volatile uint32_t ads_poll_capture_count = 0U;
volatile uint32_t ads_last_irq_tick = 0U;
volatile uint32_t ads_start_attempts = 0U;
volatile uint32_t ads_start_failures = 0U;
volatile uint32_t ads_start_hard_failures = 0U;
volatile uint32_t ads_recovery_cycles = 0U;
volatile uint32_t ads_last_failed_id = 0U;
volatile uint32_t ads_start_total_attempts = 0U;
volatile uint32_t ads_start_successes = 0U;
volatile uint32_t ads_start_id_failures = 0U;
volatile uint32_t ads_start_config_failures = 0U;
volatile uint32_t ads_start_drdy_failures = 0U;
volatile uint32_t modem_boot_stage = 0U;
volatile uint32_t modem_power_requested = 0U;
volatile uint32_t modem_power_state = 0U;
volatile uint32_t modem_power_stage = 0U;
volatile uint32_t modem_power_last_transition_tick = 0U;
volatile uint32_t modem_power_enables = 0U;
volatile uint32_t modem_power_disables = 0U;
volatile uint32_t modem_power_cycles = 0U;
volatile uint32_t network_time_valid = 0U;
volatile uint32_t network_time_syncs = 0U;
volatile uint32_t network_time_failures = 0U;
volatile uint32_t network_time_fallback_uploads = 0U;
volatile uint32_t network_time_source = 0U; /* 0=invalid, 1=CCLK, 2=server */
volatile uint32_t modem_clock_failures = 0U;
volatile uint32_t server_time_syncs = 0U;
volatile uint32_t server_time_failures = 0U;
volatile uint32_t server_time_round_trip_ms = 0U;
volatile uint32_t network_time_reference_tick = 0U;
volatile uint64_t network_time_epoch_ms = 0U;
volatile uint32_t current_upload_batch_index = 0U;
volatile uint32_t upload_batches_ok = 0U;
volatile uint32_t upload_batch_retries = 0U;
volatile uint32_t upload_batch_failures = 0U;
volatile char last_modem_response[512] = {0};
volatile char modem_boot_failure[64] = {0};
volatile char modem_power_last_on_reason[64] = {0};
volatile char modem_power_last_off_reason[64] = {0};
volatile char modem_boot_last_response[512] = {0};
volatile char upload_failure_step[64] = {0};
volatile char upload_failure_response[512] = {0};
volatile char record_abort_reason[64] = {0};
volatile char ads_recovery_reason[64] = {0};
volatile char current_log_filename[32] = {0};
volatile char current_upload_filename[32] = {0};
volatile char device_id[40] = {0};
volatile char network_time_last_response[96] = {0};
volatile char modem_clock_last_response[96] = {0};
volatile char server_time_last_response[160] = {0};
volatile char current_upload_file_id[32] = {0};

static void Background_Service(void);
static void ADS_CaptureFromISR(void);
static void ADS_Service(void);
static void LIS3DH_Service(void);
static void Logger_RecoverQueue(void);
static FRESULT Logger_SyncActiveFile(void);
static void Handle_ADSStartFailure(void);
static void Handle_SDRecordStall(void);
static void Run_SDRecoveryIfNeeded(void);

static uint8_t IsReadyFileName(const char *name)
{
    if ((name[0] != 'E') || (name[1] != 'C') || (name[2] != 'G')) return 0U;
    for (uint32_t i = 3U; i < 8U; i++)
    {
        if ((name[i] < '0') || (name[i] > '9')) return 0U;
    }
    return ((name[8] == '.') &&
            (name[9] == 'R') &&
            (name[10] == 'D') &&
            (name[11] == 'Y') &&
            (name[12] == '\0')) ? 1U : 0U;
}

static inline void ADS_CS(uint8_t active)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4,
                      active ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void ADS_Command(uint8_t command)
{
    ADS_CS(1U);
    (void)HAL_SPI_Transmit(&hspi1, &command, 1U, 20U);
    ADS_CS(0U);
    HAL_Delay(2U);
}

static void ADS_WriteReg(uint8_t reg, uint8_t value)
{
    uint8_t bytes[3] = {(uint8_t)(0x40U | (reg & 0x1FU)), 0U, value};
    ADS_CS(1U);
    (void)HAL_SPI_Transmit(&hspi1, bytes, sizeof(bytes), 20U);
    ADS_CS(0U);
    HAL_Delay(2U);
}

static uint8_t ADS_ReadReg(uint8_t reg)
{
    uint8_t command[2] = {(uint8_t)(0x20U | (reg & 0x1FU)), 0U};
    uint8_t tx = 0U, value = 0U;
    ADS_CS(1U);
    (void)HAL_SPI_Transmit(&hspi1, command, 2U, 20U);
    (void)HAL_SPI_TransmitReceive(&hspi1, &tx, &value, 1U, 20U);
    ADS_CS(0U);
    return value;
}

static HAL_StatusTypeDef ADS_ConfigureAndStartAttempt(void)
{
    ads_stream_stage = 1U;
    ads_capture_mode = 0U;
    acquisition_enabled = 0U;
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
    ADS_CS(0U);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_Delay(10U);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_Delay(2000U);

    /* Full hardware-reset sequence validated by b3feab2/f2a0433. This is
     * required when RESET/PWDN has remained low throughout a boot upload. */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_Delay(100U);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_Delay(100U);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_Delay(200U);

    ADS_Command(0x11U); /* SDATAC */
    HAL_Delay(200U);
    ads_id_value = 0U;
    for (uint32_t id_attempt = 1U; id_attempt <= ADS_ID_READ_MAX_ATTEMPTS; id_attempt++)
    {
        ads_id_value = ADS_ReadReg(0x00U);
        if ((ads_id_value & 0x7FU) == 0x73U) break;
        if (id_attempt < ADS_ID_READ_MAX_ATTEMPTS)
        {
            HAL_Delay(ADS_ID_READ_RETRY_MS);
            ADS_Command(0x11U); /* SDATAC */
            HAL_Delay(20U);
        }
    }
    if ((ads_id_value & 0x7FU) != 0x73U)
    {
        ads_start_id_failures++;
        ads_stream_stage = 255U;
        return HAL_ERROR;
    }
    ads_stream_stage = 5U;

    ADS_WriteReg(0x01U, ADS_CONFIG1_250_SPS); /* 250 SPS */
    ADS_WriteReg(0x02U, 0xE0U);
    ADS_WriteReg(0x03U, 0x10U);
    ADS_WriteReg(0x04U, 0x00U);
    ADS_WriteReg(0x05U, 0x00U);
    ADS_WriteReg(0x06U, 0x2CU);
    ADS_WriteReg(0x07U, 0x0FU);
    ADS_WriteReg(0x09U, 0xEAU);
    ADS_WriteReg(0x0AU, 0x03U);
    ads_config1_readback = ADS_ReadReg(0x01U);
    if (ads_config1_readback != ADS_CONFIG1_250_SPS)
    {
        ads_start_config_failures++;
        ads_stream_stage = 255U;
        return HAL_ERROR;
    }
    ads_stream_stage = 10U;

    /* Use the sequence proven on this board in b3feab2/f2a0433: start the
     * converter first, then enter continuous-read mode.  Sending START after
     * RDATAC left the device responsive to register reads but produced no
     * DRDY frames. */
    ADS_Command(0x08U); /* START */
    HAL_Delay(10U);
    ADS_Command(0x10U); /* RDATAC */
    HAL_Delay(10U);
    ads_stream_stage = 20U;

    /* PB9 may already be LOW by the time EXTI is armed. Drain that frame once
     * to release DRDY, then enable falling-edge interrupts for every following
     * conversion. */
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_9);
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, 5U, 0U);
    ads_drdy_pin_state = (uint32_t)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9);
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_RESET)
    {
        ADS_CaptureFromISR();
        ads_bootstrap_capture_count++;
    }
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_9);
    acquisition_enabled = 1U;
    ads_last_irq_tick = HAL_GetTick();
    ads_capture_mode = 1U;
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
    ads_has_started = 1U;

    uint16_t start_head = ring_head;
    uint32_t start_tick = HAL_GetTick();
    while (((uint16_t)(ring_head - start_head) == 0U) &&
           ((HAL_GetTick() - start_tick) < ADS_START_DRDY_TIMEOUT_MS))
    {
        ADS_Service();
        LIS3DH_Service();
        HAL_Delay(2U);
    }
    if ((uint16_t)(ring_head - start_head) == 0U)
    {
        ads_start_drdy_failures++;
        ads_stream_stage = 255U;
        acquisition_enabled = 0U;
        ads_capture_mode = 0U;
        HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
        return HAL_ERROR;
    }

    ads_stream_stage = 100U;
    return HAL_OK;
}

static HAL_StatusTypeDef ADS_StartAcquisition(void)
{
    for (uint32_t attempt = 1U; attempt <= ADS_START_MAX_ATTEMPTS; attempt++)
    {
        ads_start_attempts = attempt;
        ads_start_total_attempts++;
        HAL_Delay(ADS_RESTART_QUIET_MS);
        if (attempt > 1U)
        {
            (void)HAL_SPI_DeInit(&hspi1);
            HAL_Delay(20U);
            if (HAL_SPI_Init(&hspi1) != HAL_OK) continue;
        }

        if (ADS_ConfigureAndStartAttempt() == HAL_OK)
        {
            ads_start_successes++;
            return HAL_OK;
        }

        ads_start_failures++;
        acquisition_enabled = 0U;
        ads_has_started = 0U;
        HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
        ADS_CS(0U);
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET);
        HAL_Delay(500U);
    }
    return HAL_ERROR;
}

static void ADS_StopAcquisition(void)
{
    acquisition_enabled = 0U;
    ads_capture_mode = 0U;
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_9);
    if (!ads_has_started) return;
    ADS_Command(0x11U); /* SDATAC */
    ADS_Command(0x0AU); /* STOP */
    ads_has_started = 0U;
}

static void ADS_CaptureFromISR(void)
{
    uint8_t tx[9] = {0};
    uint8_t rx[9] = {0};
    ADS_CS(1U);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi1, tx, rx, 9U, 2U);
    ADS_CS(0U);
    if (status != HAL_OK)
    {
        ads_spi_errors++;
        return;
    }

    int32_t ch1 = ((int32_t)rx[3] << 16) | ((int32_t)rx[4] << 8) | rx[5];
    int32_t ch2 = ((int32_t)rx[6] << 16) | ((int32_t)rx[7] << 8) | rx[8];
    if ((ch1 & 0x00800000L) != 0) ch1 |= (int32_t)0xFF000000L;
    if ((ch2 & 0x00800000L) != 0) ch2 |= (int32_t)0xFF000000L;

    uint16_t head = ring_head;
    uint16_t next = (uint16_t)((head + 1U) & SAMPLE_RING_MASK);
    if (next == ring_tail)
    {
        sample_ring_overflows++;
        return;
    }

    sample_ring[head].tick_ms = HAL_GetTick();
    sample_ring[head].ecg_ch1 = ch1;
    sample_ring[head].ecg_ch2 = ch2;
    sample_ring[head].accel_x = latest_accel_x;
    sample_ring[head].accel_y = latest_accel_y;
    sample_ring[head].accel_z = latest_accel_z;
    __DMB();
    ring_head = next;
    total_samples_acquired++;
    if (total_samples_acquired >= 2U) ads_stream_stage = 100U;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_9)
    {
        ads_drdy_pin_state = (uint32_t)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9);
        ads_drdy_irq_count++;
        ads_last_irq_tick = HAL_GetTick();
        if (acquisition_enabled) ADS_CaptureFromISR();
    }
}

/* The validated sensor firmware polls PB9. Keep EXTI as the normal fast path,
 * but fall back to that proven behavior if no DRDY interrupt is observed for
 * several expected frames at 250 SPS. */
static void ADS_Service(void)
{
    if (!acquisition_enabled) return;

    uint32_t now = HAL_GetTick();
    ads_drdy_pin_state = (uint32_t)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9);

    if ((ads_capture_mode == 1U) && ((now - ads_last_irq_tick) >= ADS_DRDY_FALLBACK_MS))
    {
        HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_9);
        ads_capture_mode = 2U;
        ads_stream_stage = 30U;
        ads_last_poll_tick = now;
    }

    if ((ads_capture_mode == 2U) && ((now - ads_last_poll_tick) >= ADS_POLL_INTERVAL_MS) &&
        (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_RESET))
    {
        ads_last_poll_tick = now;
        ADS_CaptureFromISR();
        ads_poll_capture_count++;
    }
}

static HAL_StatusTypeDef LIS3DH_Init(void)
{
    uint8_t value = 0x47U;
    if (HAL_I2C_Mem_Write(&hi2c1, 0x18U << 1, 0x20U,
                          I2C_MEMADD_SIZE_8BIT, &value, 1U, 50U) != HAL_OK)
        return HAL_ERROR;
    value = 0x88U;
    return HAL_I2C_Mem_Write(&hi2c1, 0x18U << 1, 0x23U,
                             I2C_MEMADD_SIZE_8BIT, &value, 1U, 50U);
}

static void LIS3DH_Service(void)
{
    uint32_t now = HAL_GetTick();
    if ((now - last_imu_tick) < 20U) return;
    last_imu_tick = now;

    uint8_t data[6];
    if (HAL_I2C_Mem_Read(&hi2c1, 0x18U << 1, 0xA8U,
                         I2C_MEMADD_SIZE_8BIT, data, sizeof(data), 20U) == HAL_OK)
    {
        latest_accel_x = (int16_t)((data[1] << 8) | data[0]) >> 4;
        latest_accel_y = (int16_t)((data[3] << 8) | data[2]) >> 4;
        latest_accel_z = (int16_t)((data[5] << 8) | data[4]) >> 4;
    }
}

static void MakePath(char *out, size_t out_size, const char *name)
{
    (void)snprintf(out, out_size, "%s%s", SDPath, name);
}

static void ReadyName(char *out, size_t out_size, uint32_t sequence)
{
    (void)snprintf(out, out_size, "ECG%05lu.RDY", (unsigned long)sequence);
}

static FRESULT Logger_OpenActive(void)
{
    char path[32];
    MakePath(path, sizeof(path), ACTIVE_LOG_NAME);
    FRESULT result = f_open(&log_file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (result != FR_OK) return result;
    log_open = 1U;
    log_rows = 0U;
    snprintf((char *)current_log_filename, sizeof(current_log_filename), "%s", path);
    UINT written = 0U;
    result = f_write(&log_file, CSV_HEADER, strlen(CSV_HEADER), &written);
    if ((result != FR_OK) || (written != strlen(CSV_HEADER)))
    {
        sd_write_errors++;
        sd_logger_write_fault = 1U;
        sd_last_write_result = (uint32_t)result;
        sd_write_retry_tick = HAL_GetTick();
    }
    return result;
}

static FRESULT Logger_CloseAndQueue(uint8_t reopen)
{
    if (!log_open) return FR_INVALID_OBJECT;

    sd_sync_result = (uint32_t)Logger_SyncActiveFile();
    if (sd_sync_result != (uint32_t)FR_OK)
    {
        sd_write_errors++;
        sd_logger_write_fault = 1U;
        sd_last_write_result = sd_sync_result;
        sd_write_retry_tick = HAL_GetTick();
        snprintf((char *)system_status, sizeof(system_status),
                 "SD sync failed: %lu", (unsigned long)sd_sync_result);
        return (FRESULT)sd_sync_result;
    }

    sd_close_result = (uint32_t)f_close(&log_file);
    if (sd_close_result != (uint32_t)FR_OK)
    {
        sd_write_errors++;
        snprintf((char *)system_status, sizeof(system_status),
                 "SD close failed: %lu", (unsigned long)sd_close_result);
        return (FRESULT)sd_close_result;
    }
    log_open = 0U;

    char source[32], ready_name[16], destination[32];
    MakePath(source, sizeof(source), ACTIVE_LOG_NAME);
    ReadyName(ready_name, sizeof(ready_name), file_sequence);
    MakePath(destination, sizeof(destination), ready_name);
    sd_rename_result = (uint32_t)f_rename(source, destination);
    FRESULT result = (FRESULT)sd_rename_result;
    if (result != FR_OK)
    {
        sd_write_errors++;
        snprintf((char *)system_status, sizeof(system_status),
                 "SD rename failed: %u", (unsigned)result);
        /* Preserve the active data and retry rotation on a later pass. */
        result = f_open(&log_file, source, FA_OPEN_EXISTING | FA_WRITE);
        if (result == FR_OK)
        {
            result = f_lseek(&log_file, f_size(&log_file));
            if (result == FR_OK) log_open = 1U;
        }
        sd_rotation_deferred = 1U;
        return (FRESULT)sd_rename_result;
    }
    file_sequence++;
    sd_files_queued++;
    sd_rotation_deferred = 0U;
    current_log_filename[0] = '\0';
    if (reopen && (Logger_OpenActive() != FR_OK))
    {
        sd_write_errors++;
        Error_Handler();
    }
    return result;
}

static FRESULT Logger_StartRecording(void)
{
    if (log_open) return FR_OK;
    return Logger_OpenActive();
}

static FRESULT SD_RemountForLogger(void)
{
    sd_remount_attempts++;
    sd_recovery_stage = 20U;
    (void)f_mount(NULL, SDPath, 0U);
    sd_recovery_stage = 30U;
    (void)HAL_SD_DeInit(&hsd1);
    HAL_Delay(20U);

    sd_recovery_stage = 40U;
    if (HAL_SD_Init(&hsd1) != HAL_OK)
    {
        sd_remount_result = 1000U;
        sd_recovery_stage = 255U;
        return FR_DISK_ERR;
    }
    sd_recovery_stage = 50U;
    if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK)
    {
        sd_remount_result = 1001U;
        sd_recovery_stage = 255U;
        return FR_DISK_ERR;
    }

    sd_recovery_stage = 60U;
    FRESULT result = f_mount(&SDFatFS, SDPath, 1U);
    sd_remount_result = (uint32_t)result;
    if (result == FR_OK)
    {
        sd_recovery_stage = 70U;
        Logger_RecoverQueue();
        sd_recovery_stage = 100U;
    }
    else
    {
        sd_recovery_stage = 255U;
    }
    return result;
}

static FRESULT Logger_SyncActiveFile(void)
{
    uint32_t sync_start = HAL_GetTick();
    sd_sync_count++;
    sd_last_sync_tick = sync_start;
    FRESULT result = f_sync(&log_file);
    sd_last_sync_duration_ms = HAL_GetTick() - sync_start;
    sd_sync_result = (uint32_t)result;
    if (result != FR_OK) sd_sync_failures++;
    return result;
}

static FRESULT Logger_StartRecordingWithRecovery(void)
{
    FRESULT result = Logger_StartRecording();
    logger_start_result = (uint32_t)result;
    if (result == FR_OK) return result;

    logger_start_failures++;
    (void)SD_RemountForLogger();
    result = Logger_StartRecording();
    logger_start_result = (uint32_t)result;
    return result;
}

static FRESULT Logger_FinalizeRecording(void)
{
    if (!log_open) return FR_OK;

    if (log_rows == 0U)
    {
        char path[32];
        sd_sync_result = (uint32_t)Logger_SyncActiveFile();
        sd_close_result = (uint32_t)f_close(&log_file);
        log_open = 0U;
        current_log_filename[0] = '\0';
        MakePath(path, sizeof(path), ACTIVE_LOG_NAME);
        return f_unlink(path);
    }

    return Logger_CloseAndQueue(0U);
}

static void Logger_Drain(uint32_t maximum_rows)
{
    if (!log_open) return;
    if (sd_logger_write_fault && ((HAL_GetTick() - sd_write_retry_tick) < 100U))
        return;
    char line[96];
    uint32_t drained = 0U;
    while ((ring_tail != ring_head) && (drained < maximum_rows))
    {
        uint16_t tail = ring_tail;
        SampleRecord sample = sample_ring[tail];

        int length = snprintf(line, sizeof(line), "%lu,%d,%d,%d,%ld,%ld\r\n",
                              (unsigned long)sample.tick_ms,
                              sample.accel_x, sample.accel_y, sample.accel_z,
                              (long)sample.ecg_ch1, (long)sample.ecg_ch2);
        if ((length <= 0) || ((size_t)length >= sizeof(line)))
        {
            sd_write_errors++;
            Error_Handler();
        }
        UINT written = 0U;
        FRESULT result = f_write(&log_file, line, (UINT)length, &written);
        if ((result != FR_OK) || (written != (UINT)length))
        {
            if (!sd_logger_write_fault) sd_write_errors++;
            sd_logger_write_fault = 1U;
            sd_last_write_result = (uint32_t)result;
            sd_write_retry_tick = HAL_GetTick();
            break;
        }
        sd_logger_write_fault = 0U;
        sd_last_write_result = 0U;
        ring_tail = (uint16_t)((tail + 1U) & SAMPLE_RING_MASK);
        log_rows++;
        total_samples_logged++;
        drained++;
    }
}

static void Logger_ScanQueue(void)
{
    DIR directory;
    FILINFO info;
    uint32_t maximum_sequence = 0U;
    sd_files_queued = 0U;
    if (f_opendir(&directory, SDPath) != FR_OK) return;
    while ((f_readdir(&directory, &info) == FR_OK) && (info.fname[0] != '\0'))
    {
        unsigned long sequence = 0U;
        if (IsReadyFileName(info.fname) &&
            (sscanf(info.fname, "ECG%5lu.RDY", &sequence) == 1))
        {
            sd_files_queued++;
            if (sequence > maximum_sequence) maximum_sequence = (uint32_t)sequence;
        }
    }
    (void)f_closedir(&directory);
    file_sequence = maximum_sequence + 1U;
}

static void Logger_RecoverQueue(void)
{
    Logger_ScanQueue();
    char active_path[32];
    MakePath(active_path, sizeof(active_path), ACTIVE_LOG_NAME);
    FILINFO info;
    if (f_stat(active_path, &info) == FR_OK)
    {
        if (info.fsize <= strlen(CSV_HEADER))
        {
            (void)f_unlink(active_path);
        }
        else
        {
            char ready_name[16], ready_path[32];
            ReadyName(ready_name, sizeof(ready_name), file_sequence++);
            MakePath(ready_path, sizeof(ready_path), ready_name);
            if (f_rename(active_path, ready_path) == FR_OK)
            {
                sd_files_queued++;
            }
            else
            {
                sd_write_errors++;
                snprintf((char *)system_status, sizeof(system_status),
                         "SD active recovery deferred");
            }
        }
    }
}

static uint8_t FindOldestReady(char *path, size_t path_size)
{
    DIR directory;
    FILINFO info;
    char selected[32] = {0};
    if (f_opendir(&directory, SDPath) != FR_OK) return 0U;
    while (f_readdir(&directory, &info) == FR_OK && info.fname[0] != '\0')
    {
        if ((info.fattrib & AM_DIR) != 0U) continue;
        if (!IsReadyFileName(info.fname)) continue;
        if ((selected[0] == '\0') || (strcmp(info.fname, selected) < 0))
        {
            strncpy(selected, info.fname, sizeof(selected) - 1U);
            selected[sizeof(selected) - 1U] = '\0';
        }
    }
    (void)f_closedir(&directory);
    if (selected[0] == '\0') return 0U;
    MakePath(path, path_size, selected);
    return 1U;
}

static void Modem_StopRx(void)
{
    modem_rx_active = 0U;
    (void)HAL_UART_AbortReceive(&huart1);
}

static HAL_StatusTypeDef Modem_StartRx(void)
{
    Modem_StopRx();
    uint8_t byte;
    while (HAL_UART_Receive(&huart1, &byte, 1U, 1U) == HAL_OK) {}
    memset((char *)modem_rx, 0, sizeof(modem_rx));
    modem_rx_len = 0U;
    uart_error_flags = 0U;
    modem_rx_active = 1U;
    HAL_StatusTypeDef status = HAL_UART_Receive_IT(&huart1,
                                                   (uint8_t *)&modem_rx_byte, 1U);
    if (status != HAL_OK) modem_rx_active = 0U;
    return status;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if ((huart->Instance != USART1) || !modem_rx_active) return;
    if (modem_rx_len < (sizeof(modem_rx) - 1U))
    {
        modem_rx[modem_rx_len++] = (char)modem_rx_byte;
        modem_rx[modem_rx_len] = '\0';
    }
    if (HAL_UART_Receive_IT(&huart1, (uint8_t *)&modem_rx_byte, 1U) != HAL_OK)
        modem_rx_active = 0U;
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART1) return;
    uint32_t error = HAL_UART_GetError(huart);
    uart_error_flags |= error;
    if ((error & HAL_UART_ERROR_ORE) != 0U) uart_overruns++;
    __HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_OREF | UART_CLEAR_NEF |
                                UART_CLEAR_FEF | UART_CLEAR_PEF);
    if (modem_rx_active)
        (void)HAL_UART_Receive_IT(huart, (uint8_t *)&modem_rx_byte, 1U);
}

static uint8_t Modem_Wait(const char *token, uint32_t timeout_ms,
                          uint8_t require_complete_line)
{
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        const char *found = strstr((const char *)modem_rx, token);
        if (found != NULL)
        {
            if (!require_complete_line || (strchr(found, '\n') != NULL)) return 1U;
        }
        if (strstr((const char *)modem_rx, "ERROR") != NULL) return 0U;
        Background_Service();
        HAL_Delay(1U);
    }
    return 0U;
}

static uint8_t Modem_Command(const char *command, const char *expected,
                             uint32_t timeout_ms, uint8_t complete_line)
{
    if (Modem_StartRx() != HAL_OK) return 0U;
    if (HAL_UART_Transmit(&huart1, (uint8_t *)command,
                          (uint16_t)strlen(command), 2000U) != HAL_OK)
    {
        Modem_StopRx();
        return 0U;
    }
    uint8_t result = Modem_Wait(expected, timeout_ms, complete_line);
    Modem_StopRx();
    snprintf((char *)last_modem_response, sizeof(last_modem_response), "%s",
             (const char *)modem_rx);
    return result;
}

static void Modem_ServiceDelay(uint32_t delay_ms)
{
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < delay_ms)
    {
        Background_Service();
        HAL_Delay(1U);
    }
}

static void Modem_RecordBootFailure(const char *step)
{
    modem_boot_stage = 255U;
    snprintf((char *)modem_boot_failure, sizeof(modem_boot_failure), "%s", step);
    snprintf((char *)modem_boot_last_response,
             sizeof(modem_boot_last_response), "%s",
             (const char *)last_modem_response);
    snprintf((char *)system_status, sizeof(system_status),
             "Modem failed: %s", step);
}

static uint8_t Modem_TryAT(uint8_t attempts)
{
    for (uint8_t i = 0U; i < attempts; i++)
    {
        if (Modem_Command("AT\r\n", "OK", 1500U, 0U)) return 1U;
        Modem_ServiceDelay(500U);
    }
    return 0U;
}

static uint8_t Modem_IsRegistered(void)
{
    const char *response = (const char *)last_modem_response;
    return (strstr(response, ": 1,1") != NULL) ||
           (strstr(response, ": 1,5") != NULL) ||
           (strstr(response, ":1,1")  != NULL) ||
           (strstr(response, ":1,5")  != NULL);
}

static uint8_t Modem_Boot(void)
{
    modem_boot_failure[0] = '\0';
    modem_boot_last_response[0] = '\0';
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1 | GPIO_PIN_2, GPIO_PIN_RESET);
    Modem_ServiceDelay(200U);

    /* Never toggle PWRKEY blindly. A debugger reset often leaves the modem on. */
    modem_boot_stage = 1U;
    snprintf((char *)system_status, sizeof(system_status),
             "Modem: probing existing power");
    uint8_t responsive = Modem_TryAT(3U);

    if (!responsive)
    {
        modem_boot_stage = 2U;
        snprintf((char *)system_status, sizeof(system_status),
                 "Modem: PWRKEY power-on");
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_SET);
        Modem_ServiceDelay(600U);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_RESET);
        Modem_ServiceDelay(8000U);
        responsive = Modem_TryAT(10U);
    }

    if (!responsive)
    {
        modem_boot_stage = 3U;
        snprintf((char *)system_status, sizeof(system_status),
                 "Modem: hardware reset recovery");
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_SET);
        Modem_ServiceDelay(500U);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET);
        Modem_ServiceDelay(8000U);
        responsive = Modem_TryAT(10U);
    }

    if (!responsive)
    {
        modem_boot_stage = 4U;
        snprintf((char *)system_status, sizeof(system_status),
                 "Modem: forced off/on recovery");
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_SET);
        Modem_ServiceDelay(2500U);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_RESET);
        Modem_ServiceDelay(3000U);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_SET);
        Modem_ServiceDelay(600U);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_RESET);
        Modem_ServiceDelay(8000U);
        responsive = Modem_TryAT(10U);
    }

    if (!responsive)
    {
        Modem_RecordBootFailure("NO_AT_RESPONSE");
        return 0U;
    }

    modem_boot_stage = 10U;
    snprintf((char *)system_status, sizeof(system_status), "Modem: SIM check");
    (void)Modem_Command("ATE0\r\n", "OK", 2000U, 0U);
    if (!Modem_Command("AT+CPIN?\r\n", "READY", 5000U, 0U))
    {
        Modem_RecordBootFailure("SIM_NOT_READY");
        return 0U;
    }
    (void)Modem_Command("AT+CEREG=1\r\n", "OK", 2000U, 0U);

    modem_boot_stage = 20U;
    snprintf((char *)system_status, sizeof(system_status),
             "Modem: LTE registration");
    uint32_t registration_start = HAL_GetTick();
    while ((HAL_GetTick() - registration_start) < 90000U)
    {
        if (Modem_Command("AT+CEREG?\r\n", "OK", 3000U, 0U))
        {
            if (Modem_IsRegistered()) break;
        }
        snprintf((char *)system_status, sizeof(system_status),
                 "Modem: LTE registration %lus",
                 (unsigned long)((HAL_GetTick() - registration_start) / 1000U));
        Modem_ServiceDelay(2000U);
    }
    if ((HAL_GetTick() - registration_start) >= 90000U)
    {
        Modem_RecordBootFailure("LTE_REGISTRATION_TIMEOUT");
        return 0U;
    }

    modem_boot_stage = 30U;
    snprintf((char *)system_status, sizeof(system_status), "Modem: PDP setup");
    char command[96];
    snprintf(command, sizeof(command),
             "AT+CGDCONT=1,\"IP\",\"%s\"\r\n", MODEM_APN);
    (void)Modem_Command(command, "OK", 5000U, 0U);
    (void)Modem_Command("AT+CGATT=1\r\n", "OK", 15000U, 0U);
    (void)Modem_Command("AT+CGACT=1,1\r\n", "OK", 15000U, 0U);

    modem_boot_stage = 40U;
    snprintf((char *)system_status, sizeof(system_status), "Modem: checking IP");
    uint32_t ip_start = HAL_GetTick();
    uint8_t has_ip = 0U;
    while ((HAL_GetTick() - ip_start) < 30000U)
    {
        if (Modem_Command("AT+CGPADDR=1\r\n", "+CGPADDR:", 5000U, 0U) &&
            (strstr((const char *)last_modem_response, "0.0.0.0") == NULL) &&
            (strstr((const char *)last_modem_response, ".") != NULL))
        {
            has_ip = 1U;
            break;
        }
        Modem_ServiceDelay(2000U);
    }
    if (!has_ip)
    {
        Modem_RecordBootFailure("NO_PDP_IP_ADDRESS");
        return 0U;
    }

    modem_boot_stage = 100U;
    snprintf((char *)modem_boot_last_response,
             sizeof(modem_boot_last_response), "%s",
             (const char *)last_modem_response);
    snprintf((char *)system_status, sizeof(system_status),
             "Modem: PDP context ready");
    return 1U;
}

static uint8_t NetworkTime_IsLeapYear(uint32_t year)
{
    return ((year % 4U) == 0U && ((year % 100U) != 0U || (year % 400U) == 0U)) ? 1U : 0U;
}

static uint64_t NetworkTime_EpochMs(uint32_t year, uint32_t month, uint32_t day,
                                    uint32_t hour, uint32_t minute, uint32_t second)
{
    static const uint16_t days_before_month[] =
        {0U, 0U, 31U, 59U, 90U, 120U, 151U, 181U, 212U, 243U, 273U, 304U, 334U};
    uint64_t days = 0U;
    for (uint32_t y = 1970U; y < year; y++) days += NetworkTime_IsLeapYear(y) ? 366U : 365U;
    days += days_before_month[month] + (day - 1U);
    if ((month > 2U) && NetworkTime_IsLeapYear(year)) days++;
    return (((days * 24U + hour) * 60U + minute) * 60U + second) * 1000U;
}

static void DeviceId_Init(void)
{
    snprintf((char *)device_id, sizeof(device_id), "stm32l452-%08lX%08lX%08lX",
             (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(),
             (unsigned long)HAL_GetUIDw2());
}

static uint8_t ParseUint64Field(const char *text, const char *field,
                                uint64_t *value)
{
    const char *cursor = strstr(text, field);
    uint64_t parsed = 0U;
    uint8_t digits = 0U;
    if (cursor == NULL) return 0U;
    cursor += strlen(field);
    while ((*cursor == ' ') || (*cursor == '\t') || (*cursor == ':')) cursor++;
    while ((*cursor >= '0') && (*cursor <= '9'))
    {
        parsed = parsed * 10U + (uint64_t)(*cursor - '0');
        cursor++;
        digits++;
    }
    if (digits == 0U) return 0U;
    *value = parsed;
    return 1U;
}

static uint8_t ModemClock_Sync(void)
{
    int year, month, day, hour, minute, second, timezone_quarters;
    snprintf((char *)system_status, sizeof(system_status), "Modem: CCLK time sync");
    if (!Modem_Command("AT+CCLK?\r\n", "+CCLK:", 5000U, 1U)) return 0U;
    strncpy((char *)modem_clock_last_response,
            (const char *)last_modem_response,
            sizeof(modem_clock_last_response) - 1U);
    modem_clock_last_response[sizeof(modem_clock_last_response) - 1U] = '\0';
    strncpy((char *)network_time_last_response, (const char *)last_modem_response,
            sizeof(network_time_last_response) - 1U);
    network_time_last_response[sizeof(network_time_last_response) - 1U] = '\0';
    if (sscanf((const char *)last_modem_response,
               "%*[^\"]\"%2d/%2d/%2d,%2d:%2d:%2d%3d\"",
               &year, &month, &day, &hour, &minute, &second, &timezone_quarters) != 7)
        return 0U;
    if ((year < 24) || (year > 40) ||
        (month < 1) || (month > 12) || (day < 1) || (day > 31) ||
        (hour > 23) || (minute > 59) || (second > 59)) return 0U;

    network_time_epoch_ms = (uint64_t)((int64_t)NetworkTime_EpochMs((uint32_t)(2000 + year),
                                                                      (uint32_t)month, (uint32_t)day,
                                                                      (uint32_t)hour, (uint32_t)minute,
                                                                      (uint32_t)second) -
                                       (int64_t)timezone_quarters * 15LL * 60LL * 1000LL);
    network_time_reference_tick = HAL_GetTick();
    network_time_valid = 1U;
    network_time_source = 1U;
    network_time_syncs++;
    return 1U;
}

static uint8_t ServerTime_Sync(void)
{
    char command[200];
    uint8_t success = 0U;
    uint32_t request_start_tick = 0U;
    uint32_t request_end_tick = 0U;
    uint64_t epoch_ms = 0U;

    snprintf((char *)system_status, sizeof(system_status), "Modem: server time sync");
    (void)Modem_Command("AT+HTTPTERM\r\n", "OK", 3000U, 0U);
    if (!Modem_Command("AT+HTTPINIT\r\n", "OK", 5000U, 0U)) goto done;
    snprintf(command, sizeof(command), "AT+HTTPPARA=\"URL\",\"%s\"\r\n",
             SERVER_TIME_URL);
    if (!Modem_Command(command, "OK", 5000U, 0U)) goto terminate;

    request_start_tick = HAL_GetTick();
    if (!Modem_Command("AT+HTTPACTION=0\r\n", "+HTTPACTION:", 65000U, 1U))
        goto terminate;
    {
        const char *action = strstr((const char *)last_modem_response,
                                    "+HTTPACTION:");
        unsigned long status = 0U, response_length = 0U;
        if ((action == NULL) ||
            (sscanf(action, "+HTTPACTION: %*u,%lu,%lu",
                    &status, &response_length) != 2) ||
            (status != 200U) || (response_length == 0U))
            goto terminate;
    }
    if (!Modem_Command("AT+HTTPREAD=0,128\r\n", "OK", 5000U, 1U))
        goto terminate;
    request_end_tick = HAL_GetTick();
    strncpy((char *)server_time_last_response, (const char *)last_modem_response,
            sizeof(server_time_last_response) - 1U);
    server_time_last_response[sizeof(server_time_last_response) - 1U] = '\0';
    if (!ParseUint64Field((const char *)last_modem_response,
                          "\"epoch_ms\"", &epoch_ms))
        goto terminate;

    server_time_round_trip_ms = request_end_tick - request_start_tick;
    network_time_epoch_ms = epoch_ms;
    network_time_reference_tick =
        request_start_tick + (server_time_round_trip_ms / 2U);
    network_time_valid = 1U;
    network_time_source = 2U;
    network_time_syncs++;
    server_time_syncs++;
    strncpy((char *)network_time_last_response,
            (const char *)server_time_last_response,
            sizeof(network_time_last_response) - 1U);
    network_time_last_response[sizeof(network_time_last_response) - 1U] = '\0';
    success = 1U;

terminate:
    (void)Modem_Command("AT+HTTPTERM\r\n", "OK", 3000U, 0U);
done:
    if (!success)
    {
        server_time_failures++;
        strncpy((char *)server_time_last_response,
                (const char *)last_modem_response,
                sizeof(server_time_last_response) - 1U);
        server_time_last_response[sizeof(server_time_last_response) - 1U] = '\0';
    }
    return success;
}

static uint8_t NetworkTime_Sync(void)
{
    if (ModemClock_Sync()) return 1U;
    modem_clock_failures++;
    if (ServerTime_Sync()) return 1U;

    network_time_valid = 0U;
    network_time_source = 0U;
    network_time_failures++;
    return 0U;
}

static uint64_t NetworkTime_TimestampForTick(uint32_t sample_tick)
{
    int32_t elapsed_ms = (int32_t)(sample_tick - network_time_reference_tick);
    return network_time_valid ? (uint64_t)((int64_t)network_time_epoch_ms + elapsed_ms) : 0U;
}

static uint8_t UInt64_ToDecimal(uint64_t value, char *buffer, size_t capacity)
{
    char reversed[20];
    size_t digits = 0U;

    do
    {
        reversed[digits++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while ((value != 0U) && (digits < sizeof(reversed)));

    if (capacity <= digits) return 0U;
    for (size_t i = 0U; i < digits; i++)
        buffer[i] = reversed[digits - 1U - i];
    buffer[digits] = '\0';
    return 1U;
}

static void ModemPower_Enable(const char *reason)
{
    modem_power_requested = 1U;
    if (modem_power_state != 0U) return;

    modem_power_stage = 10U;
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_Delay(MODEM_RAIL_SETTLE_MS);
    modem_power_state = 1U;
    modem_power_enables++;
    modem_power_last_transition_tick = HAL_GetTick();
    snprintf((char *)modem_power_last_on_reason,
             sizeof(modem_power_last_on_reason), "%s", reason);
}

static void ModemPower_Disable(const char *reason)
{
    modem_power_requested = 0U;
    Modem_StopRx();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET);
    HAL_Delay(MODEM_RAIL_SETTLE_MS);
    if (modem_power_state != 0U) modem_power_disables++;
    modem_power_state = 0U;
    modem_power_stage = 0U;
    modem_power_last_transition_tick = HAL_GetTick();
    snprintf((char *)modem_power_last_off_reason,
             sizeof(modem_power_last_off_reason), "%s", reason);
}

static uint8_t ModemPower_BootForUpload(void)
{
    modem_power_cycles++;
    ModemPower_Enable("upload queue");
    modem_power_stage = 20U;
    if (!Modem_Boot())
    {
        ModemPower_Disable("modem boot failed");
        return 0U;
    }
    if (!NetworkTime_Sync()) network_time_fallback_uploads++;
    modem_power_stage = 30U;
    return 1U;
}

static void Upload_CaptureFailure(const char *step)
{
    snprintf((char *)upload_failure_step, sizeof(upload_failure_step), "%s", step);
    const char *source = (modem_rx_len > 0U)
                       ? (const char *)modem_rx
                       : (const char *)last_modem_response;
    snprintf((char *)upload_failure_response,
             sizeof(upload_failure_response), "%s", source);
}

static uint8_t HTTP_PostJsonBatch(const char *payload, uint32_t payload_size)
{
    char command[200];
    uint8_t success = 0U;
    current_http_status = 0U;
    upload_bytes_sent = 0U;
    upload_uart_chunks = 0U;
    upload_uart_last_chunk_size = 0U;
    upload_stage = 10U;
    upload_failure_step[0] = '\0';
    upload_failure_response[0] = '\0';

    (void)Modem_Command("AT+HTTPTERM\r\n", "OK", 3000U, 0U);
    if (!Modem_Command("AT+HTTPINIT\r\n", "OK", 5000U, 0U))
    {
        Upload_CaptureFailure("HTTPINIT");
        goto done;
    }
    snprintf(command, sizeof(command), "AT+HTTPPARA=\"URL\",\"%s\"\r\n", UPLOAD_URL);
    if (!Modem_Command(command, "OK", 5000U, 0U))
    {
        Upload_CaptureFailure("URL");
        goto terminate;
    }
    if (!Modem_Command("AT+HTTPPARA=\"CONTENT\",\"application/json\"\r\n",
                       "OK", 3000U, 0U))
    {
        Upload_CaptureFailure("CONTENT");
        goto terminate;
    }
    snprintf(command, sizeof(command), "AT+HTTPDATA=%lu,60000\r\n",
             (unsigned long)payload_size);
    if (!Modem_Command(command, "DOWNLOAD", 5000U, 0U))
    {
        Upload_CaptureFailure("HTTPDATA_PROMPT");
        goto terminate;
    }

    if (Modem_StartRx() != HAL_OK)
    {
        Upload_CaptureFailure("DATA_RX_ARM");
        goto terminate;
    }
    upload_stage = 30U;
    while (upload_bytes_sent < payload_size)
    {
        uint32_t chunk_size = payload_size - upload_bytes_sent;
        if (chunk_size > MODEM_HTTP_TX_CHUNK_SIZE)
            chunk_size = MODEM_HTTP_TX_CHUNK_SIZE;
        upload_uart_last_chunk_size = chunk_size;
        if (HAL_UART_Transmit(&huart1,
                              (uint8_t *)(payload + upload_bytes_sent),
                              (uint16_t)chunk_size, 5000U) != HAL_OK)
        {
            upload_uart_chunk_failures++;
            Upload_CaptureFailure("DATA_TX");
            Modem_StopRx();
            goto terminate;
        }
        upload_bytes_sent += chunk_size;
        upload_uart_chunks++;
        Background_Service();
        if (upload_bytes_sent < payload_size)
            HAL_Delay(MODEM_HTTP_TX_PACING_MS);
    }
    upload_stage = 40U;
    if (!Modem_Wait("OK", 65000U, 0U))
    {
        Upload_CaptureFailure("DATA_ACK");
        Modem_StopRx();
        goto terminate;
    }
    Modem_StopRx();
    upload_stage = 50U;

    if (!Modem_Command("AT+HTTPACTION=1\r\n", "+HTTPACTION:", 65000U, 1U))
    {
        Upload_CaptureFailure("HTTPACTION_WAIT");
        goto terminate;
    }
    {
        const char *action = strstr((const char *)last_modem_response, "+HTTPACTION:");
        unsigned long status = 0U, response_length = 0U;
        if ((action != NULL) &&
            (sscanf(action, "+HTTPACTION: %*u,%lu,%lu", &status, &response_length) == 2))
        {
            current_http_status = (uint32_t)status;
            last_http_status = current_http_status;
            success = (status == 200U) ? 1U : 0U;
            if (success) upload_stage = 70U;
            else Upload_CaptureFailure("HTTP_STATUS");
        }
        else Upload_CaptureFailure("HTTPACTION_PARSE");
    }

terminate:
    (void)Modem_Command("AT+HTTPTERM\r\n", "OK", 3000U, 0U);
done:
    if (!success) upload_stage = 255U;
    return success;
}

static uint8_t HTTP_PostJsonBatchWithRetry(const char *payload,
                                           uint32_t payload_size,
                                           uint32_t batch_index)
{
    current_upload_batch_index = batch_index;
    for (uint32_t attempt = 1U; attempt <= HTTP_MAX_ATTEMPTS; attempt++)
    {
        if (attempt > 1U) upload_batch_retries++;
        if (HTTP_PostJsonBatch(payload, payload_size))
        {
            upload_batches_ok++;
            return 1U;
        }
        if (attempt < HTTP_MAX_ATTEMPTS)
        {
            uint32_t retry_start = HAL_GetTick();
            while ((HAL_GetTick() - retry_start) < HTTP_RETRY_BACKOFF_MS)
            {
                Background_Service();
                HAL_Delay(1U);
            }
        }
    }
    upload_batch_failures++;
    return 0U;
}

static uint8_t File_ComputeId(FIL *file, char *file_id, size_t capacity)
{
    uint8_t data[256];
    UINT bytes_read = 0U;
    uint32_t hash = 2166136261U;
    FSIZE_t original_position = f_tell(file);
    FSIZE_t file_size = f_size(file);

    if (f_lseek(file, 0U) != FR_OK) return 0U;
    do
    {
        if (f_read(file, data, sizeof(data), &bytes_read) != FR_OK)
        {
            (void)f_lseek(file, original_position);
            return 0U;
        }
        for (UINT i = 0U; i < bytes_read; i++)
        {
            hash ^= data[i];
            hash *= 16777619U;
        }
    } while (bytes_read > 0U);
    if (f_lseek(file, original_position) != FR_OK) return 0U;

    int length = snprintf(file_id, capacity, "%08lX-%lu",
                          (unsigned long)hash, (unsigned long)file_size);
    return ((length > 0) && ((size_t)length < capacity)) ? 1U : 0U;
}

static uint8_t HTTP_PostReadyFileJson(const char *path)
{
    FIL upload;
    char line[96];
    char file_id[32];
    static char json_batch[JSON_BATCH_CAPACITY];
    uint32_t batch_length, batch_rows = 0U, batch_index = 0U, total_rows = 0U;
    uint8_t success = 0U;

    sd_upload_read_active = 1U;
    if (f_open(&upload, path, FA_READ) != FR_OK)
    {
        sd_upload_read_active = 0U;
        Upload_CaptureFailure("FILE_OPEN");
        return 0U;
    }
    if (!File_ComputeId(&upload, file_id, sizeof(file_id)))
    {
        Upload_CaptureFailure("FILE_ID");
        goto done;
    }
    snprintf((char *)current_upload_file_id,
             sizeof(current_upload_file_id), "%s", file_id);
    if (f_gets(line, sizeof(line), &upload) == NULL)
    {
        Upload_CaptureFailure("CSV_HEADER_READ");
        goto done;
    }

    {
        int header_length = snprintf(
            json_batch, sizeof(json_batch),
            "{\"device_id\":\"%s\",\"file_id\":\"%s\",\"batch_index\":%lu,\"readings\":[",
            (const char *)device_id, file_id, (unsigned long)batch_index);
        if ((header_length <= 0) || ((size_t)header_length >= sizeof(json_batch)))
        {
            Upload_CaptureFailure("JSON_HEADER");
            goto done;
        }
        batch_length = (uint32_t)header_length;
    }
    while (f_gets(line, sizeof(line), &upload) != NULL)
    {
        unsigned long tick;
        int accel_x, accel_y, accel_z;
        long ecg_ch1, ecg_ch2;
        char row[192];
        char timestamp[21];
        if (sscanf(line, "%lu,%d,%d,%d,%ld,%ld", &tick, &accel_x, &accel_y, &accel_z,
                   &ecg_ch1, &ecg_ch2) != 6)
        {
            Upload_CaptureFailure("CSV_ROW_PARSE");
            goto done;
        }
        if (!UInt64_ToDecimal(NetworkTime_TimestampForTick((uint32_t)tick),
                              timestamp, sizeof(timestamp)))
        {
            Upload_CaptureFailure("TIMESTAMP_FORMAT");
            goto done;
        }
        int row_length = snprintf(row, sizeof(row),
                                  "{\"timestamp\":%s,\"accel_x\":%d,\"accel_y\":%d,\"accel_z\":%d,\"ecg_ch1\":%ld,\"ecg_ch2\":%ld}",
                                  timestamp,
                                  accel_x, accel_y, accel_z, ecg_ch1, ecg_ch2);
        if ((row_length <= 0) || ((size_t)row_length >= sizeof(row)))
        {
            Upload_CaptureFailure("JSON_ROW_FORMAT");
            goto done;
        }
        if ((batch_length + (batch_rows ? 1U : 0U) +
             (uint32_t)row_length + 2U) >= JSON_BATCH_CAPACITY)
        {
            Upload_CaptureFailure("JSON_BATCH_OVERFLOW");
            goto done;
        }
        if (batch_rows > 0U) json_batch[batch_length++] = ',';
        memcpy(json_batch + batch_length, row, (size_t)row_length);
        batch_length += (uint32_t)row_length;
        batch_rows++;
        total_rows++;

        if (batch_rows >= JSON_READINGS_PER_BATCH)
        {
            json_batch[batch_length++] = ']';
            json_batch[batch_length++] = '}';
            json_batch[batch_length] = '\0';
            if (!HTTP_PostJsonBatchWithRetry(json_batch, batch_length,
                                             batch_index))
                goto done;
            batch_index++;
            batch_rows = 0U;
            int header_length = snprintf(
                json_batch, sizeof(json_batch),
                "{\"device_id\":\"%s\",\"file_id\":\"%s\",\"batch_index\":%lu,\"readings\":[",
                (const char *)device_id, file_id, (unsigned long)batch_index);
            if ((header_length <= 0) ||
                ((size_t)header_length >= sizeof(json_batch)))
            {
                Upload_CaptureFailure("JSON_HEADER");
                goto done;
            }
            batch_length = (uint32_t)header_length;
        }
    }
    if (total_rows == 0U)
    {
        Upload_CaptureFailure("CSV_NO_ROWS");
        goto done;
    }
    if (batch_rows > 0U)
    {
        json_batch[batch_length++] = ']';
        json_batch[batch_length++] = '}';
        json_batch[batch_length] = '\0';
        if (!HTTP_PostJsonBatchWithRetry(json_batch, batch_length,
                                         batch_index))
            goto done;
    }
    success = 1U;

done:
    (void)f_close(&upload);
    sd_upload_read_active = 0U;
    return success;
}

static void Upload_OldestReady(void)
{
    char path[32];
    if (!FindOldestReady(path, sizeof(path)))
    {
        queue_reconcile_events++;
        Upload_CaptureFailure("NO_READY_FILE");
        snprintf((char *)system_status, sizeof(system_status),
                 "Upload queue mismatch: rescanning");
        Logger_ScanQueue();
        return;
    }
    FILINFO info;
    if (f_stat(path, &info) != FR_OK)
    {
        queue_reconcile_events++;
        snprintf((char *)current_upload_filename, sizeof(current_upload_filename), "%s", path);
        Upload_CaptureFailure("READY_STAT");
        snprintf((char *)system_status, sizeof(system_status),
                 "Ready file stat failed: %s", path);
        current_upload_filename[0] = '\0';
        Logger_ScanQueue();
        return;
    }
    last_upload_file_size = (uint32_t)info.fsize;
    if (info.fsize <= strlen(CSV_HEADER))
    {
        if (f_unlink(path) == FR_OK && sd_files_queued > 0U) sd_files_queued--;
        snprintf((char *)system_status, sizeof(system_status),
                 "Discarded empty batch %s", path);
        return;
    }
    snprintf((char *)current_upload_filename, sizeof(current_upload_filename), "%s", path);
    upload_in_progress = 1U;
    uint8_t uploaded = 0U;

    for (uint32_t attempt = 1U; attempt <= HTTP_MAX_ATTEMPTS; attempt++)
    {
        last_upload_attempts = attempt;
        snprintf((char *)system_status, sizeof(system_status),
                 "Uploading %s attempt %lu", path, (unsigned long)attempt);
        if (HTTP_PostReadyFileJson(path))
        {
            upload_stage = 90U;
            uploads_ok++;
            sd_unlink_result = (uint32_t)f_unlink(path);
            if (sd_unlink_result == (uint32_t)FR_OK)
            {
                if (sd_files_queued > 0U) sd_files_queued--;
            }
            else upload_delete_failures++;
            uploaded = 1U;
            break;
        }
        if (attempt < HTTP_MAX_ATTEMPTS)
        {
            uint32_t start = HAL_GetTick();
            while ((HAL_GetTick() - start) < HTTP_RETRY_BACKOFF_MS)
            {
                Background_Service();
                HAL_Delay(1U);
            }
        }
    }
    if (!uploaded) uploads_failed++;
    upload_in_progress = 0U;
    current_upload_filename[0] = '\0';

    if (uploaded) upload_stage = 100U;
}

static void Background_Service(void)
{
    ADS_Service();
    LIS3DH_Service();
    if (!sd_upload_read_active) Logger_Drain(32U);
}

static void Handle_ADSStartFailure(void)
{
    ads_start_hard_failures++;
    ads_recovery_cycles++;
    ads_last_failed_id = ads_id_value;
    snprintf((char *)ads_recovery_reason, sizeof(ads_recovery_reason),
             "ADS start failed ID=%lu", (unsigned long)ads_last_failed_id);
    snprintf((char *)record_abort_reason, sizeof(record_abort_reason),
             "ADS start failed");

    ADS_StopAcquisition();
    acquisition_enabled = 0U;
    ads_has_started = 0U;
    ads_capture_mode = 0U;
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_9);

    if (log_open)
    {
        (void)f_close(&log_file);
        log_open = 0U;
        current_log_filename[0] = '\0';
        (void)f_unlink(ACTIVE_LOG_NAME);
    }

    ADS_CS(0U);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_Delay(500U);
    (void)HAL_SPI_DeInit(&hspi1);
    HAL_Delay(20U);
    (void)HAL_SPI_Init(&hspi1);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);

    system_phase = 60U;
    snprintf((char *)system_status, sizeof(system_status),
             "ADS recovery: ID=%lu", (unsigned long)ads_last_failed_id);

    uint32_t recovery_start = HAL_GetTick();
    while ((HAL_GetTick() - recovery_start) < ADS_START_RECOVERY_MS)
    {
        LIS3DH_Service();
        HAL_Delay(10U);
    }
}

static void Handle_SDRecordStall(void)
{
    record_progress_timeouts++;
    sd_record_stall_recoveries++;
    sd_recovery_required = 1U;
    sd_recovery_stage = 10U;
    system_phase = 60U;
    snprintf((char *)record_abort_reason, sizeof(record_abort_reason),
             "Recording stalled");
    snprintf((char *)system_status, sizeof(system_status),
             "SD record stall; recovery queued");

    ADS_StopAcquisition();
    acquisition_enabled = 0U;
    ads_capture_mode = 0U;
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_9);

    if (log_open)
    {
        log_open = 0U;
        current_log_filename[0] = '\0';
        sd_active_abandoned++;
    }

    ring_tail = ring_head;
    sd_logger_write_fault = 0U;
    sd_write_retry_tick = HAL_GetTick();
}

static void Run_SDRecoveryIfNeeded(void)
{
    if (!sd_recovery_required) return;

    system_phase = 60U;
    snprintf((char *)system_status, sizeof(system_status), "SD recovery remount");

    FRESULT result = SD_RemountForLogger();
    if (result == FR_OK)
    {
        char active_path[32];
        MakePath(active_path, sizeof(active_path), ACTIVE_LOG_NAME);
        (void)f_unlink(active_path);
        sd_logger_write_fault = 0U;
        sd_last_write_result = 0U;
        sd_recovery_required = 0U;
        snprintf((char *)system_status, sizeof(system_status), "SD recovery complete");
    }
    else
    {
        sd_last_write_result = (uint32_t)result;
        sd_logger_write_fault = 1U;
        sd_write_retry_tick = HAL_GetTick();
        snprintf((char *)system_status, sizeof(system_status),
                 "SD recovery failed: %lu", (unsigned long)sd_last_write_result);
    }
}

static void Run_RecordPhase(void)
{
    system_phase = 20U;
    snprintf((char *)system_status, sizeof(system_status),
             "Recording session %lu", (unsigned long)(record_sessions_completed + 1U));

    while (Logger_StartRecordingWithRecovery() != FR_OK)
    {
        system_phase = 60U;
        snprintf((char *)system_status, sizeof(system_status),
                 "SD log start retry: %lu",
                 (unsigned long)logger_start_result);
        uint32_t retry_start = HAL_GetTick();
        while ((HAL_GetTick() - retry_start) < SD_RECORD_START_RETRY_MS)
        {
            LIS3DH_Service();
            HAL_Delay(10U);
        }
    }
    system_phase = 20U;
    snprintf((char *)system_status, sizeof(system_status),
             "Recording session %lu", (unsigned long)(record_sessions_completed + 1U));

    if (ADS_StartAcquisition() != HAL_OK)
    {
        Handle_ADSStartFailure();
        return;
    }

    uint32_t record_start = HAL_GetTick();
    uint32_t session_start_logged = total_samples_logged;
    uint32_t session_last_logged = total_samples_logged;
    record_last_progress_tick = HAL_GetTick();
    record_abort_reason[0] = '\0';
    while (((total_samples_logged - session_start_logged) < SAMPLES_PER_FILE) &&
           ((HAL_GetTick() - record_start) < RECORD_SESSION_MAX_MS))
    {
        ADS_Service();
        LIS3DH_Service();
        Logger_Drain(64U);
        uint32_t session_logged_now = total_samples_logged;
        if (session_logged_now != session_last_logged)
        {
            session_last_logged = session_logged_now;
            record_last_progress_tick = HAL_GetTick();
        }
        else if ((HAL_GetTick() - record_last_progress_tick) >= RECORD_PROGRESS_TIMEOUT_MS)
        {
            Handle_SDRecordStall();
            return;
        }
    }

    system_phase = 30U;
    snprintf((char *)system_status, sizeof(system_status), "Finalizing recording");
    ADS_StopAcquisition();

    uint32_t drain_start = HAL_GetTick();
    while (ring_tail != ring_head)
    {
        uint16_t before = ring_tail;
        Logger_Drain(128U);
        if ((ring_tail == before) && ((HAL_GetTick() - drain_start) >= 5000U))
        {
            sd_drain_failures++;
            snprintf((char *)system_status, sizeof(system_status),
                     "SD drain failed: %lu", (unsigned long)sd_last_write_result);
            ring_tail = ring_head;
            break;
        }
    }

    if (Logger_FinalizeRecording() != FR_OK)
    {
        snprintf((char *)system_status, sizeof(system_status),
                 "SD finalize deferred: %lu", (unsigned long)sd_last_write_result);
        (void)f_close(&log_file);
        log_open = 0U;
        current_log_filename[0] = '\0';
        Logger_RecoverQueue();
    }
    record_sessions_completed++;
}

static void Run_UploadPhase(void)
{
    ADS_StopAcquisition();
    if (log_open && (Logger_FinalizeRecording() != FR_OK))
    {
        snprintf((char *)system_status, sizeof(system_status),
                 "SD finalize deferred: %lu", (unsigned long)sd_last_write_result);
        (void)f_close(&log_file);
        log_open = 0U;
        current_log_filename[0] = '\0';
    }

    Logger_RecoverQueue();
    if (sd_files_queued == 0U)
    {
        modem_power_stage = 0U;
        return;
    }
    if (!ModemPower_BootForUpload())
    {
        system_phase = 50U;
        return;
    }
    system_phase = 40U;
    snprintf((char *)system_status, sizeof(system_status),
             "Uploading %lu queued files", (unsigned long)sd_files_queued);

    while (sd_files_queued > 0U)
    {
        uint32_t queued_before = sd_files_queued;
        Upload_OldestReady();
        if (sd_files_queued >= queued_before)
        {
            system_phase = 50U;
            snprintf((char *)system_status, sizeof(system_status),
                     "Upload deferred; %lu files queued",
                     (unsigned long)sd_files_queued);
            ModemPower_Disable("upload deferred");
            return;
        }
    }

    upload_phases_completed++;
    system_phase = 100U;
    snprintf((char *)system_status, sizeof(system_status), "Upload queue drained");
    ModemPower_Disable("upload complete");
}

static void Drain_UploadQueueBeforeNextRecord(void)
{
    for (;;)
    {
        Logger_RecoverQueue();
        if (sd_files_queued == 0U)
        {
            return;
        }

        Run_UploadPhase();
        Logger_RecoverQueue();
        if (sd_files_queued == 0U)
        {
            return;
        }

        system_phase = 50U;
        snprintf((char *)system_status, sizeof(system_status),
                 "Upload retry idle; %lu files queued",
                 (unsigned long)sd_files_queued);

        uint32_t start = HAL_GetTick();
        ModemPower_Disable("upload retry");
        while ((HAL_GetTick() - start) < UPLOAD_RETRY_IDLE_MS)
        {
            LIS3DH_Service();
            HAL_Delay(10U);
        }
    }
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_I2C1_Init();
    MX_SDMMC1_SD_Init();
    MX_SPI1_Init();
    MX_USART1_UART_Init();
    MX_FATFS_Init();
    DeviceId_Init();

    system_phase = 10U;
    snprintf((char *)system_status, sizeof(system_status), "Recovering SD queue");
    if (f_mount(&SDFatFS, SDPath, 1U) != FR_OK) Error_Handler();
    Logger_RecoverQueue();
    if (LIS3DH_Init() != HAL_OK) Error_Handler();

    Drain_UploadQueueBeforeNextRecord();

    while (1)
    {
        Run_SDRecoveryIfNeeded();
        Drain_UploadQueueBeforeNextRecord();
        Run_RecordPhase();
        Run_SDRecoveryIfNeeded();
        Run_UploadPhase();
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
        Error_Handler();
    osc.OscillatorType = RCC_OSCILLATORTYPE_MSI;
    osc.MSIState = RCC_MSI_ON;
    osc.MSICalibrationValue = 0;
    osc.MSIClockRange = RCC_MSIRANGE_6;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_MSI;
    osc.PLL.PLLM = 1;
    osc.PLL.PLLN = 40;
    osc.PLL.PLLP = RCC_PLLP_DIV7;
    osc.PLL.PLLQ = RCC_PLLQ_DIV2;
    osc.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();
    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) Error_Handler();
}

static void MX_I2C1_Init(void)
{
    hi2c1.Instance = I2C1;
    hi2c1.Init.Timing = 0x00D09BE3;
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0;
    hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) Error_Handler();
    if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
        Error_Handler();
}

static void MX_SDMMC1_SD_Init(void)
{
    hsd1.Instance = SDMMC1;
    hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
    hsd1.Init.ClockBypass = SDMMC_CLOCK_BYPASS_DISABLE;
    hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd1.Init.BusWide = SDMMC_BUS_WIDE_4B;
    hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd1.Init.ClockDiv = 7;
    if (HAL_SD_Init(&hsd1) != HAL_OK) Error_Handler();
    if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK)
        Error_Handler();
}

static void MX_SPI1_Init(void)
{
    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_2EDGE;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 7;
    hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
    hspi1.Init.NSSPMode = SPI_NSS_PULSE_ENABLE;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) Error_Handler();
}

static void MX_USART1_UART_Init(void)
{
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4 | GPIO_PIN_13, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_8, GPIO_PIN_RESET);

    gpio.Pin = GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_4 | GPIO_PIN_13;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_8;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_IT_FALLING;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &gpio);
}

void Error_Handler(void)
{
    acquisition_enabled = 0U;
    __disable_irq();
    while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
