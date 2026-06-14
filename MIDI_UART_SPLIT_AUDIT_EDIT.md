# MIDI_UART_SPLIT_AUDIT

Date: 2026-06-14
Status: current AVR<->STM comms reference; Session 014 moved the Sequencer/MIDI runtime callers off the load cache, Session 017 expanded the outbound send inventory, and this audit now carries an implementation-ready plan for the remaining send/receive protocol split and the follow-on MIDI parser split.

## Purpose

This audit tracks the follow-on protocol and MIDI layout cleanup after the Preset
consolidation work has stabilized.

The goal is not to change behavior first. The goal is to make the send/receive
boundary and the MIDI parser ownership split visible in the file layout before
we ask the code to move.

This document is intentionally implementation-oriented. It should answer:

- which file should own each behavior bucket;
- which direct send call sites should move first;
- which helpers are safe to extract mechanically;
- which behavior needs a build-and-feedback checkpoint before the next pass;
- what "done" looks like for the front-panel UART split and the MIDI parser split.

## Current Shape

The current files that matter for this split are:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/FrontPanelProtocol.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.h`
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiParser.h`
- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/Preset/EndpointRestore.c`

Those files still combine too much protocol, parser, and transport ownership in
one place. The split work should expose that ownership more clearly without
forcing a risky behavior rewrite too early.

The current parser-local holdover is still real:

- `frontPanelParser.c` still owns the remaining transitional load/session bridge;
- `Uart.c` still owns the actual FIFO and IRQ plumbing;
- `MidiParser.c` still mixes stream coordination, per-channel dispatch, and
  front-panel echo traffic;
- the visible send callers are still spread across the parser, sequencer,
  endpoint restore, MIDI parser, and voice-control code.

## Front-Panel UART Split

### Goal

Reshape `mainboard/LxrStm32/src/uARTFrontSYX/` into a visibly separated send
and receive layout.

The long-term target is:

- `Uart.c` and `Uart.h` for raw transport, FIFO ownership, and IRQ plumbing;
- `frontPanelParser.c/.h` for receive-side parsing and dispatch during the
  split, renamed to `frontPanelReceivingProtocol.c/.h` only when the split is
  final;
- `frontPanelSendingProtocol.c/.h` for outbound framing and packet helpers;
- `FrontPanelProtocol.h` for the opcode namespace.

### Target file set

- `Uart.c`
- `Uart.h`
- `frontPanelParser.c`
- `frontPanelParser.h`
- `frontPanelSendingProtocol.c`
- `frontPanelSendingProtocol.h`

The important contract here is that `Uart.c` keeps the raw transport and FIFO
ownership, while the send and receive protocol files own the packet
composition. After the split, no STM-side file outside
`frontPanelSendingProtocol.c/.h` should call `uart_sendFrontpanelByte()`,
`uart_sendFrontpanelPriorityByte()`, `uart_sendFrontpanelPriorityByteWait()`,
or `uart_sendFrontpanelSysExByte()` directly for AVR command traffic.

The MIDI transport helpers `uart_sendMidi()` and `uart_sendMidiByte()` remain
outside this split because they are not front-panel AVR command traffic.

### What this split should do

- Keep UART transport code out of the protocol parser files.
- Make front-panel receive handling explicit in the filesystem.
- Make front-panel send handling explicit in the filesystem.
- Avoid keeping the parser as the place where every front-panel concern lands.
- Preserve the current byte-level behavior until the new files prove they can
  carry it unchanged.

### Transport Boundary Details

- `Uart.c` should continue to own the front TX and RX FIFOs, the USART3 IRQ
  plumbing, and the raw queue-drain behavior.
- The protocol-aware gating currently embedded in the send helpers should move
  out of `Uart.c` and into the send protocol layer.
- `uart_clearFrontFifo()` should remain a transport-side helper because it is a
  queue-management operation, but the reason for calling it should be visible in
  the receive protocol code, not hidden in a generic transport API.
- If the raw send primitives remain public during the transition, they should be
  treated as compatibility-only transport helpers and not as the preferred call
  site for new protocol code.

### Parameter Indexing Rule

- Session 003 already resolved the raw endpoint-storage offset rule: file bytes,
  AVR menu bytes, STM endpoint arrays, and PRF restore opcodes use raw AVR/menu
  parameter indices.
- The remaining `+1` / `-1` adjustments are part of the live MIDI CC and display
  echo path, not the endpoint restore path.
- Do not move the live-CC translation rule into the send/receive split; keep it
  in the MIDI live-apply / echo helpers where it belongs.
- If the split exposes any ambiguity around a low-parameter echo, fix it in the
  MIDI path first and verify that the endpoint restore path still treats the raw
  stored index as authoritative.

### Current Direct Send Surface

These are the STM-side files that currently emit front-panel bytes directly and
need to be routed through the future send protocol layer:

| File | Current direct-send role |
|------|--------------------------|
| `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c` | Flow grant/abort replies, PRF cache status, restore-ready/ack responses, LED updates, SysEx framing, step/pattern query replies, sample count replies, and other front-panel responses. |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | Restore begin/done packets, global morph reports, pattern-change notifications, run-stop updates, beat LEDs, current-step LEDs, and SysEx pattern/step export packets. |
| `mainboard/LxrStm32/src/Preset/EndpointRestore.c` | Restore parameter packets, morph parameter packets, restore begin/done sequencing, and global morph report packets. |
| `mainboard/LxrStm32/src/MIDI/MidiParser.c` | Front-panel run-stop echoes, bank-change echoes, parameter echo packets, patch reset mirroring, and other display-facing MIDI feedback. |
| `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c` | Voice-activity LED feedback packets. |

### Outbound Send Contract

The future send-side split should land in `frontPanelSendingProtocol.c/.h`.
That module should own outbound AVR command framing so the rest of the STM code
can reuse the same packet helpers instead of rebuilding the same byte triples
everywhere.

The reusable helper surface should cover:

- ordinary command/status byte triples;
- priority command/status byte triples for flow-control and restore traffic;
- wait-for-space variants for restore sequencing and any other blocking packet
  family;
- SysEx-aware packet emission for front-panel streams that stay open across
  multiple bytes;
- common packet families such as restore begin/done, restore param, restore
  morph param, flow grant/abort, pattern change, run-stop, LED updates, query
  replies, and the specific restore/flow/status packets currently built inline
  in the parser.

The send header should also document the transmit mode bits or flags clearly so
callers can tell whether a packet is:

- normal or priority;
- blocking or non-blocking;
- SysEx-safe or ordinary command traffic;
- allowed to bypass the quiet UI gate or not.

### Receive-Side Ownership

The receive module should own the current parser responsibilities that are
about reading and interpreting bytes, not emitting them. For now that module is
`frontPanelParser.c/.h`; when the split is final, those files should be renamed
to `frontPanelReceivingProtocol.c/.h`.

That includes:

- the receive byte counter and temporary `MidiMsg` assembly state;
- the sysex-active state machine and its private buffers;
- the flow-control bookkeeping for quiet UI and credit windows;
- the parser-local load/session bridge that Session 014 left behind;
- the `FRONT_SEQ_*`, `SAMPLE_CC`, and sysex dispatch tables;
- the parser-side helpers that update `Preset`, `Pattern`, or `Sequencer`
  after a message has already been interpreted.

The send and receive modules should not remain coupled by shared "one file owns
both directions" assumptions once the split is complete.

### Detailed Migration Order

- Extract the reusable packet families first so the repeated three-byte send
  sequences stop being duplicated while the receive state machine is still in
  place.
- Move the parser-local send helpers into `frontPanelSendingProtocol.c` before
  changing any high-volume caller, because that gives one stable implementation
  target for the rest of the tree.
- Redirect `Sequencer` and `EndpointRestore` to the new send helpers next, since
  those paths are already clearly front-panel protocol work rather than parser
  logic.
- Redirect `MidiParser` and `MidiVoiceControl` after the send helpers are
  stable, because those files are the most likely to reveal missing echo or
  update helpers if the send API is too thin.
- Only after the send side is stable should the remaining parser-local bridge
  be reduced into `frontPanelParser.c/.h` in place, with the final rename to
  `frontPanelReceivingProtocol.c/.h` deferred until the split is complete.

### Phase 1: Send-Side Scaffolding

- Add `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c` and
  `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h`.
- Put the packet-composition code for `frontParser_sendFlowGrant()`,
  `frontParser_sendFlowGrantWait()`, `frontParser_sendFlowAbort()`, and
  `frontParser_sendPrfCacheStatus()` into the new send module, with the parser
  file keeping only the high-level decision of when those packets should be
  emitted.
- Move `frontParser_flowGrantByte()` into the send module as the local helper
  that builds the channel/credit nibble pair used by the grant packet.
- Keep `frontParser_isFlowMessage()`, `frontParser_suspendCreditFlowForSysex()`,
  `frontParser_flowMessageApplied()`, the `comm_*` state, and the parser-local
  load/session bridge in `frontPanelParser.c` for now; Phase 1 is only about
  separating packet assembly from parser state.
- Make `frontPanelParser.c` include `frontPanelSendingProtocol.h` and remove the
  moved helper bodies from the file so the parser can compile against the new
  send API instead of owning the packet formatting inline.
- Keep the send-module implementation thin: it should still call the existing
  `uart_sendFrontpanelPriorityByte()` and `uart_sendFrontpanelPriorityByteWait()`
  primitives in this first pass rather than changing transport behavior.
- Leave `Uart.c` and `Uart.h` unchanged in this phase; do not refactor the IRQ,
  FIFO ownership, or raw transport primitives yet.
- Do not touch `Sequencer`, `EndpointRestore`, `MidiParser`, or
  `MidiVoiceControl` in this phase; the first pass should only move the parser's
  small packet helpers into the new send file and leave every caller shape the
  same.
- Keep the new header focused on helper declarations and packet-mode
  documentation so callers can tell which helpers are priority, wait-blocking,
  sysex-safe, or quiet-UI gated.
- Make the new header depend on `stdint.h` and `FrontPanelProtocol.h`; keep the
  raw transport dependency in the `.c` file through `Uart.h` so the header
  stays protocol-focused.
- Verify the build after the helper move, then smoke-test `FRONT_SEQ_FLOW_BEGIN`,
  `FRONT_SEQ_FLOW_END`, `FRONT_SEQ_FLOW_ABORT`, `FRONT_SEQ_PRF_CACHE_BEGIN`,
  and the `CALLBACK_ACK` path before touching any higher-volume send sites.
- Do not add Makefile wiring if the existing `find`-based source discovery and
  `src/uARTFrontSYX` include path already pick up the new files cleanly.

### Phase 1 Progress Notes

- The new send module now exists in `src/uARTFrontSYX` as
  `frontPanelSendingProtocol.c/.h`.
- The parser-local packet families for flow grant, flow grant-wait, flow abort,
  and PRF cache status now route through the new send module.
- The parser still owns the sysex/flow decision state and the load/session
  bridge, so Phase 1 remains a narrow packet-assembly extraction rather than a
  behavior rewrite.
- `make -C mainboard/LxrStm32 -j4 stm32` now builds successfully with the new
  send module in place; the remaining warnings are the existing parser warnings
  that were already present before this split.
- The next pass should smoke-test the flow-control and callback-ack paths
  before any higher-volume send callers move.

### Phase 2: Parser-Local Send Cleanup

- Remove every remaining `uart_sendFrontpanelByte()` and
  `uart_sendFrontpanelSysExByte()` packet build sequence from
  `frontPanelParser.c` and replace them with dedicated send helpers.
- Move the parser-owned LED emitters into the send module:
  - `frontParser_updateTrackLeds()`
  - `frontParser_updateSubStepLeds()`
  - the direct `FRONT_LED_QUERY_SEQ_TRACK` reply path
  - the `FRONT_LED_ALL_SUBSTEP` reply path
  - the `FRONT_SEQ_SET_ACTIVE_TRACK` rotation / transpose reply pair
  - the `FRONT_MAIN_STEP_CC` step-light reply
  - the `FRONT_SAMPLE_COUNT` / `ACK` reply path
- Add send helpers for the parser's ordinary reply families rather than leaving
  them as open-coded byte triples:
  - sample-count replies;
  - track-length and track-rotation replies;
  - pattern `changeBar` / `nextPattern` replies;
  - Euklid length, step count, rotation, and substep-rotation replies;
  - current-step volume, note, and probability replies;
  - step destination/value replies for `FRONT_SET_P1_*` and `FRONT_SET_P2_*`.
- Add send helpers for the sysex request/response families that currently live
  in `frontParser_handleSysexData()`:
  - `SYSEX_START` and `SYSEX_END` acknowledge bytes;
  - `SYSEX_REQUEST_PATTERN_DATA` responses;
  - `SYSEX_REQUEST_MAIN_STEP_DATA` responses;
  - `SYSEX_REQUEST_STEP_DATA` responses;
  - `SYSEX_RECEIVE_MAIN_STEP_DATA` / `SYSEX_RECEIVE_PAT_CHAIN_DATA` /
    `SYSEX_RECEIVE_PAT_LEN_DATA` / `SYSEX_RECEIVE_PAT_SCALE_DATA` /
    `SYSEX_RECEIVE_STEP_DATA` chunk acknowledgements;
  - `SYSEX_STEP_ACK` and `SYSEX_BEGIN_PATTERN_TRANSMIT` end-of-chunk replies;
  - the `FRONT_CALLBACK_ACK` echo path if it remains a pure response packet.
- Move the dormant `presetLoad_sendPrfRestoreParam()` packet composer out of
  the parser file as part of this phase, or delete it if the load-session bridge
  no longer needs it by the time this phase is implemented.
- Keep the receive dispatch, the parser-local load/session bridge, the
  `comm_*` flow state, and the sysex receive state machine in place so behavior
  stays constant while the send helper abstraction proves itself.
- Preserve the current packet contents and ordering exactly. Phase 2 should
  change where the packet bytes are assembled, not what the receiver sees.
- Check that `frontPanelParser.c` is no longer the place where packet assembly
  rules are duplicated, even if it still owns the receive state machine and the
  load/session compatibility layer.
- Rebuild and re-run the parser-facing hardware flows that rely on sysex
  request/response behavior, LED query replies, and sample upload replies before
  moving to the sequencer and restore callers.
- Treat this as a parser-only cleanup pass; do not touch the MIDI parser files
  yet, and do not start the MIDI-risk phase until Phase 4.
- The user has already confirmed menu change, file load, copy-to-temp, and tmp
  switch behavior after Phase 1, so Phase 2 should only be validated against the
  parser reply paths that this pass actually moves.

### Phase 2 Progress Notes

- `frontPanelSendingProtocol.c/.h` now owns the parser-facing reply helpers for
  sample upload ack, sample count, pattern-data sysex replies, track length,
  track rotation, pattern params, Euklid params, active-track replies,
  main-step LED replies, step param replies, callback ack, and the parser's
  sysex ack bytes.
- `frontPanelParser.c` now dispatches those replies through semantic helpers
  instead of open-coding front-panel byte triples, and the dormant
  `presetLoad_sendPrfLiveRestore()` stub was removed with its branch reduced to
  a flow-wait grant.
- The remaining parser send logic is now decision-oriented only; the parser no
  longer owns the packet byte layout for the reply families this phase targeted.
- `make -C mainboard/LxrStm32 -j4 stm32` succeeds after the Phase 2 cleanup.
- The parser still relies on the existing `seq_sendMainStepInfoToFront()` and
  `seq_sendStepInfoToFront()` sysex response helpers in Sequencer for the
  request-data paths that already live outside the parser file.

### Phase 3: Sequencer and Endpoint Restore Migration

- Move the restore begin/done, restore parameter, restore morph parameter, and
  global morph send blocks out of `sequencer.c` and `EndpointRestore.c` and
  into `frontPanelSendingProtocol.c/.h`.
- Make the send module own the reusable restore packet families:
  - `PARAM_RESTORE_BEGIN` and `PARAM_RESTORE_DONE` framing;
  - `PRF_RESTORE_PARAM_CC` / `PRF_RESTORE_PARAM_CC2` raw parameter pushes;
  - `PRF_RESTORE_MORPH_CC` / `PRF_RESTORE_MORPH_CC2` raw morph pushes;
  - global morph report packets using `FRONT_SEQ_REPORT_GLOBAL_MORPH_LSB` and
    `FRONT_SEQ_REPORT_GLOBAL_MORPH_MSB`.
- Convert the sequencer-visible feedback packets to send helpers:
  - pattern-change echo;
  - run-stop echo;
  - beat LED pulse on/off;
  - current-step LED indicator;
  - track-rotation reset/reply packets;
  - the SysEx step-export helpers that back `seq_sendMainStepInfoToFront()` and
    `seq_sendStepInfoToFront()`.
- Keep the restore queueing, endpoint selection, temp/normal boundary
  decisions, and voice-mask iteration logic in `Sequencer` and `Preset`; only
  the packet emission should move.
- Preserve the raw parameter index rules exactly. The restore helpers must send
  the raw stored parameter number, not the live-CC `+1` translated display form.
- Keep the existing blocking semantics intact:
  - restore begin/done packets still use the wait-for-space priority path;
  - restore parameter pushes still block while the queue is draining;
  - the visible sequencer feedback packets stay on the ordinary non-blocking
    send path.
- Leave the parser-side load/session bridge alone in this phase; the goal is to
  move visible output packets out of the Sequencer and Preset restore services
  without changing the receive-side restore handshake.
- Build immediately after the send-helper move and run the temp-boundary and
  restore smoke tests before changing anything in the MIDI path.

### Phase 3 Progress Notes

- `frontPanelSendingProtocol.c/.h` now owns the restore begin/done framing,
  restore parameter/morph parameter packet composers, global morph report
  packets, pattern-change echoes, run-stop echoes, beat LED pulses, current-step
  LED pulses, track-rotation helper packets, and the SysEx step-export helpers.
- `sequencer.c` and `EndpointRestore.c` now call the send module directly
  instead of hand-building those packet triples inline.
- The queueing logic, temp-boundary decisions, and restore phase machines stay
  where they were; only packet emission moved.
- `seq_sendMainStepInfoToFront()` and `seq_sendStepInfoToFront()` remain the
  public Sequencer wrappers for the parser, but their packet assembly now lives
  in the send module.
- `make -C mainboard/LxrStm32 -j4 stm32` succeeds after this pass; the only
  remaining warnings are the pre-existing parser and sequencer warnings that
  were already present before the split.

### Phase 4: MIDI Echo and Voice Feedback Migration

- Before this phase starts, explicitly notify the user that the work is now
  touching the MIDI-risk seam and ask for MIDI-specific testing after the code
  change.
- First pass: add `GlobalMidiParser.c/.h` and move the MIDI-wide system logic
  there.
- The global file should absorb `midiParser_checkMtc()` and the
  system-message branch of `midiParser_parseMidiMessage()`, including the
  `MIDI_CLOCK`, `MIDI_START`, `MIDI_CONTINUE`, `MIDI_STOP`, and
  `MIDI_MTC_QFRAME` handling.
- Move the front-panel run-stop echo out of `MidiParser.c` into the global file
  so the same module owns the MTC/start-stop trigger and the visible feedback.
- Keep `midiParser_parseUartData()` in `MidiParser.c` for this pass so the byte
  stream coordinator stays stable while the global split is introduced.
- Keep the per-voice note, CC, routing, and bank-change logic in `MidiParser.c`
  until the global file compiles and smoke-tests cleanly.
- If the global split needs a new send helper for run-stop or MTC feedback, add
  it to `frontPanelSendingProtocol.c/.h` rather than opening a new inline
  packet builder in the MIDI file.
- Pause after the global split and run MIDI-specific smoke tests for
  external-sync start/stop, MTC start/stop, and note routing before moving the
  channel ladder.
- Second pass: add `ChannelMidiParser.c/.h` and move the per-channel MIDI work
  there.
- The channel file should absorb the per-voice note-on/note-off routing, the
  per-voice CC ladder, and the helper logic currently hidden inside
  `midiParser_ccHandler()`.
- Move the bank-change echo, parameter echo, and patch-reset mirroring out of
  `MidiParser.c` in the same pass so the channel parser owns both the routing
  decision and the visible front-panel feedback for those events.
- Keep the low-parameter and high-parameter split exactly the same while moving
  the packet emission: `PARAM_CC` must still cover `LXRparamNr < 128`, and
  `PARAM_CC2` must still cover the `>= 128` path with the same `-1` / `-128`
  offsets.
- Keep `midiParser_originalCcValues[]`, `midiParser_activeNrpnNumber`, and the
  existing `preset_storeParameterIngress()` / `modNode_originalValueChanged()`
  bookkeeping in place so the refactor only changes ownership, not behavior.
- Leave the routing and filter gates intact during this pass, including the
  MIDI-to-MIDI, MIDI-to-USB, and USB-to-MIDI forwarding decisions.
- Move the `PATCH_RESET` mirroring into the channel/global split only after the
  send helper exists, and keep the raw reset opcode unchanged.
- Third pass: move the voice-activity LED pulse out of `MidiVoiceControl.c` once
  the send helpers are stable.
- Add one dedicated send helper for the voice pulse packet so
  `voiceControl_noteOn()` can delegate the `NOTE_ON, voice, 0` front-panel echo
  without building the triple inline.
- Keep `voiceControl_noteOff()` unchanged in this phase unless a shared helper
  is needed for symmetry; note-off currently only updates sequencer state and
  does not emit front-panel feedback.
- Keep `MidiParser.c` as the broad stream coordinator after these moves; do not
  start the receive-side `frontPanelParser` split yet.
- Verify the full MIDI-visible behavior after each pass: note-on/off echo,
  bank-change echo, parameter echo order, patch reset mirroring, and external
  sync start/stop.
- Re-test the low-parameter versus high-parameter parameter echo split, because
  that is the easiest place to accidentally regress the front-panel numbering.
- Treat this phase as complete only when the new MIDI ownership files compile
  cleanly and the visible MIDI feedback packets are still emitted in the same
  order as before.
- Implementation checkpoint: `GlobalMidiParser.c/.h` now owns the MTC/start-
  stop and system-message handling, `ChannelMidiParser.c/.h` now owns the
  front-panel bank/parameter/patch-reset echo helpers plus the note-on/note-off
  routing helpers, and `MidiVoiceControl.c` now delegates the voice LED pulse
  through the channel helper.
- The low-risk MIDI feedback split is build-verified again after this pass.
- The remaining deeper per-channel CC ladder still lives in `MidiParser.c` for
  the next MIDI refactor slice; this pass stopped at the least risky structural
  seam.

### Phase 5: Channel and Global Case Parsing Transfer

- This is the phase that moves the remaining `if (chanonly == ...)` case ladders
  out of `MidiParser.c` and into the new ownership files.
- The current `midiParser_MIDIccHandler()` body is still the umbrella dispatcher
  for all MIDI CC handling. Phase 5 should turn it into a thin router that hands
  off to voice/global-specific handler functions while leaving the top-level
  entry point intact for callers.
- Move the voice-specific case blocks for voices 0-6 into
  `ChannelMidiParser.c/.h`, one voice cluster at a time:
  - the DRUM1 block at `if (chanonly == midiParser_voiceMidiChannel(0))`;
  - the DRUM2 block at `if (chanonly == midiParser_voiceMidiChannel(1))`;
  - the DRUM3 block at `if (chanonly == midiParser_voiceMidiChannel(2))`;
  - the SNARE block at `if (chanonly == midiParser_voiceMidiChannel(3))`;
  - the CYMBAL block at `if (chanonly == midiParser_voiceMidiChannel(4))`;
  - the HAT CLOSED block at `if (chanonly == midiParser_voiceMidiChannel(5))`;
  - the HAT OPEN block at `if (chanonly == midiParser_voiceMidiChannel(6))`.
- Each voice block should carry over the full switch body unchanged in behavior:
  - the voice-local CC cases and the `LXRparamNr` assignments;
  - the voice-local DSP side effects such as oscillator tuning, filter drive,
    envelope setters, transient generator values, LFO settings, mix routing,
    mute toggles, and note override / voice-channel dependent logic;
  - the shared bookkeeping block that records `midiParser_originalCcValues[]`,
    calls `preset_storeParameterIngress()`, calls
    `modNode_originalValueChanged()`, and optionally forwards the front-panel
    parameter echo when `midiParser_txRxFilter` allows it;
  - the exact low-parameter versus high-parameter echo split via
    `channelMidiParser_sendParameterEcho()`.
- Move the global-channel case block into `GlobalMidiParser.c/.h`, including
  the `chanonly == midiParser_voiceMidiChannel(7)` ladder and any global-only
  CC handling, pattern selection, roll control, mute control, and related
  sequencer side effects.
- Keep `midiParser_parseUartData()` in `MidiParser.c` so the byte-stream
  coordinator and running-status handling stay stable while the case ladders
  move.
- Keep `midiParser_ccHandler()` in place as the shared NRPN/CC entry point for
  automation and live MIDI input, but extract any helper that becomes truly
  voice/global specific into the new modules instead of leaving a cross-file
  switch maze behind.
- If the extraction reveals repeated per-voice helper patterns, factor them into
  `static` helpers inside the new module rather than duplicating eight nearly
  identical bookkeeping blocks.
- Preserve the existing parameter bookkeeping exactly:
  `midiParser_originalCcValues[]`, `midiParser_activeNrpnNumber`,
  `preset_storeParameterIngress()`, and `modNode_originalValueChanged()` must
  keep the same call order and index math.
- Preserve the routing/filter gates and the source/destination decisions around
  `midiParser_txRxFilter`, `seq_recordActive`, `uart_sendMidi()`, and
  `usb_sendMidi()` so this phase changes ownership only.
- Keep the `BANK` and `MOD_WHEEL` special handling behavior intact while moving
  the cases, including the bank-mask construction, the global morph path, and
  the `preset_morphLoadDisabled` guard.
- Move the global `TRACK*_SOUND_OFF`, `ALL_SOUND_OFF`, and `RESET_ALL_CONTROLLERS`
  behaviors together with the global block so the “global channel” logic lives
  in one place rather than being split across two files.
- Treat this as an implement-test-feedback phase because the broad CC ladder is
  the highest-risk MIDI seam left in the tree.
- Stop after the first voice block has moved if the build or MIDI smoke tests
  show a regression, then continue the remaining voice blocks only after the
  first extraction proves the helper boundary is sound.
- The examples called out by the user, such as the voice-scoped
  `if (chanonly == midiParser_voiceMidiChannel(4))` block and the global
  `if (chanonly == midiParser_voiceMidiChannel(7))` block, are part of this
  phase, not the later receive-side split.
- Phase 5 should finish with `MidiParser.c` reduced to the stream parser plus a
  small routing shim, while the per-voice and global case parsing lives in the
  new ownership files.

### Phase 5 Progress Notes

- `ChannelMidiParser.c/.h` now owns the per-voice CC case ladders for voices
  0-6, including the shared parameter bookkeeping and low/high parameter echo
  split.
- `GlobalMidiParser.c/.h` now owns the global-channel CC case ladder and the
  global-only sequencer side effects.
- `MidiParser.c` now routes `midiParser_MIDIccHandler()` into the new channel
  and global handlers instead of carrying the full case maze itself.
- `make -C mainboard/LxrStm32 -j4 stm32` completes successfully after the
  split.
- Future work that touches the channel/global MIDI CC path should be treated as
  a high-risk MIDI change and smoke-tested explicitly before the next phase is
  considered done.

### Phase 6: Comment and API Documentation Sweep

- Make the documentation pass a separate phase so behavior and comment work do
  not get mixed together.
- Review every exported function and exported/shared variable in
  `mainboard/LxrStm32/src/uARTFrontSYX/` and `mainboard/LxrStm32/src/MIDI/`.
- Add a comment block in code next to every exported function declaration and
  definition that explains ownership, inputs, outputs, and side effects.
- Add a matching comment near every exported `extern` or shared file-scope
  variable that explains what owns it and why other modules need access to it.
- Normalize the public headers first: `Uart.h`, `frontPanelParser.h`,
  `frontPanelSendingProtocol.h`, `MidiParser.h`, `MidiVoiceControl.h`,
  `GlobalMidiParser.h`, and `ChannelMidiParser.h`.
- Then fill in the corresponding source files so the exported functions are not
  left undocumented at the definition site.
- Keep this phase limited to comment and declaration cleanup only; do not let it
  become a behavior pass in disguise.

### Phase 6 Coverage Map

- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.h` and `Uart.c` need ownership
  comments for the raw transport surface: `initMidiUart()`,
  `uart_sendMidi()`, `uart_sendMidiByte()`, `uart_processMidi()`,
  `initFrontpanelUart()`, `uart_processFront()`, `uart_sendFrontpanelByte()`,
  `uart_sendFrontpanelPriorityByte()`, `uart_sendFrontpanelPriorityByteWait()`,
  `uart_sendFrontpanelSysExByte()`, and `uart_clearFrontFifo()`. The source
  comments should also make the USART2/USART3 IRQ handlers read as transport
  plumbing rather than protocol ownership.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.h` and
  `frontPanelParser.c` need comments for the shared receive state and parser
  entry points: `frontParser_activeTrack`, `frontParser_shownPattern`,
  `frontParser_sysexActive`, `frontParser_activeFrontTrack`,
  `frontParser_parseUartData()`, `frontParser_isQuietUi()`, and
  `frontParser_handleMidiMessage()`, plus the deferred-load and PRF cache state
  blocks that still live in the source file.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h` and
  `frontPanelSendingProtocol.c` need comment blocks for the whole outbound
  surface:
  - raw packet helpers: `frontPanelSending_sendByte()`,
    `frontPanelSending_sendPriorityByte()`,
    `frontPanelSending_sendPriorityByteWait()`,
    `frontPanelSending_sendSysExByte()`,
    `frontPanelSending_sendTriplet()`,
    `frontPanelSending_sendPriorityTriplet()`, and
    `frontPanelSending_sendPriorityTripletWait()`;
  - ack and reply helpers: `frontPanelSending_sendCallbackAck()`,
    `frontPanelSending_sendSampleUploadAck()`,
    `frontPanelSending_sendSampleCountReply()`,
    `frontPanelSending_sendTrackLengthReply()`,
    `frontPanelSending_sendTrackRotationReply()`,
    `frontPanelSending_sendTrackRotationValue()`,
    `frontPanelSending_sendPatternParamsReply()`,
    `frontPanelSending_sendPatternDataReply()`,
    `frontPanelSending_sendEuklidParamsReply()`,
    `frontPanelSending_sendActiveTrackReply()`,
    `frontPanelSending_sendMainStepLedReply()`,
    `frontPanelSending_sendStepParamsReply()`,
    `frontPanelSending_sendSysexStartAck()`,
    `frontPanelSending_sendSysexEndAck()`,
    `frontPanelSending_sendSysexReceiveAck()`,
    `frontPanelSending_sendSysexStepAck()`, and
    `frontPanelSending_sendSysexBeginPatternTransmitAck()`;
  - restore and runtime helpers: `frontPanelSending_sendRestoreBegin()`,
    `frontPanelSending_sendRestoreDone()`,
    `frontPanelSending_sendRestoreParam()`,
    `frontPanelSending_sendRestoreMorphParam()`,
    `frontPanelSending_sendGlobalMorphReport()`,
    `frontPanelSending_sendPatternChange()`,
    `frontPanelSending_sendRunStop()`,
    `frontPanelSending_sendBeatLed()`,
    `frontPanelSending_sendCurrentStepLed()`,
    `frontPanelSending_sendMainStepInfo()`,
    `frontPanelSending_sendStepInfo()`,
    `frontPanelSending_updateTrackLeds()`, and
    `frontPanelSending_updateSubStepLeds()`;
  - flow-control helpers: `frontPanelSending_sendFlowGrant()`,
    `frontPanelSending_sendFlowGrantWait()`,
    `frontPanelSending_sendFlowAbort()`, and
    `frontPanelSending_sendPrfCacheStatus()`.
