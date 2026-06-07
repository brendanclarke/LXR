# Audit: Temp Audio Dropout After STM Morph Move

Date: 2026-06-04
Status: Session 004 working audit
Scope: Investigate why switching from normal patterns to `SEQ_TMP_PATTERN` can stop all audio, and why switching back to normal can leave audio silent.

## Corrected Temp-Cache Role Model

The intended post-morph architecture should not treat the AVR as an owner of temp playback state.

The AVR/front-panel role in temp caching should be narrow:

1. User initiates copy-to-temp. AVR sends a request to copy the currently selected normal pattern and current parameter set into STM temp storage.
2. User selects SEQ16/temp or returns to a normal pattern. AVR sends the requested pattern switch.
3. AVR receives kit/front endpoint bytes and morph parameter endpoint bytes from STM so the menu display reflects the currently selected/playback image.

The AVR still has `parameter_values_temp[]` and `parameters2_temp[]`, but in the current code those are file-read staging buffers used while reading `.SND`, `.PRF`, and `.ALL` data. They should not be the authority for temp playback. The authoritative temp playback state should be STM-side:

- `seq_tmpPattern`
- `seq_tmpKitState.frontPanelParams[]`
- `seq_tmpKitState.morphParams[]`
- worker-owned `seq_tmpKitState.interpolatedParams[]`
- temp automation target images

Desired STM-side temp-switch model:

1. Copy-to-temp copies everything needed for one complete alternate playback set:
   - one source normal pattern into `seq_tmpPattern`;
   - kit/front endpoint bytes;
   - morph parameter endpoint bytes;
   - current interpolation bytes;
   - front endpoint automation destination image;
   - morph endpoint automation destination image;
   - interpolated automation destination image;
   - normal/temp-specific morph amount state, once that state is added to the kit image.
2. Switching to temp copies no parameter or pattern storage. It only changes which existing STM pattern/kit image is used for playback and morph interpolation, applies the selected image cleanly to DSP, and transmits endpoint bytes back to AVR for menu display.
3. Switching back to normal likewise copies no storage. It reselects the normal STM pattern/kit image, applies it cleanly, and restores normal endpoint bytes to the AVR menu.

This means copy-to-temp should be a direct STM-side data copy, not a transport/staging exercise through AVR endpoint dumps or cleared normal endpoint arrays.

## AVR `*_temp` Buffers Are File-Read Scratch Images

The names `parameter_values_temp[]` and `parameters2_temp[]` are misleading in the current post-morph temp-pattern discussion.

Current definitions:

- `front/LxrAvr/Preset/presetManager.c`
- `parameter_values_temp[END_OF_SOUND_PARAMETERS]`
- `parameters2_temp[END_OF_SOUND_PARAMETERS]`

Current primary writer:

- `preset_readKitToTemp(uint8_t isMorph)`
- Reads a complete kit/front or morph parameter block from `.SND`, `.PRF`, or `.ALL` into one of these arrays.
- `isMorph == 0` writes `parameter_values_temp[]`.
- `isMorph != 0` writes `parameters2_temp[]`.
- Missing bytes at the end of short/old files are zero-filled.

Current primary readers:

- `preset_readDrumVoice(track, isMorph)` copies selected per-voice parameters from the scratch image into the menu/live endpoint arrays:
  - `parameter_values[]` for kit/front endpoint loads;
  - `parameters2[]` for morph parameter endpoint loads.
- `preset_readDrumsetMeta(isMorph)` copies non-voice tail parameters from the scratch image into `parameter_values[]` or `parameters2[]`.
- `.ALL` and `.PRF` load paths call `preset_readKitToTemp(1)` and `preset_readKitToTemp(0)` before copying selected subsets out to the real AVR menu endpoint arrays and dumping those endpoints to STM.
- `.SND` load calls `preset_readKitToTemp(isMorph)` and then copies the requested voice/meta subset out.

Legacy/secondary readers:

- `frontPanel_parseData(PATCH_RESET)` copies `parameter_values_temp[]` back to `parameter_values[]` for `END_OF_MORPH_PARAMETERS`.
- `menu_reloadKit()` does the same, reached by shift+start/stop in `buttonHandler.c`.

Conclusion:

- These arrays are best understood as AVR preset-file scratch buffers, not temporary pattern or background-playback buffers.
- They should not participate in SEQ16/temp pattern switching except indirectly when a file load has refreshed `parameter_values[]` / `parameters2[]` and those endpoint arrays are then sent to STM normal storage.
- A future rename would reduce confusion. Candidate names:
  - `preset_fileFrontParams[]` instead of `parameter_values_temp[]`;
  - `preset_fileMorphParams[]` instead of `parameters2_temp[]`.

## Observed Failure

User report:

- With the sequencer running, switching from a normal pattern set to SEQ16/temp stops all audio.
- LEDs indicate the sequencer/pattern continues to run.
- Switching back from temp to a normal pattern does not restore audio.

This points away from a total sequencer clock failure and toward one or more of:

- live DSP parameters being overwritten with silent/stale values;
- track trigger gating, such as `seq_tracksLocked`;
- source-state mismatch between normal/temp pattern data and normal/temp kit data;
- output routing or volume/decay parameters being applied from an uninitialized interpolation image.

## Current Code Path Map

### Copy-To-Temp Request

AVR copy-to-temp path:

- `front/LxrAvr/Menu/copyClearTools.c`
- `copyClear_copyPattern()`
- If destination is `SEQ_TMP_PATTERN`, AVR first calls `preset_dumpNormalEndpointsToStm()`.
- Then AVR sends `SEQ_COPY_PATTERN`.

STM endpoint dump path:

- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
- `FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN`
- Sets ingress to `SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT`.
- Clears `seq_normalKitState.frontPanelParams[]` and/or `seq_normalKitState.morphParams[]`.
- Receives `PRF_RESTORE_PARAM_*` and `PRF_RESTORE_MORPH_*` into normal endpoint storage.
- `FRONT_SEQ_TMP_KIT_ENDPOINT_END` restores ingress to current-image mode.

STM copy path:

- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
- `FRONT_SEQ_COPY_PATTERN`
- Calls `seq_copyPattern(src, dst)`.
- If `dst == SEQ_TMP_PATTERN`, `seq_copyPattern()` calls `seq_captureTmpKitState()`.

Current temp capture:

- `seq_captureTmpKitState()` copies normal front endpoint bytes, normal morph endpoint bytes, and endpoint automation target images into `seq_tmpKitState`.
- It does not initialize `seq_tmpKitState.interpolatedParams[]`.
- It does not initialize `seq_tmpKitState.interpolatedAutomationTargets`.
- It marks `seq_tmpKitState.valid = 1`.

### Switching To Temp

AVR SEQ16 request:

- `front/LxrAvr/buttonHandler.c`
- SEQ16 sends `SEQ_CHANGE_TMP_PAT`.

STM switch request:

- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
- `FRONT_SEQ_CHANGE_TMP_PAT` calls `seq_setNextPattern(SEQ_TMP_PATTERN, voice)`.

Pattern boundary:

- `seq_tick()` updates `seq_activePattern` and per-track active pattern state.
- `seq_setTmpKitActive(seq_activePattern == SEQ_TMP_PATTERN)` is called before per-track active pattern updates.
- `seq_updateVoiceSourcesForPatternChange()` later applies per-voice source changes.

Pre-fix temp activation:

- `seq_setTmpKitActive(1)` applies shared params from `seq_tmpKitState.interpolatedParams[]`.
- It applies shared automation targets from `seq_tmpKitState.interpolatedAutomationTargets`.
- It queues a full endpoint restore to AVR.
- Later, `seq_markVoiceSourceTarget(..., useTmp=1)` applies each changed voice from `seq_tmpKitState.interpolatedParams[]`.

Current Phase 1 test-build temp activation:

- `seq_setTmpKitActive(1)` no longer applies broad shared/non-voice DSP params.
- It selects temp-owned morph amounts, invalidates the temp live morph apply cache, queues endpoint restore, and marks temp active.
- Later, `seq_markVoiceSourceTarget(..., useTmp=1)` publishes the voice source first, selects that voice's temp morph amount, then applies voice-local params and automation targets.
- Switching back to normal follows the same shape: select normal morph amounts, invalidate normal cache, queue endpoint restore, and let per-voice source switching apply voice-local params.

## High-Probability Fault Pathways

### 1. Endpoint Dump Clears Normal Endpoints While Normal Is Live

The copy-to-temp endpoint dump currently uses normal endpoint ingress. At `FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN`, STM clears normal endpoint arrays before receiving the replacement bytes.

The morph worker continues running during this bracket. If normal is sounding, it can read the temporarily zeroed or partially refreshed normal endpoint arrays, compute interpolation from those transient values, and apply them to live DSP.

Why this fits the report:

- Audio can stop even though the sequencer still advances.
- DSP can receive zero/invalid voice parameters, volume, decay, oscillator, or routing values.
- The issue can persist because `interpolatedParams[]` may now contain zeros/stale values even after endpoint storage has been refilled.
- If the user switches to temp before a full morph scan has repaired normal interpolation, temp capture and later normal restore can both be based on incomplete worker state.

Architectural problem:

- A copy-to-temp snapshot should not require clearing or rewriting the live normal endpoint image.
- Normal endpoint ingress is being reused as a transport staging area.
- Desired behavior is simpler: copy the already-authoritative normal STM pattern/kit image directly into temp storage.

### 2. Temp Capture Does Not Seed Interpolated Params

`seq_captureTmpKitState()` copies endpoints only. That preserves Session 003's rule that the worker owns interpolation, but the switch path immediately applies `seq_tmpKitState.interpolatedParams[]`.

If temp interpolation has not been populated yet, entering temp applies zeroed/stale values to DSP.

Why switching back may not recover:

- Returning to normal applies `seq_normalKitState.interpolatedParams[]`.
- If normal interpolation was damaged or made stale during the endpoint dump window, normal restore can reapply bad values.
- The worker only updates the image selected by `seq_voiceSourceState`; while temp is live, normal interpolation may not be refreshed.

Architectural problem:

- Worker-owned interpolation does not mean the switch path can apply an uninitialized worker image.
- Copy-to-temp should copy the current normal interpolation image into temp along with the endpoint images.
- This does not mean AVR/file ingress writes interpolation. It means an explicit STM-side image copy can clone the current worker-owned interpolation image when creating the temp set.

### 3. Shared/Non-Voice Params Were Applied From Worker-Owned Interpolation

`seq_applySharedParameterValues()` applies all non-voice parameters from `kit->interpolatedParams[]`.

Earlier concern: `PAR_AUDIO_OUT1..6` could have followed the shared path if absent from the STM voice mask table. On the current code pass, those output params appear to be included in the voice mask, so they are less likely to be the specific shared-param dropout trigger.

The broader concern still stands: if temp shared interpolation is zero/stale when entering temp, the switch can apply wrong values for:

- macro destination/amount slots;
- kit/version/non-voice metadata;
- any other sound parameter omitted from `seq_voiceParamMask`.

Phase 1 now avoids broad shared/non-voice application on switch. This intentionally narrows the audible test to pattern switching, morph image selection, and voice-local parameter application. Shared/non-voice switching can be reintroduced later with a whitelist if a real parameter needs it.

### 3b. Voice Source Was Published After Automation Apply

Before the latest Phase 1 patch, `seq_markVoiceSourceTarget()` called `seq_applyVoiceSource()` before assigning `seq_voiceSourceState[synthVoice] = targetState`.

That matters because applying automation targets can immediately update a modulation node. Those modulation callbacks can query the current morph image through `seq_getMorphImageForVoice()`, which reads `seq_voiceSourceState[]`.

Failure shape:

1. User switches a voice/track to temp.
2. Temp automation is applied.
3. Automation update callbacks still see the voice as normal because `seq_voiceSourceState[]` has not been updated yet.
4. Modulation writes can be calculated/applied against the wrong normal/temp parameter image.

The current test build publishes `seq_voiceSourceState[]` before applying automation or voice-local params. This should prevent callbacks from observing the old image during the switch.

### 4. Track Locks Could Gate Triggers

`seq_triggerVoice()` returns immediately when `seq_tracksLocked` contains the voice bit.

File-load paths can lock tracks during fast load and unlock them later. If temp switch interacts with an unfinished or stale load/cache state, LEDs can continue while triggers produce no audio.

This is less likely than the parameter-image issue for the specific "switch to temp, then back still silent" report, but it must be ruled out because it exactly matches "sequencer appears to run, no voices trigger."

Relevant code:

- `seq_triggerVoice()`
- `seq_tracksLocked`
- `FRONT_SEQ_LOAD_VOICE`
- `FRONT_SEQ_UNHOLD_VOICE`
- `frontParser_unholdLoadedVoice()`
- `frontParser_releaseVoiceCache()`

### 5. Live Apply Cache Is Probably Secondary

The live morph apply cache can suppress repeated DSP writes, but direct source switching uses `seq_applyVoiceParameterValues()` and `seq_applySharedParameterValues()`, which call `seq_applySingleParameterValue()` directly.

Cache invalidation may still be needed around copy/switch boundaries, but the primary bug is likely that the values being directly applied are stale or zero.

### 6. Morph Amount State Is Not Yet Per Set

Current morph amount state is stored outside `SeqKitState`:

- `seq_globalMorphAmount`
- `seq_voiceMorphBaseAmount[SEQ_SYNTH_VOICES]`
- `seq_voiceMorphAmount[SEQ_SYNTH_VOICES]`
- legacy/front-facing mirrors such as `seq_vMorphAmount[]`

That means normal and temp do not yet have independent global/per-voice morph amount state. A temp set can copy endpoint and interpolation arrays, but the active morph amount state remains global to the sequencer.

Desired direction:

- Store the global morph amount and per-voice morph base amounts with each normal/temp kit image.
- On copy-to-temp, copy these morph amount fields from normal to temp.
- On switch-to-temp or switch-to-normal, select/restore the morph amount fields for that image.
- Treat live LFO/velocity modulation output separately from stored morph base state. Modulation can remain transient, but the base/global/per-voice morph positions should belong to the selected set.

This is probably not the immediate cause of total silence, but it is part of making temp switching conceptually complete and preventing later surprises where normal and temp share one morph position.

## Plan Of Action

### Phase 1: Audible STM-Only Temp Switching

Goal: make copy-to-temp and normal/temp playback switching conform to the direct STM model, so the user can hear temp playback and switch back to normal without audio dropout.

1. Remove AVR endpoint-dump staging from copy-to-temp.
   - Stop calling `preset_dumpNormalEndpointsToStm()` from the AVR copy-to-temp path.
   - AVR should send only the copy request.
   - STM should not clear or refill `seq_normalKitState` merely to create a temp copy.

2. Make copy-to-temp a direct STM-side copy.
   - Copy one source normal pattern into `seq_tmpPattern`.
   - Copy `seq_normalKitState.frontPanelParams[]` into `seq_tmpKitState.frontPanelParams[]`.
   - Copy `seq_normalKitState.morphParams[]` into `seq_tmpKitState.morphParams[]`.
   - Copy `seq_normalKitState.interpolatedParams[]` into `seq_tmpKitState.interpolatedParams[]`.
   - Copy all three automation destination images:
     - `frontPanelAutomationTargets`;
     - `morphParameterEndpointAutomationTargets`;
     - `interpolatedAutomationTargets`.
   - Mark the temp kit image valid only after the full copy is complete.

3. Add the STM-side infrastructure needed for clean source selection.
   - Switching to temp should copy no pattern or parameter storage.
   - Switching back to normal should copy no pattern or parameter storage.
   - The switch should only select which existing pattern/kit image drives playback, apply that image to DSP, and queue endpoint restore to AVR.
   - Ensure `seq_applySharedParameterValues()` and `seq_applyVoiceSource()` consume a complete image.

4. Handle morph amount state enough for stable audible switching.
   - Preferred: add normal/temp-owned morph amount fields now and copy/select them with the kit image.
   - Minimum Phase 1 fallback: leave morph amount state global but make sure it cannot cause silence while testing temp switching.
   - Stored/base global and per-voice morph positions should ultimately belong to the selected normal/temp set.

5. Handle shared/non-voice sound params that can affect audibility.
   - Audit parameters outside `seq_voiceParamMask`, especially `PAR_AUDIO_OUT1..6`, macro slots, kit version, `PAR_UNUSED01`, and VMORPH morph-modulation amount params (`PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`).
   - Either include per-voice-owned params such as `PAR_AUDIO_OUT1..6` in the voice mask, or explicitly handle them during source switch from endpoint/current interpolated values.
   - Avoid applying meaningless metadata as live DSP parameters.

6. Rule out trigger gating while testing Phase 1.
   - Inspect whether `seq_tracksLocked`, `seq_voicesLoading`, `seq_newVoiceAvailable`, or PRF deferred/cache state can remain set after the temp switch.
   - If there is a stale lock path, fix it independently from parameter image initialization.

Expected AVR menu behavior after Phase 1:

