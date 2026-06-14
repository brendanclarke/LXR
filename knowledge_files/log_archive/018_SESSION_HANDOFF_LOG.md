# LXR -bc- Enhanced Firmware - Session 018 Handoff Log

**Project**: LXR -bc- Enhanced Firmware
**Session goal**: Continue the comms and MIDI split work by extracting the transitional front-panel receive bridge, consolidating outbound front-panel send helpers, and moving the MIDI parser ownership boundaries into channel and global modules without changing the observed firmware behavior.
**Last session summary**: Session 017 finished the remaining Preset rename sweep and mapped the outbound front-panel send work into the permanent comms reference.
**Working repository**: `/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod` on branch `custom-develop-patload-envmod` with uncommitted split work.
**Constraints today**: Keep the send/receive and MIDI ownership splits behavior-preserving, preserve the raw parameter index rules, and do not reintroduce parser-local packet builders or hidden shared ownership.

Key files to be aware of:
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.h`
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiParser.h`
- `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c`
- `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.h`
- `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c`
- `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.h`
- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c`
- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.h`
- `mainboard/LxrStm32/src/Preset/EndpointRestore.c`
- `mainboard/LxrStm32/src/Preset/PresetLoadCache.c`
- `mainboard/LxrStm32/src/Preset/PresetLoadCache.h`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/FrontPanelProtocol.h`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`
- `MEMORY.md`

## Session 018 Summary

Session 018 completed the current implementation pass for the UART and MIDI ownership split.

The code-side results were:

- The front-panel send helpers were consolidated behind `frontPanelSendingProtocol.c/.h`.
- The MIDI parser was split into channel-specific and global-specific helper modules.
- The transitional receive-side front-panel load/session bridge moved out of `frontPanelParser.c` and into a dedicated `PresetLoadCache` module.
- The remaining parser, sequencer, restore, and voice-control call sites were rewired to the new helper surface.
- The public headers were updated with ownership comments so the new split reads clearly in the source tree.

The user then re-tested the important front-panel and MIDI paths and reported no visible regressions.

## Detailed Implementation

