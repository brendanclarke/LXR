# COMMS_AUDIT_SECOND

Date: 2026-05-25

## Purpose

This document is a second-pass audit after the failed broad communications repair attempt documented in `knowledge_files/failed_commit/`.

Goal here is not to re-argue every issue from the original UART audit. The goal is to identify why the prior patch was hard to trust, and to define the slowest practical, least-delta change path so each step can be tested in isolation on hardware.

## Inputs Reviewed

- `README.md`
- `MEMORY.md`
- `knowledge_files/failed_commit/ATMEGA_STM32F4_COMMS_AUDIT.md`
- `knowledge_files/failed_commit/002_SESSION_HANDOFF_LOG.md`
- `knowledge_files/failed_commit/FILEFIX_AUDIT.md`
- `knowledge_files/failed_commit/PARAMETER_AUDIT.md`
- Current source snapshot in this repository

## Executive Summary

The failed attempt appears to have gone wrong mainly because it bundled too many kinds of change into one pass:

1. Transport-layer UART fixes.
2. Parser-state recovery changes.
3. Preset-loader timeout/error-path rewrites.
4. Parameter-mapping cleanup across both MCUs.
5. Diagnostics/instrumentation additions.

That made the validation signal muddy. Later audits found non-UART bugs in preset loading and file compatibility that can easily look like communications failures:

- `.prf` load path currently does not apply voice sound parameters after reading kit data.
- Legacy v2 `.all` / `.prf` files are accepted but read using newer offsets.
- `preset_readGlobalData()` uses `bytesRead` uninitialized.

So even if some of the Session 002 comms changes were correct, the bundle was too wide to tell what actually improved, what regressed, and what was merely exposing older preset/file bugs.

## What Went Wrong In The Previous Attempt

## 1. Scope ballooned past “communications only”

The Session 002 handoff shows changes across ten firmware files on both MCUs, including:

- `front/LxrAvr/IO/uart.c`
- `front/LxrAvr/frontPanelParser.c`
- `front/LxrAvr/Preset/presetManager.c`
- `front/LxrAvr/Menu/menu.c`
- `mainboard/LxrStm32/src/MIDI/Uart.c`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`

That is already a large change surface for an embedded dual-MCU protocol. Adding preset-manager recovery rewrites on top of parser and UART changes made it impossible to isolate failures cleanly.

## 2. The highest-risk bugs are localized, but the patch was not

The current source still shows that several of the most serious comms hazards live in very small hotspots:

- AVR deadlock risk in `front/LxrAvr/frontPanelParser.c:971`
- AVR unbounded hold wait in `front/LxrAvr/frontPanelParser.c:122`
- AVR unbounded ACK wait in `front/LxrAvr/IO/uart.c:169`
- STM32 one-byte RX drain in `mainboard/LxrStm32/src/MIDI/Uart.c:152`
- STM32 premature pattern-chain ACK in `mainboard/LxrStm32/src/MIDI/frontPanelParser.c:459`
- STM32 sysex session reset fragility in `mainboard/LxrStm32/src/MIDI/frontPanelParser.c:303`

These can be addressed with tiny, isolated edits. The failed attempt instead mixed them with broader behavioral rewrites.

## 3. Preset/file bugs were acting as confounders

Post-mortem audits strongly suggest some “comms failure” symptoms were actually preset-path defects:

- `front/LxrAvr/Preset/presetManager.c:2344-2366`:
  `preset_loadPerf()` reads kit data to temp buffers but never mirrors the `preset_readDrumVoice()` apply stage used by `preset_loadAll()`.
- `front/LxrAvr/Preset/presetManager.c:229-239`:
  `preset_readGlobalData()` uses `bytesRead` before initialization.
- `front/LxrAvr/Preset/presetManager.c:300-390` plus file-layout constants:
  legacy `.all` / `.prf` compatibility is incomplete, so valid old files can be mis-read.

Because those bugs live in the same user-visible flows as preset transfer traffic, they can masquerade as UART regressions.

## 4. Too many semantic changes landed before there was a narrow test staircase

The previous path changed:

- wait semantics,
- parser reset semantics,
- parameter numbering semantics,
- preset error handling semantics,
- UART diagnostics behavior.

That is too much for “test after every step” work on real hardware. For this codebase, each transport fix should ideally touch one file, or at most one function pair across AVR/STM32, with a single focused test.

## Current High-Value Problems Still Present In Source

## Critical transport hazards

1. AVR `frontPanel_sendData()` can deadlock permanently if TX FIFO is full while interrupts are masked.
- File: `front/LxrAvr/frontPanelParser.c:971-981`

2. AVR `frontPanel_holdForBuffer()` waits forever for callback ACK.
- File: `front/LxrAvr/frontPanelParser.c:122-132`

3. AVR `uart_waitAck()` waits forever and uses invalid `NACK` semantics.
- Files:
  - `front/LxrAvr/IO/uart.c:169-186`
  - `front/LxrAvr/IO/uart.h:21-25`

4. STM32 drains only one front-panel RX byte per service call.
- File: `mainboard/LxrStm32/src/MIDI/Uart.c:152-166`

5. STM32 pattern-chain receive path ACKs even on the first half of a two-byte pair.
- File: `mainboard/LxrStm32/src/MIDI/frontPanelParser.c:459-489`

## Important but not first-pass

6. STM32 clears both front TX and RX FIFOs on `FRONT_CALLBACK_ACK`, which can discard unrelated queued outbound bytes.
- Files:
  - `mainboard/LxrStm32/src/MIDI/frontPanelParser.c:320-323`
  - `mainboard/LxrStm32/src/MIDI/Uart.c:60-64`

7. `seq_loadFastMode` writes active pattern structures while playback can read them.
- File: `mainboard/LxrStm32/src/MIDI/frontPanelParser.c:421-457`

8. Preset manager still contains many unconditional `while(1){;}` traps.
- File: `front/LxrAvr/Preset/presetManager.c`

## Slow Path: Least-Delta Fix Order

The correct recovery path is to separate pure transport fixes from preset/file correctness fixes.

### Phase 0: Establish a clean baseline

Do not change code yet. Build and record current hardware behavior for:

1. Single kit load.
2. Single pattern save/load.
3. `.all` load of a known-good modern file only.
4. `.prf` load, but treat current failure as known-confounded.
5. Same operations while sequencer is running.

Important: do not use legacy v2 `.all` / `.prf` files to validate comms changes. Those files currently have known compatibility issues.

### Phase 1: Fix the pure STM32 parser correctness bugs first

Files:

- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`

Changes:

1. Reset `frontParser_sysexSeqStepNr` and `frontParser_rxCnt` explicitly when `SYSEX_START` arrives, not only when the mode byte is later received.
2. Move `SYSEX_RECEIVE_PAT_CHAIN_DATA` ACK send so it only occurs after the second byte of the pair is processed.

Why first:

- One file only.
- Receiver-side only.
- No interface changes.
- Directly addresses real desync risk without touching preset semantics.

Hardware test gate:

1. Save and reload pattern-chain data repeatedly.
2. Repeat during playback.
3. Confirm no immediate preset-transfer desync or stuck wait.

### Phase 2: Fix STM32 front UART service throughput

Files:

- `mainboard/LxrStm32/src/MIDI/Uart.c`

Change:

1. Change `uart_processFront()` from a single-byte `if(...)` drain to a `while(...)` drain.

Why second:

- One-file, one-function change.
- Very high leverage.
- Does not alter file formats, parameter mapping, or AVR behavior.

Optional in the same commit only if you want visibility and can keep it tiny:

2. Add a simple USART3 overrun counter in the IRQ path.

If visibility adds header churn or accessor churn, defer the counter to a separate follow-up commit.

Hardware test gate:

1. Re-run the Phase 1 tests.
2. Then stress with repeated kit or pattern loads while playback runs.
3. Watch for improved responsiveness and fewer stuck transfers.

### Phase 3: Remove the AVR FIFO-full deadlock, but only that

Files:

- `front/LxrAvr/frontPanelParser.c`

Change:

1. Rework `frontPanel_sendData()` so it does not spin waiting for FIFO space inside `ATOMIC_BLOCK`.

Recommended constraint:

- Do not combine this with timeout work.
- Do not combine this with parser-reset work.
- Do not combine this with `frontPanel_holdForBuffer()` changes.

Reason:

This is the most severe AVR-side transport hazard, but it is also the easiest to accidentally “fix” in a way that introduces message interleaving. Keep the commit focused so any regression is attributable.

Hardware test gate:

1. High UI activity: encoder turns, LED traffic, step editing.
2. Pattern save/load and kit load during playback.
3. Confirm no hard hangs.

### Phase 4: Normalize ACK handling in the narrowest possible place

Files:

- `front/LxrAvr/IO/uart.h`
- `front/LxrAvr/IO/uart.c`

Changes:

1. Remove duplicate `ACK` / `NACK` definitions.
2. Make `NACK` a real byte value, not `-1` stored through a `uint8_t`.
3. Add timeout only to `uart_waitAck()`.

Why this is safe now:

- `uart_waitAck()` is only used in one visible call site in `front/LxrAvr/Menu/menu.c:2048`.
- This avoids changing the far more widely used `frontPanel_holdForBuffer()` yet.

Hardware test gate:

1. Sample-upload start path.
2. Confirm success path unchanged.
3. Confirm missing ACK no longer freezes the entire UI.

### Phase 5: Fix the non-comms confounders before broad preset recovery work

Files:

- `front/LxrAvr/Preset/presetManager.c`

Changes, in this order:

1. Fix `.prf` load path so `preset_loadPerf()` actually applies voice parameters, mirroring the relevant `preset_loadAll()` voice-apply sequence.
2. Initialize `bytesRead` in `preset_readGlobalData()`.
3. Fix stale-`res` / short-read correctness bugs identified in `FILEFIX_AUDIT.md`.

Why this phase belongs before timeout/recovery rewrites in preset code:

If preset loading is still semantically wrong, transport fixes cannot be judged fairly. This phase reduces false attribution.

Hardware test gate:

1. Test `.prf` loads with current card set.
2. Test short/legacy global config behavior.
3. Keep comms stress separate from legacy-file-compatibility testing.

### Phase 6: Only then start replacing preset-manager hard locks

Files:

- `front/LxrAvr/Preset/presetManager.c`

Approach:

Do not replace all `while(1){;}` sites in one pass.

Instead:

1. Convert one reader family at a time.
2. Start with the functions used most often during normal load:
   - `preset_readKitToTemp()`
   - `preset_readPatternMainStep()`
   - `preset_readPatternLength()`
   - `preset_readPatternStepData()`
3. Return a small error code and show LCD error text, but do not redesign the whole preset API in the same pass.

Reason:

This is where the failed attempt expanded too far. Error-path modernization is good work, but it is not a “small comms fix”.

## Changes To Explicitly Defer For Now

Do not mix any of the following into the slow path above:

1. Baud-rate changes.
2. Protocol framing / checksum redesign.
3. Startup handshake/version negotiation.
4. Cross-codebase parameter offset centralization.
5. Broad parser reset / recovery helper APIs.
6. Large diagnostics API additions on both MCUs.
7. `seq_loadFastMode` redesign, unless transport remains unstable after Phases 1-4.

These may all be worthwhile later, but each increases the blast radius and makes hardware results harder to interpret.

## Recommended Commit / Test Staircase

Use this exact rhythm:

1. One transport fix.
2. Build.
3. Hardware test.
4. Short note with observed result.
5. Only then next fix.

Suggested commit boundaries:

1. STM32 sysex reset + PAT_CHAIN ACK placement.
2. STM32 `uart_processFront()` full drain.
3. AVR `frontPanel_sendData()` deadlock removal.
4. AVR `uart_waitAck()` timeout + ACK/NACK cleanup.
5. `.prf` voice-apply fix.
6. Global read / short-read correctness fixes.
7. First preset-manager hard-lock conversion pass.

## Bottom Line

The worst mistake to repeat is reapplying the Session 002 bundle.

The safest path is:

1. Fix tiny receiver-side STM32 correctness issues first.
2. Fix STM32 RX throughput second.
3. Fix AVR deadlock third.
4. Keep preset/file correctness separate from transport recovery.
5. Postpone broad preset timeout/error-path rewrites until after `.prf` and legacy file behavior are no longer muddying the test results.

