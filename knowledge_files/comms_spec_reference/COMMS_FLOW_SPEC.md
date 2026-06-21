# COMMS FLOW SPEC - UART FRONT PANEL

Date: 2026-06-21
Status: current AVR<->STM comms reference after Session 028 completed background file loading through temporary Pattern/Preset storage. STM and AVR both have explicit receive/send protocol files, legacy parser/protocol shim headers were removed, the obsolete `PresetLoadCache` model is gone, the internal CC/CC2 parameter apply layer belongs to front-panel receive/protocol ownership rather than `MIDI/MidiParser.c`, the old cache-only opcode helpers are commented out instead of active, `MACRO_CC` is deprecated historical context, individual PERF voice morph uses dedicated full-range `VOICE_MORPH` / `FRONT_SEQ_VOICE_MORPH` traffic rather than generic `CC_2`, step automation destinations are stored as raw AVR/menu `PAR_*` ids, and active background loading now uses the `SEQ_BACKGROUND_SWAP_BEGIN` / `SEQ_BACKGROUND_SWAP_DONE` handshake (`0x6d/0x6e`) rather than the retired cache/load-fast model.

## Purpose

This document records the current and future shape of the UART link between the ATmega front panel and the STM32 audio engine.

Naming note: STM-side front-panel ownership stays under `mainboard/LxrStm32/src/uARTFrontSYX/` and keeps the `frontPanel*` names. AVR-side comms now live under `front/LxrAvr/avrComms/` and use the `avrComms*` names. Any AVR-side `frontPanel*` reference in older material is historical.

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
  the STM front-panel opcode namespace. It also owns internal CC/CC2-shaped
  parameter application through `frontParser_applyParameterCommand()`.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h`
  owns STM-to-AVR packet construction.

AVR protocol ownership mirrors this:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c/.h` owns STM response
  parsing, SysEx receive state, restore handling, long-operation receive
  state, and the AVR-side opcode namespace.
- `front/LxrAvr/avrComms/avrCommsSendingProtocol.c/.h` owns AVR-to-STM packet
  construction, send-side flow-control state, LED/query sends, legacy macro
  sends kept only as disabled compatibility context, and the now-commented-out
  PRF cache control compatibility stubs.

The old STM `FrontPanelProtocol.h`, STM `frontPanelParser.h`, and AVR
`frontPanelParser.h` shim headers were removed in the Session 020 wrap-up. The
AVR rename to `avrComms*` landed in Session 021.
Callers should include the receive or sending protocol header directly.

### Storage and Session Semantics

`Preset` owns:

- endpoint storage images;
- temp versus normal selection;
- endpoint restore policy;
- normal/temp image selection and routing.

`Pattern` owns pattern storage and pattern-specific temp behavior.

The comms layer should route bytes into those owners, not duplicate their state.

### MIDI Boundary

`mainboard/LxrStm32/src/MIDI/` owns external DIN/USB MIDI byte parsing, MIDI
routing/filter settings, channel/global MIDI interpretation, voice triggering,
and sequencer-originated MIDI output fan-out.

Internal parameter application is not owned by `MidiParser.c`. External MIDI may
parse a CC/CC2 message and then call the front-panel receive/protocol helper
when it needs to apply the internal parameter command cases. The dependency
direction is MIDI -> uARTFrontSYX for shared internal CC/CC2 application, not
uARTFrontSYX -> MIDI parser.

`mainboard/LxrStm32/src/MIDI/MidiOutputControl.c/.h` is the current MIDI
voice/output-control file. Existing `voiceControl_*` functions retain their
names; sequencer-originated MIDI output helpers use the `outputControl_*`
prefix. `Sequencer` must not own USB/DIN fan-out directly.

### Outbound Send Contract

The STM send-side split has landed in `frontPanelSendingProtocol.c/.h`.
The AVR send-side split landed in `avrCommsSendingProtocol.c/.h`.
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
`Uart.c/.h` and `avrCommsSendingProtocol.c`. Callers such as Sequencer,
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
- `VOICE_MORPH` / `FRONT_SEQ_VOICE_MORPH` - full-range `0..255` per-voice morph amount traffic, encoded as low/high 7-bit-safe packet pairs
- `MACRO_CC` - deprecated legacy macro traffic; current firmware ignores it

