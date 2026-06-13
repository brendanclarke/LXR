/*
 * sequencer.h
 *
 *  Created on: 11.04.2012
 * ------------------------------------------------------------------------------------------------------------------------
 *  Copyright 2013 Julian Schmidt
 *  Julian@sonic-potions.com
 * ------------------------------------------------------------------------------------------------------------------------
 *  This file is part of the Sonic Potions LXR drumsynth firmware.
 * ------------------------------------------------------------------------------------------------------------------------
 *  Redistribution and use of the LXR code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *       - The code may not be sold, nor may it be used in a commercial product or activity.
 *
 *       - Redistributions that are modified from the original source must include the complete
 *         source code, including the source code for all components used by a binary built
 *         from the modified sources. However, as a special exception, the source code distributed
 *         need not include anything that is normally distributed (in either source or binary form)
 *         with the major components (compiler, kernel, and so on) of the operating system on which
 *         the executable runs, unless that component itself accompanies the executable.
 *
 *       - Redistributions must reproduce the above copyright notice, this list of conditions and the
 *         following disclaimer in the documentation and/or other materials provided with the distribution.
 * ------------------------------------------------------------------------------------------------------------------------
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 *   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *   USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ------------------------------------------------------------------------------------------------------------------------
 */


#ifndef SEQUENCER_H_
#define SEQUENCER_H_



#include "stm32f4xx.h"
#include "globals.h"
#include "Preset/KitState.h"
#include "Preset/ParameterArray.h"
#include "Preset/ParameterIngress.h"
#include "Preset/MorphEngine.h"
#include "Preset/TempPlaybackSwitch.h"
#include "PatternData.h"
#include "EuklidGenerator.h"

#define SEQ_NEXT_RANDOM 		0x08
#define SEQ_NEXT_RANDOM_PREV 	0x09

#define ROLL_VOLUME 100

enum Seq_RollModeEnum //0=trig, 1=nte, 2=vel, 3=bth, 4=all
{
	ROLL_MODE_TRIG,
   ROLL_MODE_NOTE,
   ROLL_MODE_VELOCITY,
   ROLL_MODE_BOTH,
   ROLL_MODE_ALL,
   ROLL_MODE_FIRST_ON,
   ROLL_MODE_FIRST_OFF,
};

enum Seq_QuantisationEnum
{
	NO_QUANTISATION,
	QUANT_8,
	QUANT_16,
	QUANT_32,
	QUANT_64,
};

extern uint8_t seq_activePattern;
extern uint8_t seq_perTrackActivePattern[7];
extern int8_t seq_stepIndex[NUM_TRACKS+1];
extern uint8_t seq_recordActive;				/**< set to 1 to activate the reording mode*/

//extern PatternSet* seq_activePatternSetPtr;

extern uint8_t seq_transpose_voiceAmount[7];
extern uint8_t seq_transposeOnOff;

extern volatile uint8_t preset_tmpKitHandshakeReady;
extern volatile uint8_t preset_tmpKitHandshakeAck;

extern uint8_t seq_selectedStep;

extern uint8_t seq_resetBarOnPatternChange;

extern uint8_t switchOnNextStep;

extern uint8_t seq_voicesLoading;
extern uint8_t seq_newVoiceAvailable;
extern uint8_t seq_tracksLocked;
extern uint8_t seq_loadFastMode;

extern uint8_t seq_rollMode;
extern uint8_t seq_rollNote;
extern uint8_t seq_rollVelocity;
extern uint8_t seq_kitResetFlag;
extern uint8_t seq_skipFirstRoll;

#define seq_vMorphAmount preset_vMorphAmount
#define seq_vMorphFlag preset_vMorphFlag
#define seq_morphLoadDisabled preset_morphLoadDisabled

void sequencer_sendVMorph(uint8_t voiceArray, uint8_t morphAmount);
//------------------------------------------------------------------------------
void seq_triggerVoice(uint8_t voiceNr, uint8_t vol, uint8_t note);
//------------------------------------------------------------------------------
void seq_setShuffle(float shuffle);
/* Preset now owns the core sound-state images and parameter routing logic.
   Keep the old seq_* names alive here as compatibility wrappers while the
   rest of the tree migrates to the new Preset public API. */
