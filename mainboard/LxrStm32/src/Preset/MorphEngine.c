/*
 * MorphEngine.c
 *
 * Preset owns the live DSP application path here: async parameter drain,
 * interpolation mechanics, voice morph modulations, and the live replay of
 * automation targets that have already been decoded by the ingress layer.
 * ParameterIngress stores the raw bytes, KitState owns the image selection,
 * and this file emits the resulting values into the modulation nodes and live
 * DSP parameters.
 */

#include "Preset/MorphEngine.h"
#include "Preset/KitState.h"
#include "Preset/ParameterArray.h"
#include "Preset/ParameterIngress.h"
#include "Sequencer/sequencer.h"
#include "DSPAudio/modulationNode.h"
#include "DSPAudio/DrumVoice.h"
#include "DSPAudio/CymbalVoice.h"
#include "DSPAudio/HiHat.h"
#include "DSPAudio/Snare.h"
#include "MIDI/MidiParser.h"
#include "uARTFrontSYX/frontPanelSendingProtocol.h"
#include <string.h>

/* Trigger flag used to signal when a morph amount has changed. */
uint8_t preset_vMorphFlag = 0;

/* Legacy mirror array for morph amounts. [0] is global, [1..6] are per-voice. */
uint8_t preset_vMorphAmount[7] = {0};

/* Flag to disable morph interpolation during certain loading operations. */
uint8_t preset_morphLoadDisabled = 0;

/* Internal cursor for background morph scanning. */
static uint16_t preset_morphScanParam = 0;

/* Internal phase for LFO-to-morph drain. Phase 0 is baseline morph. */
static uint8_t preset_morphDrainPhase = 0;

/* Live-DSP apply cache to suppress redundant updates. */
static uint8_t preset_liveMorphAppliedValue[PRESET_MORPH_IMAGE_COUNT][END_OF_SOUND_PARAMETERS];
static uint8_t preset_liveMorphAppliedKnown[PRESET_MORPH_IMAGE_COUNT][END_OF_SOUND_PARAMETERS];

/* Shared parameter cache for non-voice parameters. */
static uint8_t preset_liveSharedParams[END_OF_SOUND_PARAMETERS];
static uint8_t preset_liveSharedParamsValid[END_OF_SOUND_PARAMETERS];

/* Internal helper to get LFO modulation node for a voice. */
static ModulationNode* preset_getLfoModNode(uint8_t voice)
{
   switch(voice)
   {
      case 0:
      case 1:
      case 2:
         return &voiceArray[voice].lfo.modTarget;
      case 3:
         return &snareVoice.lfo.modTarget;
      case 4:
         return &cymbalVoice.lfo.modTarget;
      case 5:
      default:
         return &hatVoice.lfo.modTarget;
   }
}

/* Synchronizes the legacy `preset_vMorphAmount[]` mirrors from the live
   voice sources. */
void preset_syncVMorphAmountMirrorsFromLiveSources(void)
{
   uint8_t synthVoice;

   preset_vMorphAmount[0] = preset_getCurrentImageKitState()->globalMorphAmount;

   for(synthVoice=0;synthVoice<PRESET_SYNTH_VOICES;synthVoice++)
   {
      const PresetKitState *kit =
         preset_getMorphKitForImage(preset_getMorphImageForVoice(synthVoice));
      preset_vMorphAmount[synthVoice + 1] = kit->voiceMorphAmount[synthVoice];
   }
}

/* Updates the morph mirror for a specific voice from its active kit. */
void preset_selectVoiceMorphAmountFromKit(uint8_t synthVoice, const PresetKitState *kit)
{
   if(!kit || synthVoice >= PRESET_SYNTH_VOICES)
      return;

   preset_vMorphAmount[synthVoice + 1] = kit->voiceMorphAmount[synthVoice];
}