- `FrontPanelProtocol.h` should carry a short ownership comment that makes it
  clear the file is the opcode namespace, not the send/receive implementation.
- `mainboard/LxrStm32/src/MIDI/MidiParser.h` and `MidiParser.c` need comment
  coverage for the top-level parser entry points and shared state:
  `midiParser_parseUartData()`, `midiParser_ccHandler()`,
  `midiParser_parseMidiMessage()`, `midiParser_MIDIccHandler()`,
  `midiParser_handleStatusByte()`, `midiParser_calcDetune()`,
  `midiParser_checkMtc()`, `midiDebugSend()`, `midiParser_setRouting()`,
  `midiParser_setFilter()`, `midiParser_originalCcValues`,
  `midiParser_selectedLfoVoice`, `midiParser_txRxFilter`, the MIDI cache
  buffers, and the voice/channel tables.
- `MidiParser.c` should also document that it is now the stream/router shim,
  not the owner of the per-voice or global case ladders that moved in Phase 5.
- `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.h` and
  `ChannelMidiParser.c` need comments for the split channel-owned helpers:
  `channelMidiParser_noteOn()`, `channelMidiParser_noteOff()`,
  `channelMidiParser_MIDIccHandler()`, `channelMidiParser_sendBankChange()`,
  `channelMidiParser_sendParameterEcho()`,
  `channelMidiParser_sendPatchReset()`, and
  `channelMidiParser_sendVoiceActivity()`.
