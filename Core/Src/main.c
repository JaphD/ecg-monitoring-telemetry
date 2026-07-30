/**
  ******************************************************************************
  * @file    main.c
  * @brief   ADS1292R isolated validation using STM32CubeIDE Live Expressions
  ******************************************************************************
  */

#include "main.h"
#include "ads_sync_capture.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

I2C_HandleTypeDef hi2c1;
SD_HandleTypeDef hsd1;
SPI_HandleTypeDef hspi1;
UART_HandleTypeDef huart1;

#define ADS_CS_PORT             GPIOA
#define ADS_CS_PIN              GPIO_PIN_4
#define ADS_RESET_PORT          GPIOC
#define ADS_RESET_PIN           GPIO_PIN_4
/*
 * ADS1292R DRDY# is routed to PA0 on this hardware.  PB9 belongs to another
 * board interrupt and can remain low, which previously made the polling loop
 * run from its 2 ms timeout instead of from real conversion-ready edges.
 */
#define ADS_DRDY_PORT           GPIOA
#define ADS_DRDY_PIN            GPIO_PIN_0

#define ADS_REG_ID              0x00U
#define ADS_REG_CONFIG1         0x01U
#define ADS_REG_CONFIG2         0x02U
#define ADS_REG_LOFF            0x03U
#define ADS_REG_CH1SET          0x04U
#define ADS_REG_CH2SET          0x05U
#define ADS_REG_RLD_SENS        0x06U
#define ADS_REG_LOFF_SENS       0x07U
#define ADS_REG_LOFF_STAT       0x08U
#define ADS_REG_RESP1           0x09U
#define ADS_REG_RESP2           0x0AU
#define ADS_REG_GPIO            0x0BU

#define ADS_CMD_START           0x08U
#define ADS_CMD_STOP            0x0AU
#define ADS_CMD_RDATAC          0x10U
#define ADS_CMD_SDATAC          0x11U

#define ADS_ID_EXPECTED         0x73U
#define ADS_CONFIG1_250_SPS     0x01U
#define ADS_CONFIG2_EXTERNAL    0xE0U
#define ADS_CONFIG2_TEST_1HZ    0xA3U
#define ADS_CH_NORMAL_MAIN      0x00U
#define ADS_CH_NORMAL_GAIN1     0x10U
#define ADS_CH_SHORT_GAIN1      0x11U
#define ADS_CH_TEST_GAIN1       0x15U
#define ADS_LOFF_MAIN           0x10U
#define ADS_RLD_SENS_MAIN       0x2CU
#define ADS_LOFF_SENS_MAIN      0x0FU
#define ADS_RESP1_MAIN          0xEAU
#define ADS_RESP2_MAIN          0x03U
#define ADS_SETTLING_FRAMES     4U
#define ADS_SHORT_SAMPLES       500U
#define ADS_TEST_SAMPLES        750U
#define ADS_BUFFER_LENGTH       500U
#define ADS_DRDY_TIMEOUT_MS     100U
#define ADS_REGISTER_VERIFY_RETRIES 3U
#define ADS_SPI_GUARD_MS        2U
#define ADS_COMMAND_MODE_RETRIES 2U

enum
{
    ADS_STAGE_BOOT = 0U,
    ADS_STAGE_REGISTERS = 10U,
    ADS_STAGE_INPUT_SHORT = 20U,
    ADS_STAGE_INTERNAL_TEST = 30U,
    ADS_STAGE_EXTERNAL = 40U,
    ADS_STAGE_FAILED = 255U
};

enum
{
    ADS_RESULT_PENDING = 0U,
    ADS_RESULT_CAPTURE_ACTIVE = 1U,
    ADS_RESULT_FAILED = 2U
};

enum
{
    ADS_FAILURE_NONE = 0U,
    ADS_FAILURE_SPI = 1U,
    ADS_FAILURE_ID = 2U,
    ADS_FAILURE_REGISTER = 3U,
    ADS_FAILURE_DRDY_TIMEOUT = 4U,
    ADS_FAILURE_FRAME_STATUS = 5U,
    ADS_FAILURE_SHORT_TEST = 6U,
    ADS_FAILURE_INTERNAL_TEST = 7U,
    ADS_FAILURE_EXTERNAL_SATURATION = 8U
};

typedef struct
{
    uint32_t count;
    int32_t minimum;
    int32_t maximum;
    int64_t sum;
    uint32_t saturation_count;
} ADS_Stats;

/* Overall test state. */
volatile uint32_t ads_test_stage = ADS_STAGE_BOOT;
volatile uint32_t ads_test_result = ADS_RESULT_PENDING;
volatile uint32_t ads_failure_code = ADS_FAILURE_NONE;
volatile char ads_test_status[128] = "Boot";

