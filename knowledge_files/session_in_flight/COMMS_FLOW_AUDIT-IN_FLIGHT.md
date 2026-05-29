# COMMS / FLOW AUDIT - IN FLIGHT

Date: 2026-05-29  
Status: consolidated current-state audit from stale flow/comms planning docs and `transcript.txt`.

## Purpose

This document replaces the working content of:

- `knowledge_files/session_in_flight/FLOW_CONTROL_AUDIT.md`
- `knowledge_files/session_in_flight/COMMS_AUDIT_SECOND.md`
- `knowledge_files/session_in_flight/CHECKPOINT_FLOW_CONTROL_SECOND_AUDIT.md`
- `knowledge_files/session_in_flight/FLOW_CONTROL_SECOND_AUDIT.md`

Those source documents contain useful history, but they also contain stale or contradicted assumptions from failed branches. This file keeps the failures as history and records the communications hardening plan that is currently deferred.

## Current Repository State

Communications flow-control work reached a hardware-tested checkpoint before the later `.PRF` background-load / temp-pattern isolation work. The checkpoint state should be understood as:

- `.ALL` load while playing no longer locked up in the tested paths.
- `.PRF` load while playing deferred the currently playing pattern correctly.
- `.PRF` load while playing also deferred active STM32 kit/morph/metadata application until manual pattern change or stop.
- AVR menu values could update immediately during `.PRF` read because the AVR parameter store is updated by file read. STM32 application was deferred.
- New flow waits are bounded.
- Older SysEx/callback waits are still mostly unbounded and remain deferred work.

The later WIP is no longer primarily "communications hardening". It is the `.PRF`/temp-slot isolation project described in `PRF_ALL_LOAD_FIX_AUDIT-IN_FLIGHT.md`. Do not resume broad comms work until the current temp-pattern/parameter pushback bug is resolved or explicitly set aside.

## Historical Failures To Keep

### Failed Broad Comms Repair

A prior broad repair attempt mixed too many unrelated changes:

- transport-layer UART behavior,
- parser recovery,
- preset-loader timeout/error paths,
- parameter mapping,
- diagnostics and instrumentation.

That made hardware results impossible to attribute. Future comms work must be staged in small, testable slices with one transport behavior per step.

### Unbounded STM32 RX Full-Drain Regression

Changing `uart_processFront()` to drain the entire STM32 front RX FIFO in one call caused a known-good `.ALL` load to freeze at `Loading All`. The likely failure class was burst compression: optional STM32 reply/status traffic and required callbacks shared lossy FIFOs, and the AVR waited forever for a byte that could be dropped.

Do not reintroduce unbounded full-drain as a first move. If RX throughput is revisited, use a bounded drain only after explicit session protection, timeout cleanup, and hardware tests.

### PAT_CHAIN ACK Misread

An earlier conclusion that `SYSEX_RECEIVE_PAT_CHAIN_DATA` should ACK only after a complete `(next, repeat)` pair was wrong for the actual AVR sender. The AVR sends `next[i]`, waits for `PATCHAIN_CALLBACK`, sends `repeat[i]`, and waits again.

Do not change PAT_CHAIN ACK placement unless the AVR sender is changed at the same time. Current compatibility requires byte-by-byte callbacks.

### Preset/File Bugs Confounded Comms Tests

Several non-comms issues originally looked like transport failures:

- `.PRF` load semantics were incomplete before the checkpoint.
- Legacy `.ALL` / `.PRF` file layout and short-read behavior were separate file-loader issues.
- `preset_readGlobalData()` had undefined behavior from an uninitialized `bytesRead`.

Do not use legacy or currently confounded file-load behavior to validate transport unless the file-loader state is known.

## Implemented Checkpoint Behavior

## Diff-Derived Code Inventory: `9120ea7620...` -> `90d3f08`

This section is based on a source diff of:

- `LXR-9120ea7620f1a9a4a924f029cdaf3ae71df303fd/front` -> `LXR-custom-develop-patload-envmod-90d3f08/front`
- `LXR-9120ea7620f1a9a4a924f029cdaf3ae71df303fd/mainboard` -> `LXR-custom-develop-patload-envmod-90d3f08/mainboard`

