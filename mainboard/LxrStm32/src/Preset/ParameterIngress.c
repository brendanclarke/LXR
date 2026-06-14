/*
 * ParameterIngress.c
 *
 * Preset owns the raw ingress side of the boundary here. External parsers and
 * front-panel handlers write into Preset through:
 *   - preset_storeParameterIngress()
 *   - preset_storeMorphParameterIngress()
 *   - preset_storeLfoDestinationIngress()
 *   - preset_storeVelocityDestinationIngress()
 *   - preset_storeMacroDestinationIngress()
 *
 * This file also keeps the ingress-side automation target mirrors coherent via
 * preset_updateInterpolatedAutomationTarget() and
 * preset_updateFrontAndInterpolatedAutomationTargets(), and it owns the final
 * DSP-facing parameter emit helper preset_applySingleParameterValue().
 */

#include "Preset/ParameterIngress.h"
#include "Sequencer/sequencer.h"
#include "uARTFrontSYX/frontPanelReceivingProtocol.h"

/* Current-image ingress starts in live-edit mode so the router lands on the
   active kit by default. This flag is intentionally private to the module so
   callers are forced through the mode-setting helpers below. */
static uint8_t preset_paramIngressTarget = PRESET_PARAM_INGRESS_CURRENT_IMAGE;
/* Automation ingress uses a second mode flag so the normal-kit restore path
   can distinguish front-panel selector bytes from morph-endpoint selector
   bytes without inventing a separate code path in the parsers. */
static uint8_t preset_automationIngressTarget = PRESET_AUTOMATION_INGRESS_NONE;

/* Chooses the automation target block that should receive the current write.
   The helper reads `preset_paramIngressTarget` and
   `preset_automationIngressTarget`, then returns either the front-panel target,
   the morph-endpoint target, or zero when the current mode should not store
   automation data at all. */
static PresetAutomationTargets* preset_getIngressAutomationTarget(void)
{
   PresetKitState *kit = preset_getCurrentImageKitState();

   if(preset_paramIngressTarget == PRESET_PARAM_INGRESS_NORMAL_KIT_ENDPOINT)
   {
      kit = &preset_normalKitState;

      /* During copy-to-temp, raw endpoint params are bracketed by endpoint
         restore phases. Resolved automation target messages need this extra
         selector so front endpoint assignments and morph endpoint assignments
         do not collapse into one storage image. */
      if(preset_automationIngressTarget == PRESET_AUTOMATION_INGRESS_FRONT_ENDPOINT)
         return &kit->frontPanelAutomationTargets;
      if(preset_automationIngressTarget == PRESET_AUTOMATION_INGRESS_MORPH_ENDPOINT)
         return &kit->morphParameterEndpointAutomationTargets;

      return 0;
   }

   return &kit->frontPanelAutomationTargets;
}

/* Chooses the interpolated automation target block that should mirror the
   current write. This is separate from the front-panel target helper because
   the normal-kit restore path can intentionally suppress interpolated writes
   while still keeping the live front-panel copy coherent. */
static PresetAutomationTargets* preset_getIngressInterpolatedAutomationTarget(void)
{
   if(preset_paramIngressTarget == PRESET_PARAM_INGRESS_NORMAL_KIT_ENDPOINT)
   {
      if(preset_automationIngressTarget == PRESET_AUTOMATION_INGRESS_FRONT_ENDPOINT)
         return &preset_normalKitState.interpolatedAutomationTargets;

      return 0;
   }

   return &preset_getCurrentImageKitState()->interpolatedAutomationTargets;
}

/* Updates only the interpolated automation target image for selector-bearing
   parameters. The helper uses `kit` as the destination owner and `selector` as
   the raw compact byte to decode into the destination fields. */