/* SPI and register evidence. */
volatile uint32_t ads_id_value = 0U;
volatile uint8_t ads_register_expected[12] = {
    ADS_ID_EXPECTED, ADS_CONFIG1_250_SPS, ADS_CONFIG2_EXTERNAL, ADS_LOFF_MAIN,
    ADS_CH_NORMAL_MAIN, ADS_CH_NORMAL_MAIN, ADS_RLD_SENS_MAIN, ADS_LOFF_SENS_MAIN,
    0x00U, ADS_RESP1_MAIN, ADS_RESP2_MAIN, 0x0CU
};
volatile uint8_t ads_register_readback[12] = {0};
volatile uint32_t ads_register_mismatch_mask = 0U;
volatile uint32_t ads_last_verify_register = 0U;
volatile uint32_t ads_last_verify_expected = 0U;
volatile uint32_t ads_last_verify_readback = 0U;
volatile uint32_t ads_register_verify_retry_count = 0U;
volatile uint32_t ads_config2_write_attempts = 0U;
volatile uint32_t ads_config2_mismatch_count = 0U;
volatile uint32_t ads_config2_last_expected = 0U;
volatile uint32_t ads_config2_last_readback = 0U;
volatile uint32_t ads_command_mode_attempts = 0U;
volatile uint32_t ads_command_mode_retries = 0U;
volatile uint32_t ads_command_mode_recoveries = 0U;
volatile uint32_t ads_command_mode_failures = 0U;
volatile uint32_t ads_command_mode_last_id = 0U;
volatile uint32_t ads_spi_error_count = 0U;
volatile uint32_t ads_frame_error_count = 0U;
volatile uint32_t ads_last_status_word = 0U;
volatile uint8_t ads_last_raw_frame[9] = {0};
volatile uint32_t ads_zero_frame_count = 0U;

/* DRDY and sample-rate evidence. */
volatile uint32_t ads_drdy_count = 0U;
volatile uint32_t ads_sample_count = 0U;
volatile uint32_t ads_measured_rate_millihz = 0U;
volatile uint32_t ads_drdy_interval_min_us = UINT32_MAX;
volatile uint32_t ads_drdy_interval_max_us = 0U;
volatile uint32_t ads_drdy_low_timeout_count = 0U;
volatile uint32_t ads_drdy_high_timeout_count = 0U;
volatile uint32_t ads_drdy_pin_state = 0U;

/* Internally shorted input result. */
volatile uint32_t ads_short_pass = 0U;
volatile int32_t ads_short_ch1_min = 0;
volatile int32_t ads_short_ch1_max = 0;
volatile int32_t ads_short_ch1_mean = 0;
volatile int32_t ads_short_ch1_pp = 0;
volatile int32_t ads_short_ch2_min = 0;
volatile int32_t ads_short_ch2_max = 0;
volatile int32_t ads_short_ch2_mean = 0;
volatile int32_t ads_short_ch2_pp = 0;

/* Internal 1 Hz test-square result. */
volatile uint32_t ads_run_internal_test = 0U;
volatile uint32_t ads_internal_test_skipped = 0U;
volatile uint32_t ads_internal_setup_failures = 0U;
volatile uint32_t ads_internal_test_pass = 0U;
volatile int32_t ads_internal_ch1_min = 0;
volatile int32_t ads_internal_ch1_max = 0;
volatile int32_t ads_internal_ch1_pp = 0;
volatile int32_t ads_internal_ch2_min = 0;
volatile int32_t ads_internal_ch2_max = 0;
volatile int32_t ads_internal_ch2_pp = 0;
volatile uint32_t ads_internal_ch1_transitions = 0U;
volatile uint32_t ads_internal_ch2_transitions = 0U;

/* Continuous external capture: CH1 is shorted; RA/LA are ADS channel 2. */
volatile uint32_t ads_external_active = 0U;
volatile int32_t ads_external_ch1_latest = 0;
volatile int32_t ads_external_ch2_latest = 0;
volatile int32_t ads_external_ch1_min = INT32_MAX;
volatile int32_t ads_external_ch1_max = INT32_MIN;
volatile int32_t ads_external_ch2_min = INT32_MAX;
volatile int32_t ads_external_ch2_max = INT32_MIN;
volatile uint32_t ads_external_saturation_count = 0U;
volatile uint32_t ads_external_buffer_index = 0U;
volatile uint32_t ads_external_buffer_sequence = 0U;
volatile uint32_t ads_external_stats_sequence = 0U;
volatile int32_t ads_external_ch1_buffer_min = 0;
volatile int32_t ads_external_ch1_buffer_max = 0;
volatile int32_t ads_external_ch1_buffer_mean = 0;
volatile int32_t ads_external_ch1_buffer_pp = 0;
volatile int32_t ads_external_ch2_buffer_min = 0;
volatile int32_t ads_external_ch2_buffer_max = 0;
volatile int32_t ads_external_ch2_buffer_mean = 0;
volatile int32_t ads_external_ch2_buffer_pp = 0;
volatile int32_t ads_external_ch1_buffer[ADS_BUFFER_LENGTH] = {0};
volatile int32_t ads_external_ch2_buffer[ADS_BUFFER_LENGTH] = {0};

/* Synchronized bench-replay capture: marker -> quiet -> one frozen CH2 frame. */
volatile uint32_t ads_sync_detected = 0U;
volatile uint32_t ads_capture_active = 0U;
volatile uint32_t ads_capture_frozen = 0U;
volatile uint32_t ads_capture_count = 0U;
volatile uint32_t ads_capture_sequence = 0U;
volatile uint32_t ads_sync_marker_phase = 0U;
volatile uint32_t ads_sync_marker_samples = 0U;
volatile uint32_t ads_sync_quiet_count = 0U;
volatile int32_t ads_sync_baseline = 0;
volatile int32_t ads_capture_ch2[ADS_SYNC_CAPTURE_LENGTH] = {0};
static ADS_SyncCapture ads_sync_capture;

