/*
 * MorphEngine.h
 *
 * Phase 2 of the preset refactor moves the morph engine, live-apply cache,
 * and voice morph control out of sequencer.c so Preset becomes the canonical
 * owner of sound interpolation and live apply suppression.
 */

#ifndef PRESET_MORPHENGINE_H_
#define PRESET_MORPHENGINE_H_

#include "stm32f4xx.h"
#include "Preset/KitState.h"
#include "Preset/ParameterArray.h"

/* Trigger flag used to signal when a morph amount has changed and the
   interpolation worker should refresh the live DSP state. */
extern uint8_t preset_vMorphFlag;

/* Legacy mirror array for morph amounts. Values [1..6] are per-voice, [0] is
   global. This is maintained for compatibility with the AVR menu display. */
extern uint8_t preset_vMorphAmount[7];

/* Flag to disable morph interpolation during certain loading operations. */
extern uint8_t preset_morphLoadDisabled;

/* Synchronizes the legacy `preset_vMorphAmount[]` mirrors from the live
   voice sources. This is called when kit images or voice sources change. */
void preset_syncVMorphAmountMirrorsFromLiveSources(void);

/* Updates the morph mirror for a specific voice from its active kit. */
void preset_selectVoiceMorphAmountFromKit(uint8_t synthVoice, const PresetKitState *kit);

/* Sets the live morph amount for a voice, updating both the kit state and the
   mirror array. */
void preset_setVoiceMorphLiveAmount(uint8_t synthVoice, uint8_t morphAmount);

/* Invalidates the live-apply cache for a specific morph image, forcing the
   next interpolation pass to apply all parameters to the DSP. */
void preset_invalidateLiveMorphApplyCache(uint8_t image);

/* Invalidates the live-apply caches for all morph images. */
void preset_invalidateAllLiveMorphApplyCaches(void);

/* Resets the live-apply cache (alias for `preset_invalidateAllLiveMorphApplyCaches`). */
void preset_resetLiveMorphApplyCache(void);

/* Checks if a parameter needs to be applied to the DSP for a specific image.
   This suppresses redundant updates if the value hasn't changed. */
uint8_t preset_liveMorphApplyNeeded(uint8_t image, uint16_t param, uint8_t value);

/* Interpolates between two byte values (a to b) based on the morph amount x (0-255). */
uint8_t preset_interpolateMorphValue(uint8_t a, uint8_t b, uint8_t x);

/* Applies a single morphed parameter value to the DSP, respecting the
   live-apply cache. */
void preset_applyLiveMorphParameterValue(uint8_t image, uint8_t synthVoice, uint16_t param, uint8_t value);

/* Applies the resolved automation targets for one voice to the live DSP.
   Temp playback, restore replay, and morph-driven target changes all use this
   helper once the target selector bytes have already been decoded. */
void preset_applyVoiceAutomationTargets(const PresetAutomationTargets *source,
                                        uint8_t synthVoice);

/* Reapplies the normal-image voice and shared automation targets after a temp
   boundary or endpoint restore closes. This keeps the live DSP in sync with
   the current normal kit image. */
void preset_applyNormalEndpointAutomationTargets(void);

/* Converts a 7-bit automation value (0-127) to an 8-bit morph amount (0-255). */
uint8_t preset_morphAutomationValueToAmount(uint8_t morphValue);

/* Sets the global morph amount for the current image, resetting all per-voice
   morph bases. */
void preset_setGlobalMorphAmount(uint8_t morphAmount);

/* Sets the global morph amount from an automation value. */
void preset_setGlobalMorphAutomationValue(uint8_t morphValue);

/* Resets all per-voice morph amounts to the global morph amount. */
void preset_resetVoiceMorphAmountsToGlobal(void);

/* Sets the morph amount for a specific voice. */
void preset_setVoiceMorphAmount(uint8_t synthVoice, uint8_t morphAmount);

/* Reads the current morph amount for a specific voice. */
uint8_t preset_getVoiceMorphAmount(uint8_t synthVoice);

/* Sets the morph amount for a specific voice from an automation value. */
void preset_setVoiceMorphAutomationValue(uint8_t synthVoice, uint8_t morphValue);

/* Sets the morph amount for a mask of voices from an automation value. */
void preset_setVoiceMorphMaskAutomationValue(uint8_t voiceMask, uint8_t morphValue);

/* Applies velocity-to-voice-morph as a trigger-time current morph write. */
void preset_applyVelocityVoiceMorphOnTrigger(uint8_t voiceNr, uint8_t velocity);

/* Applies morph modulation to a voice without writing to the stored
   interpolation baseline. */
void preset_modulateVoiceMorphAmount(uint8_t synthVoice, float amount, float value);

/* Background morph worker: exactly one interpolation unit (either phase 0
   baseline or one LFO overlay) is performed per call. */
void preset_serviceMorphInterpolation(void);

/* Updates the live-shared parameter cache for non-voice parameters. */
void preset_updateLiveSharedParameterCache(uint16_t param, uint8_t value);

/* Applies all shared (non-voice) parameters from a kit to the DSP,
   respecting the live-shared parameter cache. */
void preset_applySharedParameterValues(const PresetKitState *kit);

/* Applies all voice-specific parameters from a kit for a specific voice
   to the DSP. */
void preset_applyVoiceParameterValues(const PresetKitState *kit, uint8_t synthVoice);

#endif /* PRESET_MORPHENGINE_H_ */
