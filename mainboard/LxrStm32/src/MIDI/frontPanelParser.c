/*
 * frontPanelParser.c
 *
 *  Created on: 27.04.2012
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



#include "frontPanelParser.h"
#include "MidiMessages.h"
#include "MidiParser.h"
#include "ParameterArray.h"
#include "sequencer.h"
#include "Uart.h"
#include "SD_Manager.h"
#include "EuklidGenerator.h"
#include "config.h"
#include "mixer.h"

#include "DrumVoice.h"
#include "CymbalVoice.h"
#include "HiHat.h"
#include "Snare.h"
#include "SomGenerator.h"
#include "TriggerOut.h"
#include <string.h>

static void frontParser_handleMidiMessage();
static void frontParser_handleSysexData(unsigned char data);
static void frontParser_handleSeqCC();

#define FLOW_INITIAL_GRANT 4
#define FLOW_ACK_CREDITS 1
#define FRONT_FILE_DONE_TYPE_PERFORMANCE 8
#define FRONT_FILE_DONE_TYPE_ALL 9
#define DEFERRED_PERF_MSG_CACHE_SIZE 128
#define PRF_CACHE_LIVE_PARAM_COUNT 310
#define PRF_PENDING_EXPECTED_MAINSTEP_COUNT (NUM_PATTERN * NUM_TRACKS)
#define PRF_PENDING_EXPECTED_STEP_COUNT (NUM_PATTERN * NUM_TRACKS * NUM_STEPS)
#define PRF_PENDING_EXPECTED_LENGTH_COUNT (NUM_PATTERN * NUM_TRACKS)
#define PRF_PENDING_EXPECTED_SCALE_COUNT (NUM_PATTERN * NUM_TRACKS)
#define PRF_PENDING_EXPECTED_CHAIN_COUNT NUM_PATTERN

typedef enum {
   PRF_CACHE_IDLE = 0,
   PRF_CACHE_RECEIVING_AVR_LIVE,
   PRF_CACHE_LIVE_ACTIVE,
   PRF_CACHE_RECEIVING_PENDING,
   PRF_CACHE_PENDING_VALID,
   PRF_CACHE_ABORTING
} PrfCacheState;

static uint8_t comm_loadSessionActive = 0;
static uint8_t comm_quietUi = 0;
static uint8_t comm_flowActive = 0;
static uint8_t comm_flowChannel = 0;
static uint8_t comm_flowBudgetRemaining = 0;

uint8_t frontParser_deferPerfLoadCacheUntilPatternChange = 0;
static uint8_t frontParser_deferredPerfLoadActive = 0;
static uint8_t frontParser_deferredPerfVoiceCachePending = 0;
static uint8_t frontParser_deferredPerfPatternPending = 0;
static uint8_t frontParser_deferredPerfUnholdPending = 0;
static uint8_t frontParser_deferredPerfProtectedPattern = 0;
static uint8_t frontParser_deferredPerfReplay = 0;
static uint8_t frontParser_deferredPerfMsgCount = 0;
static MidiMsg frontParser_deferredPerfMsgCache[DEFERRED_PERF_MSG_CACHE_SIZE];
static uint8_t frontParser_fileLoadIngressActive = 0;
static uint8_t frontParser_fileLoadBracketActive = 0;
static PrfCacheState frontParser_prfCacheState = PRF_CACHE_IDLE;
static uint8_t frontParser_prfCacheProtectedPattern = 0;
static uint8_t frontParser_prfCachePendingValid = 0;
static uint8_t frontParser_prfCacheAvrLiveValid = 0;
static uint8_t frontParser_prfCacheStmLiveValid = 0;
// static uint8_t frontParser_prfCacheLiveParams[PRF_CACHE_LIVE_PARAM_COUNT];
// static uint8_t frontParser_prfCacheLiveMorph[END_OF_SOUND_PARAMETERS];
static TempPattern frontParser_prfCacheLivePattern;
static uint8_t frontParser_prfCacheLiveActivePattern = 0;
static uint8_t frontParser_prfCacheLivePendingPattern = 0;
static uint8_t frontParser_prfCacheLivePerTrackActivePattern[NUM_TRACKS];
static uint8_t frontParser_prfCacheLivePerTrackPendingPattern[NUM_TRACKS];
static int8_t frontParser_prfCacheLiveStepIndex[NUM_TRACKS+1];
static uint8_t frontParser_prfCacheLiveMidiChannels[8];
static uint8_t frontParser_prfCacheLiveNoteOverride[7];
static uint8_t frontParser_prfCacheLiveVMorphAmount[7];
static uint8_t frontParser_prfCacheLiveVMorphFlag = 0;
static uint8_t frontParser_prfCacheLiveSeqVoicesLoading = 0;
static uint8_t frontParser_prfCacheLiveSeqNewVoiceAvailable = 0;
static uint8_t frontParser_prfCacheLiveSeqTracksLocked = 0;
static uint8_t frontParser_prfCacheLiveSeqLoadFastMode = 0;
static uint16_t frontParser_prfPendingMainStepCount = 0;
static uint16_t frontParser_prfPendingStepCount = 0;
static uint16_t frontParser_prfPendingLengthCount = 0;
static uint16_t frontParser_prfPendingScaleCount = 0;
static uint16_t frontParser_prfPendingChainCount = 0;
static uint16_t frontParser_prfPendingProtectedWriteCount = 0;

extern MidiMsg frontParser_midiMsg;
extern uint8_t frontParser_sysexActive;

static uint8_t frontParser_flowGrantByte(uint8_t channel, uint8_t credits)
{
   return (uint8_t)(((channel & 0x07) << 4) | (credits & 0x0f));
}

static uint8_t frontParser_isFlowMessage(uint8_t status, uint8_t data1)
{
   return (status == FRONT_SEQ_CC)
      && (data1 >= FRONT_SEQ_FLOW_BEGIN)
      && (data1 <= FRONT_SEQ_FLOW_ABORT);
}

static void frontParser_sendFlowGrant(uint8_t channel, uint8_t credits)
{
   if((frontParser_sysexActive != SYSEX_INACTIVE) && (channel != FLOW_CH_LOAD_SESSION))
   {
      comm_flowActive = 0;
      comm_flowBudgetRemaining = 0;
      return;
   }

   uart_sendFrontpanelPriorityByte(FRONT_SEQ_CC);
   uart_sendFrontpanelPriorityByte(FRONT_SEQ_FLOW_GRANT);
   uart_sendFrontpanelPriorityByte(frontParser_flowGrantByte(channel, credits));
}

static void frontParser_sendFlowGrantWait(uint8_t channel, uint8_t credits)
{
   uart_sendFrontpanelPriorityByteWait(FRONT_SEQ_CC);
   uart_sendFrontpanelPriorityByteWait(FRONT_SEQ_FLOW_GRANT);
   uart_sendFrontpanelPriorityByteWait(frontParser_flowGrantByte(channel, credits));
}

static void frontParser_sendFlowAbort(uint8_t channel)
{
   uart_sendFrontpanelPriorityByte(FRONT_SEQ_CC);
   uart_sendFrontpanelPriorityByte(FRONT_SEQ_FLOW_ABORT);
   uart_sendFrontpanelPriorityByte(channel);
}

static void frontParser_sendPrfCacheStatus(uint8_t command, uint8_t status)
{
   uart_sendFrontpanelPriorityByte(PRF_CACHE_STATUS);
   uart_sendFrontpanelPriorityByte(command);
   uart_sendFrontpanelPriorityByte(status);
}

static void frontParser_suspendCreditFlowForSysex()
{
   comm_flowActive = 0;
   comm_flowBudgetRemaining = 0;
}

static void frontParser_flowMessageApplied()
{
   if(!comm_flowActive || frontParser_isFlowMessage(frontParser_midiMsg.status, frontParser_midiMsg.data1))
      return;

   if(comm_flowBudgetRemaining)
      comm_flowBudgetRemaining--;

   if(comm_flowBudgetRemaining == 0)
   {
      comm_flowBudgetRemaining = FLOW_INITIAL_GRANT;
      frontParser_sendFlowGrant(comm_flowChannel, FLOW_INITIAL_GRANT);
   }
}

uint8_t frontParser_isQuietUi()
{
   return comm_quietUi;
}

#define VOICE_PARAM_LENGTH 36
static uint8_t voice1presetMask[VOICE_PARAM_LENGTH]={1,8,9,20,      37,43,49,50,   62,70,74,78,  82,83,88,94,   102,108,115,121,     128,134,137,143,    149,155,161,167,    173,179,185,191,    197,203,209,215}; 
static uint8_t voice2presetMask[VOICE_PARAM_LENGTH]={2,10,11,21,    38,44,51,52,   63,71,75,79,  84,85,89,95,   103,109,116,122,     129,135,138,144,    150,156,162,168,    174,180,186,192,    198,204,210,216}; 
static uint8_t voice3presetMask[VOICE_PARAM_LENGTH]={3,12,13,22,    39,45,53,54,   64,72,76,80,  86,87,90,96,   104,110,117,123,     130,136,139,145,    151,157,163,169,    175,181,187,193,    199,205,211,217}; 
static uint8_t voice4presetMask[VOICE_PARAM_LENGTH]={4,14,15,27,28, 40,46,55,      56,65,68,73,  77,81,91,99,   105,111,118,124,     131,140,146,152,        158,164,170,    176,182,188,194,    200,206,212,218}; 
static uint8_t voice5presetMask[VOICE_PARAM_LENGTH]={6,16,17,23,    24,29,30,31,   32,41,47,57,  58,66,69,92,   100,106,112,119,125, 132,141,147,153,        159,165,171,    177,183,189,195,    201,207,213,219}; 
static uint8_t voice6presetMask[VOICE_PARAM_LENGTH]={7,18,19,25,    26,33,34,35,   36,42,48,59,  60,61,67,93,   101,107,113,120,126, 133,142,148,154,        160,166,172,    178,184,190,196,    202,208,214,220};  

static uint8_t* frontParser_getVoicePresetMask(uint8_t *voice)
{
   switch(*voice)
   {
      case 0:
         return voice1presetMask;
      case 1:
         return voice2presetMask;
      case 2:
         return voice3presetMask;
      case 3:
         return voice4presetMask;
      case 4:
         return voice5presetMask;
      case 6:
         *voice = 5;
         return voice6presetMask;
      case 5:
         return voice6presetMask;
      default:
         return 0;
   }
}

static void frontParser_clearVoiceCache(uint8_t voice)
{
   uint8_t i;
   uint8_t *presetMask = frontParser_getVoicePresetMask(&voice);

   if(!presetMask)
      return;

   midi_midiLfoCacheAvailable[voice] = 0;
   midi_midiVeloCacheAvailable[voice] = 0;

   for(i=0;i<VOICE_PARAM_LENGTH;i++)
   {
      midi_midiCacheAvailable[presetMask[i]] = 0;
   }
}

static void frontParser_clearDeferredPerfLoad()
{
   frontParser_deferredPerfLoadActive = 0;
   frontParser_deferredPerfVoiceCachePending = 0;
   frontParser_deferredPerfPatternPending = 0;
   frontParser_deferredPerfUnholdPending = 0;
   frontParser_deferredPerfMsgCount = 0;
}

static void frontParser_beginFileLoadIngress(uint8_t bracketed)
{
   /* FILE LOAD: File payloads update the normal pattern/parameter images.
      Live edits still use SEQ_PARAM_INGRESS_CURRENT_IMAGE outside this bracket. */
   frontParser_fileLoadIngressActive = 1;
   if(bracketed)
      frontParser_fileLoadBracketActive = 1;
   seq_setIngressTarget(SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT);
   seq_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
}

static void frontParser_endFileLoadIngress()
{
   frontParser_fileLoadIngressActive = 0;
   frontParser_fileLoadBracketActive = 0;
   seq_setIngressTarget(SEQ_PARAM_INGRESS_CURRENT_IMAGE);
   seq_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
}

static void frontParser_resetPrfPendingCounters()
{
   frontParser_prfPendingMainStepCount = 0;
   frontParser_prfPendingStepCount = 0;
   frontParser_prfPendingLengthCount = 0;
   frontParser_prfPendingScaleCount = 0;
   frontParser_prfPendingChainCount = 0;
   frontParser_prfPendingProtectedWriteCount = 0;
}

