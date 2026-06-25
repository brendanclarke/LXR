/*
 * SampleMemory.h
 *
 *  Created on: 16.10.2013
 *      Author: Julian
 */

#ifndef SAMPLEMEMORY_H_
#define SAMPLEMEMORY_H_

#include "stm32f4xx.h"
#include "flash_if.h"
#include "config.h"
#if USE_SD_CARD
#include "SD_Manager.h"
#endif
//flash region = 0x08000000 to 0x080FA000 (1024k)
//bootloader 0x08000000 to 0x08004000
//the main programm starts at 0x08004000
//if we reserve 512k for the main programm we get
// 0x08004000 + 512k = 0x08004000 + 0x0007D000 = 0x08081000
//page size is 0x4000 so we have to set the limit at 0x08080000 = ~524kb
//this results in a sample rom size of
// 0x080FA000 - 0x08080000 = 0x0007A000 = ~499kb
#define SAMPLE_ROM_START_ADDRESS 	((uint32_t)0x08080000)
#define SAMPLE_PAGE_SIZE               	(uint32_t)0x20000  		/* Page size = 128KByte */

// Sample flash is split into:
//   [SAMPLE_ROM_START_ADDRESS]          32-bit committed sample count
//   [SAMPLE_ROM_START_ADDRESS + 4 ...]  packed PCM data
//   [SAMPLE_INFO_START_ADDRESS ...]     SampleInfo metadata table
//
// SampleInfo is compiler-padded to 12 bytes on STM32:
//   char name[3] + 1 byte padding + uint32_t size + uint32_t offset.
// 248 entries require 2976 bytes (0xBA0), so reserve 0xC00 bytes.
#define SAMPLE_INFO_SIZE                ((uint32_t)0x00000C00u)
#define SAMPLE_INFO_START_ADDRESS       ((uint32_t)0x080F9400)
#define SAMPLE_ROM_SIZE                 ((uint32_t)(SAMPLE_INFO_START_ADDRESS - SAMPLE_ROM_START_ADDRESS))
#define SAMPLE_MAX_COUNT                ((uint8_t)248)

typedef struct SampleInfoStruct
{
	char name[3];
	uint32_t size;		/* bit 31 loop flag, bits 30..0 length in int16 frames */
	uint32_t offset;	/* absolute STM32 internal flash address */
} SampleInfo;

#define SAMPLE_INFO_LOOP_FLAG ((uint32_t)0x80000000u)
#define SAMPLE_INFO_SIZE_MASK ((uint32_t)0x7fffffffu)

#define SAMPLE_UPLOAD_STATUS_OK          ((uint8_t)0x00u)
#define SAMPLE_UPLOAD_STATUS_COUNT_LIMIT ((uint8_t)0x01u)
#define SAMPLE_UPLOAD_STATUS_FLASH_LIMIT ((uint8_t)0x02u)


//--------------------------------------
void sampleMemory_init();
uint8_t sampleMemory_loadSamples(void);
SampleInfo sampleMemory_getSampleInfo(uint8_t index);
uint8_t sampleMemory_getNumSamples();
uint32_t sampleMemory_setNumSamples(uint8_t num);
uint32_t sampleMemory_getSampleSizeFrames(SampleInfo info);
uint8_t sampleMemory_isSampleLooped(SampleInfo info);
SampleInfo sampleMemory_makeSampleInfo(const char* name,
                                       uint32_t sizeFrames,
                                       uint32_t offset,
                                       uint8_t looped);


#endif /* SAMPLEMEMORY_H_ */
