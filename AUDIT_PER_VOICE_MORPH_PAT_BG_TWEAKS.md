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

## Implementation Notes: Temp Morph Mirroring

Implemented only the successful temp-to-normal morph mirroring in this pass.

Files changed:

- `mainboard/LxrStm32/src/Preset/MorphEngine.c`

### STM Persistent Morph Mirroring

`preset_setGlobalMorphAmount()` still writes to the currently active kit image.
When that image is `preset_tmpKitState`, it now also mirrors the global morph
amount and the global-reset per-voice morph amounts into `preset_normalKitState`.

`preset_setVoiceMorphAmount()` still writes the selected voice amount to the kit
that currently owns that voice. When that kit is `preset_tmpKitState`, it now
also mirrors that one voice amount into `preset_normalKitState`.

This is intentionally limited to the persistent front-panel morph setters. The
automation/live morph path was not changed, so sequence or modulation playback
does not rewrite the normal preset storage while background loading is active.

### Reverted AVR PERF Voice Morph Display Attempt

The previous AVR-side attempt to fix zeroed PERF voice morph display values was
removed. That failed attempt had:

- allowed `VOICE_MORPH` reports through while `avrCommsParser_rxDisable` was
  active;
- skipped restore writes to `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`;
- suppressed `menu_repaint()` from `avrCommsParser_handleVoiceMorphReport()`
  while `rxDisable` was active.

Those AVR changes were incorrect because they tried to refresh or protect the
display after the fact instead of identifying why the AVR-stored display values
were being changed during file load.

The active code now keeps only the STM morph mirroring change from this pass.

### Verification

- `make -C front/LxrAvr -j4 avr` passed after the AVR display attempt was
  reverted and rebuilt `front/LxrAvr/LxrAvr.bin`.
- `make -C mainboard/LxrStm32 -j4 stm32` passed and rebuilt
  `mainboard/LxrStm32/LxrStm32.bin` before the revert; after the revert there
  was nothing new to build for STM.
- `make firmware` passed after the AVR revert and rebuilt
  `firmware image/FIRMWARE.BIN`.

## New Assessment: Source Of Zeroed AVR Voice Morph Display Values

The AVR menu stores the displayed PERF morph values in:

- `parameter_values[PAR_MORPH]` for global morph;
- `parameter_values[PAR_MORPH_DRUM1]` through
  `parameter_values[PAR_MORPH_HIHAT]` for individual voice morph display.

During file load these AVR-stored morph display values should not change at all.
Morph is not a file-loaded parameter in the intended behavior, and STM audio
state already keeps the correct global and per-voice morph amounts through the
background copy/switch.

### Code That Can Change The Six AVR Voice Morph Display Slots

1. User edits the global morph on AVR:

   - `front/LxrAvr/Menu/menu.c`
   - `menu_setParameter()` case `PAR_MORPH`
   - calls `menu_syncVoiceMorphDisplayValues(value)`

   This is correct for a user-initiated global morph edit because global morph is
   defined to reset the six individual voice morph display amounts.

2. User edits an individual voice morph on AVR:

   - `front/LxrAvr/Menu/menu.c`
   - `menu_sendVoiceMorphAmount()`
   - sends `VOICE_MORPH` to STM

   This path is also correct. `parameter_values[paramNr]` has already been
   changed by the normal menu edit path before the send helper runs.

3. STM sends an explicit per-voice morph report:

   - `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`
   - `avrCommsParser_handleVoiceMorphReport()`
   - writes `parameter_values[PAR_MORPH_DRUM1 + voice] = amount`

   This can be correct during normal runtime feedback, but it should not be
   required to repair file-load display state. The correct file-load behavior is
   to avoid changing these AVR display values in the first place.

