# Session 026 Voice Morph Controls Audit 2

## Goal

Bring the individual voice morph controls on the AVR PERF menu online for:

- `PAR_MORPH_DRUM1`
- `PAR_MORPH_DRUM2`
- `PAR_MORPH_DRUM3`
- `PAR_MORPH_SNARE`
- `PAR_MORPH_CYM`
- `PAR_MORPH_HIHAT`

The menu entries and labels already exist. The missing work is the live AVR<->STM
transport and display-sync path.

The target behavior is:

- AVR PERF menu edits send a full `0..255` amount to STM.
- STM stores that amount as the selected voice's current morph value in
  `Preset`, and nothing else.
- The existing morph worker consumes the stored amount asynchronously.
- Global morph remains authoritative: setting global morph overwrites all six
  voice morph values.
- Whenever STM reports a global morph value back to AVR, AVR should also show
  that same amount in all six individual voice morph menu slots.
- LFO-to-morph modulation remains a separate overlay layer in the async morph
  drain. Do not change that path.

Implementation was completed in this session. Notes below record the actual
edits that landed.

## Implementation Notes

- AVR PERF voice morph controls are being treated as `0..255` UI controls,
  separate from the existing 7-bit MIDI CC1 automation path.
- The existing `VOICE_MORPH` / `FRONT_SEQ_VOICE_MORPH` status byte is being
  reused with a low/high packet split, matching the plan below.
- Restore/display-sync traffic will report all six individual voice morph
  values after the existing global morph report.
- AVR send-side helper added:
  `avrComms_sendVoiceMorphValue(uint8_t voice, uint8_t amount)`.
- AVR PERF voice morph dtypes changed from `DTYPE_0B127` to `DTYPE_0B255`.
- AVR encoder and pot edit paths now intercept `PAR_MORPH_DRUM1..HAT` before
  generic `CC_2` transmission and send `VOICE_MORPH` low/high packets instead.
- AVR `VOICE_MORPH` receive handling is now display-only report handling for
  individual voice morph values; `MORPH_CC` remains inert legacy protection.
- STM `FRONT_SEQ_VOICE_MORPH` receive handling now combines low/high packets
  and calls `seq_setVoiceMorphAmount()`, not the scaled automation setter.
- STM send-side helper added:
  `frontPanelSending_sendVoiceMorphReport()` plus all-voices wrappers. Endpoint
  restore uses the `FromKit` wrapper so individual reports match the same kit
  image used by the existing global morph report.
- `preset_getVoiceMorphAmount()` added so the front-panel sender can report the
  current live voice morph values without reaching into kit internals.
- `EndpointRestore.c` now sends all six individual voice morph reports
  immediately after the existing global morph report.
- Build verification completed:
  `make -C front/LxrAvr avr -j4`,
  `make -C mainboard/LxrStm32 -j4 stm32`, and `make firmware` all passed.
  Warnings seen were the existing AVR/STM warning set, not new fatal errors.

## Follow-up Fix Notes

The first implementation pass brought the PERF menu transport online, but live
automation sources still needed menu-report coverage:

- Superseded by the fourth-pass hardware finding: velocity automation target
  `PAR_MORPH_*` must not apply voice morph at all. It is still stored as a
  selector value, but the live velocity node and trigger path ignore it.
- MIDI CC1 global morph should report the scaled full `0..255` global amount
  and the overridden per-voice amounts back to AVR.
- MIDI CC1 voice morph should report the scaled full `0..255` individual
  voice morph amount back to AVR.
- Trigger-time and MIDI-runtime reports should use non-blocking front-panel
  send helpers; the blocking priority helpers remain reserved for restore
  display-sync transactions.

Second-pass implementation notes:

- Added non-blocking runtime report helpers for global morph and voice morph
  display updates.
- `preset_setGlobalMorphAutomationValue()` now scales MIDI CC1 to `0..255`,
  applies the same global-override storage behavior as menu global morph, then
  reports global plus all six voice morph display values to AVR.
- `preset_setVoiceMorphAutomationValue()` now reports the scaled `0..255`
  voice morph value to AVR after updating the voice morph amount.
- Superseded by the fourth-pass fix: the trigger-time
  `preset_applyVelocityVoiceMorphOnTrigger()` path was removed.