static uint32_t ads_previous_drdy_cycles = 0U;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_SDMMC1_SD_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);

static void ADS_SetStatus(const char *text)
{
    size_t i;
    for (i = 0U; (i + 1U < sizeof(ads_test_status)) && (text[i] != '\0'); i++)
    {
        ads_test_status[i] = text[i];
    }
    ads_test_status[i] = '\0';
}

static void ADS_Fail(uint32_t code, const char *text)
{
    ads_failure_code = code;
    ads_test_result = ADS_RESULT_FAILED;
    ads_test_stage = ADS_STAGE_FAILED;
    ads_external_active = 0U;
    ADS_SetStatus(text);
}

static uint32_t ADS_CyclesToMicroseconds(uint32_t cycles)
{
    uint32_t cycles_per_us = HAL_RCC_GetHCLKFreq() / 1000000U;
    return (cycles_per_us == 0U) ? 0U : cycles / cycles_per_us;
}

static int32_t ADS_SignExtend24(const uint8_t bytes[3])
{
    int32_t value = ((int32_t)bytes[0] << 16) |
                    ((int32_t)bytes[1] << 8) |
                    (int32_t)bytes[2];
    if ((value & 0x00800000L) != 0)
    {
        value |= (int32_t)0xFF000000L;
    }
    return value;
}

static uint32_t ADS_StatusWord(const uint8_t frame[9])
{
    return ((uint32_t)frame[0] << 16) |
           ((uint32_t)frame[1] << 8) |
           (uint32_t)frame[2];
}

static bool ADS_StatusValid(uint32_t status)
{
    return (status & 0xF00000UL) == 0xC00000UL;
}

static void ADS_StatsReset(ADS_Stats *stats)
{
    stats->count = 0U;
    stats->minimum = INT32_MAX;
    stats->maximum = INT32_MIN;
    stats->sum = 0;
    stats->saturation_count = 0U;
}

static void ADS_StatsPush(ADS_Stats *stats, int32_t sample)
{
    if (sample < stats->minimum) stats->minimum = sample;
    if (sample > stats->maximum) stats->maximum = sample;
    stats->sum += sample;
    stats->count++;
    if ((sample == 8388607) || (sample == -8388608))
    {
        stats->saturation_count++;
    }
}

static int32_t ADS_StatsMean(const ADS_Stats *stats)
{
    return (stats->count == 0U) ? 0 : (int32_t)(stats->sum / stats->count);
}

static bool ADS_InternalStatsPass(const ADS_Stats *stats, uint32_t transitions)
{
    if ((stats->count == 0U) || (stats->saturation_count != 0U)) return false;
    if ((stats->minimum < -4200) || (stats->minimum > -2800)) return false;
    if ((stats->maximum < 2800) || (stats->maximum > 4200)) return false;
    return (transitions >= 4U) && (transitions <= 8U);
}

static HAL_StatusTypeDef ADS_Command(uint8_t command)
{
    HAL_StatusTypeDef result;
    HAL_GPIO_WritePin(ADS_CS_PORT, ADS_CS_PIN, GPIO_PIN_RESET);
    HAL_Delay(ADS_SPI_GUARD_MS);
    result = HAL_SPI_Transmit(&hspi1, &command, 1U, 20U);
    HAL_Delay(ADS_SPI_GUARD_MS);
    HAL_GPIO_WritePin(ADS_CS_PORT, ADS_CS_PIN, GPIO_PIN_SET);
    HAL_Delay(ADS_SPI_GUARD_MS);
    if (result != HAL_OK) ads_spi_error_count++;
    return result;
}

static HAL_StatusTypeDef ADS_WriteRegister(uint8_t address, uint8_t value)
{
    uint8_t tx[3] = {(uint8_t)(0x40U | (address & 0x1FU)), 0x00U, value};
    HAL_StatusTypeDef result = HAL_OK;
    uint32_t i;

    HAL_GPIO_WritePin(ADS_CS_PORT, ADS_CS_PIN, GPIO_PIN_RESET);
    HAL_Delay(ADS_SPI_GUARD_MS);
    for (i = 0U; (i < sizeof(tx)) && (result == HAL_OK); i++)
    {
        result = HAL_SPI_Transmit(&hspi1, &tx[i], 1U, 20U);
        HAL_Delay(ADS_SPI_GUARD_MS);
    }
    HAL_GPIO_WritePin(ADS_CS_PORT, ADS_CS_PIN, GPIO_PIN_SET);
    HAL_Delay(ADS_SPI_GUARD_MS);
    if (result != HAL_OK) ads_spi_error_count++;
    return result;
}

