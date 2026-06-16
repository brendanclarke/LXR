# Normal / Temp Copy Per-Track Audit

Session: 022
Status: implemented in code, pending review

## Goal

Audit and plan the STM-side changes needed so the copy-to-temp path copies the pattern data that is actually playing on each track, not just the single selected pattern.

This is a narrow STM-side storage audit. It focuses on how the live playing pattern layout is read, assembled, and written into `seq_tmpPattern` before any broader background-load work is attempted.

The temp-copy behavior should be implemented as a dedicated helper named `seq_copyToTmpPattern(uint8_t srcPattern)` rather than by overloading `seq_copyPattern()`.

## What Exists Today

The current copy-to-temp flow is a single-pattern copy:

- the AVR copy button eventually sends a `SEQ_COPY_PATTERN` command with the temp slot as the destination;
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c` handles `FRONT_SEQ_COPY_PATTERN`;
- that case currently calls `seq_copyPattern(src, dst)`;
- `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.c` copies one whole source pattern into one whole destination pattern using `seq_getStepPtr()`, `seq_getMainSteps()`, and `seq_getLengthRotatePtr()`;
- when the destination is `SEQ_TMP_PATTERN`, `seq_copyPattern()` also calls `preset_captureTmpKitState()`.

That current path assumes one source pattern number is enough.

The live playback model is different:

- `seq_activePattern` is the overall active pattern number;
- `seq_perTrackActivePattern[NUM_TRACKS]` records which pattern each track is actually reading from during playback;
- the sequencer updates those per-track values when a pattern switch is committed.

So the audit target is the gap between those two models.

## Scope For This Change

The first change should only solve this:

- when copying to temp from the live playing state, capture the per-track pattern data from the currently active track sources;
- keep the existing single-pattern copy behavior available for the normal selected-pattern copy path;
- do not treat pending pattern state as part of the capture decision;
- do not introduce the background-load routing, temp-switch forcing, or LED policy yet.

## Files To Inspect And Likely Change

### STM ownership files

- `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.c`
- `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.h`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/Preset/KitState.c`
- `mainboard/LxrStm32/src/Preset/KitState.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

### Call-site context

- `front/LxrAvr/buttonHandler.c`
- `front/LxrAvr/Preset/presetManager.c`

## Variables To Examine

These are the runtime values that decide what should be copied into temp:

- `seq_activePattern`
- `seq_perTrackActivePattern[NUM_TRACKS]`
- `seq_tmpPattern`
- `seq_patternSet`
- `seq_running`
- `frontParser_shownPattern`

The key distinction is:

- `seq_activePattern` tells you the currently selected overall pattern;
- `seq_perTrackActivePattern[]` tells you which pattern each track is actually sourcing from at the moment;
- `frontParser_shownPattern` is only the UI-visible pattern context and should not be mistaken for live playback state.

## Functions That Will Need Attention

### 1. `seq_copyPattern(uint8_t src, uint8_t dst)`

This is the current whole-pattern copy path in `PatternData.c`.

It currently:

- loops over all tracks;
- copies all `Step` entries for each track;
- copies the main-step bitfield for each track;
- copies the track `LengthRotate` value for each track;
- calls `preset_captureTmpKitState()` if the destination is `SEQ_TMP_PATTERN`.

For the new behavior, this function should remain the selected-pattern copy helper.

It should not be overloaded to mean "copy the currently playing arrangement" unless the audit proves that a split helper is impossible. The safer plan is to keep this function as the direct single-source copy path and add a second helper for live playback capture.

The planned split is:

- `seq_copyPattern()` remains the selected-pattern copy path;
- `seq_copyToTmpPattern(srcPattern)` becomes the only helper that snapshots the currently playing per-track arrangement into temp storage, while still falling back to the selected-pattern copy path when playback is stopped.

### 2. `seq_copyTrackPattern(uint8_t srcNr, uint8_t dstPat, uint8_t srcPat)`

This helper already copies one track from one pattern source into one destination pattern.

It is the best primitive for rebuilding `seq_tmpPattern` from multiple live sources, because the new temp-capture path needs to copy:

- track 0 from its currently active source pattern;
- track 1 from its currently active source pattern;
- track 2 from its currently active source pattern;
- track 3 from its currently active source pattern;
- track 4 from its currently active source pattern;
- track 5 from its currently active source pattern;
- track 6 from its currently active source pattern.

The audit should decide whether to:

- reuse this helper directly in a new temp-capture loop, or
- add a more explicit helper that copies one track from a live source into `seq_tmpPattern` and also makes the pattern-level metadata step clearer.

For this change, the new temp-copy helper should read only from the actual source pattern that is playing on that track when playback is running, and it should not look at pending switch targets.

### 3. `seq_applyTmpPatternTo(uint8_t pattern)` and `seq_activateTmpPattern()`

These are the inverse direction: they copy the temporary pattern image back into a selected pattern.

They should stay separate from the new live capture helper, but they matter because they define the expected temp structure layout:

- `seq_tmpPattern.seq_subStepPattern[track][step]`
- `seq_tmpPattern.seq_mainSteps[track]`
- `seq_tmpPattern.seq_patternSettings`
- `seq_tmpPattern.seq_patternLengthRotate[track]`

The new capture helper must fully populate those same fields.

### 4. `frontPanelReceivingProtocol.c` copy handling

`FRONT_SEQ_COPY_PATTERN` currently routes into `seq_copyPattern(src, dst)`.

If the copy-to-temp command path needs to snapshot the live playing arrangement instead of the selected pattern, this call site is where `seq_copyToTmpPattern()` would be selected for the temp destination case.

The audit should determine whether the decision belongs in:

- the front-panel receive layer, because it knows the command semantics, or
- `PatternData.c`, because it owns the copy primitives and can choose the right source set internally.

### 5. `sequencer.c` pattern-switch state commit

`sequencer.c` is where the live per-track pattern source state becomes authoritative after a switch.

The audit should inspect the code that updates:

- `seq_activePattern`
- `seq_perTrackActivePattern[]`

so the new temp-copy helper reads the same source state the sequencer itself uses when deciding what is audible.

## Planned Read Path For The Live Temp Copy

The live capture helper should read pattern data in this order:

1. Read the overall active pattern context from `seq_activePattern`.
2. Read the per-track live source pattern number from `seq_perTrackActivePattern[track]` for each track `0` through `6`.
3. For each track, read the source pattern payload using the existing pattern accessors:
   - `seq_getStepPtr(sourcePattern, track, step)`
   - `seq_getMainSteps(sourcePattern, track)`
   - `seq_getLengthRotatePtr(sourcePattern, track)`
4. Read the active pattern settings with `seq_getPatternSettingPtr(seq_activePattern)` unless the audit proves that temp playback needs a different pattern-settings source.
5. Write those pieces into the temp storage image.

The important point is that the step data and per-track pattern metadata come from the live per-track source, not from one single pattern number.

## Planned Temp Reassembly

The new helper should rebuild `seq_tmpPattern` as a full synthetic snapshot of the currently playing arrangement.

That means:

- `seq_tmpPattern.seq_subStepPattern[track][step]` gets copied from the source pattern selected by `seq_perTrackActivePattern[track]`;
- `seq_tmpPattern.seq_mainSteps[track]` gets copied from the same source pattern;
- `seq_tmpPattern.seq_patternLengthRotate[track]` gets copied from the same source pattern;
- `seq_tmpPattern.seq_patternSettings` gets copied once for the overall temp snapshot;
- `seq_tmpPattern` is only considered valid after all tracks and metadata have been written.

The audit should explicitly check whether the pattern-settings copy should preserve:

- the active pattern's `changeBar` and `nextPattern` values verbatim, or
- a temp-specific adjustment such as keeping `nextPattern = SEQ_TMP_PATTERN`.

That question matters because `PatternSetting` is not just decoration; `seq_determineNextPattern()` in `sequencer.c` uses it to decide the next switch target.

This copy-to-temp helper should not change how preset parameters are captured. `preset_captureTmpKitState()` and the preset image copy behavior stay exactly as they are now.

## Existing Structure Layout To Preserve

The temp pattern storage already has the right shape for the reassembled live snapshot:

- `seq_tmpPattern.seq_subStepPattern[NUM_TRACKS][NUM_STEPS]`
- `seq_tmpPattern.seq_mainSteps[NUM_TRACKS]`
- `seq_tmpPattern.seq_patternSettings`
- `seq_tmpPattern.seq_patternLengthRotate[NUM_TRACKS]`

The audit should confirm that the new live-capture logic populates those exact fields instead of introducing a second temp-pattern model.

## Likely Implementation Shape

The cleanest plan is:

1. Add a dedicated helper in `PatternData.c` named `seq_copyToTmpPattern(uint8_t srcPattern)` for capturing the currently playing arrangement into `seq_tmpPattern`.
2. Keep `seq_copyPattern()` as the single-source copy helper for the selected-pattern path.
3. Reuse `seq_copyTrackPattern()` or a small track-copy helper inside the new capture loop.
4. Leave `seq_applyTmpPatternTo()` and `seq_activateTmpPattern()` as the inverse temp playback path.
5. Update the call site that triggers copy-to-temp so it chooses the live-capture helper when the semantics are "copy what is currently audible."

That keeps the selected-pattern copy path and the live-playback snapshot path separate, which is easier to reason about and less likely to regress.

## Review Questions For The Code Change

The implementation should answer these questions directly:

- Is `seq_copyToTmpPattern(srcPattern)` the single entry point for capturing the live playing arrangement into temp?
- Does that helper read only `seq_perTrackActivePattern[]`, or does it need any extra latching from `sequencer.c` to avoid a race with an in-flight active switch?
- Should `seq_patternSettings` come from `seq_activePattern` or from one of the per-track source patterns?
- Should `seq_copyPattern()` remain untouched except for a call-site split, or should it share a lower-level internal helper with the new live-capture path?
- Does `preset_captureTmpKitState()` remain coupled to the temp-pattern capture, or should that coupling be moved into the new helper so the temp image only becomes valid after the whole snapshot is complete?

## Verification Targets

Before any background-load work starts, the code should be validated with these checks:

1. Copy-to-temp while playback is running should snapshot the per-track live source patterns.
2. Copy-to-temp while playback is stopped should still behave like the selected-pattern copy path.
3. The temp snapshot should contain the full pattern data for tracks `0` through `6`, including steps, main-step masks, and per-track length/rotation data.
4. The temp image should only be marked valid after the full copy is finished.
5. The live-capture helper should not change any normal storage source pattern.
6. The selected-pattern copy path should remain available for non-live copy operations.

## Out Of Scope For This Audit

Do not solve these yet:

- background-load mode enums;
- file-type routing;
- instant temp-switch forcing;
- LED flash policy after file load;
- any AVR menu changes;
- any second cache/session owner.

Those are later steps once the live per-track temp snapshot is approved.

## Temporary Pattern Repeat / Next Rule

The code already has the temp hold sentinel we need: `SEQ_TMP_PATTERN`.

The temp-copy path should normalize the temp slot to a hold state after the payload copy finishes instead of cloning the source pattern's settings verbatim.

### Required temp behavior

- force `seq_tmpPattern.seq_patternSettings.changeBar = 0`;
- force `seq_tmpPattern.seq_patternSettings.nextPattern = SEQ_TMP_PATTERN`;
- keep the temp slot on the temp sentinel until the user makes a manual pattern change;
- do not let a source pattern's next-pattern target leak into `seq_tmpPattern`.

The important point is that the temp snapshot is a held temp image, not a normal pattern that participates in the source pattern's next-pattern chain.

### Functions that need the temp rule

The temp rule should be applied in the temp-specific path, not in the normal copy path:

- `seq_copyToTmpPattern(uint8_t srcPattern)` should apply the hold-state normalization after it copies either the live per-track snapshot or the stopped-playback fallback;
- `seq_initPatternData()` should initialize `seq_tmpPattern.seq_patternSettings` to the same hold state so the temp slot starts in the right mode at boot;
- `seq_determineNextPattern()` already understands `SEQ_TMP_PATTERN`; the temp rule should preserve that sentinel by writing it back into the temp slot's `nextPattern` field;
- `seq_applyTmpPatternTo(uint8_t pattern)` should be reviewed so the temp-only hold state does not leak into ordinary selected patterns when temp data is pasted back.

### Open design point

The audit should decide whether the hold-state normalization is best encoded as:

- a small dedicated helper in `PatternData.c` that writes `seq_tmpPattern.seq_patternSettings` after a temp copy;
- or a small Pattern API helper that returns the temp hold-state template and is reused by both temp initialization and temp capture.

Either way, the temp copy must stop treating `seq_patternSettings` as a straight source-pattern clone.

### Note on pending state

This rule is separate from the pending-pattern switch state.

- `preset_tempPlaybackSwitchState.pendingPattern` still belongs to the switch scheduler.
- The temp-copy rule is only about the contents of `seq_tmpPattern.seq_patternSettings`.
- The `SEQ_TMP_PATTERN` sentinel already exists in the sequencer switch logic, so the temp copy should preserve that sentinel in `nextPattern` rather than inventing a new pending concept.

### Verification targets for the temp rule

Before the cleanup pass starts, the temp snapshot should be verified to satisfy these cases:

1. Copy-to-temp while playback is running still captures the live per-track pattern data.
2. Copy-to-temp while playback is stopped still captures the selected-pattern payload, but the temp settings are normalized to the hold state.
3. The temp slot never inherits a normal pattern's next-advance behavior.
4. Manual pattern changes still replace the temp-held state correctly.
5. Pasting temp data back into a normal pattern does not accidentally force that normal pattern to behave like temp.

### Implementation note

- Added a dedicated temp-settings normalizer in `PatternData.c` so both `seq_initPatternData()` and `seq_copyToTmpPattern()` force `seq_tmpPattern.seq_patternSettings.changeBar = 0` and `seq_tmpPattern.seq_patternSettings.nextPattern = SEQ_TMP_PATTERN`.
- The normal selected-pattern copy path remains unchanged; only temp capture now rewrites the temp slot to the hold state after payload copy.

## Follow-On Cleanup Pass

After the temp-copy helper is accepted, the next cleanup pass should rename and re-document the entire `mainboard/LxrStm32/src/Sequencer/Pattern/` surface so the module reads like the owner of pattern storage instead of a mixed `seq_*` compatibility layer.

This pass is not meant to change playback behavior. It is a boundary, naming, and documentation cleanup so the code clearly says:

- Pattern owns pattern storage and pattern mutation/copy helpers;
- Sequencer owns scheduling, pattern-switch orchestration, and live playback timing;
- `sequencer.c` should call Pattern APIs instead of hosting pattern-paste logic itself;
- new names inside `Pattern/` should be `pat_*`, `Pat*`, or another `pat`-prefixed variation, not `seq_*`, `Seq*`, or `seq*`.

### Current Rename Inventory

The scan of `mainboard/LxrStm32/src/Sequencer/Pattern/` still shows these Pattern-owned helpers and variables using `seq` naming:

#### `PatternData.c` and `PatternData.h`

- `seq_patternSet`
- `seq_tmpPattern`
- `seq_setTmpPatternHoldSettings`
- `seq_resetNote`
- `seq_normalizePatternNumber`
- `seq_getStepPtr`
- `seq_getLengthRotatePtr`
- `seq_getPatternSettingPtr`
- `seq_getMainSteps`
- `seq_setMainSteps`
- `seq_initPatternData`
- `seq_applyTmpPatternTo`
- `seq_activateTmpPattern`
- `seq_setTrackLength`
- `seq_getTrackLength`
- `seq_setTrackScale`
- `seq_getTrackScale`
- `seq_setTrackRotation`
- `seq_getTrackRotation`
- `seq_setLoop`
- `seq_toggleStep`
- `seq_toggleMainStep`
- `seq_setMainStep`
- `seq_isStepActive`
- `seq_isMainStepActive`
- `seq_clearTrack`
- `seq_clearAutomation`
- `seq_clearPattern`
- `seq_copyTrack`
- `seq_copyPattern`
- `seq_copyToTmpPattern`
- `seq_copyTrackPattern`
- `seq_copySubStep`

#### Cross-module names referenced from Pattern/

These are not Pattern-owned, but they still appear inside the Pattern directory and should be treated as dependencies to minimize or wrap cleanly:

- `seq_activePattern`
- `seq_running`
- `seq_perTrackActivePattern[]`
- `seq_stepIndex[]`
- `seq_loopLength`
- `seq_pendingLoopLength`
- `seq_loopCurrentPosition`
- `seq_loopUpdateFlag`
- `seq_triggerVoice()`

The cleanup pass should verify whether any of these dependencies can be reduced to read-only arguments or small Pattern-facing wrappers, so the Pattern module stops depending on the wider `seq_*` surface for things it does not own.

### Functions That Stay In Sequencer

The following pieces should remain in `sequencer.c` because they are orchestration, not storage ownership:

- `seq_setNextPattern`
- `seq_determineNextPattern`
- the pending-pattern state machine around `preset_tempPlaybackSwitchState`
- the live trigger-time reads that consume pattern storage but do not own it

Those functions may call Pattern helpers, but they should not implement copy/paste or direct pattern mutation themselves.

### Proposed Pattern API After The Move

The Pattern public surface should be grouped and named consistently. The intended names are:

#### Module initialization and naming helpers

- `pat_initPatternData`
- `pat_normalizePatternNumber`

#### Accessors

- `pat_getStepPtr`
- `pat_getLengthRotatePtr`
- `pat_getPatternSettingPtr`
- `pat_getMainSteps`
- `pat_setMainSteps`

#### Read helpers

- `pat_isStepActive`
- `pat_isMainStepActive`

#### Pattern editing helpers

- `pat_setTrackLength`
- `pat_setTrackScale`
- `pat_setTrackRotation`
- `pat_setLoop`
- `pat_toggleStep`
- `pat_toggleMainStep`
- `pat_setMainStep`
- `pat_clearTrack`
- `pat_clearAutomation`
- `pat_clearPattern`

#### Copy and paste helpers

- `pat_copyTrack`
- `pat_copyPattern`
- `pat_copyTrackPattern`
- `pat_copySubStep`
- `pat_copyToTmpPattern`
- `pat_applyTmpPatternTo`
- `pat_activateTmpPattern`

#### Temp-only helper

- `pat_setTmpPatternHoldSettings`

That naming gives the sequencer and front-panel protocol a single obvious module to call for pattern data movement.

### Sequencer Call-Site Cleanup

After the rename, `sequencer.c` should only call the Pattern APIs and should not need to know how the copy or paste is implemented internally.

The cleanup pass should update the specific call sites that currently depend on Pattern helpers:

- the temp activation path around `seq_activateTmpPattern()`
- the live temp snapshot path around `seq_copyToTmpPattern()`
- the selected-pattern copy and track-copy paths that the front-panel receive code reaches through Pattern APIs
- the pattern-setting reads in `seq_determineNextPattern()`
- the step state access helpers used while triggering voices, but only as read-only accessors
- the per-track rotation reset path in the pattern-switch code
- the Pattern accessor uses in `Preset/TempPlaybackSwitch.c`
- the Pattern accessor uses in `frontPanelSendingProtocol.c`
- the generator helpers in `mainboard/LxrStm32/src/Sequencer/Pattern/EuklidGenerator.c`
- the SOM generator helpers in `mainboard/LxrStm32/src/Sequencer/Pattern/SomGenerator.c`

The goal is for `sequencer.c` to read like a scheduler and dispatcher, not a storage layer.

### Documentation To Add

The Pattern module should gain clearer ownership docs at the same time as the rename.

That documentation should say:

- Pattern owns `seq_patternSet` and `seq_tmpPattern` today, and the rename pass should move those to `pat_patternSet` / `pat_tmpPattern`;
- Pattern exposes the only public copy/paste surface for pattern data;
- live temp snapshots are a Pattern responsibility even though they read the live playback source table;
- Sequencer is allowed to ask Pattern for copy/paste behavior, but not to reimplement it;
- generator files inside `Pattern/` should document whether they are editing helpers, pattern transforms, or audio-pattern generators.

The header comments should also separate the API into sections so future work can see at a glance which helpers are initialization helpers, which are accessors, which are mutators, and which are copy/paste operations.

`PatternData.h` should read like the authoritative API reference for the module:

- a module banner that states ownership of pattern storage;
- a storage/constants section for `NUM_TRACKS`, `NUM_PATTERN`, `SEQ_TMP_PATTERN`, `NUM_STEPS`, `Step`, `PatternSetting`, `LengthRotate`, `PatternSet`, and `TempPattern`;
- an accessor block for read/write pattern storage helpers;
- an edit/mutation block for step, track, automation, and rotation writes;
- a copy/paste block for selected-pattern copy, temp snapshot copy, track copy, and temp apply helpers;
- a short note that `sequencer.c` and the front-panel protocol should call these helpers instead of manipulating pattern payloads directly;
- adjacent one-line comments for any static helper that has non-obvious state, especially `seq_setTmpPatternHoldSettings()` / its future `pat_*` equivalent.

`EuklidGenerator.h/c` and `SomGenerator.h/c` should get the same treatment:

- each exported helper should have a short purpose comment in the header;
- each module should explain whether it owns pattern-generation state, per-track rotation state, or pattern transfer state;
- module-global variables such as the Euclid working buffers and the SOM generator state should have one-line comments explaining what each one tracks.

### Rename Strategy

The prefix cleanup should prefer `pat_*` / `Pat*` for the exported Pattern API.

The intended end state is:

- no new Pattern ownership APIs should be introduced under `seq_*`;
- no Pattern-owned variable should keep a `seq_` or `Seq` prefix once the rename pass is done;
- the generator modules should also be moved onto `pat_*` / `Pat*` names rather than staying on bare `euklid_*` / `som_*`;
- any temporary compatibility wrappers should be clearly marked and removed once callers are updated;
- new code in `sequencer.c`, `frontPanelReceivingProtocol.c`, `Preset/TempPlaybackSwitch.c`, and the generator helpers should only include and call the Pattern API names.

That cleanup is intentionally separate from the per-track temp-copy fix so the first review stays narrow.

## Implementation Notes

- Added `seq_copyToTmpPattern(uint8_t srcPattern)` in `PatternData.c` as the dedicated temp-copy helper.
- The new helper uses `seq_perTrackActivePattern[]` and `seq_copyTrackPattern()` when `seq_running` is true.
- The new helper falls back to `seq_copyPattern(srcPattern, SEQ_TMP_PATTERN)` when playback is stopped so the old selected-pattern temp-copy behavior stays available.
- Temp pattern settings are copied from `seq_activePattern` during the live-running path.
- `preset_captureTmpKitState()` is still the only place that copies preset parameter images and marks the temp kit valid.
- `frontPanelReceivingProtocol.c` now routes `FRONT_SEQ_COPY_PATTERN` to `seq_copyToTmpPattern(src)` when the destination is `SEQ_TMP_PATTERN`.
- STM32 build passes with the new helper split: `make -C mainboard/LxrStm32 -j4 stm32`.
