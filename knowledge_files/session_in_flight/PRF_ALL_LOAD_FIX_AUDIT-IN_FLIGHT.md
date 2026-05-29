# `.PRF` / `.ALL` LOAD FIX AUDIT - IN FLIGHT

Date: 2026-05-29  
Status: consolidated current-state audit from `FILEFIX_AUDIT.md`, the transcript, and user-provided checkpoint notes.

## Purpose

This document replaces the active planning value of `knowledge_files/session_in_flight/FILEFIX_AUDIT.md` for the current in-flight work.

The original `FILEFIX_AUDIT.md` investigated version/layout and short-read defects in `.ALL`, `.PRF`, and kit loading. That work should be considered completed at the checkpointed commit identified by the user as `90d3f08`, with the limitations below. The active WIP is no longer basic file-layout compatibility; it is repairing `.PRF` background-load isolation so the temporary slot is fully isolated from the currently playing parameters.

## Current Functional Baseline

At checkpoint `90d3f08`:

- `.ALL` files load their parameters correctly.
- `.PRF` files load their parameters correctly if background loading into the temp slot is turned off.
- These loads are correct only when the parameter set does not include morph automation.
- This is known to be incorrect final behavior, but it is a working baseline.

The current work-in-progress is to repair background `.PRF` loading so the temporary slot and temporary parameters are isolated from the normal playing pattern/parameters.

## Historical File-Loader Failures Kept For Reference

These failures were historically important and should stay documented even though the filefix work is considered completed at the checkpoint.

### Legacy `.ALL` / `.PRF` Layout Gap

Older v2 `.ALL` / `.PRF` files were accepted because their version was `<= FILE_VERSION`, but readers used newer v4/v5 offsets. Observed v2 file size delta:

```text
51514 - 50946 = 568 = 512 morph bytes + 56 scale bytes
```

The old format had no morph block and no scale block at the tail. Reading those files with v4/v5 offsets could seek into the wrong section or EOF and transmit stale/default data.

### Legacy `.PRF` Kit Offset Risk

Legacy `.PRF` kit-like data appeared to begin at offset `73`, matching legacy `.ALL` alignment, while the old perf constant used offset `74`. That created a one-byte misalignment risk.

### Short-Read Fragility

Several readers historically checked `res` but not `bytesRead`, so an EOF/short read could leave stale buffers:

- pattern scale,
- pattern length,
- pattern chain,
- main step,
- step data.

`preset_readShuffle()` also checked stale `res` rather than the result from its `f_read()`.

### Global Read Undefined Behavior

`preset_readGlobalData()` used `bytesRead` before initialization, making short `GLO.CFG` / `GLO2.CFG` behavior nondeterministic.

### Kit Extension Rigidity

Kit file open behavior originally assumed `.snd` only. `.kit` fallback compatibility was noted as useful if actual cards contain that naming.

## Completed Checkpoint Meaning

The filefix checkpoint means:

- file layout/profile and short-read fixes should not be reopened unless a specific failing file proves they are still wrong;
- `.ALL` / `.PRF` basic load correctness is no longer the main blocker;
- communications and temp-slot isolation issues must not be misdiagnosed as stale file-layout bugs without evidence.

Known limitations at that checkpoint:

- morph automation is still outside the known-safe `.ALL` / `.PRF` parameter-load condition;
- `.PRF` background loading into a temporary slot is disabled or not yet properly isolated;
- the behavior is serviceable but not the intended final feature.

## Current Temp Pattern / Temp Parameter WIP

## Diff-Derived Code Inventory: `90d3f08` -> Current Worktree

This section is based on a source diff of:

- `LXR-custom-develop-patload-envmod-90d3f08/front` -> current `front`
- `LXR-custom-develop-patload-envmod-90d3f08/mainboard` -> current `mainboard`

Generated outputs and build artifacts were ignored. The changed source files were:

- AVR: `IO/uart.c`, `Menu/copyClearTools.c`, `Menu/menu.c`, `Preset/presetManager.c`, `buttonHandler.c`, `config.h`, `encoder.c`, `encoder.h`, `frontPanelParser.c`, `frontPanelParser.h`, `ledHandler.c`, `main.c`
- STM32: `DSPAudio/modulationNode.c`, `MIDI/MidiMessages.h`, `MIDI/MidiParser.c`, `MIDI/MidiVoiceControl.c`, `MIDI/Uart.c`, `MIDI/Uart.h`, `MIDI/frontPanelParser.c`, `MIDI/frontPanelParser.h`, `Sequencer/EuklidGenerator.c`, `Sequencer/sequencer.c`, `Sequencer/sequencer.h`
- Current-only backup files: `front/LxrAvr/config.h.bak`, `front/LxrAvr/encoder.c.bak`, `front/LxrAvr/encoder.h.bak`. These are not firmware source and should not be committed as canonical code unless intentionally preserved.

### SEQ16 Temporary Pattern Selector

SEQ16 has been temporarily reassigned as a `SELECT` button for the temporary sequence slot.

Implemented and hardware-verified by the user:

- selecting and playing the temporary pattern from SEQ16,
- viewing/editing the temporary pattern,
- copying from a normal pattern to the temp pattern,
- pasting/copying involving the temp pattern,
- routing pattern 8 / `SEQ_TMP_PATTERN` through STM pattern accessors for playback, editing, copy/clear, pattern length/scale/rotation, and Euclidean operations.

Important constraint:

- The temp pattern is observable/copyable/playable, but it is not intended to be loadable or saveable as a normal pattern.
- The existing file-load staging path still uses `seq_tmpPattern`. A previous attempt to split a new loader buffer broke `.ALL` / `.PRF` pattern-data loading and was reverted.
- Do not touch the loading paths just to support temp pattern observation/copy/paste.

Code details:

- AVR `frontPanelParser.h` defines `SEQ_CHANGE_TMP_PAT` and `SEQ_TMP_PATTERN 8`.
- STM32 `MidiMessages.h` mirrors `FRONT_SEQ_CHANGE_TMP_PAT` and `SEQ_TMP_PATTERN`.
- AVR `buttonHandler.c` adds `TMP_PATTERN_SEQ_BUTTON 15`, mapping physical SEQ16 to temp-pattern behavior in performance mode.
- `buttonHandler_changeTmpPattern()` sends `SEQ_CHANGE_TMP_PAT` either for all tracks (`0x78`) or for each held voice button, matching the normal per-track pattern-change encoding style.
- `buttonHandler_selectTmpPatternForEdit()` makes SEQ16 selectable for viewing/editing under shift, sets `menu_setShownPattern(SEQ_TMP_PATTERN)`, clears/blinks LEDs, and queries STM for pattern, step, LED, and Euclidean state.
- `buttonHandler_copyPatternButton()` makes copy/paste use `SEQ_TMP_PATTERN` as a legal source or destination when copy mode is active and SEQ16 is released.
- AVR message packing for pattern-aware operations now uses `patternNr & 0x0f` instead of `patternNr & 0x07` in the relevant LED, step, and query paths. That preserves pattern value `8` instead of truncating it to `0`.
- AVR `frontPanelParser.c` treats `SEQ_CHANGE_PAT` ACK data `8` as temp pattern instead of masking it to a normal `0..7` pattern, and maps its LED to `LED_STEP16`.
- `ledHandler.c` maps played/viewed temp pattern to `LED_STEP16` for select LEDs and blink LEDs.
- `Menu/menu.c` maps the viewed temp pattern to `LED_STEP16` when showing rotation-related blink state.
- `Menu/copyClearTools.c` and copy-related code allow pattern number `8` to pass through copy/paste operations instead of assuming only normal pattern slots.
- STM32 `frontPanelParser.c` handles `FRONT_SEQ_CHANGE_TMP_PAT` by calling `seq_setNextPattern(SEQ_TMP_PATTERN, voiceMaskOrAllTracks)`.
- STM32 pattern edit paths that receive step volume/prob/note, Euclidean length/steps/rotation/substep rotation, clear track, clear pattern, copy pattern, copy track pattern, and pattern parameter queries use `frontParser_shownPattern == SEQ_TMP_PATTERN` to route to the temp pattern instead of the normal pattern array.
- `EuklidGenerator.c` was changed from direct `seq_patternSet` access to the new sequencer accessors so Euclidean edits can operate on either normal patterns or `seq_tmpPattern`.
- `sequencer.c` / `sequencer.h` add accessors such as `seq_normalizePatternNumber()`, `seq_getStepPtr()`, `seq_getLengthRotatePtr()`, `seq_getPatternSettingPtr()`, `seq_getMainSteps()`, and `seq_setMainSteps()`. Normal patterns still index `seq_patternSet`; pattern `8` indexes `seq_tmpPattern`.
- `seq_init()` now initializes `seq_tmpPattern.seq_patternSettings` and clears `SEQ_TMP_PATTERN`.
- `seq_determineNextPattern()` allows temp pattern to self-repeat if both active and next pattern are `SEQ_TMP_PATTERN`.
- `seq_nextStep()` normalizes `seq_pendingPattern` through `seq_normalizePatternNumber()` and calls `seq_setTmpKitActive(seq_activePattern == SEQ_TMP_PATTERN)` after a pattern switch.
- `seq_copyPattern()` was changed so copying from a normal pattern to `SEQ_TMP_PATTERN` also triggers temp kit capture. It does not make the temp pattern persistent or file-loadable.

### STM Temporary Audio Parameter Storage

Temporary audio parameter space was added on the STM side only. No temporary data should live on the AVR side.

The intended STM-side shape is:

- one front-panel parameter set,
- one morph parameter set,
- one interpolated/current-play parameter set,
- one automation target set.

Current practical behavior:

- temp pattern copy also captures current voice parameters into the temp parameter cache;
- automation target state is captured beside the temp parameters;
- switching into the temp pattern applies the temp parameter/automation cache;
- switching back to a normal pattern applies the normal parameter/automation image.

The current path uses the STM-owned raw parameter image and validity masks, not `parameterArray` and not the modulated internal voice structs.

Code details:

- `sequencer.c` adds `SeqTmpKitAutomation`, containing LFO, velocity, and macro destination arrays plus bitmasks indicating which destinations are valid.
- `sequencer.c` adds `SeqTmpKitState`, containing `frontPanelParams`, `morphParams`, `interpolatedParams`, matching validity arrays, automation target state, and a `valid` flag.
- `seq_tmpKitState` stores the temp-slot parameter/audio cache. `seq_tmpKitActive` records whether the temp kit cache is currently applied.
- `seq_normalKitAutomation` and `seq_normalKitAutomationValid` snapshot the normal automation targets before entering the temp kit, so automation can be restored when leaving temp.
- `seq_captureTmpKitState()` copies the current STM raw parameter image and automation image into `seq_tmpKitState.interpolatedParams` and `seq_tmpKitState.automation`.
- `seq_applyParameterValues()` replays valid cached raw parameter bytes through `midiParser_ccHandler(msg, 0)`. This applies values to STM audio state without updating `midiParser_originalCcValues`.
- `seq_applyAutomationTargets()` applies cached LFO, velocity, and macro destinations by calling `modNode_setDestination()` and `modNode_updateValue()` on the relevant modulation nodes.
- `seq_setTmpKitActive(1)` applies temp parameters and temp automation when entering `SEQ_TMP_PATTERN`. `seq_setTmpKitActive(0)` reapplies the normal STM parameter image and normal automation snapshot when leaving it.
- Current code also calls `seq_pushParameterValuesToFront()` on both temp entry and normal return. That is the known broken pushback path described below.

