/*
 * SD_Manager.c
 *
 *  Created on: 24.10.2012
 * ------------------------------------------------------------------------------------------------------------------------
 *  Copyright 2013 Julian Schmidt
 *  Julian@sonic-potions.com
 * ------------------------------------------------------------------------------------------------------------------------
 *  This file is part of the Sonic Potions LXR drumsynth firmware.
 * ------------------------------------------------------------------------------------------------------------------------
 *  Redistribution and use of the LXR code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *       - The code may not be sold, nor may it be used in a commercial product or activity.
 *
 *       - Redistributions that are modified from the original source must include the complete
 *         source code, including the source code for all components used by a binary built
 *         from the modified sources. However, as a special exception, the source code distributed
 *         need not include anything that is normally distributed (in either source or binary form)
 *         with the major components (compiler, kernel, and so on) of the operating system on which
 *         the executable runs, unless that component itself accompanies the executable.
 *
 *       - Redistributions must reproduce the above copyright notice, this list of conditions and the
 *         following disclaimer in the documentation and/or other materials provided with the distribution.
 * ------------------------------------------------------------------------------------------------------------------------
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 *   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *   USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ------------------------------------------------------------------------------------------------------------------------
 */

#include "config.h"
#if USE_SD_CARD
#include "SD_Manager.h"
#include "Uart.h"
#include "MidiMessages.h"
#include "sequencer.h"
#include <string.h>

//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------
FATFS sd_Fatfs;		/* File system object for the logical drive */
FIL sd_File;			/* place to hold 1 file*/
DIR sd_Directory;
uint8_t sd_initOkFlag = 0;
uint16_t sd_foundSampleFiles = 0;
static uint16_t sd_foundLoopFiles = 0;
uint32_t sd_currentSampleLength = 0;
char sd_currentSampleName[12];
/* Current directory for one-pass sample import. Keeping this as state avoids
   the old indexed selector's repeated directory rescans. */
static const char* sd_activeSampleFolder = "/samples";
//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------
static uint32_t findDataChunk(void);

/* Clears the public active-sample metadata whenever no WAV is open.
   Inputs: none. Outputs: sd_currentSampleLength=0 and empty name. */
static void sdManager_clearActiveSample(void)
{
   sd_currentSampleLength = 0;
   memset(sd_currentSampleName, 0, sizeof(sd_currentSampleName));
}

/* Tiny ASCII-only uppercaser for 8.3 FAT filenames. This keeps .wav/.WAV
   matching case-insensitive without pulling in locale-dependent ctype code. */
static char sdManager_toUpper(char c)
{
   if(c >= 'a' && c <= 'z')
      return (char)(c - ('a' - 'A'));

   return c;
}

static uint8_t sdManager_isWavFile(const FILINFO* fno)
{
   const char* fn;
   int i;

   /* Inputs: FatFs FILINFO from f_readdir(). Output: 1 only for non-directory,
      non-dot entries with a .WAV extension in the short filename. The importer
      relies on this shared predicate so counting and opening agree exactly. */
   if(fno->fname[0] == 0 || fno->fname[0] == '.')
      return 0;

   if(fno->fattrib & AM_DIR)
      return 0;

   fn = fno->fname;

   for(i = 0; i < 12 - 3; i++)
   {
      if(fn[i] == '.')
      {
         return (uint8_t)((sdManager_toUpper(fn[i+1]) == 'W') &&
                          (sdManager_toUpper(fn[i+2]) == 'A') &&
                          (sdManager_toUpper(fn[i+3]) == 'V'));
      }
   }

   return 0;
}