/* Sets the live morph amount for a voice. */
void preset_setVoiceMorphLiveAmount(uint8_t synthVoice, uint8_t morphAmount)
{
   PresetKitState *kit;

   if(synthVoice >= PRESET_SYNTH_VOICES)
      return;

   kit = preset_getMorphKitForImage(preset_getMorphImageForVoice(synthVoice));
   kit->voiceMorphAmount[synthVoice] = morphAmount;
   preset_vMorphAmount[synthVoice + 1] = morphAmount;
}

/* Internal predicate to check if a specific morph image/voice is live. */
static uint8_t preset_morphImageVoiceIsLive(uint8_t image, uint8_t synthVoice)
{
   uint8_t sourceState;

   if(synthVoice >= PRESET_SYNTH_VOICES)
      return 0;

   sourceState = (image == PRESET_MORPH_IMAGE_TMP) ? PRESET_VOICE_SOURCE_TMP
                                                   : PRESET_VOICE_SOURCE_NORMAL;
   return preset_getVoiceSourceState(synthVoice) == sourceState;
}

/* Applies a live voice-specific automation target block to the modulation
   nodes that drive the current voice. This is the DSP emission side of the
   automation bridge and is used by temp playback, restore replay, and morph
   target updates once the target selector bytes have already been resolved. */
void preset_applyVoiceAutomationTargets(const PresetAutomationTargets *source,
                                        uint8_t synthVoice)
{
   ModulationNode *lfoNode;

   if(!source || synthVoice >= SEQ_SYNTH_VOICES)
      return;

   lfoNode = preset_getLfoModNode(synthVoice);
   if(source->lfoDestinationValid & (1 << synthVoice))
   {
      modNode_setDestination(lfoNode, source->lfoDestination[synthVoice]);
      modNode_updateValue(lfoNode, lfoNode->lastVal);
   }

   if(source->velocityDestinationValid & (1 << synthVoice))
   {
      if(preset_isMorphAmountParam(source->velocityDestination[synthVoice]))
      {
         modNode_setDestination(&velocityModulators[synthVoice], PAR_NONE);
      }
      else
      {
         modNode_setDestination(&velocityModulators[synthVoice],
                                source->velocityDestination[synthVoice]);
         modNode_updateValue(&velocityModulators[synthVoice],
                             velocityModulators[synthVoice].lastVal);
      }
   }
}

/* Session 025 deprecation note: shared macro replay is frozen so the macro
   feature can be removed after the test build confirms the rest of the code
   no longer depends on it. */
static void preset_applySharedAutomationTargets(const PresetAutomationTargets *source)
{
   (void)source;
}

/* Replays the normal kit image's live automation targets after the temp/normal
   boundary closes. The per-voice bridge only targets voices that currently live
   in the normal image, and the shared target refresh is skipped while the temp
   kit remains active. */
void preset_applyNormalEndpointAutomationTargets(void)
{
   uint8_t synthVoice;

   for(synthVoice=0;synthVoice<SEQ_SYNTH_VOICES;synthVoice++)
   {
      if(preset_getMorphImageForVoice(synthVoice) == PRESET_MORPH_IMAGE_NORMAL)
         preset_applyVoiceAutomationTargets(&preset_normalKitState.frontPanelAutomationTargets,
                                            synthVoice);
   }

   if(!preset_isTmpKitActive())
      preset_applySharedAutomationTargets(&preset_normalKitState.frontPanelAutomationTargets);
}

/* Invalidates the live-apply cache for a specific morph image. */
void preset_invalidateLiveMorphApplyCache(uint8_t image)
{
   if(image < PRESET_MORPH_IMAGE_COUNT)
      memset(preset_liveMorphAppliedKnown[image], 0, END_OF_SOUND_PARAMETERS);
}

/* Invalidates all live-apply caches. */
void preset_invalidateAllLiveMorphApplyCaches(void)
{
   uint8_t image;

   for(image=0;image<PRESET_MORPH_IMAGE_COUNT;image++)
      preset_invalidateLiveMorphApplyCache(image);
}

/* Resets the live-apply cache. */
void preset_resetLiveMorphApplyCache(void)
{
   preset_invalidateAllLiveMorphApplyCaches();
}

