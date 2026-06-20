# AUDIT: Per-Voice Morph + Pattern-Only Background Load Tweaks

## Goal

Plan the next small changes after the background-load temp switch is mostly
working:

1. While playback is using temporary data, user changes to global morph or
   per-voice morph amounts should update both the temporary kit image and the
   normal kit image.
2. After a background load switches playback to temporary data, the PERF menu
   should show the correct per-voice morph amounts instead of zeroes.
3. For `.pat` background loads, only pattern data should move to temporary
   storage/playback. Parameters should remain on normal storage the whole time.

No code has been changed for this audit.

## Current Relevant Paths

### AVR Global Morph User Edit

`front/LxrAvr/Menu/menu.c`

- `menu_parseGlobalParam(PAR_MORPH, value)`
  - sets `morphValue = value`;
  - calls `menu_syncVoiceMorphDisplayValues(value)`;
  - sends:
    - `SEQ_CC, SEQ_SET_GLOBAL_MORPH_LSB`
    - `SEQ_CC, SEQ_SET_GLOBAL_MORPH_MSB`

STM receives this in:

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

- `FRONT_SEQ_SET_GLOBAL_MORPH_LSB`
- `FRONT_SEQ_SET_GLOBAL_MORPH_MSB`
  - reconstructs the 8-bit amount;
  - calls `seq_setGlobalMorphAmount(morphAmount)`;

Then:

`mainboard/LxrStm32/src/Preset/MorphEngine.c`

- `preset_setGlobalMorphAmount(morphAmount)`
  - currently writes only `preset_getCurrentImageKitState()`;
  - if temp kit is active, this means only `preset_tmpKitState`.

### AVR Per-Voice Morph User Edit

`front/LxrAvr/Menu/menu.c`

- `menu_sendVoiceMorphAmount(paramNr, value)`
  - calls `avrComms_sendVoiceMorphValue(voice, value)`.

`front/LxrAvr/avrComms/avrCommsSendingProtocol.c`

- `avrComms_sendVoiceMorphValue()`
  - sends two `VOICE_MORPH` packets for LSB/MSB.

STM receives this in:

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

- `FRONT_SEQ_VOICE_MORPH`
  - `frontParser_handleVoiceMorph()`
  - reconstructs the 8-bit amount;
  - calls `seq_setVoiceMorphAmount(voice, morphAmount)`.

Then:

`mainboard/LxrStm32/src/Preset/MorphEngine.c`

- `preset_setVoiceMorphAmount(synthVoice, morphAmount)`
  - currently writes only the voice's current morph image:
    - normal if that voice source is normal;
    - temp if that voice source is temp.

### Temp Switch / AVR Display Restore

`mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c`

- `preset_setTempPlaybackActive(1)`
  - sets the STM current-image target to temp;
  - calls `preset_syncVMorphAmountMirrorsFromLiveSources()`;
  - calls `preset_maybePushKitEndpointsToFrontWithGlobalMorphReport(&preset_tmpKitState)`.

That queues endpoint restore traffic in:

`mainboard/LxrStm32/src/Preset/EndpointRestore.c`

- `preset_serviceEndpointRestore()`
  - sends `PARAM_RESTORE_BEGIN`;
  - sends front endpoint params;
  - sends morph endpoint params;
  - sends global morph report;
  - sends per-voice morph reports from the chosen kit;
  - sends `PARAM_RESTORE_DONE`.

AVR receives that in:

`front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

- `PRF_RESTORE_PARAM_CC/CC2`
  - writes `parameter_values[]`;
- `PRF_RESTORE_MORPH_CC/CC2`
  - writes `parameters2[]`;
- `SEQ_REPORT_GLOBAL_MORPH_LSB/MSB`
  - writes `parameter_values[PAR_MORPH]`;
  - writes `morphValue`;
  - calls `avrCommsParser_syncVoiceMorphDisplayValues(amount)`;
- `VOICE_MORPH`
  - calls `avrCommsParser_handleVoiceMorphReport()`;
  - writes `parameter_values[PAR_MORPH_DRUM1 + voice]`.

Important current risk:

- while `avrCommsParser_rxDisable` is true, `VOICE_MORPH` is not one of the
  explicitly allowed-through message types;
- therefore per-voice morph reports sent during the file-load-protected window
  may be dropped even though global morph reports are sent as `SEQ_CC` and may be
  accepted in a different part of the parser flow;
- `PRF_RESTORE_PARAM_*` may also write zeros to `parameter_values[]` for the
  morph display params if the temp endpoint image contains zeroes in those slots.

### `.PAT` Background Load

AVR `.pat` path:

`front/LxrAvr/Preset/presetManager.c`

- `preset_loadPattern()`
  - sets `preset_workingType = WTYPE_PATTERN`;
  - may call `preset_performBackgroundSwapWait(WTYPE_PATTERN)`;
  - then sends `SEQ_FILE_BEGIN, WTYPE_PATTERN`;
  - sends pattern data only.

STM background-swap path:

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

- `FRONT_SEQ_BACKGROUND_SWAP_BEGIN`
  - currently does not distinguish `.pat`, `.prf`, and `.all`;
  - always:
    - `pat_copyToTmpPattern(seq_activePattern)`;
    - `seq_setNextPattern(SEQ_TMP_PATTERN, 0x0f)`;
    - `preset_tempPlaybackSwitchState.forceInstantSwitch = 1`;
    - starts the ACK state machine.

Sequencer then crosses the temp boundary in:

`mainboard/LxrStm32/src/Sequencer/sequencer.c`

- `seq_nextStep()`
  - sets `seq_activePattern = SEQ_TMP_PATTERN`;
  - calls `preset_setTempPlaybackActive(seq_activePattern == SEQ_TMP_PATTERN)`;
  - calls `preset_updateVoiceSourcesForPatternChange(...)`.

That means a `.pat` background load currently switches both pattern and
parameter ownership to temp, which conflicts with the new requirement.

## Planned Code Changes

### 1. Mirror Persistent Morph Amount Edits From Temp To Normal

File:

- `mainboard/LxrStm32/src/Preset/MorphEngine.c`

Change `preset_setGlobalMorphAmount(uint8_t morphAmount)`:

- keep the current behavior for the active/current kit image;
- if `preset_isTmpKitActive()` is true, also write the same persistent morph
  state into `preset_normalKitState`:
  - `preset_normalKitState.globalMorphAmount = morphAmount`;
  - every `preset_normalKitState.voiceMorphBaseAmount[voice] = morphAmount`;
  - every `preset_normalKitState.voiceMorphAmount[voice] = morphAmount`.

Change `preset_setVoiceMorphAmount(uint8_t synthVoice, uint8_t morphAmount)`:

- keep the current behavior for the voice's active morph image;
- if the selected kit is `preset_tmpKitState`, also write:
  - `preset_normalKitState.voiceMorphBaseAmount[synthVoice] = morphAmount`;
  - `preset_normalKitState.voiceMorphAmount[synthVoice] = morphAmount`.

Rationale:

- These two functions are the persistent user-control setters reached by the AVR
  global morph and per-voice morph UI.
- Mirroring here keeps the normal storage current while playback is insulated on
  temp, so a later return to normal or save/load decision does not resurrect the
  old morph amount.

Do not mirror in these functions unless explicitly desired:

- `preset_setVoiceMorphLiveAmount()`
- `preset_setVoiceMorphAutomationValue()`
- `preset_setVoiceMorphMaskAutomationValue()`
- velocity/macro/modulation morph helpers

Reason:

- those are transient automation/modulation paths, not persistent front-panel
  amount edits. Mirroring them into normal storage would make automation rewrite
  the saved/current control amount.

### 2. Keep PERF Menu Per-Voice Morph Display Correct After Temp Switch

File:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

The display zeroing can be fixed without restoring the old unsafe
`menu_repaintAll()` during file load.

Plan A, preferred:

- let restore param traffic continue updating `parameter_values[]`, but prevent
  endpoint restore from overwriting the PERF-page per-voice morph display slots:
  - in `PRF_RESTORE_PARAM_CC`, before writing to `parameter_values[param]`, skip
    the write if `param` is `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`;
  - in `PRF_RESTORE_PARAM_CC2`, after adding 128, skip the write if `paramNr` is
    `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`.
- keep `PRF_RESTORE_MORPH_CC/CC2` writing `parameters2[]`; those are morph
  endpoint bytes, not the live PERF morph amount display.

Then make sure the explicit per-voice morph reports can correct the display:

- allow `VOICE_MORPH` reports through during `avrCommsParser_rxDisable`, like
  restore status messages are currently allowed, because these reports are
  display-only and do not echo back into STM sound state;
- alternatively, defer them in small AVR-side pending storage until
  `rxDisable` clears, but allowing them through is simpler and scoped.

Keep this existing guard:

```c
if(!avrCommsParser_rxDisable)
   menu_repaintAll();