Raw endpoint bytes are routed into `Preset` ingress helpers such as:

- `preset_storeParameterIngress()`
- `preset_storeMorphParameterIngress()`
- `preset_storeLfoDestinationIngress()`
- `preset_storeVelocityDestinationIngress()`
- `preset_storeMacroDestinationIngress()` - legacy inert compatibility stub

Important rule:

- Raw endpoint storage uses raw AVR/menu parameter indices.
- The low-CC `+1` conversion only applies when the value is being applied as an ordinary live CC to DSP/front-panel parameter apply logic.
- Do not apply that conversion to endpoint restore traffic.
- Step automation destination storage also uses raw AVR/menu parameter indices. Front-panel live recording and manual step-destination editing write raw ids to `Step.param1Nr` / `Step.param2Nr`; automation playback converts raw low destinations to `MIDI_CC data1 = destination + 1` immediately before calling `frontParser_applyParameterCommand()`.
- External DIN/USB MIDI automation recording starts from the MIDI-domain/apply-domain id. Those call sites must use `seq_recordAutomationMidiDestination()` so low MIDI-domain destinations are converted back to raw step storage.
- STM-to-AVR step-parameter replies send raw stored destinations unchanged. This is different from `frontPanelSending_sendParameterEcho()`, which still subtracts one for low parameters because its MIDI parser callers pass apply-domain ids.
- PERF individual voice morph amount edits are not ordinary `CC_2` parameter ingress. They use `VOICE_MORPH` low/high packets and land in the direct full-range Preset voice morph setter.
- MIDI CC1 morph remains a 7-bit input path; its resulting full-range global/voice morph amount may be reported back to AVR for display sync.

Per-voice morph packet shape:

```text
status = VOICE_MORPH / FRONT_SEQ_VOICE_MORPH
data1  = 0..5   for low 7 bits of Drum1, Drum2, Drum3, Snare, Cym, Hihat
data1  = 6..11  for high bit of the same six voices
data2  = 7-bit payload
```

The receiver caches the low packet and commits the full `0..255` value on the high packet.

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
- `VOICE_MORPH` / `FRONT_SEQ_VOICE_MORPH` per-voice morph reports

The boundary restore path is still needed so the AVR menu stays coherent when the audible image changes.

The global morph and per-voice morph reports are display-only:

- it updates the AVR view of the selected kit image;
- global morph reports also synchronize all six individual voice morph display slots because global morph overrides the per-voice current values;
- individual voice morph reports update only the displayed per-voice amount;
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
- `SEQ_LOAD_BACKGROUND` - legacy/background-setting byte at `0x50`, currently
  still sent when the AVR global menu value changes and still received on STM
  as `FRONT_SEQ_LOAD_FAST`; this is not the active background-swap/load
  mechanism
- `SEQ_BACKGROUND_SWAP_BEGIN` / `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` (`0x6d`)
- `SEQ_BACKGROUND_SWAP_DONE` / `FRONT_SEQ_BACKGROUND_SWAP_DONE` (`0x6e`)

On the AVR side, the user-facing control is the 5-state background-load
selector `PAR_FILE_LOAD_BACKGROUND` on the load page:

| Value | Label | File types that may request background swap |
|-------|-------|---------------------------------------------|
| `0` | `off` | none |
| `1` | `pat` | `.pat` / `WTYPE_PATTERN` |
| `2` | `prf` | `.prf` / `WTYPE_PERFORMANCE` |
| `3` | `all` | `.all` / `WTYPE_ALL` |
| `4` | `tot` | `.pat`, `.prf`, and `.all` |

That selector still persists as one raw byte in globals / `.cfg` storage. The
old `SEQ_LOAD_BACKGROUND` / `FRONT_SEQ_LOAD_FAST` setting-byte path at `0x50`
is still present in current code, but the completed background-loading feature
does not depend on it. The active background-load prelude is the `0x6d/0x6e`
swap handshake.

The old `PresetLoadCache` and `presetLoad_*` cache API were removed in Session
020. File-load receive paths now route bytes directly to normal Preset/Pattern
storage. `avrCommsReceivingProtocol.c` only keeps a tiny file-load ingress
bracket to ensure file bytes land in normal-kit endpoint mode during the old
AVR file-transfer envelope; it is not a cache or staging owner.

