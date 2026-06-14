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
#include "FrontPanelProtocol.h"
#include "MidiParser.h"
#include "Preset/EndpointRestore.h"
#include "Preset/MorphEngine.h"
#include "Preset/ParameterArray.h"
#include "sequencer.h"
#include "PatternData.h"
#include "Preset/ParameterIngress.h"
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

void frontParser_handleMidiMessage(void);
static void frontParser_handleSysexData(unsigned char data);
static void frontParser_handleSeqCC();

#define FLOW_INITIAL_GRANT 4
#define FLOW_ACK_CREDITS 1
#define FRONT_FILE_DONE_TYPE_PERFORMANCE 8
#define FRONT_FILE_DONE_TYPE_ALL 9

static uint8_t comm_loadSessionActive = 0;
static uint8_t comm_quietUi = 0;
static uint8_t comm_flowActive = 0;
static uint8_t comm_flowChannel = 0;
static uint8_t comm_flowBudgetRemaining = 0;

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

/* Transitional parser-owned load/session stub. This keeps the remaining
   background-load and deferred-performance bookkeeping local while the shared
   PresetLoadCache module is removed. */
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

/* Transitional parser-owned storage for the remaining load/session bridge.
   The helpers below still expect these symbols while the shared Preset cache
   module is being retired. */
uint8_t presetLoad_deferPerfLoadCacheUntilPatternChange = 0;
uint8_t presetLoad_deferredPerfLoadActive = 0;
uint8_t presetLoad_deferredPerfVoiceCachePending = 0;
uint8_t presetLoad_deferredPerfPatternPending = 0;
uint8_t presetLoad_deferredPerfUnholdPending = 0;
uint8_t presetLoad_deferredPerfProtectedPattern = 0;
uint8_t presetLoad_deferredPerfReplay = 0;
uint8_t presetLoad_deferredPerfMsgCount = 0;
MidiMsg presetLoad_deferredPerfMsgCache[DEFERRED_PERF_MSG_CACHE_SIZE];
uint8_t presetLoad_fileLoadIngressActive = 0;
uint8_t presetLoad_fileLoadBracketActive = 0;
PrfCacheState presetLoad_prfCacheState = PRF_CACHE_IDLE;
uint8_t presetLoad_prfCacheProtectedPattern = 0;
uint8_t presetLoad_prfCachePendingValid = 0;
uint8_t presetLoad_prfCacheAvrLiveValid = 0;
uint8_t presetLoad_prfCacheStmLiveValid = 0;
TempPattern presetLoad_prfCacheLivePattern;
uint8_t presetLoad_prfCacheLiveActivePattern = 0;
uint8_t presetLoad_prfCacheLivePendingPattern = 0;
uint8_t presetLoad_prfCacheLivePerTrackActivePattern[NUM_TRACKS];
uint8_t presetLoad_prfCacheLivePerTrackPendingPattern[NUM_TRACKS];
int8_t presetLoad_prfCacheLiveStepIndex[NUM_TRACKS+1];
uint8_t presetLoad_prfCacheLiveMidiChannels[8];
uint8_t presetLoad_prfCacheLiveNoteOverride[7];
uint8_t presetLoad_prfCacheLiveVMorphAmount[7];
uint8_t presetLoad_prfCacheLiveVMorphFlag = 0;
uint8_t presetLoad_prfCacheLiveSeqVoicesLoading = 0;
uint8_t presetLoad_prfCacheLiveSeqNewVoiceAvailable = 0;
uint8_t presetLoad_prfCacheLiveSeqTracksLocked = 0;
uint8_t presetLoad_prfCacheLiveSeqLoadFastMode = 0;
uint16_t presetLoad_prfPendingMainStepCount = 0;
uint16_t presetLoad_prfPendingStepCount = 0;
uint16_t presetLoad_prfPendingLengthCount = 0;
uint16_t presetLoad_prfPendingScaleCount = 0;
uint16_t presetLoad_prfPendingChainCount = 0;
uint16_t presetLoad_prfPendingProtectedWriteCount = 0;

#define VOICE_PARAM_LENGTH 36
static uint8_t voice1presetMask[VOICE_PARAM_LENGTH]={1,8,9,20,      37,43,49,50,   62,70,74,78,  82,83,88,94,   102,108,115,121,     128,134,137,143,    149,155,161,167,    173,179,185,191,    197,203,209,215}; 
static uint8_t voice2presetMask[VOICE_PARAM_LENGTH]={2,10,11,21,    38,44,51,52,   63,71,75,79,  84,85,89,95,   103,109,116,122,     129,135,138,144,    150,156,162,168,    174,180,186,192,    198,204,210,216}; 
static uint8_t voice3presetMask[VOICE_PARAM_LENGTH]={3,12,13,22,    39,45,53,54,   64,72,76,80,  86,87,90,96,   104,110,117,123,     130,136,139,145,    151,157,163,169,    175,181,187,193,    199,205,211,217}; 
static uint8_t voice4presetMask[VOICE_PARAM_LENGTH]={4,14,15,27,28, 40,46,55,      56,65,68,73,  77,81,91,99,   105,111,118,124,     131,140,146,152,        158,164,170,    176,182,188,194,    200,206,212,218}; 
static uint8_t voice5presetMask[VOICE_PARAM_LENGTH]={6,16,17,23,    24,29,30,31,   32,41,47,57,  58,66,69,92,   100,106,112,119,125, 132,141,147,153,        159,165,171,    177,183,189,195,    201,207,213,219}; 
static uint8_t voice6presetMask[VOICE_PARAM_LENGTH]={7,18,19,25,    26,33,34,35,   36,42,48,59,  60,61,67,93,   101,107,113,120,126, 133,142,148,154,        160,166,172,    178,184,190,196,    202,208,214,220};  