### Front-Panel Send Helpers

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h` now owns the outbound front-panel packet builders that used to be open-coded across the parser, sequencer, endpoint restore, and voice-control paths.

The module now provides:

- raw transport wrappers for front-panel bytes and SysEx bytes;
- generic triplet helpers for ordinary, priority, and wait-blocking packets;
- response helpers for callback ack, sample upload ack, sample count, track length, track rotation, pattern params, pattern data, Euclid params, active track, main-step LEDs, step params, and the SysEx request/response ack families;
- restore helpers for restore begin, restore done, restore parameter pushes, restore morph pushes, and the global morph report;
- runtime feedback helpers for pattern change, run/stop, beat LED pulse, current-step LED, main-step info, step info, track-LED updates, and sub-step LED updates;
- flow-control helpers for flow grant, flow grant wait, flow abort, and PRF cache status;
- MIDI-facing front-panel helpers for bank change, parameter echo, patch reset, and voice activity.

The important behavior rules stayed intact:

- the raw parameter index is still treated as authoritative in restore packets;
- low/high parameter numbering is still split at 128 for parameter echo packets;
- wait-blocking restore packets still use the priority FIFO path;
- normal protocol traffic still stays separate from SysEx and flow-control traffic.

`mainboard/LxrStm32/src/Preset/EndpointRestore.c` and `mainboard/LxrStm32/src/Sequencer/sequencer.c` now call these helpers instead of building the front-panel packets inline.

### Receive-Side Bridge Extraction

`mainboard/LxrStm32/src/Preset/PresetLoadCache.c/.h` was added to hold the transitional receive-side load/session bridge that used to sit in `frontPanelParser.c`.

That new module now owns the load-session state cluster and helper surface that the parser still consults during file-load and deferred-performance handling, including:

- deferred performance replay buffers and counters;
- file-load ingress bracket state;
- PRF cache session state and pending counters;
- live snapshot capture for the AVR/STM pattern and parameter bridge;
- live-pattern query helpers for steps, main-step counts, length/rotate state, pattern settings, MIDI channels, note overrides, and morph amounts;
- deferred performance message classification, caching, replay, and staging helpers;
- voice-cache clear, unhold, uncache, release, and finalize helpers.

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c` still owns the receive-byte state machine and receive dispatch, but the parser-local bridge code is gone. The file now reads as a receive module in place instead of a mixed receive/session owner.

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.h` was updated so the receive-owned state that the bridge still reads is declared explicitly in the header instead of being hidden in a large parser-local block.

### MIDI Parser Ownership Split

`mainboard/LxrStm32/src/MIDI/MidiParser.c` was reduced to the top-level MIDI stream coordinator and routing shim.

The parser now:

- keeps the raw UART stream parser and running-status state machine;
- routes system messages to `globalMidiParser_handleSystemMessage()`;
- routes note-on and note-off handling through `channelMidiParser_noteOn()` and `channelMidiParser_noteOff()`;
- fans CC handling out through `channelMidiParser_MIDIccHandler()` and `globalMidiParser_MIDIccHandler()`;
- keeps routing/filter decisions in the top-level parser, but not the channel/global case ladders themselves.

`mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c/.h` now owns:

- MIDI clock, start, continue, stop, and MTC start/stop handling;
- the MTC watchdog that stops the sequencer if timecode goes stale;
- the global-channel CC ladder and its sequencer-side effects;
- the global run-stop echo path.

`mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c/.h` now owns:

- per-voice note-on and note-off routing;
- the per-voice CC ladder for the channel-scoped voices;
- the visible front-panel feedback helpers for bank change, parameter echo, patch reset, and voice activity;
- the voice-level bookkeeping that keeps note routing, override handling, and echo behavior aligned with the existing firmware rules.

The important MIDI behavior was preserved:

- the low/high parameter echo split stayed intact;
- the global channel still owns the global bank/morph/reset side effects;
- voice-specific handling still routes through the same per-voice modulation and DSP side effects as before;
- the MIDI stream and note routing logic still respects the existing routing and filter gates.

`mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c` now delegates the front-panel LED pulse to the channel helper instead of building the byte triplet inline.

### Transport and Documentation Cleanup

`mainboard/LxrStm32/src/uARTFrontSYX/Uart.c/.h` were updated so the transport surface reads as raw FIFO and IRQ plumbing rather than protocol ownership.

`mainboard/LxrStm32/src/uARTFrontSYX/FrontPanelProtocol.h` and the public headers under `uARTFrontSYX/` and `MIDI/` were updated with ownership comments so the new split is visible in the source itself.

The code comments now describe:

- which file owns the transport boundary;
- which file owns the send helpers;
- which file owns the receive parser;
- which file owns the MIDI channel and global ladders;
- which module owns the live voice state used by the echo paths.

## Verification

- `make -C mainboard/LxrStm32 -j4 stm32` completed successfully after the split work.
- The generated firmware image was refreshed as part of the successful build.
- The user tested the important front-panel paths: menu parameter change, file load, copy-to-temp, and temp switch.
- The user also tested the MIDI-risk paths: MIDI clock sync, global CC1, voice CC1, and voice note trigger on the voice channel.
- No visible regressions were reported from those checks.

## Files Changed

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`
  - Added the outbound packet helpers for restore, runtime feedback, response packets, and MIDI-facing front-panel echoes.

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h`
  - Declared the new helper surface and documented the ownership split.

- `mainboard/LxrStm32/src/Preset/EndpointRestore.c`
  - Switched restore packet emission to the new send helpers.

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
  - Switched restore, run-stop, LED, step-info, and pattern-change packets to the new send helpers.

- `mainboard/LxrStm32/src/Preset/PresetLoadCache.c`
  - Added the transitional load/session cache module and moved the parser-local bridge helpers into it.

- `mainboard/LxrStm32/src/Preset/PresetLoadCache.h`
  - Declared the bridge state and helper surface that the receive parser still consults.

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`
  - Dropped the large parser-local load/session bridge block and kept the receive dispatcher plus its bridge calls.

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.h`
  - Declared the receive-owned state explicitly and exposed the receive entry points.

- `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c`
  - Added the channel-owned note and CC split, along with the front-panel echo helpers for that path.

- `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.h`
  - Declared the channel parser ownership surface.

- `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c`
  - Added the global system-message handler, MTC watchdog, and global-channel CC split.

- `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.h`
  - Declared the global parser ownership surface.

- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
  - Reduced the parser to the stream coordinator and routing shim.

- `mainboard/LxrStm32/src/MIDI/MidiParser.h`
  - Added ownership comments for the parser entry points and shared state.

- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c`
  - Delegated the voice LED pulse to the channel helper.

- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.h`
  - Added ownership comments for the live voice state and helper entry points.

- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.c`
  - Clarified the raw transport responsibilities and kept the low-level UART send paths there.

- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.h`
  - Clarified the transport-only API surface.

- `mainboard/LxrStm32/src/uARTFrontSYX/FrontPanelProtocol.h`
  - Added the opcode-namespace ownership comment.

- `firmware image/FIRMWARE.BIN`
  - Refreshed by the successful STM32 build.

## End of session block

```
DATE: 2026-06-14
SESSION GOAL: Continue the comms and MIDI split work by extracting the transitional front-panel receive bridge, consolidating outbound front-panel send helpers, and moving the MIDI parser ownership boundaries into channel and global modules without changing the observed firmware behavior.
COMPLETED: The front-panel send helpers were consolidated, the MIDI parser was split into channel/global ownership files, and the transitional front-panel load/session bridge moved into PresetLoadCache. The user re-tested the key front-panel and MIDI paths and did not report regressions.
VERIFIED ON HARDWARE: Yes. The user tested menu parameter change, file load, copy-to-temp, temp switch, MIDI clock sync, global CC1, voice CC1, and voice note trigger.

CHANGES THIS SESSION:
- mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h: expanded the outbound front-panel helper surface for restore, runtime feedback, responses, and MIDI-facing echoes.
- mainboard/LxrStm32/src/Preset/EndpointRestore.c and mainboard/LxrStm32/src/Sequencer/sequencer.c: rewired restore and runtime front-panel traffic to the send module.
- mainboard/LxrStm32/src/Preset/PresetLoadCache.c/.h: added the transitional load/session bridge module and moved the parser-local bridge state and helpers into it.
- mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c/.h: kept the receive parser in place while removing the large parser-local bridge block and declaring the receive-owned state explicitly.
- mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c/.h and mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c/.h: split the channel/global MIDI ownership and helper surface out of MidiParser.c.
- mainboard/LxrStm32/src/MIDI/MidiParser.c/.h and mainboard/LxrStm32/src/MIDI/MidiVoiceControl.c/.h: updated the parser and voice-control layers to route through the new ownership files and helpers.
- mainboard/LxrStm32/src/uARTFrontSYX/Uart.c/.h and mainboard/LxrStm32/src/uARTFrontSYX/FrontPanelProtocol.h: clarified transport and opcode ownership with comments.
- firmware image/FIRMWARE.BIN: refreshed by the successful STM32 build.

KNOWN ISSUES INTRODUCED: No new functional regressions were reported. The build still emits existing fallthrough warnings in `PresetLoadCache.c` and `MidiParser.c`.
KNOWN ISSUES RESOLVED: The parser-local front-panel load/session bridge was extracted into `PresetLoadCache`, the outbound front-panel packet builders were consolidated, and the MIDI channel/global ownership split is now visible in the code.

NEXT SESSION RECOMMENDED GOAL: Not recorded.
BLOCKERS: None.

CRITICAL REMINDERS FOR NEXT SESSION:
- Not recorded.
```
