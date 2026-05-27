/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : ECG + IMU CSV logger — SD card + A7670G cellular upload
  *
  * Built on the confirmed-working FatFS/RTC/LIS3DH CSV base.
  *
  * Architecture:
  *   DRDY ISR  (PA0, EXTI0, falling edge)  — reads ADS1292R @ 500 Hz
  *                                           sets ecg_sample_ready flag +
  *                                           stores latest sample
  *   TIM2 ISR  (20 ms / 50 Hz)             — sets imu_ready flag
  *   Main loop                             — writes CSV rows to SD on each flag
  *                                        — f_sync every 100 ECG samples
  *                                        — every 10 min: close file,
  *                                          POST via A7670G, open new file
  *
  * Key fixes vs. previous attempt:
  *   1. PA0 now GPIO_MODE_IT_FALLING + EXTI0 enabled  (was GPIO_MODE_INPUT)
  *   2. SPI CPHA = SPI_PHASE_2EDGE  (ADS1292R Mode 1 — was PHASE_1EDGE)
  *   3. SPI prescaler /256 ~312 kHz (was /8 ~10 MHz, above ADS1292R limit)
  *   4. ADS1292R CS starts HIGH (deasserted)  (was LOW)
  *   5. TIM2 added for IMU 50 Hz timing
  *   6. Data written on every sample — no block threshold needed
  *   7. Upload: APN configured before HTTPINIT, retry loop on network reg
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fatfs.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;

RTC_HandleTypeDef hrtc;

SD_HandleTypeDef hsd1;

SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
volatile char     test_status[128] = "Initializing...";

/* ECG — written in DRDY ISR, read in main loop */
volatile uint8_t  ecg_sample_ready = 0;
volatile int32_t  ecg_ch1_latest   = 0;
volatile int32_t  ecg_ch2_latest   = 0;

/* IMU flag — set in TIM2 ISR, cleared in main loop */
volatile uint8_t  imu_ready = 0;

/* SD / FatFS */
static FATFS  fs;
static FIL    fil;
static uint8_t file_open     = 0;
static char    current_filename[32] = {0};
static char    upload_filename[32]  = {0};

/* Upload interval — 10 minutes */
#define UPLOAD_INTERVAL_MS  (10UL * 60UL * 1000UL)
static uint32_t last_upload_tick = 0;

/* Sync every N ECG samples to protect against power loss (~0.2 s at 500 Hz) */
#define ECG_SYNC_INTERVAL   100
static uint32_t ecg_sample_count = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_SDMMC1_SD_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_RTC_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */
/* ADS1292R */
static void    ADS1292R_Init(void);
static void    ADS1292R_ReadSample(int32_t *ch1, int32_t *ch2);

/* LIS3DH */
static void    LIS3DH_Init(void);
static void    LIS3DH_ReadSample(int16_t *x, int16_t *y, int16_t *z);

/* SD helpers */
static HAL_StatusTypeDef SD_OpenNewFile(void);
static void    SD_CloseFile(void);

/* A7670G cellular */
static void    A7670G_PowerUp(void);
static void    A7670G_PowerDown(void);
static uint8_t A7670G_SendAT(const char *cmd, const char *expected,
                              uint32_t timeout_ms);
static void    A7670G_UploadFile(const char *fname);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ==========================================================================
 * ISR: DRDY falling edge on PA0 — reads one ECG sample
 * ========================================================================= */
void EXTI0_IRQHandler(void)
{
    if (__HAL_GPIO_EXTI_GET_IT(GPIO_PIN_0))
    {
        int32_t ch1, ch2;
        ADS1292R_ReadSample(&ch1, &ch2);
        ecg_ch1_latest   = ch1;
        ecg_ch2_latest   = ch2;
        ecg_sample_ready = 1;
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_0);
    }
}

/* ==========================================================================
 * ISR: TIM2 period elapsed — 50 Hz IMU flag
 * ========================================================================= */
