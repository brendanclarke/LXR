/*
 * TempPlaybackSwitch.c
 *
 * Preset owns the temp/normal pattern boundary state machine and the
 * follow-on live replay requests that boundary changes trigger. Sequencer
 * still advances the clock and decides when a switch should happen, but the
 * source-selection policy, the per-voice ownership bookkeeping, and the call
 * into MorphEngine's live DSP bridge all live here.
 */

#include "Preset/TempPlaybackSwitch.h"
#include "Sequencer/sequencer.h"
#include "Preset/KitState.h"
#include "Preset/MorphEngine.h"
#include "Preset/EndpointRestore.h"
#include "Preset/ParameterIngress.h"

PresetTempPlaybackSwitchState preset_tempPlaybackSwitchState = {0};

/* Returns the active temp/normal source selector for a pattern number. The
   temp pattern is preserved as its own sentinel so pattern lookups stay fast
   and explicit. */
uint8_t preset_trackPatternUsesTmp(uint8_t pattern)
{
   return pat_normalizePatternNumber(pattern) == SEQ_TMP_PATTERN;
}

/* Resolves whether a voice should read from the temp image when pattern
   selection is the source of truth. The hihat pair is treated as a coupled
   unit because both tracks share one synth voice and one parameter mask. */
uint8_t preset_synthVoiceUsesTmpFromTrackPatterns(const uint8_t *patternForTrack,
                                               uint8_t synthVoice)
{
   if(synthVoice >= SEQ_SYNTH_VOICES)
      return 0;

   if(synthVoice == 5)
   {
      return preset_trackPatternUsesTmp(patternForTrack[5])
          || preset_trackPatternUsesTmp(patternForTrack[6]);
   }

   return preset_trackPatternUsesTmp(patternForTrack[synthVoice]);
}

/* Applies the endpoint and automation targets for a voice based on the temp or
   normal image that currently owns it. This is the only place that should
   decide which kit image feeds the live DSP when a voice source changes. */
static void preset_applyVoiceSource(uint8_t synthVoice, uint8_t useTmp)
{
   const PresetKitState *kit = useTmp ? &preset_tmpKitState : &preset_normalKitState;

   preset_applyVoiceParameterValues(kit, synthVoice);
   preset_applyVoiceAutomationTargets(&kit->interpolatedAutomationTargets, synthVoice);
}

/* Marks a voice as belonging to the temp or normal image and applies the
   corresponding kit state to the live DSP. When the temp image is requested
   but not yet valid, the helper seeds it from the current normal image first. */
static void preset_markVoiceSourceTarget(uint8_t synthVoice, uint8_t useTmp)
{
   uint8_t targetState = useTmp ? SEQ_VOICE_SOURCE_TMP : SEQ_VOICE_SOURCE_NORMAL;

   if(synthVoice >= SEQ_SYNTH_VOICES)
      return;

   if(preset_voiceSourceState[synthVoice] == targetState)
      return;

   if(useTmp && !preset_tmpKitState.valid)
      preset_captureTmpKitState();

   preset_setVoiceSourceState(synthVoice, targetState);
   preset_selectVoiceMorphAmountFromKit(synthVoice,
                                       useTmp ? &preset_tmpKitState
                                              : &preset_normalKitState);
   preset_applyVoiceSource(synthVoice, useTmp);
}

/* Reports whether every voice source currently points at the temp image. */
uint8_t preset_allVoiceSourcesUseTmp(void)
{
   uint8_t synthVoice;

   for(synthVoice=0;synthVoice<SEQ_SYNTH_VOICES;synthVoice++)
   {
      if(preset_voiceSourceState[synthVoice] != SEQ_VOICE_SOURCE_TMP)
         return 0;
   }

   return 1;
}

/* Reports whether every voice source currently points at the normal image. */
uint8_t preset_allVoiceSourcesUseNormal(void)
{
   uint8_t synthVoice;

   for(synthVoice=0;synthVoice<SEQ_SYNTH_VOICES;synthVoice++)
   {
      if(preset_voiceSourceState[synthVoice] != SEQ_VOICE_SOURCE_NORMAL)
         return 0;
   }

   return 1;
}

uint8_t preset_isTempPresetPlaybackActive(void)
{
   return preset_tempPlaybackSwitchState.tempPresetPlaybackActive;
}

void preset_setTempPresetPlaybackActive(uint8_t active)
{
   preset_tempPlaybackSwitchState.tempPresetPlaybackActive = (active != 0);
   if(!active)
      preset_tempPlaybackSwitchState.resnapshotTmpFromNormalPending = 0;
}