static void frontParser_clearPrfCacheSession()
{
   frontParser_prfCacheState = PRF_CACHE_IDLE;
   frontParser_prfCacheProtectedPattern = 0;
   frontParser_prfCachePendingValid = 0;
   frontParser_prfCacheAvrLiveValid = 0;
   frontParser_prfCacheStmLiveValid = 0;
   frontParser_resetPrfPendingCounters();
}

static void frontParser_clearPrfRuntimeFlags()
{
   frontParser_prfCacheLiveVMorphFlag = 0;
   seq_vMorphFlag = 0;
}

static uint8_t frontParser_prfCacheSessionActive()
{
   return (frontParser_prfCacheState != PRF_CACHE_IDLE)
      && (frontParser_prfCacheState != PRF_CACHE_ABORTING);
}

static uint8_t frontParser_prfCacheCanExit()
{
   return frontParser_prfCacheStmLiveValid
      && frontParser_prfCachePendingValid
      && (frontParser_prfCacheState == PRF_CACHE_PENDING_VALID);
}

static void frontParser_prfCacheCountPatternWrite(uint8_t pattern)
{
   if(frontParser_prfCacheState != PRF_CACHE_RECEIVING_PENDING)
      return;

   if((pattern & 0x07) == (frontParser_prfCacheProtectedPattern & 0x07))
      frontParser_prfPendingProtectedWriteCount++;
}

static uint8_t frontParser_prfPendingCountsValid()
{
   return (frontParser_prfPendingMainStepCount >= PRF_PENDING_EXPECTED_MAINSTEP_COUNT)
      && (frontParser_prfPendingStepCount >= PRF_PENDING_EXPECTED_STEP_COUNT)
      && (frontParser_prfPendingLengthCount >= PRF_PENDING_EXPECTED_LENGTH_COUNT)
      && (frontParser_prfPendingScaleCount >= PRF_PENDING_EXPECTED_SCALE_COUNT)
      && (frontParser_prfPendingChainCount >= PRF_PENDING_EXPECTED_CHAIN_COUNT)
      && (frontParser_prfPendingProtectedWriteCount > 0);
}

static void frontParser_capturePrfStmLiveSnapshot()
{
   uint8_t track;
   uint8_t step;
   uint8_t pattern = (uint8_t)(frontParser_prfCacheProtectedPattern & 0x07);

   frontParser_prfCacheLiveActivePattern = seq_activePattern;
   frontParser_prfCacheLivePendingPattern = seq_pendingPattern;
   frontParser_prfCacheLivePattern.seq_patternSettings = seq_patternSet.seq_patternSettings[pattern];

   for(track=0;track<NUM_TRACKS;track++)
   {
      frontParser_prfCacheLivePerTrackActivePattern[track] = seq_perTrackActivePattern[track];
      frontParser_prfCacheLivePerTrackPendingPattern[track] = seq_perTrackPendingPattern[track];
      frontParser_prfCacheLivePattern.seq_mainSteps[track] = seq_patternSet.seq_mainSteps[pattern][track];
      frontParser_prfCacheLivePattern.seq_patternLengthRotate[track] =
         seq_patternSet.seq_patternLengthRotate[pattern][track];

      for(step=0;step<NUM_STEPS;step++)
      {
         frontParser_prfCacheLivePattern.seq_subStepPattern[track][step] =
            seq_patternSet.seq_subStepPattern[pattern][track][step];
      }
   }

   for(track=0;track<NUM_TRACKS+1;track++)
      frontParser_prfCacheLiveStepIndex[track] = seq_stepIndex[track];

   for(track=0;track<8;track++)
      frontParser_prfCacheLiveMidiChannels[track] = midi_MidiChannels[track];

   for(track=0;track<7;track++)
   {
      frontParser_prfCacheLiveNoteOverride[track] = midi_NoteOverride[track];
      frontParser_prfCacheLiveVMorphAmount[track] = seq_vMorphAmount[track];
   }

   frontParser_prfCacheLiveVMorphFlag = seq_vMorphFlag;
   frontParser_prfCacheLiveSeqVoicesLoading = seq_voicesLoading;
   frontParser_prfCacheLiveSeqNewVoiceAvailable = seq_newVoiceAvailable;
   frontParser_prfCacheLiveSeqTracksLocked = seq_tracksLocked;
   frontParser_prfCacheLiveSeqLoadFastMode = seq_loadFastMode;
   frontParser_prfCacheStmLiveValid = 1;
}

uint8_t frontParser_prfCacheUseLivePattern()
{
   return frontParser_prfCacheStmLiveValid
      && (frontParser_prfCacheState != PRF_CACHE_IDLE)
      && (frontParser_prfCacheState != PRF_CACHE_ABORTING);
}

uint8_t frontParser_prfCacheTrackUsesLivePattern(uint8_t track)
{
   if(track >= NUM_TRACKS || !frontParser_prfCacheUseLivePattern())
      return 0;

   return (frontParser_prfCacheLivePerTrackActivePattern[track] & 0x07)
      == (frontParser_prfCacheProtectedPattern & 0x07);
}

Step* frontParser_prfCacheLiveStep(uint8_t track, uint8_t step)
{
   if(track >= NUM_TRACKS)
      track = 0;

   return &frontParser_prfCacheLivePattern.seq_subStepPattern[track][step & 0x7f];
}

uint16_t frontParser_prfCacheLiveMainSteps(uint8_t track)
{
   if(track >= NUM_TRACKS)
      return 0;

   return frontParser_prfCacheLivePattern.seq_mainSteps[track];
}

LengthRotate frontParser_prfCacheLiveLengthRotate(uint8_t track)
{
   LengthRotate lr;
   lr.value = 0;

   if(track < NUM_TRACKS)
      lr = frontParser_prfCacheLivePattern.seq_patternLengthRotate[track];

   return lr;
}

PatternSetting frontParser_prfCacheLivePatternSetting()
{
   return frontParser_prfCacheLivePattern.seq_patternSettings;
}

uint8_t frontParser_prfCacheLiveMidiChannel(uint8_t voice)
{
   if(voice >= 8)
      return 0;

   if(frontParser_prfCacheUseLivePattern())
      return frontParser_prfCacheLiveMidiChannels[voice];

   return midi_MidiChannels[voice];
}

uint8_t frontParser_prfCacheLiveNoteOverrideValue(uint8_t voice)
{
   if(voice >= 7)
      return 0;

   if(frontParser_prfCacheUseLivePattern())
      return frontParser_prfCacheLiveNoteOverride[voice];

   return midi_NoteOverride[voice];
}

uint8_t frontParser_prfCacheTakeLiveVMorphFlag()
{
   uint8_t flag;

   if(frontParser_prfCacheUseLivePattern())
   {
      flag = frontParser_prfCacheLiveVMorphFlag;
      frontParser_prfCacheLiveVMorphFlag = 0;
      return flag;
   }

   flag = seq_vMorphFlag;
   seq_vMorphFlag = 0;
   return flag;
}

uint8_t frontParser_prfCacheLiveVMorphAmountValue(uint8_t voice)
{
   if(voice >= 7)
      return 0;

   if(frontParser_prfCacheUseLivePattern())
      return frontParser_prfCacheLiveVMorphAmount[voice];

   return seq_vMorphAmount[voice];
}

static void frontParser_sendPrfRestoreParam(uint16_t paramNr, uint8_t value, uint8_t isMorph)
{
   uint8_t status;
   uint8_t data1;

   if(paramNr < 128)
   {
      status = isMorph ? PRF_RESTORE_MORPH_CC : PRF_RESTORE_PARAM_CC;
      data1 = (uint8_t)paramNr;
   }
   else
   {
      status = isMorph ? PRF_RESTORE_MORPH_CC2 : PRF_RESTORE_PARAM_CC2;
      data1 = (uint8_t)(paramNr - 128);
   }

   uart_sendFrontpanelPriorityByteWait(status);
   uart_sendFrontpanelPriorityByteWait(data1);
   uart_sendFrontpanelPriorityByteWait(value);
}

static void frontParser_sendPrfLiveRestore()
{
   uint16_t i;

   if(!frontParser_prfCacheAvrLiveValid)
      return;

   /* for(i=0;i<PRF_CACHE_LIVE_PARAM_COUNT;i++)
      frontParser_sendPrfRestoreParam(i, frontParser_prfCacheLiveParams[i], 0);

   for(i=0;i<END_OF_SOUND_PARAMETERS;i++)
      frontParser_sendPrfRestoreParam(i, frontParser_prfCacheLiveMorph[i], 1); */
}

static uint8_t frontParser_cachePrfLiveSnapshotMessage()
{
   uint16_t paramNr;

   if(frontParser_prfCacheState != PRF_CACHE_RECEIVING_AVR_LIVE)
      return 0;

   switch(frontParser_midiMsg.status)
   {
      case PRF_RESTORE_PARAM_CC:
         // frontParser_prfCacheLiveParams[frontParser_midiMsg.data1] = frontParser_midiMsg.data2;
         return 1;

      case PRF_RESTORE_PARAM_CC2:
         paramNr = (uint16_t)(frontParser_midiMsg.data1 + 128);
         if(paramNr < PRF_CACHE_LIVE_PARAM_COUNT)
            // frontParser_prfCacheLiveParams[paramNr] = frontParser_midiMsg.data2;
         return 1;

      case PRF_RESTORE_MORPH_CC:
         // frontParser_prfCacheLiveMorph[frontParser_midiMsg.data1] = frontParser_midiMsg.data2;
         return 1;

      case PRF_RESTORE_MORPH_CC2:
         paramNr = (uint16_t)(frontParser_midiMsg.data1 + 128);
         if(paramNr < END_OF_SOUND_PARAMETERS)
            // frontParser_prfCacheLiveMorph[paramNr] = frontParser_midiMsg.data2;
         return 1;

      default:
         return 0;
   }
}

static uint8_t frontParser_isDeferredPerfControlMessage()
{
   if(frontParser_midiMsg.status != FRONT_SEQ_CC)
      return 0;

   if(frontParser_isFlowMessage(frontParser_midiMsg.status, frontParser_midiMsg.data1))
      return 1;

   switch(frontParser_midiMsg.data1)
   {
      case FRONT_SEQ_FILE_BEGIN:
      case FRONT_SEQ_FILE_DONE:
      case FRONT_SEQ_PRF_CACHE_BEGIN:
      case FRONT_SEQ_PRF_PENDING_BEGIN:
      case FRONT_SEQ_PRF_PENDING_DONE:
      case FRONT_SEQ_PRF_CACHE_ABORT:
      case FRONT_SEQ_PRF_AVR_SNAPSHOT_BEGIN:
      case FRONT_SEQ_PRF_AVR_SNAPSHOT_END:
      case FRONT_SEQ_PRF_RESTORE_AVR_LIVE:
      case FRONT_SEQ_LOAD_VOICE:
      case FRONT_SEQ_UNHOLD_VOICE:
         return 1;
      default:
         return 0;
   }
}

static uint8_t frontParser_shouldDeferPerfMessage()
{
   if(frontParser_prfCacheSessionActive())
      return 0;

   if(!frontParser_deferredPerfLoadActive || frontParser_deferredPerfReplay)
      return 0;

   if(frontParser_isDeferredPerfControlMessage())
      return 0;

   switch(frontParser_midiMsg.status)
   {
      case FRONT_SEQ_CC:
      case FRONT_SET_BPM:
      case FRONT_CC_MACRO_TARGET:
         return 1;

      case MIDI_CC:
      case FRONT_CC_2:
      case FRONT_CC_LFO_TARGET:
      case FRONT_CC_VELO_TARGET:
         return seq_voicesLoading ? 0 : 1;

      default:
         return 0;
   }
}

static uint8_t frontParser_cacheDeferredPerfMessage()
{
   if(!frontParser_shouldDeferPerfMessage())
      return 0;

   if(frontParser_deferredPerfMsgCount < DEFERRED_PERF_MSG_CACHE_SIZE)
   {
      frontParser_deferredPerfMsgCache[frontParser_deferredPerfMsgCount++] = frontParser_midiMsg;
   }

   return 1;
}

static void frontParser_applyDeferredPerfMessages()
{
   uint8_t i;

   frontParser_beginFileLoadIngress(0);
   frontParser_deferredPerfReplay = 1;
   for(i=0;i<frontParser_deferredPerfMsgCount;i++)
   {
      frontParser_midiMsg = frontParser_deferredPerfMsgCache[i];
      frontParser_handleMidiMessage();
   }
   frontParser_deferredPerfReplay = 0;
   frontParser_deferredPerfMsgCount = 0;
   frontParser_endFileLoadIngress();
}

