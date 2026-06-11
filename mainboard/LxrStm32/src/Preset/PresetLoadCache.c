/*
 * PresetLoadCache.c
 *
 * Preset owns the PRF/load-session cache, deferred performance replay, and
 * background-load finalization state that used to live in frontPanelParser.c.
 * The front-panel parser still parses bytes and can trigger the session API,
 * but it no longer owns the storage/session bookkeeping itself.
 */

#include "Preset/PresetLoadCache.h"
#include "Uart.h"
#include <string.h>

extern void frontParser_handleMidiMessage(void);
extern MidiMsg frontParser_midiMsg;

#define VOICE_PARAM_LENGTH 36
#define FRONT_FILE_DONE_TYPE_PERFORMANCE 8
#define FRONT_FILE_DONE_TYPE_ALL 9
#define PRF_PENDING_EXPECTED_MAINSTEP_COUNT (NUM_PATTERN * NUM_TRACKS)
#define PRF_PENDING_EXPECTED_STEP_COUNT (NUM_PATTERN * NUM_TRACKS * NUM_STEPS)
#define PRF_PENDING_EXPECTED_LENGTH_COUNT (NUM_PATTERN * NUM_TRACKS)
#define PRF_PENDING_EXPECTED_SCALE_COUNT (NUM_PATTERN * NUM_TRACKS)
#define PRF_PENDING_EXPECTED_CHAIN_COUNT NUM_PATTERN

uint8_t frontParser_deferPerfLoadCacheUntilPatternChange = 0;
uint8_t frontParser_deferredPerfLoadActive = 0;
uint8_t frontParser_deferredPerfVoiceCachePending = 0;
uint8_t frontParser_deferredPerfPatternPending = 0;
uint8_t frontParser_deferredPerfUnholdPending = 0;
uint8_t frontParser_deferredPerfProtectedPattern = 0;
uint8_t frontParser_deferredPerfReplay = 0;
uint8_t frontParser_deferredPerfMsgCount = 0;
MidiMsg frontParser_deferredPerfMsgCache[DEFERRED_PERF_MSG_CACHE_SIZE];
uint8_t frontParser_fileLoadIngressActive = 0;
uint8_t frontParser_fileLoadBracketActive = 0;
PrfCacheState frontParser_prfCacheState = PRF_CACHE_IDLE;
uint8_t frontParser_prfCacheProtectedPattern = 0;
uint8_t frontParser_prfCachePendingValid = 0;
uint8_t frontParser_prfCacheAvrLiveValid = 0;
uint8_t frontParser_prfCacheStmLiveValid = 0;
TempPattern frontParser_prfCacheLivePattern;
uint8_t frontParser_prfCacheLiveActivePattern = 0;
uint8_t frontParser_prfCacheLivePendingPattern = 0;
uint8_t frontParser_prfCacheLivePerTrackActivePattern[NUM_TRACKS];
uint8_t frontParser_prfCacheLivePerTrackPendingPattern[NUM_TRACKS];
int8_t frontParser_prfCacheLiveStepIndex[NUM_TRACKS+1];
uint8_t frontParser_prfCacheLiveMidiChannels[8];
uint8_t frontParser_prfCacheLiveNoteOverride[7];
uint8_t frontParser_prfCacheLiveVMorphAmount[7];
uint8_t frontParser_prfCacheLiveVMorphFlag = 0;
uint8_t frontParser_prfCacheLiveSeqVoicesLoading = 0;
uint8_t frontParser_prfCacheLiveSeqNewVoiceAvailable = 0;
uint8_t frontParser_prfCacheLiveSeqTracksLocked = 0;
uint8_t frontParser_prfCacheLiveSeqLoadFastMode = 0;
uint16_t frontParser_prfPendingMainStepCount = 0;
uint16_t frontParser_prfPendingStepCount = 0;
uint16_t frontParser_prfPendingLengthCount = 0;
uint16_t frontParser_prfPendingScaleCount = 0;
uint16_t frontParser_prfPendingChainCount = 0;
uint16_t frontParser_prfPendingProtectedWriteCount = 0;

/* The temporary helper arrays are still indexed by the old voice mask layout,
   so keep the explicit masks here until the pattern-module split lands. */