/* Checks if a parameter needs to be applied to the DSP. */
uint8_t preset_liveMorphApplyNeeded(uint8_t image, uint16_t param, uint8_t value)
{
   if(image >= PRESET_MORPH_IMAGE_COUNT || param >= END_OF_SOUND_PARAMETERS)
      return 0;

   if(preset_liveMorphAppliedKnown[image][param]
      && preset_liveMorphAppliedValue[image][param] == value)
   {
      return 0;
   }

   preset_liveMorphAppliedValue[image][param] = value;
   preset_liveMorphAppliedKnown[image][param] = 1;
   return 1;
}

/* Interpolates between two byte values. */
uint8_t preset_interpolateMorphValue(uint8_t a, uint8_t b, uint8_t x)
{
   int32_t fixedPointValue = ((int32_t)a * 256) + (((int32_t)b - (int32_t)a) * x);
   uint8_t result;

   if(fixedPointValue < 0)
      fixedPointValue = 0;
   else if(fixedPointValue > 0xffff)
      fixedPointValue = 0xffff;

   result = (uint8_t)(((uint32_t)fixedPointValue) / 256);

   return (uint8_t)((fixedPointValue & 0xff) < 0x7f ? result : result + 1);
}

/* Applies a selector-bearing automation change to the live DSP when the
   relevant voice is currently active. This keeps the decoded ingress value and
   the live modulation node in sync without forcing callers to know the bridge
   details. */
static void preset_applyLiveAutomationTargetSelector(uint8_t image,
                                                     uint16_t param,
                                                     uint8_t selector)
{
   PresetAutomationTargets target;
   uint8_t synthVoice;

   memset(&target, 0, sizeof(target));

   if(param >= PAR_VEL_DEST_1 && param <= PAR_VEL_DEST_6)
   {
      synthVoice = (uint8_t)(param - PAR_VEL_DEST_1);
      target.velocityDestination[synthVoice] =
         preset_resolveAutomationTargetSelector(selector);
      target.velocityDestinationValid = (uint8_t)(1 << synthVoice);
   }
   else if(param >= PAR_TARGET_LFO1 && param <= PAR_TARGET_LFO6)
   {
      synthVoice = (uint8_t)(param - PAR_TARGET_LFO1);
      target.lfoDestination[synthVoice] =
         preset_resolveAutomationTargetSelector(selector);
      target.lfoDestinationValid = (uint8_t)(1 << synthVoice);
   }
   else
   {
      return;
   }

   if(preset_morphImageVoiceIsLive(image, synthVoice))
      preset_applyVoiceAutomationTargets(&target, synthVoice);
}

/* Applies a single morphed parameter value to the DSP. */
void preset_applyLiveMorphParameterValue(uint8_t image,
                                         uint8_t synthVoice,
                                         uint16_t param,
                                         uint8_t value)
{
   if(!preset_morphImageVoiceIsLive(image, synthVoice))
      return;

   if(!preset_liveMorphApplyNeeded(image, param, value))
      return;

   preset_applyLiveAutomationTargetSelector(image, param, value);
   preset_applySingleParameterValue(param, value);
}

/* Converts automation value to morph amount. */
uint8_t preset_morphAutomationValueToAmount(uint8_t morphValue)
{
   if(morphValue >= 127)
      return 255;

   return (uint8_t)(morphValue * 2);
}

/* Advances the background morph scanning cursor. */
static void preset_advanceMorphScanCursor(void)
{
   preset_morphScanParam++;
   if(preset_morphScanParam >= END_OF_SOUND_PARAMETERS)
      preset_morphScanParam = 0;
}