- Second-pass verification completed:
  `make -C mainboard/LxrStm32 -j4 stm32` passed,
  `make firmware` passed, and `make -C front/LxrAvr avr -j4` had no AVR work
  pending. Warnings remained the existing STM warning set.

## Current Baseline

Relevant existing code:

- AVR parameter IDs exist in `front/LxrAvr/Parameters.h`.
- AVR PERF page entries exist in `front/LxrAvr/Menu/menuPages.h`.
- AVR text entries exist in `front/LxrAvr/Menu/MenuText.h`.
- STM parameter IDs exist in `mainboard/LxrStm32/src/Preset/ParameterArray.h`.
- STM parameter table maps the six params to `preset_vMorphAmount[1..6]` and
  tags them as `TYPE_UINT8_VMORPH` in
  `mainboard/LxrStm32/src/Preset/ParameterArray.c`.
- STM morph ownership already lives in
  `mainboard/LxrStm32/src/Preset/MorphEngine.c`.
- The direct full-range setter already exists:
  `preset_setVoiceMorphAmount(uint8_t synthVoice, uint8_t morphAmount)`.
- The 7-bit MIDI/automation setter already exists:
  `preset_setVoiceMorphAutomationValue(uint8_t synthVoice, uint8_t morphValue)`.
- Existing MIDI CC1/mod-wheel voice morph goes through
  `seq_setVoiceMorphMaskAutomationValue()` from
  `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c`.

Important distinction:

- MIDI CC1 is a 7-bit automation source and must keep using the scaled
  automation setter.
- The AVR PERF menu is an 8-bit UI source and must use the direct full
  `0..255` setter.

## Current Fault Lines

### AVR Value Range

In `front/LxrAvr/Menu/menu.c`, the six voice morph entries currently use
`DTYPE_0B127` in `parameter_dtypes[]`.

Plan:

- Change only these six entries to `DTYPE_0B255`.
- Leave ordinary MIDI/automation `0..127` behavior unchanged.

### AVR Generic Send Fallthrough

The PERF voice morph params are sound params above 127. Today they can fall
through the generic `CC_2` send path from:

- `menu_encoderChangeParameter()`
- `menu_parseKnobValue()`

That is not suitable because:

- raw values `>= 128` are not safe in the MIDI-shaped three-byte parser;
- STM's ordinary parameter apply path is not the intended live control path for
  `PAR_MORPH_*`;
- these controls are morph control state, not ordinary DSP CC2 parameter
  changes.

Plan:

- Add a dedicated AVR helper for voice morph amount sends.
- Intercept `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` before generic `MIDI_CC` /
  `CC_2` sends in both edit paths.
- Send the value with the dedicated voice morph transport.
- Do not use `menu_sendMorphParameterEndpoint()` for normal PERF voice morph
  edits; that helper is for shifted morph-endpoint editing.

### STM Generic Receive Path

`frontParser_applyParameterCommand()` and `preset_storeParameterIngress()` are
for normal parameter ingress. Voice morph menu edits should not become ordinary
DSP parameter writes.

Plan:

- Decode dedicated voice morph traffic in
  `frontPanelReceivingProtocol.c`.
- Route directly to `seq_setVoiceMorphAmount()` or
  `preset_setVoiceMorphAmount()`.
- Do not route through `preset_setVoiceMorphAutomationValue()`, because that
  scales `0..127` to `0..255`.

## Wire Protocol Plan

Use the existing semantic status byte family:

- AVR header: `VOICE_MORPH` is already `0xab`.
- STM header: `FRONT_SEQ_VOICE_MORPH` is already `0xab`.

This avoids spending a large block of new `SEQ_CC` opcodes and keeps the
traffic clearly separate from ordinary parameter CC/CC2 messages.

Packet shape:

```text
status = VOICE_MORPH / FRONT_SEQ_VOICE_MORPH
data1  = voice slot plus packet half
data2  = 7-bit payload
```

Encoding:

- `data1 = 0..5`: low 7 bits for `DRUM1..HIHAT`
- `data1 = 6..11`: high bit for `DRUM1..HIHAT`
- low packet `data2 = amount & 0x7f`
- high packet `data2 = (amount >> 7) & 0x01`