That path is slower, but it gives you a meaningful answer after every hardware run.

## Phase 1 Progress Notes

Date: 2026-05-25

Status: implemented in code, fresh rebuild verified with `make clean && make firmware`.

Changes made:

1. In `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`, `SYSEX_START` now explicitly resets:
   - `frontParser_sysexSeqStepNr`
   - `frontParser_rxCnt`
2. In `SYSEX_RECEIVE_PAT_CHAIN_DATA`, the ACK byte is now sent only after the second byte of the pair has been received and applied.

Scope kept intentionally narrow:

- No AVR-side changes.
- No FIFO-drain change yet.
- No timeout/recovery API changes.
- No preset-manager changes.

Expected effect:

- Lower risk of stale sysex session state carrying across a new bulk-transfer session.
- Lower risk of AVR/STM32 desync during pattern-chain transfers caused by premature ACK.

Verification:

1. Incremental build passed with `make firmware`.
2. Fresh rebuild passed with `make clean && make firmware`.

## Wrap-Up / Correction

Date: 2026-05-26

This failed analysis sequence must be treated as unreliable.

Key correction:

1. My earlier Phase 1 conclusion about `SYSEX_RECEIVE_PAT_CHAIN_DATA` ACK placement was wrong for this firmware as it actually exists.
2. The AVR sender in `preset_readPatternChain()` sends `next[i]`, waits for `PATCHAIN_CALLBACK`, then sends `repeat[i]`, and waits again.
3. Moving the STM32 ACK so it only fired after the second byte of the pair broke that handshake and can deadlock `.all` / `.prf` load at the first pattern-chain transfer wait.

Specific code context:

- AVR sender: `front/LxrAvr/Preset/presetManager.c`, `preset_readPatternChain()`
- STM32 receiver: `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`, `SYSEX_RECEIVE_PAT_CHAIN_DATA`

What future readers should assume:

1. Do not trust the earlier PAT_CHAIN “fix” in this document.
2. Do not trust my earlier claim that this Phase 1 change was safe.
3. Do not trust my earlier rollback narrative without verifying the actual source snapshot and actual hardware behavior.
4. Treat every conclusion in this failed branch of analysis as requiring independent source-level and hardware-level confirmation before use.

Plainly:

- My earlier diagnosis contained a real protocol misunderstanding.
- My earlier recommendation led to a regression.
- Future readers should distrust my earlier conclusions here unless they are re-derived directly from the code and validated on hardware.

Safer takeaway:

1. The existing AVR sender behavior must be mapped first.
2. Any proposed ACK-placement change must be checked against the exact sender wait structure before implementation.
3. “Looks like a protocol bug” is not enough in this codebase; the sender/receiver pair must be traced together.

## Phase 1 Reimplementation After Repo Reset

Date: 2026-05-26

Status: reimplemented in code after user reset repository; fresh rebuild verified with `make clean && make firmware`.

Changes reapplied:

1. In `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`, `SYSEX_START` now explicitly resets:
   - `frontParser_sysexSeqStepNr`
   - `frontParser_rxCnt`
2. In `SYSEX_RECEIVE_PAT_CHAIN_DATA`, the ACK byte is now sent only after the second byte of the pair has been received and applied.

Scope:

- STM32 parser only
- no AVR changes
- no FIFO drain change
- no preset-manager changes

Purpose:

- restore the narrow Phase 1 parser correctness fixes on top of the freshly reset repository before any further transport experiments

Verification:

1. Incremental build passed with `make firmware`.
2. Fresh rebuild passed with `make clean && make firmware`.

## Phase 2 Regression Investigation

Date: 2026-05-25

Observed result:

- User hardware test reports `.all` load now freezes with the LCD stuck on `Loading All`.
- Supplied test file `P000.ALL` is a modern v5 file (`version = 0x05`, size `51514` bytes), so this does not match the known legacy-layout compatibility failure mode from `FILEFIX_AUDIT.md`.

Most likely cause:

The Phase 2 change made `uart_processFront()` drain the STM32 front RX FIFO in an unbounded `while(...)` loop. On paper that improves throughput, but this protocol is not robust against burst compression:

1. Several paths in `.all` load send large bursts from AVR to STM32 without per-message ACK pacing.
2. During `.all` load, AVR sets `frontParser_rxDisable=1`, so normal reply traffic from STM32 is not meaningfully consumed; it is only buffered or discarded.
3. Both sides use 256-byte FIFOs and silently ignore `fifo_bufferIn()` failure in critical paths:
   - STM32 front TX path in `mainboard/LxrStm32/src/MIDI/Uart.c`
   - AVR RX ISR path in `front/LxrAvr/IO/uart.c`
4. The loader then waits forever for specific callback bytes (`SYSEX_START`, `SYSEX_END`, `*_CALLBACK`, `CALLBACK_ACK`) with no timeout.

Taken together, the unbounded drain likely compresses STM32-side parsing enough to create a tighter burst of outbound reply/callback traffic than the original pacing allowed. Once either:

- STM32 `fifo_frontTx`, or
- AVR `uart_rxBuffer`

overflows, a required callback byte can be dropped silently, and the AVR loader waits forever on `Loading All`.

Why this is plausible in this codebase:

- `.all` load first pushes globals (`menu_sendAllGlobals()`) while AVR RX parsing is disabled.
- Some of those globals provoke response traffic from STM32, especially `SEQ_SET_ACTIVE_TRACK` handlers that immediately send data back.
- Later bulk load stages depend on exact callback bytes being received without loss.
- The Phase 2 change altered pacing only; file content and parser semantics for the known-good v5 file did not otherwise change.

Most likely freeze class:

- AVR is stuck in one of the existing infinite wait loops in `presetManager.c` waiting for a callback byte that was dropped after FIFO pressure increased.

Proposed fix:

1. Revert the unbounded Phase 2 `while(...)` drain in `mainboard/LxrStm32/src/MIDI/Uart.c`.
2. Treat full-drain as unsafe until observability exists for:
   - STM32 front TX overflow
   - STM32 front RX overflow
   - AVR RX overflow
3. If Phase 2 is revisited later, prefer a bounded drain instead of an unbounded drain:
   - for example, drain a small fixed budget per call (`4`, `8`, or `16` bytes), not the entire FIFO.
4. Longer-term, add explicit overflow counters before retrying throughput changes.

Recommendation:

- Do not stack any further communications changes on top of the current Phase 2 variant.
- Back out Phase 2 first, re-test the same known-good `.all` file, and only then decide whether to attempt a bounded-drain variant.

## Phase 2 Rollback Notes

Date: 2026-05-26

Action taken:

1. Reverted the Phase 2 `uart_processFront()` full-drain change in `mainboard/LxrStm32/src/MIDI/Uart.c`.

What remains in place:

1. Phase 1 `SYSEX_START` explicit reset of:
   - `frontParser_sysexSeqStepNr`
   - `frontParser_rxCnt`
2. Phase 1 PAT_CHAIN ACK placement fix.

Reason for rollback:

- Hardware regression on known-good `.all` load strongly suggests that the unbounded full-drain behavior changed pacing enough to trigger callback loss through shared lossy FIFO paths.
- Reverting Phase 2 restores the pre-change service rate while preserving the narrower Phase 1 correctness fixes.

## Phase 2 Progress Notes

Date: 2026-05-25

Status: implemented in code, fresh rebuild verified with `make clean && make firmware`.

Changes made:

1. In `mainboard/LxrStm32/src/MIDI/Uart.c`, `uart_processFront()` now drains the entire front-panel RX FIFO with `while(...)` instead of processing only one byte per service call.

Scope kept intentionally narrow:

- No AVR-side changes.
- No new diagnostics counters yet.
- No parser logic changes beyond existing Phase 1 edits.
- No preset-manager or file-loader changes.

Expected effect:

- Lower backlog pressure on the STM32 front RX FIFO during bulk transfers.
- Lower probability of software FIFO accumulation during load/save traffic, especially while playback or DSP work is active.

Verification:

1. Incremental build passed with `make firmware`.
2. Fresh rebuild passed with `make clean && make firmware`.
