# AUDIT_PRESET-MORPH_REFACTOR

Date: 2026-06-07
Status: institutional knowledge for future STM-side `/Preset/` refactor
Source sessions: 002, 003, 004

## Purpose

This audit is the canonical place for STM-side preset, parameter image, automation
target, temp playback, and morph-engine knowledge gathered before the planned
Session 006/007 refactor.

The user intends to delete the following Session 004 working audits after this
consolidation:

- `MORPH_AUTOMATION_ASSIGN_DROPPED_BUG.md`
- `TEMP_SWITCH_WALK.md`
- `AUDIT_TEMP_AUDIO_DROPOUT.md`

Anything needed for future preset/morph work should live here instead.

## Core Architecture After Session 004

### Ownership Model

STM is now the canonical owner of sound-state authority:

- normal kit/front endpoint parameter image;
- normal morph endpoint parameter image;
- normal interpolated/current-play baseline;
- normal resolved automation target images;
- temp kit/front endpoint parameter image;
- temp morph endpoint parameter image;
- temp interpolated/current-play baseline;
- temp resolved automation target images;
- normal/temp global morph amount;
- normal/temp per-voice base morph amount;
- normal/temp live per-voice morph amount.

AVR/front panel owns:

- UI;
- SD card file I/O;
- menu arrays;
- user requests;
- endpoint push-up receiver state for menu display.

AVR does **not** own temp playback storage after the STM morph move. AVR
`parameter_values_temp[]` and `parameters2_temp[]` are file-read/menu scratch
buffers, not temp playback state.

### `SeqKitState`

Current type:

- `SeqKitState` in `mainboard/LxrStm32/src/Sequencer/sequencer.h`

Current fields:

- `frontPanelParams[END_OF_SOUND_PARAMETERS]`
  - raw kit/front endpoint bytes.
  - Misleading name: this is not "front panel owned" state.
  - Future name: `kitEndpointParams[]`.
- `morphParams[END_OF_SOUND_PARAMETERS]`
  - raw morph endpoint bytes.
  - Future name: `morphEndpointParams[]`.
- `interpolatedParams[END_OF_SOUND_PARAMETERS]`
  - worker-owned current-play/interpolation baseline.
  - Only the morph worker should normally write this, except explicit STM-side
    image clone/copy and sideband coherence for target selector params.
- `frontPanelAutomationTargets`
  - resolved kit/front endpoint automation destinations.
  - Future name: `kitEndpointAutomationTargets`.
- `morphParameterEndpointAutomationTargets`
  - resolved morph endpoint automation destinations.
  - Future name: `morphEndpointAutomationTargets`.
- `interpolatedAutomationTargets`
  - resolved current-play/interpolated automation destinations.
- `globalMorphAmount`
  - stored global morph amount for this normal/temp kit image.
- `voiceMorphBaseAmount[SEQ_SYNTH_VOICES]`
  - stored base per-voice morph amounts for this image.
- `voiceMorphAmount[SEQ_SYNTH_VOICES]`
  - live/current per-voice morph amounts for this image.
- `valid`
  - temp image validity.

Important invariant:

- Do not reintroduce per-parameter valid arrays.
- Zero is a valid endpoint value.
- Transfer/session errors belong in transfer/session state, not in the sound
  parameter model.

### Normal And Temp Images

Current global STM images:

- `seq_normalKitState`
- `seq_tmpKitState`
- `seq_patternSet`
- `seq_tmpPattern`

Normal is the file-load destination:

- Background `.ALL` / `.PRF` loads refresh normal pattern/parameter storage.
- Playback can continue from temp.
- The user can switch back to refreshed normal on demand.

Temp is the alternate playback image:

- Copy-to-temp clones current normal pattern and parameter images into STM temp
  storage.
- Temp switching changes which image is read/applied.
- Temp switching should not copy storage.

## Temp Playback And Switching

### AVR Role

AVR should do only the following for temp playback:

1. User initiates copy-to-temp.
   - AVR sends the copy request.
2. User selects SEQ16/temp or returns to normal.
   - AVR sends the pattern switch request.
3. AVR receives endpoint bytes back from STM.
   - Menu display follows the selected image.

AVR should not stage or own STM temp playback params.

### Copy-To-Temp

Desired operation:

- Copy one source normal pattern into `seq_tmpPattern`.
- Copy `seq_normalKitState.frontPanelParams[]` into
  `seq_tmpKitState.frontPanelParams[]`.
- Copy `seq_normalKitState.morphParams[]` into `seq_tmpKitState.morphParams[]`.
- Copy `seq_normalKitState.interpolatedParams[]` into
  `seq_tmpKitState.interpolatedParams[]`.
- Copy all three automation target images:
  - `frontPanelAutomationTargets`;
  - `morphParameterEndpointAutomationTargets`;
  - `interpolatedAutomationTargets`.
- Copy normal global morph amount.
- Copy normal per-voice base/live morph amounts.
- Mark temp kit valid only after the full copy is complete.

Do not:

- dump AVR endpoints into STM just to copy to temp;
- clear normal endpoint arrays while normal is live;
- route file loads into temp storage;
- write file/AVR endpoint loads directly into interpolation arrays.

### Normal/Temp Switch Request Path

STM receives one of:

- normal pattern request:
  - status: `FRONT_SEQ_CC`
  - command: `FRONT_SEQ_CHANGE_PAT`
  - data2 low bits: normal pattern `0..7`
  - data2 upper bits: whole-pattern vs per-track target.
- temp pattern request:
  - status: `FRONT_SEQ_CC`
  - command: `FRONT_SEQ_CHANGE_TMP_PAT`
  - pattern maps to `SEQ_TMP_PATTERN`.

Both paths call `seq_setNextPattern(...)`.

Immediate state set:

- `seq_pendingPattern`
- `seq_perTrackPendingPattern[]`
- `seq_loadPendigFlag = 1`
- `seq_loadSeqNow = 1`

Spelling refactor target:

- Rename `seq_loadPendigFlag` to `seq_loadPendingFlag`.

### `seq_tick()` Switch Commit Shape

The switch block runs when:

- timing gate:
  - bar/start boundary, or
  - `switchOnNextStep && seq_loadSeqNow`;
- work gate:
  - active/pending pattern mismatch, or
  - `seq_loadPendigFlag`.

Switch commit order:

1. Snapshot old track/global pattern state.
2. Commit global active pattern.
3. Call `seq_setTmpKitActive(seq_activePattern == SEQ_TMP_PATTERN)`.
4. Commit per-track active pattern state.
5. Call `seq_updateVoiceSourcesForPatternChange(...)`.
6. Queue endpoint push-up where required.

Important ordering fix:

- `seq_markVoiceSourceTarget(...)` must publish `seq_voiceSourceState[]` before
  applying voice params or automation targets.
- Automation callbacks can query the current morph image through
  `seq_getMorphImageForVoice()`, which reads `seq_voiceSourceState[]`.

### Hihat Special Case

Closed/open hihat tracks are different:

- pattern tracks 5 and 6 share one synth voice/parameter set;
- when either hihat track crosses the normal/temp boundary, both hihat patterns
  and hihat voice parameters should switch together.

## Parameter Ingress

### Ingress Modes

Current STM ingress state:

- `SEQ_PARAM_INGRESS_CURRENT_IMAGE`
  - live/menu/current-image edits.
  - Chooses normal or temp based on active image/source state.
- `SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT`
  - file/endpoint restore into normal storage.
  - Does not apply live temp sound.

Automation ingress phase:

- `SEQ_AUTOMATION_INGRESS_NONE`
- `SEQ_AUTOMATION_INGRESS_FRONT_ENDPOINT`
- `SEQ_AUTOMATION_INGRESS_MORPH_ENDPOINT`

Reason for automation phase:

- Resolved target sidebands such as `CC_LFO_TARGET` and `CC_VELO_TARGET` need to
  land in the matching endpoint image.
- Without phase, front endpoint and morph endpoint automation destinations can
  collapse into one storage image.

### Raw Endpoint Bytes

Raw endpoint bytes arrive through:

- `PRF_RESTORE_PARAM_CC`
- `PRF_RESTORE_PARAM_CC2`
- ordinary `MIDI_CC`
- `FRONT_CC_2`

These call `seq_storeParameterIngress(param, value)`.

Rules:

- Raw endpoint storage uses raw AVR/menu parameter indices.
- The low-parameter `+1` conversion only applies when generating an ordinary
  live MIDI CC for DSP/MIDI parser apply.
- `PRF_RESTORE_PARAM_*` and `PRF_RESTORE_MORPH_*` are raw parameter-index
  traffic, not ordinary MIDI CC.

### Morph Endpoint Bytes

Morph endpoint bytes arrive through:

- `PRF_RESTORE_MORPH_CC`
- `PRF_RESTORE_MORPH_CC2`

These call `seq_storeMorphParameterIngress(param, value)`.

Rules:

- Store into `morphParams[]`.
- Update morph endpoint automation target structs when the param is an
  automation selector.
- Do not apply live DSP merely because a morph endpoint byte arrived during file
  load.

### Automation Target Sidebands

Sideband messages include:

- `FRONT_CC_LFO_TARGET`
- `FRONT_CC_VELO_TARGET`
- macro target messages.

These carry resolved destination parameters, not necessarily the raw selector
bytes that live in `PAR_TARGET_LFOx` / `PAR_VEL_DEST_x` endpoint arrays.

Critical Session 004 lesson:

- Raw selector bytes and resolved automation target structs are two different
  representations.
- They must be kept coherent at every ingress path.

For LFO destinations:

- `FRONT_CC_LFO_TARGET` decodes to a resolved destination parameter.
- `seq_storeLfoDestinationIngress(...)` must update:
  - resolved `lfoDestination[]` in the appropriate automation target image;
  - raw `frontPanelParams[PAR_VOICE_LFOx]`;
  - raw `frontPanelParams[PAR_TARGET_LFOx]`;
  - matching `interpolatedParams[]` when the current/front image is updated;
  - raw `morphParams[PAR_VOICE_LFOx]` / `morphParams[PAR_TARGET_LFOx]` when the
    morph endpoint phase is active.

Why:

- Menu assignment can sound correct immediately because the resolved sideband
  directly updates the modulation node/DSP.
- If raw endpoint selector arrays remain stale/off, normal/temp switching later
  reads stale raw bytes and reapplies the wrong assignment.

## Parameter Outgress / Endpoint Push-Up

Endpoint push-up to AVR is still required:

- on every audible switch into temp;
- on every audible switch back to normal;
- on per-track/voice normal/temp source changes that affect the menu-visible
  voice.

STM sends:

- `PRF_RESTORE_PARAM_CC/CC2` for kit/front endpoint bytes;
- `PRF_RESTORE_MORPH_CC/CC2` for morph endpoint bytes.

AVR receives:

- kit/front bytes into `parameter_values[]`;
- morph bytes into `parameters2[]`.

Important:

- Endpoint restore pushes raw endpoint bytes, not resolved sidebands.
- AVR menu correctness depends on raw selector bytes being coherent before
  restore.
- Session 005 fixed the missing global morph menu sync on normal/temp switch
  with display-only STM-to-AVR report traffic.

## Morph Engine

### Standard Morph Baseline

Standard morph is STM-owned.

Base cycle:

1. Pick `seq_morphScanParam`.
2. Determine which synth voice owns that parameter.
3. Select normal/temp kit image from the voice source.
4. Read the selected voice's morph amount from that kit.
5. For ordinary params:
   - interpolate `frontPanelParams[param]` to `morphParams[param]`;
   - store in `interpolatedParams[param]`;
   - apply to DSP when needed.
