/*
 * SD_Manager.h
 *
 *  Created on: 24.10.2012
 *      Author: Julian
 *
 *      handles all high level SD functionality. load/save preset/pattern etc
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

#ifndef SD_MANAGER_H_
#define SD_MANAGER_H_
#include "config.h"
#if USE_SD_CARD
#include "stm32f4xx.h"
#include "MidiMessages.h"
#include "MidiParser.h"
#include "stdio.h"
#include "ff.h"
#include "FIFO.h"


void sdManager_init();

// Returns the total number of playable-looking .WAV entries found across
// /samples and /loops. sdManager_init() counts /samples; sample import calls
// sdManager_countLoopFolder() before this to refresh /loops.
uint16_t sd_getNumSamples(void);
// Counts .WAV entries in /loops. Missing /loops is allowed and leaves count 0.
void sdManager_countLoopFolder(void);
// Returns only the number of files found in /samples.
uint16_t sd_getNumOneShotSamples(void);
// Opens /samples or /loops for sorted import iteration.
// Input: looped=0 selects /samples, looped=1 selects /loops.
// Output: 1 when the directory was opened and its short WAV names were cached
// in sorted order; 0 when the folder is absent or unreadable.
uint8_t sd_openSampleFolder(uint8_t looped);
// Opens the next valid WAV from the active folder's cached numerical-
// alphabetical 8.3 filename list, independent of FAT directory-entry order.
// Output: 1 with sd_File positioned at its data chunk and active metadata set;
// 0 at end-of-folder or when no further valid WAV can be opened.
uint8_t sd_openNextSampleInFolder(void);
// Selects the active file by combined /samples-first index.
// Input: sampleNr 0..N where /samples entries come before /loops entries.
// Output: 1 only when a valid non-empty WAV data chunk is open. This indexed
// compatibility helper is intentionally not used by the current importer
// because repeated rescans made each later sample slower than the previous one.
uint8_t sd_setActiveSample(uint16_t sampleNr);
uint32_t sd_getActiveSampleLength();
char* sd_getActiveSampleName();
// Closes the active sample file after import has consumed or skipped it.
void sd_closeActiveSample(void);
//read sample data from the active file.
// Input size is int16 frame count; return value is bytes read.
// A zero return means EOF or a FatFs read error; sample import treats both as
// SAMPLE_UPLOAD_STATUS_READ_ERROR when they occur before the expected length.
uint16_t sd_readSampleData(int16_t* data, uint16_t size);


#endif /* SD_MANAGER_H_ */
#endif
