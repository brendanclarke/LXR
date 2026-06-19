# Live Recording Writes Wrong Automation Parameters

## Summary

Live recording from front-panel parameter edits is recording the wrong destination for low-numbered sound parameters (`< 128`). The front-panel live parameter receive path correctly converts the incoming AVR raw parameter number into the STM/MIDI-style controller number for live DSP application, but it records automation using the unconverted raw number.

The sequencer automation playback path expects low automation destinations in the STM/MIDI-style form, where ordinary low parameters are offset by `+1`. Because live recording stores the raw AVR index instead, playback applies the recorded value to the previous parameter.

This exactly explains the reported case:

- AVR parameter enum: `PAR_VOL1 = 88`, `PAR_VOL2 = 89` in `front/LxrAvr/Parameters.h`.
- When Drum 2 volume is edited, the AVR sends low live CC with `data1 = 89`.
- STM live apply converts that to `data1 = 90`, so the audible live edit correctly changes Drum 2 volume.
- STM live record stores automation destination `89`.
- Automation playback sends destination `89` as a low `MIDI_CC`.
- `frontParser_applyParameterCommand()` interprets low `MIDI_CC data1` as `paramNr = data1 - 1`, so destination `89` applies parameter `88`: Drum 1 volume.

## Relevant Code Paths

### AVR sends raw menu parameter ids for low sound edits

In `front/LxrAvr/Menu/menu.c`, normal sound parameters below 128 are sent as `MIDI_CC` with the raw AVR parameter number:

- `menu_encoderChangeParameter()`
- default sound-parameter path
- `avrComms_sendData(MIDI_CC, (uint8_t)paramNr, dtypeValue)`

Relevant lines:

- `front/LxrAvr/Menu/menu.c:3991` sends low sound parameters as `MIDI_CC`.
- `front/LxrAvr/Parameters.h:122` defines `PAR_VOL1 = 88`.
- `front/LxrAvr/Parameters.h:123` defines `PAR_VOL2 = 89`.

This raw AVR id is correct for storage/endpoint purposes and is already handled by the STM live receive path.

### STM live receive converts correctly for live DSP apply

In `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`, the low `MIDI_CC` receive case does this:

```c
uint8_t rawParam = frontParser_command.data1;
MidiMsg liveMsg = frontParser_command;

liveMsg.data1 = (uint8_t)((rawParam + 1) & 0x7f);
...
preset_storeParameterIngress(rawParam, frontParser_command.data2);
frontParser_applyParameterCommand(liveMsg,0);
seq_recordAutomation(frontParser_activeTrack, rawParam, frontParser_command.data2);
```

Relevant lines:

- `frontPanelReceivingProtocol.c:1761` captures `rawParam`.
- `frontPanelReceivingProtocol.c:1764` converts `liveMsg.data1` to `rawParam + 1`.
- `frontPanelReceivingProtocol.c:1768` stores the endpoint using the raw parameter.
- `frontPanelReceivingProtocol.c:1769` applies the converted live message.
- `frontPanelReceivingProtocol.c:1772` records automation using the unconverted raw parameter.

The first three actions are coherent: raw storage, converted live apply. The fourth action is the mismatch.

### Low `MIDI_CC` apply is one-based on STM

`frontParser_applyParameterCommand()` converts low `MIDI_CC` data back to an internal parameter by subtracting one:

```c
const uint16_t paramNr = msg.data1-1;
```

Relevant lines:

- `frontPanelReceivingProtocol.c:174` enters low `MIDI_CC` apply.
- `frontPanelReceivingProtocol.c:177` computes `paramNr = msg.data1 - 1`.

So a playback destination of `90` applies internal parameter `89` (`PAR_VOL2`). A playback destination of `89` applies internal parameter `88` (`PAR_VOL1`).

### Manual step automation destination editing stores the converted destination

The step editor path already knows about this offset. When AVR sends a step automation destination, it sends a raw parameter from `modTargets[value].param`:

- `front/LxrAvr/Menu/menu.c:3499` reads the raw parameter from `modTargets`.
- `front/LxrAvr/Menu/menu.c:3501` sends `SET_P1_DEST` / `SET_P2_DEST`.