static uint8_t* presetLoad_getVoicePresetMaskByVoice(uint8_t *voice)
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

static void presetLoad_clearVoiceCache(uint8_t voice)
{
   uint8_t i;
   uint8_t *presetMask = presetLoad_getVoicePresetMaskByVoice(&voice);

   if(!presetMask)
      return;

   midi_midiLfoCacheAvailable[voice] = 0;
   midi_midiVeloCacheAvailable[voice] = 0;

   for(i=0;i<VOICE_PARAM_LENGTH;i++)
   {
      midi_midiCacheAvailable[presetMask[i]] = 0;
   }
}

static void presetLoad_clearDeferredPerfLoad()
{
   presetLoad_deferredPerfLoadActive = 0;
   presetLoad_deferredPerfVoiceCachePending = 0;
   presetLoad_deferredPerfPatternPending = 0;
   presetLoad_deferredPerfUnholdPending = 0;
   presetLoad_deferredPerfMsgCount = 0;
}

static void presetLoad_beginFileLoadIngress(uint8_t bracketed)
{
   /* FILE LOAD: File payloads update the normal pattern/parameter images.
      Live edits still use SEQ_PARAM_INGRESS_CURRENT_IMAGE outside this bracket. */
   presetLoad_fileLoadIngressActive = 1;
   if(bracketed)
      presetLoad_fileLoadBracketActive = 1;
   preset_setIngressTarget(SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT);
   preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
}

static void presetLoad_endFileLoadIngress()
{
   presetLoad_fileLoadIngressActive = 0;
   presetLoad_fileLoadBracketActive = 0;
   preset_setIngressTarget(SEQ_PARAM_INGRESS_CURRENT_IMAGE);
   preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
}

static void presetLoad_resetPrfPendingCounters()
{
   presetLoad_prfPendingMainStepCount = 0;
   presetLoad_prfPendingStepCount = 0;
   presetLoad_prfPendingLengthCount = 0;
   presetLoad_prfPendingScaleCount = 0;
   presetLoad_prfPendingChainCount = 0;
   presetLoad_prfPendingProtectedWriteCount = 0;
}

static void presetLoad_clearPrfCacheSession()
{
   presetLoad_prfCacheState = PRF_CACHE_IDLE;
   presetLoad_prfCacheProtectedPattern = 0;
   presetLoad_prfCachePendingValid = 0;
   presetLoad_prfCacheAvrLiveValid = 0;
   presetLoad_prfCacheStmLiveValid = 0;
   presetLoad_resetPrfPendingCounters();
}

static void presetLoad_clearPrfRuntimeFlags()
{
   presetLoad_prfCacheLiveVMorphFlag = 0;
   preset_vMorphFlag = 0;
}

static uint8_t presetLoad_prfCacheSessionActive()
{
   return (presetLoad_prfCacheState != PRF_CACHE_IDLE)
      && (presetLoad_prfCacheState != PRF_CACHE_ABORTING);
}

static uint8_t presetLoad_prfCacheCanExit()
{
   return presetLoad_prfCacheStmLiveValid
      && presetLoad_prfCachePendingValid
      && (presetLoad_prfCacheState == PRF_CACHE_PENDING_VALID);
}

static void presetLoad_prfCacheCountPatternWrite(uint8_t pattern)
{
   if(presetLoad_prfCacheState != PRF_CACHE_RECEIVING_PENDING)
      return;

   if((pattern & 0x07) == (presetLoad_prfCacheProtectedPattern & 0x07))
      presetLoad_prfPendingProtectedWriteCount++;
}

static uint8_t presetLoad_prfPendingCountsValid()
{
   return (presetLoad_prfPendingMainStepCount >= PRF_PENDING_EXPECTED_MAINSTEP_COUNT)
      && (presetLoad_prfPendingStepCount >= PRF_PENDING_EXPECTED_STEP_COUNT)
      && (presetLoad_prfPendingLengthCount >= PRF_PENDING_EXPECTED_LENGTH_COUNT)
      && (presetLoad_prfPendingScaleCount >= PRF_PENDING_EXPECTED_SCALE_COUNT)
      && (presetLoad_prfPendingChainCount >= PRF_PENDING_EXPECTED_CHAIN_COUNT)
      && (presetLoad_prfPendingProtectedWriteCount > 0);
}