Current active background-load prelude:

```text
AVR file loader validates the selected file header
-> preset_backgroundSwapNeeded(fileType) checks sequencer-running state,
   PAR_FILE_LOAD_BACKGROUND, temp-playback guard, and SEQ_TMP_PATTERN guard
-> AVR displays "Bckgrnd Swap..."
-> AVR sends SEQ_CC / SEQ_BACKGROUND_SWAP_BEGIN / fileType
-> STM handles FRONT_SEQ_BACKGROUND_SWAP_BEGIN:
   - pat_copyToTmpPattern(seq_activePattern)
   - seq_setNextPattern(SEQ_TMP_PATTERN, 0x0f)
   - preset_tempPlaybackSwitchState.forceInstantSwitch = 1
   - preset_tempPlaybackSwitchState.patternOnlyTempPlayback = fileType is pattern
   - arm frontParser_backgroundSwapPending
-> frontParser_serviceBackgroundSwapAck() runs from the STM main loop
-> after temp playback readiness plus 400 ticks, STM sends
   FRONT_SEQ_BACKGROUND_SWAP_DONE / fileType as priority traffic
-> AVR accepts SEQ_BACKGROUND_SWAP_DONE even while avrCommsParser_rxDisable is true
-> AVR continues the ordinary SEQ_FILE_BEGIN / file payload / SEQ_FILE_DONE load
```

Readiness for the STM ACK is code-defined:

- `seq_activePattern == SEQ_TMP_PATTERN`;
- every `seq_perTrackActivePattern[track] == SEQ_TMP_PATTERN`;
- for `.pat`, `preset_allVoiceSourcesUseNormal()` must be true;
- for `.prf` / `.all`, `preset_allVoiceSourcesUseTmp()` must be true.

File-load traffic follows this broad rule:

- `.pat`, `.prf`, and `.all` loads refresh normal storage images while temp
  playback can keep running.
- file loads must not create a second sound-authority cache.
- file-load-while-temp-active behavior uses the existing normal/temp
  copy/playback model, not a revived load cache.
- when AVR already knows temp playback is active
  (`preset_backgroundTempPlaybackActive`), another `.pat`, `.prf`, or `.all`
  load skips `SEQ_BACKGROUND_SWAP_BEGIN` and proceeds as an ordinary load into
  normal storage.
- the temp-playback flag clears only when STM reports a non-`SEQ_TMP_PATTERN`
  played pattern through `SEQ_CHANGE_PAT`.

Morph rule:

- file loads must not reset or resend current global morph or current per-voice
  morph amounts;
- `.pat` load begin has no morph side effect;
- `.prf` / `.all` load begin may invalidate the live morph apply cache so newly
  loaded endpoints are recomputed from the existing current morph amounts, but
  it must not mutate those morph amounts;
- AVR file-backed kit reads/writes/dumps use `END_OF_KIT_PARAMETERS`, not
  `END_OF_SOUND_PARAMETERS`, so `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` stay live
  performance/display controls rather than saved kit bytes.

### 5. Background Temp-Playback UI Feedback

The AVR owns the visible temp-playback hint because it already tracks
`preset_backgroundTempPlaybackActive`.

Relevant functions:

- `preset_isBackgroundTempPlaybackActive()`
- `buttonHandler_refreshTempPlaybackLedHint()`
- `preset_notePlayedPatternChanged()`

Current UI behavior while temp playback is active:

- outside `SELECT_MODE_PERF`, `LED_MODE2` / PERF flashes while the current mode
  LED remains active;
- inside `SELECT_MODE_PERF`, all eight SELECT LEDs flash;
- `SHIFT+PERF` is ignored so PAT_GEN cannot be entered while temp playback
  remains active;
- `NUM_OF_BLINKABLE_LEDS` is currently `8` to support all SELECT LEDs blinking.

### 6. Callback and Hold Primitives

Legacy wait primitives still exist:

- `CALLBACK_ACK`
- `avrComms_holdForBuffer()`

These are still narrow compatibility tools.
They should not become a general mechanism for future protocol growth.

## Historical AVR-Side Initiators That Fed The Load Path

The AVR side does not own the canonical session state. The names below are kept as historical reference only because Session 024 commented out the thin cache helper surface that used to feed the old background-load machinery.

