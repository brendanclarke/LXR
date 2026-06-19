# Session 027 Handoff Log

DATE: 2026-06-19
SESSION GOAL: Fix live recording of front-panel parameter edits to step automation, then fix the remaining encoder `DTYPE_MENU` wrong-list-item behavior.
COMPLETED: Live-recorded step automation now stores raw AVR/menu parameter destinations and converts only at playback/application boundaries. External MIDI automation recording now converts MIDI-domain destinations back to raw storage before writing steps. Encoder edits of `DTYPE_MENU` parameters now normalize out-of-range raw byte values before applying the encoder delta, preventing the first detent from clamping to the last menu item. The clear menu encoder-button press now executes the selected clear operation instead of falling through to the generic shift/edit press action.
VERIFIED ON HARDWARE: Yes. User reported the live-record automation fix checked out, including the Drum 2 volume case that previously automated Drum 1 volume. User then reported the `DTYPE_MENU` encoder fix seems to work.

CHANGES THIS SESSION:
- `mainboard/LxrStm32/src/DSPAudio/automationNode.c`: added a single helper that converts raw stored step automation destinations into the `MidiMsg` shape expected by `frontParser_applyParameterCommand()`. Destination `0` / `NO_AUTOMATION` is treated as off. Raw low destinations `1..127` become `MIDI_CC data1 = destination + 1`; raw high destinations above 127 become `MIDI_CC2 data1 = destination - 128`.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`: kept front-panel low live edits recording `rawParam`, and removed the old low-destination `val++` from `FRONT_SET_P1_DEST` / `FRONT_SET_P2_DEST`, so explicit step-destination edits store the same raw destination convention as live recording.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`: removed the old low-destination `dest--` from `frontPanelSending_sendStepParamsReply()`, so raw step destinations round-trip back to AVR unchanged. `frontPanelSending_sendParameterEcho()` was audited and intentionally left with its low `paramNr - 1` conversion because its current MIDI parser callers pass MIDI-domain/apply-domain parameter ids.
- `mainboard/LxrStm32/src/Sequencer/sequencer.h` / `sequencer.c`: documented `seq_recordAutomation()` as raw-destination storage and added `seq_recordAutomationMidiDestination()` for call sites that receive MIDI-domain destination ids.
- `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c` / `GlobalMidiParser.c`: changed external MIDI live-record call sites to use `seq_recordAutomationMidiDestination()`, preserving correct behavior for MIDI-domain CC/CC2 destinations while step storage moved to raw ids.
- `front/LxrAvr/Menu/menu.c`: added `menu_getDtypeMenuEntryCount()` and `menu_normalizeDtypeMenuEncoderValue()`. Both regular and shift/morph encoder edit paths now normalize `DTYPE_MENU` values before applying `menu_applyEncoderDeltaToByte()`, and both final clamps now guard `numEntries == 0`.
- `front/LxrAvr/Menu/menu.c`: added an encoder-button carveout for active clear mode. Pressing the encoder while the SHIFT+COPY clear menu is shown now calls `copyClear_executeClear()` and returns before the generic edit-mode toggle.
- `firmware image/FIRMWARE.BIN`: rebuilt after the source changes.
- `AUDIT_LIVE_REC_WRONG_PARAMS.md`: temporary audit captured the live-record diagnosis and implementation notes; its content is folded into this handoff.
- `ENC_DTYPE_MENU_WRONG.md`: temporary audit captured the encoder diagnosis and implementation notes; its content is folded into this handoff.

KNOWN ISSUES INTRODUCED: None known from hardware testing. The AVR build still reports the existing `menu_resetActiveParameter` sign-conversion warning at `front/LxrAvr/Menu/menu.c:3312`.
KNOWN ISSUES RESOLVED: Front-panel live recording no longer records the previous low-bank parameter for step automation. Drum 2 volume live recording no longer automates Drum 1 volume. Encoder edit of `DTYPE_MENU` parameters no longer jumps/clamps to the last list item when the stored value begins outside the compact list-index domain. Encoder press on the clear menu now executes the selected clear target.

NEXT SESSION RECOMMENDED GOAL: Clean up temporary root audit files after confirming the handoff is sufficient, then continue with the next feature or bug from hardware testing.
BLOCKERS: None known. Preserve the raw step automation destination convention unless new hardware testing proves otherwise.

