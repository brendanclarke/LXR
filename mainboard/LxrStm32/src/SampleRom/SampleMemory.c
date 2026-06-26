/*
 * SampleMemory.c
 *
 *  Created on: 16.10.2013
 *      Author: Julian
 */


#include "SampleMemory.h"
#include "../Hardware/SD_FAT/SPI_routines.h"
#include "../uARTFrontSYX/frontPanelSendingProtocol.h"
#include "string.h"


//----- Vars ----------
//1st word is number of samples!
static uint16_t *sampleMemory_data 			= (uint16_t*)	SAMPLE_ROM_START_ADDRESS;
static SampleInfo* sampleMemory_infoData	= (SampleInfo*) SAMPLE_INFO_START_ADDRESS;
/* Import metadata is staged in RAM and written only after all accepted PCM has
   been flashed. This keeps the visible sample count dense and prevents failed
   candidates from creating silent s1/s3/etc. slots. */
static SampleInfo sampleMemory_importInfo[SAMPLE_MAX_COUNT];
/* Compile-time guard for the on-flash ABI: name[3] + pad + size + offset must
   stay 12 bytes. If a compiler/struct edit changes this, the metadata writer
   must be audited before the firmware can build. */
typedef char sampleMemory_sampleInfoMustBe12Bytes[(sizeof(SampleInfo) == SAMPLE_INFO_FLASH_BYTES) ? 1 : -1];

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
   /* Output strips the loop marker from the packed size field. */
   return info.size & SAMPLE_INFO_SIZE_MASK;
}

uint8_t sampleMemory_isSampleLooped(SampleInfo info)
{
   /* Output is 1 when this SampleInfo came from /loops, 0 for /samples. */
   return (uint8_t)((info.size & SAMPLE_INFO_LOOP_FLAG) != 0u);
}

SampleInfo sampleMemory_makeSampleInfo(const char* name,
                                       uint32_t sizeFrames,
                                       uint32_t offset,
                                       uint8_t looped)
{
   SampleInfo info;

   /* Zero first so the padded byte after name[3] is deterministic in flash.
      That avoids stale padding bytes becoming part of future metadata diffs or
      compare/verify diagnostics. */
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
	/* Output is the number of 32-bit flash words available for PCM after the
	   committed count word and before the SampleInfo metadata table. */
	return (SAMPLE_INFO_START_ADDRESS - pcmStart) / 4u;
}
//---------------------------------------------------------------
static uint32_t sampleMemory_bytesToFlashWords(uint32_t byteCount)
{
	/* Input is a WAV data-chunk byte count. Output rounds up to the number of
	   32-bit flash words needed because STM32 flash writes are word-sized. */
	return (byteCount + 3u) / 4u;
}
//---------------------------------------------------------------
#define BLOCKSIZE 2
/* Blocking sample import entry point.
   Interdependencies:
   - SD_Manager owns directory iteration and the currently open WAV data chunk.
   - flash_if owns internal-flash erase/write bounds for sectors 8..11.
   - frontPanelSendingProtocol emits progress/result packets so the AVR menu
     can parse real protocol messages instead of waiting for a raw ACK byte.
   Safety/risk notes:
   - seq_setRunning(0) is called by the receive protocol before entry because
     internal flash erase/programming stalls normal execution.
   - The AVR releases the SD SPI bus before requesting import; this STM path
     deinitializes SPI again on every exit.
   - Metadata and the committed count are written last so partially failed
     candidates cannot appear as selectable silent slots. */