static uint8_t voice1presetMask[VOICE_PARAM_LENGTH]={1,8,9,20,      37,43,49,50,   62,70,74,78,  82,83,88,94,   102,108,115,121,     128,134,137,143,    149,155,161,167,    173,179,185,191,    197,203,209,215};
static uint8_t voice2presetMask[VOICE_PARAM_LENGTH]={2,10,11,21,    38,44,51,52,   63,71,75,79,  84,85,89,95,   103,109,116,122,     129,135,138,144,    150,156,162,168,    174,180,186,192,    198,204,210,216};
static uint8_t voice3presetMask[VOICE_PARAM_LENGTH]={3,12,13,22,    39,45,53,54,   64,72,76,80,  86,87,90,96,   104,110,117,123,     130,136,139,145,    151,157,163,169,    175,181,187,193,    199,205,211,217};
static uint8_t voice4presetMask[VOICE_PARAM_LENGTH]={4,14,15,27,28, 40,46,55,      56,65,68,73,  77,81,91,99,   105,111,118,124,     131,140,146,152,        158,164,170,    176,182,188,194,    200,206,212,218};
static uint8_t voice5presetMask[VOICE_PARAM_LENGTH]={6,16,17,23,    24,29,30,31,   32,41,47,57,  58,66,69,92,   100,106,112,119,125, 132,141,147,153,        159,165,171,    177,183,189,195,    201,207,213,219};
static uint8_t voice6presetMask[VOICE_PARAM_LENGTH]={7,18,19,25,    26,33,34,35,   36,42,48,59,  60,61,67,93,   101,107,113,120,126, 133,142,148,154,        160,166,172,    178,184,190,196,    202,208,214,220};

static uint8_t* frontParser_getVoicePresetMaskByVoice(uint8_t *voice)
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

void frontParser_clearVoiceCache(uint8_t voice)
{
   uint8_t i;
   uint8_t *presetMask = frontParser_getVoicePresetMaskByVoice(&voice);

   if(!presetMask)
      return;

   midi_midiLfoCacheAvailable[voice] = 0;
   midi_midiVeloCacheAvailable[voice] = 0;

   for(i=0;i<VOICE_PARAM_LENGTH;i++)
   {
      midi_midiCacheAvailable[presetMask[i]] = 0;
   }
}

void frontParser_clearDeferredPerfLoad(void)
{
   frontParser_deferredPerfLoadActive = 0;
   frontParser_deferredPerfVoiceCachePending = 0;
   frontParser_deferredPerfPatternPending = 0;
   frontParser_deferredPerfUnholdPending = 0;
   frontParser_deferredPerfMsgCount = 0;
}

void frontParser_beginFileLoadIngress(uint8_t bracketed)
{
   /* FILE LOAD: File payloads update the normal pattern/parameter images.
      Live edits still use SEQ_PARAM_INGRESS_CURRENT_IMAGE outside this bracket. */
   frontParser_fileLoadIngressActive = 1;
   if(bracketed)
      frontParser_fileLoadBracketActive = 1;
   preset_setIngressTarget(SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT);
   preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
}

void frontParser_endFileLoadIngress(void)
{
   frontParser_fileLoadIngressActive = 0;
   frontParser_fileLoadBracketActive = 0;
   preset_setIngressTarget(SEQ_PARAM_INGRESS_CURRENT_IMAGE);
   preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
}

void frontParser_resetPrfPendingCounters(void)
{
   frontParser_prfPendingMainStepCount = 0;
   frontParser_prfPendingStepCount = 0;
   frontParser_prfPendingLengthCount = 0;
   frontParser_prfPendingScaleCount = 0;
   frontParser_prfPendingChainCount = 0;
   frontParser_prfPendingProtectedWriteCount = 0;
}

void frontParser_clearPrfCacheSession(void)
{
   frontParser_prfCacheState = PRF_CACHE_IDLE;
   frontParser_prfCacheProtectedPattern = 0;
   frontParser_prfCachePendingValid = 0;
   frontParser_prfCacheAvrLiveValid = 0;
   frontParser_prfCacheStmLiveValid = 0;
   frontParser_resetPrfPendingCounters();
}

void frontParser_clearPrfRuntimeFlags(void)
{
   frontParser_prfCacheLiveVMorphFlag = 0;
   seq_vMorphFlag = 0;
}

uint8_t frontParser_prfCacheSessionActive(void)
{
   return (frontParser_prfCacheState != PRF_CACHE_IDLE)
      && (frontParser_prfCacheState != PRF_CACHE_ABORTING);
}