static uint8_t frontParser_shouldStagePattern(uint8_t pattern)
{
   if(frontParser_fileLoadIngressActive)
      return 0;

   if(frontParser_prfCacheSessionActive())
      return 0;

   if(frontParser_deferredPerfLoadActive)
      return pattern == frontParser_deferredPerfProtectedPattern;

   return (pattern == seq_activePattern) && seq_isRunning() && !seq_loadFastMode;
}

static void frontParser_markDeferredPatternPending()
{
   if(frontParser_deferredPerfLoadActive)
      frontParser_deferredPerfPatternPending = 1;
}

static void frontParser_deferPerfUnholdVoice(uint8_t voice)
{
   if(voice > 6)
      return;

   if(voice == 6)
      voice = 5;

   frontParser_deferredPerfUnholdPending |= (uint8_t)(0x01 << voice);
   seq_voicesLoading &= (uint8_t)~(0x01 << voice);
}

//a counter for the received bytes
//each message is made up from 3 bytes (status 0xb0, parameter nr and parameter value)
uint8_t frontParser_rxCnt=0;

//the currently selected parameter
MidiMsg frontParser_midiMsg;

//the currently active track on the fron panel (shown on the seq led buttons)
uint8_t frontParser_activeFrontTrack=0;

//used to store the sysex mode and to block all other uart messages while sysexActive != 0
uint8_t frontParser_sysexActive=0;
/** used to collect 2 7 bit messages and combine them to a 14 bit message*/
uint16_t frontParser_twoByteData=0;

uint8_t frontParser_sysexBuffer[16];

/** used to count incoming sequencer step data packages*/
uint16_t frontParser_sysexSeqStepNr=0;

uint8_t frontParser_activeTrack=0;	/** the active track on the Frontpanel. track specific messages refer to the track selected with this command*/
uint8_t frontParser_shownPattern = 0;
uint8_t frontParser_activeStep=0;

uint8_t frontParser_stepCopySource=0;

  //------------------------------------------------------
void frontParser_unholdVoice(uint8_t voice)
{  
   uint8_t *presetMask;
   uint8_t i;  
   seq_newVoiceAvailable |= (0x01<<voice);
   
   switch(voice)
   {
      case 0:
         presetMask=voice1presetMask;
         break;
      case 1:
         presetMask=voice2presetMask;
         break;
      case 2:
         presetMask=voice3presetMask;
         break;
      case 3:
         presetMask=voice4presetMask;
         break;
      case 4:
         presetMask=voice5presetMask;
         break;
      case 6:
         voice--;
      case 5:
         seq_newVoiceAvailable |= 0x60;
         presetMask=voice6presetMask;
         break;
      default:
         return;
   }
   
   if(midi_midiLfoCacheAvailable[voice])
      midi_kitLfoCache[voice]=midi_midiLfoCache[voice];
   if(midi_midiVeloCacheAvailable[voice])
      midi_kitVeloCache[voice]=midi_midiVeloCache[voice];
   
   for(i=0;i<VOICE_PARAM_LENGTH;i++)
   {
      if(midi_midiCacheAvailable[presetMask[i]])
      {
         midi_midiKit[presetMask[i]]=midi_midiCache[presetMask[i]];
      }
   }
   
}  
//------------------------------------------------------
void frontParser_uncacheVoice(uint8_t voice)
{  
   uint8_t *presetMask;
   uint8_t i;  
   
   switch(voice)
   {
      case 0:
         if(midi_midiLfoCacheAvailable[voice])
            modNode_setDestination(&voiceArray[voice].lfo.modTarget, midi_midiLfoCache[voice]);
         presetMask=voice1presetMask;
         break;
      case 1:
         if(midi_midiLfoCacheAvailable[voice])
            modNode_setDestination(&voiceArray[voice].lfo.modTarget, midi_midiLfoCache[voice]);
         presetMask=voice2presetMask;
         break;
      case 2:
         if(midi_midiLfoCacheAvailable[voice])
            modNode_setDestination(&voiceArray[voice].lfo.modTarget, midi_midiLfoCache[voice]);
         presetMask=voice3presetMask;
         break;
      case 3:
         if(midi_midiLfoCacheAvailable[voice])
            modNode_setDestination(&snareVoice.lfo.modTarget, midi_midiLfoCache[voice]);
         presetMask=voice4presetMask;
         break;
      case 4:
         if(midi_midiLfoCacheAvailable[voice])
            modNode_setDestination(&cymbalVoice.lfo.modTarget, midi_midiLfoCache[voice]);
         presetMask=voice5presetMask;
         break;
      case 6:
         voice--;
      case 5:
         if(midi_midiLfoCacheAvailable[voice])
            modNode_setDestination(&hatVoice.lfo.modTarget, midi_midiLfoCache[voice]);
         seq_newVoiceAvailable &= ~(0x60);
         seq_voicesLoading &= ~(0x60);
         presetMask=voice6presetMask;
         break;
      default:
         return;
   }
   if(midi_midiLfoCacheAvailable[voice])
   {
      switch(voice)
      {
         case 0:
         case 1:
         case 2:	modNode_setDestination(&voiceArray[voice].lfo.modTarget, midi_midiLfoCache[voice]);
            break;
         case 3:	modNode_setDestination(&snareVoice.lfo.modTarget,midi_midiLfoCache[voice]);		
            break;
         case 4:	modNode_setDestination(&cymbalVoice.lfo.modTarget, midi_midiLfoCache[voice]);		
            break;
         case 5:
         case 6:	modNode_setDestination(&hatVoice.lfo.modTarget, midi_midiLfoCache[voice]);			
            break;
         default:
            break;
      }
      midi_midiLfoCacheAvailable[voice]=0;
   }
   if(midi_midiVeloCacheAvailable[voice])
   {
      modNode_setDestination(&velocityModulators[voice], midi_midiVeloCache[voice]);
      midi_midiVeloCacheAvailable[voice]=0;
   }
   
   for(i=0;i<VOICE_PARAM_LENGTH;i++)
   {
      if(midi_midiCacheAvailable[presetMask[i]])
      {
         midiParser_ccHandler(midi_midiCache[presetMask[i]],0);
         midi_midiCacheAvailable[presetMask[i]]=0;
      }
   }
   
}   

static void frontParser_releaseVoiceCache(uint8_t voice)
{
   /* FILE LOAD: When the temporary pattern is sounding, cached voice payload
      has already populated normal STM storage. Clear the delayed live cache
      instead of applying it to the temporary sound. */
   if(seq_isTmpKitActive())
      frontParser_clearVoiceCache(voice);
   else
      frontParser_uncacheVoice(voice);
}

static void frontParser_unholdLoadedVoice(uint8_t voice)
{
   /* FILE LOAD: While the temporary kit is active, unhold must not promote
      loaded voice cache into midi_midiKit or mark it for trigger-time apply. */
   if(seq_isTmpKitActive())
   {
      frontParser_clearVoiceCache(voice);
      return;
   }

   frontParser_unholdVoice(voice);
}

static uint8_t frontParser_voiceCachePending(uint8_t voice)
{
   uint8_t i;
   uint8_t *presetMask = frontParser_getVoicePresetMask(&voice);

   if(!presetMask)
      return 0;

   if(midi_midiLfoCacheAvailable[voice] || midi_midiVeloCacheAvailable[voice])
      return 1;

   for(i=0;i<VOICE_PARAM_LENGTH;i++)
   {
      if(midi_midiCacheAvailable[presetMask[i]])
         return 1;
   }

   return 0;
}

static void frontParser_applyPendingVoiceCache()
{
   uint8_t voice;

   for(voice=0;voice<6;voice++)
   {
      if(frontParser_deferredPerfUnholdPending & (0x01<<voice))
      {
         frontParser_unholdLoadedVoice(voice);
         frontParser_deferredPerfUnholdPending &= (uint8_t)~(0x01<<voice);
      }

      if(frontParser_voiceCachePending(voice))
      {
         frontParser_releaseVoiceCache(voice);
         if(voice == 5)
            seq_newVoiceAvailable &= (uint8_t)~0x60;
         else
            seq_newVoiceAvailable &= (uint8_t)~(0x01<<voice);
      }
   }
}

void frontParser_applyDeferredVoiceCache()
{
   if(frontParser_prfCacheCanExit())
   {
      frontParser_applyPendingVoiceCache();
      frontParser_deferredPerfVoiceCachePending = 0;
      frontParser_deferredPerfPatternPending = 0;
      frontParser_clearDeferredPerfLoad();
      frontParser_clearPrfRuntimeFlags();
      frontParser_clearPrfCacheSession();
      return;
   }

   if(frontParser_prfCacheSessionActive())
      return;

   if(frontParser_deferredPerfVoiceCachePending)
   {
      frontParser_applyDeferredPerfMessages();
      frontParser_applyPendingVoiceCache();
      if(frontParser_deferredPerfPatternPending)
         seq_applyTmpPatternTo(frontParser_deferredPerfProtectedPattern);
      frontParser_deferredPerfVoiceCachePending = 0;
      frontParser_deferredPerfPatternPending = 0;
      frontParser_clearDeferredPerfLoad();
   }
}

//------------------------------------------------------
/**send all active step numbers to frontpanel to light up corresponding LEDs*/
void frontParser_updateTrackLeds(const uint8_t trackNr, uint8_t patternNr)
{
   if(trackNr<=6)
   {
   
      frontParser_activeFrontTrack = trackNr;
      
      uint8_t ledByte = 0x00;
      uint8_t i;
      uint8_t k;
      
      for(k=0;k<4;k++)
      {
         for(i=0;i<4;i++)
         {
            if(seq_isMainStepActive(trackNr,(uint8_t)( (k<<2) + i),patternNr))
            {
               ledByte |= (0x01<<i);
            }
         }
         
         uart_sendFrontpanelByte(FRONT_STEP_LED_STATUS_BYTE);
         uart_sendFrontpanelByte((uint8_t)(FRONT_LED_SEQ_MAIN_ONE+k));
         uart_sendFrontpanelByte(ledByte);
         
         ledByte = 0x00;
      }
   
      
      frontParser_updateSubStepLeds(trackNr, patternNr);
   }
}

void frontParser_updateSubStepLeds(const uint8_t trackNr, uint8_t patternNr)
{
   uint8_t start = frontParser_activeStep&0x78; // truncate to main step
   uint8_t ledByte = 0x00;
   uint8_t i;
      
   for(i=0;i<4;i++)
   {
      if(seq_isStepActive(trackNr,(start+i),patternNr))
      {
         ledByte |= (0x01<<i);
      }
   }
      
   uart_sendFrontpanelByte(FRONT_STEP_LED_STATUS_BYTE);
   uart_sendFrontpanelByte(FRONT_LED_SEQ_SUB_STEP_LOWER);
   uart_sendFrontpanelByte(ledByte);
      
   ledByte = 0x00;
      
   for(i=0;i<4;i++)
   {
      if(seq_isStepActive(trackNr,(start+4+i),patternNr))
      {
         ledByte |= (0x01<<i);
      }
   }
   uart_sendFrontpanelByte(FRONT_STEP_LED_STATUS_BYTE);
   uart_sendFrontpanelByte(FRONT_LED_SEQ_SUB_STEP_UPPER);
   uart_sendFrontpanelByte(ledByte);
}

