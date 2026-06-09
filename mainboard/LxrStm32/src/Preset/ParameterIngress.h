/*
 * ParameterIngress.h
 *
 * Preset keeps the raw endpoint ingress state here so sequencer and the front
 * panel parser can route live edits and restore traffic through the same
 * policy boundary.
 */

#ifndef PRESET_PARAMETERINGRESS_H_
#define PRESET_PARAMETERINGRESS_H_

#include "stm32f4xx.h"
#include "Preset/KitState.h"
#include "Preset/ParameterMap.h"

/* These routing flags tell Preset whether incoming raw bytes should land in
   the current image or the normal-kit endpoint, and whether automation sideband
   bytes should be treated as front-end data or morph-endpoint data while a
   restore is in progress. */
enum
{
   PRESET_PARAM_INGRESS_CURRENT_IMAGE = 0,
   PRESET_PARAM_INGRESS_NORMAL_KIT_ENDPOINT = 1,
   PRESET_AUTOMATION_INGRESS_NONE = 0,
   PRESET_AUTOMATION_INGRESS_FRONT_ENDPOINT = 1,
   PRESET_AUTOMATION_INGRESS_MORPH_ENDPOINT = 2
};

/* Switches the ingress router between live current-image writes and normal-kit
   endpoint writes. The helper updates the module-level mode flags in
   ParameterIngress.c so callers do not need to manipulate those statics
   directly. */
void preset_setIngressTarget(uint8_t target);
/* Returns the current ingress target flag. Sequencer wrappers use this to
   preserve legacy behavior while the new Preset module owns the actual mode
   state. */
uint8_t preset_getIngressTarget(void);
/* Reports whether the current ingress mode should update live state as well as
   endpoint storage. This remains a tiny boolean convenience for old call sites
   that still need to distinguish current-image edits from restore traffic. */
uint8_t preset_shouldApplyIngressToLive(void);
/* Selects the automation-sideband submode used while normal-kit restore traffic
   is in flight. The target value determines whether selector bytes are routed
   to front-panel targets or morph-endpoint targets. */
void preset_setAutomationIngressTarget(uint8_t target);

/* Updates the interpolated automation target image only. This helper exists so
   the restore path can rebuild the worker-owned target cache without touching
   the front-panel copy. */
void preset_updateInterpolatedAutomationTarget(SeqKitState *kit,
                                               uint16_t param,
                                               uint8_t selector);
/* Updates both the front-panel and interpolated automation target images so
   live edits and restore writes stay in sync when the same selector byte must
   be reflected in two places. */
void preset_updateFrontAndInterpolatedAutomationTargets(SeqKitState *kit,
                                                        uint16_t param,
                                                        uint8_t selector);

/* Stores a raw endpoint byte into the correct kit image for the current ingress
   mode. The helper keeps `preset_paramIngressTarget`, `preset_voiceSourceState`
   and the transitional live-apply cache aligned with one another. */
void preset_storeParameterIngress(uint16_t param, uint8_t value);
/* Stores a raw morph-endpoint byte into the correct kit image without
   triggering interpolation. The caller uses this for morph endpoints that must
   stay as raw bytes until the morph worker consumes them. */
void preset_storeMorphParameterIngress(uint16_t param, uint8_t value);
/* Stores an LFO destination selector and its resolved destination value while
   preserving the front-panel and interpolated copies that the protocol and
   morph layers depend on. */
void preset_storeLfoDestinationIngress(uint8_t voice, uint16_t destination);
/* Stores a velocity destination selector and mirrors it into the interpolated
   automation state when the current ingress mode requires both copies. */
void preset_storeVelocityDestinationIngress(uint8_t voice, uint16_t destination);
/* Stores a macro destination selector and keeps the active automation target
   image coherent with the current ingress mode. */
void preset_storeMacroDestinationIngress(uint8_t destinationNr, uint16_t destination);

#endif /* PRESET_PARAMETERINGRESS_H_ */
