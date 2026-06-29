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

/* Phase 8 collapsed the temp-switch booleans into one explicit state object.
   The `preset_tempPlaybackSwitchState` fields below are the canonical names for
   that boundary state. Sequencer and the front-panel parser should read or
   write the struct fields directly so the ownership of pattern switching stays
   visibly inside Preset. */
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
   uint8_t forceInstantSwitch;
   uint8_t tmpBoundaryPatternSwitchAck;
   uint8_t patternOnlyTempPlayback;
   uint8_t tempPresetPlaybackActive;
   uint8_t resnapshotTmpFromNormalPending;
} PresetTempPlaybackSwitchState;

extern PresetTempPlaybackSwitchState preset_tempPlaybackSwitchState;

/* Updates the temp-image selection and performs the restore/report work that
   accompanies a normal/temp boundary change. */
void preset_setTempPlaybackActive(uint8_t active);

/* Returns whether a pattern number resolves to the temp image. */
uint8_t preset_trackPatternUsesTmp(uint8_t pattern);

/* Returns whether the track pattern selection for a voice routes that voice
   through the temp image, including the shared hihat voice coupling. */
uint8_t preset_synthVoiceUsesTmpFromTrackPatterns(const uint8_t *patternForTrack,
                                                 uint8_t synthVoice);

/* Reports whether every voice source currently points at the temp image. */
uint8_t preset_allVoiceSourcesUseTmp(void);

/* Reports whether every voice source currently points at the normal image. */
uint8_t preset_allVoiceSourcesUseNormal(void);
/* Background temp playback has two materially different forms: pattern-only
   temp playback for .pat, and preset-carrying temp playback for protected
   .prf/.all loads. Reload gating and temp resnapshot timing depend on that
   distinction, so it lives in the canonical temp-switch state. */
uint8_t preset_isTempPresetPlaybackActive(void);
void preset_setTempPresetPlaybackActive(uint8_t active);
/* Replay the currently selected normal/tmp source combination to the live DSP.
   Callers use this after PATCH_RESET restores or after a temp image is mirrored
   immediately while temp preset playback is still the audible source. */
void preset_reapplyCurrentPlaybackSources(void);
/* PATCH_RESET is an STM-owned storage restore. It copies the authoritative
   temporary preset endpoint image back into normal storage and reapplies the
   normal image because normal is the live source whenever this command is
   allowed to run. */
uint8_t preset_reloadNormalFromTemporaryPreset(void);
/* Once playback has returned to normal after a protected .prf/.all background
   load, temp must stop holding the old sound and become the new last-loaded
   preset snapshot again. */
void preset_resnapshotTemporaryPresetFromNormal(void);

/* Updates the per-voice source state after a pattern change and optionally
   pushes any affected endpoint changes to the AVR restore queue. */
void preset_updateVoiceSourcesForPatternChange(const uint8_t *oldPatternForTrack,
                                               uint8_t pushEndpointUpdates);

/* Consumes the temp-boundary acknowledgement bit that the sequencer raises
   when a pattern switch crosses the normal/temp boundary. */
uint8_t preset_consumeTmpBoundaryPatternSwitchAck(void);

#endif /* PRESET_TEMPPLAYBACKSWITCH_H_ */
