DATE: 2026-06-10
SESSION GOAL: Complete Phase 2 of the architectural refactor (Move Morph and Live-Apply logic).
COMPLETED: Morph engine, interpolation worker, phased LFO-to-morph drain, and live-apply suppression cache successfully relocated to the new Preset/MorphEngine module. Established authoritative DSP bridge in ParameterIngress.c. Verified build.

### Detailed Refactor Results (Phase 2)

#### Functions Relocated to `Preset/` Module:
- **Morph Engine (`MorphEngine.c`)**:
  - `seq_syncVMorphAmountMirrorsFromLiveSources()` -> `preset_syncVMorphAmountMirrorsFromLiveSources()`
  - `seq_selectVoiceMorphAmountFromKit()` -> `preset_selectVoiceMorphAmountFromKit()`
  - `seq_setVoiceMorphLiveAmount()` -> `preset_setVoiceMorphLiveAmount()`
  - `seq_morphImageVoiceIsLive()` -> `preset_morphImageVoiceIsLive()`
  - `seq_invalidateLiveMorphApplyCache()` -> `preset_invalidateLiveMorphApplyCache()`
  - `seq_invalidateAllLiveMorphApplyCaches()` -> `preset_invalidateAllLiveMorphApplyCaches()`
  - `seq_resetLiveMorphApplyCache()` -> `preset_resetLiveMorphApplyCache()`
  - `seq_liveMorphApplyNeeded()` -> `preset_liveMorphApplyNeeded()`
  - `seq_interpolateMorphValue()` -> `preset_interpolateMorphValue()`
  - `seq_applyLiveMorphParameterValue()` -> `preset_applyLiveMorphParameterValue()`
  - `seq_morphAutomationValueToAmount()` -> `preset_morphAutomationValueToAmount()`
  - `seq_advanceMorphScanCursor()` -> `preset_advanceMorphScanCursor()`
  - `seq_lfoMorphAssignmentForSource()` -> `preset_lfoMorphAssignmentForSource()`
  - `seq_voiceHasLfoMorphOverlay()` -> `preset_voiceHasLfoMorphOverlay()`
  - `seq_advanceMorphDrainPhase()` -> `preset_advanceMorphDrainPhase()`
  - `seq_setGlobalMorphAmount()` -> `preset_setGlobalMorphAmount()`
  - `seq_setGlobalMorphAutomationValue()` -> `preset_setGlobalMorphAutomationValue()`
  - `seq_resetVoiceMorphAmountsToGlobal()` -> `preset_resetVoiceMorphAmountsToGlobal()`
  - `seq_setVoiceMorphAmount()` -> `preset_setVoiceMorphAmount()`
  - `seq_setVoiceMorphAutomationValue()` -> `preset_setVoiceMorphAutomationValue()`
  - `seq_setVoiceMorphMaskAutomationValue()` -> `preset_setVoiceMorphMaskAutomationValue()`
  - `seq_applyVelocityVoiceMorphOnTrigger()` -> `preset_applyVelocityVoiceMorphOnTrigger()`
  - `seq_modulateVoiceMorphAmount()` -> `preset_modulateVoiceMorphAmount()`
  - `seq_serviceMorphInterpolation()` -> `preset_serviceMorphInterpolation()`
  - `seq_updateLiveSharedParameterCache()` -> `preset_updateLiveSharedParameterCache()`
  - `seq_applySharedParameterValues()` -> `preset_applySharedParameterValues()`
  - `seq_applyVoiceParameterValues()` -> `preset_applyVoiceParameterValues()`
- **Authoritative DSP Bridge (`ParameterIngress.c`)**:
  - `seq_applySingleParameterValue()` -> `preset_applySingleParameterValue()`

#### Variables Relocated to `Preset/` Module:
- `seq_vMorphFlag` -> `preset_vMorphFlag`
- `seq_vMorphAmount[]` -> `preset_vMorphAmount[]`
- `seq_morphLoadDisabled` -> `preset_morphLoadDisabled`
- `seq_morphScanParam` -> `preset_morphScanParam` (Now private to `MorphEngine.c`)
- `seq_morphDrainPhase` -> `preset_morphDrainPhase` (Now private to `MorphEngine.c`)
- `seq_liveMorphAppliedValue[][]` -> `preset_liveMorphAppliedValue[][]` (Now private to `MorphEngine.c`)
- `seq_liveMorphAppliedKnown[][]` -> `preset_liveMorphAppliedKnown[][]` (Now private to `MorphEngine.c`)
- `seq_liveSharedParams[]` -> `preset_liveSharedParams[]` (Now private to `MorphEngine.c`)
- `seq_liveSharedParamsValid[]` -> `preset_liveSharedParamsValid[]` (Now private to `MorphEngine.c`)

VERIFIED ON HARDWARE: Yes, standard morph, per-voice morph, LFO modulation to morph, velocity modulation to morph, and temp kit interactions tested successfully.

CHANGES THIS SESSION:
- mainboard/LxrStm32/src/Preset/MorphEngine.h: Created new header for morph engine API.
- mainboard/LxrStm32/src/Preset/MorphEngine.c: Created new implementation for morph interpolation and live-apply logic.
- mainboard/LxrStm32/src/Sequencer/sequencer.h: Added compatibility wrappers and defines for relocated morph symbols.
- mainboard/LxrStm32/src/Sequencer/sequencer.c: Removed morph variables and function definitions; updated to call preset APIs.
- mainboard/LxrStm32/src/Preset/ParameterIngress.h: Added declaration for preset_applySingleParameterValue().
- mainboard/LxrStm32/src/Preset/ParameterIngress.c: Relocated seq_applySingleParameterValue() as preset_applySingleParameterValue() and updated calls.
- mainboard/LxrStm32/src/main.c: Updated main loop to call preset_serviceMorphInterpolation().
- mainboard/LxrStm32/Makefile: Updated to include MorphEngine.c in the build.
- REFACTOR_PHASED_PLAN.md: Updated with Phase 2 result and status.

KNOWN ISSUES INTRODUCED: None.
KNOWN ISSUES RESOLVED: Monolithic morph engine decoupled from sequencer.c.

NEXT SESSION RECOMMENDED GOAL: Phase 3 of the refactor (Move Endpoint Restore and Temp Switching).
BLOCKERS: None.

CRITICAL REMINDERS FOR NEXT SESSION:
- Phase 3 will involve splitting frontParser_applyDeferredVoiceCache() into a single background-load finalizer and relocating endpoint restore logic.
- Maintain compatibility wrappers in sequencer.h until all call sites are migrated in Phase 6.