Examples:

```text
DRUM1 amount 200:
  VOICE_MORPH, 0, 72
  VOICE_MORPH, 6, 1

HIHAT amount 255:
  VOICE_MORPH, 5, 127
  VOICE_MORPH, 11, 1
```

The receiver stores the low packet temporarily and commits on the matching high
packet, matching the current global morph LSB/MSB style.

## AVR Code Changes

### `front/LxrAvr/avrComms/avrCommsSendingProtocol.h`

Add:

```c
void avrComms_sendVoiceMorphValue(uint8_t voice, uint8_t amount);
```

`voice` is `0..5`, where:

- `0 = DRUM1`
- `1 = DRUM2`
- `2 = DRUM3`
- `3 = SNARE`
- `4 = CYM`
- `5 = HIHAT`

### `front/LxrAvr/avrComms/avrCommsSendingProtocol.c`

Implement the helper as two packets:

```c
avrComms_sendData(VOICE_MORPH, voice, amount & 0x7f);
avrComms_sendData(VOICE_MORPH, voice + 6, (amount >> 7) & 0x01);
```

Guard invalid voices with `if(voice >= 6) return;`.

### `front/LxrAvr/Menu/menu.c`

Change these six dtype entries from `DTYPE_0B127` to `DTYPE_0B255`:

- `PAR_MORPH_DRUM1`
- `PAR_MORPH_DRUM2`
- `PAR_MORPH_DRUM3`
- `PAR_MORPH_SNARE`
- `PAR_MORPH_CYM`
- `PAR_MORPH_HIHAT`

Add a small predicate/helper near the menu send logic:

```c
static uint8_t menu_isVoiceMorphAmountParam(uint16_t paramNr);
static void menu_sendVoiceMorphAmount(uint16_t paramNr, uint8_t value);
```

Behavior:

- Predicate returns true for `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`.
- Voice index is `paramNr - PAR_MORPH_DRUM1`.
- Send with `avrComms_sendVoiceMorphValue(voice, value)`.

Intercept in `menu_encoderChangeParameter()` after clamping and before generic
sound-param send:

- If `paramNr` is one of the six voice morph amount params and this is not a
  shifted morph-endpoint edit, call `menu_sendVoiceMorphAmount()`.
- Do not send `CC_2`.
- Keep shifted endpoint edits using `menu_sendMorphParameterEndpoint()`.

Intercept in `menu_parseKnobValue()` in the default send path:

- If `paramNr` is one of the six voice morph amount params, call
  `menu_sendVoiceMorphAmount()`.
- Do not fall through to `CC_2`.

Global morph local UI update:

- In the existing `PAR_MORPH` case in `menu_parseGlobalParam()`, after
  `morphValue = value`, copy `value` into the six
  `parameter_values[PAR_MORPH_*]` slots.
- Keep the existing two-message `SEQ_SET_GLOBAL_MORPH_LSB/MSB` send.

### `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

Add receive-side state:

```c
static uint8_t avrCommsParser_reportVoiceMorphLsb[6];
```

Replace the current inert `VOICE_MORPH` receive handling with display-only
report handling:

- If `status == VOICE_MORPH` and `data1 < 6`, store low bits in
  `avrCommsParser_reportVoiceMorphLsb[data1]`.
- If `status == VOICE_MORPH` and `data1` is `6..11`, combine with the saved
  low bits and update `parameter_values[PAR_MORPH_DRUM1 + voice]`.
- Call `menu_repaint()` after committing the value.
- Do not send anything back to STM from this receive path.

In the existing `SEQ_REPORT_GLOBAL_MORPH_MSB` case:

- Keep updating `parameter_values[PAR_MORPH]` and `morphValue`.
- Also copy the same amount into all six `PAR_MORPH_*` menu values.
- Call `menu_repaint()`.

## STM Code Changes

### `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Add receive-side state:

```c
static uint8_t frontParser_voiceMorphLsb[6];
```

Add a helper:

```c
static void frontParser_handleVoiceMorph(uint8_t slot, uint8_t payload);
```

Behavior:

- If `slot < 6`, store `payload & 0x7f` in `frontParser_voiceMorphLsb[slot]`.
- If `slot` is `6..11`, compute `voice = slot - 6`, combine the high bit with
  the saved low bits, and call:

```c
seq_setVoiceMorphAmount(voice, amount);
```

or equivalently:

```c
preset_setVoiceMorphAmount(voice, amount);
```

Prefer the existing `Sequencer` compatibility wrapper if the surrounding file
already follows that pattern; either way the destination is the direct
`Preset` full-amount setter.

Add a top-level parser case for `FRONT_SEQ_VOICE_MORPH`:

```c
case FRONT_SEQ_VOICE_MORPH:
   frontParser_handleVoiceMorph(frontParser_command.data1,
                                frontParser_command.data2);
   break;
```

Do not call `frontParser_applyParameterCommand()` from this path.

Do not call `seq_setVoiceMorphAutomationValue()` or
`seq_setVoiceMorphMaskAutomationValue()` from this path.

### `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h`

Add:

```c
void frontPanelSending_sendVoiceMorphReport(uint8_t voice, uint8_t amount);
void frontPanelSending_sendVoiceMorphReports(void);
```

The all-voices helper is optional, but it keeps restore/override call sites
small and makes the reporting policy explicit.

### `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`

Implement `frontPanelSending_sendVoiceMorphReport()` as two priority packets,
mirroring global morph report style:

```c
frontPanelSending_sendPriorityByteWait(FRONT_SEQ_VOICE_MORPH);
frontPanelSending_sendPriorityByteWait(voice);
frontPanelSending_sendPriorityByteWait(amount & 0x7f);

frontPanelSending_sendPriorityByteWait(FRONT_SEQ_VOICE_MORPH);
frontPanelSending_sendPriorityByteWait(voice + 6);
frontPanelSending_sendPriorityByteWait((amount >> 7) & 0x01);
```

Use priority/wait sends because reports are part of restore/display sync, like
`frontPanelSending_sendGlobalMorphReport()`.

`frontPanelSending_sendVoiceMorphReports()` should:

- call `preset_syncVMorphAmountMirrorsFromLiveSources()` first if needed;
- send `preset_vMorphAmount[1]` through `preset_vMorphAmount[6]`, or read from
  the current image kit directly if a clean accessor is added.

The cleaner option is to add a Preset accessor, described below, so the send
layer does not reach deeper into morph storage than necessary.

### `mainboard/LxrStm32/src/Preset/MorphEngine.h`

Add accessors for report code:

```c
uint8_t preset_getVoiceMorphAmount(uint8_t synthVoice);
void preset_reportVoiceMorphAmountsToFrontPanel(void);
```

The report helper can live in `MorphEngine.c` if `MorphEngine.c` includes the
sending protocol header, but that adds a Preset -> front-panel dependency. The
lower-coupling option is:

- add only `preset_getVoiceMorphAmount()`;
- keep packet construction in `frontPanelSendingProtocol.c`;
- call the sending helper from `EndpointRestore.c`.

Preferred plan: add only the getter unless the implementation becomes awkward.

### `mainboard/LxrStm32/src/Preset/MorphEngine.c`

Add:

```c
uint8_t preset_getVoiceMorphAmount(uint8_t synthVoice)
{
   const PresetKitState *kit;
   if(synthVoice >= PRESET_SYNTH_VOICES)
      return 0;
   kit = preset_getMorphKitForImage(preset_getMorphImageForVoice(synthVoice));
   return kit->voiceMorphAmount[synthVoice];
}
```

Keep existing setter semantics:

- `preset_setGlobalMorphAmount()` continues to write `globalMorphAmount`,
  `voiceMorphBaseAmount[]`, and `voiceMorphAmount[]` for all six voices.
- `preset_setVoiceMorphAmount()` continues to write
  `voiceMorphBaseAmount[synthVoice]`, `voiceMorphAmount[synthVoice]`, and
  `preset_vMorphAmount[synthVoice + 1]`.
- `preset_setVoiceMorphAutomationValue()` remains scaled 7-bit automation.
- `preset_modulateVoiceMorphAmount()` remains an overlay and must not be
  rewritten to update base/current menu state.

No change should be made to:

- `preset_serviceMorphInterpolation()`
- the phase-0 baseline morph scan;
- the LFO overlay phase;
- the removed trigger-time velocity-to-morph path;
- `modNode_vMorph()`.

