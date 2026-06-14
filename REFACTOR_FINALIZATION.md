# REFACTOR_FINALIZATION

Date: 2026-06-14
Status: Session 020 planning document

## Purpose

This document reconciles the two active audit documents, the session logs, and
the current code state so the remaining refactor work can finish without
repeating the Session 018 planning mistake.

The short version:

- The STM front-panel send split is effectively complete.
- The MIDI parser split is effectively complete for the audit goal.
- Most Preset consolidation phases after Phase 8 are complete.
- `PresetLoadCache.c/.h` still exist and should not.
- `PresetLoadCache` is obsolete, not transitional ownership to preserve.
- The receive-side front-panel protocol rename/split is not complete.
- The AVR-side `frontPanelParser.c/.h` still mixes opcode definitions,
  receive parsing, send helpers, flow control, and restore handling.

## Evidence Read For This Plan

- `MEMORY.md`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`
- `knowledge_files/log_archive/017_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/018_SESSION_HANDOFF_LOG.md`
- `MIDI_UART_SPLIT_AUDIT_EDIT.md`
- `PRESET_CONSOLIDATION_AUDIT.md`
- Current source under:
  - `mainboard/LxrStm32/src/uARTFrontSYX/`
  - `mainboard/LxrStm32/src/MIDI/`
  - `mainboard/LxrStm32/src/Preset/`
  - `front/LxrAvr/frontPanelParser.c/.h`

## Current Confirmed State

### Preset Consolidation

Completed:

- `ParameterArray.c/.h` lives under `mainboard/LxrStm32/src/Preset/`.
- `ParameterMap.c/.h` are gone.
- Preset-owned core types and APIs now mostly use `Preset*` and `preset_*`
  naming.
- Endpoint restore exports are `preset_*`.
- Temp playback boundary state lives in `preset_tempPlaybackSwitchState`.
- Live automation application moved out of `Sequencer` into Preset-owned
  helpers.
- The old Sequencer-owned Preset aliases are no longer the main public surface.

Not completed:

- `mainboard/LxrStm32/src/Preset/PresetLoadCache.c`
  and `mainboard/LxrStm32/src/Preset/PresetLoadCache.h` still exist.
- `PresetLoadCache.h` exports a large obsolete bridge surface: deferred performance
  queues, PRF cache state, live pattern snapshots, MIDI channel/note/morph
  mirrors, pending counters, voice-cache helpers, and
  `presetLoad_finalizeTempBackgroundLoad()`.
- `frontPanelParser.c` still includes `Preset/PresetLoadCache.h` and directly
  reads/writes many `presetLoad_*` flags.
- `sequencer.c` still calls `presetLoad_finalizeTempBackgroundLoad()`.
- `TempPlaybackSwitch.h` still declares `presetLoad_finalizeTempBackgroundLoad()`
  even though that is not a temp-switch API.

Why this happened:

- Session 014 deleted `PresetLoadCache` and moved the remaining bridge into
  `frontPanelParser.c` as a temporary holdover.
- Session 018 then moved that parser-local bridge back into a dedicated
  `PresetLoadCache` module to make `frontPanelParser.c` look cleaner.
- That made the receive parser cleaner in isolation, but it reversed the Phase
  7 Preset objective: no separate `PresetLoadCache` subsystem.
- The mistake was treating "not parser-local" as more important than "not a
  second Preset load/cache owner."
- The corrected decision is stricter: the cache bridge is not a receive module
  to rehome. It is a deprecated background-load regime to remove.

### Load Model Decision

The final load model is:

- All file loads write straight to normal storage after this refactor.
- No file-load path should stage through `PresetLoadCache`.
- The existing normal/temp Preset and Pattern switching remains in place.
- Temporary parameter/pattern copy and playback functions are the only staging
  model that should survive.
- Future file-load-while-temp-is-active behavior should be implemented through
  the existing temp parameter/pattern copy and playback mechanisms, not through
  a revived load cache.
- Pattern-only load handling must stay separate from parameter storage
  switching. `.pat` loads should not move Preset parameter ingress away from
  normal storage.

This means the surviving `presetLoad_*` families are not candidates for a new
home. They should either be deleted or replaced by direct calls into the real
owners: `ParameterIngress`, `KitState`, `TempPlaybackSwitch`, and
`PatternData`.

### STM Front-Panel UART Split

Completed:

- `frontPanelSendingProtocol.c/.h` exists and owns the outbound STM-to-AVR
  packet helpers.
- Direct calls to `uart_sendFrontpanelByte()`,
  `uart_sendFrontpanelPriorityByte()`,
  `uart_sendFrontpanelPriorityByteWait()`, and
  `uart_sendFrontpanelSysExByte()` are confined to `Uart.c/.h` and
  `frontPanelSendingProtocol.c`.
- `sequencer.c`, `EndpointRestore.c`, `MidiParser.c`,
  `ChannelMidiParser.c`, `GlobalMidiParser.c`, `MidiVoiceControl.c`, and
  `frontPanelParser.c` route visible front-panel output through the send helper
  layer.
- `Uart.c` still owns raw USART/FIFO transport, which is correct.

Not completed:

- STM receive code is still named `frontPanelParser.c/.h`.
- `FrontPanelProtocol.h` still exists as a separate STM opcode namespace.
- The receive-side load/session path is split between
  `frontPanelParser.c/.h` and `PresetLoadCache.c/.h`.

### MIDI Split

Completed for the current audit goal:

- `ChannelMidiParser.c/.h` exists and owns the per-voice note and CC paths.
- `GlobalMidiParser.c/.h` exists and owns system/global handling.
- `MidiParser.c` is now mainly the external MIDI stream coordinator, routing
  shim, running-status parser, and program-change/note fanout layer.
- The broad voice CC ladders live in `ChannelMidiParser.c`.
- The global-channel CC ladder lives in `GlobalMidiParser.c`.
- The front-panel echo helpers used by MIDI paths route through
  `frontPanelSendingProtocol.c/.h`.

Remaining future MIDI work:

- `MidiParser.c` still ignores incoming SysEx bytes after external routing.
  The desired future behavior is for external DIN/USB SysEx to be forwarded to
  `uARTFrontSYX`, not parsed as AVR front-panel traffic in MIDI.
- This is future functional work, not a failed objective from the current MIDI
  split audit.

Conclusion:

- MIDI appears finalized for the audit scope.
- Do not churn the MIDI files during the finalization pass unless verification
  finds a concrete regression.

### AVR Front-Panel Protocol

Not completed:

- `front/LxrAvr/frontPanelParser.c/.h` still mixes:
  - opcode definitions;
  - receive parsing from STM;
  - send helpers to STM;
  - flow-control helpers;
  - PRF cache initiators;
  - SysEx request/response state;
  - restore handling;
  - long-operation handling.
- The AVR header still defines the front-panel opcode namespace locally.
- There is no AVR-side `frontPanelSendingProtocol.c/.h` yet.

Desired final direction:

- Rename the AVR receive parser to `frontPanelReceivingProtocol.c/.h`.
- Move AVR outbound helpers such as `frontPanel_sendData()`,
  `frontPanel_sendByte()`, `frontPanel_sendMidiMsg()`, flow send helpers, and
  PRF cache control sends into AVR-side `frontPanelSendingProtocol.c/.h`.
- Keep shared protocol mechanisms in the receiving protocol only where they are
  genuinely shared between send and receive.

### Future SysEx / Protocol Boundary

`uARTFrontSYX` is intended to become the owner of the full front-panel/SysEx
command protocol, not only the AVR UART byte stream.

The intended future path is:

- AVR UART traffic enters through `Uart.c` and is interpreted by
  `uARTFrontSYX`.
- External DIN MIDI and USB MIDI SysEx should be able to forward command
  payloads into the same `uARTFrontSYX` protocol implementation.
- The protocol layer should be able to send replies through AVR UART, DIN MIDI,
  or USB MIDI as appropriate.
- `MIDI/` should continue to own ordinary external MIDI parsing: note, CC,
  realtime, routing, and future SysEx forwarding into `uARTFrontSYX`.
- `MIDI/` should not parse AVR/front-panel command semantics itself.

This makes `uARTFrontSYX` the semantic command-protocol owner, while MIDI and
UART remain transport ingress/egress paths.

## Failed Or Stale Planning In The Old Audits

- `PRESET_CONSOLIDATION_AUDIT.md` says Phase 7 deleted
  `PresetLoadCache.c/.h`; that was true at Session 014 but false after
  Session 018.
- `MIDI_UART_SPLIT_AUDIT_EDIT.md` has completed progress notes through Phase 6,
  then placeholder Phase 7/8/9 headings. The receive-side plan was not finished.
- The Session 018 handoff explicitly calls the recreated `PresetLoadCache`
  module a success, which conflicts with the Preset audit's long-term goal.
- The docs also blur two different `frontPanelParser.c` files:
  - STM: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`
  - AVR: `front/LxrAvr/frontPanelParser.c`
