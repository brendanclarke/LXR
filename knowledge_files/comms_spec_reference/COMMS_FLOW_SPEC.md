# COMMS FLOW SPEC - UART FRONT PANEL

Date: 2026-06-14
Status: current AVR<->STM comms reference after Session 020 finalization. STM and AVR now both have explicit receive/send protocol files, legacy parser/protocol shim headers were removed in the Session 020 wrap-up, and the obsolete `PresetLoadCache` model is gone.

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

STM protocol ownership is split explicitly:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c/.h`
  owns AVR UART receive parsing, SysEx ingress, load-session receive state, and
  the STM front-panel opcode namespace.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h`
  owns STM-to-AVR packet construction.

AVR protocol ownership mirrors this:

- `front/LxrAvr/frontPanelReceivingProtocol.c/.h` owns STM response parsing,
  SysEx receive state, restore handling, long-operation receive state, and the
  AVR-side opcode namespace.
- `front/LxrAvr/frontPanelSendingProtocol.c/.h` owns AVR-to-STM packet
  construction, send-side flow-control state, LED/query sends, macro sends,
  and deprecated PRF cache control compatibility sends.

The old STM `FrontPanelProtocol.h`, STM `frontPanelParser.h`, and AVR
`frontPanelParser.h` shim headers were removed in the Session 020 wrap-up.
Callers should include the receive or sending protocol header directly.

### Storage and Session Semantics

`Preset` owns:

- endpoint storage images;
- temp versus normal selection;
- endpoint restore policy;
- normal/temp image selection and routing.

`Pattern` owns pattern storage and pattern-specific temp behavior.

The comms layer should route bytes into those owners, not duplicate their state.

### Outbound Send Contract

The STM send-side split has landed in `frontPanelSendingProtocol.c/.h`.
That module owns outbound AVR command framing so the rest of the STM code can
reuse packet helpers instead of rebuilding byte triples inline.

The reusable helper surface should cover:

- ordinary command/status byte triples;
- priority command/status byte triples for flow-control and restore traffic;
- wait-for-space variants for restore sequencing and any other blocking packet
  family;
- SysEx-aware packet emission for front-panel streams that stay open across
  multiple bytes;
- common packet families such as restore begin/done, restore param, restore
  morph param, flow grant/abort, pattern change, run-stop, LED updates, and
  query replies.

The send header should also document the transmit mode bits or flags clearly so
callers can tell whether a packet is:

- normal or priority;
- blocking or non-blocking;
- SysEx-safe or ordinary command traffic;
- allowed to bypass the quiet UI gate or not.

Direct front-panel UART byte emission for AVR command traffic is confined to
`Uart.c/.h` and `frontPanelSendingProtocol.c`. Callers such as Sequencer,
EndpointRestore, MIDI parser helpers, voice control, and receive protocol code
use the sending protocol helpers.

`Uart.c/.h` should remain transport-only. The MIDI transport helpers
`uart_sendMidi()` and `uart_sendMidiByte()` stay outside this split because
they are not AVR front-panel command traffic.

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

The old `PresetLoadCache` and `presetLoad_*` cache API were removed in Session
020. File-load receive paths now route bytes directly to normal Preset/Pattern
storage. `frontPanelReceivingProtocol.c` only keeps a tiny file-load ingress
bracket to ensure file bytes land in normal-kit endpoint mode during the old
AVR file-transfer envelope; it is not a cache or staging owner.

File-load traffic should follow this broad rule:

- `.prf` / `.all` style loads refresh normal storage images while temp
  playback can keep running.
- file loads must not create a second sound-authority cache.
- future file-load-while-temp-active behavior should use the existing
  normal/temp copy/playback model, not a revived load cache.

### 5. Callback and Hold Primitives

Legacy wait primitives still exist:

- `CALLBACK_ACK`
- `frontPanel_holdForBuffer()`

These are still narrow compatibility tools.
They should not become a general mechanism for future protocol growth.

## AVR-Side Initiators That Feed The Load Path

The AVR side does not own the canonical session state, but it does initiate the flows that currently feed the background-load machinery.

### Front-panel protocol initiators

In `front/LxrAvr/frontPanelSendingProtocol.c`:

- `frontPanel_prfCacheBegin()`
- `frontPanel_prfCacheControl()`
- `frontPanel_flowBeginSession()`
- `frontPanel_flowEndSession()`
- `frontPanel_flowAbortSession()`
- `PRF_CACHE_STATUS` handling

These are compatibility entry points for the existing file-transfer envelope.
STM rejects/no-ops the obsolete cache behavior; do not treat these commands as
a live background-load cache regime.

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

## Send-Side Completion State

The send-side migration is complete for the current audit scope:

- STM front-panel output goes through
  `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h`.
- AVR front-panel output goes through
  `front/LxrAvr/frontPanelSendingProtocol.c/.h`.
- Raw UART files remain byte transport only.
- MIDI echo paths use the STM sending protocol layer for display-facing
  feedback and do not parse AVR command semantics.

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

The comms layer now has this split:

- `Uart.c/.h` for byte transport only
- `frontPanelReceivingProtocol.c/.h` for parsing and receive-side dispatch
- `frontPanelSendingProtocol.c/.h` for outbound framing and status messages
- receive protocol headers for the opcode namespaces
- `Preset` for storage and session semantics
- `Pattern` for pattern storage and pattern temp behavior

The parser must not keep a second background-load model. It should route
semantics into `Preset` and `Pattern`, then get out of the way.

Deprecated PRF/cache initiators can stay thin or stubbed while old AVR
file-transfer code still emits compatibility traffic. That is acceptable only
as long as they do not reintroduce a cache-as-authority model.

## Guardrails

- Do not broaden blocking waits just because the comms layer is now better organized.
- Do not let display-only restore traffic feed back into live DSP state.
- Do not let transport code inspect `SeqKitState` directly.
- Do not create a second load/session model beside the temp/normal storage model.
- Do not merge pattern-only background loading with parameter storage switching.
- Do not let send/receive protocol work blur the ownership boundary between
  parsing bytes and deciding what they mean.