#define SEQ_SYNTH_VOICES PRESET_SYNTH_VOICES
#define SEQ_VOICE_PARAM_LENGTH PRESET_VOICE_PARAM_LENGTH
#define SEQ_MORPH_IMAGE_NORMAL PRESET_MORPH_IMAGE_NORMAL
#define SEQ_MORPH_IMAGE_TMP PRESET_MORPH_IMAGE_TMP
#define SEQ_MORPH_IMAGE_COUNT PRESET_MORPH_IMAGE_COUNT
#define SEQ_VOICE_SOURCE_NORMAL PRESET_VOICE_SOURCE_NORMAL
#define SEQ_VOICE_SOURCE_TMP PRESET_VOICE_SOURCE_TMP
#define SEQ_PARAM_INGRESS_CURRENT_IMAGE PRESET_PARAM_INGRESS_CURRENT_IMAGE
#define SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT PRESET_PARAM_INGRESS_NORMAL_KIT_ENDPOINT
#define SEQ_AUTOMATION_INGRESS_NONE PRESET_AUTOMATION_INGRESS_NONE
#define SEQ_AUTOMATION_INGRESS_FRONT_ENDPOINT PRESET_AUTOMATION_INGRESS_FRONT_ENDPOINT
#define SEQ_AUTOMATION_INGRESS_MORPH_ENDPOINT PRESET_AUTOMATION_INGRESS_MORPH_ENDPOINT

#define seq_normalKitState preset_normalKitState
#define seq_tmpKitState preset_tmpKitState
#define seq_tmpKitActive preset_tmpKitActive
#define seq_voiceSourceState preset_voiceSourceState
#define seq_voiceParamMask preset_voiceParamMask

/* Compatibility wrapper for the active kit-image selector. This preserves the
   old `seq_*` name while delegating the actual temp-vs-normal choice to
   `Preset`, which now owns the routing policy. */
static inline SeqKitState* seq_getCurrentImageKitState(void)
{
   return preset_getCurrentImageKitState();
}

/* Compatibility wrapper for the image-to-kit lookup helper. It keeps callers
   from learning the new file layout while still routing through `Preset`. */
static inline SeqKitState* seq_getMorphKitForImage(uint8_t image)
{
   return preset_getMorphKitForImage(image);
}

/* Compatibility wrapper for the per-voice image selection helper. The voice
   source state is now owned by `Preset`, but the old name remains available for
   callers that have not switched over yet. */
static inline uint8_t seq_getMorphImageForVoice(uint8_t synthVoice)
{
   return preset_getMorphImageForVoice(synthVoice);
}

/* Compatibility wrapper for reading a voice's source marker. This keeps the
   old `seq_*` entry point alive while the source-state array physically lives
   in `Preset`. */
static inline uint8_t seq_getVoiceSourceState(uint8_t synthVoice)
{
   return preset_getVoiceSourceState(synthVoice);
}

/* Compatibility wrapper for updating a voice source marker. Later phases can
   keep using the same call shape while `Preset` remains the owner of the array
   that the router consults. */
static inline void seq_setVoiceSourceState(uint8_t synthVoice, uint8_t sourceState)
{
   preset_setVoiceSourceState(synthVoice, sourceState);
}

/* Compatibility wrapper for the canonical-parameter hook. The function is
   currently an identity mapping, but keeping the wrapper makes future table
   normalization a single change in `Preset`. */
static inline uint16_t seq_canonicalParamFromVoiceMask(uint16_t param)
{
   return preset_canonicalParamFromVoiceMask(param);
}

/* Compatibility wrapper for the first-bit scan helper used by the voice-mask
   logic. */
static inline uint8_t seq_firstVoiceForMask(uint8_t voiceMask)
{
   return preset_firstVoiceForMask(voiceMask);
}

/* Compatibility wrapper for the parameter-to-voice-mask lookup. */
static inline uint8_t seq_voiceMaskForParameter(uint16_t param)
{
   return preset_voiceMaskForParameter(param);
}

/* Compatibility wrapper for the voice-parameter predicate used by the live
   cache and restore code. */
static inline uint8_t seq_isVoiceParameter(uint16_t param)
{
   return preset_isVoiceParameter(param);
}

/* Compatibility wrapper that maps selector bytes back to canonical destination
   parameters. */
static inline uint16_t seq_resolveAutomationTargetSelector(uint8_t selector)
{
   return preset_resolveAutomationTargetSelector(selector);
}

/* Compatibility wrapper for the reverse destination-to-selector lookup. */
static inline uint8_t seq_selectorForAutomationTargetDestination(uint16_t destination)
{
   return preset_selectorForAutomationTargetDestination(destination);
}