- The finalization plan must keep those two sides distinct.

## Finalization Plan

The plan below is intentionally short. The goal is to finish the architecture,
not start another sprawling audit.

### Step 1: Delete The Reintroduced STM `PresetLoadCache`

Goal:

- Remove `PresetLoadCache.c/.h` again.
- Keep AVR ingress and SysEx/session interpretation in `uARTFrontSYX`, not in
  `MIDI` and not in a Preset load-cache subsystem.
- Route file loads directly to normal storage.
- Keep only the existing normal/temp Preset and Pattern switching model.

Implementation shape:

- Remove the current `PresetLoadCache.c/.h` bridge instead of moving it.
- Delete the PRF cache state machine, live snapshot mirror, deferred
  performance replay cache, voice-cache promotion helpers, and pending-counter
  cache state unless a specific piece is proven necessary for the normal/temp
  switch model.
- Replace file-load ingress with direct normal-storage routing through the
  existing owners.
- Keep normal/temp playback switching in `TempPlaybackSwitch`.
- Keep temp pattern staging/commit behavior in `PatternData`.
- Keep parameter write routing in `ParameterIngress`.
- If a temporary compatibility wrapper is needed for old AVR flow-control
  commands, make it a thin no-cache adapter and document its deletion point.
- Rename STM `uARTFrontSYX/frontPanelParser.c/.h` to
  `frontPanelReceivingProtocol.c/.h` after the cache removal is understood, or
  keep the rename as a separate no-behavior pass if that makes verification
  cleaner.