6. For automation selector params:
   - store selector value from kit/front endpoint;
   - update `interpolatedAutomationTargets`;
   - avoid "morphing" the selector.

Session 003/004 invariant:

- `interpolatedParams[]` is worker-owned baseline.
- File load writes endpoint images; worker updates interpolation.
- Explicit STM-side copy-to-temp may clone interpolation as part of image copy.

### Live Apply Cache

The live apply cache is not a parameter validity model.

Purpose:

- allow zero-valued file-loaded params to land in DSP once;
- suppress repeated identical setter calls on later morph scan passes.

Do not:

- use the live apply cache to decide whether endpoint values are valid;
- reintroduce per-parameter valid arrays.

### Global And Per-Voice Morph

Current model:

- global morph input resets all STM per-voice morph bases for the selected image;
- actual interpolation reads per-voice morph state;
- per-voice morph can come from:
  - global morph;
  - step automation;
  - MIDI/automation target `PAR_MORPH_*`;
  - velocity one-shot special case;
  - LFO phased overlay.

`seq_vMorphAmount[]`:

- Legacy/front-facing mirror array.
- `seq_vMorphAmount[0]` is global-ish display/control mirror.
- `seq_vMorphAmount[1..6]` are per-voice mirrors.
- Per-voice morph amounts do not appear directly in the AVR menu.

Refactor target:

- Hide `seq_vMorphAmount[]` behind `/Preset/` or `/MorphEngine/` API.
- Sequencer should not own morph storage directly after refactor.

### Velocity To Voice Morph

Velocity to voice morph should not be a generic modulation matrix target.

Reason:

- `PAR_MORPH_*` is a control input to the STM morph engine, not an ordinary DSP
  parameter.
- Installing `PAR_MORPH_*` into `velocityModulators[]` can crash or route through
  the wrong modulation path.

Session 004 model:

- On voice trigger only:
  - detect whether the current normal/temp kit has velocity destination
    `PAR_MORPH_*`;
  - compute `round(255 * velocity/127 * velocityModDepth)`;
  - write that once to the target voice's live morph amount in the selected
    normal/temp kit image.

Expected behavior:

- velocity 127 and velocity modulation depth 127 yields morph amount 255.
- Lower velocity/depth scales proportionally.
- No continuous modulation-node interaction.

### LFO To Voice Morph

Old immediate path was too expensive:

- LFO tick calls `modNode_updateValue(...)`.
- VMORPH target calls `modNode_vMorph(...)`.
- `modNode_vMorph(...)` called `seq_modulateVoiceMorphAmount(...)`.
- That looped a full voice parameter mask immediately.
- Audio broke up when LFO-to-morph was assigned.

Session 004 model:

- LFO nodes still update their `lastVal`.
- `modNode_vMorph(...)` returns immediately for LFO mod nodes.
- `seq_serviceMorphInterpolation()` owns LFO-to-morph application.

Phased drain:

- `seq_morphDrainPhase == 0`
  - standard morph baseline phase.
  - Writes `interpolatedParams[]`.
- `seq_morphDrainPhase == 1..6`
  - source voice LFO overlay phases.
  - Each phase is active only when that source voice's LFO destination is
    `PAR_MORPH_*`.
  - Destination determines target voice.
  - Uses `lfoNode->lastVal` and LFO mod amount.
  - Interpolates from `interpolatedParams[param]` to `morphParams[param]`.
  - Applies directly to DSP.
  - Does not store.

Cursor behavior:

- `seq_morphScanParam` advances after phase 0 and all active LFO overlay phases
  for that parameter have been serviced.
- With no LFO-to-morph assignments, the worker behaves like the one-phase
  standard morph worker.
- With all six LFO-to-morph assignments active, each parameter can take up to
  seven main-loop passes, but no pass performs a burst of interpolations.

## DSP Boundary Lessons

DSP should not own preset/morph semantics.