Generated outputs and build artifacts were ignored. The changed source files were:

- AVR: `fifo.c`, `fifo.h`, `IO/uart.c`, `IO/uart.h`, `frontPanelParser.c`, `frontPanelParser.h`, `Menu/menu.c`, `Menu/menuPages.h`, `Parameters.h`, `Preset/presetManager.c`
- STM32: `MIDI/MidiMessages.h`, `MIDI/Uart.c`, `MIDI/Uart.h`, `MIDI/frontPanelParser.c`, `MIDI/frontPanelParser.h`, `Sequencer/sequencer.c`, `Sequencer/sequencer.h`

### AVR 3-Byte Sends

AVR 3-byte sends were made non-deadlocking:

- `front/LxrAvr/fifo.c` / `fifo.h` gained `fifo_count()`, `fifo_free()`, and `fifo_isEmpty()`. These expose the ring buffer occupancy without changing the buffer format.
- `front/LxrAvr/IO/uart.c` / `uart.h` gained `uart_clearRxFifo()`, `uart_txFree()`, and `uart_txEmpty()`. `uart_clearRxFifo()` clears only inbound bytes; it does not erase pending outbound session-end messages. `uart_txFree()` and `uart_txEmpty()` wrap the new FIFO helpers.
- `front/LxrAvr/frontPanelParser.c` changed `frontPanel_sendData()` from "spin inside an atomic block until `uart_putc()` succeeds" to a two-stage send: first wait for at least 3 TX bytes with interrupts enabled, then enter a short `ATOMIC_BLOCK` and enqueue the complete 3-byte message contiguously.
- `frontPanel_sendData()` now also consumes one flow credit for each non-flow message while a credit channel is active. Flow commands themselves do not consume credits.
- Load close paths were adjusted to avoid erasing queued TX bytes.

What this fixes:

- The old send path could hard-deadlock if the AVR TX FIFO was full at entry, because the UDRE interrupt that drains the FIFO was masked by the atomic block.
- The new path still preserves 3-byte message contiguity, but it does not wait for FIFO space while interrupts are disabled.

What remains:

- `frontPanel_sendByte()` still waits directly on `uart_putc()`.
- `frontPanel_holdForBuffer()` still waits indefinitely for `FRONT_CALLBACK_ACK`; it was not converted in this checkpoint.

### Flow Opcodes And Session Wrapper

Flow-control opcodes were added on both MCUs:

```c
SEQ_FLOW_BEGIN
SEQ_FLOW_GRANT
SEQ_FLOW_END
SEQ_FLOW_ABORT
```

Implemented channels:

```c
FLOW_CH_LOAD_SESSION
FLOW_CH_GLOBALS
FLOW_CH_VOICE_PARAM
FLOW_CH_DRUM_META
```

Code details:

- AVR `frontPanelParser.h` defines the new `SEQ_FLOW_*` opcodes and `FLOW_CH_*` channels, and declares `frontPanel_flowBeginSession()`, `frontPanel_flowEndSession()`, `frontPanel_flowBegin()`, `frontPanel_flowEnd()`, `frontPanel_flowFailed()`, and `frontPanel_flowAbortSession()`.
- STM32 `MidiMessages.h` mirrors the opcodes as `FRONT_SEQ_FLOW_*` and defines the same channel IDs.
- AVR `frontPanelParser.c` adds session state: `comm_flowActive`, `comm_flowChannel`, `comm_txCredits`, `comm_flowAckPending`, `comm_flowAckChannel`, and `comm_flowFailed`.
- AVR `frontPanel_flowSendAndWait()` sends `SEQ_FLOW_BEGIN` / `SEQ_FLOW_END` and waits for the STM32 grant/ACK.
- AVR `frontPanel_waitForFlowAck()` and `frontPanel_waitForCredit()` use `time_sysTick` with `FLOW_WAIT_TICKS`. On timeout they send `SEQ_FLOW_ABORT`, clear active credit state, and mark `comm_flowFailed`.
- AVR `frontPanel_parseData()` was widened so flow messages are still parsed while `frontParser_rxDisable=1`. Normal traffic remains suppressed during loads, but `SEQ_CC / SEQ_FLOW_*` can still update the flow state.
- STM32 `frontPanelParser.c` adds matching state: `comm_loadSessionActive`, `comm_quietUi`, `comm_flowActive`, `comm_flowChannel`, and `comm_flowBudgetRemaining`.
- STM32 `frontParser_sendFlowGrant()` emits priority `FRONT_SEQ_CC`, `FRONT_SEQ_FLOW_GRANT`, and packed channel/credits bytes.
- STM32 `frontParser_handleSeqCC()` handles flow begin/end/abort. Load-session begin sets quiet mode and ACKs with `FLOW_ACK_CREDITS`; load-session end clears quiet mode and ACKs. Non-session channel begin grants `FLOW_INITIAL_GRANT` credits.
- STM32 `frontParser_flowMessageApplied()` decrements the active channel budget after non-flow messages and sends another grant when the budget reaches zero.

`.ALL` and `.PRF` loads begin and end with an acknowledged load session. Flow replies are parsed even while `frontParser_rxDisable=1`. New flow waits are bounded by `FLOW_WAIT_TICKS`; timeout sends `SEQ_FLOW_ABORT` and marks flow failed.

For `FLOW_CH_LOAD_SESSION`, `SEQ_FLOW_GRANT` is used as an ACK, not a data credit grant.

### STM32 Quiet Mode

STM32 quiet mode suppresses optional UI/status traffic during a load session while preserving priority traffic:

- flow ACKs/grants,
- required SysEx callback bytes,
- `FRONT_CALLBACK_ACK`,
- session close traffic.

Code details:

- `mainboard/LxrStm32/src/MIDI/Uart.c` changes `uart_sendFrontpanelByte()` to check `frontParser_isQuietUi()`. While quiet mode is active, normal front-panel bytes are dropped/suppressed unless they are sent through the priority path.
- `Uart.c` adds `uart_sendFrontpanelPriorityByte()` and `Uart.h` declares it. Priority sends bypass quiet mode.
- `frontPanelParser.c` changes `FRONT_CALLBACK_ACK` handling to use `uart_sendFrontpanelPriorityByte(FRONT_CALLBACK_ACK)`.
- The old behavior cleared STM32 front FIFOs around `FRONT_CALLBACK_ACK`; the checkpoint code stops using that FIFO-clearing ACK echo path.

What this fixes:

- Optional beat/status/LED/query traffic can no longer fill the STM32 front TX FIFO ahead of required flow ACKs and SysEx callbacks while a load session is active.

What remains:

- Priority sends still enqueue into the same physical FIFO and do not have a separate priority queue; they only bypass quiet-mode suppression.

### Credit-Metered Bursts

The following AVR-to-STM32 normal-message bursts were credit-metered:

- globals via `menu_sendAllGlobals()`,
- voice parameter bursts via `preset_readDrumVoice()`,
- drum/meta bursts via `preset_readDrumsetMeta()`.

Code details:

- `front/LxrAvr/Menu/menu.c` wraps `menu_sendAllGlobals()` in `frontPanel_flowBegin(FLOW_CH_GLOBALS)` and `frontPanel_flowEnd(FLOW_CH_GLOBALS)`. Every `frontPanel_sendData()` emitted from `menu_parseGlobalParam()` consumes one credit.
- `front/LxrAvr/Preset/presetManager.c` wraps the normal voice-send portion of `preset_readDrumVoice()` in `FLOW_CH_VOICE_PARAM`. This covers velocity target, LFO target, output destination, morph-generated parameter sends, and `SEQ_UNHOLD_VOICE`.
- `preset_readDrumsetMeta()` wraps non-morph kit metadata/macro sends in `FLOW_CH_DRUM_META`. Morph metadata copy remains local to the AVR parameter arrays and is not wrapped as a send burst.
- Each wrapper checks `frontPanel_flowFailed()` so a timeout can stop further send bursts rather than continuing to flood the link after credit failure.

