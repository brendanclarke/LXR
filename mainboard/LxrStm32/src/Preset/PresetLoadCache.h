/*
 * PresetLoadCache.h
 *
 * Preset owns the PRF/load-session cache, deferred performance replay, and
 * background-load finalization state that used to live in frontPanelParser.c.
 * The front-panel parser still parses bytes and can trigger the session API,
 * but it no longer owns the storage/session bookkeeping itself.
 */

#ifndef PRESET_PRESETLOADCACHE_H_
#define PRESET_PRESETLOADCACHE_H_

#include "MidiMessages.h"
#include "sequencer.h"
#include "MIDI/MidiParser.h"
#include "Preset/ParameterIngress.h"
#include "Preset/KitState.h"
#include "DrumVoice.h"
#include "CymbalVoice.h"
#include "HiHat.h"
#include "Snare.h"
#include "modulationNode.h"
#include "valueShaper.h"

#define DEFERRED_PERF_MSG_CACHE_SIZE 128

typedef enum {
   PRF_CACHE_IDLE = 0,
   PRF_CACHE_RECEIVING_AVR_LIVE,
   PRF_CACHE_LIVE_ACTIVE,
   PRF_CACHE_RECEIVING_PENDING,
   PRF_CACHE_PENDING_VALID,
   PRF_CACHE_ABORTING
} PrfCacheState;

extern uint8_t frontParser_deferPerfLoadCacheUntilPatternChange;
extern uint8_t frontParser_deferredPerfLoadActive;
extern uint8_t frontParser_deferredPerfVoiceCachePending;
extern uint8_t frontParser_deferredPerfPatternPending;
extern uint8_t frontParser_deferredPerfUnholdPending;
extern uint8_t frontParser_deferredPerfProtectedPattern;
extern uint8_t frontParser_deferredPerfReplay;
extern uint8_t frontParser_deferredPerfMsgCount;
extern MidiMsg frontParser_deferredPerfMsgCache[DEFERRED_PERF_MSG_CACHE_SIZE];
extern uint8_t frontParser_fileLoadIngressActive;
extern uint8_t frontParser_fileLoadBracketActive;
extern PrfCacheState frontParser_prfCacheState;
extern uint8_t frontParser_prfCacheProtectedPattern;
extern uint8_t frontParser_prfCachePendingValid;
extern uint8_t frontParser_prfCacheAvrLiveValid;
extern uint8_t frontParser_prfCacheStmLiveValid;
extern TempPattern frontParser_prfCacheLivePattern;
extern uint8_t frontParser_prfCacheLiveActivePattern;
extern uint8_t frontParser_prfCacheLivePendingPattern;
extern uint8_t frontParser_prfCacheLivePerTrackActivePattern[NUM_TRACKS];
extern uint8_t frontParser_prfCacheLivePerTrackPendingPattern[NUM_TRACKS];
extern int8_t frontParser_prfCacheLiveStepIndex[NUM_TRACKS+1];
extern uint8_t frontParser_prfCacheLiveMidiChannels[8];
extern uint8_t frontParser_prfCacheLiveNoteOverride[7];
extern uint8_t frontParser_prfCacheLiveVMorphAmount[7];
extern uint8_t frontParser_prfCacheLiveVMorphFlag;
extern uint8_t frontParser_prfCacheLiveSeqVoicesLoading;
extern uint8_t frontParser_prfCacheLiveSeqNewVoiceAvailable;
extern uint8_t frontParser_prfCacheLiveSeqTracksLocked;
extern uint8_t frontParser_prfCacheLiveSeqLoadFastMode;
extern uint16_t frontParser_prfPendingMainStepCount;
extern uint16_t frontParser_prfPendingStepCount;
extern uint16_t frontParser_prfPendingLengthCount;
extern uint16_t frontParser_prfPendingScaleCount;
extern uint16_t frontParser_prfPendingChainCount;
extern uint16_t frontParser_prfPendingProtectedWriteCount;

void frontParser_clearDeferredPerfLoad(void);
void frontParser_beginFileLoadIngress(uint8_t bracketed);
void frontParser_endFileLoadIngress(void);
void frontParser_clearPrfCacheSession(void);
void frontParser_clearPrfRuntimeFlags(void);
void frontParser_resetPrfPendingCounters(void);
uint8_t frontParser_prfCacheSessionActive(void);
uint8_t frontParser_prfCacheCanExit(void);
void frontParser_clearVoiceCache(uint8_t voice);
void frontParser_sendPrfLiveRestore(void);
uint8_t frontParser_cachePrfLiveSnapshotMessage(void);
uint8_t frontParser_isDeferredPerfControlMessage(void);
uint8_t frontParser_shouldDeferPerfMessage(void);
uint8_t frontParser_cacheDeferredPerfMessage(void);
void frontParser_applyDeferredPerfMessages(void);
uint8_t frontParser_shouldStagePattern(uint8_t pattern);
void frontParser_markDeferredPatternPending(void);
void frontParser_clearHeldVoiceLoad(uint8_t voice);
void frontParser_unholdVoice(uint8_t voice);
void frontParser_uncacheVoice(uint8_t voice);
void frontParser_releaseVoiceCache(uint8_t voice);
void frontParser_unholdLoadedVoice(uint8_t voice);
uint8_t frontParser_voiceCachePending(uint8_t voice);
void frontParser_applyPendingVoiceCache(void);
void frontParser_prfCacheCountPatternWrite(uint8_t pattern);
uint8_t frontParser_prfPendingCountsValid(void);
void frontParser_capturePrfStmLiveSnapshot(void);
uint8_t frontParser_prfCacheUseLivePattern(void);
uint8_t frontParser_prfCacheTrackUsesLivePattern(uint8_t track);
Step* frontParser_prfCacheLiveStep(uint8_t track, uint8_t step);
uint16_t frontParser_prfCacheLiveMainSteps(uint8_t track);
LengthRotate frontParser_prfCacheLiveLengthRotate(uint8_t track);
PatternSetting frontParser_prfCacheLivePatternSetting(void);
uint8_t frontParser_prfCacheLiveMidiChannel(uint8_t voice);
uint8_t frontParser_prfCacheLiveNoteOverrideValue(uint8_t voice);
uint8_t frontParser_prfCacheTakeLiveVMorphFlag(void);
uint8_t frontParser_prfCacheLiveVMorphAmountValue(uint8_t voice);
void frontParser_applyDeferredVoiceCache(void);

#endif /* PRESET_PRESETLOADCACHE_H_ */