CRITICAL REMINDERS FOR NEXT SESSION:
- Step automation destinations in `Step.param1Nr` / `Step.param2Nr` are now raw AVR/menu `PAR_*` ids. Do not reintroduce low `+1/-1` storage in the step editor round-trip.
- The low `+1` conversion belongs only at apply/playback boundaries that feed `frontParser_applyParameterCommand()` with `MIDI_CC`.
- External MIDI CC recording enters from the MIDI-domain/apply-domain side and must continue to use `seq_recordAutomationMidiDestination()`.
- `frontPanelSending_sendParameterEcho()` still subtracts one for low parameters because its current callers pass MIDI-domain ids. Do not "fix" it to raw ids without auditing those callers.
- Encoder `DTYPE_MENU` edits are a value-domain problem, not a parameter-index problem.
- Clear menu encoder rotation and encoder press both need explicit clear-mode carveouts in `menu_parseEncoder()`: rotation selects `copyClear_clearTarget`, press executes `copyClear_executeClear()`.

---

## Session Context

The session began with a front-panel live-record bug:

- Record is enabled.
- A user turns a potentiometer for a sound parameter.
- The audible live parameter changes correctly.
- The step automation recorded to the active lane targets a different parameter on playback.

The concrete hardware symptom was:

- live-record Drum 2 volume to Drum 2's sequencer track, first automation lane;
- playback automates Drum 1 volume instead.

Later in the session, a separate encoder symptom was clarified:

- clicking/using encoder edit mode selects the correct parameter;
- if that parameter is `DTYPE_MENU`, the encoder changes to the wrong menu item;
- the value clamps to the last item in the list.

The older root `ENC_LIST_MISMATCH_ERROR.md` was treated as suspect because it described the wrong encoder symptom. The final encoder diagnosis came from the refined symptom above, not from that older file.

## Live-Record Automation Diagnosis

The first audit found a mixed parameter-id convention around low parameters.

AVR menu parameter ids are raw `PAR_*` enum indices. For example:

- `PAR_VOL1 = 88`
- `PAR_VOL2 = 89`

When the AVR sends ordinary low sound edits, it sends the raw menu parameter id as `MIDI_CC data1`. The STM front-panel receive path then converts that low raw id before live application:

```c
uint8_t rawParam = frontParser_command.data1;
MidiMsg liveMsg = frontParser_command;

liveMsg.data1 = (uint8_t)((rawParam + 1) & 0x7f);
```

That conversion is required because `frontParser_applyParameterCommand()` treats low `MIDI_CC data1` as one-based and computes:

```c
const uint16_t paramNr = msg.data1 - 1;
```

So raw `PAR_VOL2 = 89` must be applied as low `MIDI_CC data1 = 90` to reach internal parameter 89.

The original narrow diagnosis was:

- live DSP application used the converted `liveMsg.data1`, so the audible edit was correct;
- live recording stored `rawParam`, so playback later sent low destination 89;
- `frontParser_applyParameterCommand()` interpreted low destination 89 as parameter 88;
- parameter 88 is Drum 1 volume.

That exactly described the Drum 2 volume -> Drum 1 volume failure.

## Rejected Narrow Attempt

The first attempted fix changed the front-panel low live-record call from:

```c
seq_recordAutomation(frontParser_activeTrack, rawParam, frontParser_command.data2);
```

to:

```c
seq_recordAutomation(frontParser_activeTrack, liveMsg.data1, frontParser_command.data2);
```

That was reverted after hardware testing still produced Drum 1 volume when recording Drum 2 volume. The conclusion was that preserving the old mixed convention was the wrong repair. The correct repair was to make the persistent step automation storage convention explicit and consistent.

## Final Automation Convention

`Step.param1Nr` and `Step.param2Nr` now store raw AVR/menu `PAR_*` destinations.

This means:

- front-panel live recording stores `rawParam`;
- explicit front-panel step destination edits store the raw destination sent by AVR;
- step parameter replies send raw stored destinations back to AVR unchanged;
- automation playback converts raw stored destinations into the low/high `MidiMsg` shape needed by `frontParser_applyParameterCommand()`;
- external MIDI recording converts MIDI-domain destination ids back into raw destinations before writing steps.

