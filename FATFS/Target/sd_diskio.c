/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sd_diskio.c
  * @brief   SD Disk I/O driver
  ******************************************************************************
  */
/* USER CODE END Header */

/* USER CODE BEGIN firstSection */
#include "main.h"
extern SD_HandleTypeDef hsd1;
/* USER CODE END firstSection */

/* Includes ------------------------------------------------------------------*/
#include "ff_gen_drv.h"
#include "sd_diskio.h"

/* Private define ------------------------------------------------------------*/
#if defined(SDMMC_DATATIMEOUT)
#define SD_TIMEOUT SDMMC_DATATIMEOUT
#elif defined(SD_DATATIMEOUT)
#define SD_TIMEOUT SD_DATATIMEOUT
#else
#define SD_TIMEOUT 30 * 1000
#endif

#define SD_DEFAULT_BLOCK_SIZE 512
#define SD_TRANSFER_WAIT_TIMEOUT 5000U

/* Private variables ---------------------------------------------------------*/
static volatile DSTATUS Stat = STA_NOINIT;

/* Live Expressions: physical SD recovery diagnostics. */
volatile uint32_t sd_driver_timeouts = 0U;
volatile uint32_t sd_driver_recoveries = 0U;
volatile uint32_t sd_driver_recovery_failures = 0U;

/* Private function prototypes -----------------------------------------------*/
static DSTATUS SD_CheckStatus(BYTE lun);
static uint8_t SD_WaitForTransfer(void);
static uint8_t SD_RecoverCard(void);
DSTATUS SD_initialize(BYTE);
DSTATUS SD_status(BYTE);
DRESULT SD_read(BYTE, BYTE*, DWORD, UINT);
#if _USE_WRITE == 1
DRESULT SD_write(BYTE, const BYTE*, DWORD, UINT);
#endif
#if _USE_IOCTL == 1
DRESULT SD_ioctl(BYTE, BYTE, void*);
#endif

const Diskio_drvTypeDef SD_Driver =
{
  SD_initialize,
  SD_status,
  SD_read,
#if _USE_WRITE == 1
  SD_write,
#endif
#if _USE_IOCTL == 1
  SD_ioctl,
#endif
};

/* Private functions ---------------------------------------------------------*/

static DSTATUS SD_CheckStatus(BYTE lun)
{
  Stat = STA_NOINIT;
  if (HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER)
  {
    Stat &= ~STA_NOINIT;
  }
  return Stat;
}

static uint8_t SD_WaitForTransfer(void)
{
  uint32_t start = HAL_GetTick();
  while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
  {
    if ((HAL_GetTick() - start) >= SD_TRANSFER_WAIT_TIMEOUT)
    {
      sd_driver_timeouts++;
      if (SD_RecoverCard()) sd_driver_recoveries++;
      else sd_driver_recovery_failures++;
      return 0U;
    }
  }
  return 1U;
}

static uint8_t SD_RecoverCard(void)
{
  (void)HAL_SD_Abort(&hsd1);
  (void)HAL_SD_DeInit(&hsd1);
  HAL_Delay(10U);

  if (HAL_SD_Init(&hsd1) != HAL_OK) return 0U;
  if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK)
    return 0U;

  uint32_t start = HAL_GetTick();
  while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
  {
    if ((HAL_GetTick() - start) >= 1000U) return 0U;
  }
  Stat &= (DSTATUS)~STA_NOINIT;
  return 1U;
}

DSTATUS SD_initialize(BYTE lun)
{
  Stat = STA_NOINIT;
  if (HAL_SD_Init(&hsd1) == HAL_OK)
  {
    Stat = SD_CheckStatus(lun);
  }
  return Stat;
}

DSTATUS SD_status(BYTE lun)
{
  return SD_CheckStatus(lun);
}

DRESULT SD_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
  DRESULT res = RES_ERROR;
  if (HAL_SD_ReadBlocks(&hsd1, (uint8_t*)buff, sector, count, SD_TIMEOUT) == HAL_OK)
  {
    if (SD_WaitForTransfer()) res = RES_OK;
  }
  return res;
}

#if _USE_WRITE == 1
DRESULT SD_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
  DRESULT res = RES_ERROR;
  if (HAL_SD_WriteBlocks(&hsd1, (uint8_t*)buff, sector, count, SD_TIMEOUT) == HAL_OK)
  {
    if (SD_WaitForTransfer()) res = RES_OK;
  }
  return res;
}
#endif

#if _USE_IOCTL == 1
DRESULT SD_ioctl(BYTE lun, BYTE cmd, void *buff)
{
  DRESULT res = RES_ERROR;
  HAL_SD_CardInfoTypeDef CardInfo;

  if (Stat & STA_NOINIT) return RES_NOTRDY;

  switch (cmd)
  {
  case CTRL_SYNC:
    res = RES_OK;
    break;
  case GET_SECTOR_COUNT:
    HAL_SD_GetCardInfo(&hsd1, &CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockNbr;
    res = RES_OK;
    break;
  case GET_SECTOR_SIZE:
    HAL_SD_GetCardInfo(&hsd1, &CardInfo);
    *(WORD*)buff = CardInfo.LogBlockSize;
    res = RES_OK;
    break;
  case GET_BLOCK_SIZE:
    HAL_SD_GetCardInfo(&hsd1, &CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockSize / SD_DEFAULT_BLOCK_SIZE;
    res = RES_OK;
    break;
  default:
    res = RES_PARERR;
  }
  return res;
}
#endif