### Front-panel protocol initiators

In `front/LxrAvr/avrComms/avrCommsSendingProtocol.c`:

- `avrComms_prfCacheBegin()` (commented out in Session 024)
- `avrComms_prfCacheControl()` (commented out in Session 024)
- `avrComms_flowBeginSession()`
- `avrComms_flowEndSession()`
- `avrComms_flowAbortSession()`
- `PRF_CACHE_STATUS` handling (commented out in Session 024)

These were compatibility entry points for the old file-transfer envelope.
Session 024 commented out the obsolete cache behavior; do not treat these
commands as a live background-load cache regime.

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
- `preset_backgroundSwapNeeded()`
- `preset_performBackgroundSwapWait()`
- `preset_backgroundSwapDoneFromStm()`
- `preset_notePlayedPatternChanged()`
- `preset_isBackgroundTempPlaybackActive()`

These are the file-load and menu-preservation flows that start the transport/session traffic.

They are the right place to keep the user-facing initiation logic.
They are not the right place to own STM storage/session semantics. They do own
the AVR policy decision about whether the current file type should request the
STM background-swap prelude.

### STM background-swap service points

In `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`:

- `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` is the receive-side entry point.
- `frontParser_serviceBackgroundSwapAck()` is the non-blocking main-loop
  service that waits for readiness and sends `FRONT_SEQ_BACKGROUND_SWAP_DONE`.
- `frontParser_backgroundSwapTempPlaybackReady()` defines when the ACK may arm
  its final delay.

In `mainboard/LxrStm32/src/main.c`:

- `frontParser_serviceBackgroundSwapAck()` is called after
  `uart_processFront()` each main-loop pass.

## Send-Side Completion State

The send-side migration is complete for the current audit scope:

- STM front-panel output goes through
  `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h`.
- AVR front-panel output goes through
  `front/LxrAvr/avrComms/avrCommsSendingProtocol.c/.h`.
- Raw UART files remain byte transport only.
- MIDI echo paths use the STM sending protocol layer for display-facing
  feedback and do not parse AVR command semantics.

## The `.pat` Rule

Pattern-only background loading is the important wrinkle and is now active code.

`.pat` background loading should never switch preset parameter read/write away from normal storage.

That means:

- pattern data can be staged in pattern-owned temp structures;
- parameter ingress must remain on normal storage for `.pat` background loads;
- pattern temp/normal ownership and parameter temp/normal ownership must remain separate;
- the implementation uses `preset_tempPlaybackSwitchState.patternOnlyTempPlayback`
  to distinguish pattern-only background loads from parameter-bearing file loads.

This rule is easy to miss if the pattern loader and parameter loader share one broad session object.
That is exactly why the interface keeps the ownership boundaries explicit.

## Current Shape After The Audit

The comms layer now has this split:

- `Uart.c/.h` for byte transport only
- `avrCommsReceivingProtocol.c/.h` for parsing and receive-side dispatch
- `avrCommsSendingProtocol.c/.h` for outbound framing and status messages
- receive protocol headers for the opcode namespaces
- `Preset` for storage and session semantics
- `Pattern` for pattern storage and pattern temp behavior

The parser must not keep a second background-load model. It routes semantics
into `Preset` and `Pattern`, then gets out of the way.

The old PRF/cache initiator names are now commented out rather than active.
Keep their history in the log archive rather than reintroducing them in place,
and do not bring back a cache-as-authority model under the same names.

## Guardrails

- Do not broaden blocking waits just because the comms layer is now better organized.
- Do not let display-only restore traffic feed back into live DSP state.
- Do not let transport code inspect `SeqKitState` directly.
- Do not create a second load/session model beside the temp/normal storage model.
- Do not merge pattern-only background loading with parameter storage switching.
- Do not let send/receive protocol work blur the ownership boundary between
  parsing bytes and deciding what they mean.
- Do not treat `SEQ_LOAD_BACKGROUND` / `FRONT_SEQ_LOAD_FAST` at `0x50` as the
  active background-load mechanism; active background swap is `0x6d/0x6e`.
- Do not reset current global/per-voice morph amounts from any file-load begin
  or done message.
- Do not send another `SEQ_BACKGROUND_SWAP_BEGIN` while AVR temp playback state
  is already active.