/* Compatibility wrapper that preserves the old voice-selector helper name
   while delegating the actual lookup into `Preset`. */
static inline uint8_t seq_voiceSelectorForAutomationTargetDestination(uint16_t destination,
                                                                      uint8_t fallback)
{
   return preset_voiceSelectorForAutomationTargetDestination(destination,
                                                             fallback);
}

/* Compatibility wrapper for the selector-parameter predicate. */
static inline uint8_t seq_isAutomationTargetSelectorParam(uint16_t param)
{
   return preset_isAutomationTargetSelectorParam(param);
}

/* Compatibility wrapper for the morph-amount predicate. */
static inline uint8_t seq_isMorphAmountParam(uint16_t param)
{
   return preset_isMorphAmountParam(param);
}

/* Compatibility wrapper that maps morph parameters back to voice indices. */
static inline uint8_t seq_morphVoiceForParam(uint16_t param)
{
   return preset_morphVoiceForParam(param);
}

/* Compatibility wrapper for the helper that updates interpolated automation
   targets only. The real implementation now lives in `Preset`, but the old
   call shape stays intact for callers migrating in smaller steps. */
static inline void seq_updateInterpolatedAutomationTarget(SeqKitState *kit,
                                                          uint16_t param,
                                                          uint8_t selector)
{
   preset_updateInterpolatedAutomationTarget(kit, param, selector);
}

/* Compatibility wrapper for the helper that keeps the front-panel and
   interpolated automation targets coherent together. */
static inline void seq_updateFrontAndInterpolatedAutomationTargets(SeqKitState *kit,
                                                                   uint16_t param,
                                                                   uint8_t selector)
{
   preset_updateFrontAndInterpolatedAutomationTargets(kit, param, selector);
}

static inline void seq_updateLiveSharedParameterCache(uint16_t param, uint8_t value)
{
   preset_updateLiveSharedParameterCache(param, value);
}

/* Compatibility wrapper for ingress target selection. The actual mode state is
   owned by `Preset`, but old callers can keep using the `seq_*` name while the
   refactor lands. */
static inline void seq_setIngressTarget(uint8_t target)
{
   preset_setIngressTarget(target);
}

/* Compatibility wrapper that exposes the current ingress target flag through
   the legacy Sequencer API surface. */
static inline uint8_t seq_getIngressTarget(void)
{
   return preset_getIngressTarget();
}

/* Compatibility wrapper for the live-vs-restore ingress predicate. */
static inline uint8_t seq_shouldApplyIngressToLive(void)
{
   return preset_shouldApplyIngressToLive();
}

/* Compatibility wrapper for the automation-sideband ingress mode. */
static inline void seq_setAutomationIngressTarget(uint8_t target)
{
   preset_setAutomationIngressTarget(target);
}

/* Compatibility wrapper that forwards raw endpoint bytes into `Preset`. */
static inline void seq_storeParameterIngress(uint16_t param, uint8_t value)
{
   preset_storeParameterIngress(param, value);
}

/* Compatibility wrapper that forwards morph-endpoint bytes into `Preset`. */
static inline void seq_storeMorphParameterIngress(uint16_t param, uint8_t value)
{
   preset_storeMorphParameterIngress(param, value);
}

/* Compatibility wrapper for LFO destination ingress. */
static inline void seq_storeLfoDestinationIngress(uint8_t voice, uint16_t destination)
{
   preset_storeLfoDestinationIngress(voice, destination);
}

/* Compatibility wrapper for velocity destination ingress. */
static inline void seq_storeVelocityDestinationIngress(uint8_t voice, uint16_t destination)
{
   preset_storeVelocityDestinationIngress(voice, destination);
}

/* Compatibility wrapper for macro destination ingress. */
static inline void seq_storeMacroDestinationIngress(uint8_t destinationNr, uint16_t destination)
{
   preset_storeMacroDestinationIngress(destinationNr, destination);
}

//------------------------------------------------------------------------------
void seq_init();
//------------------------------------------------------------------------------
/** call periodically to check if the next step has to be processed */

void seq_tick();

static inline void seq_serviceMorphInterpolation(void)
{
   preset_serviceMorphInterpolation();
}

void seq_serviceEndpointRestore();
uint8_t seq_endpointRestoreBusy();

static inline void seq_setGlobalMorphAmount(uint8_t morphAmount)
{
   preset_setGlobalMorphAmount(morphAmount);
}

static inline void seq_resetVoiceMorphAmountsToGlobal(void)
{
   preset_resetVoiceMorphAmountsToGlobal();
}

