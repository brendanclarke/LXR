# FLOW_CONTROL_SECOND_AUDIT

Date: 2026-05-26

## Purpose

This is a fresh implementation plan for software flow control on the ATmega644 <-> STM32F4 front-panel UART link.

The immediate user-visible goal is narrow:

- loading `.PRF` and `.ALL` files must not lock the drum machine while the sequencer is running;
- required ACK/callback traffic must not be lost behind optional LED/status chatter;
- the implementation must be reviewable and testable in small hardware steps.

This is not a checksum/framing rewrite and it is not a baud-rate experiment.

## Checkpoint Status 2026-05-27

Phases 1-6 have been implemented and hardware-tested incrementally.

Current checkpoint summary:

- AVR 3-byte sends no longer wait for TX FIFO space inside an atomic block.
- `.ALL` and `.PRF` loads are wrapped in acknowledged load sessions.
- STM32 quiet mode suppresses optional UI/status traffic during load while preserving priority callbacks/flow ACKs.
- globals, voice parameter bursts, and drum/meta bursts are credit-metered.
- existing SysEx item callbacks remain intact and are protected by quiet mode rather than normal-message grants.
- `.ALL` file-done now commits pending voice/LFO/velocity cache state deterministically.
- `.PRF` while playing now intentionally defers STM32-side kit, morph, metadata, and the captured active pattern until manual pattern change or stop.
- the internal STM32 flag for that behavior is `frontParser_deferPerfLoadCacheUntilPatternChange`.

Detailed continuation notes are in `CHECKPOINT_FLOW_CONTROL_SECOND_AUDIT.md`.

## Initial Source Facts Rechecked

These were the source facts rechecked before implementation. The checkpoint section above records which items have now been mitigated.

Initial source state:

- STM32 USART3 front RX is still serviced one byte per `uart_processFront()` call in `mainboard/LxrStm32/src/MIDI/Uart.c`.
- STM32 front TX/RX FIFOs and AVR TX/RX FIFOs are 256 bytes.
- FIFO enqueue failures are still ignored in critical interrupt/send paths.
- AVR `frontParser_rxDisable=1` during kit/pattern/all/perf load suppresses normal inbound data-byte handling.
- Existing SysEx-like writers depend on per-item callbacks sent as SysEx bytes, not normal 3-byte messages.
- `preset_readPatternChain()` sends `next[i]`, waits for `PATCHAIN_CALLBACK`, sends `repeat[i]`, waits again.
- Therefore the STM32 `SYSEX_RECEIVE_PAT_CHAIN_DATA` callback after every received byte is intentional for the current AVR sender. Do not move it to "after both bytes" unless the AVR sender is changed at the same time.

Failed-attempt lessons to keep:

- Do not bundle transport changes, preset error rewrites, parameter-map fixes, and diagnostics into one patch.
- Do not reintroduce the unbounded STM32 full-drain change as a first move.
- Do not trust "obvious" ACK placement changes without tracing both sides of the exact handshake.

## Initial Verified Stop-Responding Risk Map

This section was the second-pass check against the pre-implementation code. These were the places where a `.PRF` or `.ALL` load while the sequencer is playing could make the front panel appear stuck or leave the STM32 in a bad communication state. The checkpoint section above records the mitigations already implemented through Phase 6.

### 1. AVR TX deadlock inside `frontPanel_sendData()`

Location:

- `front/LxrAvr/frontPanelParser.c`, `frontPanel_sendData()`

Current behavior:

- disables interrupts with `ATOMIC_BLOCK`;
- spins on `uart_putc()` until all three bytes fit in the TX FIFO;
- but the TX FIFO drains from the USART UDRE interrupt, which is masked inside the atomic block.

Failure mode:

- if the TX FIFO is full at entry, the AVR can hard-deadlock immediately.

Load points exposed:

- `menu_sendAllGlobals()` during `.ALL` / `.PRF`;
- `preset_readDrumVoice()` and `preset_morph()` during kit/voice application;
- `preset_readDrumsetMeta()`;
- `SEQ_FILE_DONE` at load end.

Playback makes it worse because STM32 is simultaneously producing beat/chase/status traffic, increasing the chance that the front-side RX/TX path is busy while large AVR bursts are sent.

### 2. Unbounded AVR waits for SysEx session open/close and callbacks

Locations:

- `preset_readPatternMainStep()`
- `preset_readPatternLength()`
- `preset_readPatternScale()`
- `preset_readPatternChain()`
- `preset_readPatternStepData()`

Current behavior:

- waits forever for `SYSEX_START`;
- waits forever for per-item callbacks such as `MAINSTEP_CALLBACK`, `LENGTH_CALLBACK`, `SCALE_CALLBACK`, `PATCHAIN_CALLBACK`, `STEP_ACK`, and `STEP_CALLBACK`;
- waits forever for `SYSEX_END`.

Failure mode:

- one lost callback byte leaves the AVR inside a `while(...) { uart_checkAndParse(); }` loop with the LCD still showing the load screen.

Playback makes it worse because STM32 `seq_tick()` sends front-panel LED/status traffic while running. Normal STM32 sends are suppressed during active SysEx, but traffic immediately before/after SysEx sessions can still fill or disturb the shared FIFOs.

### 3. `frontPanel_holdForBuffer()` can wait forever

Locations:

- `front/LxrAvr/frontPanelParser.c`, `frontPanel_holdForBuffer()`
- called from `preset_readDrumVoice()`

Current behavior:

- AVR sends `CALLBACK_ACK`;
- waits forever for STM32 to echo `FRONT_CALLBACK_ACK`;
- STM32 handles it by clearing both STM32 front TX and RX FIFOs before sending the echo.

Failure mode:

- if the echo is lost, AVR hangs;
- if STM32 clears RX while load bytes are already queued but not parsed, those bytes can be discarded;
- if STM32 clears TX while a required callback or future flow grant is queued, that required byte can be discarded.

Implementation consequence:

- no flow-control phase is allowed to put a required grant/end-ack behind a path that can be erased by `uart_clearFrontFifo()`.

### 4. Running sequencer creates optional STM32 -> AVR traffic

