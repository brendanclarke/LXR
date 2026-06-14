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
- `frontPanelReceivingProtocol.c/.h` for receive-side parsing and dispatch;
- `frontPanelSendingProtocol.c/.h` for outbound framing and packet helpers;
- `FrontPanelProtocol.h` for the opcode namespace;
- `frontPanelParser.c/.h` only as a compatibility shim if the migration needs
  one while the tree is being rewired.

### Target file set

- `Uart.c`
- `Uart.h`
- `frontPanelReceivingProtocol.c`
- `frontPanelReceivingProtocol.h`
- `frontPanelSendingProtocol.c`
- `frontPanelSendingProtocol.h`

The important contract here is that `Uart.c` keeps the raw transport and FIFO
ownership, while the new send/receive protocol files own the packet
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

The future receive module should own the current parser responsibilities that
are about reading and interpreting bytes, not emitting them.

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
  be reduced into `frontPanelReceivingProtocol.c` or made into a thin wrapper.

### Phase 1: Send-Side Scaffolding

- Add `frontPanelSendingProtocol.c/.h` under `src/uARTFrontSYX`.
- Move the small parser-local packet helpers there first: flow grant, flow
  grant-wait, flow abort, PRF cache status, and any small status-triplet helper
  that simply assembles a reusable packet family.
- Introduce explicit transmit mode documentation in the new header so callers
  can see which packets are priority, wait-blocking, sysex-safe, or quiet-ui
  gated.
- Keep `frontPanelParser.c` compiling during this step by leaving the existing
  receive logic untouched except for the helper calls that now route through the
  new send module.
- Keep `Uart.c` behavior stable in this step; do not refactor the IRQ or FIFO
  ownership at the same time.
- Verify the build after the helper move and smoke-test the current flow-control
  handshake paths before touching any of the higher-volume send sites.

### Phase 2: Parser-Local Send Cleanup

- Remove the remaining inline parser send sequences from the main receive file
  and route them through the send helpers.
- Convert the parser's direct LED, sample count, query, and sysex response
  packets to the new reusable send surface.
- Keep the receive dispatch and the parser-local load/session bridge in place so
  behavior stays constant while the send helper abstraction proves itself.
- Check that `frontPanelParser.c` is no longer the place where packet assembly
  rules are duplicated, even if it still owns the receive state machine.
- Rebuild and re-run the parser-facing hardware flows that rely on sysex
  request/response behavior before moving to the sequencer and restore callers.

### Phase 3: Sequencer and Endpoint Restore Migration

- Move the restore begin/done, restore parameter, restore morph parameter, and
  global morph send blocks out of `sequencer.c` and `EndpointRestore.c`.
- Convert the beat LED, current-step LED, pattern-change, run-stop, and SysEx
  export calls in `sequencer.c` to the send helpers.
- Keep the restore queueing and the temp/normal boundary decisions in
  `Preset`; only the packet emission should move.
- Make sure the new send helpers preserve the existing priority and wait-for-
  space semantics, because restore traffic currently depends on those blocking
  rules.
- Build immediately after this phase and run the temp-boundary and restore
  smoke tests before changing anything in the MIDI path.

### Phase 4: MIDI Echo and Voice Feedback Migration

- Start with a new `GlobalMidiParser` file that absorbs `midiParser_checkMtc()`
  and the system-message side of `midiParser_parseMidiMessage()`.
- Move the front-panel run-stop echo out of `MidiParser.c` at the same time so
  the new global file owns both the trigger and the visible feedback for the
  same event.
- Leave the per-voice CC ladder, note routing, and `midiParser_parseUartData()`
  in place until the global split compiles and smoke-tests cleanly.
- After that global pass is green, add the `ChannelMidiParser` split for the
  per-voice note and CC handling.
- Move the bank-change, parameter echo, and patch-reset sends out of
  `MidiParser.c` only once the channel parser helpers are in place, so the send
  side and the channel split do not get mixed together in one risky pass.
- Move the voice-activity LED pulse out of `MidiVoiceControl.c` once the send
  helpers are stable, because that is a small and well-contained follow-up once
  the send API already exists.
- Keep the MIDI parsing and voice routing behavior unchanged during the first
  pass; the only goal in this phase is to stop hand-building front-panel packet
  bytes in the MIDI files.
- Verify that MIDI-driven parameter edits still reach the front panel in the
  same form and order, especially the low-parameter versus high-parameter
  split.
- Re-test MIDI note-on/off, bank change, and external sync cases after the
  migration, because these are the easiest places for a send-side helper change
  to accidentally drop a visible feedback packet.

### Phase 5: Receive-Side Split

- Move the current parser state machine, sysex handling, and front-panel receive
  dispatch into `frontPanelReceivingProtocol.c/.h`.
- Keep `frontPanelParser.c` as a thin compatibility wrapper only if that reduces
  call-site churn; if the wrapper no longer adds value, retire it after the
  receiver file is wired in.
- Move the parser-local load/session bridge into the receive module first as a
  mechanical step, then decide whether the remaining bridge can be simplified or
  deleted in a later pass.
- Preserve the current flow-control and quiet-UI behavior during the move.
- Re-run the load, temp-switch, and callback-ack flows before any cleanup pass
  removes the temporary compatibility layer.

### Phase 6: Header Tightening and Compatibility Cleanup

- Narrow `Uart.h` so the transport-only surface is obvious and the front-panel
  protocol helpers are no longer presented as generic UART utilities.
- Narrow the exposed `uart_sendFrontpanel*` surface so only the send module
  retains direct access to the raw primitives once all callers have moved.
- Keep `FrontPanelProtocol.h` as the opcode owner and continue to treat
  `MidiMessages.h` as a compatibility include only.
- Remove any remaining direct front-panel send declarations from headers that
  are not supposed to own protocol framing.
- Make sure the final include graph reflects the ownership split: transport,
  receive protocol, send protocol, and opcode namespace should be separate
  concerns.
- If any compatibility wrapper is still needed for a phased migration, keep it
  behind the send or receive module boundary, not in generic shared headers.
- Finish this phase only after the build is clean and the high-risk feedback
  checks have already passed.

### Exit Criteria

- The transport and protocol sides are visible as separate files.
- The parser no longer looks like the only place where send and receive behavior
  can live.
- No STM-side file outside `frontPanelSendingProtocol.c/.h` sends front-panel
  command bytes directly.
- The duplicated packet-building logic in Sequencer, EndpointRestore, MidiParser,
  MidiVoiceControl, and the parser-facing front-panel helpers collapses into
  reusable send helpers.
- The current parser-local load/session bridge is either clearly transitional or
  already retired.

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
