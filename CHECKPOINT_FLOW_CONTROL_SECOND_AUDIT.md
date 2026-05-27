# CHECKPOINT_FLOW_CONTROL_SECOND_AUDIT

Date: 2026-05-27

## Purpose

This is the checkpoint for the in-flight second flow-control implementation. It records the state after Phases 1-6 of `FLOW_CONTROL_SECOND_AUDIT.md`, plus the `.PRF` load-semantic fixes that became necessary during hardware testing.

The user-tested state at this checkpoint is good:

- `.ALL` load while playing no longer locks up in the tested paths.
- `.PRF` load while playing defers the currently playing pattern correctly.
- `.PRF` load while playing now also defers the active STM32 kit/morph/metadata application until manual pattern change or stop.
- The front-panel menu may show newly loaded `.PRF` values immediately, because the AVR parameter store is updated during file read. That is expected. STM32 application is deferred.

## Build Status At Checkpoint

Verified commands:

```sh
make -C mainboard/LxrStm32 stm32
make -C front/LxrAvr avr
make firmware
```

Known compiler warnings remain in old code paths. No new build blocker is present at this checkpoint.

Generated firmware image:

```text
firmware image/FIRMWARE.BIN
```

Last observed image sizes from `make firmware`:

```text
AVR: 54158
STM: 230564
```

## Implemented Flow-Control Phases

### Phase 1: AVR 3-Byte Sends Made Non-Deadlocking

Files touched:

- `front/LxrAvr/fifo.c`
- `front/LxrAvr/fifo.h`
- `front/LxrAvr/IO/uart.c`
- `front/LxrAvr/IO/uart.h`
- `front/LxrAvr/frontPanelParser.c`

Implemented:

- FIFO enqueue now reports failure instead of silently advancing into full state.
- FIFO free-space and empty helpers were added.
- AVR UART exposes `uart_txFree()`, `uart_txEmpty()`, and `uart_clearRxFifo()`.
- `frontPanel_sendData()` waits for 3 TX bytes of space with interrupts enabled, then enqueues the 3-byte message inside a short atomic block.
- Load close paths use RX-only clearing so queued TX bytes are not erased.

Why this matters:

- The old `frontPanel_sendData()` could spin forever inside an atomic block if the TX FIFO was full, because the UDRE interrupt that drains the FIFO was masked.

### Phase 2: Flow Opcodes And Session Wrapper

Files touched:

- `front/LxrAvr/frontPanelParser.h`
- `mainboard/LxrStm32/src/MIDI/MidiMessages.h`
- `front/LxrAvr/frontPanelParser.c`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
- `front/LxrAvr/Preset/presetManager.c`

Implemented opcodes:

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

Implemented behavior:

- `.ALL` and `.PRF` loads begin with an acknowledged load session.
- The AVR waits for STM32 begin/end ACKs.
- Flow replies are parsed even while `frontParser_rxDisable=1`.
- Flow waits are bounded by `FLOW_WAIT_TICKS`.
- On timeout, AVR sends `SEQ_FLOW_ABORT` and marks the flow failed.

Important detail:

- Load-session begin/end uses `SEQ_FLOW_GRANT` as an ACK, not as data credits.

### Phase 3: STM32 Quiet Mode

Files touched:

- `mainboard/LxrStm32/src/MIDI/Uart.c`
- `mainboard/LxrStm32/src/MIDI/Uart.h`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.h`

Implemented:

- STM32 sets `comm_quietUi` during load session.
- Normal `uart_sendFrontpanelByte()` traffic is suppressed while quiet.
- `uart_sendFrontpanelPriorityByte()` bypasses quiet mode.
- SysEx callback bytes continue to use the callback path and are not suppressed.
- `FRONT_CALLBACK_ACK` now uses the priority send path and no longer clears STM32 front FIFOs before echoing.

Why this matters:

- Optional beat/current-step/status traffic cannot starve or erase required load callbacks and flow ACKs during a load.

### Phase 4: Credit-Meter Globals

File touched:

- `front/LxrAvr/Menu/menu.c`

Implemented:

- `menu_sendAllGlobals()` wraps the globals loop in `FLOW_CH_GLOBALS`.
- `frontPanel_sendData()` consumes one credit for each non-flow 3-byte message while a credit channel is active.
- STM32 grants credits in batches of 4.

Hardware result:

- User testing passed for the `.ALL` / `.PRF` globals section while playing.

### Phase 5: Credit-Meter Voice And Drum Meta Bursts

File touched:

- `front/LxrAvr/Preset/presetManager.c`

Implemented:

- `preset_readDrumVoice()` wraps voice parameter send bursts in `FLOW_CH_VOICE_PARAM`.
- `preset_readDrumsetMeta()` wraps drumset/macro metadata bursts in `FLOW_CH_DRUM_META`.
- Existing `frontPanel_holdForBuffer()` remains, but its STM32 echo is priority-protected.

Hardware result:

- User testing passed after this phase.

### Phase 6: Keep SysEx ACKs, Protect With Session

The SysEx writers were intentionally left on their existing item-callback protocol:

- `preset_readPatternMainStep()`
- `preset_readPatternLength()`
- `preset_readPatternScale()`
- `preset_readPatternChain()`
- `preset_readPatternStepData()`

Implemented protection:

- No normal `SEQ_CC` flow grants are injected inside active SysEx transfers.
- STM32 suspends active credit flow if a SysEx transfer begins.
- Quiet mode protects the callback stream from optional UI/status traffic.

Important compatibility point:

- `preset_readPatternChain()` still expects byte-by-byte callbacks: one after `next`, one after `repeat`. Do not change the STM32 callback to pair-level ACK without changing the AVR sender.

Hardware result:

- User testing passed for Phase 6 before the `.PRF` semantic issue was investigated.

## `.ALL` File-Done Voice Cache Fix

Problem found during testing:

- Loading `.ALL` while playing could fail to apply some mod destinations, specifically a test case where Drum 3 LFO was routed to Drum 2 coarse pitch.

Cause:

- Voice/LFO/velocity cache state could remain pending after file load and not deterministically commit at file completion.

Implemented:

- STM32 applies pending voice cache on `FRONT_SEQ_FILE_DONE` for normal/non-deferred loads.
- Velocity target cache availability now sets `midi_midiVeloCacheAvailable`, not the LFO availability flag.
- Cache availability bits are cleared after application.

Validated by user:

- The `.ALL` test case landed correctly after the fix.

## `.PRF` While-Playing Semantic Fix

Required behavior:

- `.PRF` while playing is intentionally different from `.ALL`.
- The currently playing pattern and currently playing drumkit parameters must keep sounding until the user changes patterns manually.
- Stop should also apply the deferred `.PRF` cache.
- There must be an internal global flag so a later setting can choose between this cache-while-playing behavior and `.ALL`-style immediate apply.

Implemented flag:

```c
uint8_t frontParser_deferPerfLoadCacheUntilPatternChange = 1;
```

Behavior:

- `1`: `.PRF` while running caches active kit/morph/metadata/current pattern until manual pattern change or stop.
- `0`: `.PRF` behaves like `.ALL` and applies at file done.

Protocol addition:

```c
SEQ_FILE_BEGIN
FRONT_SEQ_FILE_BEGIN
```

Why it was needed:

- STM32 must know a performance load has started before `.PRF` metadata/globals arrive.
- AVR sends `SEQ_FILE_BEGIN, WTYPE_PERFORMANCE` immediately after validating the `.PRF` version and before `menu_sendAllGlobals()`.

STM32 deferred `.PRF` state:

- `frontParser_deferredPerfLoadActive`
- `frontParser_deferredPerfVoiceCachePending`
- `frontParser_deferredPerfPatternPending`
- `frontParser_deferredPerfUnholdPending`
- `frontParser_deferredPerfProtectedPattern`
- `frontParser_deferredPerfMsgCache[]`

Deferred data:

- `FRONT_SEQ_CC` messages, including MIDI channel/note metadata.
- `FRONT_SET_BPM`.
- `FRONT_CC_MACRO_TARGET`.
- Direct kit/morph/metadata messages outside active voice loading.
- Voice parameter caches produced while `seq_voicesLoading` is set.
- `UNHOLD_VOICE`, now deferred so it does not publish protected voice caches to active kit state early.
- The STM32-side `seq_activePattern` captured at file begin.

Pattern behavior:

- The protected currently playing pattern is staged into `seq_tmpPattern`.
- Other patterns load into `seq_patternSet` normally.
- `seq_applyTmpPatternTo(pattern)` was added so the deferred protected pattern can be committed back to its original pattern slot later.

Apply points:

- Manual pattern change path in `seq_nextStep()`.
- Stop path in `FRONT_SEQ_RUN_STOP`.

User-observed behavior at checkpoint:

- Pattern data waits correctly.
- Kit/voice/morph application now waits correctly.
- Menu values on the AVR update immediately; this is acceptable because STM32 application is deferred.

## Important Current Caveats

### 1. Phase 7 Is Still Not Done

Timeouts for old waits remain future work:

- `SYSEX_START`
- `SYSEX_END`
- `MAINSTEP_CALLBACK`
- `LENGTH_CALLBACK`
- `SCALE_CALLBACK`
- `PATCHAIN_CALLBACK`
- `STEP_ACK`
- `STEP_CALLBACK`
- `CALLBACK_ACK`

The new flow waits are bounded. The older SysEx/callback waits are still mostly permanent waits and should be handled in Phase 7.

### 2. `.PRF` Deferred Message Cache Has A Fixed Size

Current STM32 cache:

```c
#define DEFERRED_PERF_MSG_CACHE_SIZE 128
```

This should be enough for the current metadata/direct-message set, because bulk voice parameters still use the existing voice cache. If future `.PRF` metadata grows, this needs an overflow policy before shipping.

### 3. Front-Panel Menu Shows Loaded `.PRF` Values Immediately

This is expected:

- AVR file read updates `parameter_values` and `parameters2`.
- STM32 application is deferred.

Do not "fix" this unless the desired UX changes.

### 4. Worktree Contains Non-Flow Changes

At this checkpoint, `git status` shows additional modified/untracked files outside the core flow-control implementation, including:

- `.DS_Store`
- `front/LxrAvr/Parameters.h`
- `front/LxrAvr/Menu/menuPages.h`
- `P000.ALL`
- `P000.PRF`
- `SdCardImage/`
- `knowledge_files/failed_commit/`

Review these before committing broadly.

## Suggested Re-Check Before Phase 7

Before implementing Phase 7, re-check Phase 6 with the current code:

1. Build `make clean && make firmware`.
2. Run sequencer.
3. Load known-good `.ALL`.
4. Confirm it reaches `SEQ_FILE_DONE`, applies mod destinations, and remains responsive.
5. Run sequencer.
6. Load known-good `.PRF`.
7. Confirm the current kit/morph/current pattern continue sounding after load completes.
8. Confirm menu values may update immediately.
9. Manually change patterns.
10. Confirm deferred `.PRF` kit/morph/metadata/current-pattern data applies.
11. Repeat `.PRF` while playing, then stop instead of changing pattern.
12. Confirm deferred `.PRF` data applies on stop.
13. Confirm PAT_CHAIN still ACKs byte-by-byte.
14. Confirm no tracks remain permanently locked after current-pattern running loads.

Only after that should Phase 7 add timeouts to old waits.

## Next Planned Work

Phase 7 from `FLOW_CONTROL_SECOND_AUDIT.md`:

- add recovery timeouts to existing unbounded waits;
- on timeout, abort active credit channel/session;
- ensure STM32 quiet mode cannot remain stuck on after abort;
- keep this narrow and do not mix in file-layout recovery or legacy preset compatibility.
