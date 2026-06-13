/*
 * TempPlaybackSwitch.h
 *
 * Preset owns the temp/normal pattern boundary state machine so the sequencer
 * can ask which image should be active without carrying the source-selection
 * bookkeeping itself.
 */

#ifndef PRESET_TEMPPLAYBACKSWITCH_H_
#define PRESET_TEMPPLAYBACKSWITCH_H_

#include "Sequencer/sequencer.h"

/* Pattern switch request state. These flags are owned by the temp-switch
   module and are consumed by the sequencer when it advances to the next
   boundary. */
extern uint8_t seq_pendingPattern;
extern uint8_t seq_perTrackPendingPattern[NUM_TRACKS];
extern uint8_t seq_newPatternAvailable;
extern uint8_t seq_newPatternExecuted;
extern uint8_t seq_loadPendingFlag;
extern uint8_t seq_loadSeqNow;
extern uint8_t seq_tmpBoundaryPatternSwitchAck;

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