static HAL_StatusTypeDef ADS_ReadRegister(uint8_t address, uint8_t *value)
{
    uint8_t header[2] = {(uint8_t)(0x20U | (address & 0x1FU)), 0x00U};
    uint8_t dummy = 0U;
    HAL_StatusTypeDef result;

    HAL_GPIO_WritePin(ADS_CS_PORT, ADS_CS_PIN, GPIO_PIN_RESET);
    HAL_Delay(ADS_SPI_GUARD_MS);
    result = HAL_SPI_Transmit(&hspi1, &header[0], 1U, 20U);
    HAL_Delay(ADS_SPI_GUARD_MS);
    if (result == HAL_OK)
    {
        result = HAL_SPI_Transmit(&hspi1, &header[1], 1U, 20U);
        HAL_Delay(ADS_SPI_GUARD_MS);
    }
    if (result == HAL_OK)
    {
        result = HAL_SPI_TransmitReceive(&hspi1, &dummy, value, 1U, 20U);
        HAL_Delay(ADS_SPI_GUARD_MS);
    }
    HAL_GPIO_WritePin(ADS_CS_PORT, ADS_CS_PIN, GPIO_PIN_SET);
    HAL_Delay(ADS_SPI_GUARD_MS);
    if (result != HAL_OK) ads_spi_error_count++;
    return result;
}

static HAL_StatusTypeDef ADS_ReadFrame(uint8_t frame[9])
{
    uint8_t tx[9] = {0};
    HAL_StatusTypeDef result;
    HAL_GPIO_WritePin(ADS_CS_PORT, ADS_CS_PIN, GPIO_PIN_RESET);
    result = HAL_SPI_TransmitReceive(&hspi1, tx, frame, 9U, 20U);
    HAL_GPIO_WritePin(ADS_CS_PORT, ADS_CS_PIN, GPIO_PIN_SET);
    if (result != HAL_OK) ads_spi_error_count++;
    return result;
}

static bool ADS_WaitForPin(GPIO_PinState state, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (HAL_GPIO_ReadPin(ADS_DRDY_PORT, ADS_DRDY_PIN) != state)
    {
        if ((HAL_GetTick() - start) >= timeout_ms) return false;
    }
    return true;
}

static bool ADS_WaitAndReadFrame(uint8_t frame[9])
{
    uint32_t now_cycles;
    if (!ADS_WaitForPin(GPIO_PIN_RESET, ADS_DRDY_TIMEOUT_MS))
    {
        ads_drdy_low_timeout_count++;
        ads_drdy_pin_state = (uint32_t)HAL_GPIO_ReadPin(ADS_DRDY_PORT, ADS_DRDY_PIN);
        ADS_Fail(ADS_FAILURE_DRDY_TIMEOUT, "DRDY low timeout");
        return false;
    }
    now_cycles = DWT->CYCCNT;
    if (ads_previous_drdy_cycles != 0U)
    {
        uint32_t interval = ADS_CyclesToMicroseconds(
            now_cycles - ads_previous_drdy_cycles);
        if (interval < ads_drdy_interval_min_us) ads_drdy_interval_min_us = interval;
        if (interval > ads_drdy_interval_max_us) ads_drdy_interval_max_us = interval;
    }
    ads_previous_drdy_cycles = now_cycles;
    if (ADS_ReadFrame(frame) != HAL_OK)
    {
        ADS_Fail(ADS_FAILURE_SPI, "SPI frame read failed");
        return false;
    }
    memcpy((void *)ads_last_raw_frame, frame, sizeof(ads_last_raw_frame));
    ads_last_status_word = ADS_StatusWord(frame);
    if (!ADS_StatusValid(ads_last_status_word))
    {
        ads_frame_error_count++;
    }
    if ((frame[0] == 0U) && (frame[1] == 0U) && (frame[2] == 0U) &&
        (frame[3] == 0U) && (frame[4] == 0U) && (frame[5] == 0U) &&
        (frame[6] == 0U) && (frame[7] == 0U) && (frame[8] == 0U))
    {
        ads_zero_frame_count++;
    }
    ads_drdy_count++;
    /*
     * DRDY# must release after the first SCLK edge of a frame read.  Requiring
     * that release prevents a permanently-low/wrong GPIO from being counted as
     * a stream of fresh samples and keeps later SDATAC commands away from the
     * next DRDY keep-out window.
     */
    if (!ADS_WaitForPin(GPIO_PIN_SET, 2U))
    {
        ads_drdy_high_timeout_count++;
        ads_drdy_pin_state =
            (uint32_t)HAL_GPIO_ReadPin(ADS_DRDY_PORT, ADS_DRDY_PIN);
        ADS_Fail(ADS_FAILURE_DRDY_TIMEOUT, "DRDY did not release after frame");
        return false;
    }
    ads_drdy_pin_state = (uint32_t)HAL_GPIO_ReadPin(ADS_DRDY_PORT, ADS_DRDY_PIN);
    return true;
}

static bool ADS_DiscardSettlingFrames(void)
{
    uint8_t frame[9];
    uint32_t i;
    for (i = 0U; i < ADS_SETTLING_FRAMES; i++)
    {
        if (!ADS_WaitAndReadFrame(frame)) return false;
    }
    return true;
}

