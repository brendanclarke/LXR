# LXR -bc- Enhanced Firmware — Session 020 Handoff Log

**Project**: LXR -bc- Enhanced Firmware  
**Session goal**: Reconcile stale preset/MIDI UART audit plans, remove obsolete `PresetLoadCache`, finalize STM and AVR front-panel protocol structure, and close the planning loop.  
**Last session summary**: Session 019 finished the AVR encoder retune and edit-mode acceleration. Session 018 had earlier consolidated STM send helpers, split MIDI channel/global parsing, and mistakenly recreated `PresetLoadCache` as a transitional bridge.  
**Working repository**: `/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod` on branch `custom-develop-patload-envmod`; tree contains the Session 020 refactor changes plus rebuilt firmware output.  
**Constraints today**: Do not revive background-load cache staging. Keep normal/temp Preset and Pattern switching intact. `uARTFrontSYX` owns AVR/front-panel command semantics; `MIDI/` remains external DIN/USB MIDI parsing and future SysEx forwarding.

Key files to be aware of:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`
- `front/LxrAvr/frontPanelReceivingProtocol.c`
- `front/LxrAvr/frontPanelSendingProtocol.c`
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
- `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`
- `MEMORY.md`

## Session 020 Summary

Session 020 started by creating `REFACTOR_FINALIZATION.md` to reconcile the stale `PRESET_CONSOLIDATION_AUDIT.md`, `MIDI_UART_SPLIT_AUDIT_EDIT.md`, session logs, and actual source state. The main correction was architectural: `PresetLoadCache` was not a useful module to rehome. It was obsolete background-load staging that should be removed because the normal/temp Preset and Pattern switching model already provides the intended staging surface.

The session then implemented and hardware-tested three code steps, followed by documentation closeout.

## Step 1: Remove Obsolete STM `PresetLoadCache`

Implemented:

- Deleted `mainboard/LxrStm32/src/Preset/PresetLoadCache.c`.
- Deleted `mainboard/LxrStm32/src/Preset/PresetLoadCache.h`.
- Removed active source dependencies on `Preset/PresetLoadCache.h`.
- Removed the active `presetLoad_*` cache API from STM source.
- Removed `presetLoad_finalizeTempBackgroundLoad()` from `TempPlaybackSwitch.h` and the Sequencer stop path.
- Removed the PRF cache state machine, live snapshot interception, deferred performance replay interception, pending-counter updates, and voice-cache apply/finalize calls from the STM receive path.
- Replaced cache-owned file-load ingress with a tiny parser-local bracket that only forces `ParameterIngress` into normal-kit endpoint mode for file receive.
- Changed SysEx pattern receive paths to write directly to normal `seq_patternSet` storage instead of staging through cache/background-load behavior.
- Deprecated PRF/cache STM command handling to reject/no-op compatibility responses while old AVR initiators still exist.

Build and hardware:

- `make -C mainboard/LxrStm32 -j4 stm32` passed after a clean dependency rebuild.
- User hardware-tested Step 1 successfully.

## Step 2: Tighten STM Protocol Headers

Implemented:

- Added `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h` as the canonical STM receive/protocol header.
- Renamed STM `frontPanelParser.c` to `frontPanelReceivingProtocol.c`.
- Moved the STM opcode namespace and receive declarations into the receiving protocol header.
- Kept `FrontPanelProtocol.h` and `frontPanelParser.h` as temporary compatibility shims during the first Step 2 pass.
- Updated preferred STM includes in `Uart.c`, `frontPanelReceivingProtocol.c`, `frontPanelSendingProtocol.h`, `sequencer.c`, and MIDI split files to use `frontPanelReceivingProtocol.h`.
- Kept legacy `frontParser_*` exported symbol names for this pass to avoid behavior churn.

Build and hardware:

- `make -C mainboard/LxrStm32 -j4 stm32` passed.
- User hardware-tested Step 2 successfully.

## Step 3: Split AVR Protocol Receive/Send

Implemented:

- Renamed AVR `frontPanelParser.c` to `frontPanelReceivingProtocol.c`.
- Renamed the canonical AVR protocol header to `frontPanelReceivingProtocol.h`.
- Added AVR-side `frontPanelSendingProtocol.c/.h`.
- Kept AVR `frontPanelParser.h` as a temporary compatibility shim during the first Step 3 pass.
- Moved AVR outbound packet helpers into `frontPanelSendingProtocol.c`: `frontPanel_sendByte()`, `frontPanel_sendData()`, `frontPanel_sendMidiMsg()`, LED query helpers, macro sends, flow send helpers, and PRF cache control sends.
- Moved AVR send-side flow-control state and deprecated PRF cache status wait state into `frontPanelSendingProtocol.c`.
- Kept STM response parsing, SysEx receive state, restore handling, and long-operation receive state in `frontPanelReceivingProtocol.c`.
- Added receive-to-send hooks for callback ACK, flow messages, and PRF cache status messages.
- Updated AVR include sites so UART includes the receive/protocol header, while UI/preset/menu command emitters include the sending header.
- Updated `front/LxrAvr/DrumSynthFront.cproj` for the new receive/send files.

Build and hardware:

- `make -C front/LxrAvr avr -j4` passed with the usual AVR warning noise.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.
- User hardware-tested Step 3 successfully.

## Step 4: Documentation Closeout

Implemented:

- Marked `PRESET_CONSOLIDATION_AUDIT.md` as superseded for remaining cache/protocol work by `REFACTOR_FINALIZATION.md`.
- Marked `MIDI_UART_SPLIT_AUDIT_EDIT.md` as superseded for remaining implementation tracking by `REFACTOR_FINALIZATION.md`.
- Replaced the placeholder MIDI/UART Phase 7/8/9 headings with the Session 020 finalization result.
- Updated `MEMORY.md`.
- Updated `knowledge_files/log_archive/000_SESSION_INDEX.md`.
- Added this Session 020 handoff.

## Wrap-Up: Legacy Header Shim Removal

After Step 4 hardware acceptance, the remaining compatibility headers were
checked for consumers, redirected, and removed:

- `mainboard/LxrStm32/src/MIDI/MidiMessages.h` now includes
  `uARTFrontSYX/frontPanelReceivingProtocol.h` directly.
- `mainboard/LxrStm32/src/uARTFrontSYX/FrontPanelProtocol.h` was deleted.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.h` was deleted.
- `front/LxrAvr/frontPanelParser.h` was deleted.
- `front/LxrAvr/DrumSynthFront.cproj` no longer lists the AVR
  `frontPanelParser.h` shim.
