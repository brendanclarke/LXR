# Background Load Feature Plan

Session: 021
Status: draft for review before implementation

## Goal

Add automatic background-load behavior that uses the existing temporary pattern and temporary parameter storage while a file is loading, so the currently playing pattern does not change until the copy-to-temp and instant temp switch are complete.

This work is limited to automation and data flow. The UI menu entry for the new global setting will not be added yet.

## Core Rules To Preserve

1. Background loading only applies when the sequencer is actually running.
2. If the sequencer is stopped, file loads continue to go directly into normal storage.
3. Kit, morph, and individual instrument sound load types (anything from .snd files) never background-load. They always apply directly to normal storage.
4. `.pat`, `.prf`, and `.all` are the only file types that may participate in background loading.
5. `.pat` must never redirect parameter read/write away from normal storage.
6. The temp switch must be instant, regardless of any eventual user-facing switch mode option.
7. Do not reintroduce any load-cache owner. The current normal/temp storage model stays the authority.

## New Global Setting

Add a new background-load mode enum in the AVR settings/global-parameter area, with these values:

- `0`: no background load
- `1`: pattern (`.pat`) only
- `2`: performance (`.prf`) only
- `3`: all file only (`.all`)
- `4`: all three file types

Implementation notes:

- The default value if none is set (in the .cfg file when implemented) is `0`.
- For this implementation pass, force the runtime value to `4` so the feature can be tested end to end.
- Do not add a menu page or parameter entry for the user yet.

## Files Likely To Change

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

### 3. Expand the temp copy to use the currently playing pattern layout

The existing copy-to-temp behavior currently snapshots the selected pattern model.

The background-load version needs to snapshot the currently playing arrangement:

- copy all preset parameters into temp storage, as the current copy-to-temp path already does;
- copy the active general pattern metadata;
- copy the individually playing track patterns for tracks `0` through `6`, not just the selected pattern;
- make sure the temp image is only marked valid after the full copy is complete.

This likely means introducing a dedicated helper for "copy the currently playing sequencer state into temp" rather than trying to force the existing selected-pattern helper to do both jobs.

### 4. Perform the instant temp switch after the copy finishes

Once the temp snapshot is complete:

- switch playback to the temporary `SEQ16` pattern immediately;
- force the same instant-switch behavior that the PERF-mode temp button uses;
- do this even if any future user option would otherwise prefer a next-step switch.

This is the key step that ensures the file load happens while the listener stays on the temporary image, not the currently audible normal image.

### 5. Start the file load into normal storage after the temp switch is active

After the temp switch completes, continue with the actual file load into normal storage.

The file load should behave like the current direct load path with respect to where bytes land, except that it is now happening after the temporary playback switch is already active.

Important details:

- `.prf` and `.all` keep writing to normal parameter/pattern storage.
- `.pat` must stay on normal parameter read/write behavior even in the background-load path.
- pattern temp storage and parameter temp storage must stay separate, even though they are used in the same user-visible workflow.

### 6. Finish by unlocking the UI and setting the right LED indication

When the load completes, unlock the menu/UI as normal and set the post-load indication state:

- If PERF mode is not selected, flash the `PERF` LED.
- If PERF mode is selected, keep the `PERF` LED lit normally and flash the `SELECT` LEDs for patterns with main sequencer steps enabled.
- If no pattern has any enabled main sequencer steps, flash the first `SELECT` LED.

The flash state should be treated as a temporary "load completed" indicator, not a permanent mode change.

### 7. Clear the flash state on the next real pattern change

When the user selects a new pattern and the pattern content actually changes:

- switch normally using the current existing path;
- stop the post-load flashing indication;
- leave the regular pattern/update logic intact.

This keeps the load-complete indicator from lingering after the user has moved on.

### 8. Keep copy-to-temp and background-load logic aligned

The background-load workflow should reuse the same storage and switching primitives as the current temp button path wherever possible.

That means the implementation should prefer:

- one shared temp-capture helper;
- one shared instant temp-switch helper;
- one shared post-switch indication/reset path;
- separate wrappers for "selected pattern copy" and "currently playing pattern snapshot" only where the semantics differ.

### 9. Preserve the existing comms and storage ownership split

Do not move ownership back into a load cache or a second session object.

The current split stays intact:

- `Preset` owns parameter images and temp/normal selection;
- `Pattern` owns pattern payloads;
- `TempPlaybackSwitch` owns audible-image selection;
- front-panel receive/send code owns protocol framing only.

## Behavioral Edge Cases To Cover

- Sequencer stopped: direct load only.
- `.pat` file while running: background-load only if the mode allows it, but parameter routing still stays normal.
- `.prf` file while running: background-load allowed if enabled.
- `.all` file while running: background-load allowed if enabled.
- Kit/morph/single-instrument loads: never background-load.
- No enabled main steps anywhere after a PERF-mode load: flash the first `SELECT` LED.
- Pattern changes after the load: flashing indicator must clear.