### STM Canonical Raw Parameter Image

A new STM-side canonical raw parameter image was added:

```c
uint8_t stm_currentParamImage[END_OF_SOUND_PARAMETERS];
```

An automation image and validity masks were also added so unknown/unseen parameters are not replayed as zeros:

- `stm_currentParamImageValid[]`
- temp cache validity arrays
- validity for LFO, velocity, and macro destination data

This fixed the immediate corruption where selecting the temp pattern replayed holes in the image as real zero writes.

Code details:

- `stm_currentParamImage[END_OF_SOUND_PARAMETERS]` stores the last raw parameter byte accepted at STM ingress.
- `stm_currentParamImageValid[]` marks which slots are known. This is critical: without it, unset slots default to `0` and replaying the whole array corrupts parameters such as decimation.
- `stm_currentAutomationImage` stores the current raw automation destination image.
- `seq_storeParameterIngress(param, value)` writes a raw parameter byte and marks it valid.
- `seq_storeLfoDestinationIngress()`, `seq_storeVelocityDestinationIngress()`, and `seq_storeMacroDestinationIngress()` store raw automation destinations and set validity bits.
- `SeqParamIngressTarget` and `seq_paramIngressTarget` create a future switch between feeding the normal current image and feeding temp kit state. The current code leaves the switch in `SEQ_PARAM_INGRESS_CURRENT_IMAGE`; nothing should flip it yet.
- `seq_init()` clears the temp kit state, normal automation snapshot, current parameter image, validity masks, and automation image.

### Ingress Hooks

The current image is updated at STM parameter ingress points, including:

- normal `MIDI_CC` handling,
- `FRONT_CC_2` / CC2 handling,
- cached voice-load branches,
- LFO target ingress,
- velocity target ingress,
- macro target ingress.

There is also an internal switch shape for future routing:

- current mode: ingress feeds `stm_currentParamImage` / normal automation image;
- alternate mode exists conceptually/code-wise: ingress could feed temp kit state / temp automation;
- nothing should currently flip that switch.

Only copy-to-temp should feed temp parameters for now.

Refactor note:

This implementation adds real-time hooks into STM parameter receive paths that did not exist before. They are legitimate for the current WIP, but they should be called out as potential future refactor targets. A later cleanup should centralize "canonical parameter image update" instead of scattering raw-image writes across parser and MIDI branches.

Code details:

- `MidiParser.c` calls `seq_storeParameterIngress()` when `midiParser_ccHandler()` receives normal CC parameters with `updateOriginalValue` set.
- `MidiParser.c` also calls `seq_storeParameterIngress()` for `FRONT_CC_2`/above-127 parameters with `updateOriginalValue` set.
- The MIDI CC mapping path that resolves external MIDI CCs into LXR parameter numbers also stores into the STM raw image when it changes `midiParser_originalCcValues`.
- `frontPanelParser.c` stores LFO target, velocity target, and macro target messages into the automation ingress image when those messages arrive from the AVR/front-panel stream.
- Cached voice-load branches that apply parameters while `seq_voicesLoading` is active also update the raw image so file loads populate the canonical STM-side mirror.

Refactor target:

- These ingress writes currently live in several receive/apply branches across `MidiParser.c` and `frontPanelParser.c`. A later cleanup should centralize raw-image maintenance in one helper layer so future parameter ingress paths cannot forget to update the mirror.

### STM-to-AVR Restore / Pushback Messages

The current diff adds restore-style parameter messages intended to push STM parameter state back to the AVR without repainting the menu for every parameter:

- `PRF_RESTORE_PARAM_CC`
- `PRF_RESTORE_PARAM_CC2`
- `PRF_RESTORE_MORPH_CC`
- `PRF_RESTORE_MORPH_CC2`
- `PARAM_RESTORE_DONE`

Code details:

- AVR `frontPanelParser.h` and STM32 `MidiMessages.h` define the restore-message status bytes.
- STM32 `seq_pushSingleParameterToFront()` sends one valid parameter with `uart_sendFrontpanelPriorityByteWait()`, using the normal restore status for `<128` parameters and the CC2 restore status for `>=128`.
- STM32 `seq_pushParameterValuesToFront()` iterates the valid parameter image and then sends `PARAM_RESTORE_DONE, 0, 0`.
- AVR `frontPanelParser.c` handles `PRF_RESTORE_PARAM_CC` and `PRF_RESTORE_PARAM_CC2` by updating `parameter_values`; it also mirrors sound parameters into `parameters2`.
- AVR `frontPanelParser.c` handles morph restore messages by updating `parameters2`.
- AVR repaints once on `PARAM_RESTORE_DONE`.

Current status:

- This code is the known bad edge. It was added to make the menu/display follow temp entry/exit, but hardware testing showed that switching back to normal does not reliably push the correct normal state, and the pushback path may feed stale/corrupt values back down.
- 2026-05-29 update: STM-to-AVR temp-transition pushback has now been temporarily disabled on the STM side. The restore-message sender remains in place, but `seq_setTmpKitActive()` now routes both temp-entry and normal-return pushes through `seq_maybePushParameterValuesToFront()`, gated by `seq_tmpKitPushParamsToFrontEnabled = 0`.
- The intended hardware result for the next test is: entering/leaving the temp pattern changes only STM-side playing parameters; AVR menu parameters should not change, and the sound should remain stable as it did before the broken pushback path was added.
- Later repair should flip or replace this guard only after the source of the bad normal-return push is understood.

### PRF Cache State-Machine Experiment

The current diff also contains an additional `.PRF` cache state machine in `mainboard/LxrStm32/src/MIDI/frontPanelParser.c` and protocol definitions on both MCUs.

Code details:

- `PrfCacheState` adds states for idle, receiving AVR live snapshot, live active, receiving pending load, pending valid, and aborting.
- STM32 stores live snapshots in arrays/structs such as `frontParser_prfCacheLiveParams`, `frontParser_prfCacheLiveMorph`, `frontParser_prfCacheLivePattern`, `frontParser_prfCacheLiveMidiChannels`, `frontParser_prfCacheLiveNoteOverride`, and live sequencer flags.
- `frontParser_capturePrfStmLiveSnapshot()` captures the protected active pattern, per-track pattern state, step indexes, MIDI channels, note overrides, voice morph amounts, voice-loading flags, `seq_newVoiceAvailable`, `seq_tracksLocked`, and `seq_loadFastMode`.
- `frontParser_prfPending*Count` counters track whether expected main-step, step-data, length, scale, chain, and protected-pattern writes arrived during a pending `.PRF` load.
- `frontParser_prfCacheUseLivePattern()` and related getters make the sequencer and MIDI parser read from the live snapshot while a cache session is active.
- `MidiParser.c` was changed to use `frontParser_prfCacheLiveMidiChannel()` and `frontParser_prfCacheLiveNoteOverrideValue()` instead of reading `midi_MidiChannels[]` and `midi_NoteOverride[]` directly. That lets MIDI input/output keep using live protected settings while a pending `.PRF` cache exists.
- `Sequencer/sequencer.c` was changed so trigger paths, active-step checks, MIDI channel lookup, note override lookup, pattern-setting lookup, and voice morph reads can consult the live cache when active.
- AVR `frontPanelParser.h` defines control commands `SEQ_PRF_CACHE_BEGIN`, `SEQ_PRF_PENDING_BEGIN`, `SEQ_PRF_PENDING_DONE`, `SEQ_PRF_CACHE_ABORT`, `SEQ_PRF_AVR_SNAPSHOT_BEGIN`, `SEQ_PRF_AVR_SNAPSHOT_END`, and `SEQ_PRF_RESTORE_AVR_LIVE`.
- AVR `frontPanelParser.c` adds `frontPanel_prfCacheBegin()` / `frontPanel_prfCacheControl()` and parses `PRF_CACHE_STATUS` even while RX is disabled.
- Current `presetManager.c` sends `SEQ_FILE_BEGIN` / `SEQ_FILE_DONE` for `.ALL` as well as `.PRF`, tracks whether begin/done was sent, and forces `SEQ_FILE_DONE` on early close if a begin was sent.

Current status:

- This state-machine work is not the current validated strategy. The user-reported working path is SEQ16 temp-pattern selection/copy/play plus STM-side temp parameter cache.
- Treat the PRF cache state-machine code as WIP/suspect until it is either removed, reconciled with the temp-slot plan, or proven by hardware.
- `frontParser_deferPerfLoadCacheUntilPatternChange` is now `0` in the current diff, which matches the user note that background `.PRF` loading into temp is turned off for the working checkpoint.

### Encoder Changes Present In This Diff

The 90d3f08 -> current folder diff also includes completed encoder work, even though it is not part of the `.PRF` load fix.

Code details:

- AVR `config.h` adds `ENC_USE_STABLE_DRIVER`, `ENC_DEBOUNCE_TICKS`, and `ENC_CONFIRM_COUNT`.
- AVR `encoder.h` renames the primary API to `encode_stableRead1()`, `encode_stableRead2()`, and `encode_stableRead4()`, while retaining deprecated inline wrappers for the old `encode_read*()` names.
- AVR `main.c` now calls `encode_stableRead4()`.
- AVR `encoder.c` changes Timer 0 to CTC with prescaler 64 and OCR0A for about 250 us. In legacy mode, Timer 0 still polls quadrature and debounces the button.
- When `ENC_USE_STABLE_DRIVER == 1`, Timer 1 runs as a free-running timestamp source and PCINT1 handles PC0/PC1 encoder edges with a time gate plus consecutive-direction confirmation.
- Current `ENC_USE_STABLE_DRIVER` is `0`, so the build remains on the polling driver by default, but with the renamed read API and faster Timer 0 period.

## Current Broken Behavior

STM-to-AVR parameter pushback on temp-pattern transitions exists in code, but it is temporarily disabled because it does not work correctly.

Intended behavior:

- switching into the temp pattern applies temp cached voice parameters and pushes those values up to AVR so the menu/display reflects the temp state;
- switching back to normal patterns restores normal parameters and pushes normal values up to AVR so the menu/display reflects the normal state;
- the user can always see the parameter set corresponding to the currently playing/viewed normal or temporary pattern state.

Observed/current status from the transcript and user note:

- pushing to AVR was attempted with restore-style parameter messages and a final repaint;
- it appeared to work in some temp-entry cases;
- switching back to normal did not reliably push normal parameters;
- the pushback path is suspected of corrupting or feeding stale values back down;
- the user requested a temporary disable so the STM-side temp parameter cache can be tested without AVR menu pushback changing displayed parameters or feeding bad values back down;
- that temporary disable is now implemented with the sender left in place for later repair.

Latest message exchange reflected in code:

- User request: temporarily disable pushing STM-side parameters back up to AVR when entering/leaving the temporary pattern/parameter cache; leave the code in place so it can be fixed and re-enabled later.
- Code response: keep `seq_pushSingleParameterToFront()` and `seq_pushParameterValuesToFront()` intact, but redirect `seq_setTmpKitActive()` through `seq_maybePushParameterValuesToFront()`, which returns immediately while `seq_tmpKitPushParamsToFrontEnabled` is `0`.

## Immediate Continuation Plan

### 1. Stabilize By Disabling Pushback

Status: implemented in `mainboard/LxrStm32/src/Sequencer/sequencer.c`.

STM-to-AVR parameter pushback on temp-pattern transitions is temporarily disabled. Kept:

- SEQ16 temp pattern selection,
- temp pattern copy/paste,
- temp parameter/automation cache capture,
- STM-side apply when switching into temp,
- STM-side restore when switching back to normal.

The guard is intentionally narrow:

- `seq_tmpKitPushParamsToFrontEnabled` defaults to `0`.
- `seq_maybePushParameterValuesToFront()` is called on both temp entry and normal return.
- The original pushback sender still exists for later debugging and re-enable.
- No AVR-side restore-message handling was removed.

This isolates whether remaining corruption is from parameter application or from the AVR pushback/restore-message path.

### 2. Re-test The Minimal Path

Hardware test:

1. Power on.
2. Load `P000.ALL`.
3. In `MODE_PERF`, copy pattern 0 to temp with copy + SELECT1 + SEQ16.
4. Play normal pattern.
5. Stop.
6. Press SEQ16 to select/play temp.
7. Switch back to normal pattern.

Expected after pushback is disabled:

- audio parameters remain stable when entering and leaving temp;
- AVR menu parameters should not change when entering/leaving temp; that is the expected result for this isolation test.

### 3. Fix Pushback Narrowly

Once the STM-side apply/restore path is stable without pushback, debug pushback separately:

- verify the exact messages sent for valid parameters only;
- verify AVR parser accepts restore-style messages while in the relevant mode;
- verify final repaint happens once and after all parameter bytes are parsed;
- verify normal-return source is the correct normal parameter image, not stale temp/current state;
- avoid large unmetered bursts if AVR RX cannot drain them reliably.

Do not combine this with `.PRF` loader changes, morph automation, or new comms Phase 7 timeout work.

### 4. Reconnect To `.PRF` Background Load

Only after temp selection/copy/cache/pushback is correct:

- use the temp pattern/parameter cache as the future `.PRF` background-load target;
- keep background `.PRF` loading fully isolated from playing normal pattern/parameters;
- add the user-facing global option later to choose between cache-while-playing behavior and `.ALL`-style immediate apply.

## Do Not Repeat

- Do not use `midiParser_originalCcValues` as the truth for the current STM voice parameters. It is a lossy/stale bookkeeping image, not the loaded/current parameter truth.
- Do not use `parameterArray` as the copy source for temp raw parameters. It points into live converted/modulated DSP state and cannot reconstruct all original menu bytes.
- Do not split file-load staging away from `seq_tmpPattern` without tracing the entire load path. A previous split broke pattern data loading from `.ALL` / `.PRF`.
- Do not store temp parameter data on the AVR.
- Do not make the temp pattern loadable/saveable unless explicitly requested.
- Do not merge the pushback bug with broad communications hardening.

## Bottom Line

The `.ALL` / `.PRF` filefix work is considered completed at checkpoint `90d3f08` under the stated constraints. The live project is now a WIP to make `.PRF` background loading into a temporary sequence/parameter slot fully isolated. SEQ16 temp pattern selection/copy/play works, and STM-side temp parameter capture/apply mostly works after adding a canonical raw parameter image and validity masks. The broken edge is STM-to-AVR parameter pushback on temp transitions; fixing or temporarily disabling that is the next immediate task.

## 2026-05-29 Pushback Failure Theory After Isolation Test

User hardware observation after temporarily disabling STM-to-AVR parameter pushback:

- switching back and forth to the temporary pattern now changes sound as expected;
- AVR menu parameters do not change while switching into or out of the temporary pattern;
- this is the intended result for the isolation test, because the temporary guard prevents menu/display synchronization but leaves STM-side temp parameter application and normal restoration active.

This strongly narrows the bug. The STM-side temp parameter cache, temp pattern select/play path, and normal audio restoration path are not obviously corrupting sound on their own under this test. The bad behavior is therefore most likely in the disabled push-up path, in the AVR interpretation of pushed-up values, or in a feedback path created after the AVR receives the pushed-up values.

### Current Disabled Push Path

The sender is still present in `mainboard/LxrStm32/src/Sequencer/sequencer.c`.

Current structure:

- `seq_pushSingleParameterToFront(param, value)` sends one restore-style triplet to the AVR:
  - `PRF_RESTORE_PARAM_CC`, `param`, `value` for parameters below 128;
  - `PRF_RESTORE_PARAM_CC2`, `param - 128`, `value` for parameters 128 and above.
- `seq_pushParameterValuesToFront(source, valid)` walks `0..END_OF_SOUND_PARAMETERS - 1`, skips invalid entries when a valid mask is present, sends each valid parameter, then sends `PARAM_RESTORE_DONE, 0, 0`.
- `seq_maybePushParameterValuesToFront(source, valid)` is now the narrow disable point. Because `seq_tmpKitPushParamsToFrontEnabled` defaults to `0`, this wrapper returns immediately.
- `seq_setTmpKitActive(1)` still applies temp parameters with `seq_applyParameterValues(seq_tmpKitState.interpolatedParams, seq_tmpKitState.interpolatedParamsValid)` and applies temp automation, but the AVR push is suppressed.
- `seq_setTmpKitActive(0)` still applies `stm_currentParamImage` and restores the saved normal automation image, but the AVR push is suppressed.

Important detail: `seq_applyParameterValues()` calls `midiParser_ccHandler(msg, 0)`. The `0` matters. In the main STM MIDI parser, `seq_storeParameterIngress()` is only called when `updateOriginalValue` is true. So the direct STM application of cached temp parameters should update DSP/current sound state without rewriting the canonical raw ingress image. That matches the hardware observation: with AVR pushback disabled, switching into and out of temp sounds sane.

### Most Likely Failure Mechanism: AVR Feedback Poisons The STM Normal Image

The leading theory is a feedback loop:

1. STM enters temp pattern.
2. STM applies `seq_tmpKitState.interpolatedParams` locally.
3. When pushback is enabled, STM also sends those temp values to AVR as `PRF_RESTORE_PARAM_CC` / `PRF_RESTORE_PARAM_CC2`.
4. AVR receives those restore messages and writes them into `parameter_values`.
5. AVR also currently writes the same normal restore values into `parameters2` for sound parameters.
6. Later, some AVR-side path treats the updated `parameter_values` as normal live user/menu state and sends values back down to STM as ordinary `MIDI_CC` / `CC_2`.
7. STM receives those ordinary AVR-to-STM parameter messages with normal ingress semantics. In `frontPanelParser.c`, `MIDI_CC` / `FRONT_CC_2` eventually call `midiParser_ccHandler(..., 1)` or directly call `seq_storeParameterIngress()` during load/cache paths.
8. Because `updateOriginalValue` is true on normal AVR-originating parameter messages, STM rewrites `stm_currentParamImage` with values that came from the temp cache.
9. When switching out of temp, `seq_setTmpKitActive(0)` reads `stm_currentParamImage` as the normal-return source. If that image has been poisoned by AVR retransmit, the normal restore is no longer normal.

This explains why temp entry could appear partially correct while normal return failed. The return source is not a frozen "normal before temp" snapshot; it is a live image that can be changed while temp is active.

This is not proven yet, but it is the highest-value theory to test first because it fits the code and the user's isolation result.

### Question: Do We Need To Disable Re-transmit Back Down From AVR While Push-up Is In Progress?

Probably yes, or at minimum we need to prove that no re-transmit occurs.

The AVR has many legitimate paths that send parameter values to STM:

- `menu_encoderChangeParameter()` sends `MIDI_CC` / `CC_2` for normal sound parameters after editing.
- `menu_buttonChangeParameter()` also sends `MIDI_CC` / `CC_2`.
- `menu_parseGlobalParam()` sends many sequencer/global messages.
- `preset_morph()` sends interpolated sound parameters back down whenever morph is applied.
- special parameter dtypes can send target-selection messages before or alongside normal CC messages.

The restore-message handler itself only writes arrays and calls `menu_repaintAll()` at `PARAM_RESTORE_DONE`. That repaint does not obviously send all sound parameters by itself. However, the bug does not require repaint alone to send parameters. It only requires some later AVR action, mode transition, morph refresh, menu edit, or cache restore path to reinterpret the pushed-up display values as authoritative normal values and send them back down.

Implementation implication: when STM pushes a temporary display image to AVR, AVR should know that it is receiving display/cache state, not user-authored normal sound state. During that window, any AVR-side automatic retransmit of restored values should be suppressed or explicitly tagged so STM does not record it into `stm_currentParamImage`.

The first implementation should be conservative:

- add an AVR `restoreInProgress` flag set on the first restore message and cleared on `PARAM_RESTORE_DONE`;
- suppress outbound normal parameter sends caused by restore/display refresh while that flag is active;
- optionally add a short "restore just completed" guard if testing shows a deferred repaint/menu action sends after `PARAM_RESTORE_DONE`;
- avoid suppressing deliberate user edits outside the restore window.

### Question: Are We Reading From The Correct Structures When Pushing Up?

For temp entry, probably yes:

- `seq_tmpKitState.interpolatedParams` is the temp parameter cache captured beside the temp pattern slot.
- `seq_tmpKitState.interpolatedParamsValid` is the corresponding valid mask.
- This is the right source if the desired AVR display is "the currently active temp parameter image."

For normal return, probably not robust enough:

- current code pushes and applies `stm_currentParamImage` / `stm_currentParamImageValid`;
- that image is live and can be updated by front-panel ingress, file-load ingress, external MIDI, or a bad AVR feedback loop while temp is active;
- only automation has a frozen normal snapshot today: `seq_normalKitAutomation` and `seq_normalKitAutomationValid`;
- there is no equivalent frozen `seq_normalKitParams` snapshot captured on temp entry.

The safer model is symmetrical:

- on temp entry, copy the current normal raw parameter image and valid mask into a dedicated normal snapshot;
- apply temp parameters from `seq_tmpKitState`;
- on normal return, apply and push the frozen normal snapshot, not the live `stm_currentParamImage`;
- after returning to normal, resume normal ingress updates into `stm_currentParamImage`.

This would prevent temp-time AVR feedback or external MIDI from changing what "return to normal" means. Later we can decide whether user edits made while temp is active should edit the temp cache, be blocked, or be routed into the normal snapshot, but the current WIP should first establish strict isolation.

### Question: Is The AVR-side Interpreter Storing Pushed-up Parameters Correctly?

Probably not for this use case.

Current AVR behavior in `front/LxrAvr/frontPanelParser.c`:

- `PARAM_CC` writes both `parameter_values[param]` and `parameters2[param]`, then repaints immediately.
- `PARAM_CC2` writes both `parameter_values[param + 128]` and `parameters2[param + 128]`, then repaints immediately.
- `PRF_RESTORE_PARAM_CC` writes both `parameter_values[param]` and `parameters2[param]`.
- `PRF_RESTORE_PARAM_CC2` writes `parameter_values[param]` and, for sound parameters, also writes `parameters2[param]`.
- `PRF_RESTORE_MORPH_CC` / `PRF_RESTORE_MORPH_CC2` write only `parameters2`.
- `PARAM_RESTORE_DONE` calls `menu_repaintAll()`.

For a true preset-load restore, mirroring normal values into `parameters2` may have been intended as a way to keep morph endpoints coherent after load. For temp-pattern display switching, it is risky. A normal restore push is not the same thing as a morph restore push. If the STM sends "show this current normal/temp value" and AVR stores that value into both `parameter_values` and `parameters2`, then the AVR has collapsed the normal and morph endpoints to the same value for that parameter.

That matters because `preset_morph()` computes:

`interpolate(parameter_values[paramNumber], parameters2[paramNumber], morph)`

If temp display pushback overwrites `parameters2`, later AVR morph operations may produce wrong values even if no morph automation exists in the file. This could look like morph being mysteriously involved, when the real bug is that the display restore handler corrupted the AVR's morph endpoint array.

The likely fix is to split the semantics:

- normal/display restore messages should update `parameter_values` only, or possibly a dedicated display overlay if AVR memory allows;
- morph restore messages should update `parameters2` only;
- no generic temp display push should overwrite both arrays unless the operation explicitly means "replace the whole kit and reset morph endpoint."

This should be tested carefully because older `.PRF` / `.ALL` code may rely on the old mirroring behavior after a real load.

### Question: Is Morph Becoming Re-enabled?

There is no direct evidence yet that morph is being re-enabled in the current P000.ALL test. With pushback disabled, the user's result is stable and sane.

The morph risk is more indirect:

- `P000.ALL` is expected to have no morph automation.
- STM has `seq_morphLoadDisabled` and related guards around PRF/load paths.
- AVR `preset_morph()` can still send a full set of interpolated values whenever `PAR_MORPH` or per-voice morph controls are applied.
- AVR morph interpolation always uses `parameter_values` and `parameters2`.
- The current restore handler writes normal pushed-up values into both arrays, which can corrupt or erase the morph endpoint state.

So the first question is not only "did morph turn on?" It is also "did pushback make the AVR's morph math wrong even while morph amount stayed zero?"

Concrete facts to gather during the next test phases:

- confirm `PAR_MORPH` remains `0` or expected neutral value for `P000.ALL`;
- confirm no `PRF_RESTORE_MORPH_CC` / `PRF_RESTORE_MORPH_CC2` messages are sent during temp entry/exit for this file;
- confirm STM `seq_vMorphFlag` stays clear during temp entry/exit;
- confirm AVR `preset_morph()` is not called as a side effect of restore/repaint;
- if morph messages are seen, identify whether they come from file load, menu global parse, macro/performance controls, or restore handling.

### Additional Questions Exposed By The Code

Parameter coverage:

- `seq_pushParameterValuesToFront()` currently walks `END_OF_SOUND_PARAMETERS`, not all `NUM_PARAMS`.
- That is probably right for voice/kit sound parameters, but any temp-visible global/performance parameters outside that range will not be synchronized by this push.
- If the menu page shows globals while temp is active, the menu may display a mixed state.

Validity-mask coverage:

- the push sender skips invalid parameters;
- if `stm_currentParamImageValid` is incomplete on normal return, AVR may retain temp values for parameters not included in the normal valid set;
- this could make the menu look partly temp and partly normal even if audio is correct.

Burst behavior:

- `seq_pushParameterValuesToFront()` sends a potentially large stream of priority bytes, then a single done marker;
- the AVR parser batches restore messages reasonably by avoiding repaint until done, but there is no explicit "restore transaction begin";
- without a begin marker, AVR infers the transaction only from receiving restore messages and cannot prepare/suppress behavior before the first value lands;
- if bytes are dropped, delayed, or interleaved, `PARAM_RESTORE_DONE` may repaint a partial state.

CC offset asymmetry:

- STM normal MIDI CC handling has an offset for parameters below 128 because front params and Cortex params are not numbered identically (`frontParser_midiMsg.data1 += 1` before normal handling).
- The restore push path sends raw `param` for `PRF_RESTORE_PARAM_CC`, and AVR stores it directly as `parameter_values[param]`.
- That may be correct because restore messages are custom front-panel messages, not ordinary MIDI CC, but it must be verified with known test parameters below 128.

Special dtype side effects:

- AVR dtype handlers for LFO targets, velocity targets, macro targets, and global params send specialized messages.
- Restore handlers currently bypass dtype validation and special send behavior, which is good for display-only restore.
- If any later repaint or mode transition routes restored values through dtype handlers, the restored values can produce extra outbound messages.

### Testable Implementation Plan

Goal: re-enable STM-to-AVR parameter display sync one narrow layer at a time. Each phase should leave a hardware-observable result before moving to the next.

#### Phase 0 - Keep The Known-good Isolation Baseline

Code state:

- leave `seq_tmpKitPushParamsToFrontEnabled = 0`;
- keep the push sender compiled but disabled.

Hardware test:

- load `P000.ALL`;
- copy normal pattern/params to temp;
- switch into temp and back to normal several times.

Expected:

- sound changes/restores as the user currently hears it;
- AVR menu parameters do not change during temp switching.

Purpose:

- preserve a known-good checkpoint before touching pushback again.

#### Phase 1 - Add Observability Without Changing AVR State

Implementation:

- add counters or temporary debug messages around `seq_pushParameterValuesToFront()` without enabling actual AVR array writes;
- count how many parameters would be sent for temp entry and normal return;
- optionally count low-CC vs CC2 parameters and first/last parameter IDs.

Hardware test:

- repeat temp entry/exit;
- verify debug counts are stable and plausible;
- verify sound/menu behavior remains identical to Phase 0.

Questions answered:

- Is the valid mask complete enough?
- Are temp and normal return trying to push the same number of parameters?
- Are any unexpected parameter ranges included or missing?

#### Phase 2 - Push One Known Parameter Only

Implementation:

- temporarily restrict pushback to one harmless, visible sound parameter with a known different temp value;
- leave full push disabled;
- AVR should update `parameter_values` for that one restore message only;
- do not write `parameters2` for this test unless testing morph storage specifically.

Hardware test:

- switch into temp;
- verify only the chosen menu parameter changes;
- verify sound does not unexpectedly change from AVR retransmit;
- switch back to normal and verify that one menu parameter returns.

Questions answered:

- Are restore message IDs and parameter numbering correct?
- Is below-128 / above-127 restore storage aligned correctly?
- Does a restore value by itself trigger a retransmit back down?

#### Phase 3 - Add AVR Restore Transaction Guard

Implementation:

- add an AVR-side restore transaction flag:
  - set it on first `PRF_RESTORE_PARAM_*` / `PRF_RESTORE_MORPH_*`;
  - clear it on `PARAM_RESTORE_DONE`;
  - repaint only after done;
- suppress automatic outbound parameter sends while restore is active;
- if testing shows post-done delayed sends, add a short "restore just completed" guard around repaint-driven paths.

Likely files:

- `front/LxrAvr/frontPanelParser.c` for restore transaction state;
- `front/LxrAvr/Menu/menu.c` only if suppression must be visible to menu send paths.

Hardware test:

- repeat one-parameter push;
- then use controls normally after restore completes.

Expected:

- no sound corruption during restore;
- deliberate user edits after restore still transmit normally.

Questions answered:

- Does AVR retransmit during or immediately after push-up?
- Is suppressing restore-time outbound traffic enough to prevent STM image poisoning?

#### Phase 4 - Fix AVR Restore Storage Semantics

Implementation:

- change normal restore handling so `PRF_RESTORE_PARAM_CC` / `PRF_RESTORE_PARAM_CC2` update only `parameter_values`;
- keep `PRF_RESTORE_MORPH_CC` / `PRF_RESTORE_MORPH_CC2` updating only `parameters2`;
- leave older `PARAM_CC` / `PARAM_CC2` behavior alone until separately audited, unless testing proves they are part of this failure.

Hardware test:

- repeat one-parameter push;
- test a parameter with a different morph endpoint if available;
- verify `P000.ALL` remains non-morphing;
- verify no unexpected `preset_morph()` side effects.

Questions answered:

- Was writing normal restore values into `parameters2` causing morph-related bad parameters?
- Can display sync be separated from morph endpoint mutation?

#### Phase 5 - Add A Frozen Normal Parameter Snapshot On STM

Implementation:

- add `seq_normalKitParams[END_OF_SOUND_PARAMETERS]`;
- add `seq_normalKitParamsValid[END_OF_SOUND_PARAMETERS]`;
- add `seq_normalKitParamsValidFlag`;
- on temp entry, copy `stm_currentParamImage` and `stm_currentParamImageValid` into this frozen normal snapshot, just as automation is already copied to `seq_normalKitAutomation`;
- on normal return, apply and push the frozen normal snapshot instead of the live `stm_currentParamImage`;
- after normal return completes, clear the temp-active state but keep normal ingress behavior unchanged.

Likely file:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`.

Hardware test:

- enable one-parameter push first;
- switch temp in/out repeatedly;
- then enable a small fixed subset;
- finally enable the full valid set.

Expected:

- normal return uses the same values that were present when temp was entered;
- AVR retransmit or external MIDI during temp cannot redefine the normal-return image unless deliberately supported later.

Questions answered:

- Was the normal-return source live/stale/poisoned?
- Does snapshot symmetry with automation fix the return path?

#### Phase 6 - Re-enable Full Temp-entry Push Only

Implementation:

- enable full valid-mask push on temp entry;
- keep normal-return push disabled or limited to the known test subset;
- keep AVR restore guard and corrected storage semantics active.

Hardware test:

- switch into temp;
- inspect multiple menu pages;
- verify audio stays temp-correct;
- switch back to normal and verify audio returns even if menu is not fully restored yet.

Questions answered:

- Can AVR accept the full temp display image without corrupting sound?
- Is the large burst itself safe?

#### Phase 7 - Re-enable Full Normal-return Push From Frozen Snapshot

Implementation:

- enable full valid-mask push on normal return from `seq_normalKitParams`, not `stm_currentParamImage`;
- keep temp-entry push enabled.

Hardware test:

- switch temp in/out repeatedly;
- inspect menu values before temp, in temp, and after return;
- edit a parameter after returning to normal and verify it still sends correctly.

Expected:

- menu tracks the currently active normal/temp state;
- sound tracks the currently active normal/temp state;
- no drift accumulates after repeated switches.

Questions answered:

- Is full bidirectional UI sync stable?
- Are any parameters missing from valid masks or wrong ranges?

#### Phase 8 - Morph-specific Test Pass

Implementation:

- no broad new feature work;
- add temporary instrumentation only if needed.

Hardware tests:

- with `P000.ALL`, verify morph remains neutral:
  - `PAR_MORPH` does not change unexpectedly;
  - no morph restore messages are sent;
  - no `preset_morph()` side effect is observed during restore;
  - STM `seq_vMorphFlag` stays clear.
- with a deliberately morphing file later, verify normal restore and morph restore update separate AVR arrays correctly.

Questions answered:

- Is morph genuinely inactive for the no-morph case?
- Does the new storage split preserve expected morph behavior when morph is intentionally used?

#### Phase 9 - Reconnect To `.PRF` Background Loading

Only after Phases 0-8 pass:

- route `.PRF` background load into the temp pattern/parameter cache;
- keep direct background writes away from playing normal parameters;
- keep STM-to-AVR display push as a temp-slot transition feature, not as part of file receive itself;
- re-test `.ALL` immediate load, `.PRF` foreground load, and `.PRF` temp/background load separately.

### Current Recommendation

The next code session should not simply flip `seq_tmpKitPushParamsToFrontEnabled` back to `1`. The safer path is:

1. keep pushback disabled as the baseline;
2. add a frozen normal parameter snapshot on STM;
3. add AVR restore transaction guarding;
4. stop normal restore messages from writing `parameters2`;
5. re-enable one parameter, then a subset, then the full valid set.

That sequence gives a hardware test after every change and separates the three likely bug classes: bad source structure, AVR retransmit feedback, and AVR morph-array corruption.