SysEx pattern writers were intentionally left on their existing callback protocol:

- `preset_readPatternMainStep()`
- `preset_readPatternLength()`
- `preset_readPatternScale()`
- `preset_readPatternChain()`
- `preset_readPatternStepData()`

They are protected by the outer load-session quiet mode rather than by injected normal-message grants.

### `.PRF` While-Playing Deferral

Checkpoint behavior added:

```c
uint8_t frontParser_deferPerfLoadCacheUntilPatternChange = 1;
```

When enabled, `.PRF` while running caches active kit/morph/metadata/current-pattern changes until manual pattern change or stop. Protocol addition:

```c
SEQ_FILE_BEGIN
FRONT_SEQ_FILE_BEGIN
```

Deferred STM32 state included:

- `frontParser_deferredPerfLoadActive`
- `frontParser_deferredPerfVoiceCachePending`
- `frontParser_deferredPerfPatternPending`
- `frontParser_deferredPerfUnholdPending`
- `frontParser_deferredPerfProtectedPattern`
- `frontParser_deferredPerfMsgCache[]`

Apply points were manual pattern change in `seq_nextStep()` and stop in `FRONT_SEQ_RUN_STOP`.

Code details:

- AVR `preset_loadAll()` and `preset_loadPerf()` now call `frontPanel_flowBeginSession()` before setting `frontParser_rxDisable=1`, and call `frontPanel_flowEndSession()` after `SEQ_FILE_DONE`.
- AVR load paths use `uart_clearRxFifo()` at load boundaries instead of clearing both RX and TX.
- AVR `.PRF` sends `SEQ_FILE_BEGIN, WTYPE_PERFORMANCE` immediately after version validation so the STM32 can decide whether to protect the currently playing state before metadata/globals arrive.
- STM32 `frontPanelParser.c` adds `frontParser_deferPerfLoadCacheUntilPatternChange`, defaulting to enabled at checkpoint.
- STM32 `FRONT_SEQ_FILE_BEGIN` enters deferred performance mode only for performance loads while the sequencer is running and deferral is enabled. It captures the protected active pattern number.
- During deferred performance load, STM32 caches direct non-voice messages into `frontParser_deferredPerfMsgCache[]`, defers `UNHOLD_VOICE` publication, and marks protected-pattern writes pending rather than immediately replacing the playing pattern.
- `frontParser_applyDeferredPerfMessages()` replays cached direct messages when the deferred cache is applied.
- `frontParser_applyPendingVoiceCache()` publishes cached voice, LFO, velocity, and unhold state at apply time.
- `frontParser_applyDeferredVoiceCache()` applies deferred messages, voice caches, and a pending `seq_tmpPattern` copy back into the protected pattern slot.
- `Sequencer/sequencer.c` replaces the old active-pattern-only `seq_activateTmpPattern()` behavior with `seq_applyTmpPatternTo(pattern)`, so the deferred `.PRF` protected pattern can be committed back to its original pattern slot instead of always overwriting whichever pattern happens to be active at that instant.
- `seq_nextStep()` calls `frontParser_applyDeferredVoiceCache()` during the manual pattern-change path; `FRONT_SEQ_RUN_STOP` also applies deferred cache on stop.
- `frontPanelParser.h` exposes the deferred-apply helper to the sequencer.

Related nonfunctional or minor diff items:

- `front/LxrAvr/Parameters.h` only adds/comment-adjusts notes around `PAR_FILE_LOAD_FAST` and a future cache setting; it does not add a new parameter enum in the checkpoint.
- `front/LxrAvr/Menu/menuPages.h` only changes spacing/alignment around global menu entries.

## Current Caveats

### Old Permanent Waits Remain

The deferred Phase 7 work is still not complete. These waits can still become permanent if their expected byte is lost:

- `SYSEX_START`
- `SYSEX_END`
- `MAINSTEP_CALLBACK`
- `LENGTH_CALLBACK`
- `SCALE_CALLBACK`
- `PATCHAIN_CALLBACK`
- `STEP_ACK`
- `STEP_CALLBACK`
- `CALLBACK_ACK`

### Fixed-Size Deferred Message Cache

The deferred `.PRF` message cache has a fixed size:

```c
#define DEFERRED_PERF_MSG_CACHE_SIZE 128
```

This appeared sufficient for current metadata/direct-message use because bulk voice parameters use the voice cache. If `.PRF` metadata grows, add overflow handling before relying on it.

### Front-Panel Menu May Show Loaded Values Early

During `.PRF` file read, the AVR parameter store may update before STM32 application. That was accepted at the checkpoint. Do not change it as a comms fix unless the desired UX changes.

### Current WIP Adds Another Comms Pressure Point

The temp-pattern parameter pushback path sends STM32 parameter values back to AVR so the menu reflects the currently active normal or temporary parameter set. That pushback currently does not work correctly and is the next immediate WIP task. It should not be folded into broad flow-control Phase 7 until its narrow behavior is debugged.

## Deferred Communications Hardening Plan

This is the current plan to resume after the `.PRF`/temp parameter WIP is stabilized.

### 1. Reconfirm Baseline

Before editing comms again:

1. Build `make clean && make firmware`.
2. Test known-good modern `.ALL` stopped.
3. Test known-good modern `.ALL` while running.
4. Test known-good modern `.PRF` stopped.
5. Test known-good modern `.PRF` while running.
6. Confirm front-panel controls, start/stop, sequencer playback, and status LEDs recover after each load.

Do not use legacy v2 `.ALL` / `.PRF` files as transport validation unless the file layout is the test subject.

### 2. Add Timeouts To Old Waits

Add timeouts one reader family at a time. On timeout:

- abort any active credit channel,
- abort or end any active load session,
- clear STM32 quiet mode,
- return control to the UI instead of staying in a permanent wait.

Start with the most frequently hit load paths:

- `preset_readPatternMainStep()`
- `preset_readPatternLength()`
- `preset_readPatternScale()`
- `preset_readPatternChain()`
- `preset_readPatternStepData()`
- `frontPanel_holdForBuffer()`

Keep this recovery work narrow. Do not combine it with file-layout compatibility, parameter-map cleanup, or diagnostics expansion.

### 3. Prove Quiet-Mode Cleanup

After adding any timeout:

1. Force or simulate a missing callback where practical.
2. Confirm AVR exits the load path.
3. Confirm STM32 quiet mode is off.
4. Confirm status traffic resumes.
5. Confirm a subsequent `.ALL` / `.PRF` load still works.

### 4. Reconsider STM32 RX Throughput Only After Recovery Works

If throughput still needs work after timeouts:

- try a bounded STM32 RX drain, such as 4 or 8 bytes per `uart_processFront()` call;
- compare against the single-byte-drain baseline;
- reject any change that recreates `Loading All` / `Loading Perf` freezes.

Do not use an unbounded `while(fifoBig_bufferOut(...))` drain.

### 5. Keep Explicitly Deferred

Do not mix these into the next comms pass:

- baud-rate changes,
- checksum/CRC framing,
- startup version negotiation,
- broad preset-manager error-path rewrite,
- legacy v2 `.ALL` / `.PRF` compatibility,
- parameter map centralization,
- broad diagnostics APIs across both MCUs,
- `seq_loadFastMode` redesign unless transport remains unstable after the narrow work.

## Bottom Line

The current communications layer is improved but not complete. Flow sessions, quiet mode, and credit-metered normal bursts exist at the checkpoint. The remaining communications hardening is timeout/recovery work for old SysEx/callback waits, plus any later bounded RX-throughput experiment. The immediate continuation task is not broad comms hardening; it is fixing the current temp-pattern parameter pushback behavior described in `PRF_ALL_LOAD_FIX_AUDIT-IN_FLIGHT.md`.
