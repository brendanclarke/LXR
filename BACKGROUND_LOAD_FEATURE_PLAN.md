# Background Load Feature Plan

Session: 022
Status: draft for review before implementation

## Goal

Implement background file loading through temporary storage, starting with one narrow prerequisite:

- the copy-to-temp path must snapshot the pattern data that is actually playing on each track, not just the selected pattern;
- the wider background-load routing can then reuse that temp snapshot behavior in later steps.

This session is still limited to data flow and storage ownership. The UI menu entry for the new global setting will not be added yet.

## Session 022 Scope

The first code change should be intentionally narrow.

1. Update temp-copy behavior so it captures the currently playing per-track pattern layout.
2. Keep the existing selected-pattern copy path available for the normal non-playing case.
3. Defer the broader background-load routing, temp-switch forcing, and post-load LED policy until the temp-copy behavior is reviewed.

## Core Rules To Preserve

1. Background loading only applies when the sequencer is actually running.
2. If the sequencer is stopped, file loads continue to go directly into normal storage.
3. Kit, morph, and individual instrument sound load types (anything from .snd files) never background-load. They always apply directly to normal storage.
4. `.pat`, `.prf`, and `.all` are the only file types that may participate in background loading.
5. `.pat` must never redirect parameter read/write away from normal storage.
6. The temp switch must be instant, regardless of any eventual user-facing switch mode option.
7. Do not reintroduce any load-cache owner. The current normal/temp storage model stays the authority.

## Files Likely To Change

### Phase 1: playing-pattern temp capture

- `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.c`
- `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.h`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/Preset/KitState.c`
- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c`

### Later background-load work

### AVR side

- `front/LxrAvr/Menu/menu.h`
- `front/LxrAvr/Menu/menu.c`
- `front/LxrAvr/Preset/PresetManager.h`
- `front/LxrAvr/Preset/presetManager.c`
- `front/LxrAvr/buttonHandler.c`
- `front/LxrAvr/buttonHandler.h`
- `front/LxrAvr/frontPanelSendingProtocol.c`

