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

### What this split should do

- Keep UART transport code out of the protocol parser files.
- Make front-panel receive handling explicit in the filesystem.
- Make front-panel send handling explicit in the filesystem.
- Avoid keeping the parser as the place where every front-panel concern lands.

### Exit criteria

- The transport and protocol sides are visible as separate files.
- The parser no longer looks like the only place where send and receive behavior
  can live.

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

## Coordination Notes

- Do not redirect the call graph to the new split files too early.
- Keep the behavior stable while the ownership boundaries are being made
  visible.
- Let the Preset refactor finish its ownership and naming cleanup before this
  audit drives larger parser rewrites.