//------------------------------------------------------
void frontParser_parseUartData(unsigned char data)
{

	//TODO der ganze sysex kram kann sicher noch optimiert werden
	//das das nicht andauernd abgefragt werden muss

	// if high bit is set, a new message starts
   if(data&0x80)
   {
   	//reset the byte counter
      frontParser_rxCnt = 0;
      frontParser_midiMsg.status = data;
      if(data==SYSEX_START)
      {
         frontParser_suspendCreditFlowForSysex();
         frontParser_sysexActive = SYSEX_ACTIVE_MODE_NONE;
         uart_clearFrontFifo();
         
      	//send SYSEX_START as ACK
         uart_sendFrontpanelSysExByte(SYSEX_START);
      }
      else if(data==SYSEX_END)
      {
         uart_sendFrontpanelSysExByte(SYSEX_END);
         frontParser_sysexActive = SYSEX_INACTIVE;
      }
      else if(data==PATCH_RESET)
      {
         seq_newVoiceAvailable=0x7f;
      }
      else if(data==FRONT_CALLBACK_ACK)
      {
         uart_sendFrontpanelPriorityByte(FRONT_CALLBACK_ACK);
      }
      else
      {
         frontParser_sysexActive = SYSEX_INACTIVE;
      }
   }
   else // data byte
   {
      if(frontParser_midiMsg.status == SYSEX_START)
      {
         frontParser_handleSysexData(data);
      }
      else if(frontParser_rxCnt==0) //normal operation
      {
      	//parameter nr
         frontParser_midiMsg.data1 = data;
         frontParser_rxCnt++;
      }
      else
      {
      	//parameter value
         frontParser_midiMsg.data2 = data;
      	//message received. process the midi message
         frontParser_handleMidiMessage();
      }
   }
};

