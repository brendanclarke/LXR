/**
  ******************************************************************************
  * @file    STM32F4xx_IAP/src/flash_if.c 
  * @author  MCD Application Team
  * @version V1.0.0
  * @date    10-October-2011
  * @brief   This file provides all the memory related operation functions.
  ******************************************************************************
  * @attention
  *
  * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
  * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
  * TIME. AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY
  * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
  * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
  * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
  *
  * <h2><center>&copy; COPYRIGHT 2011 STMicroelectronics</center></h2>
  ******************************************************************************
  */ 

/** @addtogroup STM32F4xx_IAP
  * @{
  */

/* Includes ------------------------------------------------------------------*/
#include "flash_if.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
#define FLASH_IF_SAMPLE_WRP_MASK (OB_WRP_Sector_8 | OB_WRP_Sector_9 | \
                                  OB_WRP_Sector_10 | OB_WRP_Sector_11)
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
static uint32_t GetSector(uint32_t Address);
static uint8_t FLASH_If_IsWordAligned(uint32_t address);
static uint8_t FLASH_If_RangeIsValid(uint32_t address, uint32_t words, uint32_t limit);

/* Private functions ---------------------------------------------------------*/

static uint8_t FLASH_If_IsWordAligned(uint32_t address)
{
  return (uint8_t)((address & 0x03u) == 0u);
}

static uint8_t FLASH_If_RangeIsValid(uint32_t address, uint32_t words, uint32_t limit)
{
  uint32_t bytes;

  if(words == 0u)
  {
    return 1u;
  }

  if(!FLASH_If_IsWordAligned(address))
  {
    return 0u;
  }

  if(address < FLASH_IF_SAMPLE_START_ADDRESS || address > limit)
  {
    return 0u;
  }

  if(words > 0x3fffffffu)
  {
    return 0u;
  }

  bytes = words * 4u;

  if(bytes > (limit - address + 1u))
  {
    return 0u;
  }

  return 1u;
}

/**
  * @brief  Unlocks Flash for write access
  * @param  None
  * @retval None
  */
void FLASH_If_Init(void)
{ 

	/* Unlock only long enough to clear stale status, then leave flash locked.
	   Erase/write helpers manage their own unlock windows. */
	FLASH_Unlock();
	/* Clear pending flags (if any) */
	FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
				  FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR|FLASH_FLAG_PGSERR);

	FLASH_Lock();
}

/**
  * @brief  This function does an erase of all user flash area
  * @param  StartSector: start of user flash area
  * @retval 0: user flash area successfully erased
  *         1: error occurred
  */
uint32_t FLASH_If_Erase(uint32_t startAddress)
{
  uint32_t UserStartSector = FLASH_Sector_1;
  uint32_t i = 0;

  if(startAddress != FLASH_IF_SAMPLE_START_ADDRESS)
  {
    return FLASH_IF_ERR_BOUNDS;
  }

  FLASH_Unlock();
  FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                  FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

  /* Get the sector where start the user flash area */
  UserStartSector = GetSector(startAddress);

  for(i = UserStartSector; i <= FLASH_Sector_11; i += 8)
  {
    /* Device voltage range supposed to be [2.7V to 3.6V], the operation will
       be done by word */
    FLASH_Status error = FLASH_EraseSector(i, VoltageRange_3);
    if (error != FLASH_COMPLETE)
    {
      /* Error occurred while page erase */
      FLASH_Lock();
      return FLASH_IF_ERR_OPERATION;
    }
  }
  FLASH_Lock();
  return FLASH_IF_OK;
}

/**
  * @brief  This function writes a data buffer in flash (data are 32-bit aligned).
  * @note   After writing data buffer, the flash content is checked.
  * @param  FlashAddress: start address for writing data buffer
  * @param  Data: pointer on data buffer
  * @param  DataLength: length of data buffer (unit is 32-bit word)   
  * @retval 0: Data successfully written to Flash memory
  *         1: Error occurred while writing data in Flash memory
  *         2: Written Data in flash memory is different from expected one
  */
uint32_t FLASH_If_Write(__IO uint32_t* FlashAddress, uint32_t* Data ,uint32_t DataLength)
{
  uint32_t i = 0;

  if(FlashAddress == 0 || Data == 0)
  {
    return FLASH_IF_ERR_OPERATION;
  }

  if(!FLASH_If_IsWordAligned(*FlashAddress))
  {
    return FLASH_IF_ERR_ALIGNMENT;
  }

  if(!FLASH_If_RangeIsValid(*FlashAddress, DataLength, FLASH_IF_SAMPLE_END_ADDRESS))
  {
    return FLASH_IF_ERR_BOUNDS;
  }

  FLASH_Unlock();
  FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                  FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

  for (i = 0; i < DataLength; i++)
  {
    /* Device voltage range supposed to be [2.7V to 3.6V], the operation will
       be done by word */ 
	  FLASH_Status error = FLASH_ProgramWord(*FlashAddress, Data[i]) ;
    if (error == FLASH_COMPLETE)
    {
     /* Check the written value */
      if (*(uint32_t*)*FlashAddress != Data[i])
      {
        /* Flash content doesn't match SRAM content */
        FLASH_Lock();
        return FLASH_IF_ERR_VERIFY;
      }
      /* Increment FLASH destination address */
      *FlashAddress += 4u;
    }
    else
    {
      /* Error occurred while writing data in Flash memory */
      FLASH_Lock();
      return FLASH_IF_ERR_OPERATION;
    }
  }
  FLASH_Lock();
  return FLASH_IF_OK;
}

