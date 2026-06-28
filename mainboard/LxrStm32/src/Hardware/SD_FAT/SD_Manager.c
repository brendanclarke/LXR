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
/* Current directory for sample import. The importer processes /samples first
   and /loops second; the same sorted-name cache below is rebuilt for each
   folder so RAM use stays bounded. */
static const char* sd_activeSampleFolder = "/samples";
/* Cached sorted short names for the active import folder.
   Inputs: one FatFs scan in sd_openSampleFolder().
   Outputs: sd_openNextSampleInFolder() opens WAVs by deterministic
   numerical-alphabetical order without rescanning the directory between
   progress packets or at end-of-folder. The cache stores only 8.3 names, not
   WAV data, and is reused for /samples then /loops. 248 mirrors the
   AVR-visible s0..s247 limit without including SampleMemory.h here. */
#define SD_SORTED_SAMPLE_CACHE_MAX ((uint16_t)248u)
static char sd_sortedSampleNames[SD_SORTED_SAMPLE_CACHE_MAX][13];
static uint16_t sd_sortedSampleNameCount = 0u;
static uint16_t sd_sortedSampleNameIndex = 0u;
//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------
static uint32_t findDataChunk(void);
static uint8_t sdManager_openEntry(const char* folder, const FILINFO* fno);

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

static uint8_t sdManager_isDigit(char c)
{
   return (uint8_t)(c >= '0' && c <= '9');
}

static uint8_t sdManager_isNameSeparator(char c)
{
   return (uint8_t)(c == '_' || c == '-' || c == ' ');
}

static void sdManager_copyShortName(char* dst, const char* src)
{
   uint8_t i;

   /* Inputs are FatFs short 8.3 names. Output is a NUL-terminated 13-byte copy
      suitable for later comparison/opening. Keeping copies fixed-size avoids
      depending on long filename support, which is disabled in ffconf.h. */
   for(i = 0u; i < 12u; i++)
   {
      dst[i] = src[i];
      if(src[i] == 0)
         break;
   }

   if(i == 12u)
      dst[12] = 0;
   else
      dst[i] = 0;
}

