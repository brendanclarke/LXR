# Normal / Temp Copy Per-Track Audit

Session: 022
Status: draft for review before implementation

## Goal

Audit and plan the STM-side changes needed so the copy-to-temp path copies the pattern data that is actually playing on each track, not just the single selected pattern.

This is a narrow STM-side storage audit. It focuses on how the live playing pattern layout is read, assembled, and written into `seq_tmpPattern` before any broader background-load work is attempted.

The temp-copy behavior should be implemented as a dedicated helper named `seq_copyToTmpPattern()` rather than by overloading `seq_copyPattern()`.

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
- `seq_copyToTmpPattern()` becomes the only helper that snapshots the currently playing per-track arrangement into temp storage.

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

For this change, the new temp-copy helper should read only from the actual source pattern that is playing on that track, and it should not look at pending switch targets.

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

1. Add a dedicated helper in `PatternData.c` named `seq_copyToTmpPattern()` for capturing the currently playing arrangement into `seq_tmpPattern`.
2. Keep `seq_copyPattern()` as the single-source copy helper for the selected-pattern path.
3. Reuse `seq_copyTrackPattern()` or a small track-copy helper inside the new capture loop.
4. Leave `seq_applyTmpPatternTo()` and `seq_activateTmpPattern()` as the inverse temp playback path.
5. Update the call site that triggers copy-to-temp so it chooses the live-capture helper when the semantics are "copy what is currently audible."

That keeps the selected-pattern copy path and the live-playback snapshot path separate, which is easier to reason about and less likely to regress.

## Review Questions For The Code Change

The implementation should answer these questions directly:

- Is `seq_copyToTmpPattern()` the single entry point for capturing the live playing arrangement into temp?
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

## Follow-On Cleanup Pass

After the temp-copy helper is accepted, the next cleanup pass should consolidate all pattern copy/paste ownership into `mainboard/LxrStm32/src/Sequencer/Pattern/`.

That follow-on pass should:

- move pattern copy/paste orchestration out of `sequencer.c` and into `PatternData.c` or adjacent `Pattern/` files;
- expose clearly documented public pattern copy/paste helpers for the sequencer to call;
- give those helpers proper `Pat*` or `pat_*` names instead of `Seq*`, `seq_*`, or mixed variants;
- keep `sequencer.c` focused on scheduling and playback control rather than owning pattern mutation primitives.

That cleanup is intentionally separate from the per-track temp-copy fix so the first review stays narrow.
