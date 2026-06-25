/*
 * SampleMemory.c
 *
 *  Created on: 16.10.2013
 *      Author: Julian
 */


#include "SampleMemory.h"
#include "../Hardware/SD_FAT/SPI_routines.h"
#include "string.h"


//----- Vars ----------
//1st word is number of samples!
static uint16_t *sampleMemory_data 			= (uint16_t*)	SAMPLE_ROM_START_ADDRESS;
static SampleInfo* sampleMemory_infoData	= (SampleInfo*) SAMPLE_INFO_START_ADDRESS;
static SampleInfo sampleMemory_importInfo[SAMPLE_MAX_COUNT];

//----- functions -----
void sampleMemory_init()
{
	FLASH_If_Init();

	//disable write protection if enabled
	if(FLASH_If_GetWriteProtectionStatus())
	{
		FLASH_If_DisableWriteProtection();
	}


#if USE_SD_CARD
	sdManager_init();
#endif
}
//---------------------------------------------------------------
uint32_t sampleMemory_getSampleSizeFrames(SampleInfo info)
{
   return info.size & SAMPLE_INFO_SIZE_MASK;
}

uint8_t sampleMemory_isSampleLooped(SampleInfo info)
{
   return (uint8_t)((info.size & SAMPLE_INFO_LOOP_FLAG) != 0u);
}

SampleInfo sampleMemory_makeSampleInfo(const char* name,
                                       uint32_t sizeFrames,
                                       uint32_t offset,
                                       uint8_t looped)
{
   SampleInfo info;

   memset(&info, 0, sizeof(info));
   memcpy(info.name, name, 3);
   info.size = sizeFrames & SAMPLE_INFO_SIZE_MASK;
   if(looped)
      info.size |= SAMPLE_INFO_LOOP_FLAG;
   info.offset = offset;

   return info;
}
//---------------------------------------------------------------
SampleInfo sampleMemory_getSampleInfo(uint8_t index)
{
	return sampleMemory_infoData[index];
}
//---------------------------------------------------------------
uint8_t sampleMemory_getNumSamples()
{
	uint16_t stored = sampleMemory_data[0];

	if(stored == 0xffffu || stored > SAMPLE_MAX_COUNT)
	{
		return 0;
	}

	return (uint8_t)stored;
}
//---------------------------------------------------------------
uint32_t sampleMemory_setNumSamples(uint8_t num)
{
//	sampleMemory_data[0] = num;
	uint32_t data = num;
	volatile uint32_t add = SAMPLE_ROM_START_ADDRESS; //*4 because we write uint32
	return FLASH_If_Write(&add, (uint32_t*)&data, 1);
}
//---------------------------------------------------------------
static uint32_t sampleMemory_getPcmCapacityWords(void)
{
	const uint32_t pcmStart = SAMPLE_ROM_START_ADDRESS + 4u;
	return (SAMPLE_INFO_START_ADDRESS - pcmStart) / 4u;
}
//---------------------------------------------------------------
static uint32_t sampleMemory_bytesToFlashWords(uint32_t byteCount)
{
	return (byteCount + 3u) / 4u;
}
//---------------------------------------------------------------
#define BLOCKSIZE 2
uint8_t sampleMemory_loadSamples(void)
{
	SampleInfo *info = sampleMemory_importInfo;
	uint32_t addr = 0;
	uint8_t loadedSamples = 0;
	uint8_t statusFlags = SAMPLE_UPLOAD_STATUS_OK;
	uint16_t i;
	volatile uint32_t add;
	uint32_t infoWords;
	uint32_t len;
	uint32_t j;
	char* name;

	sdManager_countLoopFolder();
	uint16_t totalFound = sd_getNumSamples();
	uint16_t oneShotCount = sd_getNumOneShotSamples();
	const uint32_t pcmCapacityWords = sampleMemory_getPcmCapacityWords();

	if(totalFound == 0u)
	{
		spi_deInit();
		return statusFlags;
	}

	if(totalFound > SAMPLE_MAX_COUNT)
	{
		statusFlags |= SAMPLE_UPLOAD_STATUS_COUNT_LIMIT;
	}

	//erase user memory flash
	if(FLASH_If_Erase(SAMPLE_ROM_START_ADDRESS) != FLASH_IF_OK)
	{
		spi_deInit();
		return statusFlags;
	}

	//reserve space for sample info header
	memset(info, 0, sizeof(sampleMemory_importInfo));

	for(i = 0u; i < totalFound; i++)
	{
		if(loadedSamples >= SAMPLE_MAX_COUNT)
		{
			break;
		}

		sd_setActiveSample(i);
		len 	= sd_getActiveSampleLength();
		name = sd_getActiveSampleName();

		uint32_t sampleWords = sampleMemory_bytesToFlashWords(len);
		if(sampleWords > pcmCapacityWords || addr > (pcmCapacityWords - sampleWords))
		{
			statusFlags |= SAMPLE_UPLOAD_STATUS_FLASH_LIMIT;
			continue;
		}
		
		uint8_t looped = (i >= oneShotCount) ? 1u : 0u;

		info[loadedSamples] = sampleMemory_makeSampleInfo(name,
                                             len / 2u,
                                             SAMPLE_ROM_START_ADDRESS + 4u + addr * 4u,
                                             looped);

		for(j = 0u; j < len; )
		{
			int16_t data[BLOCKSIZE];
//			uint16_t bytesRead;
//			bytesRead = sd_readSampleData(data, BLOCKSIZE);
			sd_readSampleData(data, BLOCKSIZE);
			add = 4u + SAMPLE_ROM_START_ADDRESS + 4u * addr; //*4 because we write uint32

			if(FLASH_If_WriteSamplePcm(&add, (uint32_t*)data, BLOCKSIZE / 2u) != FLASH_IF_OK)
			{
				spi_deInit();
				return statusFlags;
			}

			j += BLOCKSIZE * 2u;
			addr += BLOCKSIZE / 2u;

		}

		loadedSamples++;
	}
	//write info header
	add = SAMPLE_INFO_START_ADDRESS ;
	infoWords = (loadedSamples * sizeof(SampleInfo) + 3u) / 4u;
	if(FLASH_If_Write(&add, (uint32_t*)(info), infoWords) != FLASH_IF_OK)
	{
		spi_deInit();
		return statusFlags;
	}

	if(sampleMemory_setNumSamples(loadedSamples) != FLASH_IF_OK)
	{
		spi_deInit();
		return statusFlags;
	}

	spi_deInit();
	return statusFlags;

}
//---------------------------------------------------------------
