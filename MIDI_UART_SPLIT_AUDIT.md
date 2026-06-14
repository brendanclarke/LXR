# MIDI_UART_SPLIT_AUDIT

Date: 2026-06-13
Status: Protocol/parser split tracking moved out of `PRESET_CONSOLIDATION_AUDIT.md` so the Preset refactor audit can stay focused on ownership, naming, and live-apply extraction. This file now tracks the `uARTFrontSYX` and MIDI parser split work.

## Purpose

This audit tracks the follow-on protocol and MIDI layout cleanup after the Preset
consolidation work has stabilized.

The goal is not to change behavior first. The goal is to make the send/receive
boundary and the MIDI parser ownership split visible in the file layout before
we ask the code to move.

## Current Shape

The current files that matter for this split are:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/FrontPanelProtocol.h`
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiParser.h`

Those files still combine too much protocol, parser, and transport ownership in
one place. The split work should expose that ownership more clearly without
forcing a risky behavior rewrite too early.

## Front-Panel UART Split

### Goal

Reshape `mainboard/LxrStm32/src/uARTFrontSYX/` into a visibly separated send
and receive layout.

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

### Current Direct Send Surface

These are the STM-side files that currently emit front-panel bytes directly and
need to be routed through the future send protocol layer:

| File | Current direct-send role |
|------|--------------------------|
| `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c` | Flow grant/abort replies, PRF cache status, restore-ready/ack responses, LED updates, SysEx framing, step/pattern query replies, sample count replies, and other front-panel responses. |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | Restore begin/done packets, global morph reports, pattern-change notifications, run-stop updates, LED updates, and SysEx pattern/step export packets. |
| `mainboard/LxrStm32/src/Preset/EndpointRestore.c` | Restore parameter packets, morph parameter packets, restore begin/done sequencing, and global morph report packets. |
| `mainboard/LxrStm32/src/MIDI/MidiParser.c` | Front-panel run-stop echoes, bank-change echoes, parameter echoes, patch reset, and MIDI-output mirroring. |
| `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c` | Note-on style front-panel feedback for voice activity. |

The send protocol split should not try to make each of these files invent its
own byte packing rules. The point is to pull those repeated packet-building
patterns into one reusable sending API.

### Sending API Shape

The future `frontPanelSendingProtocol.h` should document the transmit modes and
the helper families that choose between them. The exact function names can
settle during implementation, but the exported surface needs to cover the same
small set of packet styles repeatedly used today:

- command/status byte triples for ordinary front-panel replies;
- priority command/status byte triples for flow-control and restore
  synchronization;
- wait-for-space variants for the restore path and any other blocking packet
  sequence;
- SysEx-aware byte emission for front-panel packet streams that stay open
  across multiple bytes;
- packet helpers for the repeated protocol families used by Sequencer and
  EndpointRestore, such as restore begin/done, restore param, restore morph
  param, flow grant/abort, pattern change, run-stop, LED status, and query
  responses.

The header should also document the transmit flags or mode bits used by those
helpers so the caller can tell at a glance whether a packet is:

- normal or priority;
- blocking or non-blocking;
- SysEx-safe or ordinary command traffic;
- allowed to bypass the quiet UI gate or not.

### Exit criteria

- The transport and protocol sides are visible as separate files.
- The parser no longer looks like the only place where send and receive behavior
  can live.
- No STM-side file outside `frontPanelSendingProtocol.c/.h` sends front-panel
  command bytes directly.
- The duplicated packet-building logic in Sequencer, EndpointRestore, and the
  parser-facing front-panel helpers collapses into reusable send helpers.

## MIDI Parser Split

### Goal

Keep the broad MIDI stream handling in `MidiParser.c`, but split out the
channel/global ownership layers around it.

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

### Exit criteria

- The parser split exists in the file layout.
- The first implementation pass does not force a behavior rewrite just to make
  the split visible.

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
  - step LED updates and step/pattern SysEx exports.
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

## Consolidation Follow-Up

The send-side API should be consolidated around a small set of reusable
helpers, not left as a collection of one-off wrappers that still call
`uart_sendFrontpanel*()` internally.

Recommended implementation shape:

- keep `Uart.c` as the transport implementation and FIFO owner;
- move packet composition into `frontPanelSendingProtocol.c`;
- move the current direct-send helper blocks in `sequencer.c`,
  `EndpointRestore.c`, `MidiParser.c`, `MidiVoiceControl.c`, and
  `frontPanelParser.c` onto the new sending API;
- document the transmit-mode flags in `frontPanelSendingProtocol.h` so the
  callers can choose the right emission path without duplicating the packet
  rules.

Exit criteria for this follow-up:

- no command-oriented front-panel bytes are sent directly from outside
  `frontPanelSendingProtocol.c/.h`;
- the protocol helpers become reusable by multiple callers instead of being
  duplicated in each file;
- the send layer is the only place that needs to understand the packet
  packing rules for priority, wait, and SysEx emission.

## Coordination Notes

- Do not redirect the call graph to the new split files too early.
- Keep the behavior stable while the ownership boundaries are being made
  visible.
- Let the Preset refactor finish its ownership and naming cleanup before this
  audit drives larger parser rewrites.