uint8_t frontParser_prfCacheCanExit(void)
{
   return frontParser_prfCacheStmLiveValid
      && frontParser_prfCachePendingValid
      && (frontParser_prfCacheState == PRF_CACHE_PENDING_VALID);
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

void frontParser_sendPrfLiveRestore(void)
{
   uint16_t i;

   if(!frontParser_prfCacheAvrLiveValid)
      return;

   /* for(i=0;i<PRF_CACHE_LIVE_PARAM_COUNT;i++)
      frontParser_sendPrfRestoreParam(i, frontParser_prfCacheLiveParams[i], 0);

   for(i=0;i<END_OF_SOUND_PARAMETERS;i++)
      frontParser_sendPrfRestoreParam(i, frontParser_prfCacheLiveMorph[i], 1); */
}

uint8_t frontParser_cachePrfLiveSnapshotMessage(void)
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
         if(paramNr < 310)
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

void frontParser_prfCacheCountPatternWrite(uint8_t pattern)
{
   if(frontParser_prfCacheState != PRF_CACHE_RECEIVING_PENDING)
      return;

   if((pattern & 0x07) == (frontParser_prfCacheProtectedPattern & 0x07))
      frontParser_prfPendingProtectedWriteCount++;
}

uint8_t frontParser_prfPendingCountsValid(void)
{
   return (frontParser_prfPendingMainStepCount >= PRF_PENDING_EXPECTED_MAINSTEP_COUNT)
      && (frontParser_prfPendingStepCount >= PRF_PENDING_EXPECTED_STEP_COUNT)
      && (frontParser_prfPendingLengthCount >= PRF_PENDING_EXPECTED_LENGTH_COUNT)
      && (frontParser_prfPendingScaleCount >= PRF_PENDING_EXPECTED_SCALE_COUNT)
      && (frontParser_prfPendingChainCount >= PRF_PENDING_EXPECTED_CHAIN_COUNT)
      && (frontParser_prfPendingProtectedWriteCount > 0);
}

void frontParser_capturePrfStmLiveSnapshot(void)
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

uint8_t frontParser_prfCacheUseLivePattern(void)
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

PatternSetting frontParser_prfCacheLivePatternSetting(void)
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

uint8_t frontParser_prfCacheTakeLiveVMorphFlag(void)
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

uint8_t frontParser_isDeferredPerfControlMessage(void)
{
   if(frontParser_midiMsg.status != FRONT_SEQ_CC)
      return 0;

   if((frontParser_midiMsg.status == FRONT_SEQ_CC)
      && (frontParser_midiMsg.data1 >= FRONT_SEQ_FLOW_BEGIN)
      && (frontParser_midiMsg.data1 <= FRONT_SEQ_FLOW_ABORT))
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

uint8_t frontParser_shouldDeferPerfMessage(void)
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

uint8_t frontParser_cacheDeferredPerfMessage(void)
{
   if(!frontParser_shouldDeferPerfMessage())
      return 0;

   if(frontParser_deferredPerfMsgCount < DEFERRED_PERF_MSG_CACHE_SIZE)
   {
      frontParser_deferredPerfMsgCache[frontParser_deferredPerfMsgCount++] = frontParser_midiMsg;
   }

   return 1;
}

void frontParser_applyDeferredPerfMessages(void)
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

uint8_t frontParser_shouldStagePattern(uint8_t pattern)
{
   if(frontParser_fileLoadIngressActive)
      return 0;

   if(frontParser_prfCacheSessionActive())
      return 0;

   if(frontParser_deferredPerfLoadActive)
      return pattern == frontParser_deferredPerfProtectedPattern;

   return (pattern == seq_activePattern) && seq_isRunning() && !frontParser_prfCacheLiveSeqLoadFastMode;
}

void frontParser_markDeferredPatternPending(void)
{
   if(frontParser_deferredPerfLoadActive)
      frontParser_deferredPerfPatternPending = 1;
}

void frontParser_clearHeldVoiceLoad(uint8_t voice)
{
   if(voice > 6)
      return;

   frontParser_clearVoiceCache(voice);

   if(voice == 6)
      voice = 5;

   if(voice == 5)
   {
      seq_voicesLoading &= (uint8_t)~0x60;
      seq_newVoiceAvailable &= (uint8_t)~0x60;
      frontParser_deferredPerfUnholdPending &= (uint8_t)~0x60;
   }
   else
   {
      uint8_t bit = (uint8_t)(0x01 << voice);
      seq_voicesLoading &= (uint8_t)~bit;
      seq_newVoiceAvailable &= (uint8_t)~bit;
      frontParser_deferredPerfUnholdPending &= (uint8_t)~bit;
   }
}

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

void frontParser_releaseVoiceCache(uint8_t voice)
{
   frontParser_clearHeldVoiceLoad(voice);
}

void frontParser_unholdLoadedVoice(uint8_t voice)
{
   frontParser_clearHeldVoiceLoad(voice);
}

uint8_t frontParser_voiceCachePending(uint8_t voice)
{
   uint8_t i;
   uint8_t *presetMask = frontParser_getVoicePresetMaskByVoice(&voice);

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

void frontParser_applyPendingVoiceCache(void)
{
   uint8_t voice;

   for(voice=0;voice<6;voice++)
   {
      if(frontParser_deferredPerfUnholdPending & (0x01<<voice))
      {
         frontParser_clearHeldVoiceLoad(voice);
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

void frontParser_applyDeferredVoiceCache(void)
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