static void presetLoad_capturePrfStmLiveSnapshot()
{
   uint8_t track;
   uint8_t step;
   uint8_t pattern = (uint8_t)(presetLoad_prfCacheProtectedPattern & 0x07);

   presetLoad_prfCacheLiveActivePattern = seq_activePattern;
   presetLoad_prfCacheLivePendingPattern = preset_tempPlaybackSwitchState.pendingPattern;
   presetLoad_prfCacheLivePattern.seq_patternSettings = seq_patternSet.seq_patternSettings[pattern];

   for(track=0;track<NUM_TRACKS;track++)
   {
      presetLoad_prfCacheLivePerTrackActivePattern[track] = seq_perTrackActivePattern[track];
      presetLoad_prfCacheLivePerTrackPendingPattern[track] = preset_tempPlaybackSwitchState.perTrackPendingPattern[track];
      presetLoad_prfCacheLivePattern.seq_mainSteps[track] = seq_patternSet.seq_mainSteps[pattern][track];
      presetLoad_prfCacheLivePattern.seq_patternLengthRotate[track] =
         seq_patternSet.seq_patternLengthRotate[pattern][track];

      for(step=0;step<NUM_STEPS;step++)
      {
         presetLoad_prfCacheLivePattern.seq_subStepPattern[track][step] =
            seq_patternSet.seq_subStepPattern[pattern][track][step];
      }
   }

   for(track=0;track<NUM_TRACKS+1;track++)
      presetLoad_prfCacheLiveStepIndex[track] = seq_stepIndex[track];

   for(track=0;track<8;track++)
      presetLoad_prfCacheLiveMidiChannels[track] = midi_MidiChannels[track];

   for(track=0;track<7;track++)
   {
      presetLoad_prfCacheLiveNoteOverride[track] = midi_NoteOverride[track];
      presetLoad_prfCacheLiveVMorphAmount[track] = preset_vMorphAmount[track];
   }

   presetLoad_prfCacheLiveVMorphFlag = preset_vMorphFlag;
   presetLoad_prfCacheLiveSeqVoicesLoading = seq_voicesLoading;
   presetLoad_prfCacheLiveSeqNewVoiceAvailable = seq_newVoiceAvailable;
   presetLoad_prfCacheLiveSeqTracksLocked = seq_tracksLocked;
   presetLoad_prfCacheLiveSeqLoadFastMode = seq_loadFastMode;
   presetLoad_prfCacheStmLiveValid = 1;
}

uint8_t presetLoad_prfCacheUseLivePattern()
{
   return presetLoad_prfCacheStmLiveValid
      && (presetLoad_prfCacheState != PRF_CACHE_IDLE)
      && (presetLoad_prfCacheState != PRF_CACHE_ABORTING);
}

uint8_t presetLoad_prfCacheTrackUsesLivePattern(uint8_t track)
{
   if(track >= NUM_TRACKS || !presetLoad_prfCacheUseLivePattern())
      return 0;

   return (presetLoad_prfCacheLivePerTrackActivePattern[track] & 0x07)
      == (presetLoad_prfCacheProtectedPattern & 0x07);
}

Step* presetLoad_prfCacheLiveStep(uint8_t track, uint8_t step)
{
   if(track >= NUM_TRACKS)
      track = 0;

   return &presetLoad_prfCacheLivePattern.seq_subStepPattern[track][step & 0x7f];
}

uint16_t presetLoad_prfCacheLiveMainSteps(uint8_t track)
{
   if(track >= NUM_TRACKS)
      return 0;

   return presetLoad_prfCacheLivePattern.seq_mainSteps[track];
}

LengthRotate presetLoad_prfCacheLiveLengthRotate(uint8_t track)
{
   LengthRotate lr;
   lr.value = 0;

   if(track < NUM_TRACKS)
      lr = presetLoad_prfCacheLivePattern.seq_patternLengthRotate[track];

   return lr;
}

PatternSetting presetLoad_prfCacheLivePatternSetting()
{
   return presetLoad_prfCacheLivePattern.seq_patternSettings;
}

uint8_t presetLoad_prfCacheLiveMidiChannel(uint8_t voice)
{
   if(voice >= 8)
      return 0;

   if(presetLoad_prfCacheUseLivePattern())
      return presetLoad_prfCacheLiveMidiChannels[voice];

   return midi_MidiChannels[voice];
}

uint8_t presetLoad_prfCacheLiveNoteOverrideValue(uint8_t voice)
{
   if(voice >= 7)
      return 0;

   if(presetLoad_prfCacheUseLivePattern())
      return presetLoad_prfCacheLiveNoteOverride[voice];

   return midi_NoteOverride[voice];
}

uint8_t presetLoad_prfCacheTakeLiveVMorphFlag()
{
   uint8_t flag;

   if(presetLoad_prfCacheUseLivePattern())
   {
      flag = presetLoad_prfCacheLiveVMorphFlag;
      presetLoad_prfCacheLiveVMorphFlag = 0;
      return flag;
   }

   flag = preset_vMorphFlag;
   preset_vMorphFlag = 0;
   return flag;
}

uint8_t presetLoad_prfCacheLiveVMorphAmountValue(uint8_t voice)
{
   if(voice >= 7)
      return 0;

   if(presetLoad_prfCacheUseLivePattern())
      return presetLoad_prfCacheLiveVMorphAmount[voice];

   return preset_vMorphAmount[voice];
}

static void presetLoad_sendPrfRestoreParam(uint16_t paramNr, uint8_t value, uint8_t isMorph)
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

static void presetLoad_sendPrfLiveRestore()
{
   uint16_t i;

   if(!presetLoad_prfCacheAvrLiveValid)
      return;

   /* for(i=0;i<PRF_CACHE_LIVE_PARAM_COUNT;i++)
      presetLoad_sendPrfRestoreParam(i, presetLoad_prfCacheLiveParams[i], 0);

   for(i=0;i<END_OF_SOUND_PARAMETERS;i++)
      presetLoad_sendPrfRestoreParam(i, presetLoad_prfCacheLiveMorph[i], 1); */
}

