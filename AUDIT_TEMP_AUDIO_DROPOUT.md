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
   - Audit parameters outside `seq_voiceParamMask`, especially `PAR_AUDIO_OUT1..6`, macro slots, kit version, `PAR_UNUSED01`, and morph amount params.
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

Next fix direction:

- Keep endpoint restore at every temp boundary.
- Move or drain deferred file-load cache work before musical changeover when it is safe, especially when playback is already isolated on temp.
- If it cannot be moved entirely, make deferred cache application incremental so the first switch after file load does not perform all pending work in one foreground burst.