### `mainboard/LxrStm32/src/Preset/EndpointRestore.c`

At the same point where endpoint restore currently sends global morph report:

```c
if(preset_endpointRestoreCurrent.reportGlobalMorph)
   frontPanelSending_sendGlobalMorphReport(...);
```

Extend that display-sync transaction:

- send the global morph report as it does now;
- send all six individual voice morph reports immediately after it;
- then send `frontPanelSending_sendRestoreDone()`.

This makes temp/normal boundary restore and global override display sync happen
in one coherent restore bracket.

If global morph just overwrote all voices via `preset_setGlobalMorphAmount()`,
the six reports will naturally carry the overwritten values.

## Optional Coherence Change

`mainboard/LxrStm32/src/Preset/ParameterArray.c` declares
`TYPE_UINT8_VMORPH`, but `paramArray_setParameter()` has no case for it.

This implementation does not need generic parameter writes for the PERF menu
path, because the dedicated protocol calls `preset_setVoiceMorphAmount()`.

Still, adding a small case is reasonable for future-proofing:

```c
case TYPE_UINT8_VMORPH:
   preset_setVoiceMorphAmount(preset_morphVoiceForParam(idx), newValue.itg);
   break;
```

Only do this if it is clear that `idx` is available in the function and the
call will not create recursion through the parameter table. If there is any
doubt, leave this out and keep voice morph updates explicit in the protocol
handler.

## Explicit Non-Changes

Do not change the MIDI CC1 voice morph path:

- `ChannelMidiParser.c` should keep routing CC1/mod-wheel to
  `seq_setVoiceMorphMaskAutomationValue()`.
- That path is intentionally 7-bit and scaled through
  `preset_morphAutomationValueToAmount()`.

Do not change LFO morph modulation:

- LFO morph modulation should continue to modulate from the current voice morph
  value toward the morph endpoint as a separate async overlay.
- `preset_modulateVoiceMorphAmount()` should keep reading
  `kit->interpolatedParams[param]` as its baseline and applying an overlay
  against `kit->morphEndpointParams[param]`.
- `preset_serviceMorphInterpolation()` should keep phase 0 as baseline morph
  and later phases as LFO overlay work.

Do not revive macro behavior or cache/session opcodes.

Do not alter shifted morph-endpoint editing or `parameters2[]` behavior.

## Verification Plan For The Implementation Turn

Builds:

- `make -C front/LxrAvr avr -j4`
- `make -C mainboard/LxrStm32 -j4 stm32`
- `make firmware`

Manual tests:

1. On PERF page, confirm `d1m`, `d2m`, `d3m`, `svm`, `cvm`, and `hvm` edit
   across `0..255`.
2. Set each individual voice morph value and confirm only that voice's morph
   amount changes on STM.
3. Set global morph and confirm all six PERF voice morph displays update to the
   global value.
4. Confirm the global morph sound behavior is unchanged.
5. Confirm MIDI CC1 on the global MIDI channel still applies global morph via
   the existing 7-bit automation path.
6. Confirm MIDI CC1 on voice channels still applies individual voice morph via
   the existing 7-bit automation path.
7. Confirm LFO-to-morph modulation still works as an overlay and does not write
   back to the PERF menu value.
8. Switch normal/temp playback images and confirm AVR receives display-only
   reports for global and individual voice morph values without echo traffic.

Suggested temporary instrumentation if hardware behavior is ambiguous:

- Toggle/log inside STM `frontParser_handleVoiceMorph()` after combining the
  8-bit amount.
- Toggle/log inside AVR `VOICE_MORPH` report receive after committing the menu
  value.
- Remove any instrumentation before closeout.

## Third-Pass Fix Notes: Velocity Target Isolation

Hardware testing still showed velocity-to-individual-voice-morph topping out at
roughly the old 7-bit/half-range behavior when driven through the CC1 path. The
front-panel ingress already prevented `FRONT_CC_VELO_TARGET` from assigning
`PAR_MORPH_*` to a live velocity mod node, but there was still a second generic
path in `modulationNode.c`: any modulation node whose destination type was
`TYPE_UINT8_VMORPH` could call `modNode_vMorph()` from
`modNode_setDestination()` or `modNode_updateValue()`.