static uint8_t presetLoad_cachePrfLiveSnapshotMessage()
{
   uint16_t paramNr;

   if(presetLoad_prfCacheState != PRF_CACHE_RECEIVING_AVR_LIVE)
      return 0;

   switch(frontParser_midiMsg.status)
   {
      case PRF_RESTORE_PARAM_CC:
         // presetLoad_prfCacheLiveParams[frontParser_midiMsg.data1] = frontParser_midiMsg.data2;
         return 1;

      case PRF_RESTORE_PARAM_CC2:
         paramNr = (uint16_t)(frontParser_midiMsg.data1 + 128);
         if(paramNr < PRF_CACHE_LIVE_PARAM_COUNT)
            // presetLoad_prfCacheLiveParams[paramNr] = frontParser_midiMsg.data2;
         return 1;

      case PRF_RESTORE_MORPH_CC:
         // presetLoad_prfCacheLiveMorph[frontParser_midiMsg.data1] = frontParser_midiMsg.data2;
         return 1;

      case PRF_RESTORE_MORPH_CC2:
         paramNr = (uint16_t)(frontParser_midiMsg.data1 + 128);
         if(paramNr < END_OF_SOUND_PARAMETERS)
            // presetLoad_prfCacheLiveMorph[paramNr] = frontParser_midiMsg.data2;
         return 1;

      default:
         return 0;
   }
}

static uint8_t presetLoad_isDeferredPerfControlMessage()
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