```

in `PARAM_RESTORE_DONE`, because the load screen must not be repainted out from
under an active file load.

Expected result:

- global morph stays correct via `SEQ_REPORT_GLOBAL_MORPH_*`;
- per-voice morph values are no longer zeroed by raw endpoint restore writes;
- per-voice `VOICE_MORPH` reports from STM update
  `parameter_values[PAR_MORPH_DRUM1 + voice]` even during the protected
  background-load window.

### 3. Make `.PAT` Background Load Pattern-Only

Files:

- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.h`
- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Add a flag to `PresetTempPlaybackSwitchState`:

```c
uint8_t patternOnlyTempPlayback;
```

Set it in the STM background-swap handler:

`frontPanelReceivingProtocol.c`, `FRONT_SEQ_BACKGROUND_SWAP_BEGIN`

- incoming file type values match AVR `WTYPE_*`:
  - pattern = `7`;
  - performance = `8`;
  - all = `9`.
- when `frontParser_command.data2 == 7`, set:

```c
preset_tempPlaybackSwitchState.patternOnlyTempPlayback = 1;
```

- otherwise set it to `0`.

Then adjust the sequencer temp boundary application:

`sequencer.c`, inside the pattern switch block after:

```c
seq_activePattern = newActivePattern;
```

Current behavior:

```c
preset_setTempPlaybackActive(seq_activePattern == SEQ_TMP_PATTERN);
...
preset_updateVoiceSourcesForPatternChange(oldTrackPattern, !activePatternChanged);
```

Planned behavior:

- if `seq_activePattern == SEQ_TMP_PATTERN` and
  `preset_tempPlaybackSwitchState.patternOnlyTempPlayback` is true:
  - do **not** call `preset_setTempPlaybackActive(1)`;
  - keep or force all voice source states normal;
  - skip `preset_updateVoiceSourcesForPatternChange()` for this temp pattern
    switch, because that function currently derives temp parameter ownership
    from temp pattern ownership;
  - keep pattern playback on `SEQ_TMP_PATTERN`.
- otherwise use the existing behavior.

Provide a tiny Preset helper rather than open-coding voice loops in Sequencer:

`TempPlaybackSwitch.c`

```c
void preset_keepNormalVoiceSourcesForPatternOnlyTempPlayback(void)
```

This helper should:

- make sure `preset_setTempPlaybackActive(0)` is in effect;
- ensure every `preset_voiceSourceState[]` entry is normal;
- avoid pushing endpoint restore reports, because `.pat` background load should
  not refresh/alter audible parameters.

Update ACK readiness:

`frontPanelReceivingProtocol.c`

- `frontParser_backgroundSwapTempPlaybackReady()` currently requires:
  - `seq_activePattern == SEQ_TMP_PATTERN`;
  - all `seq_perTrackActivePattern[] == SEQ_TMP_PATTERN`;
  - `preset_allVoiceSourcesUseTmp()`.
- for `.pat`/pattern-only mode, change the final readiness condition to:

```c
if(preset_tempPlaybackSwitchState.patternOnlyTempPlayback)
   return preset_allVoiceSourcesUseNormal();

return preset_allVoiceSourcesUseTmp();
```

Rationale:

- `.pat` needs temp pattern playback so normal pattern storage can be overwritten
  silently;
- `.pat` must keep normal parameters audible and editable, so readiness should
  prove normal parameter ownership, not temp parameter ownership.

Clear the flag when leaving temp or finishing the background switch:

- set `patternOnlyTempPlayback = 0` for `.prf` and `.all` background swaps;
- clear it when switching back to normal pattern playback, or at the start of a
  non-pattern background swap, so it cannot leak into `.prf`/`.all`.

### 4. Do Not Change AVR `.PAT` Parameter Loading

No AVR `.pat` parameter-load changes should be needed for the third tweak:

- `preset_loadPattern()` already sends pattern structures, not kit parameter
  payloads;