Locations:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`

Verified examples:

- pattern-change ACK/status: `FRONT_SEQ_CHANGE_PAT`;
- beat pulse LED on/off;
- current-step LED updates;
- track rotation / transpose echoes;
- query replies for pattern, step, and Euklid parameters;
- voice morph messages from automation.

Failure mode:

- optional traffic shares the same STM32 TX FIFO as required load callbacks;
- FIFO enqueue failure is ignored;
- callback/grant bytes can be lost if optional traffic fills the FIFO first;
- AVR `frontParser_rxDisable=1` during load means most normal reply data is intentionally not applied, but it can still occupy and overflow the AVR RX FIFO before it is discarded.

This is the main reason quiet mode belongs before credit-metering large bursts.

### 5. STM32 RX can still overflow on AVR bursts

Locations:

- STM32 USART3 IRQ enqueues into `fifo_frontRx`;
- `uart_processFront()` currently processes only one byte per main-loop call.

Failure mode:

- unmetered AVR bursts can overrun STM32 front RX;
- dropped bytes can corrupt normal messages or SysEx payloads;
- corrupted SysEx state can prevent the expected callback from ever being sent.

Load points exposed:

- globals burst via `menu_sendAllGlobals()`;
- voice/morph/meta parameter bursts;
- any future attempt to drain STM32 RX in larger bursts without priority/credit handling.

### 6. AVR `uart_clearFifo()` at load close can erase queued end messages

Locations:

- `.ALL`: `preset_loadAll()` queues `SEQ_FILE_DONE`, delays 50 ms, then clears the AVR UART FIFOs;
- `.PRF`: `preset_loadPerf()` queues `SEQ_FILE_DONE`, then reaches `uart_clearFifo()` with no matching explicit TX-drained wait.

Current behavior:

- `uart_clearFifo()` clears both AVR TX and RX FIFOs.

Failure mode:

- an end-of-load message can be erased before it physically leaves the AVR;
- with quiet mode, a lost `FLOW_END` would leave STM32 quiet indefinitely, which looks like the front panel stopped responding after the load.

Implementation consequence:

- load-session end must be acknowledged by STM32 before any AVR FIFO clear;
- implementation should add either a TX-empty wait or separate RX-only clear before using `uart_clearFifo()` near load boundaries.

### 7. Running-state pattern writes can leave STM32 in transient locked/deferred states

Locations:

- `SYSEX_RECEIVE_MAIN_STEP_DATA` sets `seq_tracksLocked` when loading current pattern while running and `seq_loadFastMode` is enabled;
- `SYSEX_BEGIN_PATTERN_TRANSMIT` clears track locks at the end of a 128-step transfer;
- non-fast loads write current-pattern data into `seq_tmpPattern` and rely on later pattern activation.

Failure mode:

- if a transfer hangs between main-step load and step-data completion, tracks can remain locked or temp-pattern state can remain pending;
- this is not the same as the AVR UI wait deadlock, but it can make playback appear broken after a failed load.

Test consequence:

- every playback-active test must check not only "did the LCD return?" but also "does the sequencer still run and do loaded/unloaded voices still trigger?"

### 8. SD/file hard locks are still separate from flow control

Locations:

- many `while(1){;}` error paths in `presetManager.c`.

Failure mode:

- bad/legacy/truncated files can still hard-lock independent of UART flow.

Test consequence:

- transport phases must use known-good modern files first;
- file-layout and short-read recovery are separate follow-up work and must not be mixed into the first flow-control implementation.

## Design Choice

Use software credit flow control plus a quiet-mode session.

Do not use hardware RTS/CTS in this pass. The code does not currently configure it, the board-level pin availability has not been revalidated in this session, and a software scheme can be tested without electrical assumptions.

The design has two layers:

1. **Load-session quiet mode**: the AVR tells the STM32 that a bulk file load is active. STM32 suppresses optional UI/status traffic until the session ends.
2. **Credit-metered normal-message bursts**: for AVR -> STM32 3-byte message bursts, STM32 grants a small number of logical message credits. AVR sends only while it has credits.

Important constraint:

- Do not send normal `SEQ_CC` grant messages inside an active SysEx transfer. The existing SysEx callback parser uses `frontParser_midiMsg.status == SYSEX_START`; injecting a normal status byte during those waits risks disturbing the existing state machine.

So, in phase one:

- use credits for normal 3-byte bursts such as globals, voice parameters, morph/meta parameter sends;
- keep existing SysEx per-item callbacks for pattern data, but protect them with quiet mode.
- make load-session begin/end acknowledged so the test point can prove STM32 entered and exited quiet mode before the next phase starts.

## Protocol Additions

Reserve free `SEQ_CC` subcommands after the existing `SEQ_MIDI_CHAN_OFF` / `FRONT_SEQ_MIDI_CHAN_OFF` value `0x59`:

```c
#define SEQ_FLOW_BEGIN 0x5a
#define SEQ_FLOW_GRANT 0x5b
#define SEQ_FLOW_END   0x5c
#define SEQ_FLOW_ABORT 0x5d
```

Mirror the names on STM32 as `FRONT_SEQ_FLOW_*`.

Suggested channel ids:

```c
#define FLOW_CH_LOAD_SESSION 0x00
#define FLOW_CH_GLOBALS      0x01
#define FLOW_CH_VOICE_PARAM  0x02
#define FLOW_CH_DRUM_META    0x03
```

Keep channel ids at `0..7` so grant encoding fits in one 7-bit MIDI-style data byte.

Message formats:

- Begin: `SEQ_CC, SEQ_FLOW_BEGIN, channel`
- End: `SEQ_CC, SEQ_FLOW_END, channel`
- Abort: `SEQ_CC, SEQ_FLOW_ABORT, channel`
- Grant: `SEQ_CC, SEQ_FLOW_GRANT, ((channel & 0x07) << 4) | (credits & 0x0f)`

Credit count is `1..15`; `0` is reserved and should be ignored or treated as abort-equivalent during testing.

One credit means one complete 3-byte `frontPanel_sendData()` message, not one high-level parameter. This matters because `menu_parseGlobalParam()` can emit more than one 3-byte message for a single global parameter.

For `FLOW_CH_LOAD_SESSION`, use `SEQ_FLOW_GRANT` as an ACK rather than as data credits:

- STM32 replies to `SEQ_FLOW_BEGIN, FLOW_CH_LOAD_SESSION` with `SEQ_FLOW_GRANT` encoded for channel `0`, credits `1`;
- AVR must receive that ACK before setting `frontParser_rxDisable=1` and starting load traffic;
- STM32 replies to `SEQ_FLOW_END, FLOW_CH_LOAD_SESSION` with the same ACK after clearing quiet mode;
- AVR must receive that end ACK before calling any UART clear that can erase TX bytes.

## Parser Requirements

### AVR

Flow-control replies must be parsed even while `frontParser_rxDisable=1`.

Current AVR parser ignores normal data bytes while RX is disabled. That is fine for optional UI chatter, but it would also hide `SEQ_FLOW_GRANT`. Add a priority parser path:

- still suppress ordinary non-SysEx messages during `frontParser_rxDisable`;
- still allow `CALLBACK_ACK`, `SYSEX_START`, `SYSEX_END`, and SysEx callback bytes;
- additionally collect and handle only `SEQ_CC / SEQ_FLOW_*` while RX is disabled.

This is a required part of the flow-control implementation, not an optional cleanup.

### STM32

STM32 handles flow commands in `frontParser_handleSeqCC()`.

On `FLOW_CH_LOAD_SESSION` begin:

- set `comm_loadSessionActive = 1`;
- set `comm_quietUi = 1`;
- clear any stale active credit channel;
- send the session ACK with priority send bytes.

On `FLOW_CH_LOAD_SESSION` end:

- clear `comm_loadSessionActive`;
- clear `comm_quietUi`;
- clear any stale active credit channel;
- send the session-end ACK with priority send bytes.

On a credit channel begin:

- set `comm_flowActive = 1`;
- record `comm_flowChannel`;
- set `comm_flowBudgetRemaining = initialGrant`;
- send the initial `SEQ_FLOW_GRANT`.

After each managed 3-byte message is fully parsed and applied/cached:

- decrement the STM32-side remaining item count;
- when it reaches zero, send the next small grant.

Initial grant recommendation: `4`.

Reason: this caps an unmanaged burst to 12 data bytes plus UART in-flight bytes, which is small compared with the 256-byte FIFO and much safer than the current globals/voice bursts. Raise to `8` only after hardware testing proves stable.

Credit grants must be sent only outside active SysEx sessions. If a credit channel is accidentally active when `frontParser_sysexActive != 0`, abort the credit channel rather than injecting normal `SEQ_CC` bytes into the SysEx callback stream.

## Send-Path Requirements

### AVR send primitive first

Before adding credits, fix the `frontPanel_sendData()` deadlock shape.

Current code spins inside `ATOMIC_BLOCK(ATOMIC_RESTORESTATE)` while waiting for TX FIFO space. If the FIFO is full, the UDRE interrupt that would drain it is masked, so the AVR can deadlock.

Least-risk implementation shape:

1. Add a way to determine AVR TX FIFO free space.
2. Add a way to determine whether the AVR TX FIFO is empty.
3. Add an RX-only clear or otherwise stop using full `uart_clearFifo()` when pending TX bytes must survive.
4. Wait for at least 3 free bytes with interrupts enabled.
5. Use a short atomic block only to enqueue the three bytes contiguously.
6. Do not call `uart_checkAndParse()` from inside the atomic block.

Flow-control waiting depends on this. It should be Phase 1 of the implementation even though it is not the credit protocol itself.

### AVR credit consumption

When a credit channel is active, `frontPanel_sendData()` should consume one credit for every non-flow 3-byte message.

It should not consume credits for:

- `SEQ_FLOW_BEGIN`
- `SEQ_FLOW_GRANT`
- `SEQ_FLOW_END`
- `SEQ_FLOW_ABORT`

When credits are zero:

- wait by calling `uart_checkAndParse()`;
- only proceed after a matching `SEQ_FLOW_GRANT` is parsed;
- use a bounded wait for new flow waits from the start. On timeout, send `SEQ_FLOW_ABORT` and fail the active load path rather than waiting forever.

This lets existing nested send sites such as `menu_parseGlobalParam()`, `preset_morph()`, and `preset_readDrumsetMeta()` become paced without rewriting every individual parameter send at once.

Load-session begin/end waits are also bounded. They are new waits introduced by this plan, so they must not reproduce the old permanent-wait bug.

### STM32 priority and quiet sends

Add a distinction between:

- priority traffic: flow grants, existing SysEx callbacks, `SYSEX_START`, `SYSEX_END`, `FRONT_CALLBACK_ACK`, sample-upload ACK;
- optional UI/status traffic: LED chase, beat pulse, track rotation echoes, transpose echoes, pattern/step query replies during load.

When `comm_quietUi` is set:

- suppress optional `uart_sendFrontpanelByte()` traffic;
- never suppress `uart_sendFrontpanelSysExByte()` callbacks;
- send flow grants through a priority send path that bypasses quiet mode.

If implementing a new function is clearer, add `uart_sendFrontpanelPriorityByte()` on STM32 and use it only for flow-control normal messages.

## Implementation Phases

### Phase 0: Baseline Confirmation

No code changes.

Build and record current behavior for:

- known-good modern `.ALL` load while stopped;
- known-good modern `.ALL` load while sequencer is running;
- known-good `.PRF` load while stopped;
- known-good `.PRF` load while sequencer is running.

Do not use legacy v2 `.ALL`/`.PRF` files for transport validation. File-layout compatibility is a separate confounder.

Record the exact visible stop point if a load hangs:

- before `Loading All` / `Loading Perf`;
- during globals, before the loading screen appears;
- during main-step, length, scale, chain, or step-data transfer;
- during voice/morph application;
- after the loading screen clears but UI/status no longer updates.

Minimum responsiveness check at the end of every baseline run:

- front-panel controls still respond;
- start/stop still toggles;
- sequencer audibly continues or restarts;
- pattern LEDs/status resume after load.

### Phase 1: Make AVR 3-Byte Sends Non-Deadlocking

Files:

- `front/LxrAvr/fifo.c`
- `front/LxrAvr/fifo.h`
- `front/LxrAvr/IO/uart.c`
- `front/LxrAvr/IO/uart.h`
- `front/LxrAvr/frontPanelParser.c`

Changes:

- add TX free-space query;
- add TX-empty query;
- add RX-only clear or otherwise avoid clearing TX at load/session boundaries;
- change `frontPanel_sendData()` to wait outside the atomic block and enqueue the triplet inside a short atomic block;
- leave protocol behavior unchanged.

Hardware gate:

- normal UI editing still works;
- kit load works;
- playback-active `.ALL`/`.PRF` behavior is no worse than baseline;
- if a load still hangs, it must be in an existing callback/session wait, not in a local TX deadlock.

Specific test point:

- run sequencer;
- start `.ALL`, then `.PRF`;
- during the first seconds of load, verify the AVR does not freeze before any SysEx callback waits. A freeze here means the send primitive is still unsafe.

### Phase 2: Add Flow Opcodes and Acknowledged Session Wrapper

Files:

- both protocol headers;
- AVR front parser;
- STM32 front parser.

Changes:

- define `SEQ_FLOW_*` / `FRONT_SEQ_FLOW_*`;
- add AVR state: `comm_flowActive`, `comm_flowChannel`, `comm_txCredits`;
- add STM32 state: `comm_loadSessionActive`, `comm_quietUi`, `comm_flowActive`, `comm_flowChannel`;
- parse flow commands on both sides;
- add bounded helper functions for begin/end/grant waits;
- wrap `.ALL` and `.PRF` loads with acknowledged `FLOW_CH_LOAD_SESSION` begin/end, but do not suppress STM32 UI traffic yet;
- do not meter any real burst yet.

Hardware gate:

- build passes;
- behavior unchanged because no real burst uses credits yet;
- playback-active `.ALL`/`.PRF` tests do not regress.

Specific test point:

- exercise the acknowledged `FLOW_CH_LOAD_SESSION` begin/end around real `.ALL` and `.PRF` loads without enabling quiet-mode suppression yet;
- verify both begin and end ACKs arrive;
- then run playback-active `.ALL` and `.PRF` loads. If either now hangs after load end, suspect end ACK/TX-clear ordering before proceeding.

### Phase 3: Add STM32 Quiet Mode

Files:

- `mainboard/LxrStm32/src/MIDI/Uart.c`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
- possibly `mainboard/LxrStm32/src/MIDI/Uart.h`

Changes:

- `FLOW_CH_LOAD_SESSION` begin enables quiet mode;
- end/abort disables quiet mode;
- optional STM32 -> AVR traffic is suppressed while quiet;
- priority traffic is not suppressed.

Session wrapper behavior:

- send session begin after the initial AVR UART clear and before `frontParser_rxDisable=1`;
- send session end after `SEQ_FILE_DONE`;
- wait for session-end ACK before `uart_clearFifo()` or replace that close-path clear with RX-only clear.

Hardware gate:

- `.ALL` and `.PRF` loads still complete;
- during load, missing beat/chase LEDs are acceptable;
- after load, UI status resumes.

Specific test point:

- run sequencer;
- load `.ALL`;
- confirm beat/current-step LEDs pause or become stale during load but resume after the end ACK;
- repeat with `.PRF`;
- if the load completes but the front panel no longer receives sequencer status, `FLOW_END` or its ACK was lost and this phase fails.

### Phase 4: Credit-Meter `menu_sendAllGlobals()`

Files:

- `front/LxrAvr/Menu/menu.c`
- AVR parser/helper state
- STM32 parser/helper state

Changes:

- before the globals loop, begin `FLOW_CH_GLOBALS`;
- let `frontPanel_sendData()` consume one credit per emitted 3-byte message;
- after the loop, send `FLOW_CH_GLOBALS` end;
- STM32 grants `4` messages at a time.

Why first:

- globals are sent early in `.ALL` and `.PRF`;
- `menu_parseGlobalParam()` can emit immediate STM32 replies, especially around `SEQ_SET_ACTIVE_TRACK`;
- this is the clearest current unmetered normal-message burst.

Hardware gate:

- repeated `.ALL` load while running;
- repeated `.PRF` load while running;
- no freeze at or shortly after "Loading All" / "Loading Perf".

Specific test point:

- run sequencer;
- load an `.ALL` file that includes globals;
- load a `.PRF` with `voiceArray >= 0x3f` so perf metadata/globals are sent;
- verify the machine reaches the first SysEx pattern section every time. A hang before the pattern-section callbacks means globals credit handling is still wrong.

### Phase 5: Credit-Meter Voice and Meta Parameter Bursts

Files:

- `front/LxrAvr/Preset/presetManager.c`
- existing flow parser/helper files.

Changes:

- wrap `preset_readDrumVoice()` body after `SEQ_LOAD_VOICE` with `FLOW_CH_VOICE_PARAM`;
- wrap `preset_readDrumsetMeta()` with `FLOW_CH_DRUM_META`;
- keep existing `frontPanel_holdForBuffer()` calls initially, but priority-protect its `CALLBACK_ACK` echo;
- if `frontPanel_holdForBuffer()` hangs during hardware testing, do not proceed to later phases until a narrow bounded wait is added for that function.

Reason:

- voice load sends velocity target, LFO target, output, morph-generated parameter changes, and unhold commands;
- these are ordinary 3-byte messages and currently rely on FIFO luck.

Hardware gate:

- kit load;
- `.ALL` load;
- `.PRF` load;
- all while stopped and while running.

Specific test point:

- run sequencer;
- load `.ALL` with all voices selected;
- verify it passes the currently playing pattern's step-data transfer and then survives the voice/morph application section;
- repeat `.PRF`;
- if the LCD hangs after pattern data but before returning to normal UI, suspect `frontPanel_holdForBuffer()` or voice/meta credits.

### Phase 6: Keep SysEx Item ACKs, Add Session Protection Only

Do not add normal flow grants inside these existing SysEx writers in the first implementation:

- `preset_readPatternMainStep()`
- `preset_readPatternLength()`
- `preset_readPatternScale()`
- `preset_readPatternChain()`
- `preset_readPatternStepData()`

Rationale:

- these already have item-level SysEx callbacks;
- normal `SEQ_CC` grants would interfere with the current AVR SysEx callback parser unless the parser is redesigned;
- quiet mode from the outer load session is the safer first protection.

Specific warning:

- do not change PAT_CHAIN to ACK only after a complete `(next, repeat)` pair unless AVR `preset_readPatternChain()` is changed away from its current byte-by-byte wait.

Hardware gate:

- pattern-chain sections do not hang;
- step-data sections do not hang;
- playback-active load reaches `SEQ_FILE_DONE`.

Specific test point:

- run sequencer with the currently playing pattern included in the load;
- load `.ALL`;
- confirm `SYSEX_RECEIVE_MAIN_STEP_DATA` running-state behavior does not leave tracks permanently locked;
- confirm `preset_readPatternChain()` still receives two callbacks per pattern, one after `next` and one after `repeat`;
- repeat with `.PRF`.

### Phase 7: Add Timeouts After Credits Are Stable

Only after Phases 1-6 pass hardware. This phase is deferred for sequencing, but it is not optional for a firmware build intended to avoid permanent lockups.

- add timeouts to credit waits;
- then add timeouts to `SYSEX_START`, `SYSEX_END`, `*_CALLBACK`, `STEP_ACK`, and `CALLBACK_ACK` waits;
- on timeout, send `SEQ_FLOW_ABORT` for any active channel and load session.

Reason for deferring:

- timeouts are recovery behavior, not flow control;
- adding them before preventing callback loss makes failures recoverable but does not make the transport healthy.

Specific test point:

- intentionally force a missing callback if possible, for example by temporarily disabling one callback in a test build or using a known bad transfer case;
- verify the AVR exits the load path and the front panel remains responsive;
- verify STM32 quiet mode is cleared by abort/end cleanup.

### Phase 8: Reconsider STM32 RX Throughput

Only after explicit flow control and quiet mode are stable:

- consider a bounded STM32 front RX drain, for example 4 or 8 bytes per `uart_processFront()` call;
- do not use an unbounded `while(fifoBig_bufferOut(...))` drain as the first retry.

Hardware gate:

- same `.ALL` / `.PRF` playback-active tests;
- no callback loss;
- no new lockups.

Specific test point:

- repeat the exact Phase 4-6 playback-active `.ALL` and `.PRF` tests;
- compare failure location and responsiveness against the single-byte-drain baseline;
- any return of the `Loading All` / `Loading Perf` freeze means the bounded drain is too aggressive or priority protection is incomplete.

## Code Organization Recommendation

Because both Makefiles already discover all `.c` files, a small helper module is acceptable:

- AVR: `front/LxrAvr/commFlow.c/.h`
- STM32: `mainboard/LxrStm32/src/MIDI/CommFlow.c/.h`

However, if the first implementation should be maximally small, keep the state and helpers inside the existing parser/UART files until the behavior is proven. The important part is not file layout; it is preserving narrow phase boundaries.

## Acceptance Criteria

Minimum success for this project goal:

1. `make clean && make firmware` passes.
2. Modern known-good `.ALL` loads repeatedly while sequencer is running.
3. Modern known-good `.PRF` loads repeatedly while sequencer is running.
4. No freeze waiting on `SYSEX_START`, `SYSEX_END`, `PATCHAIN_CALLBACK`, `MAINSTEP_CALLBACK`, `LENGTH_CALLBACK`, `SCALE_CALLBACK`, `STEP_ACK`, `STEP_CALLBACK`, or `CALLBACK_ACK`.
5. Normal UI LEDs/status may pause during load, but resume after load-session end.
6. PAT_CHAIN byte-by-byte ACK behavior remains compatible with the current AVR sender.
7. STM32 quiet mode cannot remain stuck on after load completion, timeout, or abort.
8. AVR load close paths cannot clear queued `SEQ_FILE_DONE` or `SEQ_FLOW_END` bytes before transmission.

## Required Phase Test Matrix

Every implementation phase has a test point. Do not stack the next phase until the current phase result is recorded.

Use this fixed matrix after each phase:

1. Build: `make clean && make firmware`.
2. Stopped transport check: known-good modern `.ALL`, then `.PRF`.
3. Playback-active check: start sequencer, load the same `.ALL`, then `.PRF`.
4. End responsiveness check: front controls respond, start/stop toggles, sequencer still runs or restarts, status LEDs resume.
5. Failure-location note if any: globals, mainstep, length, scale, chain, stepdata, voice/morph, load-end cleanup.

Phase-specific additions:

- Phase 1: confirm no freeze before the first SysEx section; that would indicate the AVR send primitive is still capable of deadlock.
- Phase 2: confirm the acknowledged session begin/end wrapper ACKs both directions.
- Phase 3: confirm quiet mode exits; a completed load with dead/stale UI is a fail.
- Phase 4: confirm globals no longer hang before `Loading All` / `Loading Perf` reaches pattern data.
- Phase 5: confirm voice/morph application after pattern data does not hang.
- Phase 6: confirm PAT_CHAIN still ACKs byte-by-byte and current-pattern running loads do not leave voices permanently locked.
- Phase 7: confirm forced/missing callback cases recover instead of permanently waiting.
- Phase 8: confirm any bounded drain does not recreate the prior `Loading All` freeze.

## Deferred Work

Do not include these in the first flow-control implementation:

- baud-rate changes;
- checksum/CRC framing;
- startup version handshake;
- legacy v2 `.ALL`/`.PRF` layout fixes;
- broad preset-manager hard-lock cleanup;
- global-settings UI for toggling `.PRF` cache-while-playing behavior;
- parameter-map/bounds cleanup.

Those may be important, but mixing them into flow control would recreate the failed broad-change problem.

## Bottom Line

The safest implementation is:

1. make AVR triplet sends non-deadlocking;
2. add TX-empty/RX-only-clear support so load-end bytes cannot be erased locally;
3. add flow opcodes, acknowledged load-session wrappers, and parser bypass for flow grants during `rxDisable`;
4. add acknowledged STM32 quiet mode for load sessions;
5. credit-meter globals first;
6. credit-meter voice/meta parameter bursts second;
7. leave SysEx item callbacks intact and protected by quiet mode;
8. add old-wait timeouts after the transport behavior is stable on hardware;
9. consider bounded STM32 drain only after all playback-active load tests pass.

This plan deliberately avoids the two proven traps: unbounded STM32 burst processing and unverified ACK-placement changes.