That generic path was not the desired behavior for velocity-to-morph. This
third-pass note was later superseded by the fourth-pass hardware finding:
velocity target "Individual Voice Morph" should not apply voice morph from
velocity at all.

Applied STM change:

- Added a local `modNode_isVelocityModTarget()` helper in
  `mainboard/LxrStm32/src/DSPAudio/modulationNode.c`.
- In `modNode_setDestination()`, if the node is one of the six
  `velocityModulators[]` and the requested destination is `TYPE_UINT8_VMORPH`,
  the live modulation-node destination is forced to `PAR_NONE`.
- The old-destination VMORPH reset in `modNode_setDestination()` now skips
  velocity nodes, so stale velocity VMORPH state cannot write another morph
  amount while being cleared.
- The new-destination VMORPH apply in `modNode_setDestination()` now skips
  velocity nodes.
- `modNode_updateValue()` now ignores `TYPE_UINT8_VMORPH` for velocity nodes if
  stale state ever reaches that function.

Expected result:

- Other velocity modulation destinations still use the generic velocity node
  path unchanged.
- Velocity destination "Individual Voice Morph" is stored in the kit automation
  target state, but it is not assigned to the live generic velocity mod node.
- Superseded: no trigger-time velocity-to-voice-morph code path should apply.
- LFO-to-morph remains unchanged. LFO morph still uses `lastVal` as the overlay
  input consumed by the async morph drain and does not write the PERF base value.

Verification after third-pass fix:

- `make -C mainboard/LxrStm32 -j4 stm32` passes. Existing warnings remain:
  duplicate `const` specifiers in `modulationNode.c` and RWX load segment from
  the linker.
- `make firmware` passes and regenerates `firmware image/FIRMWARE.BIN`.
- `make -C front/LxrAvr avr -j4` reports nothing to be done; no AVR files were
  changed in this pass.

## Fourth-Pass Fix Notes: Remove Velocity Voice-Morph Application

Hardware testing showed morph was still being applied whenever the velocity
modulation target was set to "Individual Voice Morph". The third-pass mod-node
guard only removed the generic velocity-node writer; it missed the explicit
sequencer trigger bridge:

```c
preset_applyVelocityVoiceMorphOnTrigger(voiceNr, vol);
```

That bridge was still reading the stored velocity destination, detecting
`PAR_MORPH_*`, computing a morph amount from trigger velocity/depth, and writing
the voice morph amount on every trigger.

Applied STM correction:

- Removed the `preset_applyVelocityVoiceMorphOnTrigger()` call from
  `seq_triggerVoice()`.
- Removed `preset_applyVelocityVoiceMorphOnTrigger()` and its trigger-voice
  conversion helper from `MorphEngine.c`.
- Removed the obsolete declaration from `MorphEngine.h`.
- Removed the stale commented-out `preset_setVelocityVoiceMorphAmount()` helper.
- Updated the `FRONT_CC_VELO_TARGET` comment in
  `frontPanelReceivingProtocol.c`: `PAR_MORPH_*` is stored as an automation
  target selector, but it is not assigned to the live generic velocity mod node
  and is not applied at trigger time.

Current expected behavior:

- Velocity modulation target "Individual Voice Morph" does not apply voice
  morph at all.
- Other velocity modulation targets still apply through the normal
  `velocityModulators[]` path.
- MIDI CC1 on voice/global channels still directly sets voice/global morph via
  the dedicated morph automation path, independent of velocity destination.
- Step automation and front-panel PERF voice morph controls still set voice
  morph amounts through their dedicated paths.
- LFO morph modulation remains an async overlay and is unchanged.

Verification after fourth-pass fix:

- `rg` over `mainboard/LxrStm32/src` finds no remaining
  `preset_applyVelocityVoiceMorphOnTrigger`,
  `preset_setVelocityVoiceMorphAmount`, or old `Velocity-to-VMORPH` comment.
- `make -C mainboard/LxrStm32 -j4 stm32` passes. Existing warning set remains.
- `make firmware` passes and regenerates `firmware image/FIRMWARE.BIN`.
- `make -C front/LxrAvr avr -j4` reports nothing to be done.