void preset_updateInterpolatedAutomationTarget(PresetKitState *kit,
                                               uint16_t param,
                                               uint8_t selector)
{
   uint8_t synthVoice;

   if(!kit)
      return;

   if(param >= PAR_VEL_DEST_1 && param <= PAR_VEL_DEST_6)
   {
      synthVoice = (uint8_t)(param - PAR_VEL_DEST_1);
      kit->interpolatedAutomationTargets.velocityDestination[synthVoice] =
         preset_resolveAutomationTargetSelector(selector);
      kit->interpolatedAutomationTargets.velocityDestinationValid |=
         (uint8_t)(1 << synthVoice);
   }
   else if(param >= PAR_TARGET_LFO1 && param <= PAR_TARGET_LFO6)
   {
      synthVoice = (uint8_t)(param - PAR_TARGET_LFO1);
      kit->interpolatedAutomationTargets.lfoDestination[synthVoice] =
         preset_resolveAutomationTargetSelector(selector);
      kit->interpolatedAutomationTargets.lfoDestinationValid |=
         (uint8_t)(1 << synthVoice);
   }
}

/* Updates both the front-panel and interpolated automation target images so
   live edits and restore writes stay in sync when the same selector byte needs
   to be reflected in two storage images. The helper first refreshes the
   interpolated cache and then mirrors the resolved destination into the
   front-panel image. */
void preset_updateFrontAndInterpolatedAutomationTargets(PresetKitState *kit,
                                                        uint16_t param,
                                                        uint8_t selector)
{
   if(!kit)
      return;

   preset_updateInterpolatedAutomationTarget(kit, param, selector);

   if(param >= PAR_VEL_DEST_1 && param <= PAR_VEL_DEST_6)
   {
      uint8_t synthVoice = (uint8_t)(param - PAR_VEL_DEST_1);
      uint16_t destination = preset_resolveAutomationTargetSelector(selector);

      kit->frontPanelAutomationTargets.velocityDestination[synthVoice] =
         destination;
      kit->frontPanelAutomationTargets.velocityDestinationValid |=
         (uint8_t)(1 << synthVoice);
      kit->interpolatedAutomationTargets.velocityDestination[synthVoice] =
         destination;
      kit->interpolatedAutomationTargets.velocityDestinationValid |=
         (uint8_t)(1 << synthVoice);
   }
   else if(param >= PAR_TARGET_LFO1 && param <= PAR_TARGET_LFO6)
   {
      uint8_t synthVoice = (uint8_t)(param - PAR_TARGET_LFO1);
      uint16_t destination = preset_resolveAutomationTargetSelector(selector);

      kit->frontPanelAutomationTargets.lfoDestination[synthVoice] =
         destination;
      kit->frontPanelAutomationTargets.lfoDestinationValid |=
         (uint8_t)(1 << synthVoice);
      kit->interpolatedAutomationTargets.lfoDestination[synthVoice] =
         destination;
      kit->interpolatedAutomationTargets.lfoDestinationValid |=
         (uint8_t)(1 << synthVoice);
   }
}

/* Switches the ingress router between current-image writes and normal-kit
   endpoint writes. When the router moves back to current-image mode, the
   automation submode is cleared as well so stale restore state cannot leak
   into the next live edit. */
void preset_setIngressTarget(uint8_t target)
{
   if(target <= PRESET_PARAM_INGRESS_NORMAL_KIT_ENDPOINT)
   {
      preset_paramIngressTarget = target;
      if(target == PRESET_PARAM_INGRESS_CURRENT_IMAGE)
         preset_automationIngressTarget = PRESET_AUTOMATION_INGRESS_NONE;
   }
}

/* Returns the current ingress target flag so wrappers and parsers can preserve
   the old live-vs-restore branching behavior without peeking at private state
   variables. */
uint8_t preset_getIngressTarget(void)
{
   return (uint8_t)preset_paramIngressTarget;
}

/* Small convenience predicate for the current ingress mode. Existing call
   sites only need the yes/no answer, so this keeps them from duplicating the
   mode comparison in several files. */
uint8_t preset_shouldApplyIngressToLive(void)
{
   return (preset_paramIngressTarget == PRESET_PARAM_INGRESS_CURRENT_IMAGE);
}

/* Selects the automation submode used during normal-kit restore traffic. The
   parser uses this to distinguish front-panel destination selectors from
   morph-endpoint destination selectors while writing into the same kit image. */
void preset_setAutomationIngressTarget(uint8_t target)
{
   if(target <= PRESET_AUTOMATION_INGRESS_MORPH_ENDPOINT)
      preset_automationIngressTarget = target;
}

/* Stores a raw morph endpoint byte without touching interpolation. The helper
   uses `voiceMask`, `preset_voiceSourceState`, `preset_tmpKitActive`, and the
   chosen `kit` pointer to decide whether the value belongs in the temp image or
   the normal image, but it always writes only the raw morph endpoint array. */
