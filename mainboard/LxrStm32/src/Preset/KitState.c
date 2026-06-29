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
#include "Preset/MorphEngine.h"
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

static void preset_copyFullKitState(PresetKitState *dst, const PresetKitState *src)
{
   if(!dst || !src)
      return;

   memcpy(dst->kitEndpointParams, src->kitEndpointParams, END_OF_SOUND_PARAMETERS);
   memcpy(dst->morphEndpointParams, src->morphEndpointParams, END_OF_SOUND_PARAMETERS);
   memcpy(dst->interpolatedParams, src->interpolatedParams, END_OF_SOUND_PARAMETERS);
   memcpy(&dst->frontPanelAutomationTargets,
          &src->frontPanelAutomationTargets,
          sizeof(dst->frontPanelAutomationTargets));
   memcpy(&dst->morphParameterEndpointAutomationTargets,
          &src->morphParameterEndpointAutomationTargets,
          sizeof(dst->morphParameterEndpointAutomationTargets));
   memcpy(&dst->interpolatedAutomationTargets,
          &src->interpolatedAutomationTargets,
          sizeof(dst->interpolatedAutomationTargets));
   dst->globalMorphAmount = src->globalMorphAmount;
   memcpy(dst->voiceMorphBaseAmount, src->voiceMorphBaseAmount, PRESET_SYNTH_VOICES);
   memcpy(dst->voiceMorphAmount, src->voiceMorphAmount, PRESET_SYNTH_VOICES);
   dst->valid = src->valid;
}

static void preset_refreshInterpolatedParamsFromEndpoints(PresetKitState *kit)
{
   uint16_t param;

   if(!kit)
      return;

   for(param=0; param<END_OF_SOUND_PARAMETERS; param++)
   {
      uint8_t voiceMask = preset_voiceMaskForParameter(param);

      /* Temporary preset storage now doubles as the canonical “last loaded
         preset” image. Rebuild the interpolated cache from endpoint bytes here
         so later PATCH_RESET restores do not depend on whichever image happened
         to be live when the snapshot was refreshed. */
      if(preset_isAutomationTargetSelectorParam(param)
         || preset_isMorphAmountParam(param)
         || !voiceMask)
      {
         kit->interpolatedParams[param] = kit->kitEndpointParams[param];
      }
      else
      {
         uint8_t synthVoice = preset_firstVoiceForMask(voiceMask);

         if(synthVoice >= PRESET_SYNTH_VOICES)
            kit->interpolatedParams[param] = kit->kitEndpointParams[param];
         else
            kit->interpolatedParams[param] =
               preset_interpolateMorphValue(kit->kitEndpointParams[param],
                                            kit->morphEndpointParams[param],
                                            kit->voiceMorphAmount[synthVoice]);
      }
   }
}

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
   preset_copyFullKitState(&preset_tmpKitState, &preset_normalKitState);
   preset_tmpKitState.valid = 1;
}

void preset_copyKitEndpoints(PresetKitState *dst,
                             const PresetKitState *src,
                             uint8_t endpointMode)
{
   if(!dst || !src)
      return;

   /* Partial file loads need partial snapshot copies:
      .snd/instrument loads refresh only the kit/front endpoint image, morph
      loads refresh only the morph endpoint image, and .prf/.all refresh both.
      Morph amounts remain live performance state, so this helper copies the
      loaded endpoint subsets without overwriting the destination morph-amount
      controls. */
   if(endpointMode != PRESET_KIT_ENDPOINT_MORPH_ONLY)
   {
      memcpy(dst->kitEndpointParams, src->kitEndpointParams, END_OF_SOUND_PARAMETERS);
      memcpy(&dst->frontPanelAutomationTargets,
             &src->frontPanelAutomationTargets,
             sizeof(dst->frontPanelAutomationTargets));
      memcpy(&dst->interpolatedAutomationTargets,
             &src->interpolatedAutomationTargets,
             sizeof(dst->interpolatedAutomationTargets));
   }

   if(endpointMode != PRESET_KIT_ENDPOINT_FRONT_ONLY)
   {
      memcpy(dst->morphEndpointParams, src->morphEndpointParams, END_OF_SOUND_PARAMETERS);
      memcpy(&dst->morphParameterEndpointAutomationTargets,
             &src->morphParameterEndpointAutomationTargets,
             sizeof(dst->morphParameterEndpointAutomationTargets));
   }

   preset_refreshInterpolatedParamsFromEndpoints(dst);
   dst->valid = 1;
}