4. STM sends a global morph report:

   - `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`
   - `SEQ_REPORT_GLOBAL_MORPH_MSB` handler
   - writes `parameter_values[PAR_MORPH] = amount`
   - then calls `avrCommsParser_syncVoiceMorphDisplayValues(amount)`

   This can overwrite all six individual display values during normal runtime,
   but it does not explain the observed background-load symptom where global
   morph remains positive while the six individual voice morph display values
   become zero. During protected file-load receive, generic `SEQ_CC` messages are
   assembled, but the completed-message `rxDisable` gate only allows flow
   messages and `SEQ_BACKGROUND_SWAP_DONE` through. `SEQ_REPORT_GLOBAL_MORPH_LSB`
   and `SEQ_REPORT_GLOBAL_MORPH_MSB` are therefore suppressed during the protected
   load window. That means the AVR global morph display can simply retain its
   previous positive value.

5. STM endpoint restore sends normal parameter values:

   - `mainboard/LxrStm32/src/Preset/EndpointRestore.c`
   - `preset_endpointRestoreSendNextFull(0)` / masked restore
   - emits `PRF_RESTORE_PARAM_CC` / `PRF_RESTORE_PARAM_CC2`
   - AVR receives them in
     `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`
   - restore handlers write `parameter_values[param] = value`

   This is the path that matches the observed symptom. On the STM side,
   `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` are inside `END_OF_SOUND_PARAMETERS`, so
   the restore stream can include those parameter IDs. During protected
   file-load receive, `PRF_RESTORE_PARAM_CC` and `PRF_RESTORE_PARAM_CC2` are
   explicitly allowed through the `rxDisable` gate. If the kit endpoint array
   contains zero for these IDs, the AVR restore handler overwrites the displayed
   individual morph values with zero while global morph remains unchanged.

### Intended Fix Direction

Disable file-load restore/report writes that target AVR morph display state
instead of adding another refresh afterwards.

The narrow fix should be:

- In the AVR restore handlers for `PRF_RESTORE_PARAM_CC` and
  `PRF_RESTORE_PARAM_CC2`, do not write `parameter_values[]` when the target
  parameter is `PAR_MORPH`, `PAR_MORPH_DRUM1`, `PAR_MORPH_DRUM2`,
  `PAR_MORPH_DRUM3`, `PAR_MORPH_SNARE`, `PAR_MORPH_CYM`, or
  `PAR_MORPH_HIHAT` and the AVR is in file-load protected receive state
  (`avrCommsParser_rxDisable`).
- In the AVR `SEQ_REPORT_GLOBAL_MORPH_MSB` handler, do not write
  `parameter_values[PAR_MORPH]` and do not call
  `avrCommsParser_syncVoiceMorphDisplayValues(amount)` while file-load protected
  receive is active.
- Leave the existing user-edit paths alone:
  - AVR global morph edits should still update global and all six individual
    display slots.
  - AVR individual voice morph edits should still update their own display slot
    and send the value to STM.
  - Normal non-file-load STM reports can remain as runtime synchronization.

This keeps AVR morph display values unchanged throughout `.pat`, `.prf`, and
`.all` load operations, matching the intended rule that file load does not alter
global morph or individual voice morph values.

No new code for this intended fix has been applied yet.

## Exact Planned Line Changes For Voice Morph Display Preservation

No implementation code is changed in this planning step.

The fix should only prevent protected file-load restore traffic from overwriting
the AVR-stored individual voice morph menu values. It should not request a new
refresh from STM, should not allow `VOICE_MORPH` through `rxDisable`, and should
not alter user-edit behavior.

### File: `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

1. Add a local helper after `avrCommsParser_syncVoiceMorphDisplayValues()`.

Current location:

- line 43: `static void avrCommsParser_syncVoiceMorphDisplayValues(uint8_t amount)`
- line 49: closing brace of that helper
- line 51: `static void avrCommsParser_handleVoiceMorphReport(uint8_t slot, uint8_t payload)`

Insert between current lines 49 and 51:

```c
static uint8_t avrCommsParser_isVoiceMorphDisplayParam(uint16_t paramNr)
{
   return paramNr >= PAR_MORPH_DRUM1 && paramNr <= PAR_MORPH_HIHAT;
}
```

Reason: the restore handlers need one explicit local test for the six
individual voice morph menu values. These are the only AVR display values that
are observed to become zero while the STM voice morph/audio state remains
correct.

2. Change the `PRF_RESTORE_PARAM_CC` write.

Current lines 734-740:

```c
else if(avrCommsParser_command.status == PRF_RESTORE_PARAM_CC)
{
   // RESTORE: Update main parameters only. Do not touch parameters2 (morph parameter endpoint)
   // to avoid corrupting morph state during a display-only synchronization.
   parameter_values[avrCommsParser_command.data1]=avrCommsParser_command.data2;
   avrCommsParser_restoreCount++;
}
```

Replace only the assignment line, current line 738, with:

```c
if(!avrCommsParser_rxDisable
   || !avrCommsParser_isVoiceMorphDisplayParam(avrCommsParser_command.data1))
   parameter_values[avrCommsParser_command.data1]=avrCommsParser_command.data2;
```

Leave `avrCommsParser_restoreCount++` in place. The restore stream was still
received and processed; it just must not overwrite the AVR menu's stored voice
morph display values during file-load protection.

3. Change the `PRF_RESTORE_PARAM_CC2` write.

Current lines 741-750:

```c
else if(avrCommsParser_command.status == PRF_RESTORE_PARAM_CC2)
{
   uint16_t paramNr = (uint16_t)(avrCommsParser_command.data1+128);
   if(paramNr < NUM_PARAMS)
   {
      parameter_values[paramNr]=avrCommsParser_command.data2;
      avrCommsParser_restoreCount++;
      // RESTORE: Parameters2 mirroring explicitly removed for restore dumps.
   }
}
```

Replace only the assignment line, current line 746, with:

```c
if(!avrCommsParser_rxDisable
   || !avrCommsParser_isVoiceMorphDisplayParam(paramNr))
   parameter_values[paramNr]=avrCommsParser_command.data2;
```

Leave the `paramNr < NUM_PARAMS` guard, `avrCommsParser_restoreCount++`, and the
existing parameters2 comment unchanged.

### Lines That Should Not Change

- Do not change the `VOICE_MORPH` receive gates. The failed attempt to allow
  `VOICE_MORPH` through while `avrCommsParser_rxDisable` is active should remain
  reverted.
- Do not change `avrCommsParser_handleVoiceMorphReport()`. It should continue to
  update `parameter_values[PAR_MORPH_DRUM1 + voice]` during normal runtime
  reports.
- Do not change the `SEQ_REPORT_GLOBAL_MORPH_MSB` handler for this fix. During
  protected file-load receive, the completed-message `rxDisable` gate already
  prevents ordinary global morph report processing; the observed symptom is
  better explained by the explicitly allowed `PRF_RESTORE_PARAM_CC/CC2` restore
  writes.
- Do not change `front/LxrAvr/Menu/menu.c`. User edits to `PAR_MORPH` should
  still reset the six individual display values to the new global morph amount,
  and user edits to `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` should still update their
  own menu values and send the edit to STM.

### Expected Result

During `.pat`, `.prf`, and `.all` file loads, if `avrCommsParser_rxDisable` is
active, restore traffic will no longer change the six AVR-stored individual
voice morph display values. The values shown on the PERF menu should therefore
remain whatever they were before the load started, matching the STM voice morph
state and the unchanged audible morph behavior.

## Implementation Notes: Guard Restore Writes During File Load

Implemented the exact planned AVR guard.

File changed:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

Changes:

- Added `avrCommsParser_isVoiceMorphDisplayParam()` near the existing morph
  display helpers.
- In `PRF_RESTORE_PARAM_CC`, the `parameter_values[]` assignment is skipped only
  when both conditions are true:
  - `avrCommsParser_rxDisable` is active;
  - the target parameter is `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`.