uint8_t sampleMemory_loadSamples(void)
{
	SampleInfo *info = sampleMemory_importInfo;
	uint32_t addr = 0;
	uint8_t loadedSamples = 0;
	uint8_t statusFlags = SAMPLE_UPLOAD_STATUS_OK;
	uint8_t folder;
	volatile uint32_t add;
	uint32_t infoWords;
	uint32_t len;
	uint32_t j;
	char* name;
	uint8_t sampleProgress = 0u;
	uint8_t loopProgress = 0u;

	sdManager_countLoopFolder();
	uint16_t totalFound = sd_getNumSamples();
	const uint32_t pcmCapacityWords = sampleMemory_getPcmCapacityWords();

	/* No samples is not an error: leave the old erased/import state alone and
	   return OK so the AVR does not show a false hardware failure. */
	if(totalFound == 0u)
	{
		spi_deInit();
		return statusFlags;
	}

	if(totalFound > SAMPLE_MAX_COUNT)
	{
		/* Keep importing the first 248 playable WAVs, but report that the menu
		   range was exceeded. The AVR cannot select beyond s247. */
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

	for(folder = 0u; folder < 2u; folder++)
	{
		uint8_t looped = folder;

		if(loadedSamples >= SAMPLE_MAX_COUNT)
		{
			break;
		}

		/* One pass per folder prevents the previous O(n^2) indexed rescans
		   where each later sample took longer than the last. */
		if(!sd_openSampleFolder(looped))
		{
			continue;
		}

		while(loadedSamples < SAMPLE_MAX_COUNT && sd_openNextSampleInFolder())
		{
			len 	= sd_getActiveSampleLength();
			/* Require complete int16 PCM frames. Odd or empty data chunks are
			   skipped so they do not consume a waveform slot. */
			if(len < 2u || (len & 1u) != 0u)
			{
				sd_closeActiveSample();
				statusFlags |= SAMPLE_UPLOAD_STATUS_READ_ERROR;
				continue;
			}

			name = sd_getActiveSampleName();

			uint32_t sampleWords = sampleMemory_bytesToFlashWords(len);
			if(sampleWords > pcmCapacityWords || addr > (pcmCapacityWords - sampleWords))
			{
				/* Skip this candidate and keep the dense slot index unchanged.
				   The result flag tells the AVR that at least one file did not
				   fit into the reserved STM32 internal flash sample area. */
				sd_closeActiveSample();
				statusFlags |= SAMPLE_UPLOAD_STATUS_FLASH_LIMIT;
				continue;
			}

			uint32_t sampleStartWord = addr;
			uint8_t progressIndex;

			if(looped)
			{
				loopProgress++;
				progressIndex = loopProgress;
			}
			else
			{
				sampleProgress++;
				progressIndex = sampleProgress;
			}

			frontPanelSending_sendSampleUploadProgress(looped, progressIndex);

			for(j = 0u; j < len; )
			{
				int16_t data[BLOCKSIZE];
				uint16_t framesToRead;
				uint16_t bytesRead;
				uint32_t wordsRead;

				memset(data, 0, sizeof(data));
				/* Read two int16 frames at a time except for a final single
				   frame. Zero-fill before reading so an odd 32-bit flash word
				   has deterministic padding in the unused halfword. */
				framesToRead = ((len - j) >= (uint32_t)(BLOCKSIZE * 2u)) ? BLOCKSIZE : 1u;
				bytesRead = sd_readSampleData(data, framesToRead);
				if(bytesRead == 0u || (bytesRead & 1u) != 0u || (j + bytesRead) > len)
				{
					sd_closeActiveSample();
					statusFlags |= SAMPLE_UPLOAD_STATUS_READ_ERROR;
					spi_deInit();
					return statusFlags;
				}

				if(bytesRead < (uint16_t)(framesToRead * 2u) && (j + bytesRead) < len)
				{
					sd_closeActiveSample();
					statusFlags |= SAMPLE_UPLOAD_STATUS_READ_ERROR;
					spi_deInit();
					return statusFlags;
				}

				wordsRead = (((uint32_t)bytesRead / 2u) + 1u) / 2u;
				add = 4u + SAMPLE_ROM_START_ADDRESS + 4u * addr; //*4 because we write uint32

				/* FLASH_If_WriteSamplePcm enforces the PCM/metadata boundary;
				   using the wrapper here is the guard against corrupting the
				   SampleInfo table when importing large files. */
				if(FLASH_If_WriteSamplePcm(&add, (uint32_t*)data, wordsRead) != FLASH_IF_OK)
				{
					sd_closeActiveSample();
					spi_deInit();
					return statusFlags;
				}

				j += bytesRead;
				addr += wordsRead;

			}

			sd_closeActiveSample();

			/* Commit metadata only after the entire PCM payload was written.
			   This is what makes s0,s1,s2... dense even when earlier directory
			   entries were invalid or too large. */
			info[loadedSamples] = sampleMemory_makeSampleInfo(name,
	                                             len / 2u,
	                                             SAMPLE_ROM_START_ADDRESS + 4u + sampleStartWord * 4u,
	                                             looped);

			loadedSamples++;
		}
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