static uint8_t sdManager_openEntry(const char* folder, const FILINFO* fno)
{
   char filename[24];
   size_t prefixLen;
   size_t nameLen;
   uint32_t length;
   char *fn = (char*)fno->fname;
   FRESULT res;

   /* Inputs: folder string such as "/samples" plus a FILINFO entry from that
      folder. Output: 1 with sd_File open and positioned just after the WAV
      data chunk header; 0 if path construction, open, or data-chunk discovery
      fails. Returning 0 lets callers skip bad files without consuming a sample
      metadata slot. */
   sdManager_clearActiveSample();

   memset(filename, 0, sizeof(filename));
   prefixLen = strlen(folder);
   nameLen = strlen(fn);
   if(prefixLen + 1u + nameLen >= sizeof(filename))
   {
      return 0;
   }

   memcpy(filename, folder, prefixLen);
   filename[prefixLen] = '/';
   memcpy(&filename[prefixLen + 1u], fn, nameLen + 1u);

   res = f_open((FIL*)&sd_File, filename, FA_OPEN_EXISTING | FA_READ);
   if(res != FR_OK)
   {
      return 0;
   }

   length = findDataChunk();
   if(length == 0u)
   {
      f_close((FIL*)&sd_File);
      return 0;
   }

   memcpy(sd_currentSampleName, fn, 11);
   sd_currentSampleName[11] = 0;
   sd_currentSampleLength = length;

   return 1;
}

void sdManager_init()
{
	FRESULT res;

	sd_foundSampleFiles = 0;

	//init the Filesystem card
	f_mount(0,(FATFS*)&sd_Fatfs);

	//goto sample directory
	res = f_opendir(&sd_Directory, "/samples");                       /* Open the directory */
    if (res == FR_OK)
    {
		//count .wav files in sample dir
		FILINFO fno;

		while(1)
		{
			res = f_readdir(&sd_Directory, &fno);
			if (res != FR_OK || fno.fname[0] == 0) break;  /* Break on error or end of dir */
			if(sdManager_isWavFile(&fno))
			{
				sd_foundSampleFiles++;
			}
		}

		sd_initOkFlag = 1;
    }

}

//---------------------------------------------------------------------------------------
void sdManager_countLoopFolder(void)
{
   FRESULT res;
   FILINFO fno;

   sd_foundLoopFiles = 0;

   res = f_opendir(&sd_Directory, "/loops");
   if(res != FR_OK)
   {
      /* /loops is optional. Missing folder means one-shot import only, not a
         fatal SD error. */
      return;
   }

   while(1)
   {
      res = f_readdir(&sd_Directory, &fno);
      if(res != FR_OK || fno.fname[0] == 0) break;

      if(sdManager_isWavFile(&fno))
      {
         sd_foundLoopFiles++;
      }
   }
}

//---------------------------------------------------------------------------------------
uint16_t sd_getNumSamples(void)
{
   return (uint16_t)(sd_foundSampleFiles + sd_foundLoopFiles);
}

uint16_t sd_getNumOneShotSamples(void)
{
   return sd_foundSampleFiles;
}
//---------------------------------------------------------------------------------------
uint8_t sd_openSampleFolder(uint8_t looped)
{
   /* Input looped maps directly to the import pass: 0=/samples, 1=/loops.
      Output 0 allows sampleMemory_loadSamples() to skip an absent optional
      folder without blocking on the "Sample Load" screen. */
   sd_activeSampleFolder = looped ? "/loops" : "/samples";

   if(f_opendir(&sd_Directory, sd_activeSampleFolder) != FR_OK)
   {
      sdManager_clearActiveSample();
      return 0;
   }

   return 1;
}
//---------------------------------------------------------------------------------------
uint8_t sd_openNextSampleInFolder(void)
{
   FRESULT res;
   FILINFO fno;

   while(1)
   {
      res = f_readdir(&sd_Directory, &fno);
      if(res != FR_OK || fno.fname[0] == 0)
      {
         sdManager_clearActiveSample();
         return 0;
      }

      /* Keep scanning until a valid WAV is open. Bad WAV candidates are skipped
         here so the importer only sees playable files and keeps dense sN slots. */
      if(sdManager_isWavFile(&fno) &&
         sdManager_openEntry(sd_activeSampleFolder, &fno))
      {
         return 1;
      }
   }
}
//---------------------------------------------------------------------------------------
/*
 * set the file read pointer to the beginning of the sample data in the data chunk
 * returns length of the data block in byte
 * Risk note: this is still a minimal legacy WAV scanner. It does not validate
 * RIFF/fmt fully; it finds the first "data" token and returns that chunk length.
 * Bad reads return 0 so callers can skip/report the file instead of hanging.
 */