static inline void seq_resetLiveMorphApplyCache(void)
{
   preset_resetLiveMorphApplyCache();
}

static inline void seq_setGlobalMorphAutomationValue(uint8_t morphValue)
{
   preset_setGlobalMorphAutomationValue(morphValue);
}

static inline void seq_setVoiceMorphAmount(uint8_t synthVoice, uint8_t morphAmount)
{
   preset_setVoiceMorphAmount(synthVoice, morphAmount);
}

static inline void seq_setVoiceMorphAutomationValue(uint8_t synthVoice, uint8_t morphValue)
{
   preset_setVoiceMorphAutomationValue(synthVoice, morphValue);
}

static inline void seq_setVoiceMorphMaskAutomationValue(uint8_t voiceMask, uint8_t morphValue)
{
   preset_setVoiceMorphMaskAutomationValue(voiceMask, morphValue);
}

static inline void seq_modulateVoiceMorphAmount(uint8_t synthVoice, float amount, float value)
{
   preset_modulateVoiceMorphAmount(synthVoice, amount, value);
}

static inline void seq_applySingleParameterValue(uint16_t param, uint8_t value)
{
   preset_applySingleParameterValue(param, value);
}

void seq_applyVoiceAutomationTargets(const SeqKitAutomationTargets *source,
                                     uint8_t synthVoice);

void seq_applyNormalEndpointAutomationTargets();
//------------------------------------------------------------------------------
void seq_armAutomationStep(uint8_t stepNr, uint8_t track,uint8_t isArmed);
//------------------------------------------------------------------------------
void seq_resetDeltaAndTick();
//------------------------------------------------------------------------------
void seq_realign();
uint8_t seq_consumeTmpBoundaryPatternSwitchAck();
//------------------------------------------------------------------------------
void seq_setDeltaT(float delta);
//------------------------------------------------------------------------------
void seq_triggerNextMasterStep(uint8_t stepSize);
//------------------------------------------------------------------------------
void seq_setBpm(uint16_t bpm);
//------------------------------------------------------------------------------
uint16_t seq_getBpm();
//------------------------------------------------------------------------------
void seq_sync();
//------------------------------------------------------------------------------
//void seq_nextStep();
//------------------------------------------------------------------------------
uint8_t seq_getExtSync();
//------------------------------------------------------------------------------
void seq_setQuantisation(uint8_t value);
//------------------------------------------------------------------------------
void seq_setExtSync(uint8_t isExt);
//------------------------------------------------------------------------------
/** switch to pattern patNr after the current pattern has finished*/
void seq_setNextPattern(const uint8_t patNr, uint8_t voice);
//------------------------------------------------------------------------------
void seq_setRunning(uint8_t isRunning);
uint8_t seq_isRunning();
/** mutes and unmutes a track [0..maxTrack]*/
void seq_setMute(uint8_t trackNr, uint8_t isMuted);
uint8_t seq_isTrackMuted(uint8_t trackNr);
/** send step data to front panel. the whole step struct for one step is transmitted*/
void seq_sendStepInfoToFront(uint16_t stepNr);
void seq_sendMainStepInfoToFront(uint16_t stepNr);
void seq_rollChange(uint8_t voice, uint8_t onOff);
uint8_t seq_setRoll(uint8_t voice, uint8_t onOff);
uint8_t seq_checkRollStep(uint8_t voice);
uint8_t seq_rollTrig(uint8_t voice);
void seq_setRollRate(uint8_t rate);
void seq_setRollNote(uint8_t note);
void seq_setRollVelocity(uint8_t velocity);
/** add a note to the current pattern position*/
void seq_addNote(uint8_t trackNr,uint8_t vel, uint8_t note);
void seq_setRecordingMode(uint8_t active);
void seq_setErasingMode(uint8_t active);
//selects the automation track (0:1) that is recorded to
void seq_setActiveAutomationTrack(uint8_t trackNr);
void seq_recordAutomation(uint8_t voice, uint8_t dest, uint8_t value);
void seq_writeTranspose();
int8_t seq_quantize(int8_t step, uint8_t track);
// send a note off for a channel if there is a note playing on that channel
// if 0xff is specified, send a note off on all channels that have a note playing
void seq_midiNoteOff(uint8_t chan);
void seq_sendMidiNoteOn(const uint8_t channel, const uint8_t note, const uint8_t veloc);

#endif /* SEQUENCER_H_ */