DSP receives:

- ordinary parameter values;
- modulation destination updates;
- trigger events;
- envelope setters where explicitly requested.

DSP should not decide:

- normal vs temp image;
- front endpoint vs morph endpoint;
- selector-byte coherence;
- copy-to-temp semantics;
- background file-load state.

Special actionful paths:

- Envelope-position params can trigger envelope setters and should not be
  broadly applied during bulk normal/temp switching.
- `PAR_MORPH_*` is a morph-engine control input and needs special casing for
  LFO/velocity.

## AVR Temp Buffers

Current AVR arrays:

- `parameter_values_temp[]`
- `parameters2_temp[]`

Meaning:

- file-read scratch buffers for `.SND`, `.PRF`, `.ALL`;
- menu restoration/snapshot scratch in some paths.

They are not:

- STM temp playback params;
- background temp pattern storage;
- authoritative normal/temp sound state.

Rename targets:

- `parameter_values_fileBuffer[]`
- `parameters2_fileBuffer[]`
- or split scratch vs menu snapshot roles if both remain.

## Legacy Voice Cache / Hold-Unhold Paths

Current legacy state/functions:

- `midi_midiCache[]`
- `midi_midiCacheAvailable[]`
- `midi_midiLfoCache[]`
- `midi_midiLfoCacheAvailable[]`
- `midi_midiVeloCache[]`
- `midi_midiVeloCacheAvailable[]`
- `midi_midiKit[]`
- `midi_kitLfoCache[]`
- `midi_kitVeloCache[]`
- `frontParser_unholdVoice(...)`
- `frontParser_uncacheVoice(...)`
- `frontParser_releaseVoiceCache(...)`
- `frontParser_unholdLoadedVoice(...)`
- `frontParser_applyPendingVoiceCache(...)`
- `frontParser_applyDeferredVoiceCache(...)`

Session 004 finding:

- These paths were designed for older kit/voice load promotion.
- Temp-background loading should not depend on them.
- They caused first-switch freezes and stale live promotion behavior.

Refactor target:

- Split direct/legacy load cache promotion from temp-background-load finalizer.
- If direct `.SND`/voice load still needs a hold/release path, rebuild it as an
  explicit preset-load mode.
- Temp-background path should clear bookkeeping and release locks without
  replaying cached params into live DSP.

## Proposed `/Preset/` Refactor Layout

Actual repo path:

- `mainboard/LxrStm32/src/Preset/`

### `Preset/KitState.h/.c`

Own:

- renamed `PresetKitState` / `SeqKitState`;
- renamed `PresetAutomationTargets` / `SeqKitAutomationTargets`;
- normal/temp image instances;
- current image selection helpers;
- temp image capture/validity.

Move candidates:

- `seq_normalKitState`
- `seq_tmpKitState`
- `seq_getCurrentImageKitState()`
- `seq_getMorphKitForImage()`
- `seq_captureTmpKitState()`
- `seq_isTmpKitActive()`
- kit-image parts of `seq_setTmpKitActive()`

### `Preset/ParameterMap.h/.c`

Own:

- voice parameter masks;
- canonical parameter conversion;
- parameter classification.

Move candidates:

- `seq_voiceParamMask`
- `seq_canonicalParamFromVoiceMask()`
- `seq_firstVoiceForMask()`
- `seq_isVoiceParameter()`
- `seq_voiceMaskForParameter()`
- `seq_isAutomationTargetSelectorParam()`
- `seq_isMorphAmountParam()`
- `seq_morphVoiceForParam()`

Longer target:

- generate/share AVR and STM voice masks from one source.

### `Preset/AutomationTargets.h/.c`

Own:

- raw selector to resolved destination map;
- inverse destination to selector map;
- target voice inference;
- resolved target storage;
- target application to DSP/mod nodes.

Move candidates:

- `seq_modTargetParams`
- `seq_resolveAutomationTargetSelector()`
- `seq_selectorForAutomationTargetDestination()`
- `seq_voiceSelectorForAutomationTargetDestination()`
- `seq_updateInterpolatedAutomationTarget()`
- `seq_updateFrontAndInterpolatedAutomationTargets()`
- `seq_storeLfoDestinationIngress()`
- `seq_storeVelocityDestinationIngress()`
- `seq_storeMacroDestinationIngress()`
- `seq_applyLiveAutomationTargetSelector()`
- `seq_applyVoiceAutomationTargets()`
- `seq_applySharedAutomationTargets()`
- `seq_applyNormalEndpointAutomationTargets()`

API goal:

- No caller should update only raw selectors or only resolved targets.
- Provide coherent setters.

### `Preset/ParameterIngress.h/.c`

Own:

- raw endpoint byte ingress;
- normal/current image routing;
- passive vs actionful apply policy.

Move candidates:

- `seq_setIngressTarget()`
- `seq_getIngressTarget()`
- `seq_shouldApplyIngressToLive()`
- `seq_setAutomationIngressTarget()`
- `seq_storeParameterIngress()`
- `seq_storeMorphParameterIngress()`
- `seq_applySingleParameterValue()`
- any passive/apply flags used to avoid retrigger/envelope side effects.

API goal:

- MIDI/front parser delegates to "store param into selected preset image".
- Parser should not know normal/temp array internals.

### `Preset/MorphEngine.h/.c`

Own:

- morph amount state;
- interpolation;
- live apply cache;
- phased LFO-to-morph drain;
- velocity-to-morph one-shot helper;
- legacy mirror sync to `seq_vMorphAmount[]` until that mirror is removed.

Move candidates:

- `seq_syncVMorphAmountMirrorsFromLiveSources()`
- `seq_selectVoiceMorphAmountFromKit()`
- `seq_setVoiceMorphLiveAmount()`
- `seq_morphImageVoiceIsLive()`
- `seq_getMorphImageForVoice()`
- `seq_invalidateLiveMorphApplyCache()`
- `seq_invalidateAllLiveMorphApplyCaches()`
- `seq_resetLiveMorphApplyCache()`
- `seq_liveMorphApplyNeeded()`
- `seq_interpolateMorphValue()`
- `seq_morphAutomationValueToAmount()`
- `seq_advanceMorphScanCursor()`
- `seq_morphDrainPhase`
- `seq_lfoMorphAssignmentForSource()`
- `seq_voiceHasLfoMorphOverlay()`
- `seq_advanceMorphDrainPhase()`
- `seq_setGlobalMorphAmount()`
- `seq_setGlobalMorphAutomationValue()`
- `seq_resetVoiceMorphAmountsToGlobal()`
- `seq_setVoiceMorphAmount()`
- `seq_setVoiceMorphAutomationValue()`
- `seq_setVoiceMorphMaskAutomationValue()`
- `seq_applyVelocityVoiceMorphOnTrigger()`
- `seq_serviceMorphInterpolation()`

API goal:

- Sequencer reports trigger/source changes.
- Modulation nodes report source values or expose `lastVal`.
- Morph engine decides how to apply morph to the selected image/DSP.

### `Preset/EndpointRestore.h/.c`

Own:

- STM-to-AVR endpoint/menu push-up;
- restore queue;
- restore handshake;
- full vs masked restore.

Move candidates:

- `seq_pushSingleParameterToFront()`
- `seq_pushSingleMorphParameterToFront()`
- `seq_pushKitEndpointsToFront()`
- `seq_pushKitEndpointVoiceMaskToFront()`
- endpoint restore queue state;
- `seq_endpointRestorePopRequest()`
- `seq_endpointRestoreClearCurrent()`
- `seq_endpointRestoreWaitTimedOut()`
- `seq_endpointRestoreSendNextFull()`
- `seq_endpointRestoreSendNextMasked()`
- `seq_endpointRestoreSendNext()`
- `seq_endpointRestoreBusy()`
- `seq_serviceEndpointRestore()`
- `seq_pushEndpointUpdateForVoiceSourceChange()`
- `seq_maybePushKitEndpointsToFront()`