- Durable details from `REFACTOR_FINALIZATION.md`,
  `PRESET_CONSOLIDATION_AUDIT.md`, and `MIDI_UART_SPLIT_AUDIT_EDIT.md` were
  copied into this handoff, `COMMS_FLOW_SPEC.md`, and
  `TEMPORARY_PAT_PARAM_LOAD_SPEC.md` before those temporary audit docs are
  deleted.

## Cleanup Pass: Final Boundary Corrections

After the initial wrap-up, a review found that several MIDI/front-panel
boundaries were still muddy. A corrective cleanup pass was implemented:

- STM and AVR front-panel receive state was renamed from the MIDI-named
  `frontParser_midiMsg` to `frontParser_command`.
- AVR `midiMsg_checkLongOps()` was renamed to `frontPanel_checkLongOps()`.
- The old `midiParser_ccHandler()` parameter-apply ladder moved out of
  `MIDI/MidiParser.c` and into
  `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c` as
  `frontParser_applyParameterCommand()`.
- Internal parameter-apply callers now use `frontParser_applyParameterCommand()`
  directly, including automation node restore/apply and
  `Preset/ParameterIngress.c`.
- The parameter apply layer now owns its NRPN assembly helper, LFO/velocity
  apply helpers, original-CC value table, and envelope-position table. The
  symbol names remain source-compatible where needed.
- `MIDI/MidiParser.c` retains external DIN/USB byte parsing, routing, filters,
  and channel/global MIDI interpretation.
- MIDI routing/filter setters were renamed from parser-shaped names to
  `midi_setRouting()` and `midi_setFilter()`.
- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c/.h` was renamed to
  `MidiOutputControl.c/.h`. Existing `voiceControl_*` functions kept their
  names; new sequencer-originated MIDI fan-out helpers use the
  `outputControl_*` prefix.
- `Sequencer/sequencer.c` no longer owns USB/DIN MIDI fan-out through
  `seq_sendMidi()` and no longer includes raw USB or MIDI UART transports for
  that output path.
- `frontPanelSendingProtocol.h` now documents the exported STM send helpers by
  command family, direction, priority/wait behavior, and deprecated cache
  compatibility status.

## Final Architecture State

- `PresetLoadCache.c/.h` do not exist.
- No active source exports or calls a `presetLoad_*` cache API.
- File loads route directly to normal Preset/Pattern storage.
- Existing normal/temp Preset and Pattern switching remains intact and is the only supported staging model.
- STM front-panel output goes through `uARTFrontSYX/frontPanelSendingProtocol.c/.h`.
- STM front-panel input goes through `uARTFrontSYX/frontPanelReceivingProtocol.c/.h`.
- STM `FrontPanelProtocol.h` and `frontPanelParser.h` do not exist; callers use
  `frontPanelReceivingProtocol.h` directly.
- AVR front-panel receive lives in `frontPanelReceivingProtocol.c/.h`.
- AVR front-panel send lives in `frontPanelSendingProtocol.c/.h`.
- AVR `frontPanelParser.h` does not exist; callers use
  `frontPanelReceivingProtocol.h` or `frontPanelSendingProtocol.h` directly.
- `MIDI/` remains focused on external DIN/USB MIDI parsing and routing.
- Internal CC/CC2-shaped parameter application is owned by
  `uARTFrontSYX/frontPanelReceivingProtocol.c` and exposed as
  `frontParser_applyParameterCommand()`.
- Sequencer-originated MIDI output is owned by `MIDI/MidiOutputControl.c/.h`
  through `outputControl_*` helpers.
- Future external DIN/USB SysEx command handling should forward into `uARTFrontSYX`; MIDI should not parse AVR/front-panel command semantics itself.

## Known Remaining Work

- Remove deprecated AVR PRF cache initiators once file-load code no longer sends cache compatibility traffic during the old transfer envelope.
- Implement future file-load-while-temp-active behavior through existing temporary parameter/pattern copy and playback mechanisms, not through a revived load cache.
- Implement future full SysEx command protocol in `uARTFrontSYX`, reachable through AVR UART, DIN MIDI, or USB MIDI transport ingress.

## Verification

- `make -C mainboard/LxrStm32 -j4 stm32`
- `make -C front/LxrAvr avr -j4`
- `make firmware`
- User hardware-tested Steps 1, 2, and 3 successfully.
- After legacy header shim removal, clean STM and AVR rebuilds plus
  `make firmware` passed again.
- After the final boundary cleanup, `make -C mainboard/LxrStm32 -j4 stm32`,
  `make -C front/LxrAvr avr -j4`, and `make firmware` passed again.

## End Of Session Block

DATE: 2026-06-14  
SESSION GOAL: Complete `MIDI_UART_SPLIT_AUDIT_EDIT.md` and `PRESET_CONSOLIDATION_AUDIT.md` objectives after stale planning around Phase 7, remove obsolete `PresetLoadCache`, and finalize STM/AVR protocol boundaries.  
COMPLETED: `PresetLoadCache` removed, STM receive renamed, AVR receive/send split completed, final MIDI/front-panel boundary cleanup implemented, stale audits superseded, memory/index/handoff updated.  
VERIFIED ON HARDWARE: yes; user accepted Steps 1, 2, and 3 after testing.

CHANGES THIS SESSION:

- `REFACTOR_FINALIZATION.md`: created/updated final plan, implementation notes, and exit criteria.
- `mainboard/LxrStm32/src/Preset/PresetLoadCache.c/.h`: deleted.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c/.h`: canonical STM receive/protocol files.
- `mainboard/LxrStm32/src/uARTFrontSYX/FrontPanelProtocol.h` and `frontPanelParser.h`: deleted after include redirection.
- `front/LxrAvr/frontPanelReceivingProtocol.c/.h`: canonical AVR receive/protocol files.
- `front/LxrAvr/frontPanelSendingProtocol.c/.h`: AVR outbound packet helpers and send-side flow state.
- `front/LxrAvr/frontPanelParser.h`: deleted after include/project redirection.
- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c/.h`: renamed to
  `MidiOutputControl.c/.h`.
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`: no longer owns the internal
  CC/CC2 parameter-apply ladder.
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`: now sends MIDI output
  through `outputControl_*` helpers instead of direct USB/DIN fan-out.
- `PRESET_CONSOLIDATION_AUDIT.md` and `MIDI_UART_SPLIT_AUDIT_EDIT.md`: marked superseded for remaining implementation tracking.
- `MEMORY.md` and `knowledge_files/log_archive/000_SESSION_INDEX.md`: updated with Session 020 state.

KNOWN ISSUES INTRODUCED: none known.  
KNOWN ISSUES RESOLVED: obsolete `PresetLoadCache` recreation, stale Phase 7/8/9 MIDI/UART planning, mixed AVR receive/send protocol file ownership.

NEXT SESSION RECOMMENDED GOAL: Review generated `firmware image/FIRMWARE.BIN` and commit/package the finalized refactor, or start a fresh plan for future transport-neutral SysEx forwarding.
BLOCKERS: none for the finalized refactor.

CRITICAL REMINDERS FOR NEXT SESSION:

- Do not recreate `PresetLoadCache` or a hidden replacement cache.
- Do not make file loads stage through a new cache; use direct normal storage unless a future task explicitly uses the existing normal/temp copy/playback model.
- Keep AVR/front-panel command semantics in `uARTFrontSYX`, not `MIDI/`.
- Do not alter the hardware-approved AVR encoder Timer1 path unless new hardware testing identifies a regression.