- The existing STM endpoint restore path should probably still work, because it already pushes `SeqKitState.frontPanelParams[]` and `SeqKitState.morphParams[]` back to AVR via `PRF_RESTORE_PARAM_*` and `PRF_RESTORE_MORPH_*`.
- If Phase 1 fills `seq_tmpKitState` correctly, switching to temp should queue a restore from that temp image, and switching back should queue a restore from `seq_normalKitState`.
- However, Phase 1 success should be judged primarily by audio behavior. If audio switching works but AVR menu values are missing, stale, partial, or late, treat endpoint restore/menu coherence as Phase 2 rather than mixing it into the first audio fix.

### Phase 2: AVR Menu Restore Coherence

Only start this after Phase 1 produces reliable audible temp/normal switching.

1. Verify full temp endpoint restore on switch-to-temp.
   - AVR `parameter_values[]` should reflect `seq_tmpKitState.frontPanelParams[]`.
   - AVR `parameters2[]` should reflect `seq_tmpKitState.morphParams[]`.

2. Verify full normal endpoint restore on switch-back.
   - AVR `parameter_values[]` should reflect `seq_normalKitState.frontPanelParams[]`.
   - AVR `parameters2[]` should reflect `seq_normalKitState.morphParams[]`.

3. Verify restore timing and queue behavior.
   - The restore queue should not block sequencer audio.
   - A new switch should not leave the AVR menu showing the previously selected image.
   - Full restores and masked per-voice restores should not fight each other.

4. Preserve restore constraints.
   - Do not reintroduce per-parameter valid arrays.
   - Zeros are authoritative endpoint values.
   - Do not add LCD/debug/status output unless explicitly requested.

### Hardware Test Sequence

1. Phase 1 audio tests:
   - Boot and confirm normal sound.
   - Copy active normal pattern to temp while running; confirm audio does not drop during the copy operation.
   - Switch to SEQ16/temp; confirm temp audio continues.
   - Switch back to normal; confirm normal audio returns.

2. Phase 2 menu tests:
   - On switch-to-temp, confirm AVR menu values show temp kit/front and morph parameter endpoints.
   - On switch-back, confirm AVR menu values show normal kit/front and morph parameter endpoints.
   - Confirm endpoint restore does not create audible interruption.

3. Regression tests:
   - Repeat after `.PRF` / `.ALL` load while temp is sounding.
   - Reconfirm Session 003 morph invariants: standard morph, file-loaded zero values, and LFO-to-voice-morph behavior.

## Do Not Do

- Do not reintroduce AVR live morph computation.
- Do not reintroduce per-parameter valid arrays.
- Do not route file loads into temp storage.
- Do not make AVR temp arrays authoritative for temp playback.
- Do not write file/AVR endpoint loads directly into interpolation arrays.
- Do not add LCD/debug/status output without explicit approval.

## Current Working Hypothesis

The initial dropout was most likely caused by applying uninitialized or transiently zeroed STM interpolation images during temp/normal source switching.

The originally suspicious sequence was:

1. Copy-to-temp endpoint dump clears normal endpoints while normal is live.
2. STM morph worker applies zero/partial normal endpoint values to live DSP.
3. `seq_captureTmpKitState()` copies endpoints but does not seed temp interpolation.
4. Entering temp directly applies zero/stale temp interpolation values.
5. Returning to normal applies normal interpolation that may also be stale or partially zeroed, so audio does not recover immediately.

The first Phase 1 fix removed the AVR endpoint dump and made temp capture a true STM-side snapshot of normal pattern and parameter state. Hardware testing still produced complete dropout when switching into temp, so the remaining likely causes moved downstream into the switch/apply path.

Current test-build hypothesis:

1. Broad shared/non-voice parameter application during `seq_setTmpKitActive()` may still write dangerous stale or semantically invalid non-voice values into the DSP. Phase 1 now suppresses that broad apply on both temp entry and normal return.
2. Voice-source ordering was wrong: automation callbacks could observe the old normal/temp source while a voice was being switched. Phase 1 now publishes the source state before applying voice-local params and automation targets.
3. If dropout persists after this build, the next strongest suspect is not endpoint staging anymore. It is either:
   - trigger gating/locks such as `seq_tracksLocked`;
   - temp pattern not actually containing active trigger data at the moment of switch;
   - a specific voice-local parameter in `seq_voiceParamMask` being copied/applied with a silent value;
   - endpoint restore traffic to AVR causing an unintended feedback write back into STM while audio is active.

## Phase 1 Implementation Status

Applied in the current test build:

- AVR copy-to-temp no longer calls `preset_dumpNormalEndpointsToStm()`.
- STM `seq_captureTmpKitState()` directly copies normal front endpoints, morph endpoints, interpolation, all three automation target images, and stored morph amount state into `seq_tmpKitState`.
- `SeqKitState` now carries `globalMorphAmount` and per-voice `voiceMorphBaseAmount[]`.
- Global and per-voice morph amount setters store their base amounts into the active normal/temp kit image.
- Temp/normal switching selects morph amounts from the selected kit image.
- Voice source switching now updates `seq_voiceSourceState[]` before applying params/automation.
- Broad shared/non-voice DSP param application is temporarily disabled during temp/normal switching to isolate the audio dropout.

Open after this build:

- The build has not yet been hardware-tested by the user.
- The STM build emits an unused-function warning for `seq_applySharedParameterValues()` because Phase 1 no longer calls it during switching. This is expected for the test build; cleanup or whitelisted reintroduction can happen after the audible path is proven.

## Per-Track Normal/Temp Completeness Audit

Hardware result after the Phase 1 test build:

- Switching into the temp pattern no longer drops all audio.
- Individual tracks can be switched between normal and temp pattern/parameter sources audibly.

Current per-voice/per-track behavior:

1. Pattern data source.
   - `seq_setNextPattern()` writes per-track pending pattern slots.
   - `seq_tick()` commits `seq_perTrackActivePattern[]` at the pattern boundary or instant switch point.
   - `seq_liveStepForTrack()` reads steps from `seq_perTrackActivePattern[track]`, so normal/temp pattern data is selected per track.

2. Hi-hat coupling.
   - Tracks 5 and 6 share synth voice 5.
   - `seq_setNextPattern()` couples tracks 5 and 6 whenever a normal/temp boundary is involved.
   - `seq_synthVoiceUsesTmpFromTrackPatterns()` treats synth voice 5 as temp if either hihat track is temp.
   - This matches the desired rule: closed/open hihat pattern source and hihat voice parameters move together across normal/temp.

3. DSP parameter image.
   - `seq_updateVoiceSourcesForPatternChange()` maps per-track active patterns to per-synth-voice normal/temp source state.
   - `seq_markVoiceSourceTarget()` publishes `seq_voiceSourceState[]`, selects the image-owned morph amount, then applies the selected kit image.
   - `seq_applyVoiceParameterValues()` applies voice-local params from `kit->interpolatedParams[]`, so DSP voice params use the interpolated array from the selected normal/temp struct.

4. Morph interpolation storage.
   - `seq_serviceMorphInterpolation()` derives the image from `seq_getMorphImageForVoice(synthVoice)`.
   - It writes interpolated values into that image's `interpolatedParams[]`.
   - It applies live DSP only when that image is live for the voice.
   - This is correct per synth voice for voice-local params.

5. Endpoint restore to AVR.
   - Full normal/temp switches queue a full endpoint restore.
   - Mixed per-track switches queue a masked restore for changed synth voices.
   - Masked restore sends both front endpoint bytes and morph endpoint bytes for the voice mask.
   - Caveat: masked restore only sends params present in `seq_voiceParamMask[]`. Shared/global params are not coherently per-track by design.

6. Automation target images.
   - Copy-to-temp clones all three target images:
     - `frontPanelAutomationTargets`;
     - `morphParameterEndpointAutomationTargets`;
     - `interpolatedAutomationTargets`.
   - The morph worker updates the selected image's interpolated target image when selector params are scanned.
   - `seq_applyVoiceSource()` applies LFO and velocity targets from the selected image's `interpolatedAutomationTargets`.
   - This is correct per synth voice for LFO/velocity target destinations.

Remaining strict-completeness gaps:

1. Macro modulation depth and macro destinations are still global live nodes.
   - Voice-local LFO/velocity destination and amount params are in the voice mask and can switch per synth voice.
   - Macro destinations/amounts are shared/non-voice params and broad shared apply is disabled in the Phase 1 test build.
   - The four `macroModulators[]` nodes are global live state, so they cannot currently be fully separated per normal/temp per-track voice.
   - Desired fix: either define macros as global to the current overall kit image, or add explicit image-aware macro state and a whitelisted apply/switch policy. This should not be solved accidentally through broad shared-param apply.

Conclusion:

- Pattern source, DSP interpolated arrays, endpoint arrays, interpolation writes, LFO/velocity automation targets, and hihat normal/temp coupling are aligned well enough for current audible testing.
- The strict remaining work is deciding how global macro modulation should behave when only some tracks are temp.

## Per-Voice Morph Separation Implementation

Applied after the per-track completeness audit:

1. `SeqKitState` now owns morph amount state.
   - `globalMorphAmount` stores the image's global/base morph amount.
   - `voiceMorphBaseAmount[]` stores the image's per-voice base amounts.
   - `voiceMorphAmount[]` stores the image's current live per-voice interpolation amounts.
   - The old file-scope STM mirrors `seq_globalMorphAmount`, `seq_voiceMorphBaseAmount[]`, and `seq_voiceMorphAmount[]` were removed.

2. `seq_vMorphAmount[]` is now only a compatibility mirror.
   - It still exists because `ParameterArray` and PRF live-cache code point at it.
   - It is no longer authoritative storage for normal/temp playback.
   - Per user note, per-voice morph amounts are STM-side playback/control state, not AVR menu state.

3. Base morph edits and automation are separated.
   - `seq_setGlobalMorphAmount()` writes the current image's `globalMorphAmount`, resets that image's per-voice base amounts, and resets that image's live per-voice morph amounts.
   - `seq_setVoiceMorphAmount()` writes one selected image's per-voice base amount and live amount.
   - `seq_setVoiceMorphAutomationValue()` writes only the selected image's live per-voice amount.
   - `seq_setGlobalMorphAutomationValue()` writes live per-voice amounts across currently selected voice images without overwriting stored/base morph amounts.

4. Morph interpolation now reads from the selected image.
   - `seq_serviceMorphInterpolation()` reads `kit->voiceMorphAmount[synthVoice]`.
   - The image is selected via `seq_getMorphImageForVoice()`, so mixed normal/temp tracks interpolate into their own `SeqKitState`.

5. Copy-to-temp clones the complete morph amount state.
   - Copy-to-temp copies `globalMorphAmount`, `voiceMorphBaseAmount[]`, and `voiceMorphAmount[]` from normal to temp.

6. Switch-to-temp/back updates only mirrors and selected DSP state.
   - Full normal/temp switches update `seq_tmpKitActive` before syncing the global compatibility mirror.
   - Per-voice switches sync the selected voice's mirror from the selected kit image before applying voice params and automation.

## Endpoint Restore / Front-Panel Sync Fault

Observed after audio-side temp switching started working:

- Normal/temp audio and per-track source separation sound correct.
- The AVR menu does not update on full-pattern or per-track normal/temp switches.
- The last adjusted menu values remain visible until the user adjusts a parameter again.
- The first switch into playing temp can freeze the front panel for several seconds.
- After that, subsequent push-ups appear not to start.

Likely failure path:

1. STM queues an endpoint restore and sends `PARAM_RESTORE_BEGIN`.
2. AVR sets `frontParser_restoreActive = 1`, which suppresses normal outbound front-panel traffic until `PARAM_RESTORE_DONE`.
3. If STM misses or delays `PARAM_RESTORE_READY`, it waits in `SEQ_ENDPOINT_RESTORE_PHASE_WAIT_READY`.
4. AVR remains restore-active because it is waiting for DONE.
5. Normal user messages from AVR to STM are suppressed, so subsequent switches/requests appear frozen or never start.

Fixes applied:

1. STM handshake priority.
   - `PARAM_RESTORE_READY` and `PARAM_RESTORE_ACK` are now handled at the very top of `frontParser_handleMidiMessage()`.
   - They bypass PRF live-cache and deferred-performance cache hooks.
   - This prevents restore handshake bytes from being consumed by unrelated traffic-control modes.

2. STM restore timeout.
   - Endpoint restore wait phases now maintain `seq_endpointRestoreWaitCounter`.
   - If WAIT_READY times out, STM sends `PARAM_RESTORE_DONE` to release AVR `frontParser_restoreActive`, then clears the current restore transaction.
   - If WAIT_ACK times out, STM clears its own current restore transaction.
   - This prevents a lost handshake byte from permanently blocking future push-ups.

3. AVR DONE cleanup ordering.
   - AVR now clears `frontParser_restoreActive` before repainting and sending ACK on `PARAM_RESTORE_DONE`.
   - This shortens the restore-active window and avoids holding outbound traffic longer than necessary.

Remaining thing to watch:

- Full restores still send many front+morph endpoint messages. If the front panel still pauses briefly but values update correctly, the next optimization should be restore payload sizing/rate, not the audio source-switch model.

## First-Switch Delay After File Load

Observed after restore handshake fixes:

- Parameter sync to AVR works for full normal/temp switches and per-track switches.
- A remaining multi-second delay can happen on the first switch into temp after loading a file.
- The same kind of delay can happen on the first switch back to normal after loading a file while playing from temp.

Correction:

- Endpoint push-up must happen on every change into or out of temp. Removing changeover push-up breaks the AVR menu authority model.
- The attempted move of endpoint sync to copy-to-temp was wrong and has been reverted.
- Full normal/temp switch and per-track switch must continue to queue endpoint restores at changeover.

More likely cause:

- The "first switch only after file load" shape matches deferred file-load cache work, not ordinary endpoint restore.
- In the manual pattern switch path, `seq_tick()` calls `frontParser_applyDeferredVoiceCache()` when `seq_loadPendigFlag` is set.
- `frontParser_applyDeferredVoiceCache()` can replay deferred performance messages and/or apply pending voice caches after `.prf`/performance-style loading.
- Once this runs, the pending deferred state is cleared, which explains why subsequent temp/normal switches sync correctly without the same delay.

Question: should `frontParser_applyDeferredVoiceCache()` apply at all?

- For the temp-backed `.PRF` / `.ALL` background-load path, probably not in the old "apply cached voice params to live DSP" sense.
- That function exists for an older/direct kit-load safety model: hold voice parameter payloads while a voice/kit is being loaded, then release them at a safer boundary so the live kit is not partially updated mid-voice.
- In the new intended temp model, `.PRF` / `.ALL` file data should already have populated the STM normal storage image while playback continues from temp storage. The changeover should select the normal or temp `SeqKitState` and push endpoints to AVR; it should not need to replay `midi_midiCache` as the authority for the sound.
- The current implementation partly agrees with this: `frontParser_releaseVoiceCache()` and `frontParser_unholdLoadedVoice()` clear the pending cache instead of applying it when `seq_isTmpKitActive()` is true.
- However, `frontParser_applyDeferredVoiceCache()` is mixed-purpose. Besides voice-cache promotion, it also drains deferred performance messages, applies pending temp pattern data, clears deferred-load flags, clears PRF runtime flags, and exits the PRF cache session.
- Therefore the answer is not "remove the function from the flow"; it is "do not use live voice-cache application as the changeover mechanism for temp-backed `.PRF` / `.ALL` loads."
- The first-switch hang can still be this function even if the sound data is already correctly separated, because the first switch may be the first time all deferred cache/session cleanup is drained in one foreground burst.

Next fix direction:

- Keep endpoint restore at every temp boundary.
- Split the current mixed responsibility into two conceptual operations:
  - a legacy/direct-load operation that applies pending voice cache to the live normal kit when that is genuinely required;
  - a temp-background-load finalizer that clears/dequeues PRF/deferred bookkeeping without replaying cached voice params into the live DSP.
- Move or drain the temp-background-load finalizer before musical changeover when it is safe, especially when playback is already isolated on temp.
- If any remaining deferred work cannot be moved entirely, make it incremental so the first switch after file load does not perform all pending work in one foreground burst.
- Do not blindly delete the legacy voice-cache path; `.SND`, direct kit/voice loads, or non-temp load modes may still depend on held-cache promotion.

## Temp Boundary Retrigger-Like Audio Glitch

Observed:

- Switching between normal and temp while audio is playing can produce a sudden short sound that resembles all voices being retriggered and then stopping a few ms later.
- The sound appears even when voices/tracks are muted.
- This does not sound like ordinary parameter morphing or oscillator phase movement. It sounds like trigger/envelope activity.

Expected model:

- Normal/temp switching should not trigger voices.
- The switch should only change which pattern and parameter image is read:
  - normal pattern set vs `seq_tmpPattern`;
  - normal `SeqKitState` vs temp `SeqKitState`.
- No switch path should behave like `seq_triggerVoice()`, `voiceControl_noteOn()`, or a manual note-on.

Important code finding:

- The direct sequencer trigger path is probably not the main explanation for the "muted voices still glitch" report.
  - Normal step triggering goes through the mute check in `seq_nextStep()`.
  - `seq_triggerVoice()` is only reached after the mute check for ordinary sequencer steps.
  - Therefore, if all tracks are muted and the sound still happens, the glitch is likely bypassing the sequencer trigger/mute gate.

High-probability pathway: actionful DSP parameter setters during source switch.

1. A temp/normal full switch changes all synth voices from one `SeqKitState` source image to the other.
2. `seq_updateVoiceSourcesForPatternChange()` detects the source change.
3. For each changed synth voice, `seq_markVoiceSourceTarget()` calls `seq_applyVoiceSource()`.
4. `seq_applyVoiceSource()` calls `seq_applyVoiceParameterValues()`.
5. `seq_applyVoiceParameterValues()` sends every parameter in `seq_voiceParamMask[synthVoice]` to `seq_applySingleParameterValue()`.
6. Each voice mask includes the envelope-position parameter:
   - voice 1: `PAR_ENVELOPE_POSITION_1` / raw param `221`
   - voice 2: `PAR_ENVELOPE_POSITION_2` / raw param `222`
   - voice 3: `PAR_ENVELOPE_POSITION_3` / raw param `223`
   - voice 4: `PAR_ENVELOPE_POSITION_4` / raw param `224`
   - voice 5: `PAR_ENVELOPE_POSITION_5` / raw param `225`
   - voice 6: `PAR_ENVELOPE_POSITION_6` / raw param `226`
7. `seq_applySingleParameterValue()` routes these high params through `midiParser_ccHandler(...)`.
8. The MIDI parser envelope-position cases call:
   - `drumVoice_setEnvelope(...)`
   - `snare_setEnvelope(...)`
   - `cymbal_setEnvelope(...)`
   - `hihat_setEnvelope(...)`
9. Those DSP setters are not passive value setters when the value is `0`.
   - Drum/snare `envPos == 0` calls pitch/volume envelope trigger functions.
   - Cymbal/hat `envPos == 0` calls the volume envelope trigger function.
10. This is effectively a trigger-like DSP action caused by applying stored parameters, not by sequencer playback.

Why this matches the symptom:

- It can affect all voices on a full normal/temp switch because all synth voice sources change.
- It can happen while all tracks are muted because the mute gate is only in the sequencer step trigger path, not in raw DSP parameter setters.
- It can sound exactly like a brief retrigger because envelope setters call `DecayEg_trigger(...)` / `slopeEg2_trigger(...)`.
- It can be brief because only envelopes/transients are being kicked without a normal sequencer note context.

Lower-probability switch-path side effects still worth noting:

- `seq_nextStep()` always calls `voiceControl_noteOff(0xFF)` after a pattern switch, including temp boundaries.
  - This is not expected to trigger internal DSP voices by itself; current `voiceControl_noteOff(0xFF)` clears `active_voices` and sends MIDI note-offs.
  - It is still a non-read-only side effect and should be reviewed against the desired "only source selection changes" model.
- The switch occurs before the same `seq_nextStep()` call continues into ordinary track processing.
  - At a bar boundary, a legitimate first-step trigger can happen after the switch if steps are active and tracks are unmuted.
  - This does not explain the muted-track report, but it can make boundary behavior hard to distinguish by ear.
- Roll/SOM paths can call `seq_triggerVoice()` outside the ordinary muted-step branch.
  - This is not the leading candidate unless the glitch appears only with roll/SOM state active.

Investigation-only next steps:

1. Treat `PAR_ENVELOPE_POSITION_*` as the first suspect.
   - Verify whether temp-boundary switch applies these params to DSP.
   - Hardware A/B idea for a code test later: skip only `PAR_ENVELOPE_POSITION_*` in the source-switch parameter apply path and check whether the glitch disappears.

2. Separate "stateful value apply" from "actionful performance commands".
   - Applying a parameter image during source switching should restore steady DSP state.
   - It should not call setters that intentionally trigger envelopes or transients.

3. Audit other actionful params before patching broadly.
   - Envelope position is confirmed actionful.
   - Also look for setters that reset LFOs, retrigger transients, pulse trigger outs, reset oscillator phase, or send note-on/off as side effects.

4. Keep endpoint push-up out of the blame path unless proven otherwise.
   - STM-to-AVR endpoint restore sends menu bytes and should not apply live DSP.
   - The glitch matches STM-side voice parameter application more closely than front-panel sync traffic.

Likely repair direction after confirmation:

- Do not stop normal/temp source switching from applying necessary steady-state voice parameters.
- Instead, make the source-switch apply path skip or specially handle actionful parameters such as `PAR_ENVELOPE_POSITION_*`.
- A cleaner future API would distinguish:
  - "store/restore this parameter value into the selected image";
  - "apply this parameter as a live user/performance action";
  - "apply this parameter passively to rebuild DSP state without retriggering."

Fix applied:

- Added a general MIDI parser apply-flag API:
  - `midiParser_ccHandlerWithFlags(msg, updateOriginalValue, applyFlags)`
  - `MIDI_PARSER_CC_APPLY_NORMAL`
  - `MIDI_PARSER_CC_APPLY_PASSIVE`
- Kept the existing `midiParser_ccHandler(msg, updateOriginalValue)` as a compatibility wrapper that uses normal behavior.
- In passive mode, the envelope-position CC2 handler still updates `midi_envPosition[]`, but suppresses the actionful `drumVoice_setEnvelope(...)` call.
  - Because the current CC2 grouped handler uses `drumVoice_setEnvelope(...)` for `CC2_ENVELOPE_POSITION_1..6`, this suppresses the known trigger-like source reached by source-switch parameter apply.
- Added `seq_applySingleParameterValueWithFlags(...)` in the sequencer.
- `seq_applyVoiceParameterValues(...)` now uses passive apply flags when rebuilding a changed normal/temp voice source from a stored kit image.
- Other callers of `midiParser_ccHandler(...)` remain on normal behavior, so live edits, automation, cache release, and external MIDI keep their existing envelope-position action semantics unless they explicitly opt into passive apply later.

Hardware test expectation:

- Full normal/temp switch should still apply the selected voice parameter image.
- `PAR_ENVELOPE_POSITION_*` values should remain mirrored in `midi_envPosition[]`.
- The switch should no longer kick envelope setters for all voices, so the muted-track retrigger-like blip should disappear or be substantially reduced.
- If a residual glitch remains, audit the next actionful parameters in the same passive-apply category rather than removing normal/temp source application.

Hardware result:

- User reports that the passive envelope-position fix sounds exactly the same.
- Interpretation: the direct `CC2_ENVELOPE_POSITION_* -> *_setEnvelope(...)` path was either not the active cause, or it was only one of multiple actionful apply paths.
- The current fix is therefore insufficient, but it is still useful infrastructure: temp-boundary source switching can now opt into a non-performance "apply parameter image" mode.

Important follow-up finding: passive apply does not yet cover modulation-node side effects.

- `midiParser_ccHandlerWithFlags(...)` still calls `modNode_originalValueChanged(paramNr)` at the end of both normal CC and CC2 handling, regardless of `MIDI_PARSER_CC_APPLY_PASSIVE`.
  - This is probably not the immediate trigger-like path by itself; `modNode_originalValueChanged(...)` updates stored modulator `originalValue` snapshots and does not directly call the envelope setters.
  - It is still part of the broader "live edit" API semantics that are currently being reused for passive source-switch rebuild.
- Several parameter cases also directly call `modNode_updateValue(...)` while still inside the passive source-switch rebuild:
  - LFO mod amount changes call `midiParser_setLfoModAmount(...)`, which calls `modNode_updateValue(...)`.
  - Velocity mod amount changes call `midiParser_setVelocityModAmount(...)`, which calls `modNode_updateValue(...)`.
  - Macro amount changes call `modNode_updateValue(&macroModulators[n], macroModulators[n].lastVal)`.
- `modNode_updateValue(...)` has its own actionful envelope-position tail:
  - if a modulation destination is `PAR_ENVELOPE_POSITION_1..6` and `midi_envPosition[voice]` is nonzero, it calls `drumVoice_setEnvelope(...)`, `snare_setEnvelope(...)`, `cymbal_setEnvelope(...)`, or `hihat_setEnvelope(...)`.
- That path does not go through the new MIDI parser passive guard.
- Therefore, if the current normal/temp parameter image has an LFO, velocity, or macro destination pointed at envelope position, the source-switch parameter apply can still call envelope setters even though the direct CC2 envelope-position handler is passive.

Why this remains consistent with "muted voices still glitch":

- Muting blocks ordinary sequencer step triggers.
- It does not block `modNode_updateValue(...)` or raw DSP setters reached while rebuilding a parameter image.
- A muted-track sound can therefore still happen if the switch mutates live DSP state directly rather than firing through `seq_triggerVoice(...)`.

Second follow-up finding: full source-switch voice rebuild is still a large synchronous DSP rewrite.

- `seq_applyVoiceParameterValues(...)` currently applies every canonical param in `seq_voiceParamMask[synthVoice]` for each voice whose normal/temp source changes.
- This means a full normal/temp boundary can rewrite roughly 37 voice parameters per synth voice in one foreground burst, plus shared parameters when the full kit source changes.
- Even when none of those setters is explicitly a note trigger, rewriting oscillator frequency, filter parameters, transient settings, envelope rates, modulation depths, macro depths, output routing, and related active voice controls can produce a short discontinuity if voices are ringing.
- This is a lower-level "state mutation pop/blip" explanation rather than a true retrigger, but it can sound similar when all voices are updated at once.

Next contained API-level attempts, without transferring DSP runtime state:

1. Extend passive mode through the modulation-node API.
   - Add a passive variant or flag-aware path for `modNode_updateValue(...)`.
   - During passive source-switch apply, skip only the actionful envelope-position tail inside `modNode_updateValue(...)`.
   - Keep normal user edits, macro movement, velocity hits, LFO ticks, and live modulation behavior unchanged.
   - Expected code surface if implemented later: `modulationNode.h`, `modulationNode.c`, `MidiParser.c`; possibly `sequencer.c` only if the passive flag needs to be routed more explicitly.

2. Diagnostic-only narrow test: suppress `modNode_originalValueChanged(...)` during passive source-switch apply.
   - This would be a quick A/B to test whether the remaining live-edit bookkeeping contributes to the glitch.
   - It is less semantically complete than a flag-aware modulation API because it can leave mod-node original values stale until the next normal modulation event.
   - Because `modNode_originalValueChanged(...)` does not directly call envelope setters, this is not the preferred first test after the failed direct envelope-position fix.

3. Reduce source-switch rebuild to values that actually changed at the live DSP boundary.
   - `seq_applySharedParameterValues(...)` already has a last-live-value cache for shared params.
   - `seq_applyVoiceParameterValues(...)` does not currently skip unchanged voice params; it applies the full voice mask when a voice source changes.
   - A per-voice last-live cache, or use of the existing live morph apply cache where appropriate, could reduce unnecessary setter traffic.
   - This would not solve genuinely actionful setters by itself, but it would lower the number of DSP writes at the boundary and make remaining side effects easier to isolate.

4. If the modulation-node passive extension does not change the hardware result, audit other actionful setter families in the source-switch apply list.
   - Start with setters that can reset phase, retrigger/transient state, or rewrite active envelope/filter state.
   - Keep this as an API-level "passive apply" audit, not a DSP runtime-state transfer project.

Current recommendation:

- Next code attempt should be the modulation-node passive extension, not DSP envelope-state transfer.
- It is still contained inside the parameter/modulation API and directly addresses a confirmed bypass around the passive envelope-position guard.
- Do not attempt full DSP runtime state copy yet. That would require carrying oscillator/filter/envelope/transient runtime state across normal/temp images, which is far more invasive than the current symptom justifies.

Follow-up fix applied for next hardware test:

- Added a modulation-node apply flag API:
  - `modNode_updateValueWithFlags(vm, val, flags)`
  - `MOD_NODE_UPDATE_NORMAL`
  - `MOD_NODE_UPDATE_PASSIVE`
- Kept the existing `modNode_updateValue(vm, val)` wrapper as normal/live behavior.
- In passive mode, `modNode_updateValueWithFlags(...)` still updates `lastVal` and applies the modulation value to the destination parameter, but suppresses only the envelope-position action tail:
  - `PAR_ENVELOPE_POSITION_1..3 -> drumVoice_setEnvelope(...)`
  - `PAR_ENVELOPE_POSITION_4 -> snare_setEnvelope(...)`
  - `PAR_ENVELOPE_POSITION_5 -> cymbal_setEnvelope(...)`
  - `PAR_ENVELOPE_POSITION_6 -> hihat_setEnvelope(...)`
- Routed passive source-switch parameter rebuild through the flag-aware modulation-node path for:
  - LFO mod amount changes;
  - velocity mod amount changes;
  - macro mod amount changes.
- Routed `seq_applyVoiceSource(...)` automation-target restore through passive modulation-node updates when switching a voice between normal and temp sources.
- Normal live modulation behavior remains routed through the compatibility wrappers:
  - live MIDI/front-panel edits still use normal update semantics;
  - macro movement, velocity hits, LFO ticks, and explicit automation-target edits still keep their existing actionful envelope behavior.

Hardware test expectation for this pass:

- If the glitch was caused by a macro/LFO/velocity destination pointing to envelope position during normal/temp source-switch restore, this should remove or substantially reduce it.
- If the sound is unchanged again, the remaining leading suspect becomes bulk synchronous parameter-image application/discontinuity rather than the known envelope setter paths.

Build result:

- `make -C mainboard/LxrStm32 stm32` passed with the existing project warning set.
- `make firmware` rebuilt `firmware image/FIRMWARE.BIN` for hardware retest.

Hardware result:

- User reports that the retrigger-like sound is still present.
- This means both contained API experiments failed to remove the audible glitch:
  - direct passive suppression of `CC2_ENVELOPE_POSITION_* -> *_setEnvelope(...)`;
  - passive suppression of the modulation-node envelope-position tail reached through macro/LFO/velocity restore.
- Current conclusion: do not keep these code changes as a functional fix.
- Keep the audit notes because they document eliminated pathways and future refactor targets.
- Reset code and firmware binary back to HEAD while retaining audit document edits.

Post-reset direction:

- Treat the remaining glitch as more likely caused by bulk synchronous parameter-image application or another actionful setter family, not by the two tested envelope-position paths alone.
- Next investigation should start from the switch-time call stack and reduce what is applied at the boundary, ideally with a diagnostic that applies no voice parameters, then selected parameter families, rather than adding more suppressors blindly.

## Reset Verification And New Glitch Plan

Reset verification, without using git:

- `MEMORY.md` now contains the imperative that no LLM/agent shall ever run git commands in a coding context.
- The failed-pass source symbols are absent from `mainboard/LxrStm32/src/`:
  - `MIDI_PARSER_CC_APPLY_*`
  - `midiParser_ccHandlerWithFlags(...)`
  - `MOD_NODE_UPDATE_*`
  - `modNode_updateValueWithFlags(...)`
  - `seq_applySingleParameterValueWithFlags(...)`
- Those symbols remain only in audit/refactor documents, which is intentional.
- `firmware image/FIRMWARE.BIN` is back to `296436` bytes, matching the pre-experiment size observed before the failed modulation-node pass.
- Source currently matches the expected post-reset shape: boundary switching again calls ordinary `seq_applyVoiceSource(...)`, which calls ordinary `seq_applyVoiceParameterValues(...)` and `seq_applyVoiceAutomationTargets(...)`.

What the failed experiments eliminated:

- The audible glitch is not fixed by suppressing only direct `CC2_ENVELOPE_POSITION_* -> *_setEnvelope(...)` calls during source switch.
- The audible glitch is not fixed by also suppressing modulation-node envelope-position tails reached through LFO, velocity, or macro restore.
- Therefore the next pass should not add more narrow suppressors first. The better first move is to isolate the whole boundary operation.

Current strongest model:

- The temp boundary currently does more than select which image is read.
- For any voice whose normal/temp source changes, `seq_markVoiceSourceTarget(...)` immediately calls `seq_applyVoiceSource(...)`.
- `seq_applyVoiceSource(...)` immediately writes the selected image's full voice parameter set into the one live DSP voice struct for that synth voice, then reapplies automation targets.
- That means a pattern-source switch can rewrite oscillator, filter, transient, envelope, modulation, routing, and morph-related live state for several voices in one foreground burst.
- Since the STM has one live DSP struct per voice, this rewrite can disturb sounding DSP state even when the sequencer mute gate blocks ordinary note triggers.
- The sound still "feels like retrigger" because several setter families affect trigger-adjacent state:
  - envelope position setters can trigger envelopes;
  - transient and snap state can produce short attacks when disturbed;
  - oscillator frequency/waveform/filter changes can click if active signal exists;
  - automation target restore can change modulation destination and recompute live mod values.

Important side-paths still to isolate:

- `voiceControl_noteOff(0xFF)` is still called after pattern switch in `seq_nextStep()`.
  - It does not call internal synth note-off, but it is still a boundary side effect.
  - It clears MIDI active-note state and can send external note-offs.
  - It should be isolated only after parameter-application isolation, because the muted-track symptom still points more strongly at DSP state writes.
- Roll/SOM paths can call `seq_triggerVoice(...)` outside the ordinary muted-step branch.
  - Hardware tests should ensure roll and SOM are off while isolating the boundary glitch.
- The same `seq_nextStep()` call can continue into ordinary step processing immediately after a pattern switch.
  - With all tracks muted this should not explain the sound, but it can confuse tests when any track is unmuted.
- `seq_serviceMorphInterpolation()` applies one morph parameter per main-loop pass to voices considered live by `seq_getMorphImageForVoice(...)`.
  - If a voice source is marked switched but DSP application is deferred, morph live-apply must also respect the deferred state, otherwise it can continue rewriting the live DSP voice before the next note trigger.

### New Resolution Plan

Phase A: prove whether boundary voice-source application is the cause.

1. Add a temporary diagnostic build that makes temp boundary switching update only source bookkeeping and AVR endpoint sync.
   - Keep `seq_voiceSourceState[...]` updates.
   - Keep per-voice morph amount mirror selection.
   - Keep endpoint push-up to AVR at every normal/temp boundary.
   - Do not call `seq_applyVoiceSource(...)` at the boundary.
   - Do not apply voice parameters or automation targets at the boundary.
2. Hardware expectation:
   - If the retrigger-like sound disappears, the glitch is inside immediate live DSP image application.
   - If the sound remains, look next at `voiceControl_noteOff(0xFF)`, roll/SOM, same-tick step processing, or endpoint restore side effects.
3. This test should be removed or converted after it answers the question; it is not the final musical behavior because changed parameters would not become audible until another mechanism applies them.

Phase B: if Phase A removes the glitch, move from boundary-apply to trigger-time apply.

1. Replace immediate boundary `seq_applyVoiceSource(...)` with a per-voice "source dirty" bit.
   - Boundary switch marks the affected synth voice dirty.
   - Boundary switch still selects normal/temp pattern reads immediately.
   - Boundary switch still pushes endpoint parameters to AVR immediately so the menu is correct.
2. At the start of a legitimate voice trigger, before `seq_parseAutomationNodes(...)` and before `voiceControl_noteOn(...)`, apply the dirty voice's selected source image.
   - This makes the next actual hit for that voice use the correct normal/temp parameters.
   - If the voice is muted, no trigger occurs and no DSP image rewrite occurs.
   - Any unavoidable setter clicks are masked by, or coincide with, an intentional note attack instead of happening as an independent boundary sound.
3. Hi-hat handling:
   - Closed/open hi-hat tracks share one synth voice state.
   - Any normal/temp switch involving either hi-hat track should mark the shared hi-hat synth voice dirty.
   - The next closed or open hi-hat trigger applies the selected hi-hat source before the trigger.
4. Morph handling:
   - Dirty voice sources must prevent background morph live-apply from writing that voice before trigger-time source application.
   - `seq_serviceMorphInterpolation()` and `seq_modulateVoiceMorphAmount(...)` should still update stored/interpolated kit state, but should skip live DSP application for voices whose source is dirty.
   - Once trigger-time source apply completes, clear the dirty bit and allow normal live morph application again.
5. Automation handling:
   - Apply stored automation targets together with the dirty source at trigger time, before parsing step automation for the current hit.
   - Step automation then applies on top of the correct source image for that hit.

Phase C: if Phase A does not remove the glitch, isolate non-parameter boundary side effects.

1. Temporarily suppress `voiceControl_noteOff(0xFF)` only for temp-boundary switches.
   - Keep normal pattern-change note-off behavior for non-temp pattern changes.
   - Expected result: if the sound disappears, the issue is not normal DSP note triggering but the boundary note-off/trigger-output/external-MIDI cleanup path.
2. Temporarily stop same-tick step processing after a temp boundary.
   - Return from `seq_nextStep()` after source/menu switch for one diagnostic build.
   - Expected result: if this removes the sound, the issue is not source selection but ordinary/roll/SOM trigger processing later in the same tick.
3. Explicitly test with roll and SOM disabled.
   - If the sound only occurs with roll/SOM active, isolate `seq_setRoll(...)`, `seq_rollTrig(...)`, and `som_tick(...)`.

Phase D: if trigger-time apply is confirmed but still clicks on the first intentional hit, narrow by parameter family.

1. Build a family-gated `seq_applyVoiceParameterValues(...)` diagnostic.
2. Apply families in this order:
   - automation destinations only;
   - level/pan/routing/decimation style params;
   - envelope slopes and envelope position;
   - oscillator pitch/waveform/mod-osc params;
   - filter params;
   - transient/snap params;
   - LFO params and modulation depths.
3. Stop at the first family that recreates the unwanted extra sound.
4. Only then design a targeted passive setter or deferred family apply. Do not reintroduce broad passive APIs until the family is known.

Preferred final architecture if Phase A confirms the model:

- Pattern-source switching should be a read-source selection event, not an immediate full DSP rewrite event.
- Endpoint/menu sync should happen at every boundary.
- Pattern data should be read from the selected normal/temp pattern immediately.
- Parameter image data should be selected immediately for future reads, but live DSP parameter application should occur at the next musically legitimate event for that voice.
- Background morph interpolation may update selected kit storage continuously, but live DSP writes for a dirty switched voice should wait until that voice is applied at trigger time.

## Diagnostic: Disable Temp-Boundary AVR Endpoint Push-Up

Purpose:

- Quick hardware check requested by user: determine whether the retrigger-like sound is related to STM-to-AVR parameter restore traffic on normal/temp pattern switches.

Temporary code change:

- Set `seq_tmpKitPushParamsToFrontEnabled = 0` in `sequencer.c`.
- Also keep it disabled in `seq_init()`.
- This suppresses:
  - full kit endpoint push-up in `seq_setTmpKitActive(...)` via `seq_maybePushKitEndpointsToFront(...)`;
  - per-voice endpoint push-up in `seq_pushEndpointUpdateForVoiceSourceChange(...)`, because that function already checks the same gate.

Expected behavior:

- Switching normal-to-temp and temp-to-normal should still switch pattern/parameter source internally.
- The AVR menu will intentionally not be synchronized at those boundaries during this diagnostic.
- If the retrigger-like sound disappears, endpoint restore traffic or its handshake/service timing becomes a leading suspect.
- If the sound remains unchanged, endpoint push-up is unlikely to be the cause and the plan should return to isolating boundary live-DSP source application.

Build result:

- `make -C mainboard/LxrStm32 stm32` passed with the existing project warning set.
- `make firmware` rebuilt `firmware image/FIRMWARE.BIN`.
- Firmware image end offset reported by builder: `296428`.

Hardware result:

- User reports that the retrigger-like glitch is still present with temp-boundary AVR endpoint push-up disabled.
- Conclusion: STM-to-AVR endpoint restore traffic is unlikely to be the cause of the sound.
- Restored `seq_tmpKitPushParamsToFrontEnabled = 1` so parameter endpoint push-up to the AVR happens again on normal/temp boundaries.
- Return to the main plan: isolate live DSP voice-source application at the switch boundary.
- Rebuilt firmware after restoring endpoint push-up; firmware image end offset returned to `296436`.

## Implemented Test: Per-Voice Deferred Morph Live Apply

Purpose:

- Test the current strongest remaining theory: the switch itself is not retriggering voices, but switch-time morph cache invalidation lets `seq_serviceMorphInterpolation()` immediately resume live DSP setter calls for the newly selected normal/temp image.
- User model to enforce:
  - normal/temp pattern switching changes which pattern and parameter image are read;
  - cached live morph application for a switched voice should not be invalidated at the boundary;
  - that voice's live morph cache should become invalid only just before a legitimate retrigger, or when that voice's morph amount is explicitly updated.

Code change:

- Added `seq_voiceMorphApplyDeferred[SEQ_SYNTH_VOICES]` in `sequencer.c`.
- Added a per-voice cache invalidator:
  - `seq_invalidateLiveMorphApplyCacheForVoice(image, synthVoice)`
  - It only clears `seq_liveMorphAppliedKnown[image][param]` for that synth voice's parameter mask.
- Added a deferred-release path:
  - `seq_releaseDeferredMorphLiveApplyForVoice(synthVoice, applyNow)`
  - It clears the deferred flag and invalidates only that voice's selected normal/temp image cache.
  - With `applyNow != 0`, it immediately applies the current interpolated values for that voice just before a legitimate trigger.
- `seq_markVoiceSourceTarget(...)` now marks the changed synth voice deferred after selecting the target normal/temp source.
- `seq_applyLiveMorphParameterValue(...)` returns early while that synth voice is deferred, so the background morph worker can continue updating stored/interpolated kit state without touching live DSP for that switched voice.
- Removed the switch-time broad image invalidation from `seq_setTmpKitActive(...)`.
  - Entering temp no longer calls `seq_invalidateLiveMorphApplyCache(SEQ_MORPH_IMAGE_TMP)`.
  - Leaving temp no longer calls `seq_invalidateLiveMorphApplyCache(SEQ_MORPH_IMAGE_NORMAL)`.
- Removed the broad temp-image invalidation from `seq_captureTmpKitState()`.
  - Source switching now supplies the deferred per-voice invalidation instead.
- `seq_triggerVoice(...)` releases and applies deferred morph values for the track's synth voice before `seq_parseAutomationNodes(...)` and before `voiceControl_noteOn(...)`.
  - Tracks 5 and 6 both map to synth voice 5 for shared hi-hat handling.
- Per-voice morph amount changes release the deferred state without immediate application, and invalidate that voice's cache so the normal morph worker can apply the new values.
  - touched paths include `seq_setVoiceMorphLiveAmount(...)`, `seq_setGlobalMorphAmount(...)`, `seq_resetVoiceMorphAmountsToGlobal(...)`, and `seq_setVoiceMorphAmount(...)`.
- `seq_modulateVoiceMorphAmount(...)` releases the deferred state only if the voice is currently deferred, then continues applying its live overlay normally.

Expected hardware result:

- If the retrigger-like sound disappears with all tracks muted, then the cause was likely post-switch background morph live-apply, not AVR endpoint push-up and not immediate voice-source apply alone.
- If the sound remains, then the next suspects are:
  - immediate `seq_applyVoiceSource(...)` still doing enough live DSP rewriting despite the earlier manual test;
  - `voiceControl_noteOff(0xFF)` or same-tick step processing;
  - a boundary side effect outside the morph live-apply path.

Important limitation:

- This is still implemented inside `Sequencer` for speed and containment.
- It should move to the future STM-side `/Preset/` morph service together with normal/temp kit image ownership, morph amount ownership, voice-source ownership, and live-apply cache ownership.

Hardware result:

- User reports no improvement to the original normal/temp switch retrigger.
- New failure: after a switch, the retrigger-like sound now appears whenever morph value changes.

Interpretation:

- This makes the morph live-apply path a more suspicious area, but shows the first implementation had the wrong release semantics.
- The mistake was treating "per-voice morph amount changed" as "release the deferred voice and invalidate its whole live morph cache."
- Once released, the background morph worker could resume live DSP setter calls for that voice. Because the cache had been invalidated for the voice, the next scan could replay a broad family of setter calls, recreating the retrigger-like sound on morph movement.
- A morph amount update should update the selected kit's morph amount and let stored/interpolated state move, but it should not clear a source-switch deferral that exists specifically to prevent boundary-adjacent live DSP setter traffic.

Correction:

- Removed deferred-release calls from morph amount setters:
  - `seq_setVoiceMorphLiveAmount(...)`
  - `seq_setGlobalMorphAmount(...)`
  - `seq_resetVoiceMorphAmountsToGlobal(...)`
  - `seq_setVoiceMorphAmount(...)`
- Removed deferred-release from `seq_modulateVoiceMorphAmount(...)`.
- The only remaining release point is now `seq_triggerVoice(...)`, immediately before normal trigger processing for that track's synth voice.

Revised expectation:

- Morph movement after a source switch should no longer trigger a full live-cache release.
- If the original switch glitch remains, the next likely test is to combine this strict morph deferral with deferring `seq_applyVoiceSource(...)` itself until trigger time, even though an earlier manual test suggested `seq_applyVoiceSource(...)` alone was not the whole cause.

## Side Fix: Global Morph 8-Bit Transport And Push-Up

Problem:

- User reports global morph from the menu responds only for values `0..127`.
- Menu values `128..255` appear to do nothing.
- Per-voice step automation at value `127` reaches the expected full morph endpoint, which points away from interpolation math and toward transport/framing.

Cause:

- The front-panel UART parser treats any byte with the high bit set as a new status byte.
- The old menu path sent:
  - `SEQ_CC, SEQ_SET_GLOBAL_MORPH, value`
- Therefore `value >= 128` could not survive as `data2`.

Implemented fix:

- Added a 7-bit-clean command pair:
  - `SEQ_SET_GLOBAL_MORPH` / `FRONT_SEQ_SET_GLOBAL_MORPH` (`0x68`) carries values `0..127`.
  - `SEQ_SET_GLOBAL_MORPH_HI` / `FRONT_SEQ_SET_GLOBAL_MORPH_HI` (`0x69`) carries values `128..255`.
- AVR menu send now encodes:
  - command = `SEQ_SET_GLOBAL_MORPH | ((value >> 7) & 0x01)`
  - data2 = `value & 0x7f`
- STM front parser reconstructs:
  - `value = data2 | (command == FRONT_SEQ_SET_GLOBAL_MORPH_HI ? 0x80 : 0x00)`
  - then calls `seq_setGlobalMorphAmount(value)`.

Global morph push-up:

- STM endpoint restore now sends the selected `SeqKitState.globalMorphAmount` back to AVR during the existing restore transaction, before endpoint parameter bytes.
- The same 7-bit-clean command pair is used for this push-up.
- AVR `SEQ_CC` receive now handles the global morph command pair and updates:
  - `parameter_values[PAR_MORPH]`
  - `morphValue`
  - menu repaint
- Because this happens while `frontParser_restoreActive` is set, AVR outbound traffic remains suppressed and the pushed-up global morph value should not echo back down as an authoritative edit.

Build result:

- `make -C front/LxrAvr avr` passed with existing warnings.
- `make -C mainboard/LxrStm32 stm32` passed with existing warnings.
- `make firmware` passed.
- Firmware image end offset: `296604`.

## Current Suspect Ledger

Known suspects still in play:

- `frontParser_applyDeferredVoiceCache()`
  - Called in the manual switch path before per-track active patterns are committed.
  - It is mixed-purpose: cache promotion, deferred perf messages, PRF cleanup/session state.
  - Not yet isolated for the retrigger-like sound.
- `voiceControl_noteOff(0xFF)`
  - Called after every pattern switch, including temp boundaries.
  - Expected to clear active MIDI-note bookkeeping rather than trigger DSP, but not yet isolated.
- `seq_voiceSourceState[]`
  - The raw assignment is low suspicion, but its readers can wake other systems:
    `seq_getMorphImageForVoice()`, morph interpolation, live morph apply, modulation, and endpoint source selection.
- `seq_vMorphAmount[]`
  - Medium suspicion as a compatibility mirror and UI/morph amount bridge.
  - Direct array writes are less suspicious than the setter paths and morph live-apply traffic that follow morph amount changes.

Mostly acquitted or partially acquitted:

- STM-to-AVR endpoint push-up:
  - Temporarily disabled; glitch remained.
- Direct envelope-position setter suppression:
  - Glitch remained.
- Modulation-node envelope tail suppression:
  - Glitch remained.
- `seq_applyVoiceSource()` as sole cause:
  - User manually commented out its body; glitch remained.
  - Therefore its direct children are unlikely to be the only cause:
    `seq_applyVoiceParameterValues()`, `seq_applySingleParameterValue()`, `midiParser_ccHandler(...)` through that path, and `seq_applyVoiceAutomationTargets()`.
  - Still possible as a collaborator with same-tick step processing, morph sweep, or other switch-block side effects.

### Same-Tick Continuation

Meaning:

- `seq_nextStep()` does not return after the pattern/temp switch block finishes.
- In the same call that commits the normal/temp switch, it continues into the rest of sequencer step processing.

Current post-switch continuation includes:

- beat LED pulse messages;
- roll quantization/rate handling;
- `trigger_clockTick(seq_stepIndex[NUM_TRACKS] + 1)`;
- per-track length/scale/rotation step advancement;
- roll start/stop and `seq_checkRollStep(...)`;
- SOM tick if active;
- ordinary muted-track/main-step/substep/probability checks;
- possible `seq_triggerVoice(...)` calls for active unmuted steps;
- reference step index increment;
- current-step LED update;
- `midiParser_checkMtc()`.

Why it matters:

- A temp switch can be committed, then the newly selected normal/temp pattern can be evaluated for the current tick immediately afterward.
- If the active position lands on a step, roll event, SOM event, or trigger-clock side effect, the audible result can feel like "the switch retriggered everything" even if the trigger came from ordinary post-switch step processing in the same `seq_nextStep()` call.
- This is especially relevant when `switchOnNextStep && seq_loadSeqNow` allows a switch away from bar start.