- the problem is STM-side ownership after the temp switch, not AVR file content.

Avoid adding AVR-side temp parameter exceptions to `.pat`. The correct behavior
is for STM to keep parameter ownership normal while only pattern ownership moves
to temp.

## Verification Plan

### Morph Mirroring

1. Start sequencer.
2. Load `.all` or `.prf` with background load enabled so playback moves to temp.
3. Change global morph on PERF page.
4. Change individual voice morph values.
5. Confirm sound changes immediately.
6. Confirm PERF page values remain correct.
7. Switch/load in a way that returns to normal parameter ownership.
8. Confirm the normal kit image kept the edited morph amounts.

### Per-Voice Display Restore

1. Start sequencer.
2. Set nonzero, distinct per-voice morph values.
3. Trigger background `.all`/`.prf` load.
4. Confirm PERF menu shows the same per-voice morph values after temp switch.
5. Confirm global morph remains correct.
6. Confirm load screen is not repainted away by `PARAM_RESTORE_DONE`.

### `.PAT` Pattern-Only Background Load

1. Start sequencer.
2. Set audible parameters and morph amounts.
3. Load `.pat` with background load enabled.
4. Confirm playback continues from temp pattern data during the load.
5. Confirm audible parameters do not change.
6. Confirm editing a parameter during/after the `.pat` background load edits
   normal parameters, not temp parameters.
7. Confirm `.all` and `.prf` still switch both pattern and parameter ownership
   to temp before normal storage is overwritten.

## Risk Notes

- The `.pat` tweak is the highest-risk part because it intentionally breaks the
  current shortcut assumption that temp pattern playback implies temp parameter
  ownership.
- The morph mirroring change should be limited to persistent user-control
  setters. Mirroring transient automation would make pattern automation rewrite
  normal morph storage, which is likely not desired.
- Allowing `VOICE_MORPH` through during `rxDisable` should be safe because the
  AVR handler is display-only and does not echo the values back to STM.

## Implementation Notes: Pattern-Only Background Load

Implemented only the third point in this pass.

Files changed:

- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`

### State Added

Added one byte to `PresetTempPlaybackSwitchState`:

```c
uint8_t patternOnlyTempPlayback;
```

This flag is set in the STM receive handler when
`FRONT_SEQ_BACKGROUND_SWAP_BEGIN` carries the pattern file type (`7`), and is
cleared for `.prf`/`.all` background swaps because the assignment is a direct
comparison against the incoming file type.

### Behavior Changed

`FRONT_SEQ_BACKGROUND_SWAP_BEGIN` still copies the active pattern to
`SEQ_TMP_PATTERN` and still requests an instant switch to that temp pattern.

When the sequencer executes that switch:

- if `patternOnlyTempPlayback` is false, behavior is unchanged:
  - `preset_setTempPlaybackActive(seq_activePattern == SEQ_TMP_PATTERN)`;
  - `preset_updateVoiceSourcesForPatternChange(...)`;
- if `patternOnlyTempPlayback` is true and the new active pattern is
  `SEQ_TMP_PATTERN`, those two Preset parameter-ownership calls are skipped.

This keeps `.pat` background playback on the temporary pattern while leaving kit
and voice parameter ownership on the normal image.

### ACK Readiness

`frontParser_backgroundSwapTempPlaybackReady()` still requires:

- `seq_activePattern == SEQ_TMP_PATTERN`;
- all per-track active patterns are `SEQ_TMP_PATTERN`.

For `.pat`, it now expects normal voice sources:

```c
if(preset_tempPlaybackSwitchState.patternOnlyTempPlayback)
   return preset_allVoiceSourcesUseNormal();
```

For `.prf`/`.all`, it still expects temp voice sources:

```c
return preset_allVoiceSourcesUseTmp();
```

### Scope

No AVR code was changed in this pass.

No morph mirroring or per-voice morph display fixes were implemented yet.

### Verification

- `make -C mainboard/LxrStm32 -j4 stm32` passed and rebuilt
  `mainboard/LxrStm32/LxrStm32.bin`.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.
- STM build warnings remain in existing unrelated areas such as mixer inline CMSIS
  helpers, old sequencer init array bounds, trigger memset size, `_exit`
  recursion, and MIDI parser fallthrough.
