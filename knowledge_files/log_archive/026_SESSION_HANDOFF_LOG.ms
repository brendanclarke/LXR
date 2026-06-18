# LXR -bc- Enhanced Firmware — Session 026 Handoff Log

**Project**: LXR -bc- Enhanced Firmware  
**Session goal**: Bring the individual voice morph controls on the AVR PERF menu online, preserve the existing morph-worker/LFO overlay model, repair the MIDI/velocity reporting paths that affect PERF display state, and document the unresolved encoder list mismatch separately.  
**Last session summary**: Session 025 disabled the legacy macro path end-to-end and refreshed the comms/memory docs to treat macro traffic as deprecated historical context.  
**Working repository**: `/Users/bc/LXR01/LXR-current/LXR` on branch `master-avr-fp-clean`; the voice-morph implementation remains present, while the failed encoder-list experiments were reverted by the user before this closeout.  
**Constraints today**: Do not change firmware logic during closeout. Preserve the voice-morph behavior that tested acceptably, document the unresolved encoder-list bug as findings only, and refresh durable knowledge files.

Key files to be aware of:

- `VOICE_MORPH_CONTROLS_AUDIT2.md`
- `ENC_LIST_MISMATCH_ERROR.md`
- `front/LxrAvr/Menu/menu.c`
- `front/LxrAvr/avrComms/avrCommsSendingProtocol.c/.h`
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c/.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c/.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h`
- `mainboard/LxrStm32/src/Preset/MorphEngine.c/.h`
- `mainboard/LxrStm32/src/Preset/EndpointRestore.c`
- `mainboard/LxrStm32/src/DSPAudio/modulationNode.c`
- `mainboard/LxrStm32/src/MIDI/MidiOutputControl.c`
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
- `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`
- `MEMORY.md`

## Session 026 Summary

Session 026 brought the existing PERF-page individual voice morph entries online for `DRUM1`, `DRUM2`, `DRUM3`, `SNARE`, `CYM`, and `HIHAT`. The AVR now treats those controls as full `0..255` values, sends them over a dedicated two-packet voice-morph transport, and receives STM display reports for per-voice morph values. The STM decodes those packets into direct per-voice current morph writes in `Preset`, without routing them through ordinary CC2 parameter application or the 7-bit automation scaling path.

Global morph remains authoritative: setting global morph stores the full global amount, copies it into all six current/base voice morph amounts, reports the global amount back to AVR where appropriate, and keeps the six PERF voice morph display slots coherent with the global override.

The session also repaired the menu-display side of MIDI CC1 morph changes. Global CC1 now scales to `0..255`, applies the same global override semantics, reports the full global display value, and reports all six overridden voice values. Voice-channel CC1 still uses the existing 7-bit automation input, but the resulting full `0..255` individual voice amount is reported back to the AVR PERF menu.

Velocity-to-voice-morph went through several passes. The final intended behavior is trigger-time only: when a velocity destination is set to one of the `PAR_MORPH_*` targets, the generic velocity modulation node must not own or apply `TYPE_UINT8_VMORPH`. Instead, note-on calls `preset_applyVelocityVoiceMorphOnTrigger()`, which computes a full-range morph amount from velocity and velocity-mod depth, writes only the target voice's current voice morph value, and reports that value to the AVR menu. LFO-to-voice-morph was intentionally left as the async morph-drain overlay and was not rewritten.

The later encoder-list mismatch investigation did not produce a valid fix. The attempted acceleration/list-count patches were reverted. Findings and the recommended next trace are archived in root `ENC_LIST_MISMATCH_ERROR.md`.

## Implementation Details

### AVR PERF voice morph controls

`front/LxrAvr/Menu/menu.c` now treats the six PERF voice morph amount parameters as full byte controls:

- `PAR_MORPH_DRUM1`
- `PAR_MORPH_DRUM2`
- `PAR_MORPH_DRUM3`
- `PAR_MORPH_SNARE`
- `PAR_MORPH_CYM`
- `PAR_MORPH_HIHAT`

Their dtypes are `DTYPE_0B255`, matching the user-facing requirement that the front panel edits and displays the full `0..255` value.

The menu send path gained local helpers:

- `menu_isVoiceMorphAmountParam()`
- `menu_sendVoiceMorphAmount()`
- `menu_syncVoiceMorphDisplayValues()`

Normal PERF edits intercept the six voice morph params before the generic `CC_2` path and send dedicated voice-morph packets instead. Shifted morph-endpoint edits remain separate and still use the morph endpoint restore/send path; normal voice morph amount edits are not morph endpoint writes.

Global morph menu edits still use the existing global morph control path, but the AVR also copies the new global value into all six individual voice morph display slots immediately so the UI reflects the override.

### AVR voice morph transport

`front/LxrAvr/avrComms/avrCommsSendingProtocol.c/.h` gained:

```c
void avrComms_sendVoiceMorphValue(uint8_t voice, uint8_t amount);
```

The helper sends a 7-bit-safe low/high pair on the existing `VOICE_MORPH` status byte:

```text
VOICE_MORPH, voice,     amount & 0x7f
VOICE_MORPH, voice + 6, (amount >> 7) & 0x01
```

This mirrors the global morph `LSB/MSB` pattern and avoids sending raw bytes `>= 128` through the MIDI-shaped parser.

`front/LxrAvr/avrComms/avrCommsReceivingProtocol.c` now treats `VOICE_MORPH` from STM as display-only report traffic:

- slots `0..5` cache the low seven bits per voice;
- slots `6..11` commit the high bit plus cached low bits;
- the committed value updates `parameter_values[PAR_MORPH_DRUM1 + voice]`;
- the receive path calls `menu_repaint()`;
- it does not echo the value back to STM.

The existing global morph report receive path now also copies the committed global value into all six individual voice morph display slots.

### STM voice morph receive

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c` gained a `FRONT_SEQ_VOICE_MORPH` handler that mirrors the AVR packet shape:

- slots `0..5` cache low bits;
- slots `6..11` combine and commit;
- the commit calls `seq_setVoiceMorphAmount(voice, morphAmount)`.

That call reaches the direct full-range Preset setter. The path does not call the ordinary CC/CC2 parameter application helper and does not call the 7-bit automation setter.

Global morph receive now accepts the full `0..255` two-packet value through `FRONT_SEQ_SET_GLOBAL_MORPH_LSB/MSB` and calls the global morph setter once the high packet arrives.

### STM voice morph reports

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h` gained report helpers:

- `frontPanelSending_sendVoiceMorphReport()`
- `frontPanelSending_sendVoiceMorphRuntimeReport()`
- `frontPanelSending_sendVoiceMorphRuntimeReports()`
- `frontPanelSending_sendVoiceMorphReports()`
- `frontPanelSending_sendVoiceMorphReportsFromKit()`

There are two send styles:

- priority/wait reports for restore/display-sync transactions;
- non-blocking runtime reports for MIDI CC1 and trigger-time velocity updates.

`EndpointRestore.c` now sends the six per-voice morph display reports immediately after the global morph report during endpoint restore, using the kit image involved in that restore. This keeps temp/normal boundary display sync coherent.

### Preset/MorphEngine behavior

`mainboard/LxrStm32/src/Preset/MorphEngine.c/.h` now has a getter:

```c
uint8_t preset_getVoiceMorphAmount(uint8_t synthVoice);
```

The voice morph setters have distinct jobs:

- `preset_setVoiceMorphAmount()` is the direct full-range setter used by the PERF menu transport.
- `preset_setVoiceMorphAutomationValue()` remains the 7-bit MIDI CC1 automation setter, scales to `0..255`, writes the live/current amount, and reports the full value to AVR.
- `preset_setGlobalMorphAutomationValue()` scales the 7-bit MIDI CC1 value to `0..255`, applies global override semantics, reports global morph, and reports all six individual voice morph values.
- `preset_setGlobalMorphAmount()` remains the authoritative full-range global morph setter and overwrites the per-voice current/base amounts.

The LFO morph overlay path was not changed. `preset_modulateVoiceMorphAmount()` remains the async overlay layer consumed by the morph drain, not a menu/base-value writer.

### Velocity-to-voice-morph final shape

The final velocity behavior is intentionally split away from generic modulation nodes:

- `FRONT_CC_VELO_TARGET` still stores `PAR_MORPH_*` as the selected destination in Preset automation target state.
- If the destination is `PAR_MORPH_*`, `frontPanelReceivingProtocol.c` assigns `PAR_NONE` to the live generic `velocityModulators[]` node.
- `modulationNode.c` also guards stale/accidental generic velocity `TYPE_UINT8_VMORPH` application so velocity nodes do not call the generic VMORPH path.
- `voiceControl_noteOn()` calls `preset_applyVelocityVoiceMorphOnTrigger()`.
- `preset_applyVelocityVoiceMorphOnTrigger()` reads the stored velocity destination for the triggering/source voice, resolves the target morph voice, computes `(velocity / 127) * (mod depth / 127) * 255`, writes only the target voice current morph amount with `preset_setVoiceMorphLiveAmount()`, and sends a non-blocking voice morph runtime report to AVR.

This means velocity voice morph is applied once at voice trigger time and does not interfere with LFO morph modulation.

### LFO morph investigation

During the session there was a suspected bug where LFO-to-voice-morph seemed to use the source LFO voice's morph value rather than the target voice's morph value. After discussion and user-side verification, that was determined to be working correctly in the current code. The bad patch attempt was undone and no final LFO morph code change belongs to this handoff.

The rule remains:

- LFO morph modulation is an overlay from the current target voice morph value toward the morph kit endpoint;
- it is consumed by the async morph drain;
- it must not write back to the PERF base/current voice morph display value unless another explicit control source changes that value.

### Encoder list mismatch

The encoder list mismatch was investigated late in the session. The attempted fixes were not valid and were reverted. The durable notes are in `ENC_LIST_MISMATCH_ERROR.md`.

Important status:

- The encoder-list bug is unresolved.
- Acceleration applied to `DTYPE_MENU` is a real secondary concern, but was not proven to be the root cause.
- Direct `PROGMEM` count reads in `getMaxEntriesForMenu()` are suspicious, but the attempted count-read patch did not solve the reported mismatch and was reverted.
- The next correct step is instrumentation of `paramNr`, active slot, displayed value, pre-edit value, encoder `inc`, acceleration multiplier, and post-clamp value for a failing list parameter.

## Verification

Builds completed during the implementation passes:

- `make -C front/LxrAvr avr -j4`
- `make -C mainboard/LxrStm32 -j4 stm32`
- `make firmware`

The usual existing warning set remained. The final closeout turn only changed documentation, so no firmware rebuild was needed after writing the logs/spec updates.

Hardware/user validation notes:

- The user reported the main voice morph and velocity-restored behavior seemed acceptable before moving on.
- The LFO morph concern was resolved by user-side verification as a misunderstanding of the current code behavior.
- The encoder-list bug is not fixed.

## Residual State

What is now active:

- AVR PERF voice morph amount controls are full `0..255`.
- Dedicated AVR-to-STM voice morph amount traffic is active on `VOICE_MORPH` / `FRONT_SEQ_VOICE_MORPH`.
- STM-to-AVR per-voice voice morph display reports are active.
- Global morph display sync also updates the individual voice morph display slots.
- MIDI CC1 global/voice morph changes report full-range values back to AVR.
- Velocity-to-voice-morph is trigger-time only and reports the target voice amount to AVR.

What remains intentionally unchanged:

- LFO-to-morph modulation is still an async overlay.
- Shifted morph endpoint editing still uses `parameters2[]` / morph endpoint restore traffic.
- The legacy macro path remains deprecated from Session 025.

What remains unresolved:

- Encoder text-list mismatch between pot/list display and encoder editing.

## End of Session Block

```text
DATE: 2026-06-18
SESSION GOAL: Bring individual voice morph controls to the PERF menu, keep morph-worker/LFO semantics intact, repair display reporting for global/voice/velocity morph sources, and document the unresolved encoder list mismatch.
COMPLETED: PERF voice morph amount controls now send and receive full 0..255 values through dedicated voice morph traffic; STM stores those values as current per-voice morph state for the morph worker; global morph overrides all six voices and reports coherent menu values; MIDI CC1 morph paths report full-range display values; velocity-to-voice-morph is a trigger-time current morph write outside the generic velocity node; encoder-list findings were written separately.
VERIFIED ON HARDWARE: partially; user reported the voice-morph behavior seemed OK and confirmed the suspected LFO morph bug was not a real bug. Encoder-list behavior remains unresolved.