API goal:

- Sequencer asks for restore of full current image or selected voice mask.
- Restore module handles rate limiting and protocol details.

### `Preset/TempPlaybackSwitch.h/.c`

Own:

- per-track normal/temp source state;
- per-voice source application;
- hihat coupled behavior.

Move candidates:

- `seq_trackPatternUsesTmp()`
- `seq_synthVoiceUsesTmpFromTrackPatterns()`
- `seq_synthVoiceFromTriggerVoice()`
- `seq_applyVoiceSource()`
- `seq_markVoiceSourceTarget()`
- `seq_allVoiceSourcesUseTmp()`
- `seq_allVoiceSourcesUseNormal()`
- `seq_updateVoiceSourcesForPatternChange()`
- source-selection portions of `seq_setTmpKitActive()`

API goal:

- Sequencer reports pattern source changes.
- Preset/temp switch service applies correct parameter/morph/automation image.

### `Preset/PresetLoadCache.h/.c`

Own:

- PRF/cache/deferred file-load state now living in `frontPanelParser.c`.

Move candidates:

- `frontParser_beginFileLoadIngress()`
- `frontParser_endFileLoadIngress()`
- `frontParser_clearDeferredPerfLoad()`
- `frontParser_resetPrfPendingCounters()`
- `frontParser_clearPrfCacheSession()`
- `frontParser_clearPrfRuntimeFlags()`
- `frontParser_prfCacheSessionActive()`
- `frontParser_prfCacheCanExit()`
- `frontParser_prfCacheCountPatternWrite()`
- `frontParser_prfPendingCountsValid()`
- `frontParser_capturePrfStmLiveSnapshot()`
- `frontParser_prfCacheUseLivePattern()`
- `frontParser_prfCacheTrackUsesLivePattern()`
- `frontParser_prfCacheLiveStep()`
- `frontParser_prfCacheLiveMainSteps()`
- `frontParser_prfCacheLiveLengthRotate()`
- `frontParser_prfCacheLivePatternSetting()`
- `frontParser_prfCacheLiveMidiChannel()`
- `frontParser_prfCacheLiveNoteOverrideValue()`
- `frontParser_prfCacheTakeLiveVMorphFlag()`
- `frontParser_prfCacheLiveVMorphAmountValue()`
- split replacement for `frontParser_applyDeferredVoiceCache()`

### `Preset/ProtocolEndpoint.h/.c`

Optional thin layer.

Own:

- raw PRF restore param/morph opcode encoding;
- endpoint begin/end and automation phase interpretation;
- restore begin/ready/done/ack state.

Could overlap with a future broader `Comms/` split; keep small if introduced.

## Non-Negotiable Refactor Rules

- Do not run git commands.
- Do not make AVR canonical owner of temp playback.
- Do not route file loads into temp storage.
- Do not reintroduce per-parameter valid arrays.
- Do not make source switching copy storage.
- Do not remove endpoint push-up on normal/temp boundaries.
- Do not treat `PAR_MORPH_*` as ordinary modulation destinations.
- Keep SEQ16 temp keyhole until after the refactor.

## Session 005 Items Before Refactor

User identified likely Session 005 work:

- encoder phase is slightly unbalanced and needs adjustment;
- global morph menu sync on normal/temp switch was fixed in Session 005;
- automate temp pattern switch/background-load process;
- add global parameter switches for background loading;
- keep SEQ16 keyhole until after refactor.

## Recommended Refactor Start Criteria

Begin the large `/Preset/` move only after:

- Session 005 global morph closeout is documented and the remaining small
  workflow items are either handled or explicitly deferred;
- hardware confirms current normal/temp background loading remains stable;
- the code is committed by the user;
- the stale Session 004 working audits have been deleted or archived;
- this audit and the comms audit are treated as canonical references.