void preset_reapplyCurrentPlaybackSources(void)
{
   uint8_t synthVoice;
   const PresetKitState *sharedKit = preset_isTmpKitActive()
                                   ? &preset_tmpKitState
                                   : &preset_normalKitState;

   preset_syncVMorphAmountMirrorsFromLiveSources();
   preset_applySharedParameterValues(sharedKit);

   for(synthVoice=0; synthVoice<SEQ_SYNTH_VOICES; synthVoice++)
   {
      const PresetKitState *voiceKit =
         (preset_voiceSourceState[synthVoice] == PRESET_VOICE_SOURCE_TMP)
       ? &preset_tmpKitState
       : &preset_normalKitState;

      preset_applyVoiceParameterValues(voiceKit, synthVoice);
      preset_applyVoiceAutomationTargets(&voiceKit->interpolatedAutomationTargets,
                                         synthVoice);
   }
}

uint8_t preset_reloadNormalFromTemporaryPreset(void)
{
   if(preset_isTempPresetPlaybackActive() || !preset_tmpKitState.valid)
      return 0;

   /* PATCH_RESET must restore both kit/front and morph endpoints, but not the
      live morph amount controls. Those remain performance state and are
      applied when the refreshed normal image is replayed below. */
   preset_setEndpointIngressSuppressed(1);
   preset_copyKitEndpoints(&preset_normalKitState,
                           &preset_tmpKitState,
                           PRESET_KIT_ENDPOINT_BOTH);
   preset_setEndpointIngressSuppressed(0);

   preset_invalidateLiveMorphApplyCache(PRESET_MORPH_IMAGE_NORMAL);
   preset_reapplyCurrentPlaybackSources();
   preset_maybePushKitEndpointsToFrontWithGlobalMorphReport(&preset_normalKitState);
   return 1;
}

void preset_resnapshotTemporaryPresetFromNormal(void)
{
   preset_setEndpointIngressSuppressed(1);
   preset_copyKitEndpoints(&preset_tmpKitState,
                           &preset_normalKitState,
                           PRESET_KIT_ENDPOINT_BOTH);
   preset_setEndpointIngressSuppressed(0);
   preset_invalidateLiveMorphApplyCache(PRESET_MORPH_IMAGE_TMP);
   preset_tempPlaybackSwitchState.resnapshotTmpFromNormalPending = 0;
   preset_setTempPresetPlaybackActive(0);
}

/* Updates the per-voice source state after a pattern change and optionally
   pushes restore traffic for the affected voices. */
void preset_updateVoiceSourcesForPatternChange(const uint8_t *oldPatternForTrack,
                                            uint8_t pushEndpointUpdates)
{
   uint8_t synthVoice;
   uint8_t changedVoiceMask = 0;

   if(!oldPatternForTrack)
      return;

   for(synthVoice=0;synthVoice<SEQ_SYNTH_VOICES;synthVoice++)
   {
      uint8_t oldUseTmp =
         preset_synthVoiceUsesTmpFromTrackPatterns(oldPatternForTrack, synthVoice);
      uint8_t newUseTmp =
         preset_synthVoiceUsesTmpFromTrackPatterns(seq_perTrackActivePattern, synthVoice);

      if(oldUseTmp != newUseTmp)
      {
         preset_markVoiceSourceTarget(synthVoice, newUseTmp);
         changedVoiceMask |= (uint8_t)(1 << synthVoice);
      }
   }

   if(pushEndpointUpdates && changedVoiceMask)
      preset_pushEndpointUpdateForVoiceSourceChange(changedVoiceMask);
}

/* Copies the canonical normal kit into the temp image, selects the temp image
   as the active current-image target, and refreshes the live morph caches so
   the AVR menu and DSP stay coherent across the boundary. */
void preset_setTempPlaybackActive(uint8_t active)
{
   if(active)
   {
      if(preset_isTmpKitActive())
         return;

      if(!preset_tmpKitState.valid)
      {
         preset_captureTmpKitState();
      }

      preset_setTmpKitActive(1);
      preset_syncVMorphAmountMirrorsFromLiveSources();
      preset_invalidateLiveMorphApplyCache(PRESET_MORPH_IMAGE_TMP);
      preset_maybePushKitEndpointsToFrontWithGlobalMorphReport(&preset_tmpKitState);
      return;
   }

   if(!preset_isTmpKitActive())
      return;

   preset_setTmpKitActive(0);
   preset_syncVMorphAmountMirrorsFromLiveSources();
   preset_invalidateLiveMorphApplyCache(PRESET_MORPH_IMAGE_NORMAL);
   preset_maybePushKitEndpointsToFrontWithGlobalMorphReport(&preset_normalKitState);
}

uint8_t preset_consumeTmpBoundaryPatternSwitchAck(void)
{
   uint8_t ret = preset_tempPlaybackSwitchState.tmpBoundaryPatternSwitchAck;
   preset_tempPlaybackSwitchState.tmpBoundaryPatternSwitchAck = 0;
   return ret;
}