- Update `Uart.c` to include and call
  `frontPanelReceiving_parseUartData()` or the chosen receive-prefixed name.
- Update `sequencer.c` so the pattern-boundary finalizer no longer calls a
  `presetLoad_*` function. Sequencer may signal that a safe boundary occurred,
  but it must not know about obsolete load-cache finalization.
- Remove `presetLoad_finalizeTempBackgroundLoad()` from `TempPlaybackSwitch.h`.
- Delete `mainboard/LxrStm32/src/Preset/PresetLoadCache.c`.
- Delete `mainboard/LxrStm32/src/Preset/PresetLoadCache.h`.

Rules:

- Do not move AVR UART parsing into `MIDI`.
- Do not recreate the cache under a different Preset filename.
- Do not recreate the cache inside the receive protocol module.
- Do not preserve background-load staging as a hidden compatibility behavior.
- Preserve `.ALL`, `.PRF`, `.PAT`, temp-switch, restore, and deferred-perf
  behavior only where it still belongs to normal storage or the normal/temp
  switching model.
- Keep pattern-only background loading from switching parameter ingress away
  from normal storage.
- Treat PRF/cache opcodes as deprecated. Keep them only if they are still
  required for normal/temp copy/paste, file load while temporary playback is in
  use, normal/temp playback switching, or parameter send-to-AVR. Otherwise make
  them no-op/reject compatibility traffic first, then remove them.

Verification:

- `make -C mainboard/LxrStm32 -j4 stm32`
- Hardware smoke tests: file load, copy-to-temp, temp switch, parameter edit,
  restore begin/done, PRF cache accept/reject/abort path if available.

### Step 2: Tighten STM Protocol Headers