/* Internal helper to check LFO-to-morph assignment. */
static uint8_t preset_lfoMorphAssignmentForSource(uint8_t sourceVoice,
                                                  uint8_t *targetVoice,
                                                  ModulationNode **lfoNode)
{
   uint8_t image;
   PresetKitState *kit;
   uint16_t destination;
   uint8_t morphVoice;

   if(sourceVoice >= PRESET_SYNTH_VOICES)
      return 0;

   image = preset_getMorphImageForVoice(sourceVoice);
   kit = preset_getMorphKitForImage(image);

   if(!(kit->frontPanelAutomationTargets.lfoDestinationValid &
        (uint8_t)(1 << sourceVoice)))
   {
      return 0;
   }

   destination = kit->frontPanelAutomationTargets.lfoDestination[sourceVoice];
   morphVoice = preset_morphVoiceForParam(destination);
   if(morphVoice >= PRESET_SYNTH_VOICES)
      return 0;

   if(targetVoice)
      *targetVoice = morphVoice;
   if(lfoNode)
      *lfoNode = preset_getLfoModNode(sourceVoice);

   return 1;
}

/* Internal predicate to check if a voice has an LFO-to-morph overlay. */
static uint8_t preset_voiceHasLfoMorphOverlay(uint8_t targetVoice)
{
   uint8_t sourceVoice;

   if(targetVoice >= PRESET_SYNTH_VOICES)
      return 0;

   for(sourceVoice=0;sourceVoice<PRESET_SYNTH_VOICES;sourceVoice++)
   {
      uint8_t morphVoice;
      if(preset_lfoMorphAssignmentForSource(sourceVoice, &morphVoice, 0)
         && morphVoice == targetVoice)
      {
         return 1;
      }
   }

   return 0;
}

/* Advances the morph drain phase. */
static void preset_advanceMorphDrainPhase(void)
{
   uint8_t nextPhase;

   for(nextPhase=(uint8_t)(preset_morphDrainPhase + 1);
       nextPhase<=PRESET_SYNTH_VOICES;
       nextPhase++)
   {
      if(preset_lfoMorphAssignmentForSource((uint8_t)(nextPhase - 1), 0, 0))
      {
         preset_morphDrainPhase = nextPhase;
         return;
      }
   }

   preset_morphDrainPhase = 0;
   preset_advanceMorphScanCursor();
}

/* Sets the global morph amount. */
void preset_setGlobalMorphAmount(uint8_t morphAmount)
{
   PresetKitState *kit = preset_getCurrentImageKitState();
   uint8_t voice;

   kit->globalMorphAmount = morphAmount;

   for(voice=0;voice<PRESET_SYNTH_VOICES;voice++)
   {
      kit->voiceMorphBaseAmount[voice] = morphAmount;
      kit->voiceMorphAmount[voice] = morphAmount;
   }

   preset_syncVMorphAmountMirrorsFromLiveSources();
}

/* Sets global morph amount from automation. */
void preset_setGlobalMorphAutomationValue(uint8_t morphValue)
{
   uint8_t morphAmount = preset_morphAutomationValueToAmount(morphValue);

   preset_setGlobalMorphAmount(morphAmount);
   frontPanelSending_sendGlobalMorphRuntimeReport(morphAmount);
   frontPanelSending_sendVoiceMorphRuntimeReports();
}

/* Resets per-voice morph amounts to global. */
void preset_resetVoiceMorphAmountsToGlobal(void)
{
   PresetKitState *kit = preset_getCurrentImageKitState();
   uint8_t synthVoice;

   for(synthVoice=0;synthVoice<PRESET_SYNTH_VOICES;synthVoice++)
   {
      kit->voiceMorphBaseAmount[synthVoice] = kit->globalMorphAmount;
      kit->voiceMorphAmount[synthVoice] = kit->globalMorphAmount;
   }

   preset_syncVMorphAmountMirrorsFromLiveSources();
}

/* Sets morph amount for a specific voice. */
void preset_setVoiceMorphAmount(uint8_t synthVoice, uint8_t morphAmount)
{
   PresetKitState *kit;

   if(synthVoice >= PRESET_SYNTH_VOICES)
      return;

   kit = preset_getMorphKitForImage(preset_getMorphImageForVoice(synthVoice));
   kit->voiceMorphBaseAmount[synthVoice] = morphAmount;
   kit->voiceMorphAmount[synthVoice] = morphAmount;
   preset_vMorphAmount[synthVoice + 1] = morphAmount;
}