static uint8_t presetLoad_shouldDeferPerfMessage()
{
   if(presetLoad_prfCacheSessionActive())
      return 0;

   if(!presetLoad_deferredPerfLoadActive || presetLoad_deferredPerfReplay)
      return 0;

   if(presetLoad_isDeferredPerfControlMessage())
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

static uint8_t presetLoad_cacheDeferredPerfMessage()
{
   if(!presetLoad_shouldDeferPerfMessage())
      return 0;

   if(presetLoad_deferredPerfMsgCount < DEFERRED_PERF_MSG_CACHE_SIZE)
   {
      presetLoad_deferredPerfMsgCache[presetLoad_deferredPerfMsgCount++] = frontParser_midiMsg;
   }

   return 1;
}

static void presetLoad_applyDeferredPerfMessages()
{
   uint8_t i;

   presetLoad_beginFileLoadIngress(0);
   presetLoad_deferredPerfReplay = 1;
   for(i=0;i<presetLoad_deferredPerfMsgCount;i++)
   {
      frontParser_midiMsg = presetLoad_deferredPerfMsgCache[i];
      frontParser_handleMidiMessage();
   }
   presetLoad_deferredPerfReplay = 0;
   presetLoad_deferredPerfMsgCount = 0;
   presetLoad_endFileLoadIngress();
}

static uint8_t presetLoad_shouldStagePattern(uint8_t pattern)
{
   if(presetLoad_fileLoadIngressActive)
      return 0;

   if(presetLoad_prfCacheSessionActive())
      return 0;

   if(presetLoad_deferredPerfLoadActive)
      return pattern == presetLoad_deferredPerfProtectedPattern;

   return (pattern == seq_activePattern) && seq_isRunning() && !seq_loadFastMode;
}

static void presetLoad_markDeferredPatternPending()
{
   if(presetLoad_deferredPerfLoadActive)
      presetLoad_deferredPerfPatternPending = 1;
}

static void presetLoad_clearHeldVoiceLoad(uint8_t voice)
{
   if(voice > 6)
      return;

   presetLoad_clearVoiceCache(voice);

   if(voice == 6)
      voice = 5;

   if(voice == 5)
   {
      seq_voicesLoading &= (uint8_t)~0x60;
      seq_newVoiceAvailable &= (uint8_t)~0x60;
      presetLoad_deferredPerfUnholdPending &= (uint8_t)~0x60;
   }
   else
   {
      uint8_t bit = (uint8_t)(0x01 << voice);
      seq_voicesLoading &= (uint8_t)~bit;
      seq_newVoiceAvailable &= (uint8_t)~bit;
      presetLoad_deferredPerfUnholdPending &= (uint8_t)~bit;
   }
}

void presetLoad_unholdVoice(uint8_t voice)
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

void presetLoad_uncacheVoice(uint8_t voice)
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
	      if(midi_midiVeloCache[voice] >= PAR_MORPH_DRUM1
	         && midi_midiVeloCache[voice] <= PAR_MORPH_HIHAT)
	      {
	         modNode_setDestination(&velocityModulators[voice], PAR_NONE);
	      }
	      else
	      {
	         modNode_setDestination(&velocityModulators[voice],
	                                midi_midiVeloCache[voice]);
	      }
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

void presetLoad_releaseVoiceCache(uint8_t voice)
{
   presetLoad_clearHeldVoiceLoad(voice);
}

void presetLoad_unholdLoadedVoice(uint8_t voice)
{
   presetLoad_clearHeldVoiceLoad(voice);
}

uint8_t presetLoad_voiceCachePending(uint8_t voice)
{
   uint8_t i;
   uint8_t *presetMask = presetLoad_getVoicePresetMaskByVoice(&voice);

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

void presetLoad_applyPendingVoiceCache(void)
{
   uint8_t voice;

   for(voice=0;voice<6;voice++)
   {
      if(presetLoad_deferredPerfUnholdPending & (0x01<<voice))
      {
         presetLoad_clearHeldVoiceLoad(voice);
         presetLoad_deferredPerfUnholdPending &= (uint8_t)~(0x01<<voice);
      }

      if(presetLoad_voiceCachePending(voice))
      {
         presetLoad_releaseVoiceCache(voice);
         if(voice == 5)
            seq_newVoiceAvailable &= (uint8_t)~0x60;
         else
            seq_newVoiceAvailable &= (uint8_t)~(0x01<<voice);
      }
   }
}

void presetLoad_finalizeTempBackgroundLoad(void)
{
   if(presetLoad_prfCacheCanExit())
   {
      presetLoad_applyPendingVoiceCache();
      presetLoad_deferredPerfVoiceCachePending = 0;
      presetLoad_deferredPerfPatternPending = 0;
      presetLoad_clearDeferredPerfLoad();
      presetLoad_clearPrfRuntimeFlags();
      presetLoad_clearPrfCacheSession();
      return;
   }

   if(presetLoad_prfCacheSessionActive())
      return;

   if(presetLoad_deferredPerfVoiceCachePending)
   {
      presetLoad_applyDeferredPerfMessages();
      presetLoad_applyPendingVoiceCache();
      if(presetLoad_deferredPerfPatternPending)
         seq_applyTmpPatternTo(presetLoad_deferredPerfProtectedPattern);
      presetLoad_deferredPerfVoiceCachePending = 0;
      presetLoad_deferredPerfPatternPending = 0;
      presetLoad_clearDeferredPerfLoad();
   }
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
static uint8_t frontParser_globalMorphLsb = 0;

uint8_t frontParser_sysexBuffer[16];

/** used to count incoming sequencer step data packages*/
uint16_t frontParser_sysexSeqStepNr=0;

uint8_t frontParser_activeTrack=0;	/** the active track on the Frontpanel. track specific messages refer to the track selected with this command*/
uint8_t frontParser_shownPattern = 0;
uint8_t frontParser_activeStep=0;

uint8_t frontParser_stepCopySource=0;

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
            
            if(presetLoad_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_mainSteps[currentTrack] = mainStepData;
               presetLoad_markDeferredPatternPending();
            }
            else if(!presetLoad_prfCacheSessionActive()
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

            if(presetLoad_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
            {
               presetLoad_prfPendingMainStepCount++;
               presetLoad_prfCacheCountPatternWrite(currentPattern);
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
            if(presetLoad_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_patternSettings.nextPattern=next;
               seq_tmpPattern.seq_patternSettings.changeBar=repeat;
               presetLoad_markDeferredPatternPending();
            }
            else
            {
               patternSet->seq_patternSettings[currentPattern].nextPattern = next;
               patternSet->seq_patternSettings[currentPattern].changeBar = repeat;
            }
            if(presetLoad_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
            {
               presetLoad_prfPendingChainCount++;
               presetLoad_prfCacheCountPatternWrite(currentPattern);
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
         
         
            if(presetLoad_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_patternLengthRotate[currentTrack].length = data;
               presetLoad_markDeferredPatternPending();
            } 
            else 
            
            {
               patternSet->seq_patternLengthRotate[currentPattern][currentTrack].length = data;
            }

            if(presetLoad_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
            {
               presetLoad_prfPendingLengthCount++;
               presetLoad_prfCacheCountPatternWrite(currentPattern);
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
         
            
            if(presetLoad_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_patternLengthRotate[currentTrack].scale = data;
               presetLoad_markDeferredPatternPending();
            } 
            else 
            {
               patternSet->seq_patternLengthRotate[currentPattern][currentTrack].scale = data;
            }

            if(presetLoad_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
            {
               presetLoad_prfPendingScaleCount++;
               presetLoad_prfCacheCountPatternWrite(currentPattern);
            }
         
         
            //inc the step counter
            frontParser_sysexSeqStepNr++;
            frontParser_rxCnt = 0;
         
         // signal new pattern after receiving all the data
         /*
            if( seq_isRunning() && (frontParser_sysexSeqStepNr == NUM_TRACKS*NUM_PATTERN)) {
               preset_tempPlaybackSwitchState.newPatternAvailable = 1;
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
            if(presetLoad_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].volume 	= frontParser_sysexBuffer[0];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].prob 	= frontParser_sysexBuffer[1];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].note 	= frontParser_sysexBuffer[2];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param1Nr = frontParser_sysexBuffer[3];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param1Val 	= frontParser_sysexBuffer[4];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param2Nr 	= frontParser_sysexBuffer[5];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param2Val 	= frontParser_sysexBuffer[6];
               presetLoad_markDeferredPatternPending();
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
         
            if(presetLoad_shouldStagePattern(currentPattern))
            {
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].volume 	= frontParser_sysexBuffer[0];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].prob 	= frontParser_sysexBuffer[1];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].note 	= frontParser_sysexBuffer[2];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param1Nr = frontParser_sysexBuffer[3];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param1Val 	= frontParser_sysexBuffer[4];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param2Nr 	= frontParser_sysexBuffer[5];
               seq_tmpPattern.seq_subStepPattern[currentTrack][currentStep].param2Val 	= frontParser_sysexBuffer[6];
               presetLoad_markDeferredPatternPending();
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

            if(presetLoad_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
            {
               presetLoad_prfPendingStepCount++;
               presetLoad_prfCacheCountPatternWrite(currentPattern);
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
               else if (!presetLoad_deferredPerfLoadActive
                  && !presetLoad_prfCacheSessionActive()
                  && currentPattern == 7
                  && seq_isRunning()
                  && !seq_loadFastMode)
               {
                  preset_tempPlaybackSwitchState.newPatternAvailable=1;
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
void frontParser_handleMidiMessage(void)
{
   switch(frontParser_midiMsg.status)
   {
      case PARAM_RESTORE_READY:
         preset_tmpKitHandshakeReady = 1;
         return;

      case PARAM_RESTORE_ACK:
         preset_tmpKitHandshakeAck = 1;
         return;

      default:
         break;
   }

   if(presetLoad_cachePrfLiveSnapshotMessage())
   {
      frontParser_flowMessageApplied();
      return;
   }

   if(presetLoad_cacheDeferredPerfMessage())
   {
      frontParser_flowMessageApplied();
      return;
   }

   switch(frontParser_midiMsg.status)
   {
      case PRF_RESTORE_PARAM_CC:
      case PRF_RESTORE_PARAM_CC2:
         {
            uint16_t paramNr = frontParser_midiMsg.data1;
            if(frontParser_midiMsg.status == PRF_RESTORE_PARAM_CC2)
               paramNr += 128;

            /* PRF_RESTORE_PARAM_* carries raw kit/front endpoint bytes. These
               are not live low MIDI CC numbers, so do not apply the +1 offset. */
            preset_storeParameterIngress(paramNr, frontParser_midiMsg.data2);
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
            uint8_t currentTarget = preset_getIngressTarget();
            if(paramNr < END_OF_SOUND_PARAMETERS)
            {
               if(currentTarget == SEQ_PARAM_INGRESS_CURRENT_IMAGE)
               {
                  preset_storeMorphParameterIngress(paramNr, frontParser_midiMsg.data2);
               }
               else
               {
                  preset_normalKitState.morphEndpointParams[paramNr] = frontParser_midiMsg.data2;
               }
            }
         }
         break;

      case FRONT_CC_MACRO_TARGET: //frontParser_midiMsg.status
         {
            uint8_t applyLive = preset_shouldApplyIngressToLive();
         
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
               preset_storeMacroDestinationIngress(whichModDest, value);
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
               preset_storeParameterIngress(rawParam, frontParser_midiMsg.data2);
            }
            else
            {
               if(preset_shouldApplyIngressToLive())
               {
                  preset_storeParameterIngress(rawParam, frontParser_midiMsg.data2);
                  midiParser_ccHandler(liveMsg,0);

               //record automation if record is turned on
                  seq_recordAutomation(frontParser_activeTrack, rawParam, frontParser_midiMsg.data2);
               }
               else
               {
                  /* FILE LOAD: Store normal-image params without changing the
                     temporary sound when the temporary pattern is active. */
                  preset_storeParameterIngress(rawParam, frontParser_midiMsg.data2);
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
               preset_storeParameterIngress(frontParser_midiMsg.data1+128, frontParser_midiMsg.data2);
            }
            else
            {
               if(preset_shouldApplyIngressToLive())
               {
                  midiParser_ccHandler(frontParser_midiMsg,1);
                  //record automation if record is turned on
                  seq_recordAutomation(frontParser_activeTrack, frontParser_midiMsg.data1+128, frontParser_midiMsg.data2);
               }
               else
               {
                  /* FILE LOAD: Store normal-image params without changing the
                     temporary sound when the temporary pattern is active. */
                  preset_storeParameterIngress(frontParser_midiMsg.data1+128, frontParser_midiMsg.data2);
               }
            }
         }
         break;
   
   
      case FRONT_CC_LFO_TARGET: //frontParser_midiMsg.status
         {
            uint8_t upper = frontParser_midiMsg.data1;
            uint8_t lower = frontParser_midiMsg.data2;
            uint8_t applyLive = preset_shouldApplyIngressToLive();
         // --AS **PATROT note that the only valid values for the following are listed in
         // the modTargets array in the AVR code
            uint8_t value = ((upper&0x01)<<7) | lower;
         
            uint8_t lfoNr = (upper&0xfe)>>1;
            preset_storeLfoDestinationIngress(lfoNr, value);
            
            if(!applyLive)
            {
               /* RESTORE: Endpoint-copy automation target sidebands are stored only.
                  They must not touch the currently sounding modulation nodes. */
            }
            else if(seq_voicesLoading&(0x01<<(lfoNr)))
            {
               /* Post-morph-move file load stores endpoint state on STM.
                  Do not populate the legacy release cache. */
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
            uint8_t applyLive = preset_shouldApplyIngressToLive();
         // --AS **PATROT note that the only valid values for the following are listed in
         // the modTargets array in the AVR code
            uint8_t value = ((upper&0x01)<<7) | lower;
            uint8_t velModNr = (upper&0xfe)>>1;
            preset_storeVelocityDestinationIngress(velModNr, value);
            if(!applyLive)
            {
               /* RESTORE: Endpoint-copy automation target sidebands are stored only.
                  They must not touch the currently sounding modulation nodes. */
            }
            else if(seq_voicesLoading&(0x01<<(velModNr)))
            {
               /* Post-morph-move file load stores endpoint state on STM.
                  Do not populate the legacy release cache. */
            }
            else
            {
               /* Velocity-to-VMORPH is applied once at trigger time by the
                  sequencer; keep PAR_MORPH_* out of the generic velocity node. */
               if(value >= PAR_MORPH_DRUM1 && value <= PAR_MORPH_HIHAT)
                  modNode_setDestination(&velocityModulators[velModNr], PAR_NONE);
               else
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
         presetLoad_endFileLoadIngress();
         presetLoad_clearPrfCacheSession();
         presetLoad_clearDeferredPerfLoad();
         break;

      case FRONT_SEQ_FLOW_GRANT:
         break;

      case FRONT_SEQ_PRF_CACHE_BEGIN:
      {
         uint8_t cacheAccepted = PRF_CACHE_REJECTED;

         presetLoad_clearPrfCacheSession();
         if((frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE)
            && seq_isRunning()
            && presetLoad_deferPerfLoadCacheUntilPatternChange)
         {
            presetLoad_prfCacheProtectedPattern = seq_activePattern & 0x07;
            presetLoad_capturePrfStmLiveSnapshot();
            presetLoad_prfCacheState = PRF_CACHE_LIVE_ACTIVE;
            presetLoad_prfCachePendingValid = 0;
            presetLoad_clearDeferredPerfLoad();
            cacheAccepted = PRF_CACHE_ACCEPTED;
         }
         frontParser_sendPrfCacheStatus(FRONT_SEQ_PRF_CACHE_BEGIN, cacheAccepted);
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;
      }

      case FRONT_SEQ_PRF_PENDING_BEGIN:
         if(presetLoad_prfCacheState != PRF_CACHE_IDLE)
         {
            presetLoad_resetPrfPendingCounters();
            presetLoad_prfCachePendingValid = 0;
            presetLoad_prfCacheState = PRF_CACHE_RECEIVING_PENDING;
         }
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_PENDING_DONE:
         if(presetLoad_prfCacheState == PRF_CACHE_RECEIVING_PENDING)
         {
            if(presetLoad_prfPendingCountsValid())
            {
               presetLoad_prfCachePendingValid = 1;
               presetLoad_prfCacheState = PRF_CACHE_PENDING_VALID;
               frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
            }
            else
            {
               presetLoad_prfCacheState = PRF_CACHE_ABORTING;
               presetLoad_clearPrfCacheSession();
               presetLoad_clearDeferredPerfLoad();
               frontParser_sendFlowAbort(FLOW_CH_LOAD_SESSION);
            }
         }
         else
         {
            frontParser_sendFlowAbort(FLOW_CH_LOAD_SESSION);
         }
         break;

      case FRONT_SEQ_PRF_CACHE_ABORT:
         presetLoad_prfCacheState = PRF_CACHE_ABORTING;
         presetLoad_endFileLoadIngress();
         presetLoad_clearPrfCacheSession();
         presetLoad_clearDeferredPerfLoad();
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_AVR_SNAPSHOT_BEGIN:
         if(presetLoad_prfCacheState != PRF_CACHE_IDLE)
         {
            presetLoad_prfCacheState = PRF_CACHE_RECEIVING_AVR_LIVE;
            presetLoad_prfCacheAvrLiveValid = 0;
         }
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_AVR_SNAPSHOT_END:
         if(presetLoad_prfCacheState == PRF_CACHE_RECEIVING_AVR_LIVE)
         {
            presetLoad_prfCacheAvrLiveValid = 1;
            presetLoad_prfCacheState = PRF_CACHE_LIVE_ACTIVE;
         }
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_RESTORE_AVR_LIVE:
         presetLoad_sendPrfLiveRestore();
         frontParser_sendFlowGrantWait(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN:
      {
         uint8_t endpointMode = frontParser_midiMsg.data2;

         /* RESTORE: Switch ingress target to normal kit endpoint buffer.
            Subsequent parameter and target messages will populate preset_normalKitState endpoint images.
            The data byte selects which endpoint group is being refreshed, so file
            loads can update only the kit/front endpoint or only the morph parameter
            endpoint without clearing the other side. */
         preset_setIngressTarget(SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT);
         preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);

         if(endpointMode != FRONT_SEQ_TMP_KIT_ENDPOINT_MORPH_ONLY)
         {
            memset(preset_normalKitState.kitEndpointParams, 0, END_OF_SOUND_PARAMETERS);
            memset(&preset_normalKitState.frontPanelAutomationTargets,
                   0,
                   sizeof(preset_normalKitState.frontPanelAutomationTargets));
         }

         if(endpointMode != FRONT_SEQ_TMP_KIT_ENDPOINT_FRONT_ONLY)
         {
            memset(preset_normalKitState.morphEndpointParams, 0, END_OF_SOUND_PARAMETERS);
            memset(&preset_normalKitState.morphParameterEndpointAutomationTargets,
                   0,
                   sizeof(preset_normalKitState.morphParameterEndpointAutomationTargets));
         }
         break;
      }

      case FRONT_SEQ_TMP_KIT_AUTOMATION_PHASE:
         /* RESTORE: Inside the copy-to-temp endpoint bracket, raw param opcodes
            identify parameter_values[] vs parameters2[]. Resolved automation target
            sidebands need this explicit phase marker so the STM stores them with
            the matching endpoint image and does not apply them to live audio. */
         if(frontParser_midiMsg.data2 == FRONT_SEQ_TMP_KIT_AUTOMATION_FRONT_ENDPOINT)
            preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_FRONT_ENDPOINT);
         else if(frontParser_midiMsg.data2 == FRONT_SEQ_TMP_KIT_AUTOMATION_MORPH_ENDPOINT)
            preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_MORPH_ENDPOINT);
         else
            preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
         break;

      case FRONT_SEQ_TMP_KIT_ENDPOINT_END:
         preset_applyNormalEndpointAutomationTargets();

         /* RESTORE: Switch ingress target back to the surrounding context. During
            file load, endpoint dumps are nested inside normal kit-endpoint ingress. */
         preset_setIngressTarget(presetLoad_fileLoadIngressActive
                                ? SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT
                                : SEQ_PARAM_INGRESS_CURRENT_IMAGE);
         preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
         break;

      case FRONT_SEQ_SET_GLOBAL_MORPH:
         /* AVR menu/global morph resets all STM per-voice morph amounts. Per
            voice step automation or modulation may later overwrite individual
            voices without asking AVR to recompute morph. */
         seq_setGlobalMorphAmount(frontParser_midiMsg.data2);
         break;

      case FRONT_SEQ_SET_GLOBAL_MORPH_LSB:
         frontParser_globalMorphLsb = (uint8_t)(frontParser_midiMsg.data2 & 0x7f);
         break;

      case FRONT_SEQ_SET_GLOBAL_MORPH_MSB:
      {
         uint8_t morphAmount =
            (uint8_t)(frontParser_globalMorphLsb
               | ((frontParser_midiMsg.data2 & 0x01) << 7));
         seq_setGlobalMorphAmount(morphAmount);
         break;
      }

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
            if(!preset_consumeTmpBoundaryPatternSwitchAck())
               seq_realign();
         }
         else
         {
            (void)preset_consumeTmpBoundaryPatternSwitchAck();
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
            presetLoad_finalizeTempBackgroundLoad();
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
            - presetLoad_uncacheVoice(voice number), turns off each individual flag.
            
            case FRONT_SEQ_UNHOLD_VOICE (data for voice number):
            calls presetLoad_unholdVoice(voice). also calls fp_uncacheVoice if seq not 
            running.
            frontParser_unHoldVoice:
            	- sets seq_newVoiceAvailable (a register) for the voice
            	- then this waits for one of 2 cases (below) to actually update the voice
            	- searches cacheAvailable to see if any new params need to be applied
            	- sends the param to midi_midiKit, midi_kitLfoCache, or midi_kitVeloCache. 
               this is not the actual voiced parameter. those get applied below:
            
            	CASE 1: global load fast mode is on. voice params get applied 
               (presetLoad_uncacheVoice) when seq_triggerVoice(voice #) is called. They 
               also change over if the sequence is changed as case 2 below.
            
            	CASE 2: global load fast mode is off. voice params get applied 
               (presetLoad_uncacheVoice) when the sequence changes 
               (flag preset_tempPlaybackSwitchState.newPatternExecuted). This happens if the voice is trigged OR on 
               corresponding substep(mod 8) (0=drum1, 3=snare etc.). when 
            
            presetLoad_uncacheVoice(voice) actually applies the parameters
            
            
            NOTES:
            PATCH_RESET sets seq_newVoiceAvailable for ALL.
            
            seq_tracksLocked
            - only used in seq_triggerVoice to return. ie the voice does not play at all.
            - set only by SYSEX_RECEIVE_MAIN_STEP_DATA, unset by SYSEX_BEGIN_PATTERN_TRANSMIT
         */
         
         presetLoad_beginFileLoadIngress(0);
         presetLoad_clearVoiceCache(frontParser_midiMsg.data2);
         seq_voicesLoading |= (0x01<<frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_UNHOLD_VOICE:
         if(presetLoad_deferredPerfLoadActive)
         {
            presetLoad_clearHeldVoiceLoad(frontParser_midiMsg.data2);
         }
         else
         {
            presetLoad_unholdLoadedVoice(frontParser_midiMsg.data2);
         }
         if(!presetLoad_fileLoadBracketActive && !seq_voicesLoading)
            presetLoad_endFileLoadIngress();
         break;
      case FRONT_SEQ_LOAD_FAST:
         seq_loadFastMode=frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_FILE_BEGIN:
         presetLoad_beginFileLoadIngress(1);
         presetLoad_clearDeferredPerfLoad();
         seq_resetVoiceMorphAmountsToGlobal();
         seq_resetLiveMorphApplyCache();
         if((frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE)
            || (frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_ALL))
         {
            preset_morphLoadDisabled = 1;
            preset_vMorphFlag = 0;
         }
         if((frontParser_midiMsg.data2 != FRONT_FILE_DONE_TYPE_PERFORMANCE)
            || (presetLoad_prfCacheState == PRF_CACHE_IDLE))
         {
            presetLoad_clearPrfCacheSession();
         }
         break;
      case FRONT_SEQ_FILE_DONE:
         if((frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE)
            || (frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_ALL))
         {
            preset_morphLoadDisabled = 0;
            preset_vMorphFlag = 0;
         }
         if(presetLoad_prfCacheSessionActive()
            && (frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE))
         {
            presetLoad_deferredPerfLoadActive = 0;
         }
         else if(presetLoad_deferredPerfLoadActive
            && (frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE))
         {
            presetLoad_clearDeferredPerfLoad();
         }
         else
         {
            presetLoad_deferredPerfLoadActive = 0;
            presetLoad_applyPendingVoiceCache();
            presetLoad_clearDeferredPerfLoad();
         }
         seq_voicesLoading=0;
         presetLoad_endFileLoadIngress();
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
