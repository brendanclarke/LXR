# LXR -bc- Enhanced Firmware — Session 017 Handoff Log

**Project**: LXR -bc- Enhanced Firmware  
**Session goal**: Finish the remaining Preset refactor cleanup for the current plan, verify the Phase 10/11 rename and rehome work, and preserve the follow-up notes for the comms and temp-load split in the permanent docs.  
**Last session summary**: Session 016 finalized the AVR encoder on the Timer1 16 kHz fixed-rest FSM and left the firmware in a stable state for the next refactor pass.  
**Working repository**: `/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod` on branch `custom-develop-patload-envmod`.  
**Constraints today**: Keep the refactor focused on the current Preset/comms cleanup, do not reintroduce the retired `seq_`/`Seq*` Preset-owned names, and preserve the verified firmware behavior.

Key files to be aware of:
- `mainboard/LxrStm32/src/Preset/KitState.c`
- `mainboard/LxrStm32/src/Preset/KitState.h`
- `mainboard/LxrStm32/src/Preset/MorphEngine.c`
- `mainboard/LxrStm32/src/Preset/MorphEngine.h`
- `mainboard/LxrStm32/src/Preset/ParameterIngress.c`
- `mainboard/LxrStm32/src/Preset/ParameterIngress.h`
- `mainboard/LxrStm32/src/Preset/EndpointRestore.c`
- `mainboard/LxrStm32/src/Preset/EndpointRestore.h`
- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c`
- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.h`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/Sequencer/sequencer.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c`
- `mainboard/LxrStm32/src/DSPAudio/modulationNode.c`
- `mainboard/LxrStm32/src/Preset/ParameterArray.c`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`
- `knowledge_files/session_in_flight/COMMS_FLOW_SPEC.md`
- `knowledge_files/session_in_flight/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`
- `knowledge_files/log_archive/017_SESSION_HANDOFF_LOG.md`
- `MEMORY.md`

## Session 017 Summary

Session 017 completed the remaining Preset cleanup and then moved the durable notes into the permanent docs.

The code-side results were:

- Phase 10 live-apply ownership stayed in `Preset`, with the remaining bridge helpers renamed to the `preset_` surface and the Sequencer-facing callers switched over.
- Phase 11 finished the remaining Preset-owned rename sweep:
  - `SeqKitState` and `SeqKitAutomationTargets` became `PresetKitState` and `PresetAutomationTargets`;
  - `SeqEndpointRestoreRequest` became `PresetEndpointRestoreRequest`;
  - the Preset-owned `seq_` state aliases were removed from `sequencer.h`;
  - the temp-switch alias block was removed from `TempPlaybackSwitch.h`;
  - the Preset-owned call sites were switched to the canonical `preset_*` names.
- The remaining `seq_` names in the tree are now Sequencer-owned compatibility or control APIs, not Preset-owned storage aliases.
- Phase 12 was rechecked as a retrospective only; the live state lookup helpers in `sequencer.c` are still Sequencer orchestration, not a missed ownership split.

The documentation results were:

- `PRESET_CONSOLIDATION_AUDIT.md` was expanded so the Phase 12 section records the useful follow-up note: the remaining front-panel UART emitters are protocol-split work, not more Preset ownership cleanup.
- `MIDI_UART_SPLIT_AUDIT.md` was expanded to map the current direct-send callers and the future `frontPanelSendingProtocol.c/.h` contract.
- `COMMS_FLOW_SPEC.md` and `TEMPORARY_PAT_PARAM_LOAD_SPEC.md` were refreshed so the useful refactor notes live in the permanent reference docs, not only in the temporary audit.
- `000_SESSION_INDEX.md` was updated with the terse Session 017 summary.
- `MEMORY.md` was updated so the current context points at the permanent session/spec docs rather than the temporary audit notes.

## Detailed Refactor Results

### Phase 10 completion

The remaining live-apply helpers were kept under `Preset` ownership and the callers were switched over to the `preset_` API surface.

That included:

- `preset_applyVoiceAutomationTargets()`
- `preset_applyNormalEndpointAutomationTargets()`
- the associated `PresetAutomationTargets` and `PresetKitState` type usage

The important design rule remained the same:

- `ParameterIngress` owns the inbound storage/apply boundary;
- `KitState` owns the runtime image selection;
- `KitState` and `EndpointRestore` own the state that lives inside `Preset`;
- the rest of the tree should talk to those owners directly rather than through old `seq_` aliases.

### Phase 11 rename sweep

The remaining Preset-owned compatibility names were removed or replaced:

- `mainboard/LxrStm32/src/Preset/KitState.h`
  - `SeqKitState` was renamed to `PresetKitState`;
  - `SeqKitAutomationTargets` was renamed to `PresetAutomationTargets`;
  - the exported kit globals and getter return types now use the new type names.

- `mainboard/LxrStm32/src/Preset/EndpointRestore.c/.h`
  - `SeqEndpointRestoreRequest` was renamed to `PresetEndpointRestoreRequest`;
  - the restore queue and helper signatures now use `PresetKitState`.

