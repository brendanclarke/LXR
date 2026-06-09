/*
 * ParameterMap.h
 *
 * Preset keeps the pure parameter-classification helpers here so the ingress
 * layer and sequencer can share a single view of voice masks, automation
 * selectors, and destination decoding.
 */

#ifndef PRESET_PARAMETERMAP_H_
#define PRESET_PARAMETERMAP_H_

#include "stm32f4xx.h"
#include "Preset/KitState.h"

/* The voice parameter table is fixed-width so the lookup code can scan the
   canonical voice-owned parameters without depending on the raw enum layout in
   other modules. The length intentionally mirrors the table in ParameterMap.c
   so future moves can extend the list in one place. */
enum
{
   PRESET_VOICE_PARAM_LENGTH = 37
};

/* Each voice has a canonical list of parameters that belong to it. The table
   is shared by the voice-mask classifier and the predicate helpers so they can
   answer ownership questions with one source of truth. */
extern const uint16_t preset_voiceParamMask[PRESET_SYNTH_VOICES][PRESET_VOICE_PARAM_LENGTH];

/* Returns the canonical parameter for a voice-mask entry. The current Phase 1
   implementation is an identity mapping, but the helper keeps a single place
   for any future normalization that needs to happen before classifier lookups. */
uint16_t preset_canonicalParamFromVoiceMask(uint16_t param);
/* Scans a voice-mask bitset and returns the first enabled voice index, or 0xff
   when no voice owns the mask. Callers use it to collapse ownership into a
   single synth-voice selector. */
uint8_t preset_firstVoiceForMask(uint8_t voiceMask);
/* Builds the bitmask of voices that own a given parameter by scanning the
   canonical voice parameter table. This is the key helper that lets ingress
   code decide whether a parameter write should target the temp image or the
   normal image. */
uint8_t preset_voiceMaskForParameter(uint16_t param);
/* Fast predicate wrapper around the voice parameter table. It exists so the
   live-apply cache and parser code can ask ownership questions without having
   to duplicate table scans. */
uint8_t preset_isVoiceParameter(uint16_t param);
/* Resolves a compact selector byte back into the destination parameter used by
   the front-panel and automation routing tables. Invalid selectors map to
   `PAR_NONE` so the caller can detect an out-of-range selector cleanly. */
uint16_t preset_resolveAutomationTargetSelector(uint8_t selector);
/* Performs the reverse lookup from destination parameter to selector byte. The
   parser uses this when it needs to preserve the compact front-panel encoding
   while updating a stored destination. */
uint8_t preset_selectorForAutomationTargetDestination(uint16_t destination);
/* Chooses the voice selector value that should accompany a destination write.
   The helper derives the selector from the destination map first and falls
   back to the supplied voice index when the destination is not known. */
uint8_t preset_voiceSelectorForAutomationTargetDestination(uint16_t destination,
                                                           uint8_t fallback);
/* Identifies selector-bearing parameters so the ingress layer knows when a raw
   write should also refresh the automation target tables. */
uint8_t preset_isAutomationTargetSelectorParam(uint16_t param);
/* Identifies morph-amount parameters that belong to the global morph range so
   the morph engine can route them as a family rather than a series of special
   cases. */
uint8_t preset_isMorphAmountParam(uint16_t param);
/* Maps a morph-amount parameter back to its synth-voice index. The helper
   returns 0xff for non-morph parameters so callers can reject out-of-range
   requests without guessing. */
uint8_t preset_morphVoiceForParam(uint16_t param);

#endif /* PRESET_PARAMETERMAP_H_ */