void TIM2_IRQHandler(void)
{
    if (__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE) &&
        __HAL_TIM_GET_IT_SOURCE(&htim2, TIM_IT_UPDATE))
    {
        __HAL_TIM_CLEAR_IT(&htim2, TIM_IT_UPDATE);
        imu_ready = 1;
    }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_SDMMC1_SD_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  MX_RTC_Init();
  MX_TIM2_Init();
  MX_FATFS_Init();
  /* USER CODE BEGIN 2 */

    /* ---- Init sensors ---- */
    snprintf((char*)test_status, sizeof(test_status), "Init: ADS1292R...");
    ADS1292R_Init();

    snprintf((char*)test_status, sizeof(test_status), "Init: LIS3DH...");
    LIS3DH_Init();

    /* ---- Mount SD ---- */
    snprintf((char*)test_status, sizeof(test_status), "Init: Mounting SD...");
    if (f_mount(&fs, "", 1) != FR_OK)
    {
        snprintf((char*)test_status, sizeof(test_status), "FAIL: f_mount");
        Error_Handler();
    }

    if (SD_OpenNewFile() != HAL_OK)
    {
        snprintf((char*)test_status, sizeof(test_status), "FAIL: SD open");
        Error_Handler();
    }

    /* ---- Start TIM2 @ 50 Hz for IMU ---- */
    HAL_TIM_Base_Start_IT(&htim2);

    /* ---- Enable DRDY interrupt on PA0 ---- */
    HAL_NVIC_SetPriority(EXTI0_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);

    last_upload_tick = HAL_GetTick();
    snprintf((char*)test_status, sizeof(test_status),
             "Logging: %s", current_filename);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
    while (1)
    {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

        /* ------------------------------------------------------------------
         * 1. Write ECG row on every DRDY sample (500 Hz)
         * ----------------------------------------------------------------- */
        if (ecg_sample_ready)
        {
            /* Snapshot volatiles with interrupts briefly masked */
            __disable_irq();
            int32_t ch1 = ecg_ch1_latest;
            int32_t ch2 = ecg_ch2_latest;
            ecg_sample_ready = 0;
            __enable_irq();

            if (file_open)
            {
                RTC_TimeTypeDef t = {0};
                RTC_DateTypeDef d = {0};
                HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
                HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);

                char row[64];
                snprintf(row, sizeof(row),
                         "E,%02d:%02d:%02d,%ld,%ld\r\n",
                         t.Hours, t.Minutes, t.Seconds,
                         (long)ch1, (long)ch2);
                f_puts(row, &fil);

                ecg_sample_count++;
                if (ecg_sample_count % ECG_SYNC_INTERVAL == 0)
                    f_sync(&fil);
            }
        }

        /* ------------------------------------------------------------------
         * 2. Write IMU row on TIM2 flag (50 Hz)
         * ----------------------------------------------------------------- */
        if (imu_ready)
        {
            imu_ready = 0;

            int16_t ax, ay, az;
            LIS3DH_ReadSample(&ax, &ay, &az);

            if (file_open)
            {
                RTC_TimeTypeDef t = {0};
                RTC_DateTypeDef d = {0};
                HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
                HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);

                char row[64];
                snprintf(row, sizeof(row),
                         "I,%02d:%02d:%02d,%d,%d,%d\r\n",
                         t.Hours, t.Minutes, t.Seconds,
                         ax, ay, az);
                f_puts(row, &fil);
            }
        }

        /* ------------------------------------------------------------------
         * 3. Upload every 10 minutes
         * ----------------------------------------------------------------- */
        if ((HAL_GetTick() - last_upload_tick) >= UPLOAD_INTERVAL_MS)
        {
            last_upload_tick = HAL_GetTick();

            /* Disable data capture during upload */
            HAL_NVIC_DisableIRQ(EXTI0_IRQn);
            HAL_TIM_Base_Stop_IT(&htim2);

            f_sync(&fil);
            strncpy(upload_filename, current_filename,
                    sizeof(upload_filename));
            SD_CloseFile();

            snprintf((char*)test_status, sizeof(test_status),
                     "Upload: %s", upload_filename);
            A7670G_PowerUp();
            A7670G_UploadFile(upload_filename);
            A7670G_PowerDown();

            /* Open new file and resume */
            if (SD_OpenNewFile() != HAL_OK)
            {
                snprintf((char*)test_status, sizeof(test_status),
                         "FAIL: SD reopen");
                Error_Handler();
            }

            ecg_sample_count = 0;
            HAL_TIM_Base_Start_IT(&htim2);
            HAL_NVIC_EnableIRQ(EXTI0_IRQn);

            snprintf((char*)test_status, sizeof(test_status),
                     "Logging: %s", current_filename);
        }

    } /* while(1) */
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_LSE
                              |RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_MSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 40;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enable MSI Auto calibration
  */
  HAL_RCCEx_EnableMSIPLLMode();
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x00D09BE3;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief RTC Initialization Function
  * @param None
  * @retval None
  */
static void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN Check_RTC_BKUP */

  /* USER CODE END Check_RTC_BKUP */

  /** Initialize RTC and set the Time and Date
  */
  sTime.Hours = 0x0;
  sTime.Minutes = 0x0;
  sTime.Seconds = 0x0;
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  sDate.WeekDay = RTC_WEEKDAY_MONDAY;
  sDate.Month = RTC_MONTH_JANUARY;
  sDate.Date = 0x1;
  sDate.Year = 0x0;

  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */

  /* USER CODE END RTC_Init 2 */

}

/**
  * @brief SDMMC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SDMMC1_SD_Init(void)
{

  /* USER CODE BEGIN SDMMC1_Init 0 */

  /* USER CODE END SDMMC1_Init 0 */

  /* USER CODE BEGIN SDMMC1_Init 1 */

  /* USER CODE END SDMMC1_Init 1 */
  hsd1.Instance = SDMMC1;
  hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockBypass = SDMMC_CLOCK_BYPASS_DISABLE;
  hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd1.Init.BusWide = SDMMC_BUS_WIDE_4B;
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd1.Init.ClockDiv = 0;
  /* USER CODE BEGIN SDMMC1_Init 2 */

  /* USER CODE END SDMMC1_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_2EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 3999;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 399;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
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
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13|GPIO_PIN_4, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_8, GPIO_PIN_RESET);

  /*Configure GPIO pins : PC13 PC4 */
  GPIO_InitStruct.Pin = GPIO_PIN_13|GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : PA0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PA4 */
  GPIO_InitStruct.Pin = GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PB1 PB2 PB8 */
  GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PB5 PB9 */
  GPIO_InitStruct.Pin = GPIO_PIN_5|GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

/* USER CODE BEGIN MX_GPIO_Init_2 */
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