- `mainboard/LxrStm32/src/Preset/MorphEngine.c/.h`
  - all kit and automation helper signatures now use `PresetKitState` and `PresetAutomationTargets`;
  - the live-apply and morph helper internals were updated to match the renamed types.

- `mainboard/LxrStm32/src/Preset/ParameterIngress.c/.h`
  - ingress helper signatures now use `PresetKitState` and `PresetAutomationTargets`;
  - the internal ingress-selection helpers were updated to match the renamed types.

- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.h/.c`
  - the legacy `seq_*` temp-switch alias macros were removed from the header;
  - the implementation now reads and writes `preset_voiceSourceState` directly.

- `mainboard/LxrStm32/src/Sequencer/sequencer.h`
  - the Preset-owned `seq_*` re-export macros were removed;
  - the handshake mailbox re-export was removed;
  - the compatibility wrappers now return `PresetKitState*`.

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
  - direct Preset-owned state accesses now use `preset_*` names and `preset_tempPlaybackSwitchState.*` fields;
  - restore and temp-switch bookkeeping was aligned with the renamed types and state fields.

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`
  - the parser now includes `Preset/EndpointRestore.h` directly for the restore handshake mailboxes;
  - all remaining Preset-owned state access uses the canonical `preset_*` names.

- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
  - the remaining morph-load disable checks now use `preset_morphLoadDisabled`.

- `mainboard/LxrStm32/src/DSPAudio/modulationNode.c`
  - the morph-load guard now uses `preset_morphLoadDisabled`.

- `mainboard/LxrStm32/src/Preset/ParameterArray.c`
  - the morph parameter pointers now bind to `preset_vMorphAmount`.

### Phase 12 review

Phase 12 remains a retrospective only.

The useful conclusion is unchanged:

- the `seq_live*` helpers in `sequencer.c` are orchestration helpers;
- they combine pattern selection, active-track selection, and timing context before reading pattern data;
- they should stay in Sequencer unless a later refactor reveals a more specific ownership split.

The additional follow-up item that matters for future work is the UART send path:

- the remaining direct AVR-facing transmit calls are protocol work;
- they do not imply a new Preset ownership target;
- they should be consolidated under the future send-side protocol split.

### UART send-split findings

The current STM-side direct-send callers are still spread across several files:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/Preset/EndpointRestore.c`
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c`

The important future boundary is:

- `Uart.c/.h` should remain transport-only;
- the future `frontPanelSendingProtocol.c/.h` should own outbound AVR command framing;
- no command-oriented front-panel bytes should be sent directly from outside that send module once the split lands;
- MIDI transport helpers stay outside this split because they are not AVR front-panel command traffic.

The send-side protocol contract needs reusable helpers for:

- ordinary command/status byte triples;
- priority command/status byte triples;
- wait-for-space variants for restore and other blocking sequences;
- SysEx-safe packet emission;
- common packet families such as restore begin/done, restore param, restore morph param, flow grant/abort, pattern change, run-stop, LED updates, and query replies.

## Verification

- The STM32 firmware build was run successfully after the refactor sweep.
- The user re-tested the firmware and reported that nothing obvious broke.
- The refactor notes were moved out of the temporary audit path and into the permanent docs.

## Files Changed

- `mainboard/LxrStm32/src/Preset/KitState.h`
  - Renamed `SeqKitState` and `SeqKitAutomationTargets` to `PresetKitState` and `PresetAutomationTargets`.

- `mainboard/LxrStm32/src/Preset/KitState.c`
  - Updated the exported kit globals and getter return types to the renamed `Preset*` types.

- `mainboard/LxrStm32/src/Preset/EndpointRestore.h`
  - Updated the restore helper signature to use `PresetKitState`.

- `mainboard/LxrStm32/src/Preset/EndpointRestore.c`
  - Renamed the internal restore request type to `PresetEndpointRestoreRequest`.

- `mainboard/LxrStm32/src/Preset/MorphEngine.h`
  - Updated helper signatures to use `PresetKitState` and `PresetAutomationTargets`.

- `mainboard/LxrStm32/src/Preset/MorphEngine.c`
  - Updated helper locals and call sites to the renamed `Preset*` types.

- `mainboard/LxrStm32/src/Preset/ParameterIngress.h`
  - Updated ingress helper signatures to use `PresetKitState`.

- `mainboard/LxrStm32/src/Preset/ParameterIngress.c`
  - Updated ingress helper locals and selector handling to the renamed `Preset*` types.

- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.h`
  - Removed the temp-switch `seq_*` alias macros.

- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c`
  - Switched voice-source access to the canonical `preset_*` names.