The important low-bank boundary is:

- raw storage id `89` means `PAR_VOL2`;
- playback converts raw `89` to `MIDI_CC data1 = 90`;
- `frontParser_applyParameterCommand()` subtracts one and applies internal parameter `89`.

High-bank destinations remain direct high ids for storage:

- raw high destination above 127 is stored as its raw value;
- playback emits `MIDI_CC2 data1 = destination - 128`.

Destination `0` is treated as no automation/off. `autoNode_setDestination()` stores `NO_AUTOMATION` for `dest == 0`, and the new automation-node helper refuses to emit an apply message for destination `0` or `NO_AUTOMATION`.

## External MIDI Recording Boundary

The audit called out that external MIDI recording was not the same as front-panel raw parameter storage.

MIDI parser call sites operate in the MIDI/apply-domain destination convention. Before Session 027, those call sites passed `LXRparamNr` directly to `seq_recordAutomation()`.

After the storage convention changed, those call sites were moved to:

```c
seq_recordAutomationMidiDestination(frontParser_activeTrack, LXRparamNr, msg.data2);
```

That helper converts low MIDI-domain ids back to raw storage:

- `NO_AUTOMATION` stays `NO_AUTOMATION`;
- low ids `1..127` become `dest - 1`;
- high ids remain as-is.

This preserved external MIDI live-record behavior while making pattern storage raw.

## Step Editor Round Trip

Before Session 027, the manual step destination receive path incremented low non-zero destinations before storing them:

```c
if(val && val < 128)
   val++;
```

The send-back path decremented low non-zero destinations before returning them to AVR.

Those two conversions were removed. The step editor now round-trips the same raw destination id that AVR uses in `modTargets[]`.

This is the piece that made the live-record fix succeed on hardware: both ways of writing `Step.param1Nr` / `param2Nr` now use the same raw storage convention.

## `frontPanelSending_sendParameterEcho()` Audit

`frontPanelSending_sendParameterEcho()` was specifically checked because it still subtracts one for low parameters:

```c
if(paramNr < 128)
{
   frontPanelSending_sendByte(PARAM_CC);
   frontPanelSending_sendByte((uint8_t)(paramNr - 1));
}
```

That remained intentional. Current callers in the MIDI parser pass MIDI-domain/apply-domain parameter ids, not raw AVR/menu ids. The echo path is therefore still converting from the apply-domain low id back to the AVR-facing raw id for display feedback.

Do not change this helper unless its callers are also changed to pass raw ids.

## Automation Affected Scope

The fixed bug affected front-panel live recording for low sound parameters. It was easy to see on volume parameters:

- Drum 2 volume raw id 89 had been played back as id 88.
- Drum 3 volume raw id 90 would have played back as id 89.

The same off-by-one risk applied to other low-bank oscillator, envelope, filter, pan, drive, decimation, and related parameters when recorded from front-panel live edits.

High/front `CC_2` parameters were less likely to be affected by the same low-bank bug because high destinations already carry the high-bank offset as `data1 + 128`.

Voice morph live edits were separate. AVR sends them via dedicated `VOICE_MORPH` traffic, not the generic low `MIDI_CC` path audited here.

## Encoder `DTYPE_MENU` Diagnosis

The encoder symptom was independent of the automation `+1/-1` bug.

The correct refined symptom:

- encoder edit mode selects and edits the correct parameter;
- if the selected parameter is `DTYPE_MENU`, the edited value jumps/clamps to the last menu list item.

The cause was a value-domain mismatch. The encoder edit path assumed the stored value for `DTYPE_MENU` was already a compact menu list index:

- valid range: `0..getMaxEntriesForMenu(menuId)-1`;
- each encoder detent adds or subtracts one list index.

The old shape was:

```c
menu_applyEncoderDeltaToByte(paramValue, inc);
...
if(*paramValue >= numEntries)
   *paramValue = (uint8_t)(numEntries - 1);
```

If `parameter_values[paramNr]` contained a raw byte such as `64`, `127`, or `255`, the first encoder movement left it above `numEntries`; the final clamp then forced the value to the last item.

The pot path did not show the same issue because it maps raw pot bytes into compact menu indices:

```c
return (uint8_t)(frac * (numEntries - 1));
```

Likely sources of out-of-range `DTYPE_MENU` bytes include restore/report/receive paths that assign `data2` directly into `parameter_values[]`, including `PARAM_CC`, `PARAM_CC2`, `PRF_RESTORE_PARAM_CC`, and `PRF_RESTORE_PARAM_CC2`.

## Encoder `DTYPE_MENU` Fix

The final fix was intentionally local to the encoder edit surface.

Added:

- `menu_getDtypeMenuEntryCount(uint16_t paramNr)`;
- `menu_normalizeDtypeMenuEncoderValue(uint16_t paramNr, uint8_t *paramValue)`.

Before applying the encoder delta, both edit paths now do:

```c
if(dtype == DTYPE_MENU)
   menu_normalizeDtypeMenuEncoderValue(paramNr, paramValue);
```

The normalizer behavior:

- if `numEntries == 0`, set value to `0`;
- if the current value is already below `numEntries`, leave it untouched;
- otherwise treat the stored value as a raw byte and map it proportionally into `0..numEntries-1`;
- then the normal one-detent encoder delta is applied.

Both the regular edit path and the shift/morph endpoint edit path were updated:

- `menu_encoderChangeParameter()`;
- `menu_encoderChangeShiftParameter()`.

The final `DTYPE_MENU` clamp in both paths was also guarded so `numEntries == 0` cannot underflow to `255`.

This is separate from live-record automation indexing. The encoder was not choosing the wrong parameter; it was starting from a stored value outside the compact menu-list value domain.

## Clear Menu Encoder-Press Fix

After the Session 027 fixes, one small clear-menu bug remained:

- SHIFT+COPY opened the clear menu correctly;
- rotating the encoder selected the correct target: track, pattern, automation 1, automation 2;
- pressing the encoder did not execute the selected clear operation;
- instead, the click fell through to the generic encoder press behavior for the current shifted context.

The code already had a clear-mode carveout for encoder rotation inside `menu_parseEncoder()`:

```c
if(copyClear_isClearModeActive()) {
   ...
   copyClear_setClearTarget(target);
   return;
}
```

The missing piece was the encoder button edge path earlier in the same function. Before the fix, any encoder press toggled `editModeActive` before clear mode could consume it.

The fix adds this early branch:

```c
if(btnClicked && copyClear_isClearModeActive())
{
   lastEncoderButton = button;
   copyClear_executeClear();
   return;
}
```

This keeps the existing clear action implementation unchanged and only routes the encoder press to it while clear mode is active.

## Verification

Builds run during Session 027:

- `make -C mainboard/LxrStm32 -j4 stm32` passed after the automation storage/playback fix.
- `make firmware` passed after the STM automation fix and rebuilt `firmware image/FIRMWARE.BIN`.
- `make -C front/LxrAvr avr -j4` passed after the encoder `DTYPE_MENU` fix.
- `make firmware` passed again after the AVR encoder fix and rebuilt `firmware image/FIRMWARE.BIN`.
- `make -C front/LxrAvr avr -j4` passed after the clear-menu encoder-press fix.
- `make firmware` passed again after the clear-menu fix and rebuilt `firmware image/FIRMWARE.BIN`.

The AVR build still reports the existing sign-conversion warning in `menu_resetActiveParameter`:

```text
front/LxrAvr/Menu/menu.c:3312:30: warning: unsigned conversion from 'int' to 'uint8_t' changes the value of '-8'
```

Hardware verification:

- user reported the live-record automation fix checked out;
- user reported the encoder `DTYPE_MENU` fix seems to work.

## Documentation Closeout

This handoff absorbs the temporary root audit files:

- `AUDIT_LIVE_REC_WRONG_PARAMS.md`;
- `ENC_DTYPE_MENU_WRONG.md`.

Those files can be deleted after this closeout. The old root `ENC_LIST_MISMATCH_ERROR.md` should not be treated as authoritative for the final `DTYPE_MENU` symptom; it was explicitly suspect once the user clarified that the encoder selected the correct parameter and only the list item/value was wrong.

Comms/reference docs updated during closeout:

- `knowledge_files/log_archive/000_SESSION_INDEX.md`;
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`;
- `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`;
- `MEMORY.md`.
