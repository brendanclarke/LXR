/*
 * KitState.h
 *
 * Phase 1 of the preset refactor moves the core sound-state images out of
 * sequencer.c so Preset becomes the canonical owner of normal/temp kit state
 * and the voice-image selection flags that decide which image is active.
 */

#ifndef PRESET_KITSTATE_H_
#define PRESET_KITSTATE_H_

#include "stm32f4xx.h"
#include "globals.h"
#include "ParameterArray.h"

/* Preset keeps these image/source constants local so callers can describe the
   active sound-state image without reaching back into Sequencer internals.
   The two image slots and two voice-source markers are the only values needed
   by Phase 1 routing and they are intentionally tiny so later phases can grow
   the model without rewriting callers. */
enum
{
   PRESET_SYNTH_VOICES = 6,
   PRESET_MORPH_IMAGE_NORMAL = 0,
   PRESET_MORPH_IMAGE_TMP = 1,
   PRESET_MORPH_IMAGE_COUNT = 2,
   PRESET_VOICE_SOURCE_NORMAL = 1,
   PRESET_VOICE_SOURCE_TMP = 3
};

/* Automation target groups travel with each kit image so restore and live-edit
   paths can store selector destinations, know which voice slots are valid, and
   keep front-panel and interpolated target images coherent when values move. */
typedef struct SeqKitAutomationTargetsStruct
{
   uint16_t lfoDestination[PRESET_SYNTH_VOICES];
   uint16_t velocityDestination[PRESET_SYNTH_VOICES];
   uint16_t macroDestination[4];
   uint8_t lfoDestinationValid;
   uint8_t velocityDestinationValid;
   uint8_t macroDestinationValid;
} SeqKitAutomationTargets;

/* Kit state is the always-defined preset image that Phase 1 moves out of the
   sequencer. The kit-endpoint array stores the raw live/edit endpoint, the
   morph-endpoint array stores the alternate endpoint image, and the
   interpolated array is the worker-owned render cache rebuilt from those two
   sources. The automation target blocks mirror the same split so the protocol
   layer can preserve selector bytes while the morph and restore workers
   decide which image is authoritative. */
typedef struct SeqKitStateStruct
{
   /* These arrays are always-defined sound state from zero init. File/front
      ingress writes endpoint bytes here, and the morph worker later rebuilds
      interpolatedParams[] from these images. */
   uint8_t kitEndpointParams[END_OF_SOUND_PARAMETERS];
   uint8_t morphEndpointParams[END_OF_SOUND_PARAMETERS];
   uint8_t interpolatedParams[END_OF_SOUND_PARAMETERS];
   SeqKitAutomationTargets frontPanelAutomationTargets;
   SeqKitAutomationTargets morphParameterEndpointAutomationTargets;
   SeqKitAutomationTargets interpolatedAutomationTargets;
   uint8_t globalMorphAmount;
   uint8_t voiceMorphBaseAmount[PRESET_SYNTH_VOICES];
   uint8_t voiceMorphAmount[PRESET_SYNTH_VOICES];
   uint8_t valid;
} SeqKitState;

/* Temporary kit image used for temp-pattern playback and any current-image
   routing that needs to land in the temporary bank instead of the normal kit.
   This stays externally visible so the compatibility façade and future preset
   phases can inspect or seed the image without duplicating storage. */
extern SeqKitState preset_tmpKitState;
/* Normal kit image that holds the canonical preset data for restore, file
   load, and the default live-edit path when no temp image is active. */
extern SeqKitState preset_normalKitState;
/* Boolean runtime flag that tells current-image helpers whether live routing
   should point at the temporary kit or the normal kit. */
extern uint8_t preset_tmpKitActive;
/* Per-voice source markers used to decide whether a voice-owned parameter
   write should land in the normal image or the temp image. */
extern uint8_t preset_voiceSourceState[PRESET_SYNTH_VOICES];

/* Returns whichever kit image is currently active for current-image ingress.
   The helper reads `preset_tmpKitActive` so callers do not need to inspect the
   storage flags or duplicate the temp-vs-normal selection rule. */
SeqKitState* preset_getCurrentImageKitState(void);
/* Translates the compact morph-image enum into a concrete kit pointer so the
   morph and restore code can address the correct storage block without caring
   about the underlying global variable names. */
SeqKitState* preset_getMorphKitForImage(uint8_t image);

/* Reports whether the temporary kit is currently selected as the active
   current-image target. This remains a tiny compatibility hook while the temp
   pattern switch state machine is still being carved out in later phases. */
uint8_t preset_isTmpKitActive(void);
/* Updates the current-image temp flag without moving any kit data. Callers use
   this when the temp-pattern state machine changes rather than when endpoint
   data itself is written. */
void preset_setTmpKitActive(uint8_t active);

/* Copies the canonical normal kit image into the temp kit sandbox and marks
   the temp image valid. Temp switching and copy-to-temp flows call this when
   they need a fresh temporary image without teaching the parser or sequencer
   how the kit struct is laid out. */
void preset_captureTmpKitState(void);

/* Maps a voice index to the morph-image slot that should receive its per-voice
   endpoint writes. This exists so the parameter router can choose temp vs
   normal storage from one canonical source of truth. */
uint8_t preset_getMorphImageForVoice(uint8_t synthVoice);
/* Returns the current source marker for a voice. The voice-source array is a
   small but important routing aid because it decides whether per-voice writes
   should hit the temp image or the normal image. */
uint8_t preset_getVoiceSourceState(uint8_t synthVoice);
/* Stores the voice-source marker for a voice. Later phases use this to switch
   voice ownership without having to duplicate the image-selection logic in the
   protocol or sequencer layers. */
void preset_setVoiceSourceState(uint8_t synthVoice, uint8_t sourceState);

#endif /* PRESET_KITSTATE_H_ */