/* Reads morph amount for a specific voice. */
uint8_t preset_getVoiceMorphAmount(uint8_t synthVoice)
{
   const PresetKitState *kit;

   if(synthVoice >= PRESET_SYNTH_VOICES)
      return 0;

   kit = preset_getMorphKitForImage(preset_getMorphImageForVoice(synthVoice));
   return kit->voiceMorphAmount[synthVoice];
}

/* Sets morph amount for a specific voice from automation. */
void preset_setVoiceMorphAutomationValue(uint8_t synthVoice, uint8_t morphValue)
{
   uint8_t morphAmount;

   if(synthVoice >= PRESET_SYNTH_VOICES)
      return;

   morphAmount = preset_morphAutomationValueToAmount(morphValue);
   preset_setVoiceMorphLiveAmount(synthVoice, morphAmount);
   frontPanelSending_sendVoiceMorphRuntimeReport(synthVoice, morphAmount);
}

/* Sets morph amount for a mask of voices from automation. */
void preset_setVoiceMorphMaskAutomationValue(uint8_t voiceMask, uint8_t morphValue)
{
   uint8_t synthVoice;

   for(synthVoice=0;synthVoice<PRESET_SYNTH_VOICES;synthVoice++)
   {
      uint8_t bit = (uint8_t)(1 << synthVoice);
      if(voiceMask & bit)
         preset_setVoiceMorphAutomationValue(synthVoice, morphValue);
   }

   if(voiceMask & 0x40)
      preset_setVoiceMorphAutomationValue(5, morphValue);
}

/* Applies morph modulation overlay. */
void preset_modulateVoiceMorphAmount(uint8_t synthVoice, float amount, float value)
{
   float travel;
   uint8_t modulationAmount;
   uint8_t image;
   PresetKitState *kit;
   uint8_t i;

   if(synthVoice >= PRESET_SYNTH_VOICES)
      return;

   travel = amount * value;
   if(travel < 0.f)
      travel = 0.f;
   else if(travel > 1.f)
      travel = 1.f;

   modulationAmount = (uint8_t)((travel * 255.f) + 0.5f);
   image = preset_getMorphImageForVoice(synthVoice);
   kit = preset_getMorphKitForImage(image);

   for(i=0;i<PRESET_VOICE_PARAM_LENGTH;i++)
   {
      uint16_t param =
         preset_canonicalParamFromVoiceMask(preset_voiceParamMask[synthVoice][i]);
      uint8_t baseline;
      uint8_t liveValue;

      if(param >= END_OF_SOUND_PARAMETERS
         || preset_isAutomationTargetSelectorParam(param)
         || preset_isMorphAmountParam(param))
      {
         continue;
      }

      baseline = kit->interpolatedParams[param];

      liveValue = baseline;
      if(modulationAmount)
      {
         liveValue = preset_interpolateMorphValue(baseline,
                                                  kit->morphEndpointParams[param],
                                                  modulationAmount);
      }

      preset_applyLiveMorphParameterValue(image, synthVoice, param, liveValue);
   }
}