CHANGES THIS SESSION:
- `front/LxrAvr/Menu/menu.c`: changed voice morph amount dtypes to `DTYPE_0B255`; added voice morph send/display helpers; intercepted voice morph amount edits before generic `CC_2`; global morph edits now mirror the value into all six voice morph display slots.
- `front/LxrAvr/avrComms/avrCommsSendingProtocol.c/.h`: added `avrComms_sendVoiceMorphValue()` low/high 7-bit-safe packet helper.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`: added per-voice voice morph report receive state; made `VOICE_MORPH` display-only; made global morph reports sync all six voice morph display slots.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c/.h`: decoded `FRONT_SEQ_VOICE_MORPH` low/high packets and routed them to the direct full-range voice morph setter; kept `PAR_MORPH_*` out of the live generic velocity modulation node.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h`: added priority restore reports and non-blocking runtime reports for per-voice morph display sync.
- `mainboard/LxrStm32/src/Preset/MorphEngine.c/.h`: added voice morph getter/report support; global/voice MIDI CC1 morph paths now report full-range values; restored trigger-time velocity-to-voice-morph as a current-value write only.
- `mainboard/LxrStm32/src/Preset/EndpointRestore.c`: sends all six individual voice morph reports after the existing global morph report.
- `mainboard/LxrStm32/src/DSPAudio/modulationNode.c`: prevents generic velocity nodes from applying `TYPE_UINT8_VMORPH`.
- `mainboard/LxrStm32/src/MIDI/MidiOutputControl.c`: calls the trigger-time velocity voice morph helper from `voiceControl_noteOn()`.
- `VOICE_MORPH_CONTROLS_AUDIT2.md`: kept working implementation and follow-up notes.
- `ENC_LIST_MISMATCH_ERROR.md`: records unresolved encoder-list findings and next instrumentation target.
- `knowledge_files/log_archive/000_SESSION_INDEX.md`, `knowledge_files/log_archive/026_SESSION_HANDOFF_LOG.ms`, `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`, `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`, `MEMORY.md`: session closeout and durable docs.

KNOWN ISSUES INTRODUCED: none intentionally left in the voice-morph feature; the encoder-list mismatch remains unresolved and the invalid attempted fixes were reverted.
KNOWN ISSUES RESOLVED: PERF voice morph controls are connected to STM current morph state; global/voice MIDI morph changes update PERF display values; velocity-to-voice-morph no longer travels through the generic velocity modulation node and is restored as a trigger-time current morph write.
NEXT SESSION RECOMMENDED GOAL: instrument and fix the encoder `DTYPE_MENU` list mismatch using the trace plan in `ENC_LIST_MISMATCH_ERROR.md`.
BLOCKERS: encoder-list fix needs targeted AVR trace or hardware observation to prove active parameter/value state at the moment of encoder edit.
CRITICAL REMINDERS FOR NEXT SESSION:
- Do not rewrite LFO-to-morph modulation; keep it as an async overlay from current voice morph value to morph endpoint.
- Do not route PERF voice morph amount edits through generic `CC_2` or the 7-bit automation setter.
- Do not let generic velocity modulation nodes own/apply `TYPE_UINT8_VMORPH`; velocity voice morph belongs in the trigger-time helper.
- Do not apply another encoder-list patch until the trace proves whether the encoder is editing the displayed parameter and whether the pre-edit value is actually the displayed list index.
```