void preset_storeMorphParameterIngress(uint16_t param, uint8_t value)
{
   PresetKitState *kit;
   uint8_t voiceMask;

   if(param >= END_OF_SOUND_PARAMETERS)
      return;

   /* Morph endpoint ingress stores raw endpoint bytes only. It does not
      compute or apply live morph, and it does not touch interpolatedParams[]. */
   voiceMask = preset_voiceMaskForParameter(param);

   if(preset_paramIngressTarget == PRESET_PARAM_INGRESS_CURRENT_IMAGE && voiceMask)
   {
      uint8_t synthVoice;
      uint8_t useTmp = 0;

      for(synthVoice=0;synthVoice<PRESET_SYNTH_VOICES;synthVoice++)
      {
         if((voiceMask & (uint8_t)(1 << synthVoice))
            && preset_voiceSourceState[synthVoice] == PRESET_VOICE_SOURCE_TMP)
         {
            useTmp = 1;
            break;
         }
      }

      kit = useTmp ? &preset_tmpKitState : &preset_normalKitState;
   }
   else if(preset_paramIngressTarget == PRESET_PARAM_INGRESS_CURRENT_IMAGE
           && preset_isTmpKitActive())
   {
      kit = &preset_tmpKitState;
   }
   else
   {
      kit = &preset_normalKitState;
   }

   kit->morphEndpointParams[param] = value;
}

/* Stores a raw parameter byte into the correct kit image for the current
   ingress mode. In live-edit mode the helper may route voice-owned parameters
   to the temp image based on `preset_voiceSourceState`, and it also refreshes
   the transitional live-apply cache through `preset_updateLiveSharedParameterCache`
   so old Sequencer code can keep suppressing redundant updates while ownership
   migrates. In restore mode the helper writes the normal-kit endpoint and then
   updates the front-panel plus interpolated automation targets. */
void preset_storeParameterIngress(uint16_t param, uint8_t value)
{
   PresetKitState *kit;

   if(param >= END_OF_SOUND_PARAMETERS)
      return;

   /* Current-image ingress is a live/menu edit. Normal-kit-endpoint ingress is
      file/front endpoint restore and must only overwrite normal endpoint state;
      the morph worker will rebuild interpolatedParams[] in the background. */
   if(preset_paramIngressTarget == PRESET_PARAM_INGRESS_CURRENT_IMAGE)
   {
      uint8_t voiceMask = preset_voiceMaskForParameter(param);

      kit = &preset_normalKitState;

      if(voiceMask)
      {
         uint8_t synthVoice;
         uint8_t useTmp = 0;

         for(synthVoice=0;synthVoice<PRESET_SYNTH_VOICES;synthVoice++)
         {
            if((voiceMask & (uint8_t)(1 << synthVoice))
               && preset_voiceSourceState[synthVoice] == PRESET_VOICE_SOURCE_TMP)
            {
               useTmp = 1;
               break;
            }
         }

         kit = useTmp ? &preset_tmpKitState : &preset_normalKitState;
      }
      else if(preset_isTmpKitActive())
      {
         kit = &preset_tmpKitState;
      }

      kit->kitEndpointParams[param] = value;
      preset_updateLiveSharedParameterCache(param, value);
      return;
   }

   preset_normalKitState.kitEndpointParams[param] = value;
   preset_updateFrontAndInterpolatedAutomationTargets(&preset_normalKitState,
                                                      param,
                                                      value);
}

/* Stores an LFO destination selector and the resolved destination value into
   the correct automation target image. The helper uses `target` and
   `interpolatedTarget` to decide which automation tables need to be updated,
   while `kit`, `voiceParam`, `targetParam`, `selector`, and `voiceSelector`
   keep the raw selector byte and the companion voice selector coherent across
   live edits and restore writes. */