- In `PRF_RESTORE_PARAM_CC2`, the same guard is applied after `paramNr` is
  reconstructed and bounds-checked.

No `VOICE_MORPH` receive gates were changed. No STM refresh/report path was
added. Normal non-file-load restore behavior still writes these display values,
because the guard only blocks them while file-load receive protection is active.

### Re-test Result: Guard Was Not The Source

This AVR restore-traffic guard did not fix the symptom. The guard was removed
from `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`; the notes are kept
here only as a failed attempt record.

Important new observation:

- background loading off still produces the same symptom for `.all` and `.prf`;
- global morph display remains valid;
- all six individual voice morph display values become zero;
- `.pat` loading does not produce the symptom.

That rules out the background temp-switch restore traffic as the primary cause.
The reset happens in the ordinary AVR file-load path for file types that load
kit/performance endpoint data.

## Corrected Assessment: AVR File Load Meta Copy Zeros Voice Morph Display

The real zeroing path is in `front/LxrAvr/Preset/presetManager.c`, not in the
AVR receive parser.

### Relevant Parameter Layout

In `front/LxrAvr/Parameters.h`:

- `END_OF_INDIVIDUAL_VOICE_PARAMS` is line 290.
- `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` are lines 305-310.
- `END_OF_SOUND_PARAMETERS` is line 330.

Therefore the individual voice morph display values are not part of the
per-voice parameter masks. They are in the drumset meta range copied by
`preset_readDrumsetMeta(0)`:

```c
END_OF_INDIVIDUAL_VOICE_PARAMS <= PAR_MORPH_DRUM1..PAR_MORPH_HIHAT < END_OF_SOUND_PARAMETERS
```

### `.all` Load Flow

In `preset_loadAll()`:

- lines 2420-2421:
  - `preset_readKitToTemp(1);`
  - `preset_readKitToTemp(0);`
- lines 2447-2450:
  - for full `.all` loads, call `preset_readDrumsetMeta(0);`
  - then call `preset_readDrumsetMeta(1);`

`preset_readKitToTemp(0)` reads a full kit endpoint block from the `.all` file
into `parameter_values_fileLoadSnapshot`:

- line 484:

```c
res=f_read((FIL*)&kitRead_File, para,END_OF_SOUND_PARAMETERS, &bytesRead);
```

If the file does not provide bytes through the current
`END_OF_SOUND_PARAMETERS`, the unread tail is zero-filled:

- lines 512-514:

```c
if(END_OF_SOUND_PARAMETERS-bytesRead)
   memset(para+bytesRead,0,END_OF_SOUND_PARAMETERS-bytesRead);
```

Then `preset_readDrumsetMeta(0)` copies the entire meta range from that snapshot
into live AVR menu storage:

- lines 660-663:

```c
for (i=0;i<END_OF_SOUND_PARAMETERS-END_OF_INDIVIDUAL_VOICE_PARAMS;i++)
{
   parameter_values[END_OF_INDIVIDUAL_VOICE_PARAMS+i]=
      parameter_values_fileLoadSnapshot[END_OF_INDIVIDUAL_VOICE_PARAMS+i];
}
```

Because `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` are inside that copied range, any
zeroes in the file snapshot overwrite the AVR-stored individual voice morph menu
values.

### `.prf` Load Flow

In `preset_loadPerf()`:

- lines 2661-2662:
  - `preset_readKitToTemp(1);`
  - `preset_readKitToTemp(0);`
- lines 2682-2685:
  - for full `.prf` loads, call `preset_readDrumsetMeta(0);`
  - then call `preset_readDrumsetMeta(1);`

This reaches the same `preset_readDrumsetMeta(0)` bulk copy at lines 660-663 and
therefore overwrites the same six AVR menu display slots.

### Why `.pat` Load Does Not Do This

