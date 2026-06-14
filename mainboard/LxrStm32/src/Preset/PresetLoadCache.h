/*
 * PresetLoadCache.h
 *
 * Preset owns the transitional load/session cache that still bridges file
 * loads, deferred performance replay, and the receive-side parser. The parser
 * keeps the receive dispatch, while this module owns the cache bookkeeping and
 * the state that the parser consults during a load transaction.
 */

#ifndef PRESET_PRESETLOADCACHE_H_
#define PRESET_PRESETLOADCACHE_H_

#include <stdint.h>

#include "MidiMessages.h"
#include "Sequencer/Pattern/PatternData.h"

typedef enum {
   PRF_CACHE_IDLE = 0,
   PRF_CACHE_RECEIVING_AVR_LIVE,
   PRF_CACHE_LIVE_ACTIVE,
   PRF_CACHE_RECEIVING_PENDING,
   PRF_CACHE_PENDING_VALID,
   PRF_CACHE_ABORTING
} PrfCacheState;

/* Load-session state owned by the cache bridge. The receive parser still reads
   these flags directly while the bridge remains transitional. */
extern uint8_t presetLoad_deferPerfLoadCacheUntilPatternChange;
extern uint8_t presetLoad_deferredPerfLoadActive;
extern uint8_t presetLoad_deferredPerfVoiceCachePending;
extern uint8_t presetLoad_deferredPerfPatternPending;
extern uint8_t presetLoad_deferredPerfUnholdPending;
extern uint8_t presetLoad_deferredPerfProtectedPattern;
extern uint8_t presetLoad_deferredPerfReplay;
extern uint8_t presetLoad_deferredPerfMsgCount;
extern MidiMsg presetLoad_deferredPerfMsgCache[128];
extern uint8_t presetLoad_fileLoadIngressActive;
extern uint8_t presetLoad_fileLoadBracketActive;
extern PrfCacheState presetLoad_prfCacheState;
extern uint8_t presetLoad_prfCacheProtectedPattern;
extern uint8_t presetLoad_prfCachePendingValid;
extern uint8_t presetLoad_prfCacheAvrLiveValid;
extern uint8_t presetLoad_prfCacheStmLiveValid;
extern TempPattern presetLoad_prfCacheLivePattern;
extern uint8_t presetLoad_prfCacheLiveActivePattern;
extern uint8_t presetLoad_prfCacheLivePendingPattern;
extern uint8_t presetLoad_prfCacheLivePerTrackActivePattern[NUM_TRACKS];
extern uint8_t presetLoad_prfCacheLivePerTrackPendingPattern[NUM_TRACKS];
extern int8_t presetLoad_prfCacheLiveStepIndex[NUM_TRACKS+1];
extern uint8_t presetLoad_prfCacheLiveMidiChannels[8];
extern uint8_t presetLoad_prfCacheLiveNoteOverride[7];
extern uint8_t presetLoad_prfCacheLiveVMorphAmount[7];
extern uint8_t presetLoad_prfCacheLiveVMorphFlag;
extern uint8_t presetLoad_prfCacheLiveSeqVoicesLoading;
extern uint8_t presetLoad_prfCacheLiveSeqNewVoiceAvailable;
extern uint8_t presetLoad_prfCacheLiveSeqTracksLocked;
extern uint8_t presetLoad_prfCacheLiveSeqLoadFastMode;
extern uint16_t presetLoad_prfPendingMainStepCount;
extern uint16_t presetLoad_prfPendingStepCount;
extern uint16_t presetLoad_prfPendingLengthCount;
extern uint16_t presetLoad_prfPendingScaleCount;
extern uint16_t presetLoad_prfPendingChainCount;
extern uint16_t presetLoad_prfPendingProtectedWriteCount;

/* Load-session helpers that the receive parser uses while the bridge remains
   transitional. */
void presetLoad_clearDeferredPerfLoad(void);
void presetLoad_beginFileLoadIngress(uint8_t bracketed);
void presetLoad_endFileLoadIngress(void);
void presetLoad_resetPrfPendingCounters(void);
void presetLoad_clearPrfCacheSession(void);
void presetLoad_clearPrfRuntimeFlags(void);
uint8_t presetLoad_prfCacheSessionActive(void);
uint8_t presetLoad_prfCacheCanExit(void);
void presetLoad_prfCacheCountPatternWrite(uint8_t pattern);
void presetLoad_capturePrfStmLiveSnapshot(void);
uint8_t presetLoad_prfCacheUseLivePattern(void);
uint8_t presetLoad_prfCacheTrackUsesLivePattern(uint8_t track);
uint8_t presetLoad_prfPendingCountsValid(void);
uint8_t presetLoad_cachePrfLiveSnapshotMessage(void);
uint8_t presetLoad_isDeferredPerfControlMessage(void);
uint8_t presetLoad_shouldDeferPerfMessage(void);
uint8_t presetLoad_cacheDeferredPerfMessage(void);
uint8_t presetLoad_shouldStagePattern(uint8_t pattern);
void presetLoad_markDeferredPatternPending(void);
void presetLoad_clearHeldVoiceLoad(uint8_t voice);
void presetLoad_clearVoiceCache(uint8_t voice);
Step* presetLoad_prfCacheLiveStep(uint8_t track, uint8_t step);
uint16_t presetLoad_prfCacheLiveMainSteps(uint8_t track);
LengthRotate presetLoad_prfCacheLiveLengthRotate(uint8_t track);
PatternSetting presetLoad_prfCacheLivePatternSetting(void);
uint8_t presetLoad_prfCacheLiveMidiChannel(uint8_t voice);
uint8_t presetLoad_prfCacheLiveNoteOverrideValue(uint8_t voice);
uint8_t presetLoad_prfCacheTakeLiveVMorphFlag(void);
uint8_t presetLoad_prfCacheLiveVMorphAmountValue(uint8_t voice);
void presetLoad_unholdVoice(uint8_t voice);
void presetLoad_uncacheVoice(uint8_t voice);
void presetLoad_releaseVoiceCache(uint8_t voice);
void presetLoad_unholdLoadedVoice(uint8_t voice);
uint8_t presetLoad_voiceCachePending(uint8_t voice);
void presetLoad_applyPendingVoiceCache(void);
void presetLoad_finalizeTempBackgroundLoad(void);

#endif /* PRESET_PRESETLOADCACHE_H_ */
