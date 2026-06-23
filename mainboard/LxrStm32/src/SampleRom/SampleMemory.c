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
#define BLOCKSIZE 2
void sampleMemory_loadSamples()
{
	uint8_t numSamples = sd_getNumSamples();
	SampleInfo info[SAMPLE_MAX_COUNT];
	uint32_t addr = 0;
	uint8_t i;
	volatile uint32_t add;
	uint32_t infoWords;
	uint32_t len;
	uint32_t j;
	char* name;


	if(numSamples==0 || numSamples > SAMPLE_MAX_COUNT)
	{
		spi_deInit();
		return;
	}

	//erase user memory flash
	if(FLASH_If_Erase(SAMPLE_ROM_START_ADDRESS) != FLASH_IF_OK)
	{
		spi_deInit();
		return;
	}

	//reserve space for sample info header
	memset(info, 0, sizeof(info));

	for(i=0;i<numSamples;i++)
	{
		sd_setActiveSample(i);
		len 	= sd_getActiveSampleLength();
		name = sd_getActiveSampleName();
		memcpy(info[i].name, name,3);
		info[i].offset 	= SAMPLE_ROM_START_ADDRESS + 4 + addr*4; //+1 because of num_samples
		info[i].size 	= len/2;

		for(j=0;j<len;)
		{
			int16_t data[BLOCKSIZE];
//			uint16_t bytesRead;
//			bytesRead = sd_readSampleData(data, BLOCKSIZE);
			sd_readSampleData(data, BLOCKSIZE);
			add = 4+SAMPLE_ROM_START_ADDRESS + 4*addr; //*4 because we write uint32

			if(FLASH_If_WriteSamplePcm(&add, (uint32_t*)data, BLOCKSIZE/2) != FLASH_IF_OK)
			{
				spi_deInit();
				return;
			}

			j += BLOCKSIZE*2;
			addr += BLOCKSIZE/2;

		}
	}
	//write info header
	add = SAMPLE_INFO_START_ADDRESS ;
	infoWords = (numSamples * sizeof(SampleInfo) + 3u) / 4u;
	if(FLASH_If_Write(&add, (uint32_t*)(info), infoWords) != FLASH_IF_OK)
	{
		spi_deInit();
		return;
	}

	if(sampleMemory_setNumSamples(numSamples) != FLASH_IF_OK)
	{
		spi_deInit();
		return;
	}

	spi_deInit();

}
//---------------------------------------------------------------