void preset_storeLfoDestinationIngress(uint8_t voice, uint16_t destination)
{
   PresetAutomationTargets *target = preset_getIngressAutomationTarget();
   PresetAutomationTargets *interpolatedTarget =
      preset_getIngressInterpolatedAutomationTarget();
   PresetKitState *kit = 0;
   uint16_t voiceParam;
   uint16_t targetParam;
   uint8_t selector;
   uint8_t voiceSelector;

   if(!target || voice >= PRESET_SYNTH_VOICES)
      return;

   voiceParam = (uint16_t)(PAR_VOICE_LFO1 + voice);
   targetParam = (uint16_t)(PAR_TARGET_LFO1 + voice);
   selector = preset_selectorForAutomationTargetDestination(destination);

   if(preset_paramIngressTarget == PRESET_PARAM_INGRESS_CURRENT_IMAGE)
   {
      kit = (preset_voiceSourceState[voice] == PRESET_VOICE_SOURCE_TMP)
          ? &preset_tmpKitState
          : &preset_normalKitState;

      target = &kit->frontPanelAutomationTargets;
      interpolatedTarget = &kit->interpolatedAutomationTargets;
   }
   else if(preset_paramIngressTarget == PRESET_PARAM_INGRESS_NORMAL_KIT_ENDPOINT)
   {
      kit = &preset_normalKitState;
   }

   if(preset_paramIngressTarget == PRESET_PARAM_INGRESS_NORMAL_KIT_ENDPOINT
      && preset_automationIngressTarget == PRESET_AUTOMATION_INGRESS_MORPH_ENDPOINT)
   {
      voiceSelector =
         preset_voiceSelectorForAutomationTargetDestination(
            destination,
            kit ? kit->morphEndpointParams[voiceParam] : 0);

      if(kit)
      {
         kit->morphEndpointParams[voiceParam] = voiceSelector;
         kit->morphEndpointParams[targetParam] = selector;
      }
   }
   else
   {
      voiceSelector =
         preset_voiceSelectorForAutomationTargetDestination(
            destination,
            kit ? kit->kitEndpointParams[voiceParam] : 0);

      if(kit)
      {
         kit->kitEndpointParams[voiceParam] = voiceSelector;
         kit->kitEndpointParams[targetParam] = selector;

         if(interpolatedTarget)
         {
            kit->interpolatedParams[voiceParam] = voiceSelector;
            kit->interpolatedParams[targetParam] = selector;
         }
      }
   }

   target->lfoDestination[voice] = destination;
   target->lfoDestinationValid |= (uint8_t)(1 << voice);

   if(interpolatedTarget && interpolatedTarget != target)
   {
      interpolatedTarget->lfoDestination[voice] = destination;
      interpolatedTarget->lfoDestinationValid |= (uint8_t)(1 << voice);
   }
}

/* Stores a velocity destination selector into the active automation target
   image and mirrors it into the interpolated cache when the current ingress
   mode requires both copies. This helper is simpler than the LFO path because
   velocity destinations do not need the extra voice-selector bookkeeping. */
void preset_storeVelocityDestinationIngress(uint8_t voice, uint16_t destination)
{
   PresetAutomationTargets *target = preset_getIngressAutomationTarget();
   PresetAutomationTargets *interpolatedTarget =
      preset_getIngressInterpolatedAutomationTarget();

   if(!target || voice >= PRESET_SYNTH_VOICES)
      return;

   target->velocityDestination[voice] = destination;
   target->velocityDestinationValid |= (uint8_t)(1 << voice);

   if(interpolatedTarget && interpolatedTarget != target)
   {
      interpolatedTarget->velocityDestination[voice] = destination;
      interpolatedTarget->velocityDestinationValid |= (uint8_t)(1 << voice);
   }
}

/* Stores a macro destination selector into the active automation target image
   and mirrors it into the interpolated cache when the current ingress mode
   keeps a second copy alive. This keeps macro routing coherent for both live
   edits and restore traffic. */
void preset_storeMacroDestinationIngress(uint8_t destinationNr, uint16_t destination)
{
   PresetAutomationTargets *target = preset_getIngressAutomationTarget();
   PresetAutomationTargets *interpolatedTarget =
      preset_getIngressInterpolatedAutomationTarget();

   if(!target || destinationNr >= 4)
      return;

   target->macroDestination[destinationNr] = destination;
   target->macroDestinationValid |= (uint8_t)(1 << destinationNr);

   if(interpolatedTarget && interpolatedTarget != target)
   {
      interpolatedTarget->macroDestination[destinationNr] = destination;
      interpolatedTarget->macroDestinationValid |=
         (uint8_t)(1 << destinationNr);
   }
}