- `mainboard/LxrStm32/src/Sequencer/sequencer.h`
  - Removed the Preset-owned `seq_*` re-export macros and handshake re-export.

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
  - Switched Preset-owned state access to `preset_*` names and `preset_tempPlaybackSwitchState`.

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`
  - Included `Preset/EndpointRestore.h` directly and updated the Preset-owned state references.

- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
  - Switched the morph-load guard to `preset_morphLoadDisabled`.

- `mainboard/LxrStm32/src/DSPAudio/modulationNode.c`
  - Switched the morph-load guard to `preset_morphLoadDisabled`.

- `mainboard/LxrStm32/src/Preset/ParameterArray.c`
  - Switched the morph parameter bindings to `preset_vMorphAmount`.

- `PRESET_CONSOLIDATION_AUDIT.md`
  - Expanded Phase 12 with the remaining protocol-split follow-up and the direct-send note.

- `MIDI_UART_SPLIT_AUDIT.md`
  - Expanded with the direct-send inventory and the future send API shape.

- `knowledge_files/log_archive/000_SESSION_INDEX.md`
  - Appended the terse Session 017 entry and a new cross-session fact.

- `knowledge_files/log_archive/017_SESSION_HANDOFF_LOG.md`
  - Created this detailed handoff log.

- `knowledge_files/session_in_flight/COMMS_FLOW_SPEC.md`
  - Updated the comms reference with the current send-split mapping and direct-send inventory.

- `knowledge_files/session_in_flight/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`
  - Updated the temp/pattern/parameter load reference to match the current ownership model and rename state.

- `MEMORY.md`
  - Updated the current session/status notes and the active-doc pointers.

## End of session block

```
DATE: 2026-06-14
SESSION GOAL: Finish the remaining Preset refactor cleanup for the current plan, verify the Phase 10/11 rename and rehome work, and preserve the follow-up notes for the comms and temp-load split in the permanent docs.
COMPLETED: The Preset-owned live-apply and rename cleanup is complete, the remaining Preset-owned seq/Seq names were removed or replaced, the Phase 12 review was confirmed as retrospective only, and the comms/temp-load reference notes were moved into the permanent docs.
VERIFIED ON HARDWARE: Yes. The user re-tested the firmware and reported nothing obvious broke.

CHANGES THIS SESSION:
- mainboard/LxrStm32/src/Preset/KitState.h/.c: renamed the Preset-owned kit and automation types to `PresetKitState` / `PresetAutomationTargets` and updated the exported kit globals.
- mainboard/LxrStm32/src/Preset/EndpointRestore.h/.c: renamed the restore queue request type to `PresetEndpointRestoreRequest` and updated the restore helpers.
- mainboard/LxrStm32/src/Preset/MorphEngine.h/.c: updated morph/live-apply helper signatures and locals to the renamed Preset types.
- mainboard/LxrStm32/src/Preset/ParameterIngress.h/.c: updated ingress helper signatures and locals to the renamed Preset types.
- mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.h/.c: removed the temp-switch `seq_*` alias macros and switched to direct `preset_*` state access.
- mainboard/LxrStm32/src/Sequencer/sequencer.h/.c: removed Preset-owned `seq_*` re-exports, switched to `preset_*` state access, and aligned restore/temp-switch bookkeeping with the renamed types.
- mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c: included `Preset/EndpointRestore.h` directly and updated remaining Preset-owned state references.
- mainboard/LxrStm32/src/MIDI/MidiParser.c: switched the morph-load guard to `preset_morphLoadDisabled`.
- mainboard/LxrStm32/src/DSPAudio/modulationNode.c: switched the morph-load guard to `preset_morphLoadDisabled`.
- mainboard/LxrStm32/src/Preset/ParameterArray.c: bound the morph parameter pointers to `preset_vMorphAmount`.
- PRESET_CONSOLIDATION_AUDIT.md: expanded Phase 12 with the protocol-split follow-up and the direct-send note.
- MIDI_UART_SPLIT_AUDIT.md: expanded the send-side split plan, direct-send inventory, and reusable send-API shape.
- knowledge_files/log_archive/000_SESSION_INDEX.md: appended the Session 017 index entry and cross-session fact.
- knowledge_files/session_in_flight/COMMS_FLOW_SPEC.md: updated the UART comms reference with the direct-send inventory and send-protocol contract.
- knowledge_files/session_in_flight/TEMPORARY_PAT_PARAM_LOAD_SPEC.md: updated the load/spec reference to match the current Preset ownership and rename state.
- MEMORY.md: refreshed the active-doc pointers and current session notes.

KNOWN ISSUES INTRODUCED: None known.
KNOWN ISSUES RESOLVED: The remaining Preset-owned `seq_` / `Seq*` naming debt was removed, and the comms notes now live in the permanent reference docs instead of only in the temporary audit.

NEXT SESSION RECOMMENDED GOAL: Start the UART send-side split by extracting outbound AVR command framing into `frontPanelSendingProtocol.c/.h` and moving the direct-send callers onto that reusable API.
BLOCKERS: None.

CRITICAL REMINDERS FOR NEXT SESSION:
- Do not reintroduce Preset-owned `seq_` aliases or `Seq*` type names.
- Keep `Uart.c/.h` transport-only.
- All AVR-facing command bytes should eventually flow through the future send-side protocol layer.
- The `.pat` load rule still applies: pattern-only background loading must not switch parameter read/write away from normal storage.
```