## Verification Plan

1. Build the STM and AVR firmware after the changes.
2. Test direct-load behavior with the sequencer stopped.
3. Test `.pat`, `.prf`, and `.all` loads while playback is running and the mode is enabled.
4. Verify the temp switch happens before the load writes continue.
5. Verify kit/morph/instrument loads still go directly to normal storage.
6. Verify the post-load LED behavior in both non-PERF and PERF mode.
7. Verify a later pattern change clears the flashing indicator.

## Notes For Review

- The default runtime mode should be `4` during implementation testing, but the long-term default should still be `0`.
- The menu entry for the setting is intentionally deferred.
- If the currently playing pattern snapshot requires a new helper in `PatternData`, that is preferred over overloading the existing selected-pattern copy path.
- The switch to temporary pattern and parameter (if .all or .prf) use must be instant when used for this workflow, regardless of global option flag ('pci'/'SequencrPCInstnt').

## Implementation Status Update

### What Is In Place

- Added `PAR_FILE_LOAD_BACKGROUND_MODE` on the AVR side and a matching `BackgroundLoadModeEnum` with values `0` through `4`.
- Kept the setting hidden from the menu for now, but wired the global parameter plumbing so it can be loaded and stored.
- Forced the runtime value to `BG_LOAD_ALL_TYPES` during global-settings load so the feature can be exercised immediately; the long-term default remains `0`.
- Added a new front-panel opcode pair for the temp-prep handshake:
  - AVR send opcode: `SEQ_COPY_PLAYING_TO_TMP`
  - STM receive opcode: `FRONT_SEQ_COPY_PLAYING_TO_TMP`
- Added `seq_capturePlayingPatternToTmp()` so the temp snapshot copies:
  - the active pattern metadata,
  - the currently playing per-track pattern data for tracks `0` through `6`,
  - and the rest of the temp-pattern state used by the existing temp copy flow.
- Added `preset_forceTempPlaybackInstantSwitch()` and a force flag inside the STM temp switch state so the background-load workflow always uses the instant temp switch path.
- Updated the STM sequencer switch logic so the forced instant switch can happen independently of the normal next-step switch option.
- Added AVR-side background-load gating so only `.pat`, `.prf`, and `.all` can take the background path, and only while the sequencer is running.
- Kept kit, morph, and individual sound loads on the direct normal-storage path.
- Added the post-load flash indicator logic:
  - flash PERF when PERF mode is not selected,
  - otherwise flash the SELECT LEDs for patterns with active main sequencer steps,
  - fall back to the first SELECT LED if no pattern has main steps enabled.
- Added a clear path so the flash state is removed when a later real pattern change occurs.

### Supporting Cleanup

- Added a missing `frontPanelParser.h` header so the AVR build can compile cleanly in this workspace.
- Updated the stale `preset_loadDrumset()` call in `frontPanelParser.c` to the current 3-argument signature.
- Excluded the legacy duplicate `frontPanelParser.c` from the AVR build, since `frontPanelReceivingProtocol.c` is the active implementation and linking both copies produced duplicate symbols.
- Added the missing `I_DUNNO` sentinel to STM `config.h` so the MIDI parser code can initialize its local "unset" parameter marker.

### Build Verification

- AVR build passes: `make -C front/LxrAvr avr -j4`
- STM32 build passes: `make -C mainboard/LxrStm32 -j4 stm32`

### Remaining Risks

- The new temp-prep handshake depends on the AVR wait state being released by the existing pattern-change acknowledgement path. If that protocol changes later, this will need to move with it.
- The flash-indicator logic is intentionally conservative and only touches the LEDs it sets. That avoids clobbering unrelated UI state, but the exact look should still be confirmed on hardware.
- The temp snapshot helper now copies the currently playing track layout into temp storage. That is the behavior we want for this feature, but it is the main place to watch for any mismatch between currently selected and currently playing pattern state.
- The runtime background-load mode is still forced to `BG_LOAD_ALL_TYPES` for testing. It needs to be returned to the intended default behavior before release.

### What To Test

1. Load `.pat`, `.prf`, and `.all` while the sequencer is running and confirm the temp snapshot and instant switch happen before file bytes continue loading.
2. Load the same file types while the sequencer is stopped and confirm they still load directly into normal storage.
3. Confirm kit, morph, and individual sound loads always bypass background loading.
4. Confirm the post-load flash behavior:
   - non-PERF mode flashes the PERF LED,
   - PERF mode flashes the correct SELECT LEDs for patterns with active main steps,
   - PERF mode with no active main steps flashes the first SELECT LED.
5. Change to a different pattern after background loading and confirm the temporary flash state clears.
6. Verify the temp-prep switch still behaves correctly if the temporary pattern is already active before loading starts.