`.pat` load does not call `preset_readKitToTemp()` or
`preset_readDrumsetMeta(0)`. It only loads pattern data, so it never bulk-copies
the kit meta range that contains `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`.

### Correct Fix Direction

The fix should be in `preset_readDrumsetMeta(0)`, not in the AVR receive parser.

When copying the drumset meta range from
`parameter_values_fileLoadSnapshot[]` into `parameter_values[]`, skip
`PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`.

That preserves the AVR-stored individual voice morph menu values through `.all`
and `.prf` file loads, while leaving ordinary saved kit/performance parameters
to load normally.

Open question before implementation:

- `PAR_MORPH` itself is outside `END_OF_SOUND_PARAMETERS`, so it is not touched
  by this meta-copy path. That matches the observation that global morph display
  remains valid.

## Follow-up Attempt: END_OF_KIT Boundary Plus STM Restore Guard

Retest showed the first `END_OF_KIT_PARAMETERS` changes were not sufficient:
after `.all`/`.prf` load, the six AVR individual voice morph menu values still
displayed zero.

### Re-checked AVR Boundary Uses

The `END_OF_SOUND_PARAMETERS` uses in `front/LxrAvr/Menu/menu.c`,
`front/LxrAvr/buttonHandler.c`, AVR `parameters2[]`, `paramToModTarget[]`, and
the file-load temp/save arrays should stay as `END_OF_SOUND_PARAMETERS`. Those
paths are runtime/morph/automation parameter domains, and the per-voice morph
amount params must remain inside them.

The suspicious remaining file-load path was
`front/LxrAvr/Preset/presetManager.c::preset_dumpEndpointsToStm()`:

```c
for (i = 0; i < END_OF_SOUND_PARAMETERS; i++)
   avrComms_sendData(PRF_RESTORE_PARAM_CC/CC2, ..., parameter_values[i]);

for (i = 0; i < END_OF_SOUND_PARAMETERS; i++)
   avrComms_sendData(PRF_RESTORE_MORPH_CC/CC2, ..., parameters2[i]);
```

Even after the file read/meta-copy path uses `END_OF_KIT_PARAMETERS`, this dump
still crossed into `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`. During `.all`/`.prf`
loads it forwards loaded endpoint state to STM, so it should use the same
file-backed kit boundary and not transmit non-file-backed morph amount display
params as if they were loaded kit bytes.

Changed both endpoint dump loops to:

```c
for (i = 0; i < END_OF_KIT_PARAMETERS; i++)
```

This keeps the per-voice morph amount params available for runtime automation
and modulation-node handling, but excludes them from file-backed kit endpoint
dumps.

### Re-added AVR rxDisable Guard

Also re-added the AVR receive guard in
`front/LxrAvr/avrComms/avrCommsReceivingProtocol.c` in case STM endpoint restore
traffic is also writing the AVR menu values during file-load protection.

Added:

```c
static uint8_t avrCommsParser_isVoiceMorphDisplayParam(uint16_t paramNr)
{
   return paramNr >= PAR_MORPH_DRUM1 && paramNr <= PAR_MORPH_HIHAT;
}
```

Then guarded `PRF_RESTORE_PARAM_CC` and `PRF_RESTORE_PARAM_CC2` writes:

```c
if(!avrCommsParser_rxDisable
   || !avrCommsParser_isVoiceMorphDisplayParam(paramNr))
   parameter_values[paramNr] = value;
```

The guard only blocks protected file-load receive from overwriting the AVR menu
voice morph display values. It does not change normal runtime `VOICE_MORPH`
reports and does not request any new refresh from STM.

### Retest Result

Confirmed fixed.

With the endpoint dump loops changed to `END_OF_KIT_PARAMETERS` and the AVR
`rxDisable` guard restored for `PRF_RESTORE_PARAM_CC/CC2`, `.all` and `.prf`
loads no longer reset the six individual voice morph values shown in the PERF
menu. The individual voice morph display values now remain aligned with the
unchanged STM morph/audio state through the file load.