/* Background morph worker. */
void preset_serviceMorphInterpolation(void)
{
   PresetKitState *kit;
   uint8_t synthVoice;
   uint8_t image;
   uint8_t voiceMask;
   uint16_t param;
   uint8_t isAutomationTargetSelector;

   if(preset_morphScanParam >= END_OF_SOUND_PARAMETERS)
      preset_morphScanParam = 0;
   if(preset_morphDrainPhase > PRESET_SYNTH_VOICES)
      preset_morphDrainPhase = 0;

   param = preset_morphScanParam;

   if(preset_morphDrainPhase)
   {
      uint8_t sourceVoice = (uint8_t)(preset_morphDrainPhase - 1);
      uint8_t targetVoice;
      ModulationNode *lfoNode;
      float travel;
      uint8_t modulationAmount;
      uint8_t liveValue;

      if(!preset_lfoMorphAssignmentForSource(sourceVoice, &targetVoice, &lfoNode))
      {
         preset_advanceMorphDrainPhase();
         return;
      }

      voiceMask = preset_voiceMaskForParameter(param);
      if(!(voiceMask & (uint8_t)(1 << targetVoice))
         || param >= END_OF_SOUND_PARAMETERS
         || preset_isAutomationTargetSelectorParam(param)
         || preset_isMorphAmountParam(param))
      {
         preset_advanceMorphDrainPhase();
         return;
      }

      image = preset_getMorphImageForVoice(targetVoice);
      kit = preset_getMorphKitForImage(image);

      travel = lfoNode->amount * lfoNode->lastVal;
      if(travel < 0.f)
         travel = 0.f;
      else if(travel > 1.f)
         travel = 1.f;

      modulationAmount = (uint8_t)((travel * 255.f) + 0.5f);
      liveValue = kit->interpolatedParams[param];
      if(modulationAmount)
      {
         liveValue = preset_interpolateMorphValue(liveValue,
                                                  kit->morphEndpointParams[param],
                                                  modulationAmount);
      }

      preset_applyLiveMorphParameterValue(image, targetVoice, param, liveValue);
      preset_advanceMorphDrainPhase();
      return;
   }

   voiceMask = preset_voiceMaskForParameter(param);

   synthVoice = preset_firstVoiceForMask(voiceMask);
   if(synthVoice >= PRESET_SYNTH_VOICES)
   {
      preset_advanceMorphDrainPhase();
      return;
   }

   image = preset_getMorphImageForVoice(synthVoice);
   kit = preset_getMorphKitForImage(image);
   isAutomationTargetSelector = preset_isAutomationTargetSelectorParam(param);

   if(param < END_OF_SOUND_PARAMETERS)
   {
      uint8_t value = isAutomationTargetSelector
                    ? kit->kitEndpointParams[param]
                    : preset_interpolateMorphValue(kit->kitEndpointParams[param],
                                                   kit->morphEndpointParams[param],
                                                   kit->voiceMorphAmount[synthVoice]);
      uint8_t liveValue = value;

      kit->interpolatedParams[param] = value;
      preset_updateInterpolatedAutomationTarget(kit, param, value);
      if(isAutomationTargetSelector || !preset_voiceHasLfoMorphOverlay(synthVoice))
         preset_applyLiveMorphParameterValue(image, synthVoice, param, liveValue);
   }

   preset_advanceMorphDrainPhase();
}

/* Updates the live-shared parameter cache. */
void preset_updateLiveSharedParameterCache(uint16_t param, uint8_t value)
{
   if(!preset_isVoiceParameter(param))
   {
      preset_liveSharedParams[param] = value;
      preset_liveSharedParamsValid[param] = 1;
   }
}

/* Applies all shared (non-voice) parameters from a kit to the DSP. */
void preset_applySharedParameterValues(const PresetKitState *kit)
{
   uint16_t param;

   if(!kit)
      return;

   for(param=1;param<END_OF_SOUND_PARAMETERS;param++)
   {
      if(preset_isVoiceParameter(param))
         continue;

      if(preset_liveSharedParamsValid[param]
         && preset_liveSharedParams[param] == kit->interpolatedParams[param])
         continue;

      preset_applySingleParameterValue(param, kit->interpolatedParams[param]);
      preset_liveSharedParams[param] = kit->interpolatedParams[param];
      preset_liveSharedParamsValid[param] = 1;
   }
}

/* Applies all voice-specific parameters from a kit for a specific voice. */
void preset_applyVoiceParameterValues(const PresetKitState *kit, uint8_t synthVoice)
{
   uint8_t i;

   if(!kit || synthVoice >= PRESET_SYNTH_VOICES)
      return;

   for(i=0;i<PRESET_VOICE_PARAM_LENGTH;i++)
   {
      uint16_t param = preset_canonicalParamFromVoiceMask(preset_voiceParamMask[synthVoice][i]);
      if(param < END_OF_SOUND_PARAMETERS)
         preset_applySingleParameterValue(param, kit->interpolatedParams[param]);
   }
}
