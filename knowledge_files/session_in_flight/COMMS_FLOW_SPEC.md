# COMMS FLOW SPEC - UART FRONT PANEL

Date: 2026-06-13
Status: current AVR<->STM comms reference; Session 014 already moved the Sequencer/MIDI runtime callers off the load cache, and Phase 9 will split the send/receive protocol files.

## Purpose

This document records the current and future shape of the UART link between the ATmega front panel and the STM32 audio engine.

It answers four questions:

- What kinds of messages cross the link?
- Which side owns each message family?
- Which parts are transport, protocol, and storage/session semantics?
- What should the comms layer look like once the preset consolidation is finished?

This is the working comms map for the audit pass. Transport details, protocol parsing, and preset storage should stay separate even when they interact.

## Ownership Boundaries

### Transport

`mainboard/LxrStm32/src/uARTFrontSYX/Uart.c/.h` should stay transport-only.

Its job is to move bytes, manage FIFOs, and service IRQ plumbing.
It should not know about normal/temp storage, background-load policy, or preset image ownership.

### Protocol

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c/.h` currently parses both directions of front-panel traffic.

The future shape should split that responsibility into:

- `frontPanelReceivingProtocol.c/.h`
- `frontPanelSendingProtocol.c/.h`

`FrontPanelProtocol.h` owns the opcode namespace.
`MidiMessages.h` should remain a compatibility include only.

### Storage and Session Semantics

`Preset` owns:

- endpoint storage images;
- temp versus normal selection;
- endpoint restore policy;
- background-load finalization;
- any session state that describes where bytes should land.

`Pattern` owns pattern storage and pattern-specific temp behavior.

The comms layer should route bytes into those owners, not duplicate their state.

## Current Message Families

### 1. Live Control and Parameter Ingress

These are the ordinary single-message control paths:

- `MIDI_CC`
- `CC_2`
- `SEQ_CC`
- `CC_LFO_TARGET`
- `CC_VELO_TARGET`
- `MACRO_CC`

Raw endpoint bytes are routed into `Preset` ingress helpers such as:

- `preset_storeParameterIngress()`
- `preset_storeMorphParameterIngress()`
- `preset_storeLfoDestinationIngress()`
- `preset_storeVelocityDestinationIngress()`
- `preset_storeMacroDestinationIngress()`

Important rule:

- Raw endpoint storage uses raw AVR/menu parameter indices.
- The low-CC `+1` conversion only applies when the value is being applied as an ordinary live MIDI CC to DSP or parser-facing live apply logic.
- Do not apply that conversion to endpoint restore traffic.

### 2. Endpoint Restore and Display Sync

These messages push authoritative preset bytes back to the AVR so the menu matches the selected image:

- `SEQ_TMP_KIT_ENDPOINT_BEGIN`
- `SEQ_TMP_KIT_ENDPOINT_END`
- `PRF_RESTORE_PARAM_CC`
- `PRF_RESTORE_PARAM_CC2`
- `PRF_RESTORE_MORPH_CC`
- `PRF_RESTORE_MORPH_CC2`
- `SEQ_TMP_KIT_AUTOMATION_PHASE`
- `SEQ_REPORT_GLOBAL_MORPH_LSB`
- `SEQ_REPORT_GLOBAL_MORPH_MSB`

The boundary restore path is still needed so the AVR menu stays coherent when the audible image changes.

The global morph report is display-only:

- it updates the AVR view of the selected kit image;
- it must not feed back into DSP state as if it were a new file-load payload.

### 3. Flow-Control Session Mode

These messages meter high-volume transfers:

- `SEQ_FLOW_BEGIN`
- `SEQ_FLOW_GRANT`
- `SEQ_FLOW_END`
- `SEQ_FLOW_ABORT`

Known channels:

- `FLOW_CH_LOAD_SESSION`
- `FLOW_CH_GLOBALS`
- `FLOW_CH_VOICE_PARAM`
- `FLOW_CH_DRUM_META`

This layer exists to prevent bursts from overwhelming the link.
It is not a replacement for preset/session ownership.

### 4. File-Load Semantics

These messages tell STM that the incoming traffic belongs to a file load or background load:

- `SEQ_FILE_BEGIN`
- `SEQ_FILE_DONE`
- `SEQ_LOAD_VOICE`
- `SEQ_LOAD_FAST`

The current implementation still passes through `PresetLoadCache` for the remaining parser/session bridge, but the Sequencer/MIDI runtime callers have already moved to direct owner reads. The Phase 7 target is to remove that remaining overlap and make the temp/normal storage model carry the load semantics directly.

File-load traffic should follow this broad rule:

- `.prf` / `.all` style loads refresh the normal storage images while temp playback can keep running.
- background loads must not create a second sound-authority cache.
- newer background loads may replace older in-flight ones.

### 5. Callback and Hold Primitives

Legacy wait primitives still exist:

- `CALLBACK_ACK`
- `frontPanel_holdForBuffer()`

These are still narrow compatibility tools.
They should not become a general mechanism for future protocol growth.

## AVR-Side Initiators That Feed The Load Path

The AVR side does not own the canonical session state, but it does initiate the flows that currently feed the background-load machinery.

### Front-panel protocol initiators

In `front/LxrAvr/frontPanelParser.c`:

- `frontPanel_prfCacheBegin()`
- `frontPanel_prfCacheControl()`
- `frontPanel_flowBeginSession()`
- `frontPanel_flowEndSession()`
- `frontPanel_flowAbortSession()`
- `PRF_CACHE_STATUS` handling

These are the AVR-facing entry points that need to be reconnected once the background-load machinery no longer depends on `PresetLoadCache`.

### AVR preset/file-load flows

In `front/LxrAvr/Preset/presetManager.c`:

- `preset_loadDrumset()`
- `preset_loadAll()`
- `preset_loadPerf()`
- `preset_loadPattern()`
- `preset_saveDrumset()`
- `preset_saveAll()`
- `preset_shouldPreserveMenuEndpointsDuringFileLoad()`
- `preset_saveMenuEndpointsDuringFileLoad()`
- `preset_restoreMenuEndpointsDuringFileLoad()`

These are the file-load and menu-preservation flows that start the transport/session traffic.

They are the right place to keep the user-facing initiation logic.
They are not the right place to own the in-flight load-session state.

## The `.pat` Rule

Pattern-only background loading is the important wrinkle.

`.pat` background loading should never switch preset parameter read/write away from normal storage.

That means:

- pattern data can be staged in pattern-owned temp structures;
- parameter ingress must remain on normal storage for `.pat` background loads;
- pattern temp/normal ownership and parameter temp/normal ownership must remain separate;
- the implementation may need an explicit load-kind discriminator or mode bit to distinguish pattern-only background loads from parameter-bearing file loads.

This rule is easy to miss if the pattern loader and parameter loader share one broad session object.
That is exactly why the future interface needs to keep the ownership boundaries explicit.

## Future Shape After The Audit

The comms layer should end up with a very clear split:

- `Uart.c/.h` for byte transport only
- `frontPanelReceivingProtocol.c/.h` for parsing and receive-side dispatch
- `frontPanelSendingProtocol.c/.h` for outbound framing and status messages
- `FrontPanelProtocol.h` for the opcode namespace
- `Preset` for storage and session semantics
- `Pattern` for pattern storage and pattern temp behavior

The parser should not keep a second background-load model after `PresetLoadCache` is removed.
It should route semantics into `Preset` and `Pattern`, then get out of the way.

The background-load initiators can stay thin or stubbed while the replacement path is being connected.
That is acceptable as long as they do not reintroduce a second cache-as-authority model.

## Guardrails

- Do not broaden blocking waits just because the comms layer is now better organized.
- Do not let display-only restore traffic feed back into live DSP state.
- Do not let transport code inspect `SeqKitState` directly.
- Do not create a second load/session model beside the temp/normal storage model.
- Do not merge pattern-only background loading with parameter storage switching.
- Do not let the future send/receive split blur the ownership boundary between parsing bytes and deciding what they mean.
