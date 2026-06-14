/*
 * KitState.c
 *
 * Preset owns the authoritative normal and temp kit images plus the minimal
 * voice-source bookkeeping required to choose which image is active. The
 * boundary helpers here are the state-selection side of the contract:
 *   - preset_getCurrentImageKitState()
 *   - preset_getMorphKitForImage()
 *   - preset_isTmpKitActive()
 *   - preset_setTmpKitActive()
 *   - preset_getMorphImageForVoice()
 *   - preset_getVoiceSourceState()
 *   - preset_setVoiceSourceState()
 *   - preset_captureTmpKitState()
 *
 * Those helpers are what Preset uses when live DSP emission or restore work
 * needs to know which image owns the state change.
 */

#include "Preset/KitState.h"
#include <string.h>

/* The temporary kit image is the dedicated storage block for temp-pattern
   playback and any current-image writes that route to the temp bank. Keeping it
   global here gives later phases a single authoritative location for temp
   state instead of scattering copies through sequencer-owned code. */
PresetKitState preset_tmpKitState;
/* The normal kit image holds the canonical preset data used by restore, file
   load, and the default live-edit path when temp playback is inactive. */
PresetKitState preset_normalKitState;
/* Current-image ingress only needs a compact boolean, so this flag tells the
   routing helpers whether to pick the temp image or the normal image. */
uint8_t preset_tmpKitActive = 0;
/* Each synth voice keeps a tiny source marker so per-voice parameter writes can
   decide whether they belong in the temp image or the normal image. */
uint8_t preset_voiceSourceState[PRESET_SYNTH_VOICES];

/* Returns the kit image that current-image ingress should target right now.
   The function reads `preset_tmpKitActive` and then hands back either
   `preset_tmpKitState` or `preset_normalKitState`, which keeps callers out of
   the image-selection policy. */
PresetKitState* preset_getCurrentImageKitState(void)
{
   return preset_tmpKitActive ? &preset_tmpKitState : &preset_normalKitState;
}

/* Maps the compact morph-image enum to the matching kit pointer so morph and
   restore code can address the right image without knowing the global symbol
   layout. */
PresetKitState* preset_getMorphKitForImage(uint8_t image)
{
   return (image == PRESET_MORPH_IMAGE_TMP) ? &preset_tmpKitState
                                            : &preset_normalKitState;
}

/* Reports whether the temp kit is currently selected as the current-image
   target. This is intentionally a tiny accessor because later phases still
   need a compatibility hook while the temp-pattern state machine moves out of
   Sequencer. */
uint8_t preset_isTmpKitActive(void)
{
   return preset_tmpKitActive;
}

/* Updates the temp-image selection flag without touching any kit data. The
   caller is responsible for deciding when a temp/normal switch should happen;
   this helper only records the choice. */
void preset_setTmpKitActive(uint8_t active)
{
   preset_tmpKitActive = (active != 0);
}

/* Chooses the morph-image slot for a voice-owned parameter write. The only
   state it consults is `preset_voiceSourceState`, which keeps the mapping rule
   centralized for the router and future temp-switch logic. */
uint8_t preset_getMorphImageForVoice(uint8_t synthVoice)
{
   if(synthVoice < PRESET_SYNTH_VOICES
      && preset_voiceSourceState[synthVoice] == PRESET_VOICE_SOURCE_TMP)
      return PRESET_MORPH_IMAGE_TMP;

   return PRESET_MORPH_IMAGE_NORMAL;
}

/* Returns the current source marker for a voice, or zero if the caller asks
   for an out-of-range voice. That makes the helper safe to use from routing
   code that already validated the index only partially. */
uint8_t preset_getVoiceSourceState(uint8_t synthVoice)
{
   if(synthVoice >= PRESET_SYNTH_VOICES)
      return 0;

   return preset_voiceSourceState[synthVoice];
}

/* Stores a voice source marker in the compact per-voice array. Later phases
   can update the routing policy by changing this one array instead of teaching
   every parser and sequencer path about temp-vs-normal ownership. */
void preset_setVoiceSourceState(uint8_t synthVoice, uint8_t sourceState)
{
   if(synthVoice < PRESET_SYNTH_VOICES)
      preset_voiceSourceState[synthVoice] = sourceState;
}

/* Copies the canonical normal kit image into the temp sandbox. The temp
   pattern switch and copy-to-temp flows use this helper so the temp image is
   created in one place and the capture semantics stay tied to the kit model
   rather than the sequencer or parser. */
void preset_captureTmpKitState(void)
{
   memcpy(preset_tmpKitState.kitEndpointParams,
          preset_normalKitState.kitEndpointParams,
          END_OF_SOUND_PARAMETERS);
   memcpy(preset_tmpKitState.morphEndpointParams,
          preset_normalKitState.morphEndpointParams,
          END_OF_SOUND_PARAMETERS);
   memcpy(preset_tmpKitState.interpolatedParams,
          preset_normalKitState.interpolatedParams,
          END_OF_SOUND_PARAMETERS);
   memcpy(&preset_tmpKitState.frontPanelAutomationTargets,
          &preset_normalKitState.frontPanelAutomationTargets,
          sizeof(preset_tmpKitState.frontPanelAutomationTargets));
   memcpy(&preset_tmpKitState.morphParameterEndpointAutomationTargets,
          &preset_normalKitState.morphParameterEndpointAutomationTargets,
          sizeof(preset_tmpKitState.morphParameterEndpointAutomationTargets));
   memcpy(&preset_tmpKitState.interpolatedAutomationTargets,
          &preset_normalKitState.interpolatedAutomationTargets,
          sizeof(preset_tmpKitState.interpolatedAutomationTargets));
   preset_tmpKitState.globalMorphAmount = preset_normalKitState.globalMorphAmount;
   memcpy(preset_tmpKitState.voiceMorphBaseAmount,
          preset_normalKitState.voiceMorphBaseAmount,
          PRESET_SYNTH_VOICES);
   memcpy(preset_tmpKitState.voiceMorphAmount,
          preset_normalKitState.voiceMorphAmount,
          PRESET_SYNTH_VOICES);

   preset_tmpKitState.valid = 1;
}
