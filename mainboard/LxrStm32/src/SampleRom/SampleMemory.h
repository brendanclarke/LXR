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
#define SAMPLE_MAX_COUNT                ((uint8_t)248) /* AVR waveform menu exposes s0..s247 only. */

typedef struct SampleInfoStruct
{
	char name[3];
	uint32_t size;		/* bit 31 loop flag, bits 30..0 length in int16 frames */
	uint32_t offset;	/* absolute STM32 internal flash address */
} SampleInfo;

/* Phase 2 reuses SampleInfo.size bit 31 as the loop flag without expanding
   the on-flash ABI. The lower 31 bits remain the int16 frame count. */
#define SAMPLE_INFO_LOOP_FLAG ((uint32_t)0x80000000u)
#define SAMPLE_INFO_SIZE_MASK ((uint32_t)0x7fffffffu)
/* The table writer deals in flash words. These constants document the expected
   padded STM32 layout and are checked at compile time in SampleMemory.c. */
#define SAMPLE_INFO_FLASH_WORDS ((uint32_t)3u)
#define SAMPLE_INFO_FLASH_BYTES (SAMPLE_INFO_FLASH_WORDS * 4u)

/* SAMPLE_UPLOAD_STATUS_* bits are sent STM->AVR in SAMPLE_UPLOAD_RESULT.
   OK means no warning bits set; non-zero bits are warnings/errors displayed by
   the AVR menu after the blocking import returns. */
#define SAMPLE_UPLOAD_STATUS_OK          ((uint8_t)0x00u)
#define SAMPLE_UPLOAD_STATUS_COUNT_LIMIT ((uint8_t)0x01u)
#define SAMPLE_UPLOAD_STATUS_FLASH_LIMIT ((uint8_t)0x02u)
#define SAMPLE_UPLOAD_STATUS_READ_ERROR  ((uint8_t)0x04u)


//--------------------------------------
void sampleMemory_init();
/* Imports /samples first and /loops second from the SD card into STM32
   internal flash. Output is SAMPLE_UPLOAD_STATUS_* flags; on success the
   committed sample count at SAMPLE_ROM_START_ADDRESS is updated last. */
uint8_t sampleMemory_loadSamples(void);
SampleInfo sampleMemory_getSampleInfo(uint8_t index);
uint8_t sampleMemory_getNumSamples();
uint32_t sampleMemory_setNumSamples(uint8_t num);
/* Inputs: on-flash SampleInfo. Outputs: decoded frame count / loop flag. */
uint32_t sampleMemory_getSampleSizeFrames(SampleInfo info);
uint8_t sampleMemory_isSampleLooped(SampleInfo info);
/* Builds a zero-padded SampleInfo for flash. Inputs are 8.3 name, int16 frame
   length, absolute PCM offset, and loop flag; output is the packed metadata. */
SampleInfo sampleMemory_makeSampleInfo(const char* name,
                                       uint32_t sizeFrames,
                                       uint32_t offset,
                                       uint8_t looped);


#endif /* SAMPLEMEMORY_H_ */