Simple diagnostic direction:

- Add a temporary test build that returns from `seq_nextStep()` immediately after a temp-boundary switch is committed and acknowledged.
- Keep non-temp pattern changes unchanged.
- Expected result:
  - If the retrigger-like sound disappears, the culprit is downstream same-tick sequencer processing, not the switch assignment itself.
  - If it remains, focus back on switch-block side effects: `frontParser_applyDeferredVoiceCache()`, `voiceControl_noteOff(0xFF)`, `seq_voiceSourceState[]`, and `seq_vMorphAmount[]` / morph live-apply.

Implemented diagnostic:

- Added a temporary early return in `seq_nextStep()` after:
  - temp boundary detection;
  - `voiceControl_noteOff(0xFF)`;
  - AVR pattern-change acknowledgement;
  - `seq_loadPendigFlag = 0`.
- Only temp-boundary switches return early.
- Non-temp pattern switches still call `seq_realign()` and continue existing behavior.
- This intentionally skips the rest of the current tick after a temp boundary:
  - beat pulse;
  - roll/SOM handling;
  - trigger clock;
  - per-track step processing;
  - possible `seq_triggerVoice(...)` calls;
  - current-step LED update.

Hardware result:

- User reports the glitch remained.
- User reverted code while keeping audit notes.
- Conclusion: same-tick continuation is unlikely to be the cause.

## Diagnostic: Suppress `voiceControl_noteOff(0xFF)` On Temp Boundaries

Purpose:

- Eliminate `voiceControl_noteOff(0xFF)` as a switch-block suspect before moving deeper into `seq_vMorphAmount[]`.
- This is simpler and narrower than morph-state diagnostics because the suspect is a single call site in the pattern switch block.

Temporary code change:

- In `seq_nextStep()`, skip `voiceControl_noteOff(0xFF)` when `tmpBoundaryPatternChanged` is true.
- Non-temp pattern changes still call `voiceControl_noteOff(0xFF)` normally.
- Temp boundary switch still performs:
  - active pattern/source commit;
  - `frontParser_applyDeferredVoiceCache()`;
  - `seq_updateVoiceSourcesForPatternChange(...)`;
  - temp-boundary ack flag;
  - AVR pattern-change acknowledgement;
  - `seq_loadPendigFlag = 0`.

Expected hardware result:

- If the retrigger-like glitch disappears, `voiceControl_noteOff(0xFF)` or its collaborators become the leading culprit.
- If the glitch remains, `voiceControl_noteOff(0xFF)` is mostly acquitted and the next lead is `seq_vMorphAmount[]` / morph mirror writes and their downstream readers.

Hardware result:

- User reports the glitch remained.
- User reverted code while keeping audit notes.
- Conclusion: `voiceControl_noteOff(0xFF)` is mostly acquitted.

## Interview: `seq_vMorphAmount[]` And Morph Associates

New evidence:

- Changing the morph kit from the menu can produce the same retrigger-like sound even when global morph amount is `0`.
- At morph amount `0`, the effective interpolated parameter values should remain at the kit/front endpoint.
- Therefore the sound is less likely to be caused only by "new morph endpoint values changed the audible interpolated kit" and more likely to be caused by a morph-control path, cache invalidation, automation target reapply, or live setter burst.

Direct `seq_vMorphAmount[]` role:

- `seq_vMorphAmount[0]` mirrors the selected image's global morph amount.
- `seq_vMorphAmount[1..6]` mirror per-synth-voice morph amounts.
- It is exposed through `ParameterArray.c` as `TYPE_UINT8_VMORPH` for:
  - `PAR_MORPH_DRUM1`
  - `PAR_MORPH_DRUM2`
  - `PAR_MORPH_DRUM3`
  - `PAR_MORPH_SNARE`
  - `PAR_MORPH_CYM`
  - `PAR_MORPH_HIHAT`
- The direct mirror writes are in:
  - `seq_syncVMorphAmountMirrorsFromLiveSources()`
  - `seq_selectVoiceMorphAmountFromKit()`
  - `seq_setVoiceMorphLiveAmount()`
  - `seq_setGlobalMorphAutomationValue()`
  - `seq_setVoiceMorphAmount()`
  - `seq_init()` memset

Important observation:

- Loading/changing a morph kit from the AVR menu does not obviously write `seq_vMorphAmount[]`.
- The morph-kit load path sends `PRF_RESTORE_MORPH_*` endpoint bytes to STM, which store into `SeqKitState.morphParams[]`.
- This means `seq_vMorphAmount[]` may be involved in temp switching, but the new morph-kit-load symptom points to a broader morph subsystem suspect.

Known contacts and collaborators:

1. `TYPE_UINT8_VMORPH` in `ParameterArray.c`
   - `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` point at `seq_vMorphAmount[1..6]`.
   - `paramArray_setParameter(...)` does not directly write `TYPE_UINT8_VMORPH`; it falls through default.
   - However, `modulationNode.c` treats `TYPE_UINT8_VMORPH` specially and calls `modNode_vMorph(...)`.

2. `modNode_vMorph(...)`
   - Called when a modulation node destination is a voice morph parameter.
   - Calls `seq_modulateVoiceMorphAmount(...)`.
   - `seq_modulateVoiceMorphAmount(...)` loops over that voice's parameter mask and calls `seq_applyLiveMorphParameterValue(...)`.
   - This is a high-risk collaborator because it can produce a burst of live DSP setter calls without a normal note trigger.

3. `modNode_setDestination(...)`
   - If the old destination was `TYPE_UINT8_VMORPH`, it calls `modNode_vMorph(vm, 0.f)`.
   - Then it calls `modNode_resetTargets()`.
   - Then it sets the new destination.
   - If the new destination is `TYPE_UINT8_VMORPH`, it calls `modNode_vMorph(vm, vm->lastVal)`.
   - Therefore merely reapplying automation destinations can invoke morph live-apply machinery.

4. `seq_applyVoiceAutomationTargets(...)`
   - Calls `modNode_setDestination(...)` and immediately `modNode_updateValue(...)` for LFO/velocity destinations.
   - If the selected/resolved destination is a VMORPH parameter, this calls into morph live-apply.

5. `seq_applyNormalEndpointAutomationTargets()`
   - Called at `FRONT_SEQ_TMP_KIT_ENDPOINT_END`.
   - Reapplies normal/front endpoint automation targets to live voices.
   - This occurs even after a morph-only endpoint dump, because the endpoint-end handler does not currently check which endpoint group was loaded.
   - If any front endpoint LFO/velocity/macro target points to VMORPH, the morph kit load can call live morph apply even when global morph amount is `0`.

6. Morph endpoint ingress:
   - `PRF_RESTORE_MORPH_CC/CC2` with current-image ingress calls `seq_storeMorphParameterIngress(...)`.
   - In normal endpoint bracket mode, the parser writes `seq_normalKitState.morphParams[param] = value`.
   - These storage writes do not directly apply DSP, but the background morph worker will see new `morphParams[]` values on later scan passes.

7. `seq_serviceMorphInterpolation()`
   - Runs continuously in the STM main loop.
   - Computes `interpolatedParams[param]` from front endpoint, morph endpoint, and per-voice morph amount.
   - Calls `seq_applyLiveMorphParameterValue(...)` for the live source image.
   - At morph amount `0`, the computed value should equal the front endpoint, but cache state can still decide whether a live setter call happens.

8. `seq_liveMorphApplyNeeded(...)`
   - The live-apply cache suppresses repeated identical setter calls per image/parameter.
   - If cache entries are unknown, stale, or invalidated, a morph scan can call setters even when the computed value equals the normal/front endpoint.
   - This remains a plausible shared mechanism for both temp switching and morph-kit-load glitches.

9. PRF/cache snapshot path:
   - `frontParser_capturePrfStmLiveSnapshot()` copies `seq_vMorphAmount[]`.
   - `frontParser_prfCacheLiveVMorphAmountValue(...)` can later feed `sequencer_sendVMorph(...)`.
   - `sequencer_sendVMorph(...)` calls `seq_setVoiceMorphMaskAutomationValue(...)`.
   - This path is relevant around file/PRF cache sessions, but less obviously tied to a manual morph-kit load.

Current ranking after this interview:

- Highest suspicion:
  - `seq_applyLiveMorphParameterValue(...)` and live morph setter bursts from the background morph worker or VMORPH modulation paths.
  - `modNode_vMorph(...)` / `seq_modulateVoiceMorphAmount(...)` when automation destinations touch VMORPH.
  - `seq_applyNormalEndpointAutomationTargets()` being called after morph-only endpoint loads.
- Medium suspicion:
  - `seq_vMorphAmount[]` mirror writes during temp source switching.
  - `seq_syncVMorphAmountMirrorsFromLiveSources()` / `seq_selectVoiceMorphAmountFromKit()`.
- Lower suspicion:
  - Direct `seq_vMorphAmount[]` storage itself.
  - The array does not appear to be directly written by morph-kit load, so it cannot alone explain the new symptom.

Suggested next diagnostics:

1. VMORPH modulation path isolation:
   - Temporarily make `modNode_vMorph(...)` return immediately.
   - Expected result:
     - If morph-kit-load and/or temp-switch glitches disappear, VMORPH modulation/live overlay is the leading culprit.
     - If they remain, the broader background morph worker/live-apply path is more likely.

2. Morph-only endpoint-end isolation:
   - Track whether the current endpoint bracket was `MORPH_ONLY`.
   - Skip `seq_applyNormalEndpointAutomationTargets()` at `FRONT_SEQ_TMP_KIT_ENDPOINT_END` for morph-only loads.
   - Expected result:
     - If morph-kit-load glitch disappears but temp-switch glitch remains, endpoint-end automation reapply is a separate morph-load offender.

3. Background morph live-apply isolation:
   - Temporarily allow `seq_serviceMorphInterpolation()` to update `interpolatedParams[]` and automation target storage, but skip `seq_applyLiveMorphParameterValue(...)`.
   - Expected result:
     - If both temp-switch and morph-kit-load glitches disappear, the live morph apply path is the shared offender.
     - This is a blunt test because morph movement will not affect live DSP during the diagnostic.

Implemented diagnostic:

- `modNode_vMorph(...)` now returns immediately.
- This disables VMORPH modulation/live overlay reached from modulation-node destinations.
- It does not disable:
  - global morph amount storage;
  - per-voice morph amount storage;
  - background morph interpolation;
  - direct `seq_applyLiveMorphParameterValue(...)` calls from `seq_serviceMorphInterpolation()`.

Expected hardware result:

- If the morph-kit-load glitch disappears, VMORPH modulation target reapply is implicated.
- If the normal/temp switch glitch disappears, VMORPH modulation/live overlay is implicated in temp switching too.
- If both glitches remain, move to the broader background morph live-apply diagnostic.

Hardware result:

- User reports both glitches remained.
- User reverted code while keeping audit notes.
- Conclusion: `modNode_vMorph(...)` / VMORPH modulation-node overlay is mostly acquitted.

## Diagnostic: Disable Background Morph Live Apply

Purpose:

- Test the broader shared suspect: `seq_serviceMorphInterpolation()` live DSP setter traffic.
- This is broader than VMORPH modulation-node overlay because the background worker can call live apply directly for ordinary scanned morph parameters.

Temporary code change:

- In `seq_serviceMorphInterpolation()`, keep:
  - scan cursor advance;
  - live image lookup;
  - interpolation computation;
  - `kit->interpolatedParams[param] = value`;
  - `seq_updateInterpolatedAutomationTarget(kit, param, value)`.
- Temporarily skip:
  - `seq_applyLiveMorphParameterValue(image, synthVoice, param, liveValue)`.

Expected hardware result:

- If both the temp-switch glitch and morph-kit-load glitch disappear, the shared offender is confirmed as background live morph apply/setter traffic.
- If morph-kit-load glitch disappears but temp-switch remains, morph endpoint writes are probably reaching live DSP through the morph worker, while temp switch has an additional suspect.
- If both glitches remain, look next at endpoint-end automation reapply, `frontParser_applyDeferredVoiceCache()`, and non-morph switch-block side effects.

Hardware result:

- User reports:
  - normal/temp pattern switch glitch remained;
  - morph-kit-load glitch disappeared;
  - sound parameters were incorrect after loading the `.all` file during this diagnostic;
  - parameters became correct after copy-to-temp, switch-to-temp, and then switch back to normal.
- User reverted code while keeping audit notes.

Conclusion:

- Background morph live apply is confirmed as the cause, or a necessary cause, of the morph-kit-load glitch.
- Background morph live apply is not the only cause of the normal/temp switch glitch.
- Disabling `seq_applyLiveMorphParameterValue(...)` inside `seq_serviceMorphInterpolation()` is not a viable functional fix as-is because `.all` load correctness depends on the morph worker eventually applying or refreshing live parameter state.
- The `.all` observation implies a distinction:
  - File load can leave stored endpoint/interpolated images correct but live DSP stale when background live apply is disabled.
  - Copy-to-temp and normal/temp switching can force enough source/image application to make parameters sound correct again.

Updated split of issues:

1. Morph-kit-load glitch:
   - Leading culprit: background morph live apply writing live DSP during/after morph endpoint replacement.
   - Likely fix shape: suspend or gate live morph apply during morph endpoint ingress, then resume/recompute in a controlled way after the endpoint image is stable.
   - Must not simply disable live apply globally, because `.all` live parameter refresh suffers.

2. Normal/temp switch glitch:
   - Survives without same-tick continuation, `voiceControl_noteOff(0xFF)`, `seq_applyVoiceSource()` body, VMORPH modulation overlay, and background morph worker live apply.
   - Remaining suspects rise in priority:
     - `frontParser_applyDeferredVoiceCache()`;
     - `seq_syncVMorphAmountMirrorsFromLiveSources()` / `seq_selectVoiceMorphAmountFromKit()` mirror writes or downstream readers;
     - `seq_voiceSourceState[]` itself or source-state readers outside the disabled worker path;
     - `seq_setTmpKitActive(...)` endpoint/menu push bookkeeping only if not already fully acquitted by the earlier push-up-disable diagnostic;
     - `seq_applyNormalEndpointAutomationTargets()` for load/morph cases, but less likely for pure temp switch unless it is invoked by a concurrent file/cache state.

Next recommended normal/temp switch diagnostic:

- Isolate `frontParser_applyDeferredVoiceCache()` during temp-boundary switches.
- Keep the call for non-temp pattern changes and stopped/direct load behavior.
- Expected result:
  - If the normal/temp glitch disappears, the mixed deferred-cache finalizer is the switch offender.
  - If it remains, move to a `seq_vMorphAmount[]` mirror-write isolation diagnostic.

## Diagnostic: Skip Deferred Cache Finalizer On Temp Boundaries

Purpose:

- Test whether `frontParser_applyDeferredVoiceCache()` is causing the normal/temp switch glitch.
- This tests only temp-boundary switch culpability, not whether the function remains necessary for other load/session paths.

Temporary code change:

- In `seq_nextStep()`, compute `pendingTmpBoundary` before the manual-switch branch drains deferred cache state.
- `pendingTmpBoundary` compares:
  - old global active pattern vs pending global pattern;
  - each old per-track active pattern vs each pending per-track pattern.
- In the `seq_loadPendigFlag` path:
  - skip `frontParser_applyDeferredVoiceCache()` when `pendingTmpBoundary` is true;
  - keep `frontParser_applyDeferredVoiceCache()` for non-temp pattern switches.
- The later existing `tmpBoundaryPatternChanged` detection remains unchanged and still controls temp-boundary ack/realign behavior.

Expected hardware result:

- If the normal/temp switch glitch disappears, `frontParser_applyDeferredVoiceCache()` or one of the actions it drains is the switch offender.
- If the glitch remains, `frontParser_applyDeferredVoiceCache()` is mostly acquitted for temp switching and the next diagnostic should isolate `seq_vMorphAmount[]` mirror writes.

Known diagnostic side effect:

- If a real deferred load/cache session is pending, skipping the finalizer during a temp-boundary switch may leave cleanup/deferred state pending until another path drains it.
- Acceptable for this test.

Hardware result:

- User reports:
  - normal/temp pattern switch glitch remained;
  - morph-kit-load glitch remained.
- User reverted code while keeping audit notes.

Conclusion:

- `frontParser_applyDeferredVoiceCache()` is mostly acquitted for the normal/temp switch glitch.
- It is also not relevant to the morph-kit-load glitch in this test.
- For the normal/temp switch case, the most useful remaining diagnostic target is now the morph mirror/source-state family:
  - `seq_syncVMorphAmountMirrorsFromLiveSources()`;
  - `seq_selectVoiceMorphAmountFromKit()`;
  - direct `seq_vMorphAmount[]` writes during temp switch;
  - source-state readers outside the background morph worker path.

Current split after this result:

- Morph-kit-load glitch:
  - Still tied most strongly to background morph live apply during/after morph endpoint replacement.
  - The previous worker-live-apply diagnostic removed this glitch, but also broke `.all` live parameter refresh.