Goal:

- Finish the STM send/receive naming boundary.

Implementation shape:

- Fold the STM opcode namespace from `FrontPanelProtocol.h` into the receive
  protocol header, or keep only a short compatibility shim if the include churn
  is too large for one pass.
- Make `frontPanelSendingProtocol.h` include the receive/protocol header for
  shared opcodes instead of treating `FrontPanelProtocol.h` as a separate
  owner.
- Keep `MidiMessages.h` as compatibility-only for front-panel opcode access
  until remaining includes are migrated.
- Update comments so the file ownership is explicit:
  - `Uart.c/.h`: raw transport only;
  - `frontPanelReceivingProtocol.c/.h`: AVR receive parsing, SysEx ingress,
    load-session receive state;
  - `frontPanelSendingProtocol.c/.h`: outbound STM-to-AVR packet construction.

Verification:

- `make -C mainboard/LxrStm32 -j4 stm32`
- Re-run the front-panel smoke tests from Step 1.

### Step 3: AVR Protocol Split And Rename

Goal:

- Bring the AVR source layout in line with the same protocol model.

Implementation shape:

- Rename `front/LxrAvr/frontPanelParser.c/.h` to
  `frontPanelReceivingProtocol.c/.h`.
- Add AVR-side `frontPanelSendingProtocol.c/.h`.
- Move AVR outbound helpers into the sending file:
  - `frontPanel_sendByte()`
  - `frontPanel_sendData()`
  - `frontPanel_sendMidiMsg()`
  - send-side flow helpers
  - PRF cache command send helpers
  - LED/query request send helpers where they are pure packet emission
- Keep STM response parsing, restore handling, long-operation handling, and
  SysEx receive state in the receiving file.
- Keep compatibility wrappers/macros for one pass if that keeps AVR call-site
  churn manageable.
- Deprecate the AVR PRF cache initiators. Keep only the command traffic needed
  for normal/temp copy/paste, file loading to normal storage while temp
  playback is active, normal/temp playback switching, and parameter restore to
  AVR.

Rules:

- Do not alter the Timer1 encoder implementation while doing this.
- Do not change protocol byte values.
- Do not combine this with new background-load behavior.

Verification:

- `make -C front/LxrAvr avr -j4`
- `make firmware`
- Hardware smoke tests: menu parameter edit, file load, pattern request/reply,
  copy-to-temp, temp switch, restore, and sample count/upload acknowledgement
  if available.

### Step 4: Documentation And Exit Check

Goal:

- Close the stale planning loop.

Tasks:

- Mark `PRESET_CONSOLIDATION_AUDIT.md` as superseded by this finalization plan
  for the remaining cache/receive work.
- Mark `MIDI_UART_SPLIT_AUDIT_EDIT.md` phases 1-6 as completed and replace
  placeholder Phase 7/8/9 headings with a pointer to this document.
- Update `MEMORY.md` and `knowledge_files/log_archive/000_SESSION_INDEX.md`
  after implementation.
- Add a Session 020 handoff log once the finalization pass is verified.

Final exit criteria:

- No `PresetLoadCache.c/.h` files exist.
- No exported `presetLoad_*` cache API remains.
- No hidden replacement cache exists in `uARTFrontSYX`.
- File loads route to normal Preset/Pattern storage unless future work
  explicitly invokes the temp copy/playback model.
- AVR front-panel ingress into STM state is owned by `uARTFrontSYX`, not MIDI.
- STM front-panel output goes through `frontPanelSendingProtocol.c/.h`.
- STM front-panel input goes through `frontPanelReceivingProtocol.c/.h`.
- AVR protocol code has matching receive/send filenames or compatibility
  shims with a clear removal point.
- `MIDI/` remains focused on external DIN/USB MIDI parsing and routing.
- Full firmware build succeeds.

## Recommended First Implementation Pass

Start with Step 1 only.

That is the architectural bug that invalidates the Preset audit. The work
should remove the obsolete cache path, not rehome it. The MIDI files should be
left alone during that pass because they already satisfy the current split goal
and were hardware-smoke-tested in Session 018.