### STM side

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.c`
- `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.h`
- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c`
- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.h`
- `mainboard/LxrStm32/src/Preset/KitState.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`

## Implementation Plan

### 1. Add the temp-copy helper for the currently playing arrangement

The current copy-to-temp flow still assumes the selected pattern is the thing we want to snapshot. That is the part to change first.

The new helper should:

- copy the current normal kit image into temp storage, as the existing temp-capture path already does;
- copy the active general pattern metadata that belongs with the audible arrangement;
- copy the per-track pattern data for tracks `0` through `6` from `seq_perTrackActivePattern[]`, not from the selected pattern number;
- copy the associated main-step and track-rotation data for each source track;
- only mark the temp image valid after the full copy is complete.

This helper should live beside the existing selected-pattern copy path instead of replacing it. The normal copy path still matters for non-playing temp capture and for any later reuse that wants the old semantics.

### 2. Rewire the existing temp capture entry point to use the new helper when playback is live

Once the helper exists, the current copy-to-temp path should choose between:

- the existing selected-pattern snapshot, when that is the intended semantics;
- the new currently-playing snapshot, when the temp copy is part of the live temp-normal switching path.

The point of this step is to keep the behavior change narrow:

- the copy-to-temp button or command should still perform one atomic temp capture;
- the user-visible temp switch should still happen through the existing temp-switch authority;
- only the source of the copied pattern data changes.

### 3. Keep the temp switch boundary unchanged for now

Do not fold the broader background-load routing into this first change.

For the first implementation slice:

- keep the temp/normal source-selection model intact;
- keep the current instant-switch policy intact where it already exists;
- do not introduce the new background-load mode enum yet;
- do not add the file-type routing or LED policy yet.

That keeps the first review focused on whether the temp image is now capturing the correct live track layout.

### 4. Verify the new temp snapshot shape against live playback

The first verification target is a playback scenario where the selected pattern and the playing per-track sources are not the same.

Confirm that:

- the temp image receives the currently audible track data;
- the selected-pattern copy path still behaves the old way when it is invoked directly;
- the temp image is only exposed after the full copy has finished;
- no second owner or cache layer is introduced as part of the fix.

### 5. Carry this helper forward into the broader background-load design

After this first change is reviewed, later phases can reuse the new helper for the broader background-load feature.

That later work should still follow the original ownership split:

- `Preset` owns parameter images and temp/normal selection;
- `Pattern` owns pattern payloads;
- `TempPlaybackSwitch` owns audible-image selection;
- front-panel receive/send code owns protocol framing only.

## Later Background-Load Plan

The remaining background-load work stays in the plan, but it is explicitly second phase material after the temp-copy helper is accepted.

### 1. Add the background-load mode enum and storage

Create the new background-load mode enum in the AVR settings/global-parameter area.

The code should have a single authoritative runtime value and a small flag check for:

- whether the feature is enabled for the current file type;
- whether the sequencer is running;
- whether the file type is one of the supported background-load kinds.

The menu should not expose this yet.

### 2. Route file loads through a background-load decision

Update the file-load entry points so they can decide between:

- direct normal load, or
- background load with temp copy + temp switch first.

The decision should happen before the file transfer begins.

For this feature pass, the relevant file-load entry points are:

- `.pat` load
- `.prf` load
- `.all` load

Direct-load-only entry points remain direct loads:

- kit load
- morph load
- single instrument/sound load

### 3. Perform the instant temp switch after the copy finishes

Once the temp snapshot is complete:

- switch playback to the temporary `SEQ16` pattern immediately;
- force the same instant-switch behavior that the PERF-mode temp button uses;
- do this even if any future user option would otherwise prefer a next-step switch.

This is the key step that ensures the file load happens while the listener stays on the temporary image, not the currently audible normal image.

### 4. Start the file load into normal storage after the temp switch is active

After the temp switch completes, continue with the actual file load into normal storage.

The file load should behave like the current direct load path with respect to where bytes land, except that it is now happening after the temporary playback switch is already active.

Important details:

- `.prf` and `.all` keep writing to normal parameter/pattern storage.
- `.pat` must stay on normal parameter read/write behavior even in the background-load path.
- pattern temp storage and parameter temp storage must stay separate, even though they are used in the same user-visible workflow.

### 5. Finish by unlocking the UI and setting the right LED indication

When the load completes, unlock the menu/UI as normal and set the post-load indication state:

- If PERF mode is not selected, flash the `PERF` LED.
- If PERF mode is selected, keep the `PERF` LED lit normally and flash the `SELECT` LEDs for patterns with main sequencer steps enabled.
- If no pattern has any enabled main sequencer steps, flash the first `SELECT` LED.

The flash state should be treated as a temporary "load completed" indicator, not a permanent mode change.

### 6. Clear the flash state on the next real pattern change

When the user selects a new pattern and the pattern content actually changes:

- switch normally using the current existing path;
- stop the post-load flashing indication;
- leave the regular pattern/update logic intact.

This keeps the load-complete indicator from lingering after the user has moved on.

## Behavioral Edge Cases To Cover

- Sequencer stopped: direct load only.
- `.pat` file while running: background-load only if the mode allows it, but parameter routing still stays normal.
- `.prf` file while running: background-load allowed if enabled.
- `.all` file while running: background-load allowed if enabled.
- Kit/morph/single-instrument loads: never background-load.
- No enabled main steps anywhere after a PERF-mode load: flash the first `SELECT` LED.
- Pattern changes after the load: flashing indicator must clear.

## Verification Plan

### Phase 1 verification

1. Build the STM and AVR firmware after the temp-copy helper changes.
2. Test copy-to-temp while the sequencer is running and confirm the temp snapshot matches the currently playing per-track arrangement.
3. Confirm the selected-pattern copy path still behaves the old way when called directly.
4. Confirm the temp image is only marked valid after the full copy completes.
5. Confirm no cache-style owner or duplicate staging layer was introduced.

### Later background-load verification

1. Test direct-load behavior with the sequencer stopped.
2. Test `.pat`, `.prf`, and `.all` loads while playback is running and the mode is enabled.
3. Verify the temp switch happens before the load writes continue.
4. Verify kit/morph/instrument loads still go directly to normal storage.
5. Verify the post-load LED behavior in both non-PERF and PERF mode.
6. Verify a later pattern change clears the flashing indicator.

## Notes For Review

- The first code review target is the temp-copy helper and its call sites, not the full background-load path.
- If the currently playing pattern snapshot requires a new helper in `PatternData`, that is preferred over overloading the existing selected-pattern copy path.
- The background-load mode enum and file-type routing should stay deferred until the temp-copy semantics are approved.
- The switch to temporary pattern and parameter use must still be instant when it is eventually used for this workflow, regardless of global option flag (`pci`/`SequencrPCInstnt`).