static int8_t sdManager_compareSampleNames(const char* a, const char* b)
{
   /* Natural, ASCII-only short-name comparator.
      Inputs: two NUL-terminated 8.3 filenames from FILINFO.fname.
      Output: <0 when a sorts first, 0 when equal, >0 when b sorts first.
      Digit runs compare by numeric value so numbered waveform sets load in slot
      order. Separator runs ('_', '-', and spaces) are skipped when both names
      are at a separator boundary after an equal prefix, so punctuation does not
      place 01-unohit before 01_cut. This comparator defines the slot assignment
      order. */
   while(*a || *b)
   {
      if(sdManager_isNameSeparator(*a) && sdManager_isNameSeparator(*b))
      {
         while(sdManager_isNameSeparator(*a))
            a++;
         while(sdManager_isNameSeparator(*b))
            b++;
         continue;
      }

      if(sdManager_isDigit(*a) && sdManager_isDigit(*b))
      {
         const char* aRun = a;
         const char* bRun = b;
         const char* aSig = a;
         const char* bSig = b;
         const char* aEnd;
         const char* bEnd;
         uint8_t aSigLen;
         uint8_t bSigLen;
         uint8_t aRunLen;
         uint8_t bRunLen;

         while(*aSig == '0')
            aSig++;
         while(*bSig == '0')
            bSig++;

         aEnd = aSig;
         bEnd = bSig;
         while(sdManager_isDigit(*aEnd))
            aEnd++;
         while(sdManager_isDigit(*bEnd))
            bEnd++;

         aSigLen = (uint8_t)(aEnd - aSig);
         bSigLen = (uint8_t)(bEnd - bSig);
         if(aSigLen != bSigLen)
            return (aSigLen < bSigLen) ? -1 : 1;

         while(aSig < aEnd && bSig < bEnd)
         {
            if(*aSig != *bSig)
               return (*aSig < *bSig) ? -1 : 1;
            aSig++;
            bSig++;
         }

         while(sdManager_isDigit(*a))
            a++;
         while(sdManager_isDigit(*b))
            b++;

         aRunLen = (uint8_t)(a - aRun);
         bRunLen = (uint8_t)(b - bRun);
         if(aRunLen != bRunLen)
            return (aRunLen < bRunLen) ? -1 : 1;

         continue;
      }
      else
      {
         const char ca = sdManager_toUpper(*a);
         const char cb = sdManager_toUpper(*b);

         if(ca != cb)
            return (ca < cb) ? -1 : 1;

         if(*a)
            a++;
         if(*b)
            b++;
      }
   }

   return 0;
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

static void sdManager_resetSortedSampleCache(void)
{
   /* Inputs: none. Outputs: an empty cache and rewinded iterator. Called before
      each folder pass so /samples and /loops share the same BSS array instead
      of reserving two filename tables. */
   sd_sortedSampleNameCount = 0u;
   sd_sortedSampleNameIndex = 0u;
}

static void sdManager_insertSortedSampleName(const char* name)
{
   uint16_t pos = 0u;
   uint16_t i;

   /* Input is one short 8.3 WAV name from FatFs. Output mutates the fixed cache
      so names remain sorted as they are discovered. Keeping the cache sorted at
      insertion time avoids a second sort pass and, when a folder has more than
      248 WAVs, preserves the first 248 sorted names instead of whichever 248
      happened to appear earliest in raw FAT directory order. */
   while(pos < sd_sortedSampleNameCount &&
         sdManager_compareSampleNames(sd_sortedSampleNames[pos], name) <= 0)
   {
      pos++;
   }

   if(sd_sortedSampleNameCount >= SD_SORTED_SAMPLE_CACHE_MAX)
   {
      if(pos >= SD_SORTED_SAMPLE_CACHE_MAX)
      {
         return;
      }

      for(i = SD_SORTED_SAMPLE_CACHE_MAX - 1u; i > pos; i--)
      {
         sdManager_copyShortName(sd_sortedSampleNames[i],
                                 sd_sortedSampleNames[i - 1u]);
      }
   }
   else
   {
      for(i = sd_sortedSampleNameCount; i > pos; i--)
      {
         sdManager_copyShortName(sd_sortedSampleNames[i],
                                 sd_sortedSampleNames[i - 1u]);
      }
      sd_sortedSampleNameCount++;
   }

   sdManager_copyShortName(sd_sortedSampleNames[pos], name);
}

static uint8_t sdManager_buildSortedSampleCache(void)
{
   FRESULT res;
   FILINFO fno;

   /* Inputs are sd_activeSampleFolder and FatFs. Output is a cached sorted list
      of up to SD_SORTED_SAMPLE_CACHE_MAX short WAV names plus an iterator reset
      to the first entry. This replaces repeated directory walks with one scan
      and makes end-of-folder a cheap index comparison. */
   sdManager_resetSortedSampleCache();
   res = f_opendir(&sd_Directory, sd_activeSampleFolder);
   if(res != FR_OK)
   {
      sdManager_clearActiveSample();
      return 0u;
   }

   while(1)
   {
      res = f_readdir(&sd_Directory, &fno);
      if(res != FR_OK || fno.fname[0] == 0)
         break;

      if(!sdManager_isWavFile(&fno))
         continue;

      sdManager_insertSortedSampleName(fno.fname);
   }

   return 1u;
}

static uint8_t sdManager_openSortedEntryByName(const char* name)
{
   FILINFO fno;

   /* Input is a selected short filename from the sorted scanner. Output matches
      sdManager_openEntry(): 1 with sd_File open at the WAV data chunk, 0 if the
      file disappeared or failed validation. A synthetic FILINFO is enough here
      because sdManager_openEntry() only needs fname to build the path. */
   memset(&fno, 0, sizeof(fno));
   sdManager_copyShortName(fno.fname, name);
   return sdManager_openEntry(sd_activeSampleFolder, &fno);
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
      folder without blocking on the "Sample Load" screen. Opening a folder now
      builds the sorted short-name cache once; later sd_openNext... calls only
      advance an index through that cache. */
   sd_activeSampleFolder = looped ? "/loops" : "/samples";

   return sdManager_buildSortedSampleCache();
}
//---------------------------------------------------------------------------------------
uint8_t sd_openNextSampleInFolder(void)
{
   /* Open the next valid WAV from the active folder's sorted name cache.
      Inputs: sd_sortedSampleNameIndex and the cache built by
      sd_openSampleFolder(). Output: 1 with sd_File positioned at the WAV data
      chunk, or 0 at end-of-folder. Bad/truncated/non-parseable WAV candidates
      are consumed here without creating sample slots. End-of-folder is now an
      index compare instead of another complete FatFs directory walk, which
      prevents the UI from sitting on an old progress number while the STM
      scans. */
   while(sd_sortedSampleNameIndex < sd_sortedSampleNameCount)
   {
      if(sdManager_openSortedEntryByName(
            sd_sortedSampleNames[sd_sortedSampleNameIndex++]))
      {
         return 1u;
      }
   }

   sdManager_clearActiveSample();
   return 0u;
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
