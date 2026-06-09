/*
 * KitState.c
 *
 * Preset owns the authoritative normal and temp kit images plus the minimal
 * voice-source bookkeeping required to choose which image is active.
 */

#include "Preset/KitState.h"

/* The temporary kit image is the dedicated storage block for temp-pattern
   playback and any current-image writes that route to the temp bank. Keeping it
   global here gives later phases a single authoritative location for temp
   state instead of scattering copies through sequencer-owned code. */
SeqKitState preset_tmpKitState;
/* The normal kit image holds the canonical preset data used by restore, file
   load, and the default live-edit path when temp playback is inactive. */
SeqKitState preset_normalKitState;
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
SeqKitState* preset_getCurrentImageKitState(void)
{
   return preset_tmpKitActive ? &preset_tmpKitState : &preset_normalKitState;
}

/* Maps the compact morph-image enum to the matching kit pointer so morph and
   restore code can address the right image without knowing the global symbol
   layout. */
SeqKitState* preset_getMorphKitForImage(uint8_t image)
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