/* Applies a single parameter value to the DSP. */
void preset_applySingleParameterValue(uint16_t param, uint8_t value)
{
   MidiMsg msg;

   /* Diagnostic: reintroduce live DSP apply by low-risk parameter families.
      All other stored/morphed params remain blocked from the old live MIDI
      parameter API while we isolate the retrigger-like glitch. */
   if(!((param >= PAR_VOL1 && param <= PAR_VOL6)
      || (param >= PAR_OSC_WAVE_DRUM1 && param <= PAR_OSC_WAVE_SNARE)
      || param == PAR_WAVE1_CYM
      || param == PAR_WAVE1_HH
      || (param >= PAR_PAN1 && param <= PAR_PAN3)
      || (param >= PAR_PAN4 && param <= PAR_PAN6)
      || (param >= PAR_AUDIO_OUT1 && param <= PAR_AUDIO_OUT6)
      || (param >= PAR_COARSE1 && param <= PAR_FINE6)
      || (param >= PAR_MOD_WAVE_DRUM1 && param <= PAR_WAVE3_HH)
      || (param >= PAR_NOISE_FREQ1 && param <= PAR_MIX1)
      || (param >= PAR_MOD_OSC_F1_CYM && param <= PAR_MOD_OSC_GAIN2)
      || (param >= PAR_FILTER_FREQ_1 && param <= PAR_RESO_6)
      || (param >= PAR_VELOA1 && param <= PAR_VELOD6_OPEN)
      || (param >= PAR_VOL_SLOPE1 && param <= PAR_VOL_SLOPE6)
      || (param >= PAR_REPEAT4 && param <= PAR_REPEAT5)
      || (param >= PAR_MOD_EG1 && param <= PAR_MODAMNT4)
      || (param >= PAR_PITCH_SLOPE1 && param <= PAR_PITCH_SLOPE4)
      || (param >= PAR_FMAMNT1 && param <= PAR_FM_FREQ3)
      || (param >= PAR_DRIVE1 && param <= PAR_HAT_DISTORTION)
      || (param >= PAR_VOICE_DECIMATION1 && param <= PAR_VOICE_DECIMATION_ALL)
      || (param >= PAR_FREQ_LFO1 && param <= PAR_AMOUNT_LFO6)
      || (param >= PAR_FILTER_DRIVE_1 && param <= PAR_FILTER_DRIVE_6)
      || (param >= PAR_MIX_MOD_1 && param <= PAR_MIX_MOD_3)
      || (param >= PAR_VOLUME_MOD_ON_OFF1 && param <= PAR_VOLUME_MOD_ON_OFF6)
      || (param >= PAR_VELO_MOD_AMT_1 && param <= PAR_VELO_MOD_AMT_6)
      || (param >= PAR_WAVE_LFO1 && param <= PAR_WAVE_LFO6)
      || (param >= PAR_RETRIGGER_LFO1 && param <= PAR_OFFSET_LFO6)
      || (param >= PAR_FILTER_TYPE_1 && param <= PAR_FILTER_TYPE_6)
      || (param >= PAR_TRANS1_VOL && param <= PAR_TRANS6_FREQ)
      || (param >= PAR_VEL_DEST_1 && param <= PAR_VEL_DEST_6)
      || (param >= PAR_VOICE_LFO1 && param <= PAR_VOICE_LFO6)
      || (param >= PAR_TARGET_LFO1 && param <= PAR_TARGET_LFO6)
      || (param >= PAR_MAC1_DST1 && param <= PAR_MAC2_DST2_AMT)
      || (param >= PAR_MORPH_DRUM1 && param <= PAR_MORPH_HIHAT)))
   {
      return;
   }

   /* This is the only low-parameter +1 conversion point for ordinary live DSP
      application. Endpoint storage and PRF_RESTORE_* traffic use raw indices. */
   if(param < 128)
   {
      msg.status = MIDI_CC;
      msg.data1 = (uint8_t)((param + 1) & 0x7f);
   }
   else
   {
      msg.status = FRONT_CC_2;
      msg.data1 = (uint8_t)(param - 128);
   }

   msg.data2 = value;
   frontParser_applyParameterCommand(msg, 0);
}