- Normal/temp switch glitch:
  - Survives the following eliminations:
    - same-tick continuation;
    - `voiceControl_noteOff(0xFF)`;
    - `seq_applyVoiceSource()` body;
    - VMORPH modulation overlay;
    - background morph worker live apply;
    - `frontParser_applyDeferredVoiceCache()`.
  - Remaining likely suspects:
    - morph amount mirror writes;
    - `seq_voiceSourceState[]` or source-state readers not covered by the worker diagnostic;
    - `seq_setTmpKitActive(...)` side effects other than endpoint push-up and broad cache invalidation;
    - some still-unidentified direct DSP side effect in the switch block.

## Diagnostic: Disable Broad VMorph Mirror Sync

Purpose:

- Test whether the broad mirror refresh in
  `seq_syncVMorphAmountMirrorsFromLiveSources()` is causing the normal/temp
  switch glitch.
- This specifically tests the sweep that copies live image morph amounts into
  `seq_vMorphAmount[]` during temp enter/exit.

Temporary code change:

- `seq_syncVMorphAmountMirrorsFromLiveSources()` now returns immediately.
- This disables the broad mirror refresh used by:
  - `seq_setTmpKitActive(...)` when entering temp;
  - `seq_setTmpKitActive(...)` when leaving temp;
  - `seq_setGlobalMorphAmount(...)`;
  - `seq_resetVoiceMorphAmountsToGlobal(...)`.
- This does **not** disable:
  - normal/temp parameter image selection;
  - morph interpolation storage;
  - `seq_selectVoiceMorphAmountFromKit(...)`;
  - per-voice mirror writes performed while changing an individual voice source.

Expected hardware result:

- If the normal/temp switch glitch disappears, the broad `seq_vMorphAmount[]`
  mirror refresh is implicated.
- If the glitch remains, the broad sync helper is mostly acquitted and the next
  diagnostic should isolate the narrower per-voice mirror write path in
  `seq_selectVoiceMorphAmountFromKit(...)`.

Known diagnostic side effects:

- Global morph menu changes and reset-to-global behavior may leave
  `seq_vMorphAmount[]` mirrors stale during this test.
- Menu/front-panel sync may not reflect the current global/per-voice morph
  mirror values correctly.
- These side effects are acceptable for this narrow audio-glitch test.

Hardware result:

- User reports:
  - normal/temp switch glitch remained;
  - morph-kit-load glitch remained.
- User reverted code while keeping audit notes.

Conclusion:

- `seq_syncVMorphAmountMirrorsFromLiveSources()` is mostly acquitted as the
  direct cause of either glitch.

## New Observation: File Load Glitch While Stopped

User observation:

- Loading a file can produce the same retrigger-like sound even when the
  sequencer is not playing.

Why this matters:

- This broadens the fault beyond `seq_tick()`, pattern switching, and
  same-tick sequencer behavior.
- A stopped sequencer should not be able to trigger voices through normal
  step playback.
- The shared suspect class is now bulk restore/live parameter application into
  the DSP layer.

## Comparison: Successful Morph-Load Diagnostic vs Stopped File Load

Successful previous morph-kit diagnostic:

- The diagnostic that made the morph-kit-load glitch disappear was disabling
  the live DSP apply call inside `seq_serviceMorphInterpolation()`.
- That diagnostic kept the storage/morph work:
  - `kit->interpolatedParams[param] = value`;
  - `seq_updateInterpolatedAutomationTarget(kit, param, value)`.
- It suppressed only:
  - `seq_applyLiveMorphParameterValue(image, synthVoice, param, liveValue)`.
- That call normally reaches:
  - `seq_applySingleParameterValue(param, value)`;
  - `midiParser_ccHandler(...)`;
  - whichever live DSP setter the parameter maps to.

Current stopped file-load path:

- File load begins at `FRONT_SEQ_FILE_BEGIN`:
  - `frontParser_beginFileLoadIngress(1)`;
  - `seq_resetVoiceMorphAmountsToGlobal()`;
  - `seq_resetLiveMorphApplyCache()`;
  - performance/all loads temporarily set `seq_morphLoadDisabled = 1`.
- Incoming normal endpoint parameter bytes store through:
  - `PRF_RESTORE_PARAM_*` -> `seq_storeParameterIngress(...)`;
  - `MIDI_CC` / `FRONT_CC_2` during `seq_voicesLoading` ->
    `seq_storeParameterIngress(...)` plus `midi_midiCache[...]`.
- File load ends at `FRONT_SEQ_FILE_DONE`.
- In the non-deferred path, `FRONT_SEQ_FILE_DONE` calls:
  - `frontParser_applyPendingVoiceCache()`;
  - `frontParser_releaseVoiceCache(voice)`;
  - `frontParser_uncacheVoice(voice)` when temp kit is not active.
- `frontParser_uncacheVoice(voice)` loops the cached voice parameter mask and
  calls:
  - `midiParser_ccHandler(midi_midiCache[presetMask[i]], 0)`.

Important difference:

- Morph endpoint ingress is storage-only until the background morph worker
  applies one live parameter.
- Stopped file load can replay many cached voice parameters immediately through
  `midiParser_ccHandler(...)`, even though there is no sequencer playback.

Shared mechanism:

- Both pathways can reach the same old live DSP setter surface:
  - morph-kit load: `seq_serviceMorphInterpolation()` ->
    `seq_applyLiveMorphParameterValue(...)` ->
    `seq_applySingleParameterValue(...)` -> `midiParser_ccHandler(...)`;
  - stopped file load: `frontParser_uncacheVoice(...)` ->
    `midiParser_ccHandler(...)`.
- Therefore the stopped-load sound is consistent with a live setter side effect,
  not with a note event from the sequencer.

Envelope-position suspicion:

- The DSP envelope-position setters are explicitly not passive:
  - `drumVoice_setEnvelope(..., 0)` triggers `DecayEg_trigger(...)` and
    `slopeEg2_trigger(...)`;
  - `snare_setEnvelope(..., 0)` does the same for snare;
  - `cymbal_setEnvelope(..., 0)` and `hihat_setEnvelope(..., 0)` trigger the
    amp envelope.
- This can sound like a retrigger even if no sequencer note was emitted.
- However, the legacy `frontParser_uncacheVoice()` masks do not obviously map
  one-to-one to the current `CC2_ENVELOPE_POSITION_*` ids:
  - `CC2_ENVELOPE_POSITION_1..6` are `128+93..98`;
  - the legacy voice cache masks currently end at `220`.
- So envelope-position setters remain a strong class-level suspect, but the
  immediate stopped-load cache replay path may also be triggering another
  side-effecting DSP setter.

Why muted voices can still make the sound:

- Sequencer mute suppresses sequencer-triggered playback.
- It does not necessarily prevent direct live DSP parameter setters from moving
  envelopes, transients, oscillator/filter state, modulation targets, or other
  live voice state.
- A file-load restore can therefore create a sound while stopped or muted if a
  setter has an audible side effect.

Next diagnostic:

- Test stopped file-load cache replay directly by making
  `frontParser_releaseVoiceCache(...)` avoid `frontParser_uncacheVoice(...)`
  during this diagnostic.
- The narrowest diagnostic version:
  - when temp kit is active, keep current behavior: clear cache;
  - when temp kit is not active, clear the cache instead of replaying it to DSP.
- Expected result:
  - if the stopped file-load glitch disappears, `frontParser_uncacheVoice(...)`
    live replay is implicated;
  - if the stopped file-load glitch remains, the sound is coming from another
    load-time live path, most likely direct `MIDI_CC` / `FRONT_CC_2` handling
    outside voice-cache replay or the background morph worker resuming after
    `FRONT_SEQ_FILE_DONE`.

Likely functional side effect of this diagnostic:

- Loaded file parameters may be stored in STM endpoint structs but not applied
  to the current live DSP voice state until another path refreshes them.
- This is acceptable for the diagnostic because the only question is whether
  stopped-load sound disappears when cache replay does not hit live DSP setters.

Potential real fix direction:

- Add a passive/bulk-restore apply mode instead of using
  `midiParser_ccHandler(...)` directly for file/morph/temp restore.
- The passive mode should update parameter variables and DSP coefficients but
  suppress functions with trigger-like side effects.
- Candidate side-effect setters to audit:
  - `drumVoice_setEnvelope(...)`;
  - `snare_setEnvelope(...)`;
  - `cymbal_setEnvelope(...)`;
  - `hihat_setEnvelope(...)`;
  - transient setters if any waveform/phase update resets transient state;
  - oscillator phase/start-phase setters if they can create a discontinuity;
  - filter reset or snap envelope setters if reached by restore traffic.

Current working theory:

- The morph-kit-load diagnostic already proved that avoiding live setter calls
  during bulk morph restore can remove at least one version of the glitch.
- The stopped file-load symptom points to the same architectural problem from
  a different route: restore code is still using live/edit setter APIs that can
  behave like performance actions.

## Diagnostic: Disable File-Load Voice Cache Replay

Purpose:

- Test whether stopped file-load glitch comes from cached voice parameter replay
  into live DSP setters.
- This targets the path:
  - `FRONT_SEQ_FILE_DONE`;
  - `frontParser_applyPendingVoiceCache()`;
  - `frontParser_releaseVoiceCache(voice)`;
  - `frontParser_uncacheVoice(voice)`;
  - `midiParser_ccHandler(...)`.

Temporary code change:

- `frontParser_releaseVoiceCache(voice)` now always calls
  `frontParser_clearVoiceCache(voice)`.
- It no longer calls `frontParser_uncacheVoice(voice)` when the temp kit is
  inactive.
- This prevents cached file-load voice params from being replayed to live DSP
  through `midiParser_ccHandler(...)` at file-load completion or unhold release.

What this does not suppress:

- Direct live `MIDI_CC` / `FRONT_CC_2` handling when not in `seq_voicesLoading`.
- Background morph interpolation worker live apply after `FRONT_SEQ_FILE_DONE`.
- Normal endpoint storage in `seq_normalKitState`.
- Temp/normal pattern switching itself.

Expected hardware result:

- If stopped file-load glitch disappears, `frontParser_uncacheVoice(...)` cache
  replay is implicated.
- If stopped file-load glitch remains, cache replay is mostly acquitted and the
  next target should be direct live parameter handling or morph-worker resume
  after file done.
- If normal/temp switch glitch remains but stopped file-load glitch disappears,
  this confirms the switch glitch has a separate live-apply route.

Known diagnostic side effects:

- Loaded voice parameters may be stored in STM normal endpoint state but not
  applied to the current live DSP voices immediately.
- Sound after file load may be stale until another refresh path applies the new
  parameters.
- This is expected and acceptable for the diagnostic.

Hardware result:

- User reports all three glitches are still present:
  - normal/temp pattern switch glitch;
  - morph-kit-load glitch;
  - stopped file-load glitch.
- User reverted code while keeping audit notes.

Conclusion:

- `frontParser_uncacheVoice(...)` cache replay is mostly acquitted as the source
  of the stopped file-load glitch.
- The diagnostic was too shallow to test the DSP boundary itself. It only tested
  one caller that can enter the boundary.
- Remaining live/DSP-boundary entrances include:
  - direct `MIDI_CC` / `FRONT_CC_2` handling outside `seq_voicesLoading`;
  - `seq_applyLiveMorphParameterValue(...)` ->
    `seq_applySingleParameterValue(...)` -> `midiParser_ccHandler(...)`;
  - `seq_applyVoiceSource(...)` / `seq_applyVoiceParameterValues(...)` when
    enabled;
  - `modNode_originalValueChanged(...)` and `modNode_updateValue(...)` paths that
    can re-touch modulation destinations;
  - automation-target restore via `seq_applyNormalEndpointAutomationTargets()`
    and `seq_applyVoiceAutomationTargets(...)`.

Correction to interpretation:

- The previous audit pass identified "live DSP setter traffic" as the broad
  problem class.
- The file-load cache-replay diagnostic tested only whether one file-load path
  was responsible.
- Because the glitches survived, the next diagnostic must be placed lower, at
  or immediately before the shared parameter-apply boundary, not in
  `frontPanelParser` cache release.

## Next Boundary Diagnostic: Suppress Restore-Time `midiParser_ccHandler(...)`

Purpose:

- Test whether the audible glitches are caused by restore/morph/source-switch
  code entering the old live parameter edit API.
- This is the direct DSP-boundary test that the cache-replay diagnostic did not
  perform.

Proposed temporary code shape:

- Add a diagnostic guard around `seq_applySingleParameterValue(...)` so that it
  returns before calling `midiParser_ccHandler(...)`.
- Leave all storage-side work intact:
  - endpoint arrays;
  - morph endpoint arrays;
  - interpolated arrays;
  - automation target storage.
- This suppresses live parameter application from:
  - `seq_serviceMorphInterpolation()` via `seq_applyLiveMorphParameterValue(...)`;
  - any active `seq_applyVoiceParameterValues(...)` source-switch apply path.

Expected readout:

- If morph-kit-load glitch disappears again, this reconfirms the worker/live
  setter boundary as the morph-load offender.
- If normal/temp switch glitch disappears, then source-switch or
  switch-adjacent live parameter application is still entering this path despite
  previous narrower tests.
- If stopped file-load glitch remains, it is coming from direct
  `frontPanelParser` -> `midiParser_ccHandler(...)` ingress or another non-seq
  boundary path.

If stopped file-load remains after that:

- Add a second, broader diagnostic guard directly inside `midiParser_ccHandler()`
  for file-load/restore context.
- The guard should only be used diagnostically at first, because globally
  disabling `midiParser_ccHandler(...)` would also break ordinary live edits.

Potentially cleaner real fix:

- Split the old single "apply/edit parameter" API into at least two modes:
  - live edit/performance apply, where actionful setters are allowed;
  - passive restore/bulk rebuild, where storage and continuous DSP coefficients
    update but trigger-like side effects are suppressed or deferred.
- Long-term this belongs in the future STM-side `/Preset/` API, not scattered
  across `frontPanelParser`, `MidiParser`, and `sequencer`.

## Diagnostic: Disable Sequencer Live Parameter Boundary

Purpose:

- Test the shared sequencer/morph live parameter boundary directly.
- This diagnostic asks whether the glitches are caused by sequencer-side restore
  or morph code calling into the old live MIDI parameter API.

Temporary code change:

- `seq_applySingleParameterValue(...)` now returns immediately.
- It no longer constructs a `MidiMsg`.
- It no longer calls `midiParser_ccHandler(...)`.

What remains active:

- Normal/temp endpoint storage.
- Morph endpoint storage.
- Interpolated parameter storage.
- Automation target storage/update bookkeeping.
- Direct live parameter messages that enter `midiParser_ccHandler(...)` without
  going through `seq_applySingleParameterValue(...)`.

What is suppressed:

- Live DSP parameter application from `seq_serviceMorphInterpolation()` through:
  - `seq_applyLiveMorphParameterValue(...)`;
  - `seq_applySingleParameterValue(...)`.
- Live DSP parameter application from any enabled
  `seq_applyVoiceParameterValues(...)` / source-switch apply path.

Expected hardware result:

- If morph-kit-load glitch disappears, this reconfirms that the morph worker's
  live parameter apply is the morph-load offender.
- If normal/temp switch glitch disappears, a source-switch or switch-adjacent
  path is still entering this boundary.
- If stopped file-load glitch remains, it is not using this sequencer boundary;
  the next diagnostic must guard direct `midiParser_ccHandler(...)` ingress
  during file-load/restore context.

Known diagnostic side effects:

- Morph interpolation will continue updating stored `interpolatedParams[]`, but
  the DSP will not hear those values through this boundary.
- Normal/temp source-switch parameter differences may not audibly apply.
- Loaded parameter data may be stored correctly while the current live DSP state
  remains stale.
- These side effects are expected and acceptable for this boundary test.

Hardware result:

- User reports:
  - no audible morph changes;
  - loaded/restored parameters do not audibly apply except when set discretely
    from the menu;
  - no retrigger glitch.

Conclusion:

- This is the strongest confirmation so far.
- The retrigger-like sound is downstream of
  `seq_applySingleParameterValue(...)` entering `midiParser_ccHandler(...)`.
- The normal/temp switch and morph/load operations are not themselves required
  to produce the glitch; they become problematic when they bulk-apply stored
  parameter images through the old live parameter edit API.
- Direct menu edits still work because they enter the same general setter world
  as isolated, intentional edits rather than as an automated restore/morph sweep.

Important distinction:

- The fix is not to make restore/morph/temp switching literally behave like a
  slow menu edit.
- The fix is to make the restore/morph/temp DSP-facing path use a controlled
  parameter-apply API:
  - storage updates always happen;
  - safe continuous DSP updates can happen;
  - actionful trigger-like setters are suppressed, deferred, or applied only at
    an intentional note trigger;
  - parameter families are reintroduced deliberately until the offending family
    is found.

## Reintroduction Plan From Boundary Diagnostic

Goal:

- Re-enable audible DSP application gradually until the retrigger glitch returns.
- Use each hardware result to identify the parameter family or apply side effect
  that causes the sound.

Proposed next diagnostic structure:

- Replace the unconditional `return` in `seq_applySingleParameterValue(...)`
  with a temporary allowlist.
- Only parameters in the current diagnostic family are allowed to call
  `midiParser_ccHandler(...)`.
- Everything else continues to store/interpolate/update bookkeeping but remains
  muted at the live DSP boundary.

Suggested diagnostic order:

1. Level/pan/output-only family.
   - Volumes, pans, audio routing, possibly drive amount if it is only a
     continuous coefficient update.
   - Expected: should restore audible parameter movement with low trigger risk.

2. Filter/oscillator continuous family.
   - Filter frequency/resonance/drive/type, oscillator pitch/fine/coarse, FM
     amounts/frequencies, waveform selectors.
   - If this glitches, the issue is more likely discontinuity/click from bulk
     coefficient or waveform changes rather than true envelope retrigger.

3. Modulation destination/depth family.
   - LFO destinations, velocity destinations, macro destinations, modulation
     depths.
   - This retests the `modNode_originalValueChanged(...)` /
     `modNode_updateValue(...)` side-effect world as a family rather than one
     small envelope-only bypass.

4. Envelope-shape family, excluding envelope-position.
   - Attack/decay/slope/mod envelope amount parameters.
   - These may be safe continuous shape changes or may touch active envelope
     state enough to click.

5. Transient/snap/phase/start family.
   - Transient waveform/volume/frequency and any oscillator phase/start
     parameters.
   - High suspicion for short "hit" or discontinuity behavior.

6. Envelope-position family.
   - `PAR_ENVELOPE_POSITION_1..6`.
   - Highest suspicion for trigger-like behavior because its setters are
     explicitly actionful.

Interpretation:

- If the glitch returns at one family, split that family into smaller groups or
  individual params.
- If the glitch only returns when several families are combined, the problem may
  be aggregate bulk setter traffic rather than a single parameter.
- If all families work individually but full apply glitches, the fix should be
  staged/incremental live apply, not only a passive setter exclusion.

Likely real implementation shape after diagnosis:

- `seq_applySingleParameterValue(...)` should become a wrapper around a new
  preset/parameter apply API with mode flags, for example:
  - live menu edit;
  - passive restore;
  - morph continuous apply;
  - trigger-time refresh.
- The future `/Preset/` module should own that policy.
- `sequencer.c` should eventually request parameter application by image/voice
  and mode, not manually translate endpoint bytes into MIDI messages.

## Diagnostic: Reintroduce Level/Pan/Output Family

Purpose:

- Start re-enabling the sequencer/morph live parameter boundary one family at a
  time.
- First family is intended to be low trigger-risk:
  - `PAR_VOL1..PAR_VOL6`;
  - `PAR_PAN1..PAR_PAN6`;
  - `PAR_AUDIO_OUT1..PAR_AUDIO_OUT6`.

Temporary code change:

- `seq_applySingleParameterValue(...)` no longer returns unconditionally.
- It now allows only the level/pan/output family to construct a `MidiMsg` and
  call `midiParser_ccHandler(...)`.
- All other parameters still return before the live MIDI parameter API.

Notes:

- Pan is checked in two ranges because the legacy enum places
  `PAR_NRPN_FINE` / `PAR_NRPN_COARSE` between `PAR_PAN3` and `PAR_PAN4`.
- Storage/interpolation/automation bookkeeping remains active for all params.

Expected hardware result:

- If all glitches remain gone, level/pan/output live apply is likely clean.
- If a glitch returns immediately, split this family:
  - volume only;
  - pan only;
  - audio output only.
- If morph becomes partially audible only for kits where volume/pan/output
  differ, that is expected and useful.

Known diagnostic side effects:

- Most morph and restored sound differences will still not apply audibly because
  oscillator, filter, envelope, transient, modulation, and waveform families are
  still blocked at the boundary.

Hardware result:

- User reports:
  - no audible retrigger glitch;
  - only menu-set parameters seem to land;
  - morph/restored parameters are still mostly inaudible.

Conclusion:

- Level/pan/output live apply is provisionally clean.
- This family did not reintroduce the glitch, but also did not make enough of
  the sound parameter image audible to prove much beyond "not immediately
  guilty."

## Diagnostic: Add Continuous Pitch/Filter/FM/Drive Family

Purpose:

- Continue staged reintroduction at the sequencer/morph live parameter boundary.
- Add continuous-ish oscillator, filter, FM, and drive controls while still
  blocking high-risk action/selector families.

Newly allowed through `seq_applySingleParameterValue(...)`:

- Pitch/frequency:
  - `PAR_COARSE1..PAR_FINE6`;
  - `PAR_NOISE_FREQ1..PAR_MIX1`;
  - `PAR_MOD_OSC_F1_CYM..PAR_MOD_OSC_GAIN2`.
- Filter:
  - `PAR_FILTER_FREQ_1..PAR_RESO_6`;
  - `PAR_FILTER_DRIVE_1..PAR_FILTER_DRIVE_6`.
- FM/drive:
  - `PAR_FMAMNT1..PAR_FM_FREQ3`;
  - `PAR_DRIVE1..PAR_HAT_DISTORTION`.

Still blocked:

- Oscillator waveform selectors.
- Filter type selectors.
- LFO/velocity/macro destination and modulation-depth families.
- Envelope shape and envelope-position families.
- Transient waveform/volume/frequency family.
- Morph amount params.

Expected hardware result:

- If the retrigger glitch remains absent, this continuous-control family is
  likely clean.
- If the glitch returns, split this family into:
  - pitch/FM only;
  - filter frequency/resonance/drive only;
  - distortion/drive only;
  - noise/mix/mod-osc only.

Known diagnostic side effects:

- More morph/load differences may become audible, but waveform, envelope,
  transient, and modulation-destination changes should still not apply.

Hardware result:

- User reports:
  - the sound is a bit different;
  - most file-loaded parameters still do not land audibly;
  - menu changes still apply;
  - some morph effect is audible when morph changes;
  - no retrigger glitch on load or normal/temp changeover.

Conclusion:

- The continuous pitch/filter/FM/drive family is provisionally clean.
- This family restores some audible morph behavior without reintroducing the
  retrigger-like sound.

## Diagnostic: Add Envelope Shape/Rate Family

Purpose:

- Add envelope-rate/shape style controls while continuing to block the known
  actionful envelope-position family.
- Keep modulation destination/depth, waveform selector, filter type selector,
  and transient families blocked.

Newly allowed through `seq_applySingleParameterValue(...)`:

- Velocity envelope controls:
  - `PAR_VELOA1..PAR_VELOD6_OPEN`.
- Volume envelope slopes:
  - `PAR_VOL_SLOPE1..PAR_VOL_SLOPE6`.
- Snare/cym repeat:
  - `PAR_REPEAT4..PAR_REPEAT5`.
- Pitch/mod envelope controls:
  - `PAR_MOD_EG1..PAR_MODAMNT4`;
  - `PAR_PITCH_SLOPE1..PAR_PITCH_SLOPE4`.

Still blocked:

- `PAR_ENVELOPE_POSITION_1..6`.
- Oscillator waveform selectors.
- Filter type selectors.
- LFO/velocity/macro destination and modulation-depth families.
- Transient waveform/volume/frequency family.
- Morph amount params.

Expected hardware result:

- If the retrigger glitch remains absent, this envelope-shape/rate family is
  likely clean.
- If the glitch returns, split this family into:
  - velocity attack/decay;
  - volume slope;
  - pitch/mod envelope shape/amount;
  - repeat params.

Known diagnostic side effects:

- Many file/morph differences still may not land because selectors,
  modulation, transient, waveform, and envelope-position families remain
  blocked.

Hardware result:

- User reports:
  - parameter loading is still not fully correct;
  - no retrigger glitch on loading;
  - no retrigger glitch on normal/temp changeover.

Conclusion:

- Envelope shape/rate family is provisionally clean.
- The remaining blocked families are now more likely to contain the offender:
  - waveform/filter selectors;
  - modulation routing/depth;
  - transient controls;
  - envelope-position;
  - VMORPH morph-modulation amount params
    (`PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`).

## Diagnostic: Add Waveform/Filter-Type/Decimation Selectors

Purpose:

- Add selector-style sound engine parameters while still keeping modulation
  routing/depth, transient controls, envelope-position, and VMORPH
  morph-modulation amount params blocked.

Newly allowed through `seq_applySingleParameterValue(...)`:

- Primary oscillator waveform selectors:
  - `PAR_OSC_WAVE_DRUM1..PAR_OSC_WAVE_SNARE`;
  - `PAR_WAVE1_CYM`;
  - `PAR_WAVE1_HH`.
- Mod/noise oscillator waveform selectors:
  - `PAR_MOD_WAVE_DRUM1..PAR_WAVE3_HH`.
- Voice decimation:
  - `PAR_VOICE_DECIMATION1..PAR_VOICE_DECIMATION_ALL`.
- Filter type selectors:
  - `PAR_FILTER_TYPE_1..PAR_FILTER_TYPE_6`.

Notes:

- The primary waveform block is not allowed as one broad range all the way to
  `PAR_WAVE1_HH`, because `PAR_NRPN_DATA_ENTRY_COARSE` sits inside that legacy
  enum area.
- This diagnostic intentionally avoids LFO/velocity/macro destination and depth
  parameters.

Still blocked:

- LFO/velocity/macro destination and modulation-depth families.
- Transient waveform/volume/frequency family.
- `PAR_ENVELOPE_POSITION_1..6`.
- VMORPH morph-modulation amount params
  (`PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`).

Expected hardware result:

- If the retrigger glitch remains absent, waveform/filter-type/decimation
  selectors are provisionally clean.
- If the glitch returns, split this phase into:
  - primary oscillator waveforms;
  - mod/noise waveforms;
  - filter type;
  - voice decimation.

Known diagnostic side effects:

- Modulation routing/depth, transient, envelope-position, and VMORPH
  morph-modulation amount changes still may not land audibly.

Hardware result:

- User reports:
  - parameter loading is getting closer;
  - no retrigger glitch has returned.

Conclusion:

- Waveform/filter-type/decimation selectors are provisionally clean.
- Remaining high-interest blocked families:
  - modulation destination/routing selectors;
  - transient controls;
  - envelope-position;
  - VMORPH morph-modulation amount params.

## Diagnostic: Add Modulation Controls, Excluding Destinations

Purpose:

- Add modulation controls that do not directly select modulation destinations.
- Keep actual routing/destination selectors blocked because they can alter the
  modulation graph and trigger `modNode_*` side effects.

Newly allowed through `seq_applySingleParameterValue(...)`:

- LFO rate/amount:
  - `PAR_FREQ_LFO1..PAR_AMOUNT_LFO6`.
- Mix modulation amounts:
  - `PAR_MIX_MOD_1..PAR_MIX_MOD_3`.
- Volume/velocity modulation amounts and toggles:
  - `PAR_VOLUME_MOD_ON_OFF1..PAR_VOLUME_MOD_ON_OFF6`;
  - `PAR_VELO_MOD_AMT_1..PAR_VELO_MOD_AMT_6`.
- LFO waveform/retrigger/sync/offset:
  - `PAR_WAVE_LFO1..PAR_WAVE_LFO6`;
  - `PAR_RETRIGGER_LFO1..PAR_OFFSET_LFO6`.

Still blocked:

- Velocity destination selectors:
  - `PAR_VEL_DEST_1..PAR_VEL_DEST_6`.
- LFO voice/target routing selectors:
  - `PAR_VOICE_LFO1..PAR_TARGET_LFO6`.
- Macro destinations and amounts.
- Transient waveform/volume/frequency family.
- `PAR_ENVELOPE_POSITION_1..6`.
- VMORPH morph-modulation amount params
  (`PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`).

Expected hardware result:

- If the retrigger glitch remains absent, non-destination modulation controls
  are provisionally clean.
- If the glitch returns, split this phase into:
  - LFO rate/amount;
  - LFO waveform/retrigger/sync/offset;
  - velocity/volume modulation amounts;
  - mix modulation amounts.

Known diagnostic side effects:

- Destination/routing, macro, transient, envelope-position, and VMORPH
  morph-modulation amount changes still may not land audibly.

Hardware result:

- User reports:
  - no retrigger glitch has returned;
  - parameters sound mostly correct after load.

Conclusion:

- Non-destination modulation controls are provisionally clean.
- Remaining blocked families are now narrow:
  - modulation destination/routing selectors;
  - macro destinations and amounts;
  - transient controls;
  - envelope-position;
  - VMORPH morph-modulation amount params.

## Diagnostic: Add Transient Controls

Purpose:

- Add the transient waveform/volume/frequency family.
- Continue blocking modulation routing/destination, macro, envelope-position,
  and VMORPH morph-modulation amount params.

Newly allowed through `seq_applySingleParameterValue(...)`:

- `PAR_TRANS1_VOL..PAR_TRANS6_FREQ`.

Still blocked:

- Velocity destination selectors:
  - `PAR_VEL_DEST_1..PAR_VEL_DEST_6`.
- LFO voice/target routing selectors:
  - `PAR_VOICE_LFO1..PAR_TARGET_LFO6`.
- Macro destinations and amounts:
  - `PAR_MAC1_DST1..PAR_MAC2_DST2_AMT`.
- `PAR_ENVELOPE_POSITION_1..6`.
- VMORPH morph-modulation amount params
  (`PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`).

Expected hardware result:

- If the retrigger glitch remains absent, transient controls are provisionally
  clean.
- If the glitch returns, split this phase into:
  - transient volume;
  - transient waveform;
  - transient frequency.

Known diagnostic side effects:

- Modulation routing/macro, envelope-position, and VMORPH morph-modulation
  amount changes still may not land audibly.

Hardware result:

- User reports:
  - transient controls seem okay;
  - no retrigger glitch has re-emerged.

Conclusion:

- Transient controls are provisionally clean.
- The remaining blocked families are now:
  - modulation destination/routing selectors;
  - macro destinations and amounts;
  - `PAR_ENVELOPE_POSITION_1..6`;
  - VMORPH morph-modulation amount params.

## Functional Fix: Disable Legacy File-Load Voice Unhold Promotion

Reason:

- The couple-second delay on the first normal/temp transfer after file load
  re-emerged during staged parameter reintroduction.
- The old voice hold/unhold mechanism no longer matches the post-morph-move
  temp-background model:
  - STM `SeqKitState` owns loaded normal endpoint state;
  - temp playback can continue from temp storage;
  - switching should select normal/temp state, not replay legacy cached voice
    messages.

Code change:

- `FRONT_SEQ_LOAD_VOICE` still sets `seq_voicesLoading` so incoming file payload
  is treated as load ingress and stored into STM normal endpoint state.
- `frontParser_clearHeldVoiceLoad(...)` now clears the old held voice cache and
  clears the associated `seq_voicesLoading`, `seq_newVoiceAvailable`, and
  deferred-unhold bits.
- `frontParser_releaseVoiceCache(...)` and
  `frontParser_unholdLoadedVoice(...)` now call the clear helper instead of
  promoting/replaying cached params.
- `FRONT_SEQ_UNHOLD_VOICE` no longer breaks out early during deferred perf load;
  it clears held cache state and then allows file-load ingress to close if all
  voices are unheld.
- `frontParser_applyPendingVoiceCache()` clears any pending held voice load
  instead of causing live cache promotion.

Expected hardware result:

- The first switch into/out of temp after file load should no longer pause while
  deferred voice-cache promotion drains.
- Loaded endpoint data should still be stored in normal STM state.
- Any truly required direct kit/voice live-promotion behavior must be rebuilt in
  a later explicit preset-load path.

Hardware result:

- User reports:
  - first normal/temp transfer delay is gone.

Conclusion:

- Legacy file-load voice unhold/promotion was responsible for the re-emerged
  first-switch delay.
- Keep the replacement direct kit/voice load pathway deferred to the future
  preset refactor.

## Side Fix: Global Morph 8-Bit Transport

Problem:

- User reports no audible difference for global morph menu values above `127`;
  the morph endpoint seems effectively reached at `127` or greater.
- STM interpolation expects an 8-bit amount (`0..255`) for stored/global morph.
- The old automation path intentionally maps 7-bit values to 8-bit amounts:
  - `0..126 -> value * 2`;
  - `127 -> 255`.
- The global menu path should not use that 7-bit automation convention.

Root cause:

- AVR `PAR_MORPH` was sending:
  - `SEQ_CC, SEQ_SET_GLOBAL_MORPH, value`.
- `value` can be `0..255`.
- STM `frontParser_parseUartData(...)` treats any byte with bit 7 set as a new
  status byte.
- Therefore raw `128..255` global morph values cannot be safely carried in the
  third byte of the normal 3-byte command.

Code change:

- Added 7-bit-safe commands:
  - AVR: `SEQ_SET_GLOBAL_MORPH_LSB`, `SEQ_SET_GLOBAL_MORPH_MSB`;
  - STM: `FRONT_SEQ_SET_GLOBAL_MORPH_LSB`, `FRONT_SEQ_SET_GLOBAL_MORPH_MSB`.
- AVR now sends global morph as:
  - low 7 bits first;
  - bit 7 second.
- STM latches the low 7 bits and applies the reconstructed `0..255` amount when
  the high-bit message arrives.
- Existing `FRONT_SEQ_SET_GLOBAL_MORPH` remains as a compatibility path for
  direct `0..127` messages.

Expected hardware result:

- Global menu morph should now continue changing across `128..255`.
- Full morph endpoint should occur at `255`, not at `127`.
- Step automation / per-voice automation remains 7-bit and should still reach
  full endpoint at automation value `127`.

## Diagnostic Phase: Re-Enable Modulation Routing Selectors

Reason:

- The staged live-apply allowlist has reached a point where transient controls
  and most ordinary voice/DSP controls appear clean.
- Remaining blocked families before this phase:
  - modulation destination/routing selectors;
  - macro destinations and amounts;
  - `PAR_ENVELOPE_POSITION_1..6`;
  - VMORPH morph-modulation amount params
    (`PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`).
- The lowest-risk next family is selector/routing state because these params
  primarily choose modulation destinations or LFO voice/target routing rather
  than directly setting envelope position or VMORPH morph-modulation amount.

Code change:

- `seq_applySingleParameterValue(...)` now also allows:
  - `PAR_VEL_DEST_1..PAR_VEL_DEST_6`;
  - `PAR_VOICE_LFO1..PAR_VOICE_LFO6`;
  - `PAR_TARGET_LFO1..PAR_TARGET_LFO6`.
- Still blocked:
  - macro destinations/amounts;
  - `PAR_ENVELOPE_POSITION_1..6`;
  - VMORPH morph-modulation amount params
    (`PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`).

Expected hardware result:

- If the retrigger glitch remains absent, modulation routing selectors are
  provisionally clean.
- If the glitch returns, split this phase into:
  - velocity destination selectors;
  - LFO voice selectors;
  - LFO target selectors.

Hardware result:

- User reports:
  - no retrigger glitch has re-emerged.

Conclusion:

- Modulation routing selectors are provisionally clean.
- Remaining blocked families:
  - macro destinations/amounts;
  - `PAR_ENVELOPE_POSITION_1..6`;
  - VMORPH morph-modulation amount params
    (`PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`).

## Diagnostic Phase: Re-Enable Macro Destinations And Amounts

Reason:

- Macro destination/amount params are the next remaining routing-like family.
- This phase tests whether macro destination reattachment or macro amount live
  application causes the retrigger-like glitch.
- Envelope position and VMORPH morph-modulation amount params stay blocked
  because they remain the highest-suspicion actionful paths.

Code change:

- `seq_applySingleParameterValue(...)` now also allows:
  - `PAR_MAC1_DST1..PAR_MAC2_DST2_AMT`.
- Still blocked:
  - `PAR_ENVELOPE_POSITION_1..6`;
  - VMORPH morph-modulation amount params
    (`PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`).

Expected hardware result:

- If the retrigger glitch remains absent, macro destination/amount params are
  provisionally clean.
- If the glitch returns, split this phase into:
  - macro destination selectors only;
  - macro amount params only.

Hardware result:

- User reports:
  - no retrigger glitch has re-emerged.

Conclusion:

- Macro destination/amount params are provisionally clean.
- Remaining blocked families:
  - `PAR_ENVELOPE_POSITION_1..6`;
  - VMORPH morph-modulation amount params
    (`PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`).

## Diagnostic Phase: Re-Enable VMORPH Morph-Modulation Amount Params

Reason:

- VMORPH morph-modulation amount params are the next remaining family after
  macro destination/amount params tested clean.
- This family is distinct from:
  - global `PAR_MORPH`;
  - stored normal/temp global morph position;
  - stored normal/temp per-voice morph position;
  - normal/morph endpoint parameter images.
- This phase checks whether allowing the VMORPH parameter family through the
  generic live apply path reintroduces the retrigger-like glitch.

Code change:

- `seq_applySingleParameterValue(...)` now also allows:
  - `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`.
- Still blocked:
  - `PAR_ENVELOPE_POSITION_1..6`.

Expected hardware result:

- If the retrigger glitch remains absent, VMORPH morph-modulation amount params
  are provisionally clean as a glitch trigger.
- If VMORPH changes still do not audibly land through this path, treat that as
  a separate API-routing problem rather than proof of glitch culpability.

Hardware result:

- User reports:
  - no retrigger glitch has re-emerged.
  - A couple-second front-panel delay returned in this sequence:
    - load `.all`;
    - load morph kit;
    - copy/paste pattern to temp;
    - switch to temp.

Conclusion:

- VMORPH morph-modulation amount params are provisionally clean as a retrigger
  source.
- `PAR_ENVELOPE_POSITION_1..6` is now the only family still blocked from the
  staged generic parameter update path.
- The returned delay points back at old voice hold/unhold/cache bookkeeping,
  especially after morph-side payloads are loaded.

## Functional Fix: Stop Populating Legacy Load Voice Release Caches

Reason:

- Earlier work changed release/unhold to clear legacy held voice state instead
  of replaying it.
- However, file-load voice payloads still populated the old release caches:
  - `midi_midiCache[]` / `midi_midiCacheAvailable[]`;
  - `midi_midiLfoCache[]` / `midi_midiLfoCacheAvailable[]`;
  - `midi_midiVeloCache[]` / `midi_midiVeloCacheAvailable[]`.
- `.all` loads send both morph and non-morph voice payloads under
  `SEQ_LOAD_VOICE`.
- Post-morph-move, those payloads should update STM endpoint storage only.
  They should not create a delayed live release/promotion workload.

Code change:

- During `seq_voicesLoading`, `MIDI_CC` and `FRONT_CC_2` still call
  `seq_storeParameterIngress(...)`, but no longer populate `midi_midiCache[]`.
- During `seq_voicesLoading`, LFO and velocity destination sideband messages
  still store endpoint automation targets, but no longer populate the old LFO
  or velocity release caches.
- `frontParser_clearHeldVoiceLoad(...)` now clears both hi-hat bits
  (`0x60`) from `frontParser_deferredPerfUnholdPending`, matching its existing
  `seq_voicesLoading` and `seq_newVoiceAvailable` cleanup.

Expected hardware result:

- The `.all` -> morph kit -> copy temp -> switch temp sequence should no longer
  have a delayed cache promotion/finalization burst on the first temp switch.
- Loaded endpoint data should remain correct because the STM-side
  `SeqKitState` storage path is unchanged.

Follow-up after hardware retest:

- User reported the first normal/temp switch delay returned after morph load.
- The cache-population removal was still present, but
  `FRONT_SEQ_FILE_DONE` still had a deferred-performance branch that set
  `frontParser_deferredPerfVoiceCachePending = 1`.
- That flag is consumed by `frontParser_applyDeferredVoiceCache()` on the next
  manual pattern/temp boundary, so it can still create a first-switch cleanup
  burst even when no old voice payload cache remains.

Additional code change:

- The deferred-performance `FRONT_SEQ_FILE_DONE` branch now calls
  `frontParser_clearDeferredPerfLoad()` instead of scheduling
  `frontParser_deferredPerfVoiceCachePending`.

Expected hardware result:

- First normal/temp switch after `.all` + morph load should no longer drain any
  deferred voice-cache finalizer work.

Follow-up for temp-active `.all` load:

- User reports a remaining first-switch delay when:
  - playback is already on temp;
  - `.all` is loaded;
  - user switches back to a normal pattern.
- Important clarification: this is not endpoint restore. Subsequent
  normal/temp switches still perform parameter restore correctly without the
  delay.
- Therefore the first-switch-only delay remains a deferred cleanup path.

Additional code change:

- In the manual pattern switch path, compute whether the pending switch crosses
  a normal/temp boundary before calling `frontParser_applyDeferredVoiceCache()`.
- Skip `frontParser_applyDeferredVoiceCache()` when the pending manual switch is
  a normal/temp boundary.
- Keep endpoint restore behavior unchanged.

Expected hardware result:

- First switch from temp back to normal after `.all` load should no longer
  drain deferred PRF/cache cleanup at the boundary.
- Parameter restore should still happen on every normal/temp transition.

Hardware result:

- User reports the same first-switch lockup remains in this path:
  - load `.all`;
  - copy to temp;
  - switch to temp;
  - load `.all`;
  - switch to normal.

Follow-up interpretation:

- Either the normal/temp boundary guard did not match the STM's actual
  active/pending state after this load sequence, or the remaining first-switch
  work is not in `frontParser_applyDeferredVoiceCache()`.
- Since the post-morph temp model does not require legacy deferred voice-cache
  finalization on pattern boundaries, the next contained test removes the
  boundary call entirely.

Additional code change:

- Removed the `frontParser_applyDeferredVoiceCache()` call from the manual
  pattern-switch path rather than conditionally guarding it.

Expected hardware result:

- If the first-switch lockup disappears, the culprit was still hidden behind
  the legacy deferred-cache finalizer, but the boundary detection was
  insufficient.
- If the lockup remains, the next suspect is first live application of newly
  loaded normal parameter state or another file-load flag consumed by the first
  normal pattern tick.

Hardware result:

- User reports the same first-switch lockup remains even after removing
  `frontParser_applyDeferredVoiceCache()` from the manual pattern-switch path.
- This acquits the manual-switch deferred-cache finalizer for the remaining
  freeze, at least as a direct switch-time cause.

## Functional Fix: Remove Trigger-Time Legacy Voice Cache Replay

Remaining first-only pathway:

- `PATCH_RESET`, load/unhold bookkeeping, and some file-load paths can leave
  `seq_newVoiceAvailable` set after a file load.
- `seq_tick()` sets `seq_newPatternExecuted = 1` when a pattern/temp switch is
  executed.
- The first subsequent `seq_triggerVoice(...)` checks:
  - `seq_newVoiceAvailable & voiceBit`;
  - `seq_loadFastMode || seq_newPatternExecuted`.
- If both are true, it still called `frontParser_uncacheVoice(voice)` before
  clearing the bit.

Why this matches the remaining freeze:

- It is consumed only once after the file load/switch, matching the "first
  switch only" symptom.
- It can run after the switch, at the first triggered voice, so removing the
  switch-time finalizer would not affect it.
- The post-morph-move parameter path no longer needs this legacy cache replay:
  file-load payloads now update STM `SeqKitState` endpoint/interpolated storage
  through the explicit parameter ingress path, and the old
  `midi_midiCache[]` release path is deprecated.

Code change:

- `seq_triggerVoice(...)` no longer calls `frontParser_uncacheVoice(voiceNr)`
  from the `seq_newVoiceAvailable` one-shot branch.
- The branch now only clears the stale availability bit.
- Hi-hat voices clear both shared hi-hat bits (`0x60`) to avoid leaving one
  half of the paired voice source armed after the other half triggers.
- `seq_newPatternExecuted` is still cleared once all availability bits have
  been consumed.

Expected hardware result:

- The sequence:
  - load `.all`;
  - copy to temp;
  - switch to temp;
  - load `.all`;
  - switch to normal;
  should no longer freeze the front panel on the first triggered voice after
  the switch.
- Normal/temp endpoint restore behavior is unchanged.
- Loaded parameters should remain audible because parameter application now
  flows through the STM `SeqKitState`/generic parameter API path, not through
  trigger-time `frontParser_uncacheVoice(...)` replay.

Hardware result:

- User reports the first-switch front-panel freeze still remains.
- Therefore the remaining freeze is not caused by STM trigger-time
  `frontParser_uncacheVoice(...)` replay.

## Functional Fix: Remove AVR First-Pattern-Ack Held Voice Replay

Remaining first-only pathway:

- AVR `front/LxrAvr/frontPanelParser.c` handles STM `SEQ_CHANGE_PAT` acks.
- Before this fix, every pattern-change ack checked `preset_workingVoiceArray`.
- If nonzero, the AVR looped voices and called:
  - `preset_readDrumVoice(i, 0)`;
  - `preset_readDrumVoice(i, 1)`.
- `preset_readDrumVoice(...)` is not a passive local menu update. It sends a
  complete voice-param transaction to STM:
  - `SEQ_LOAD_VOICE`;
  - `CC_VELO_TARGET`;
  - `CC_LFO_TARGET`;
  - all voice params through the existing front-panel flow-control path;
  - `SEQ_UNHOLD_VOICE`;
  - flow end.

Why this matches the remaining freeze:

- It is AVR-side hold/load behavior, not STM endpoint restore.
- It runs on the first `SEQ_CHANGE_PAT` ack after a `.all`/performance load if
  `preset_workingVoiceArray` is still nonzero.
- It then clears `preset_workingVoiceArray`, so subsequent normal/temp switches
  do not repeat the work.
- The observed path:
  - load `.all`;
  - copy to temp;
  - switch to temp;
  - load `.all` while on temp;
  - switch to normal;
  can leave `preset_workingVoiceArray` set until the first post-load pattern
  ack, causing exactly one visible front-panel stall.

Code change:

- On `SEQ_CHANGE_PAT` ack, AVR no longer replays
  `preset_readDrumVoice(...)` from `preset_workingVoiceArray`.
- The ack handler now simply clears `preset_workingVoiceArray`.
- This keeps the first and subsequent normal/temp switches structurally the
  same from the AVR side.

Expected hardware result:

- The first temp-to-normal switch after `.all` load should no longer freeze the
  front panel for the duration of hidden AVR voice-param replay.
- Loaded sound state should remain correct because `.all` already sends its
  parameter payload during the file load, and STM owns the normal/temp kit
  images post-morph-move.

## Envelope Position Restore Policy

Current finding:

- `PAR_ENVELOPE_POSITION_1..6` is the last family still blocked from the staged
  bulk parameter update path.
- These params are not ordinary static synth settings in practice:
  - `ParameterArray.c` maps them to `midi_envPosition[0..5]`;
  - `MidiParser.c` writes `midi_envPosition[]` and immediately calls
    `drumVoice_setEnvelope(...)`, `snare_setEnvelope(...)`,
    `cymbal_setEnvelope(...)`, or `hihat_setEnvelope(...)`;
  - `modulationNode.c` can also call those envelope setters when a modulation
    destination points at an envelope-position param and `midi_envPosition[]`
    is nonzero.

What is missed if bulk restore keeps skipping them:

- Bulk file/temp/morph source switching will not force `midi_envPosition[]` to
  match the stored normal/temp image.
- A later modulation-node action that reads `midi_envPosition[]` could use the
  previous live/menu envelope position until the next explicit envelope
  position edit or modulation event updates it.
- The actual stored endpoint bytes in `SeqKitState.frontPanelParams[]` and
  `SeqKitState.morphParams[]` are still preserved; this only affects the
  compatibility mirror/action path.

Recommendation:

- Keep `PAR_ENVELOPE_POSITION_1..6` blocked from generic bulk live apply for
  now.
- Do not call envelope setters during normal/temp source switching or file-load
  endpoint restore.
- If the mirror must be made coherent later, add an explicit passive mirror
  update helper for `midi_envPosition[]` that does not call the DSP envelope
  setters.
- A small `envelopes_dirty`/`envPositionMirrorDirty` side array is reasonable
  as a refactor step if any reader needs to know that the mirror is stale.
  The dirty flag should be cleared by the next explicit envelope-position menu,
  MIDI, automation, or passive mirror update event.

## Functional Fix: Push Global Morph Back To AVR On Temp/Normal Restore

Problem:

- Audio switching between normal/temp parameter sets is correct, but AVR menu
  `PAR_MORPH` can remain stale after switching between normal and temp.
- Existing endpoint restore pushes:
  - kit/front endpoint params;
  - morph endpoint params.
- Global morph is not part of `END_OF_SOUND_PARAMETERS`; it is stored in
  `SeqKitState.globalMorphAmount`, so the endpoint restore loop never sends it.

Code change:

- STM endpoint restore now sends the selected kit image's global morph value
  after the front endpoint segment.
- It uses two 7-bit-safe `FRONT_SEQ_CC` messages:
  - `FRONT_SEQ_SET_GLOBAL_MORPH_LSB`;
  - `FRONT_SEQ_SET_GLOBAL_MORPH_MSB`.
- AVR front parser latches the low 7 bits and reconstructs
  `parameter_values[PAR_MORPH]` when the high-bit message arrives.
- Because the update is received inside the existing restore transaction,
  `frontParser_restoreActive` suppresses outbound echo back to STM.

Expected hardware result:

- Switching into or out of temp should update the global morph menu value to
  the selected normal/temp image's stored global morph value.
- No audio behavior should change.

Hardware result:

- User reports firmware freezes at boot with `SD card OK` on screen.

Status:

- Reverted this code attempt.
- Do not reuse the split `FRONT_SEQ_SET_GLOBAL_MORPH_LSB/MSB` push during the
  endpoint restore transaction without re-auditing AVR boot/restore parser
  interactions.