STM receives that destination and increments low non-zero values before storing them in the step:

```c
uint8_t val = (hi<<7)|lo;
if(val && val < 128 )
   val++;
pat_getStepPtr(...)->param1Nr = val;
```

Relevant lines:

- `frontPanelReceivingProtocol.c:1840` receives `FRONT_SET_P1_DEST`.
- `frontPanelReceivingProtocol.c:1846` increments low non-zero destinations.
- `frontPanelReceivingProtocol.c:1848` stores `param1Nr`.
- `frontPanelReceivingProtocol.c:1851` receives `FRONT_SET_P2_DEST`.
- `frontPanelReceivingProtocol.c:1857` increments low non-zero destinations.
- `frontPanelReceivingProtocol.c:1859` stores `param2Nr`.

STM also decrements low stored destinations when sending step parameters back to AVR:

- `frontPanelSendingProtocol.c:226` loads `step->param1Nr`.
- `frontPanelSendingProtocol.c:227` decrements low non-zero destinations before sending.
- `frontPanelSendingProtocol.c:241` / `frontPanelSendingProtocol.c:242` do the same for lane 2.

This means the explicit step editor path round-trips correctly. The bug is specific to live recording from front-panel live parameter edits.

### Sequencer playback consumes stored destinations as apply-ready tokens