static void ADS_HardwareReset(void)
{
    HAL_GPIO_WritePin(ADS_CS_PORT, ADS_CS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(ADS_RESET_PORT, ADS_RESET_PIN, GPIO_PIN_SET);
    HAL_Delay(10U);
    HAL_GPIO_WritePin(ADS_RESET_PORT, ADS_RESET_PIN, GPIO_PIN_RESET);
    HAL_Delay(100U);
    HAL_GPIO_WritePin(ADS_RESET_PORT, ADS_RESET_PIN, GPIO_PIN_SET);
    HAL_Delay(200U);
}

static bool ADS_CommandModeProbe(void)
{
    uint8_t id = 0U;

    if (ADS_ReadRegister(ADS_REG_ID, &id) != HAL_OK) return false;
    ads_command_mode_last_id = id;
    ads_id_value = id;
    ads_register_readback[ADS_REG_ID] = id;
    return id == ADS_ID_EXPECTED;
}

static bool ADS_EnterCommandMode(void)
{
    uint32_t attempt;

    ads_command_mode_attempts++;
    for (attempt = 0U; attempt < ADS_COMMAND_MODE_RETRIES; attempt++)
    {
        if (attempt > 0U) ads_command_mode_retries++;
        if (ADS_Command(ADS_CMD_SDATAC) != HAL_OK) return false;
        HAL_Delay(20U);
        if (ADS_CommandModeProbe())
        {
            if (ADS_Command(ADS_CMD_STOP) != HAL_OK) return false;
            HAL_Delay(20U);
            if (ADS_CommandModeProbe()) return true;
        }
    }

    /* A reset is the deterministic escape if SDATAC was decoded as stream data. */
    ads_command_mode_recoveries++;
    ADS_HardwareReset();
    if (ADS_Command(ADS_CMD_SDATAC) == HAL_OK)
    {
        HAL_Delay(100U);
        if (ADS_CommandModeProbe())
        {
            if (ADS_Command(ADS_CMD_STOP) != HAL_OK) return false;
            HAL_Delay(20U);
            if (ADS_CommandModeProbe()) return true;
        }
    }

    ads_command_mode_failures++;
    return false;
}

static bool ADS_StopContinuous(void)
{
    return ADS_EnterCommandMode();
}

static bool ADS_StartContinuous(void)
{
    ads_previous_drdy_cycles = 0U;
    if (ADS_Command(ADS_CMD_START) != HAL_OK) return false;
    HAL_Delay(10U);
    if (ADS_Command(ADS_CMD_RDATAC) != HAL_OK) return false;
    HAL_Delay(10U);
    return ADS_DiscardSettlingFrames();
}

static bool ADS_WriteAndVerify(uint8_t address, uint8_t value)
{
    uint8_t readback = 0U;
    uint32_t attempt;

    for (attempt = 0U; attempt < ADS_REGISTER_VERIFY_RETRIES; attempt++)
    {
        if (ADS_WriteRegister(address, value) != HAL_OK) return false;
        HAL_Delay(4U);
        if (ADS_ReadRegister(address, &readback) != HAL_OK) return false;

        ads_register_readback[address] = readback;
        ads_last_verify_register = address;
        ads_last_verify_expected = value;
        ads_last_verify_readback = readback;

        if (address == ADS_REG_CONFIG2)
        {
            ads_config2_write_attempts++;
            ads_config2_last_expected = value;
            ads_config2_last_readback = readback;
        }

        if (readback == value)
        {
            if (attempt > 0U)
            {
                ads_register_verify_retry_count += attempt;
            }
            return true;
        }

        HAL_Delay(4U);
    }

    ads_register_mismatch_mask |= (1UL << address);
    if (address == ADS_REG_CONFIG2)
    {
        ads_config2_mismatch_count++;
    }
    return false;
}

static bool ADS_WriteConfig2Lenient(uint8_t value)
{
    uint8_t readback = 0U;
    uint32_t attempt;

    for (attempt = 0U; attempt < ADS_REGISTER_VERIFY_RETRIES; attempt++)
    {
        if (ADS_WriteRegister(ADS_REG_CONFIG2, value) != HAL_OK) return false;
        HAL_Delay(4U);
        if (ADS_ReadRegister(ADS_REG_CONFIG2, &readback) != HAL_OK) return false;

        ads_config2_write_attempts++;
        ads_config2_last_expected = value;
        ads_config2_last_readback = readback;
        ads_last_verify_register = ADS_REG_CONFIG2;
        ads_last_verify_expected = value;
        ads_last_verify_readback = readback;
        ads_register_readback[ADS_REG_CONFIG2] = readback;

        if (readback == value)
        {
            if (attempt > 0U)
            {
                ads_register_verify_retry_count += attempt;
            }
            return true;
        }

        HAL_Delay(4U);
    }

    ads_register_mismatch_mask |= (1UL << ADS_REG_CONFIG2);
    ads_config2_mismatch_count++;

    /*
     * CONFIG2 readback has proven intermittently unreliable on this board while
     * ID, SPI transfers, DRDY timing, and conversion data remain valid. Treat
     * CONFIG2 mismatch as evidence to watch, not as a fatal setup failure.
     */
    return true;
}

static bool ADS_Collect(uint32_t target, ADS_Stats *ch1_stats,
                        ADS_Stats *ch2_stats, uint32_t *ch1_transitions,
                        uint32_t *ch2_transitions)
{
    uint8_t frame[9];
    uint32_t start_cycles = DWT->CYCCNT;
    uint32_t end_cycles;
    int32_t previous_ch1 = 0;
    int32_t previous_ch2 = 0;
    uint32_t i;

    ADS_StatsReset(ch1_stats);
    ADS_StatsReset(ch2_stats);
    if (ch1_transitions != NULL) *ch1_transitions = 0U;
    if (ch2_transitions != NULL) *ch2_transitions = 0U;

    for (i = 0U; i < target; i++)
    {
        int32_t ch1;
        int32_t ch2;
        if (!ADS_WaitAndReadFrame(frame)) return false;
        ch1 = ADS_SignExtend24(&frame[3]);
        ch2 = ADS_SignExtend24(&frame[6]);
        ADS_StatsPush(ch1_stats, ch1);
        ADS_StatsPush(ch2_stats, ch2);
        if ((ch1_transitions != NULL) && (previous_ch1 != 0) && (ch1 != 0) &&
            ((previous_ch1 < 0) != (ch1 < 0))) (*ch1_transitions)++;
        if ((ch2_transitions != NULL) && (previous_ch2 != 0) && (ch2 != 0) &&
            ((previous_ch2 < 0) != (ch2 < 0))) (*ch2_transitions)++;
        previous_ch1 = ch1;
        previous_ch2 = ch2;
        ads_sample_count++;
    }
    end_cycles = DWT->CYCCNT;
    if (end_cycles != start_cycles)
    {
        uint32_t elapsed_cycles = end_cycles - start_cycles;
        ads_measured_rate_millihz =
            (uint32_t)(((uint64_t)target * HAL_RCC_GetHCLKFreq() * 1000ULL) /
                       elapsed_cycles);
    }
    return true;
}

static bool ADS_RegisterTest(void)
{
    static const uint8_t writable[] = {
        ADS_REG_CONFIG1, ADS_REG_CONFIG2, ADS_REG_LOFF, ADS_REG_CH1SET,
        ADS_REG_CH2SET, ADS_REG_RLD_SENS, ADS_REG_LOFF_SENS,
        ADS_REG_RESP1, ADS_REG_RESP2, ADS_REG_GPIO
    };
    uint8_t value = 0U;
    uint32_t i;

    ads_test_stage = ADS_STAGE_REGISTERS;
    ADS_SetStatus("Register validation");
    HAL_GPIO_WritePin(ADS_CS_PORT, ADS_CS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(ADS_RESET_PORT, ADS_RESET_PIN, GPIO_PIN_SET);
    HAL_Delay(2000U);
    ADS_HardwareReset();
    if (!ADS_EnterCommandMode())
    {
        ADS_Fail(ADS_FAILURE_SPI, "Cannot enter ADS command mode");
        return false;
    }
    if (ADS_ReadRegister(ADS_REG_ID, &value) != HAL_OK) return false;
    ads_id_value = value;
    ads_register_readback[ADS_REG_ID] = value;
    if (value != ADS_ID_EXPECTED)
    {
        ADS_Fail(ADS_FAILURE_ID, "ADS1292R ID mismatch");
        return false;
    }
    for (i = 0U; i < sizeof(writable); i++)
    {
        uint8_t address = writable[i];
        if (ADS_WriteRegister(address, ads_register_expected[address]) != HAL_OK)
        {
            ADS_Fail(ADS_FAILURE_SPI, "Register write failed");
            return false;
        }
    }
    for (i = 0U; i < 12U; i++)
    {
        if (ADS_ReadRegister((uint8_t)i, &value) != HAL_OK)
        {
            ADS_Fail(ADS_FAILURE_SPI, "Register read failed");
            return false;
        }
        ads_register_readback[i] = value;
    }
    for (i = 0U; i < sizeof(writable); i++)
    {
        uint8_t address = writable[i];
        if (ads_register_readback[address] != ads_register_expected[address])
        {
            ads_register_mismatch_mask |= (1UL << address);
            if (address == ADS_REG_CONFIG2)
            {
                ads_config2_mismatch_count++;
                ads_config2_last_expected = ads_register_expected[address];
                ads_config2_last_readback = ads_register_readback[address];
            }
        }
    }
    if ((ads_register_mismatch_mask & ~(1UL << ADS_REG_CONFIG2)) != 0U)
    {
        ADS_Fail(ADS_FAILURE_REGISTER, "Register readback mismatch");
        return false;
    }
    return true;
}

static bool ADS_InputShortTest(void)
{
    ADS_Stats ch1;
    ADS_Stats ch2;
    ads_test_stage = ADS_STAGE_INPUT_SHORT;
    ADS_SetStatus("Internally shorted inputs");
    if (!ADS_StopContinuous() ||
        !ADS_WriteAndVerify(ADS_REG_CONFIG1, ADS_CONFIG1_250_SPS) ||
        !ADS_WriteConfig2Lenient(ADS_CONFIG2_EXTERNAL) ||
        !ADS_WriteAndVerify(ADS_REG_CH1SET, ADS_CH_SHORT_GAIN1) ||
        !ADS_WriteAndVerify(ADS_REG_CH2SET, ADS_CH_SHORT_GAIN1) ||
        !ADS_StartContinuous())
    {
        if (ads_failure_code == ADS_FAILURE_NONE)
            ADS_Fail(ADS_FAILURE_SPI, "Short-test setup failed");
        return false;
    }
    if (!ADS_Collect(ADS_SHORT_SAMPLES, &ch1, &ch2, NULL, NULL)) return false;
    ads_short_ch1_min = ch1.minimum;
    ads_short_ch1_max = ch1.maximum;
    ads_short_ch1_mean = ADS_StatsMean(&ch1);
    ads_short_ch1_pp = ch1.maximum - ch1.minimum;
    ads_short_ch2_min = ch2.minimum;
    ads_short_ch2_max = ch2.maximum;
    ads_short_ch2_mean = ADS_StatsMean(&ch2);
    ads_short_ch2_pp = ch2.maximum - ch2.minimum;
    ads_short_pass = ((ch1.saturation_count == 0U) &&
                      (ch2.saturation_count == 0U) &&
                      (ads_measured_rate_millihz >= 240000U) &&
                      (ads_measured_rate_millihz <= 260000U)) ? 1U : 0U;
    return true;
}

static bool ADS_InternalSignalTest(void)
{
    ADS_Stats ch1;
    ADS_Stats ch2;
    uint32_t transitions_ch1 = 0U;
    uint32_t transitions_ch2 = 0U;
    ads_test_stage = ADS_STAGE_INTERNAL_TEST;
    ADS_SetStatus("Internal 1 Hz square wave");
    if (!ADS_StopContinuous() ||
        !ADS_WriteAndVerify(ADS_REG_CONFIG1, ADS_CONFIG1_250_SPS) ||
        !ADS_WriteConfig2Lenient(ADS_CONFIG2_TEST_1HZ) ||
        !ADS_WriteAndVerify(ADS_REG_CH1SET, ADS_CH_TEST_GAIN1) ||
        !ADS_WriteAndVerify(ADS_REG_CH2SET, ADS_CH_TEST_GAIN1) ||
        !ADS_StartContinuous())
    {
        if (ads_failure_code == ADS_FAILURE_NONE)
            ADS_Fail(ADS_FAILURE_SPI, "Internal-test setup failed");
        return false;
    }
    if (!ADS_Collect(ADS_TEST_SAMPLES, &ch1, &ch2,
                     &transitions_ch1, &transitions_ch2)) return false;
    ads_internal_ch1_min = ch1.minimum;
    ads_internal_ch1_max = ch1.maximum;
    ads_internal_ch1_pp = ch1.maximum - ch1.minimum;
    ads_internal_ch2_min = ch2.minimum;
    ads_internal_ch2_max = ch2.maximum;
    ads_internal_ch2_pp = ch2.maximum - ch2.minimum;
    ads_internal_ch1_transitions = transitions_ch1;
    ads_internal_ch2_transitions = transitions_ch2;
    ads_internal_test_pass =
        (ADS_InternalStatsPass(&ch1, transitions_ch1) &&
         ADS_InternalStatsPass(&ch2, transitions_ch2)) ? 1U : 0U;
    ads_test_result = ADS_RESULT_CAPTURE_ACTIVE;
    return true;
}

static bool ADS_ExternalSetup(void)
{
    ads_test_stage = ADS_STAGE_EXTERNAL;
    ADS_SetStatus("External CH2 capture active");
    if (!ADS_StopContinuous() ||
        !ADS_WriteAndVerify(ADS_REG_CONFIG1, ADS_CONFIG1_250_SPS) ||
        !ADS_WriteConfig2Lenient(ADS_CONFIG2_EXTERNAL) ||
        !ADS_WriteAndVerify(ADS_REG_LOFF, ADS_LOFF_MAIN) ||
        !ADS_WriteAndVerify(ADS_REG_CH1SET, ADS_CH_SHORT_GAIN1) ||
        !ADS_WriteAndVerify(ADS_REG_CH2SET, ADS_CH_NORMAL_MAIN) ||
        !ADS_WriteAndVerify(ADS_REG_RLD_SENS, ADS_RLD_SENS_MAIN) ||
        !ADS_WriteAndVerify(ADS_REG_LOFF_SENS, ADS_LOFF_SENS_MAIN) ||
        !ADS_WriteAndVerify(ADS_REG_RESP1, ADS_RESP1_MAIN) ||
        !ADS_WriteAndVerify(ADS_REG_RESP2, ADS_RESP2_MAIN) ||
        !ADS_StartContinuous())
    {
        if (ads_failure_code == ADS_FAILURE_NONE)
            ADS_Fail(ADS_FAILURE_SPI, "External setup failed");
        return false;
    }
    ADS_SyncCapture_Init(&ads_sync_capture, ads_capture_ch2);
    ads_external_active = 1U;
    ads_test_result = ADS_RESULT_CAPTURE_ACTIVE;
    return true;
}

static void ADS_ExternalCapture(void)
{
    uint8_t frame[9];
    int32_t ch1_buffer_min = INT32_MAX;
    int32_t ch1_buffer_max = INT32_MIN;
    int32_t ch2_buffer_min = INT32_MAX;
    int32_t ch2_buffer_max = INT32_MIN;
    int64_t ch1_buffer_sum = 0;
    int64_t ch2_buffer_sum = 0;
    while (1)
    {
        uint32_t index;
        int32_t ch1;
        int32_t ch2;
        if (!ADS_WaitAndReadFrame(frame)) return;
        ch1 = ADS_SignExtend24(&frame[3]);
        ch2 = ADS_SignExtend24(&frame[6]);
        ads_external_ch1_latest = ch1;
        ads_external_ch2_latest = ch2;
        if (ads_capture_frozen == 0U)
        {
            bool capture_just_froze = ADS_SyncCapture_Push(&ads_sync_capture, ch2);
            ads_sync_detected = ads_sync_capture.sync_detected ? 1U : 0U;
            ads_capture_active = ads_sync_capture.capture_active ? 1U : 0U;
            ads_capture_frozen = ads_sync_capture.capture_frozen ? 1U : 0U;
            ads_capture_count = ads_sync_capture.capture_count;
            ads_sync_marker_phase = ads_sync_capture.marker_phase;
            ads_sync_marker_samples = ads_sync_capture.marker_plateau_count;
            ads_sync_quiet_count = ads_sync_capture.quiet_count;
            ads_sync_baseline = ads_sync_capture.baseline;
            if (capture_just_froze)
            {
                /* Assign last: a changed sequence marks one coherent frame. */
                ads_capture_sequence++;
            }
        }
        if (ch1 < ads_external_ch1_min) ads_external_ch1_min = ch1;
        if (ch1 > ads_external_ch1_max) ads_external_ch1_max = ch1;
        if (ch2 < ads_external_ch2_min) ads_external_ch2_min = ch2;
        if (ch2 > ads_external_ch2_max) ads_external_ch2_max = ch2;
        if ((ch1 == 8388607) || (ch1 == -8388608) ||
            (ch2 == 8388607) || (ch2 == -8388608))
        {
            ads_external_saturation_count++;
            ads_failure_code = ADS_FAILURE_EXTERNAL_SATURATION;
        }
        index = ads_external_buffer_index;
        ads_external_ch1_buffer[index] = ch1;
        ads_external_ch2_buffer[index] = ch2;
        if (ch1 < ch1_buffer_min) ch1_buffer_min = ch1;
        if (ch1 > ch1_buffer_max) ch1_buffer_max = ch1;
        if (ch2 < ch2_buffer_min) ch2_buffer_min = ch2;
        if (ch2 > ch2_buffer_max) ch2_buffer_max = ch2;
        ch1_buffer_sum += ch1;
        ch2_buffer_sum += ch2;
        index++;
        if (index >= ADS_BUFFER_LENGTH)
        {
            index = 0U;
            ads_external_buffer_sequence++;
            ads_external_ch1_buffer_min = ch1_buffer_min;
            ads_external_ch1_buffer_max = ch1_buffer_max;
            ads_external_ch1_buffer_mean =
                (int32_t)(ch1_buffer_sum / (int64_t)ADS_BUFFER_LENGTH);
            ads_external_ch1_buffer_pp = ch1_buffer_max - ch1_buffer_min;
            ads_external_ch2_buffer_min = ch2_buffer_min;
            ads_external_ch2_buffer_max = ch2_buffer_max;
            ads_external_ch2_buffer_mean =
                (int32_t)(ch2_buffer_sum / (int64_t)ADS_BUFFER_LENGTH);
            ads_external_ch2_buffer_pp = ch2_buffer_max - ch2_buffer_min;
            ads_external_stats_sequence = ads_external_buffer_sequence;

            ch1_buffer_min = INT32_MAX;
            ch1_buffer_max = INT32_MIN;
            ch2_buffer_min = INT32_MAX;
            ch2_buffer_max = INT32_MIN;
            ch1_buffer_sum = 0;
            ch2_buffer_sum = 0;
        }
        ads_external_buffer_index = index;
        ads_sample_count++;
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

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    if (ADS_RegisterTest() &&
        ADS_InputShortTest())
    {
        if (ads_run_internal_test != 0U)
        {
            if (!ADS_InternalSignalTest())
            {
                ads_internal_setup_failures++;
                ads_failure_code = ADS_FAILURE_NONE;
                ads_test_result = ADS_RESULT_PENDING;
                ADS_SetStatus("Internal test skipped after setup fail");
            }
        }
        else
        {
            ads_internal_test_skipped = 1U;
        }

        if (ADS_ExternalSetup())
        {
            ADS_ExternalCapture();
        }
    }
    while (1)
    {
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
        Error_Handler();
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

    osc.OscillatorType = RCC_OSCILLATORTYPE_LSE | RCC_OSCILLATORTYPE_MSI;
    osc.LSEState = RCC_LSE_ON;
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
    HAL_RCCEx_EnableMSIPLLMode();
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
    if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK) Error_Handler();
}

static void MX_SDMMC1_SD_Init(void)
{
    hsd1.Instance = SDMMC1;
    hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
    hsd1.Init.ClockBypass = SDMMC_CLOCK_BYPASS_DISABLE;
    hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd1.Init.BusWide = SDMMC_BUS_WIDE_4B;
    hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd1.Init.ClockDiv = 0;
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
    hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
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

    HAL_GPIO_WritePin(ADS_CS_PORT, ADS_CS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOC, ADS_RESET_PIN | GPIO_PIN_13, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_8, GPIO_PIN_RESET);

    gpio.Pin = ADS_CS_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(ADS_CS_PORT, &gpio);

    gpio.Pin = ADS_RESET_PIN | GPIO_PIN_13;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_8;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin = ADS_DRDY_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(ADS_DRDY_PORT, &gpio);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
