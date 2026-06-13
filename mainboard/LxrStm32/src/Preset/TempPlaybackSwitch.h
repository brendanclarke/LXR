/*
 * TempPlaybackSwitch.h
 *
 * Preset owns the temp/normal pattern boundary state machine so the sequencer
 * can ask which image should be active without carrying the source-selection
 * bookkeeping itself.
 */

#ifndef PRESET_TEMPPLAYBACKSWITCH_H_
#define PRESET_TEMPPLAYBACKSWITCH_H_

#include "stm32f4xx.h"

/* Phase 8 collapses the temp-switch booleans into one explicit state object.
   The legacy `seq_*` names remain as macros so the sequencer and parser can
   keep compiling while the ownership boundary is simplified. */
enum
{
   PRESET_TEMP_PLAYBACK_TRACKS = 7
};

typedef struct PresetTempPlaybackSwitchStateStruct
{
   uint8_t pendingPattern;
   uint8_t perTrackPendingPattern[PRESET_TEMP_PLAYBACK_TRACKS];
   uint8_t newPatternAvailable;
   uint8_t newPatternExecuted;
   uint8_t loadPendingFlag;
   uint8_t loadSeqNow;
   uint8_t tmpBoundaryPatternSwitchAck;
} PresetTempPlaybackSwitchState;

extern PresetTempPlaybackSwitchState preset_tempPlaybackSwitchState;

#define seq_pendingPattern (preset_tempPlaybackSwitchState.pendingPattern)
#define seq_perTrackPendingPattern (preset_tempPlaybackSwitchState.perTrackPendingPattern)
#define seq_newPatternAvailable (preset_tempPlaybackSwitchState.newPatternAvailable)
#define seq_newPatternExecuted (preset_tempPlaybackSwitchState.newPatternExecuted)
#define seq_loadPendingFlag (preset_tempPlaybackSwitchState.loadPendingFlag)
#define seq_loadSeqNow (preset_tempPlaybackSwitchState.loadSeqNow)
#define seq_tmpBoundaryPatternSwitchAck (preset_tempPlaybackSwitchState.tmpBoundaryPatternSwitchAck)

/* Updates the temp-image selection and performs the restore/report work that
   accompanies a normal/temp boundary change. */
void seq_setTmpKitActive(uint8_t active);

/* Returns whether a pattern number resolves to the temp image. */
uint8_t seq_trackPatternUsesTmp(uint8_t pattern);

/* Returns whether the track pattern selection for a voice routes that voice
   through the temp image, including the shared hihat voice coupling. */
uint8_t seq_synthVoiceUsesTmpFromTrackPatterns(const uint8_t *patternForTrack,
                                              uint8_t synthVoice);

/* Reports whether every voice source currently points at the temp image. */
uint8_t seq_allVoiceSourcesUseTmp(void);

/* Reports whether every voice source currently points at the normal image. */
uint8_t seq_allVoiceSourcesUseNormal(void);

/* Updates the per-voice source state after a pattern change and optionally
   pushes any affected endpoint changes to the AVR restore queue. */
void seq_updateVoiceSourcesForPatternChange(const uint8_t *oldPatternForTrack,
                                            uint8_t pushEndpointUpdates);

/* Consumes the temp-boundary acknowledgement bit that the sequencer raises
   when a pattern switch crosses the normal/temp boundary. */
uint8_t seq_consumeTmpBoundaryPatternSwitchAck(void);

/* Temporary load-session finalization hook used by the sequencer while the
   cache cutover still routes background-load completion through Preset. */
void presetLoad_finalizeTempBackgroundLoad(void);

#endif /* PRESET_TEMPPLAYBACKSWITCH_H_ */
