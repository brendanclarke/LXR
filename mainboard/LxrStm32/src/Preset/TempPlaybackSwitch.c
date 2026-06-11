/*
 * TempPlaybackSwitch.c
 *
 * Preset owns the temp/normal pattern boundary state machine. The sequencer
 * still advances the clock and decides when a switch should happen, but the
 * source-selection policy and the bookkeeping for temp playback live here.
 */

#include "Preset/TempPlaybackSwitch.h"
#include "Preset/KitState.h"
#include "Preset/MorphEngine.h"
#include "Preset/EndpointRestore.h"

uint8_t seq_pendingPattern = 0;
uint8_t seq_perTrackPendingPattern[NUM_TRACKS];
uint8_t seq_newPatternAvailable = 0;
uint8_t seq_newPatternExecuted = 0;
uint8_t seq_loadPendingFlag = 0;
uint8_t seq_loadSeqNow = 0;
uint8_t seq_tmpBoundaryPatternSwitchAck = 0;

/* Returns the active temp/normal source selector for a pattern number. The
   temp pattern is preserved as its own sentinel so pattern lookups stay fast
   and explicit. */
uint8_t seq_trackPatternUsesTmp(uint8_t pattern)
{
   return seq_normalizePatternNumber(pattern) == SEQ_TMP_PATTERN;
}

/* Resolves whether a voice should read from the temp image when pattern
   selection is the source of truth. The hihat pair is treated as a coupled
   unit because both tracks share one synth voice and one parameter mask. */
uint8_t seq_synthVoiceUsesTmpFromTrackPatterns(const uint8_t *patternForTrack,
                                               uint8_t synthVoice)
{
   if(synthVoice >= SEQ_SYNTH_VOICES)
      return 0;

   if(synthVoice == 5)
   {
      return seq_trackPatternUsesTmp(patternForTrack[5])
          || seq_trackPatternUsesTmp(patternForTrack[6]);
   }

   return seq_trackPatternUsesTmp(patternForTrack[synthVoice]);
}

/* Applies the endpoint and automation targets for a voice based on the temp or
   normal image that currently owns it. This is the only place that should
   decide which kit image feeds the live DSP when a voice source changes. */
static void seq_applyVoiceSource(uint8_t synthVoice, uint8_t useTmp)
{
   const SeqKitState *kit = useTmp ? &seq_tmpKitState : &seq_normalKitState;

   preset_applyVoiceParameterValues(kit, synthVoice);
   seq_applyVoiceAutomationTargets(&kit->interpolatedAutomationTargets, synthVoice);
}

/* Marks a voice as belonging to the temp or normal image and applies the
   corresponding kit state to the live DSP. When the temp image is requested
   but not yet valid, the helper seeds it from the current normal image first. */
static void seq_markVoiceSourceTarget(uint8_t synthVoice, uint8_t useTmp)
{
   uint8_t targetState = useTmp ? SEQ_VOICE_SOURCE_TMP : SEQ_VOICE_SOURCE_NORMAL;

   if(synthVoice >= SEQ_SYNTH_VOICES)
      return;

   if(seq_voiceSourceState[synthVoice] == targetState)
      return;

   if(useTmp && !seq_tmpKitState.valid)
      preset_captureTmpKitState();

   preset_setVoiceSourceState(synthVoice, targetState);
   preset_selectVoiceMorphAmountFromKit(synthVoice,
                                       useTmp ? &seq_tmpKitState
                                              : &seq_normalKitState);
   seq_applyVoiceSource(synthVoice, useTmp);
}

/* Reports whether every voice source currently points at the temp image. */
uint8_t seq_allVoiceSourcesUseTmp(void)
{
   uint8_t synthVoice;

   for(synthVoice=0;synthVoice<SEQ_SYNTH_VOICES;synthVoice++)
   {
      if(seq_voiceSourceState[synthVoice] != SEQ_VOICE_SOURCE_TMP)
         return 0;
   }

   return 1;
}

/* Reports whether every voice source currently points at the normal image. */
uint8_t seq_allVoiceSourcesUseNormal(void)
{
   uint8_t synthVoice;

   for(synthVoice=0;synthVoice<SEQ_SYNTH_VOICES;synthVoice++)
   {
      if(seq_voiceSourceState[synthVoice] != SEQ_VOICE_SOURCE_NORMAL)
         return 0;
   }

   return 1;
}

/* Updates the per-voice source state after a pattern change and optionally
   pushes restore traffic for the affected voices. */
void seq_updateVoiceSourcesForPatternChange(const uint8_t *oldPatternForTrack,
                                            uint8_t pushEndpointUpdates)
{
   uint8_t synthVoice;
   uint8_t changedVoiceMask = 0;

   if(!oldPatternForTrack)
      return;

   for(synthVoice=0;synthVoice<SEQ_SYNTH_VOICES;synthVoice++)
   {
      uint8_t oldUseTmp =
         seq_synthVoiceUsesTmpFromTrackPatterns(oldPatternForTrack, synthVoice);
      uint8_t newUseTmp =
         seq_synthVoiceUsesTmpFromTrackPatterns(seq_perTrackActivePattern, synthVoice);

      if(oldUseTmp != newUseTmp)
      {
         seq_markVoiceSourceTarget(synthVoice, newUseTmp);
         changedVoiceMask |= (uint8_t)(1 << synthVoice);
      }
   }

   if(pushEndpointUpdates && changedVoiceMask)
      seq_pushEndpointUpdateForVoiceSourceChange(changedVoiceMask);
}

/* Copies the canonical normal kit into the temp image, selects the temp image
   as the active current-image target, and refreshes the live morph caches so
   the AVR menu and DSP stay coherent across the boundary. */
void seq_setTmpKitActive(uint8_t active)
{
   if(active)
   {
      if(preset_isTmpKitActive())
         return;

      if(!seq_tmpKitState.valid)
      {
         preset_captureTmpKitState();
      }

      preset_setTmpKitActive(1);
      preset_syncVMorphAmountMirrorsFromLiveSources();
      preset_invalidateLiveMorphApplyCache(PRESET_MORPH_IMAGE_TMP);
      seq_maybePushKitEndpointsToFrontWithGlobalMorphReport(&seq_tmpKitState);
      return;
   }

   if(!preset_isTmpKitActive())
      return;

   preset_setTmpKitActive(0);
   preset_syncVMorphAmountMirrorsFromLiveSources();
   preset_invalidateLiveMorphApplyCache(PRESET_MORPH_IMAGE_NORMAL);
   seq_maybePushKitEndpointsToFrontWithGlobalMorphReport(&seq_normalKitState);
}

uint8_t seq_consumeTmpBoundaryPatternSwitchAck(void)
{
   uint8_t ret = seq_tmpBoundaryPatternSwitchAck;
   seq_tmpBoundaryPatternSwitchAck = 0;
   return ret;
}