//------------------------------------------------------
// This is called when we are in sysex mode and are receiving the sysex bytes
static void frontParser_handleSysexData(unsigned char data)
{
	//SYSEX
	// this is not a correct sysex implementation.
	// in the front panel communication it is used to send sequencer data for preset saving
	// while the preset saving is active, no other data has to be send over the uart!!!
	// so when the SYSEX_START is received, all other communication is stopped until theSYSEX_END message is received
	// no abort and no manufacturer id is supported

	//we have received sysex data
	//first we receive a mode message
	//SYSEX_REQUEST_STEP_DATA or SYSEX_SEND_STEP_DATA
	//then the corresponding data

   switch(frontParser_sysexActive) {
      case SYSEX_REQUEST_PATTERN_DATA:
      //1 byte = pattern nr
      //send back next and repeat
         uart_sendFrontpanelSysExByte(seq_patternSet.seq_patternSettings[data].nextPattern);
         uart_sendFrontpanelSysExByte(seq_patternSet.seq_patternSettings[data].changeBar);
         break;
   
      case SYSEX_REQUEST_MAIN_STEP_DATA:
      //we expect a 2 byte message containing a step nr
      //which tells us which main step data to send back
         frontParser_rxCnt++;
         if(frontParser_rxCnt == 1)
         {
         //first nibble received -> upper nibble
            frontParser_twoByteData = data<<7;
         }
         else
         {
         //second nibble -> lower nibble
            frontParser_twoByteData |= data&0x7f;
         //reset rxCount for next 2 nibble message
            frontParser_rxCnt = 0;
         //we have received a complete 2 nibble step nr
         //send data back to front
            seq_sendMainStepInfoToFront(frontParser_twoByteData);
         }
         break;
   
      case SYSEX_REQUEST_STEP_DATA:
      //we expect a 2 byte message containing a step nr
      //which tells us which step data to send back
         frontParser_rxCnt++;
      
         if(frontParser_rxCnt == 1)
         {
         //first nibble received -> upper nibble
            frontParser_twoByteData = data<<7;
         }
         else
         {
         //second nibble -> lower nibble
            frontParser_twoByteData |= data&0x7f;
         //reset rxCount for next 2 nibble message
            frontParser_rxCnt = 0;
         //we have received a complete 2 nibble step nr
         //send data back to front
            seq_sendStepInfoToFront(frontParser_twoByteData);
         }
      
         break;
   
      
      case SYSEX_RECEIVE_MAIN_STEP_DATA:
         if(frontParser_rxCnt<3)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         
         //calculate the step pattern and track indices
            uint8_t currentTrack = (frontParser_sysexBuffer[0]>>3)&0x07;
            uint8_t currentPattern = (frontParser_sysexBuffer[0]&0x07);
         
            uint16_t mainStepData = frontParser_sysexBuffer[1] | frontParser_sysexBuffer[2]<<7 | frontParser_sysexBuffer[3]<<14;
         
         //first load into inactive track
            PatternSet* patternSet = &seq_patternSet;
            
            if(frontParser_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_mainSteps[currentTrack] = mainStepData;
               frontParser_markDeferredPatternPending();
            }
            else if(!frontParser_prfCacheSessionActive()
               && (currentPattern == seq_activePattern)
               && seq_isRunning())
            {  
               if(seq_loadFastMode)
               {
                  seq_tracksLocked |= (0x01<<currentTrack);
                  patternSet->seq_mainSteps[currentPattern][currentTrack] = mainStepData;
               }
               else
               {
                  seq_tmpPattern.seq_mainSteps[currentTrack] = mainStepData;
               }
            } 
            else
            {
               patternSet->seq_mainSteps[currentPattern][currentTrack] = mainStepData;
            }

            if(frontParser_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
            {
               frontParser_prfPendingMainStepCount++;
               frontParser_prfCacheCountPatternWrite(currentPattern);
            }
         
            
            frontParser_rxCnt = 0;
            uart_sendFrontpanelSysExByte(SYSEX_RECEIVE_MAIN_STEP_DATA);
         }
         break;
      case SYSEX_RECEIVE_PAT_CHAIN_DATA:
         if(frontParser_rxCnt<1)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
            //calculate the pattern index. we expect 'next', then all 'repeat' for pat 0-7
            const uint8_t currentPattern	= frontParser_sysexSeqStepNr;
            uint8_t next = frontParser_sysexBuffer[0];
            uint8_t repeat = frontParser_sysexBuffer[1];
            //first load into inactive track
            PatternSet* patternSet = &seq_patternSet;
            if(frontParser_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_patternSettings.nextPattern=next;
               seq_tmpPattern.seq_patternSettings.changeBar=repeat;
               frontParser_markDeferredPatternPending();
            }
            else
            {
               patternSet->seq_patternSettings[currentPattern].nextPattern = next;
               patternSet->seq_patternSettings[currentPattern].changeBar = repeat;
            }
            if(frontParser_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
            {
               frontParser_prfPendingChainCount++;
               frontParser_prfCacheCountPatternWrite(currentPattern);
            }
            //inc the step counter
            frontParser_sysexSeqStepNr++;
            frontParser_rxCnt = 0;
         
         }
         uart_sendFrontpanelSysExByte(SYSEX_RECEIVE_PAT_CHAIN_DATA);
         break;
      case SYSEX_RECEIVE_PAT_LEN_DATA:
      // --AS same as above but we are receiving length data for each pattern
         if(frontParser_rxCnt<1)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         //calculate the step pattern and track indices
            uint8_t currentTrack = (frontParser_sysexBuffer[0]>>3)&0x07;
            uint8_t currentPattern = (frontParser_sysexBuffer[0]&0x07);
         //first load into inactive track
            PatternSet* patternSet = &seq_patternSet;
         
         
            if(frontParser_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_patternLengthRotate[currentTrack].length = data;
               frontParser_markDeferredPatternPending();
            } 
            else 
            
            {
               patternSet->seq_patternLengthRotate[currentPattern][currentTrack].length = data;
            }

            if(frontParser_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
            {
               frontParser_prfPendingLengthCount++;
               frontParser_prfCacheCountPatternWrite(currentPattern);
            }
            
            frontParser_rxCnt = 0;
            uart_sendFrontpanelSysExByte(SYSEX_RECEIVE_PAT_LEN_DATA);
         }
         break;
         
      case SYSEX_RECEIVE_PAT_SCALE_DATA:
       // --BC same as above but we are receiving scale data for each pattern
         if(frontParser_rxCnt<1)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
         
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         //calculate the step pattern and track indices
            uint8_t currentTrack = (frontParser_sysexBuffer[0]>>3)&0x07;
            uint8_t currentPattern = (frontParser_sysexBuffer[0]&0x07);
            /*
            const uint8_t currentPattern	= frontParser_sysexSeqStepNr / 7;
            const uint8_t currentTrack  	= frontParser_sysexSeqStepNr - currentPattern*7;
            */
         
         //first load into inactive track
            PatternSet* patternSet = &seq_patternSet;
         
            
            if(frontParser_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_patternLengthRotate[currentTrack].scale = data;
               frontParser_markDeferredPatternPending();
            } 
            else 
            {
               patternSet->seq_patternLengthRotate[currentPattern][currentTrack].scale = data;
            }

            if(frontParser_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
            {
               frontParser_prfPendingScaleCount++;
               frontParser_prfCacheCountPatternWrite(currentPattern);
            }
         
         
            //inc the step counter
            frontParser_sysexSeqStepNr++;
            frontParser_rxCnt = 0;
         
         // signal new pattern after receiving all the data
         /*
            if( seq_isRunning() && (frontParser_sysexSeqStepNr == NUM_TRACKS*NUM_PATTERN)) {
               seq_newPatternAvailable = 1;
            }
            */
            uart_sendFrontpanelSysExByte(SYSEX_RECEIVE_PAT_SCALE_DATA);
         }
         
         break;
      
   
      case SYSEX_RECEIVE_STEP_DATA:
      // we expect a bunch of 8 byte sysex message containing new step data for the sequencer
      // beginning with step 0 up to NUMBER_STEPS*NUM_TRACKS*NUM_PATTERN = 128*7*8 = 7168 steps
         if(frontParser_rxCnt<7)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
         //now we have to distribute the MSBs to the sysex data
            uint8_t i;
            for(i=0;i<7;i++)
            {
               frontParser_sysexBuffer[i] |= ((data&(1<<i))<<(7-i));
            
            }
         
         //calculate the step pattern and track indices
            const uint8_t absPat 	= frontParser_sysexSeqStepNr/128;
            const uint8_t currentTrack 		= absPat / 8;
            const uint8_t currentPattern 	= absPat - currentTrack*8;
            const uint8_t currentStep		= frontParser_sysexSeqStepNr - absPat*128;
         
            PatternSet* patternSet = &seq_patternSet;
         /*
            if((seq_newPatternVoiceArray&(0x01<<currentTrack))==0)
            { // track is not in the array, use current step data instead
               frontParser_sysexBuffer[0] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].volume;
               frontParser_sysexBuffer[1] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].prob;
               frontParser_sysexBuffer[2] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].note;
               frontParser_sysexBuffer[3] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Nr;
               frontParser_sysexBuffer[4] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Val;
               frontParser_sysexBuffer[5] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Nr;
               frontParser_sysexBuffer[6] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Val;
            }
         */
         //do not overwrite playing pattern
            if(frontParser_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].volume 	= frontParser_sysexBuffer[0];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].prob 	= frontParser_sysexBuffer[1];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].note 	= frontParser_sysexBuffer[2];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param1Nr = frontParser_sysexBuffer[3];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param1Val 	= frontParser_sysexBuffer[4];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param2Nr 	= frontParser_sysexBuffer[5];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param2Val 	= frontParser_sysexBuffer[6];
               frontParser_markDeferredPatternPending();
            } 
            else {
            
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].volume 	= frontParser_sysexBuffer[0];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].prob 	= frontParser_sysexBuffer[1];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].note 	= frontParser_sysexBuffer[2];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Nr = frontParser_sysexBuffer[3];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Val 	= frontParser_sysexBuffer[4];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Nr 	= frontParser_sysexBuffer[5];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Val 	= frontParser_sysexBuffer[6];
            }
         //signal that a new data chunk is available
         //frontParser_newSeqDataAvailable = 1;
         //reset receive counter for next chunk
            frontParser_rxCnt = 0;
         
         //inc the step counter
            frontParser_sysexSeqStepNr++;
         }
         break;  
         
      case SYSEX_BEGIN_PATTERN_TRANSMIT:
        // we expect a bunch of 8 byte sysex message containing new step data for the sequencer
        // beginning with step 0 up to NUMBER_STEPS*NUM_TRACKS*NUM_PATTERN = 128*7*8 = 7168 steps
         if(frontParser_rxCnt<8)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
            uint8_t currentTrack = (frontParser_sysexBuffer[0]>>3)&0x07;
            uint8_t currentPattern = (frontParser_sysexBuffer[0]&0x07);
            uint8_t currentStep = frontParser_sysexSeqStepNr;//frontParser_sysexBuffer[2];
            
            //now we have to distribute the MSBs to the sysex data
            uint8_t i;
            for(i=0;i<7;i++)
            {
               frontParser_sysexBuffer[i] = frontParser_sysexBuffer[i+1];
               frontParser_sysexBuffer[i] |= ((data&(1<<i))<<(7-i));
            
            }
         
            PatternSet* patternSet = &seq_patternSet;
            
         //do not overwrite playing pattern
         
            if(frontParser_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].volume 	= frontParser_sysexBuffer[0];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].prob 	= frontParser_sysexBuffer[1];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].note 	= frontParser_sysexBuffer[2];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param1Nr = frontParser_sysexBuffer[3];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param1Val 	= frontParser_sysexBuffer[4];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param2Nr 	= frontParser_sysexBuffer[5];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param2Val 	= frontParser_sysexBuffer[6];
               frontParser_markDeferredPatternPending();
            } 
            else 
            {
            
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].volume 	= frontParser_sysexBuffer[0];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].prob 	= frontParser_sysexBuffer[1];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].note 	= frontParser_sysexBuffer[2];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Nr = frontParser_sysexBuffer[3];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Val 	= frontParser_sysexBuffer[4];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Nr 	= frontParser_sysexBuffer[5];
               patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Val 	= frontParser_sysexBuffer[6];
            }

            if(frontParser_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
            {
               frontParser_prfPendingStepCount++;
               frontParser_prfCacheCountPatternWrite(currentPattern);
            }
            //signal that a new data chunk is available
            //frontParser_newSeqDataAvailable = 1;
            //reset receive counter for next chunk
            frontParser_rxCnt = 0;
            
            if(frontParser_sysexSeqStepNr<127)
            {
               frontParser_sysexSeqStepNr++;
               uart_sendFrontpanelSysExByte(SYSEX_STEP_ACK);
            }
            else
            {
               
               if((currentPattern == seq_activePattern) && seq_loadFastMode)
               {  
                  seq_tracksLocked &= ~(0x01<<currentTrack);
               }
               else if (!frontParser_deferredPerfLoadActive
                  && !frontParser_prfCacheSessionActive()
                  && currentPattern == 7
                  && seq_isRunning()
                  && !seq_loadFastMode)
               {
                  seq_newPatternAvailable=1;
               }
               uart_sendFrontpanelSysExByte(SYSEX_BEGIN_PATTERN_TRANSMIT);
            }
         }
         break;  
      
      case SYSEX_ACTIVE_MODE_NONE:
      default:
      
      //we received a mode message -> set the active mode
         frontParser_sysexActive = data;
         frontParser_sysexSeqStepNr = 0;
         frontParser_rxCnt = 0;
         break;
   }
}
//------------------------------------------------------
// This is called when we've received a full midi message
static void frontParser_handleMidiMessage()
{
   if(frontParser_cachePrfLiveSnapshotMessage())
   {
      frontParser_flowMessageApplied();
      return;
   }

   if(frontParser_cacheDeferredPerfMessage())
   {
      frontParser_flowMessageApplied();
      return;
   }

   switch(frontParser_midiMsg.status)
   {
      case PARAM_RESTORE_READY:
         seq_tmpKitHandshakeReady = 1;
         break;

      case PARAM_RESTORE_ACK:
         seq_tmpKitHandshakeAck = 1;
         break;

      case PRF_RESTORE_PARAM_CC:
      case PRF_RESTORE_PARAM_CC2:
         {
            uint16_t paramNr = frontParser_midiMsg.data1;
            if(frontParser_midiMsg.status == PRF_RESTORE_PARAM_CC2)
               paramNr += 128;

            /* PRF_RESTORE_PARAM_* carries raw kit/front endpoint bytes. These
               are not live low MIDI CC numbers, so do not apply the +1 offset. */
            seq_storeParameterIngress(paramNr, frontParser_midiMsg.data2);
         }
         break;

      case PRF_RESTORE_MORPH_CC:
      case PRF_RESTORE_MORPH_CC2:
         {
            uint16_t paramNr = frontParser_midiMsg.data1;
            if(frontParser_midiMsg.status == PRF_RESTORE_MORPH_CC2)
               paramNr += 128;

            /* RESTORE: Store morph parameter endpoint bytes into the endpoint image
               selected by the current transfer context. These raw selector bytes are
               distinct from the resolved automation target sideband messages. */
            uint8_t currentTarget = seq_getIngressTarget();
            if(paramNr < END_OF_SOUND_PARAMETERS)
            {
               if(currentTarget == SEQ_PARAM_INGRESS_CURRENT_IMAGE)
               {
                  seq_storeMorphParameterIngress(paramNr, frontParser_midiMsg.data2);
               }
               else
               {
                  seq_normalKitState.morphParams[paramNr] = frontParser_midiMsg.data2;
               }
            }
         }
         break;

      case FRONT_CC_MACRO_TARGET: //frontParser_midiMsg.status
         {
            uint8_t applyLive = seq_shouldApplyIngressToLive();
         
            /* MACRO_CC message structure
            byte1 - status byte 0xaa as above
            byte2, data1 byte: xtta aa-b : tt= top level macro value sent (2 macros exist now, we can do 2 more if we want)
                                           aaa= macro destination value sent (4 destinations exist now, can do 8)
                                           b=macro mod target value top bit
                                           I have left a blank bit above this to make it easier to make more than 255 kit parameters
                                           if we ever want to take on that can of worms
                                          
            byte3, data2 byte: xbbb bbbb : b=macro mod target value lower 7 bits or top level value full
            */
            
            uint8_t upper = frontParser_midiMsg.data1;
            uint8_t lower = frontParser_midiMsg.data2;
            //sequencer_sendVMorph(0,(uint8_t)(lower*macroModulators[0].amount));
            if (upper&0x20)
            {
               float value = ((float)(lower))/127.f;
               // top level macro amount message received
               if(applyLive)
               {
                  modNode_updateValue(&macroModulators[0],(value));
                  modNode_updateValue(&macroModulators[1],(value));
               }
            }
            else if (upper&0x40)
            {
               float value = ((float)(lower))/127.f;
               // top level macro amount message received
               if(applyLive)
               {
                  modNode_updateValue(&macroModulators[2],(value));
                  modNode_updateValue(&macroModulators[3],(value));
               }
            }
            else
            {
               // macro destination message
               uint16_t value = (uint16_t)( ( (upper&0x03)<<8) | lower);
               uint8_t whichModDest = (uint8_t)( 0x07&(upper>>2) ); // whichModDest 0,1,2,3 mac1d1,mac1d2,mac2d1,mac2d2
               seq_storeMacroDestinationIngress(whichModDest, value);
               if(applyLive)
               {
                  modNode_setDestination(&macroModulators[whichModDest], value);
                  modNode_updateValue(&macroModulators[whichModDest],macroModulators[whichModDest].lastVal);
               }
            }
         
         }
         break; // case FRONT_CC_LFO_TARGET
      //SEQ MESSAGES
      case FRONT_SEQ_CC: // frontParser_midiMsg.status
         frontParser_handleSeqCC();
         break;
   
      case SAMPLE_CC:
         switch(frontParser_midiMsg.data1)
         {
         
            case FRONT_SAMPLE_START_UPLOAD:
               seq_setRunning(0);
               sampleMemory_init();
               sampleMemory_loadSamples();
               FLASH_Lock();
            
               uart_sendFrontpanelByte(ACK);
               break;
         
            case FRONT_SAMPLE_COUNT:
               uart_sendFrontpanelByte(SAMPLE_CC);
               uart_sendFrontpanelByte(FRONT_SAMPLE_COUNT);
               uart_sendFrontpanelByte(sampleMemory_getNumSamples());
               break;
         
            default:
               break;
         }
         break;
   
   //MIDI SYNTH MESSAGES
      case MIDI_CC: //frontParser_midiMsg.status
         // this is for parameters below 128
         {
            uint8_t rawParam = frontParser_midiMsg.data1;
            MidiMsg liveMsg = frontParser_midiMsg;

            liveMsg.data1 = (uint8_t)((rawParam + 1) & 0x7f);

            // are receiving a file transmit for voice?
            // message is for loading voice, or if all voices loading, always hold message
            if(seq_voicesLoading)
            {
               seq_storeParameterIngress(rawParam, frontParser_midiMsg.data2);

               midi_midiCache[rawParam]=liveMsg;
               midi_midiCacheAvailable[rawParam]=1;
               // message is cached for voice release. 
               // we can do: midiParser_ccHandler(seq_midiCache[voice1PresetMask[i]],1)
               // for i=0:37 to release a voice. no need to split CC and CC2.
            }
            else
            {
               if(seq_shouldApplyIngressToLive())
               {
                  seq_storeParameterIngress(rawParam, frontParser_midiMsg.data2);
                  midiParser_ccHandler(liveMsg,0);

               //record automation if record is turned on
                  seq_recordAutomation(frontParser_activeTrack, rawParam, frontParser_midiMsg.data2);
               }
               else
               {
                  /* FILE LOAD: Store normal-image params without changing the
                     temporary sound when the temporary pattern is active. */
                  seq_storeParameterIngress(rawParam, frontParser_midiMsg.data2);
               }
            }
         }
         break;
   
   //CC2 above 127
      case FRONT_CC_2: // frontParser_midiMsg.status
         {
            // are receiving a file transmit for voice?
            // message is for loading voice, or if all voices loading, always hold message
            if(seq_voicesLoading)
            {
               seq_storeParameterIngress(frontParser_midiMsg.data1+128, frontParser_midiMsg.data2);
               midi_midiCache[frontParser_midiMsg.data1+128]=frontParser_midiMsg;
               midi_midiCacheAvailable[frontParser_midiMsg.data1+128]=1;
            // message is cached for voice release. 
            // we can do: midiParser_ccHandler(seq_midiCache[voice1PresetMask[i]],1)
            // for i=0:37 to release a voice. no need to split CC and CC2.     
            }
            else
            {
               if(seq_shouldApplyIngressToLive())
               {
                  midiParser_ccHandler(frontParser_midiMsg,1);
                  //record automation if record is turned on
                  seq_recordAutomation(frontParser_activeTrack, frontParser_midiMsg.data1+128, frontParser_midiMsg.data2);
               }
               else
               {
                  /* FILE LOAD: Store normal-image params without changing the
                     temporary sound when the temporary pattern is active. */
                  seq_storeParameterIngress(frontParser_midiMsg.data1+128, frontParser_midiMsg.data2);
               }
            }
         }
         break;
   
   
      case FRONT_CC_LFO_TARGET: //frontParser_midiMsg.status
         {
            uint8_t upper = frontParser_midiMsg.data1;
            uint8_t lower = frontParser_midiMsg.data2;
            uint8_t applyLive = seq_shouldApplyIngressToLive();
         // --AS **PATROT note that the only valid values for the following are listed in
         // the modTargets array in the AVR code
            uint8_t value = ((upper&0x01)<<7) | lower;
         
            uint8_t lfoNr = (upper&0xfe)>>1;
            seq_storeLfoDestinationIngress(lfoNr, value);
            
            if(!applyLive)
            {
               /* RESTORE: Endpoint-copy automation target sidebands are stored only.
                  They must not touch the currently sounding modulation nodes. */
            }
            else if(seq_voicesLoading&(0x01<<(lfoNr)))
            {
               midi_midiLfoCache[lfoNr]=value;
               midi_midiLfoCacheAvailable[lfoNr]=1;
            }
            else
            {
               switch(lfoNr)
               {
                  case 0:
                  case 1:
                  case 2:	modNode_setDestination(&voiceArray[lfoNr].lfo.modTarget, value);
                     break;
                  case 3:	modNode_setDestination(&snareVoice.lfo.modTarget,value);		
                     break;
                  case 4:	modNode_setDestination(&cymbalVoice.lfo.modTarget, value);		
                     break;
                  case 5:	modNode_setDestination(&hatVoice.lfo.modTarget, value);			
                     break;
                  default:
                     break;
               }
            }
         }
         break; // case FRONT_CC_LFO_TARGET
   
      case FRONT_SET_P1_DEST: 
         { // frontParser_midiMsg.status
         // --AS **AUTOM add 1 to the value as our cortex parameters are off by 1 for the lower 127 params
            uint8_t hi = frontParser_midiMsg.data1;
            uint8_t lo = frontParser_midiMsg.data2;
            uint8_t val = (hi<<7)|lo;
            if(val && val < 128 )
               val++;
            seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, seq_selectedStep)->param1Nr = val;
         }
         break;
      case FRONT_SET_P2_DEST: 
         { // frontParser_midiMsg.status
         //--AS **AUTOM same here
            uint8_t hi = frontParser_midiMsg.data1;
            uint8_t lo = frontParser_midiMsg.data2;
            uint8_t val = (hi<<7)|lo;
            if(val && val < 128 )
               val++;
            seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, seq_selectedStep)->param2Nr = val;
         }
         break;
      case FRONT_SET_P1_VAL: 
         { // frontParser_midiMsg.status
            uint8_t stepNr = frontParser_midiMsg.data1;
            uint8_t value = frontParser_midiMsg.data2;
            seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, stepNr)->param1Val = value;
         
         }
         break;
      case FRONT_SET_P2_VAL: 
         { // frontParser_midiMsg.status
            uint8_t stepNr = frontParser_midiMsg.data1;
            uint8_t value = frontParser_midiMsg.data2;
            seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, stepNr)->param2Val = value;
         }
         break;
   
      case FRONT_ARM_AUTOMATION_STEP: // frontParser_midiMsg.status
         {
            const uint8_t stepNr 	= frontParser_midiMsg.data1;
            const uint8_t onOff 	= frontParser_midiMsg.data2 & 0x40;
            const uint8_t trackNr 	=  frontParser_midiMsg.data2 & 0x3f;
         
            seq_armAutomationStep(stepNr,trackNr,onOff);
         
         }
         break;
   
      case FRONT_MAIN_STEP_CC: // frontParser_midiMsg.status
         {
         //data 1 = track und pattern nr
         //data 2 = step nr
            uint8_t voiceNr 	= frontParser_midiMsg.data1 >> 4;
            uint8_t patternNr 	= seq_normalizePatternNumber(frontParser_midiMsg.data1 & 0x0f);
            uint8_t stepNr 		= frontParser_midiMsg.data2 & 0x1f;
         
         
            if (frontParser_midiMsg.data2 & 0x40) // flag for force ON
            {
               seq_setMainStep(patternNr, voiceNr, stepNr, 1);
            }
            else if (frontParser_midiMsg.data2 & 0x20) // flag for force OFF
            {
               seq_setMainStep(patternNr, voiceNr, stepNr, 0);
            }
            
            else  //toggle the step in the seq
            {
               seq_toggleMainStep(voiceNr, stepNr, patternNr);
            }   
         
         //if step active send led on message to front
            if(seq_isMainStepActive(voiceNr, stepNr, patternNr))
            {
               uart_sendFrontpanelByte(FRONT_STEP_LED_STATUS_BYTE);
               uart_sendFrontpanelByte(FRONT_LED_SEQ_BUTTON);
               uart_sendFrontpanelByte(stepNr*8);
            }
         }
         break;
   
      case FRONT_STEP_CC: // frontParser_midiMsg.status
         {
         //data 1 = track und pattern nr
         //data 2 = step nr
            uint8_t voiceNr 	= frontParser_midiMsg.data1 >> 4;
            uint8_t patternNr 	= seq_normalizePatternNumber(frontParser_midiMsg.data1 & 0x0f);
            uint8_t stepNr 		= frontParser_midiMsg.data2;
         
         //toggle the step in the seq
            seq_toggleStep(voiceNr, stepNr, patternNr);
         
         }
         break;
   
      case FRONT_CC_VELO_TARGET: // frontParser_midiMsg.status
         {
            uint8_t upper = frontParser_midiMsg.data1;
            uint8_t lower = frontParser_midiMsg.data2;
            uint8_t applyLive = seq_shouldApplyIngressToLive();
         // --AS **PATROT note that the only valid values for the following are listed in
         // the modTargets array in the AVR code
            uint8_t value = ((upper&0x01)<<7) | lower;
            uint8_t velModNr = (upper&0xfe)>>1;
            seq_storeVelocityDestinationIngress(velModNr, value);
            if(!applyLive)
            {
               /* RESTORE: Endpoint-copy automation target sidebands are stored only.
                  They must not touch the currently sounding modulation nodes. */
            }
            else if(seq_voicesLoading&(0x01<<(velModNr)))
            {
               midi_midiVeloCache[velModNr]=value;
               midi_midiVeloCacheAvailable[velModNr]=1;
            }
            else
            {
               modNode_setDestination(&velocityModulators[velModNr], value);
            }
         }
         break;
   
   //VOICE option Messages
      case VOICE_CC: // frontParser_midiMsg.status
         break;
   
   //BPM MESSAGE
      case FRONT_SET_BPM: 
         { // frontParser_midiMsg.status
            uint16_t bpm = frontParser_midiMsg.data1 |(uint16_t)(frontParser_midiMsg.data2<<7);
            if(bpm == 0) {
               seq_setExtSync(1);
            }
            else {
               seq_setBpm(bpm);
            
               seq_setExtSync(0);
            }
         }
         break;
   
   //LED MESSAGES
      case FRONT_STEP_LED_STATUS_BYTE: // frontParser_midiMsg.status
         switch(frontParser_midiMsg.data1)
         {
         //send all active step numbers to frontpanel to light up corresponding LEDs
            case FRONT_LED_QUERY_SEQ_TRACK:
               {
                  uint8_t trackNr = frontParser_midiMsg.data2 >> 4;
                  uint8_t patternNr = seq_normalizePatternNumber(frontParser_midiMsg.data2 & 0x0f);
               
                  frontParser_updateTrackLeds(trackNr, patternNr);
               
               //send track length back
                  uart_sendFrontpanelByte(FRONT_SEQ_CC);
                  uart_sendFrontpanelByte(FRONT_SEQ_TRACK_LENGTH);
                  uart_sendFrontpanelByte(seq_getTrackLength(trackNr));
               
               // **PATROT send track rotation value back
                  uart_sendFrontpanelByte(FRONT_SEQ_CC);
                  uart_sendFrontpanelByte(FRONT_SEQ_TRACK_ROTATION);
                  uart_sendFrontpanelByte(seq_getTrackRotation(trackNr));
               
               }
               break;
            case FRONT_LED_ALL_SUBSTEP:
               {
                  uint8_t trackNr = frontParser_midiMsg.data2 >> 4;
                  uint8_t patternNr = seq_normalizePatternNumber(frontParser_midiMsg.data2 & 0x0f);
               
                  frontParser_updateSubStepLeds(trackNr, patternNr);               
               }
               break;
         
            default:
               break;
         }
         break;
   } // frontParser_midiMsg.status

   frontParser_flowMessageApplied();
}
//------------------------------------------------------
// Sequencer message handler
// This is called when we have received a message with status FRONT_SEQ_CC
static void frontParser_handleSeqCC()
{
   switch(frontParser_midiMsg.data1)
   {
      case FRONT_SEQ_FLOW_BEGIN:
         if(frontParser_midiMsg.data2 == FLOW_CH_LOAD_SESSION)
         {
            comm_loadSessionActive = 1;
            comm_quietUi = 1;
            comm_flowActive = 0;
            comm_flowBudgetRemaining = 0;
            frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         }
         else
         {
            comm_flowActive = 1;
            comm_flowChannel = (uint8_t)(frontParser_midiMsg.data2 & 0x07);
            comm_flowBudgetRemaining = FLOW_INITIAL_GRANT;
            frontParser_sendFlowGrant(comm_flowChannel, FLOW_INITIAL_GRANT);
         }
         break;

      case FRONT_SEQ_FLOW_END:
         if(frontParser_midiMsg.data2 == FLOW_CH_LOAD_SESSION)
         {
            comm_loadSessionActive = 0;
            comm_quietUi = 0;
            comm_flowActive = 0;
            comm_flowBudgetRemaining = 0;
            frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         }
         else
         {
            uint8_t channel = (uint8_t)(frontParser_midiMsg.data2 & 0x07);
            if(comm_flowActive && (comm_flowChannel == channel))
            {
               comm_flowActive = 0;
               comm_flowBudgetRemaining = 0;
            }
            frontParser_sendFlowGrant(channel, FLOW_ACK_CREDITS);
         }
         break;

      case FRONT_SEQ_FLOW_ABORT:
         comm_loadSessionActive = 0;
         comm_quietUi = 0;
         comm_flowActive = 0;
         comm_flowBudgetRemaining = 0;
         frontParser_endFileLoadIngress();
         frontParser_clearPrfCacheSession();
         frontParser_clearDeferredPerfLoad();
         break;

      case FRONT_SEQ_FLOW_GRANT:
         break;

      case FRONT_SEQ_PRF_CACHE_BEGIN:
      {
         uint8_t cacheAccepted = PRF_CACHE_REJECTED;

         frontParser_clearPrfCacheSession();
         if((frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE)
            && seq_isRunning()
            && frontParser_deferPerfLoadCacheUntilPatternChange)
         {
            frontParser_prfCacheProtectedPattern = seq_activePattern & 0x07;
            frontParser_capturePrfStmLiveSnapshot();
            frontParser_prfCacheState = PRF_CACHE_LIVE_ACTIVE;
            frontParser_prfCachePendingValid = 0;
            frontParser_clearDeferredPerfLoad();
            cacheAccepted = PRF_CACHE_ACCEPTED;
         }
         frontParser_sendPrfCacheStatus(FRONT_SEQ_PRF_CACHE_BEGIN, cacheAccepted);
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;
      }

      case FRONT_SEQ_PRF_PENDING_BEGIN:
         if(frontParser_prfCacheState != PRF_CACHE_IDLE)
         {
            frontParser_resetPrfPendingCounters();
            frontParser_prfCachePendingValid = 0;
            frontParser_prfCacheState = PRF_CACHE_RECEIVING_PENDING;
         }
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_PENDING_DONE:
         if(frontParser_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
         {
            if(frontParser_prfPendingCountsValid())
            {
               frontParser_prfCachePendingValid = 1;
               frontParser_prfCacheState = PRF_CACHE_PENDING_VALID;
               frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
            }
            else
            {
               frontParser_prfCacheState = PRF_CACHE_ABORTING;
               frontParser_clearPrfCacheSession();
               frontParser_clearDeferredPerfLoad();
               frontParser_sendFlowAbort(FLOW_CH_LOAD_SESSION);
            }
         }
         else
         {
            frontParser_sendFlowAbort(FLOW_CH_LOAD_SESSION);
         }
         break;

      case FRONT_SEQ_PRF_CACHE_ABORT:
         frontParser_prfCacheState = PRF_CACHE_ABORTING;
         frontParser_endFileLoadIngress();
         frontParser_clearPrfCacheSession();
         frontParser_clearDeferredPerfLoad();
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_AVR_SNAPSHOT_BEGIN:
         if(frontParser_prfCacheState != PRF_CACHE_IDLE)
         {
            frontParser_prfCacheState = PRF_CACHE_RECEIVING_AVR_LIVE;
            frontParser_prfCacheAvrLiveValid = 0;
         }
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_AVR_SNAPSHOT_END:
         if(frontParser_prfCacheState == PRF_CACHE_RECEIVING_AVR_LIVE)
         {
            frontParser_prfCacheAvrLiveValid = 1;
            frontParser_prfCacheState = PRF_CACHE_LIVE_ACTIVE;
         }
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_RESTORE_AVR_LIVE:
         frontParser_sendPrfLiveRestore();
         frontParser_sendFlowGrantWait(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN:
      {
         uint8_t endpointMode = frontParser_midiMsg.data2;

         /* RESTORE: Switch ingress target to normal kit endpoint buffer.
            Subsequent parameter and target messages will populate seq_normalKitState endpoint images.
            The data byte selects which endpoint group is being refreshed, so file
            loads can update only the kit/front endpoint or only the morph parameter
            endpoint without clearing the other side. */
         seq_setIngressTarget(SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT);
         seq_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);

         if(endpointMode != FRONT_SEQ_TMP_KIT_ENDPOINT_MORPH_ONLY)
         {
            memset(seq_normalKitState.frontPanelParams, 0, END_OF_SOUND_PARAMETERS);
            memset(&seq_normalKitState.frontPanelAutomationTargets,
                   0,
                   sizeof(seq_normalKitState.frontPanelAutomationTargets));
         }

         if(endpointMode != FRONT_SEQ_TMP_KIT_ENDPOINT_FRONT_ONLY)
         {
            memset(seq_normalKitState.morphParams, 0, END_OF_SOUND_PARAMETERS);
            memset(&seq_normalKitState.morphParameterEndpointAutomationTargets,
                   0,
                   sizeof(seq_normalKitState.morphParameterEndpointAutomationTargets));
         }
         break;
      }

      case FRONT_SEQ_TMP_KIT_AUTOMATION_PHASE:
         /* RESTORE: Inside the copy-to-temp endpoint bracket, raw param opcodes
            identify parameter_values[] vs parameters2[]. Resolved automation target
            sidebands need this explicit phase marker so the STM stores them with
            the matching endpoint image and does not apply them to live audio. */
         if(frontParser_midiMsg.data2 == FRONT_SEQ_TMP_KIT_AUTOMATION_FRONT_ENDPOINT)
            seq_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_FRONT_ENDPOINT);
         else if(frontParser_midiMsg.data2 == FRONT_SEQ_TMP_KIT_AUTOMATION_MORPH_ENDPOINT)
            seq_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_MORPH_ENDPOINT);
         else
            seq_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
         break;

      case FRONT_SEQ_TMP_KIT_ENDPOINT_END:
         seq_applyNormalEndpointAutomationTargets();

         /* RESTORE: Switch ingress target back to the surrounding context. During
            file load, endpoint dumps are nested inside normal kit-endpoint ingress. */
         seq_setIngressTarget(frontParser_fileLoadIngressActive
                              ? SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT
                              : SEQ_PARAM_INGRESS_CURRENT_IMAGE);
         seq_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
         break;

      case FRONT_SEQ_SET_GLOBAL_MORPH:
         /* AVR menu/global morph resets all STM per-voice morph amounts. Per
            voice step automation or modulation may later overwrite individual
            voices without asking AVR to recompute morph. */
         seq_setGlobalMorphAmount(frontParser_midiMsg.data2);
         break;

      case FRONT_SEQ_REQUEST_PATTERN_PARAMS:

      /* send back bar change and next pattern params from requested pattern*/
         uart_sendFrontpanelByte(FRONT_SEQ_CC);
         uart_sendFrontpanelByte(FRONT_SEQ_SET_PAT_BEAT);
         uart_sendFrontpanelByte(seq_getPatternSettingPtr(frontParser_shownPattern)->changeBar);
      
         uart_sendFrontpanelByte(FRONT_SEQ_CC);
         uart_sendFrontpanelByte(FRONT_SEQ_SET_PAT_NEXT);
         uart_sendFrontpanelByte(seq_getPatternSettingPtr(frontParser_shownPattern)->nextPattern);
      
      
         break;
      case FRONT_SEQ_SET_PAT_BEAT:
         seq_getPatternSettingPtr(frontParser_shownPattern)->changeBar = frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_SET_PAT_NEXT:
         seq_getPatternSettingPtr(frontParser_shownPattern)->nextPattern = frontParser_midiMsg.data2;
         break;
   
      case FRONT_SEQ_REC_ON_OFF:
         seq_setRecordingMode(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_ERASE_ON_OFF:
         seq_setErasingMode(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_NOTE:
         seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, frontParser_activeStep)->note = frontParser_midiMsg.data2;
         break;
   
      case FRONT_SEQ_VOLUME:
         seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, frontParser_activeStep)->volume &= ~(0x7f);
         seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, frontParser_activeStep)->volume |= (frontParser_midiMsg.data2&0x7f);
         break;
   
      case FRONT_SEQ_PROB:
      
         seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, frontParser_activeStep)->prob = frontParser_midiMsg.data2;
         break;
         
      case FRONT_SEQ_EUKLID_RESET:
         {
            euklid_clearRotation();
         }
         break;
   
      case FRONT_SEQ_EUKLID_LENGTH:
         {
            uint8_t pattern = (frontParser_shownPattern == SEQ_TMP_PATTERN) ? SEQ_TMP_PATTERN : (frontParser_midiMsg.data2&0x07);
            uint8_t length 	= frontParser_midiMsg.data2 >> 3;
            length += 1;
            euklid_setLength(frontParser_activeTrack, pattern, length);
            frontParser_updateTrackLeds(frontParser_activeTrack, pattern);
         }
         break;
   
      case FRONT_SEQ_EUKLID_STEPS:
         {
            uint8_t steps 	= frontParser_midiMsg.data2 >> 3;
            steps += 1;
            uint8_t pattern = (frontParser_shownPattern == SEQ_TMP_PATTERN) ? SEQ_TMP_PATTERN : (frontParser_midiMsg.data2 & 0x7);
         
            euklid_setSteps(frontParser_activeTrack,steps,pattern);
            frontParser_updateTrackLeds(frontParser_activeTrack, pattern);
         }
         break;
   
      case FRONT_SEQ_EUKLID_ROTATION:
         {
            uint8_t rotation = frontParser_midiMsg.data2 >> 3;
         //rotation += 1;
            uint8_t pattern = (frontParser_shownPattern == SEQ_TMP_PATTERN) ? SEQ_TMP_PATTERN : (frontParser_midiMsg.data2 & 0x7);
         
            euklid_setRotation(frontParser_activeTrack,rotation,pattern);
            frontParser_updateTrackLeds(frontParser_activeTrack, pattern);
         }
         break;
         
      case FRONT_SEQ_EUKLID_SUBSTEP_ROTATION:
         {
            uint8_t rotation = frontParser_midiMsg.data2 >> 3;
         //rotation += 1;
            uint8_t pattern = (frontParser_shownPattern == SEQ_TMP_PATTERN) ? SEQ_TMP_PATTERN : (frontParser_midiMsg.data2 & 0x7);
         
            euklid_setSubStepRotation(frontParser_activeTrack,rotation,pattern);
            frontParser_updateTrackLeds(frontParser_activeTrack, pattern);
         }
         break;   
   
      case FRONT_SEQ_CLEAR_TRACK: 
         {
            seq_clearTrack(frontParser_midiMsg.data2, frontParser_shownPattern);
         }
         break;
   
      case FRONT_SEQ_CLEAR_PATTERN:
         seq_clearPattern(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_POSX:
         som_setX(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_POSY:
         som_setY(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_FLUX:
         som_setFlux(frontParser_midiMsg.data2/127.f);
         break;
   
      case FRONT_SEQ_SOM_FREQ:
         som_setFreq(frontParser_midiMsg.data2,frontParser_activeTrack);
         break;
   
      case FRONT_SEQ_MIDI_CHAN:
         {
            uint8_t voice = (frontParser_midiMsg.data2 >> 4)&0x07;
            uint8_t channel = (frontParser_midiMsg.data2&0x0f)+1;
            
            // --AS if midi channel changed, and a note was playing on old channel, turn it off
            if(voice < 7 && midi_MidiChannels[voice] != channel)
               voiceControl_noteOff(voice);
               
            midi_MidiChannels[voice] = channel;
            
         }
         break;
         
      case FRONT_SEQ_MIDI_CHAN_OFF:
         {
            midi_MidiChannels[frontParser_midiMsg.data2]=0;
         }
         break;
   
      // --AS not used anymore
      //case FRONT_SEQ_MIDI_MODE:
      //	midi_mode = frontParser_midiMsg.data2;
      //	break;
      
      
      //voice nr (0xf0) + autom track nr (0x0f)
      case FRONT_SEQ_CLEAR_AUTOM:
         {
            const uint8_t voice 		= frontParser_midiMsg.data2 >> 4;
            const uint8_t automTrack 	= frontParser_midiMsg.data2 &  0x0f;
            seq_clearAutomation(voice, frontParser_shownPattern, automTrack);
         }
         break;
   
      case FRONT_SEQ_COPY_TRACK:
         {
            const uint8_t src = frontParser_midiMsg.data2>>4;
            const uint8_t dst = frontParser_midiMsg.data2&0xf;
            seq_copyTrack(src,dst,frontParser_shownPattern);
         }
         break;
   
      case FRONT_SEQ_COPY_PATTERN:
         {
            const uint8_t src = frontParser_midiMsg.data2>>4;
            const uint8_t dst = frontParser_midiMsg.data2&0xf;
            seq_copyPattern(src,dst);
         }
         break;
         
      case FRONT_SEQ_COPY_TRACK_PATTERN:
         {
            const uint8_t srcNr = frontParser_midiMsg.data2>>4;
            const uint8_t dstPat = frontParser_midiMsg.data2&0xf;
            seq_copyTrackPattern(srcNr,dstPat,frontParser_shownPattern);
         }
         break;
         
      case FRONT_SEQ_COPY_SRC:
         {
            frontParser_stepCopySource = frontParser_midiMsg.data2;
         }
         break;
      
      case FRONT_SEQ_COPY_DST:
         {
            seq_copySubStep(frontParser_stepCopySource,frontParser_midiMsg.data2,frontParser_activeTrack);
         }
         break;
   
      case FRONT_SEQ_TRACK_LENGTH:
         seq_setTrackLength(frontParser_activeTrack,frontParser_midiMsg.data2);
         break;
         
      case FRONT_SEQ_TRACK_SCALE:
         seq_setTrackScale(frontParser_activeTrack,frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_TRACK_ROTATION: //**PATROT handle incoming track rotation. apply to active track
         seq_setTrackRotation(frontParser_activeTrack,frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_SHUFFLE:
         seq_setShuffle(frontParser_midiMsg.data2/127.f);
         break;
   
      case FRONT_SEQ_SELECT_ACTIVE_STEP:
         seq_selectedStep = frontParser_midiMsg.data2;
         break;
   
      case FRONT_SEQ_SET_AUTOM_TRACK:
         seq_setActiveAutomationTrack(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_SET_QUANT:
         seq_setQuantisation(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_REQUEST_EUKLID_PARAMS:
         uart_sendFrontpanelByte(FRONT_SEQ_CC);
         uart_sendFrontpanelByte(FRONT_SEQ_EUKLID_LENGTH);
         uart_sendFrontpanelByte(euklid_getLength(frontParser_midiMsg.data2));
      
         uart_sendFrontpanelByte(FRONT_SEQ_CC);
         uart_sendFrontpanelByte(FRONT_SEQ_EUKLID_STEPS);
         uart_sendFrontpanelByte(euklid_getSteps(frontParser_midiMsg.data2));
      
         uart_sendFrontpanelByte(FRONT_SEQ_CC);
         uart_sendFrontpanelByte(FRONT_SEQ_EUKLID_ROTATION);
         uart_sendFrontpanelByte(euklid_getRotation(frontParser_midiMsg.data2));
         
         uart_sendFrontpanelByte(FRONT_SEQ_CC);
         uart_sendFrontpanelByte(FRONT_SEQ_EUKLID_SUBSTEP_ROTATION);
         uart_sendFrontpanelByte(euklid_getSubStepRotation(frontParser_midiMsg.data2));
         
         uart_sendFrontpanelByte(FRONT_SEQ_CC);
         uart_sendFrontpanelByte(FRONT_SEQ_TRACK_SCALE);
         uart_sendFrontpanelByte(seq_getTrackScale(frontParser_midiMsg.data2));
         break;
   
      case FRONT_SEQ_SET_SHOWN_PATTERN:
         if (frontParser_midiMsg.data2==frontParser_shownPattern)
         {
            if(!seq_consumeTmpBoundaryPatternSwitchAck())
               seq_realign();
         }
         else
         {
            (void)seq_consumeTmpBoundaryPatternSwitchAck();
            frontParser_shownPattern = seq_normalizePatternNumber(frontParser_midiMsg.data2);
         }
         break;   
      case FRONT_SEQ_SET_ACTIVE_TRACK:
         if ( (frontParser_activeTrack==frontParser_midiMsg.data2)&&(!seq_isRunning()) )
            seq_triggerVoice(frontParser_activeTrack, seq_rollVelocity, seq_rollNote);
            
         frontParser_activeTrack = frontParser_midiMsg.data2;
         uart_sendFrontpanelByte(FRONT_SEQ_CC);
         uart_sendFrontpanelByte(FRONT_SEQ_TRACK_ROTATION);
         uart_sendFrontpanelByte(seq_getTrackRotation(frontParser_activeTrack));
         
         uart_sendFrontpanelByte(FRONT_SEQ_CC);
         uart_sendFrontpanelByte(FRONT_SEQ_TRANSPOSE);
         uart_sendFrontpanelByte(seq_transpose_voiceAmount[frontParser_activeTrack]);
         
         break;
   
      case FRONT_SEQ_REQUEST_STEP_PARAMS:
         {
            Step *step = seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, frontParser_midiMsg.data2);
         
         
         /* send back probability, volume and note nr*/
            uart_sendFrontpanelByte(FRONT_SEQ_CC);
            uart_sendFrontpanelByte(FRONT_SEQ_VOLUME);
            uart_sendFrontpanelByte(step->volume&STEP_VOLUME_MASK);
         
            uart_sendFrontpanelByte(FRONT_SEQ_CC);
            uart_sendFrontpanelByte(FRONT_SEQ_NOTE);
            uart_sendFrontpanelByte(step->note);
         
            uart_sendFrontpanelByte(FRONT_SEQ_CC);
            uart_sendFrontpanelByte(FRONT_SEQ_PROB);
            uart_sendFrontpanelByte(step->prob);
         
         //send back automation params
         // --AS **AUTOM subtract one for differing offsets when parameter is < 128
            uint8_t hi,lo;
            uint8_t dest = step->param1Nr;
            if(dest < 128 && dest)
               dest--;
            hi = dest>>7;
            lo = dest&0x7f;
            uart_sendFrontpanelByte(FRONT_SET_P1_DEST);
            uart_sendFrontpanelByte(hi);
            uart_sendFrontpanelByte(lo);
         
            uint8_t val = step->param1Val;
            hi = val>>7;
            lo = val&0x7f;
            uart_sendFrontpanelByte(FRONT_SET_P1_VAL);
            uart_sendFrontpanelByte(hi);
            uart_sendFrontpanelByte(lo);
         // --AS **AUTOM subtract one for differing offsets
            dest = step->param2Nr;
            if(dest < 128 && dest)
               dest--;
            hi = dest>>7;
            lo = dest&0x7f;
            uart_sendFrontpanelByte(FRONT_SET_P2_DEST);
            uart_sendFrontpanelByte(hi);
            uart_sendFrontpanelByte(lo);
         
            val = step->param2Val;
            hi = val>>7;
            lo = val&0x7f;
            uart_sendFrontpanelByte(FRONT_SET_P2_VAL);
            uart_sendFrontpanelByte(hi);
            uart_sendFrontpanelByte(lo);
         
         
         
            frontParser_activeStep = frontParser_midiMsg.data2;
         }
         break;
   
      case FRONT_SEQ_ROLL_RATE:
         seq_setRollRate(frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_ROLL_NOTE:
         seq_setRollNote(frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_ROLL_VELOCITY:
         seq_setRollVelocity(frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_ROLL_MODE:
         if(frontParser_midiMsg.data2==ROLL_MODE_FIRST_ON)
            seq_skipFirstRoll=0;
         else if(frontParser_midiMsg.data2==ROLL_MODE_FIRST_OFF)
            seq_skipFirstRoll=1;   
         else
            seq_rollMode = frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_TRANSPOSE:
         seq_transpose_voiceAmount[frontParser_activeTrack]=frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_TRANSPOSE_ON_OFF:
         if (frontParser_midiMsg.data2==0x0f)
            seq_writeTranspose();
         else if (frontParser_midiMsg.data2<=1)
            seq_transposeOnOff = frontParser_midiMsg.data2;
         break;
   
      case FRONT_SEQ_ROLL_ON_OFF:
         {
         
            const uint8_t onOff = frontParser_midiMsg.data2 >> 4;
            const uint8_t voice = frontParser_midiMsg.data2 & 0xf;
            seq_rollChange(voice,onOff);
         }
         break;
   
      case FRONT_SEQ_CHANGE_PAT:
         //switch to one of the 8 patterns on the next pattern start
         seq_setNextPattern( (frontParser_midiMsg.data2&0x07),(frontParser_midiMsg.data2>>3) );
         break;

      case FRONT_SEQ_CHANGE_TMP_PAT:
         seq_setNextPattern(SEQ_TMP_PATTERN, (frontParser_midiMsg.data2>>3));
         break;
   
   
      case FRONT_SEQ_RUN_STOP:
         if(frontParser_midiMsg.data2 == 0)
            frontParser_applyDeferredVoiceCache();
         seq_setRunning(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_MUTE_TRACK:
         seq_setMute(frontParser_midiMsg.data2,1);
         break;
   
      case FRONT_SEQ_UNMUTE_TRACK:
         seq_setMute(frontParser_midiMsg.data2,0);
         break;
      case FRONT_SEQ_MIDI_ROUTING:
         midiParser_setRouting(frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_MIDI_TX_FILTER:
      case FRONT_SEQ_MIDI_RX_FILTER:
         midiParser_setFilter(frontParser_midiMsg.data1==FRONT_SEQ_MIDI_TX_FILTER, frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_BAR_RESET_MODE:
      // --AS a setting of 0 is default (keep track of bars in song), a setting of 1 is
      // to reset the bar counter when a manual pattern change occurs
         seq_resetBarOnPatternChange = frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_PC_TIME_MODE:
      // a setting of 0 is default (pattern changes at end of current bar
      // a setting of 1 causes pattern to change on next step
         switchOnNextStep = frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_TRIGGER_IN_PPQ:
         switch(frontParser_midiMsg.data2)
         {
            case 0:
               trigger_prescalerClockInput = PRE_1_PPQ;
               break;
         
            default:
            case 1:
               trigger_prescalerClockInput = PRE_4_PPQ;
               break;
         
            case 2:
               trigger_prescalerClockInput = PRE_8_PPQ;
               break;
            case 3:
               trigger_prescalerClockInput = PRE_16_PPQ;
               break;
            case 4:
               trigger_prescalerClockInput = PRE_32_PPQ;
               break;
         }
         break;
      case FRONT_SEQ_TRIGGER_OUT1_PPQ:
         switch(frontParser_midiMsg.data2)
         {
            case 0:
               trigger_dividerClockOut1 = PRE_1_PPQ;
               break;
         
            default:
            case 1:
               trigger_dividerClockOut1 = PRE_4_PPQ;
               break;
         
            case 2:
               trigger_dividerClockOut1 = PRE_8_PPQ;
               break;
            case 3:
               trigger_dividerClockOut1 = PRE_16_PPQ;
               break;
            case 4:
               trigger_dividerClockOut1 = PRE_32_PPQ;
               break;
         }
      
         break;
   
      case FRONT_SEQ_TRIGGER_OUT2_PPQ:
         switch(frontParser_midiMsg.data2)
         {
            case 0:
               trigger_dividerClockOut2 = PRE_1_PPQ;
               break;
         
            default:
            case 1:
               trigger_dividerClockOut2 = PRE_4_PPQ;
               break;
         
            case 2:
               trigger_dividerClockOut2 = PRE_8_PPQ;
               break;
            case 3:
               trigger_dividerClockOut2 = PRE_16_PPQ;
               break;
            case 4:
               trigger_dividerClockOut2 = PRE_32_PPQ;
               break;
               break;
         }
         break;
   
      case FRONT_SEQ_TRIGGER_GATE_MODE:
         trigger_setGatemode(frontParser_midiMsg.data2);
         break;
         
      case FRONT_SEQ_SET_LOOP:
         seq_setLoop(frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_LOAD_VOICE:
         /*
         seq_voicesLoading is only used in Mainboard>frontPanelParser.c 
         it applies to case MIDI_CC and FRONT_CC_2, LFO_TARGET and VELO_TARGET. 
         if this flag is set, the parameter is sent to cache. 
         cache needs to be opened before any cached parameters are applied
         
         Further notes:
         case FRONT_SEQ_LOAD_VOICE:
            sets seq_voicesLoading (bit register)
            seq_voicesLoading is only used in Mainboard>frontPanelParser.c 
            it applies to case MIDI_CC and FRONT_CC_2, LFO_TARGET and VELO_TARGET. 
            if this flag is set, the parameter received to fpp is sent to cache. 
            midi_midi<type>CacheAvailable is the flag for each parameter for reg. 
            MIDI, Lfo, and Velo.
            
            voice needs to be *uncached* before any cached parameters are applied
            
            seq_voicesLoading is UNSET by:
            - FRONT_SEQ_FILE_DONE (completely set to 0)
            - frontParser_uncacheVoice(voice number), turns off each individual flag.
            
            case FRONT_SEQ_UNHOLD_VOICE (data for voice number):
            calls frontParser_unholdVoice(voice). also calls fp_uncacheVoice if seq not 
            running.
            frontParser_unHoldVoice:
            	- sets seq_newVoiceAvailable (a register) for the voice
            	- then this waits for one of 2 cases (below) to actually update the voice
            	- searches cacheAvailable to see if any new params need to be applied
            	- sends the param to midi_midiKit, midi_kitLfoCache, or midi_kitVeloCache. 
               this is not the actual voiced parameter. those get applied below:
            
            	CASE 1: global load fast mode is on. voice params get applied 
               (frontParser_uncacheVoice) when seq_triggerVoice(voice #) is called. They 
               also change over if the sequence is changed as case 2 below.
            
            	CASE 2: global load fast mode is off. voice params get applied 
               (frontParser_uncacheVoice) when the sequence changes 
               (flag seq_newPatternExecuted). This happens if the voice is trigged OR on 
               corresponding substep(mod 8) (0=drum1, 3=snare etc.). when 
            
            frontParser_uncacheVoice(voice) actually applies the parameters
            
            
            NOTES:
            PATCH_RESET sets seq_newVoiceAvailable for ALL.
            
            seq_tracksLocked
            - only used in seq_triggerVoice to return. ie the voice does not play at all.
            - set only by SYSEX_RECEIVE_MAIN_STEP_DATA, unset by SYSEX_BEGIN_PATTERN_TRANSMIT
         */
         
         frontParser_beginFileLoadIngress(0);
         frontParser_clearVoiceCache(frontParser_midiMsg.data2);
         seq_voicesLoading |= (0x01<<frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_UNHOLD_VOICE:
         if(frontParser_deferredPerfLoadActive)
         {
            frontParser_deferPerfUnholdVoice(frontParser_midiMsg.data2);
            break;
         }
         frontParser_unholdLoadedVoice(frontParser_midiMsg.data2);
         seq_voicesLoading &= ~(0x01<<frontParser_midiMsg.data2);
         if(!seq_isRunning())
            frontParser_releaseVoiceCache(frontParser_midiMsg.data2);
         if(!frontParser_fileLoadBracketActive && !seq_voicesLoading)
            frontParser_endFileLoadIngress();
         break;
      case FRONT_SEQ_LOAD_FAST:
         seq_loadFastMode=frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_FILE_BEGIN:
         frontParser_beginFileLoadIngress(1);
         frontParser_clearDeferredPerfLoad();
         seq_resetVoiceMorphAmountsToGlobal();
         seq_resetLiveMorphApplyCache();
         if((frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE)
            || (frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_ALL))
         {
            seq_morphLoadDisabled = 1;
            seq_vMorphFlag = 0;
         }
         if((frontParser_midiMsg.data2 != FRONT_FILE_DONE_TYPE_PERFORMANCE)
            || (frontParser_prfCacheState == PRF_CACHE_IDLE))
         {
            frontParser_clearPrfCacheSession();
         }
         break;
      case FRONT_SEQ_FILE_DONE:
         if((frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE)
            || (frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_ALL))
         {
            seq_morphLoadDisabled = 0;
            seq_vMorphFlag = 0;
         }
         if(frontParser_prfCacheSessionActive()
            && (frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE))
         {
            frontParser_deferredPerfLoadActive = 0;
         }
         else if(frontParser_deferredPerfLoadActive
            && (frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE))
         {
            frontParser_deferredPerfVoiceCachePending = 1;
            frontParser_deferredPerfLoadActive = 0;
         }
         else
         {
            frontParser_deferredPerfLoadActive = 0;
            frontParser_applyPendingVoiceCache();
            frontParser_clearDeferredPerfLoad();
         }
         seq_voicesLoading=0;
         frontParser_endFileLoadIngress();
         break;
      case FRONT_SEQ_TRACK_NOTE1:
      case FRONT_SEQ_TRACK_NOTE2:
      case FRONT_SEQ_TRACK_NOTE3:
      case FRONT_SEQ_TRACK_NOTE4:
      case FRONT_SEQ_TRACK_NOTE5:
      case FRONT_SEQ_TRACK_NOTE6:
      case FRONT_SEQ_TRACK_NOTE7:
         midi_NoteOverride[frontParser_midiMsg.data1-FRONT_SEQ_TRACK_NOTE1] = frontParser_midiMsg.data2;
         break;
      default:
         break;
   }
}