uint32_t FLASH_If_WriteSamplePcm(__IO uint32_t* FlashAddress,
                                 uint32_t* Data,
                                 uint32_t DataLength)
{
  if(FlashAddress == 0)
  {
    return FLASH_IF_ERR_OPERATION;
  }

  if(!FLASH_If_IsWordAligned(*FlashAddress))
  {
    return FLASH_IF_ERR_ALIGNMENT;
  }

  if(!FLASH_If_RangeIsValid(*FlashAddress,
                            DataLength,
                            FLASH_IF_SAMPLE_INFO_START_ADDRESS - 1u))
  {
    return FLASH_IF_ERR_BOUNDS;
  }

  return FLASH_If_Write(FlashAddress, Data, DataLength);
}

/**
  * @brief  Returns the write protection status of user flash area.
  * @param  None
  * @retval 0: No write protected sectors inside the user flash area
  *         1: Some sectors inside the user flash area are write protected
  */
uint16_t FLASH_If_GetWriteProtectionStatus(void)
{
  uint16_t sampleWrp = FLASH_OB_GetWRP() & FLASH_IF_SAMPLE_WRP_MASK;

  /* In STM32F4 option bytes, a set WRP bit means the sector is writable. */
  if(sampleWrp == FLASH_IF_SAMPLE_WRP_MASK)
  {
    return 0;
  }

  return 1;
}

/**
  * @brief  Disables the write protection of user flash area.
  * @param  None
  * @retval 1: Write Protection successfully disabled
  *         2: Error: Flash write unprotection failed
  */
uint32_t FLASH_If_DisableWriteProtection(void)
{
  /* Unlock the Option Bytes */
  FLASH_OB_Unlock();

  /* Disable write protection only for the sample sectors. */
  FLASH_OB_WRPConfig(FLASH_IF_SAMPLE_WRP_MASK, DISABLE);

  /* Start the Option Bytes programming process. */  
  if (FLASH_OB_Launch() != FLASH_COMPLETE)
  {
    /* Error: Flash write unprotection failed */
    FLASH_OB_Lock();
    return (2);
  }

  FLASH_OB_Lock();

  /* Write Protection successfully disabled */
  return (1);
}

/**
  * @brief  Gets the sector of a given address
  * @param  Address: Flash address
  * @retval The sector of a given address
  */
static uint32_t GetSector(uint32_t Address)
{
  uint32_t sector = 0;
  
  if((Address < ADDR_FLASH_SECTOR_1) && (Address >= ADDR_FLASH_SECTOR_0))
  {
    sector = FLASH_Sector_0;  
  }
  else if((Address < ADDR_FLASH_SECTOR_2) && (Address >= ADDR_FLASH_SECTOR_1))
  {
    sector = FLASH_Sector_1;  
  }
  else if((Address < ADDR_FLASH_SECTOR_3) && (Address >= ADDR_FLASH_SECTOR_2))
  {
    sector = FLASH_Sector_2;  
  }
  else if((Address < ADDR_FLASH_SECTOR_4) && (Address >= ADDR_FLASH_SECTOR_3))
  {
    sector = FLASH_Sector_3;  
  }
  else if((Address < ADDR_FLASH_SECTOR_5) && (Address >= ADDR_FLASH_SECTOR_4))
  {
    sector = FLASH_Sector_4;  
  }
  else if((Address < ADDR_FLASH_SECTOR_6) && (Address >= ADDR_FLASH_SECTOR_5))
  {
    sector = FLASH_Sector_5;  
  }
  else if((Address < ADDR_FLASH_SECTOR_7) && (Address >= ADDR_FLASH_SECTOR_6))
  {
    sector = FLASH_Sector_6;  
  }
  else if((Address < ADDR_FLASH_SECTOR_8) && (Address >= ADDR_FLASH_SECTOR_7))
  {
    sector = FLASH_Sector_7;  
  }
  else if((Address < ADDR_FLASH_SECTOR_9) && (Address >= ADDR_FLASH_SECTOR_8))
  {
    sector = FLASH_Sector_8;  
  }
  else if((Address < ADDR_FLASH_SECTOR_10) && (Address >= ADDR_FLASH_SECTOR_9))
  {
    sector = FLASH_Sector_9;  
  }
  else if((Address < ADDR_FLASH_SECTOR_11) && (Address >= ADDR_FLASH_SECTOR_10))
  {
    sector = FLASH_Sector_10;  
  }
  else/*(Address < FLASH_END_ADDR) && (Address >= ADDR_FLASH_SECTOR_11))*/
  {
    sector = FLASH_Sector_11;  
  }
    return sector;
}

/**
  * @}
  */

/******************* (C) COPYRIGHT 2011 STMicroelectronics *****END OF FILE****/