- `ChannelMidiParser.c` should document the channel-local modulation helpers
  and the fact that it now owns the per-voice CC ladder and its bookkeeping
  block.
- `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.h` and
  `GlobalMidiParser.c` need comments for `globalMidiParser_handleSystemMessage()`,
  `globalMidiParser_MIDIccHandler()`, and `midiParser_checkMtc()`, along with
  the shared MTC state they manage.
- `GlobalMidiParser.c` should explain that it now owns the global-channel CC
  ladder plus the global-only sequencer side effects such as bank/morph/reset
  handling.
- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.h` and
  `MidiVoiceControl.c` need comments for `voiceStatus`,
  `voiceControl_noteOn()`, `voiceControl_noteOff()`, and
  `voiceControl_isVoicePlaying()`, because that module is still the owner of the
  live voice state used by the parser split.
- Any new exported function or shared variable introduced by the earlier split
  phases must get the same comment treatment in this pass before the phase can
  be considered complete.

### Phase 6 Progress Notes

- The public headers now carry ownership comments for the transport, parser,
  send, and MIDI ownership surfaces that Phase 6 called out.
- `frontPanelParser.c`, `frontPanelSendingProtocol.c`, `MidiParser.c`,
  `ChannelMidiParser.c`, `GlobalMidiParser.c`, `MidiVoiceControl.c`, and
  `Uart.c` now have matching definition-site comments for the documented
  exported functions and shared state.
- `make -C mainboard/LxrStm32 -j4 stm32` completes successfully after the
  comment sweep.
- The next phase can focus on the receive-side split without first having to
  clean up undocumented ownership boundaries in the current modules.

- Add a standing rule for later refactor phases: any new exported function or
  shared variable introduced by a structural change must ship with its comment
  block in the same phase before the phase can be considered done.

### Phase 7: ???

### Phase 8: Receive-Side Split ???

#### Goal: ???

- Move the current front-panel receive state machine, sysex handling, and
  parser-local load/session bridge fully into `frontPanelParser.c/.h` so that
  file becomes the receive module in place.
- Preserve the option to rename `frontPanelParser.c/.h` to
  `frontPanelReceivingProtocol.c/.h` only after the send/receive split is
  completely finished and the last compatibility edge has been removed.
- Preserve flow-control, quiet-UI, load-ingress, and sysex ack behavior exactly
  while the code is being relocated.


### Phase 9: Header Tightening and Compatibility Cleanup ???

### Exit Criteria

## MIDI Parser Split

### Goal

Keep the broad MIDI stream handling in `MidiParser.c`, but split out the
channel/global ownership layers around it.

The split should make it obvious which code is:

- byte-stream parsing and MIDI routing;
- channel-specific note and CC handling;
- global-channel handling and system message handling;
- front-panel echo generation that is triggered by MIDI input.

### Target file set

- `ChannelMidiParser.c`
- `ChannelMidiParser.h`
- `GlobalMidiParser.c`
- `GlobalMidiParser.h`

### What this split should do

- Leave `MidiParser.c` as the broad stream coordinator at first.
- Move channel-specific handling into a dedicated channel parser when it is
  ready.
- Move global-channel handling into a dedicated global parser when it is ready.
- Preserve current behavior while making the ownership split visible.
- Stop `MidiParser.c` from being the only place that knows both the MIDI stream
  rules and the per-feature echo rules.

### Current MIDI Responsibilities

`MidiParser.c` currently mixes several different concerns:

- raw MIDI message routing and external I/O forwarding;
- system message handling such as MIDI clock, start, stop, and MTC;
- note-on and note-off handling for both global and per-voice channels;
- per-channel CC interpretation and parameter mapping;
- front-panel echo generation for run-stop, bank change, parameter change, and
  patch reset;
- recording and automation side effects that sit on top of the parsing rules.

That mix is why the split should be staged carefully. The code is stable enough
to move, but not so isolated that a single giant rewrite is a good idea.

### Least-Risk MIDI Entry Point

The safest first cut in the MIDI parser split is the global/system-message side
of `midiParser_parseMidiMessage()`.

Why this seam is safer:

- `midiParser_checkMtc()` is already a tiny helper with limited dependencies;
- the `MIDI_CLOCK` / `MIDI_START` / `MIDI_STOP` / `MIDI_MTC_QFRAME` branch is a
  compact top-level switch with visible side effects;
- the run-stop echo is a simple front-panel send path once the send helpers
  exist;
- none of that code owns the long per-voice CC ladder or the byte-level UART
  state machine.

What should stay put for the first pass:

- `midiParser_parseUartData()` should remain in `MidiParser.c`;
- the running-status / sysex state machine is the highest-risk seam in the
  file;
- the large per-voice `MIDI_CC` ladder should stay put until the global
  extraction proves stable.

That means Phase 4 should begin with a small global parser extraction and only
then move to the channel parser.

### Proposed Channel Ownership

- `ChannelMidiParser.c/.h` should own the per-voice note routing, note override
  checks, channel-specific CC mapping, parameter echo packets that are triggered
  by voice/channel edits, and the loops that fan a MIDI message across matching
  synth voices.
- `GlobalMidiParser.c/.h` should own the global channel message handling,
  system-message reactions, run-stop mirroring, bank-change behavior, patch
  reset mirroring, and the MIDI clock / MTC state transitions that affect the
  sequencer as a whole.
- `MidiParser.c` should keep the outer stream coordination until those two
  layers are proven, then become a dispatcher and compatibility boundary rather
  than a catch-all implementation file.

### Detailed MIDI Migration Order

- First, identify the blocks in `MidiParser.c` that already align with global
  versus channel behavior so they can move with the least logic reshaping.
- Move `midiParser_checkMtc()` and the system-message half of
  `midiParser_parseMidiMessage()` into `GlobalMidiParser` first.
- Keep `midiParser_parseUartData()` in the main parser file during that first
  pass; it is the highest-risk state machine in the file and does not need to
  move for the first global extraction.
- Once the global file is green, move the per-channel CC and note dispatch into
  `ChannelMidiParser`, but keep the old entry points alive until the new files
  prove the same note and echo behavior.
- Keep the front-panel send cleanup and the MIDI split in sequence, not in
  parallel, so any send regression is easier to isolate.
- Only after the send helpers are stable should the channel/global split be
  allowed to change file boundaries in `MidiParser.c`.

### Risk-Managed Checkpoints

- After moving the global message handling, build and verify MIDI clock,
  transport start/stop, and external-sync behavior before touching the channel
  parser.
- After moving the per-channel handling, verify note-on, note-off, parameter
  echo, and bank-change behavior on at least one voice channel and the global
  channel.
- If the MIDI split exposes any missing front-panel echo helper, stop and add
  the helper in the send protocol layer rather than reintroducing direct send
  code in the MIDI file.

### Exit Criteria

- The parser split exists in the file layout.
- The first implementation pass does not force a behavior rewrite just to make
  the split visible.
- `MidiParser.c` is no longer the only place that owns both the message stream
  and the full per-channel/global behavior set.
- MIDI-driven front-panel feedback still works after the move.

## Direct Send Inventory

This inventory is the concrete reminder of why the send-side split matters.
These call sites currently assemble front-panel packets inline and therefore
should be treated as the first migration candidates once
`frontPanelSendingProtocol.c/.h` exists.

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
  - restore packet emission in the restore helper block near the top of the
    file;
  - global morph reporting during temp-boundary transitions;
  - pattern-change and run-stop status reports;
  - beat LED and current-step LED updates;
  - SysEx pattern and step export packets.
- `mainboard/LxrStm32/src/Preset/EndpointRestore.c`
  - restore parameter and morph parameter packets;
  - restore begin / done / global morph report sequences.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`
  - flow grant and abort helpers;
  - cache status replies;
  - LED / sample / query response packets;
  - PRF restore responses and other parser-owned front-panel replies.
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
  - run-stop mirroring back to the front panel;
  - bank-change echoes;
  - parameter-echo packets for MIDI-driven edits;
  - patch reset and other global messages that are mirrored to the display.
- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c`
  - voice activity feedback packets.

## The `.pat` Rule

Pattern-only background loading is the important wrinkle.

`.pat` background loading should never switch preset parameter read/write away
from normal storage.

That means:

- pattern data can be staged in pattern-owned temp structures;
- parameter ingress must remain on normal storage for `.pat` background loads;
- pattern temp/normal ownership and parameter temp/normal ownership must remain
  separate;
- the implementation may need an explicit load-kind discriminator or mode bit to
  distinguish pattern-only background loads from parameter-bearing file loads.

This rule is easy to miss if the pattern loader and parameter loader share one
broad session object.
That is exactly why the future interface needs to keep the ownership boundaries
explicit.

## Future Shape After The Audit

The comms layer should end up with a very clear split:

- `Uart.c/.h` for byte transport only;
- `frontPanelReceivingProtocol.c/.h` for parsing and receive-side dispatch;
- `frontPanelSendingProtocol.c/.h` for outbound framing and status messages;
- `FrontPanelProtocol.h` for the opcode namespace;
- `Preset` for storage and session semantics;
- `Pattern` for pattern storage and pattern temp behavior.

The parser should not keep a second background-load model after the remaining
parser-local bridge is retired.
It should route semantics into `Preset` and `Pattern`, then get out of the way.

The background-load initiators can stay thin or stubbed while the replacement
path is being connected.
That is acceptable as long as they do not reintroduce a second cache-as-
authority model.

## Build and Verification Plan

The split is risky enough that each migration phase should end with at least
one compile check and one behavior check.

### Build checkpoints

- Run `make -C mainboard/LxrStm32 -j4 stm32` after each send-side migration
  phase.
- Use a clean rebuild at least once after the new protocol files are added so
  the dependency scanner proves the new include graph is correct.
- If the file split introduces new helper headers, verify they are reachable
  through the existing `src/uARTFrontSYX`, `src/MIDI`, `src/Preset`, and
  `src/Sequencer` include paths before changing the call sites.

### Front-panel smoke tests

- Verify flow begin, flow grant, flow abort, and quiet-UI transitions still
  behave the same.
- Verify `.ALL` / `.PRF` style load traffic still reaches the front panel and
  still resolves restore-ready / restore-ack behavior correctly.
- Verify step and pattern query responses still light the expected LEDs and
  return the same parameter values.
- Verify SysEx request/response traffic for pattern and step export still
  completes without packet corruption.

### Sequencer and restore smoke tests

- Verify restore begin/done still brackets endpoint pushes.
- Verify global morph reporting still updates the display without feeding back
  into live DSP state.
- Verify beat LEDs, current-step LEDs, pattern changes, and run-stop updates
  still appear on the front panel.
- Verify temp-boundary switching still performs the same restore/report work as
  before the split.

### MIDI smoke tests

- Verify MIDI note-on and note-off still pulse the front-panel activity LED
  path.
- Verify bank change echoes still appear with the same voice packing.
- Verify parameter echo packets still use the same low/high parameter split.
- Verify patch reset mirroring and run-stop mirroring still match the current
  external MIDI behavior.
- Verify MTC start/stop handling still drives the sequencer and front-panel
  feedback the same way.

## Guardrails

- Do not broaden blocking waits just because the comms layer is now better
  organized.
- Do not let display-only restore traffic feed back into live DSP state.
- Do not let transport code inspect `SeqKitState` directly.
- Do not create a second load/session model beside the temp/normal storage
  model.
- Do not merge pattern-only background loading with parameter storage switching.
- Do not let the future send/receive split blur the ownership boundary between
  parsing bytes and deciding what they mean.
- Do not keep the old parser-local send helper blocks alive after the new send
  module is in place unless they are strictly compatibility shims with a clear
  removal point.
- Do not split `MidiParser.c` into channel/global files before the front-panel
  send helpers are stable, because the MIDI echo paths currently depend on that
  send behavior.