On playback, `seq_parseAutomationNodes()` copies `step->param1Nr` / `param2Nr` into automation nodes:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c:330` reads `param1Nr`.
- `sequencer.c:331` reads `param2Nr`.
- `sequencer.c:367` calls `autoNode_setDestination()`.
- `sequencer.c:370` calls `autoNode_updateValue()`.

`autoNode_updateValue()` sends the destination directly as `MIDI_CC data1` for low destinations:

- `mainboard/LxrStm32/src/DSPAudio/automationNode.c:71` handles high destinations.
- `automationNode.c:75` handles low destinations.
- `automationNode.c:76` sets `msg.data1 = node->destination`.
- `automationNode.c:79` applies the message via `frontParser_applyParameterCommand()`.

Therefore low stored automation destinations must already include the `+1` required by `frontParser_applyParameterCommand()`.

## Exact Failure Walkthrough: Drum 2 Volume

1. Front-panel user edits Drum 2 volume.
2. AVR parameter id is `PAR_VOL2 = 89`.
3. AVR sends low live edit as `MIDI_CC, data1 = 89, data2 = value`.
4. STM low live receive path sets:
   - `rawParam = 89`
   - `liveMsg.data1 = 90`
5. STM live DSP apply uses `90`, which maps to internal parameter `89`; Drum 2 volume changes correctly.
6. STM live recording calls `seq_recordAutomation(..., rawParam, value)`, storing destination `89`.
7. On playback, automation node sends `MIDI_CC data1 = 89`.
8. `frontParser_applyParameterCommand()` computes `paramNr = 89 - 1 = 88`.
9. Parameter `88` is `PAR_VOL1`, so Drum 1 volume is automated.

## Affected Scope

This should affect front-panel live recording for low sound parameters `1..127`, not just Drum 2 volume. The recorded automation destination will be one parameter lower than the edited parameter when played back.

Examples implied by the code:

- `PAR_VOL2 = 89` records destination `89`, playback applies `PAR_VOL1 = 88`.
- `PAR_VOL3 = 90` records destination `90`, playback applies `PAR_VOL2 = 89`.
- Similar off-by-one behavior should appear for low oscillator, envelope, filter, pan, drive, decimation, and other low-bank parameters.

The following paths appear less likely to be affected by this specific bug:

- High/front `CC_2` parameters (`>= 128`): the live receive path records `frontParser_command.data1 + 128`, and automation playback converts high destinations back with `dest - 128`. That path appears internally consistent.
- Manual step automation target editing: STM explicitly increments low non-zero destinations before storing them.
- External MIDI CC recording: `ChannelMidiParser.c` records the STM/MIDI-style `LXRparamNr` values, not the AVR raw `PAR_*` values. A broad change inside `seq_recordAutomation()` would risk breaking this path.

Voice morph live edits are a separate special case. AVR sends them via `VOICE_MORPH`, not the generic low `MIDI_CC` path audited here.

## Likely Fix Shape

Superseded by the implementation notes below. The initial narrow hypothesis was useful for isolating the mixed raw/MIDI-domain convention, but hardware testing showed the one-line record-call change was not sufficient.

The narrow fix should be in the STM front-panel low `MIDI_CC` receive case, not in `seq_recordAutomation()` globally.

Current problematic line:

```c
seq_recordAutomation(frontParser_activeTrack, rawParam, frontParser_command.data2);
```

The recorded destination should use the same low apply-ready token as step automation playback expects, i.e. the converted low destination. A direct local fix would be conceptually:

```c
seq_recordAutomation(frontParser_activeTrack, liveMsg.data1, frontParser_command.data2);
```

or an explicit helper/name that makes the domains clear:

```c
uint8_t automationDest = (uint8_t)((rawParam + 1) & 0x7f);
...
seq_recordAutomation(frontParser_activeTrack, automationDest, frontParser_command.data2);
```

Do not move the conversion into `seq_recordAutomation()` unless all callers are audited and updated, because MIDI parser call sites already pass playback-style destination numbers.

## Suggested Verification

After applying the narrow fix:

1. Build STM firmware.
2. Record Drum 2 volume from the front panel into Drum 2 track lane 1.
3. Inspect/play back the step and confirm lane 1 targets Drum 2 volume, not Drum 1 volume.
4. Repeat one high-bank parameter (`>= 128`) to confirm no regression.
5. Repeat manual step-destination editing for the same Drum 2 volume target to confirm the existing editor round-trip remains correct.

## Implementation Notes

### Reverted Narrow Attempt

A first narrow attempt changed the low front-panel live-record call from:

```c
seq_recordAutomation(frontParser_activeTrack, rawParam, frontParser_command.data2);
```

to:

```c
seq_recordAutomation(frontParser_activeTrack, liveMsg.data1, frontParser_command.data2);
```

That was reverted after hardware testing still produced Drum 1 volume when recording Drum 2 volume. The failure means the safer fix is not to preserve the old mixed convention, but to remove the mixed convention from step automation storage.

### Current Fix Attempt

The current patch treats `Step.param1Nr` / `Step.param2Nr` as raw AVR/`PAR_*` parameter ids. Conversion to the MIDI-shaped ids expected by `frontParser_applyParameterCommand()` now happens at the automation playback boundary.

Changed pieces:

- `frontPanelReceivingProtocol.c`: `FRONT_SET_P1_DEST` and `FRONT_SET_P2_DEST` now store the raw destination sent by AVR. The old low-target `val++` was removed.
- `frontPanelSendingProtocol.c`: step-parameter replies now send the raw stored destination back to AVR. The old low-target `dest--` was removed.
- `automationNode.c`: automation playback now converts raw low destinations to `MIDI_CC data1 = destination + 1` before calling `frontParser_applyParameterCommand()`. Destination `0` is treated as no automation, matching `PAR_NONE` / off in the AVR target list.
- `sequencer.h` / `sequencer.c`: `seq_recordAutomation()` is documented as raw-destination storage, and `seq_recordAutomationMidiDestination()` was added for MIDI parser call sites that still work in the MIDI CC enum domain.
- `ChannelMidiParser.c` / `GlobalMidiParser.c`: external MIDI live-record call sites now use `seq_recordAutomationMidiDestination()` so low MIDI-domain ids are converted back to raw ids before being stored in pattern steps.

This leaves the front-panel live-record path at:

```c
seq_recordAutomation(frontParser_activeTrack, rawParam, frontParser_command.data2);
```

which now matches the raw step automation storage convention.

Related but separate: the encoder `DTYPE_MENU` last-item clamp is not this `+1/-1` bug. That symptom is a value-domain mismatch where `parameter_values[param]` is out of the compact list-index range before encoder delta/clamp runs. Notes for that issue are in `ENC_DTYPE_MENU_WRONG.md`.