static uint32_t findDataChunk(void)
{
	unsigned int bytesRead = 1;
	FRESULT res;
	uint8_t data[4];
	memset(data,0,4);
	uint8_t pos = 0;

	//search substring 'data'
	while(bytesRead == 1) //while !EOF
	{
		res = f_read((FIL*)&sd_File,(void*)&data[pos],1,&bytesRead);
		if(res != FR_OK)
		{
			return 0;
		}

		//check
		if( (data[pos] == 'a') && (data[(pos+1)%4] == 'd') && (data[(pos+2)%4] == 'a') && (data[(pos+3)%4] == 't'))
		{
			//found 'data' header
			//-> read
			uint32_t length;
			res = f_read((FIL*)&sd_File,(void*)&length,4,&bytesRead);
			if(res != FR_OK || bytesRead != 4u)
			{
				return 0;
			}
			return length;
		}

		pos++;
		pos = pos%4;
	}
	return 0;
}
//---------------------------------------------------------------------------------------
//selects the active sample from the folder
uint8_t sd_setActiveSample(uint16_t sampleNr)
{
   FRESULT res;
   uint16_t currentSample = 0;
   const char* folder;
   uint16_t localIndex;
   FILINFO fno;

   /* Compatibility helper for any older indexed callers.
      Input: combined index where /samples comes before /loops.
      Output: 1 when the requested file is open at its data chunk; 0 otherwise.
      Current import uses sd_openSampleFolder()/sd_openNextSampleInFolder()
      instead, because this helper starts scanning from the folder beginning. */
   sdManager_clearActiveSample();

   if(sampleNr < sd_foundSampleFiles)
   {
      folder = "/samples";
      localIndex = sampleNr;
   }
   else
   {
      folder = "/loops";
      localIndex = sampleNr - sd_foundSampleFiles;
   }

   res = f_opendir(&sd_Directory, folder);
   if(res != FR_OK)
   {
      return 0;
   }

   while(1)
   {
      res = f_readdir(&sd_Directory, &fno);
      if(res != FR_OK || fno.fname[0] == 0) break;

      if(sdManager_isWavFile(&fno))
      {
         if(localIndex == currentSample)
         {
            return sdManager_openEntry(folder, &fno);
         }

         currentSample++;
      }
   }

   return 0;
}
//---------------------------------------------------------------------------------------
uint32_t sd_getActiveSampleLength()
{
	return sd_currentSampleLength;
}
//---------------------------------------------------------------------------------------
void sd_closeActiveSample(void)
{
	/* Output side effect: releases the FatFs file object before the importer
	   advances to the next directory entry or exits. */
	f_close((FIL*)&sd_File);
}
//---------------------------------------------------------------------------------------
//read sample data from the active file.
//returns the bytes read.
//if 0 is returned the EOF is reached
uint16_t sd_readSampleData(int16_t* data, uint16_t size)
{
	//read the file content
	unsigned int bytesRead;
	FRESULT res;

	/* Input size is int16 frame count, matching SampleMemory's PCM framing.
	   Output is bytes read so the importer can distinguish complete final
	   chunks from premature EOF/read errors. */
	res = f_read((FIL*)&sd_File,(void*)data,size*2,&bytesRead);
	if(res != FR_OK)
	{
		return 0;
	}

	return bytesRead;
}
//---------------------------------------------------------------------------------------
char* sd_getActiveSampleName()
{
	return sd_currentSampleName;
}
//---------------------------------------------------------------------------------------
#endif
