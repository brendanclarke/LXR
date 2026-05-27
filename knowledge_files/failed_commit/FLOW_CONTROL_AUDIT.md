# FLOW_CONTROL_AUDIT

Date: 2026-05-26

## Purpose

This document proposes an explicit flow-control design for the ATmega644 front-panel MCU and STM32F4 mainboard MCU UART link.

Goals:

1. Revert the risky Phase 2 full-drain change.
2. Keep the useful Phase 1 parser fixes.
3. Replace implicit timing-based behavior with explicit, receiver-driven metering.
4. Protect critical callback/ACK traffic from being crowded out by low-priority UI chatter.

This proposal is intentionally incremental. It is not a full protocol rewrite with framing/checksum/versioning. It is a least-delta plan to make current preset load/save pathways more robust.

## Current State

## What the protocol does today

There are two broad traffic classes:

1. Normal 3-byte status/data/data messages.
2. Ad-hoc SysEx-like bulk sessions framed by `SYSEX_START` / `SYSEX_END`.

The current implementation mixes several pacing styles:

- No pacing at all for many normal 3-byte messages.
- Item-by-item ACK pacing for some SysEx bulk writes.
- Request/response pacing for some AVR readback/save paths.
- One-off `CALLBACK_ACK` flush behavior for `frontPanel_holdForBuffer()`.

This is not true flow control. It is a collection of local waits with no unified notion of receiver capacity.

## Why the current design is fragile

Both MCUs silently drop bytes on FIFO overflow:

- STM32 front RX enqueue ignores failure in [Uart.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/MIDI/Uart.c:105)
- STM32 front TX enqueue ignores failure in [Uart.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/MIDI/Uart.c:176) and [Uart.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/MIDI/Uart.c:186)
- AVR RX enqueue ignores failure in [uart.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/IO/uart.c:76)

On top of that, AVR preset paths wait forever for specific bytes with no timeout:

- `SYSEX_START`
- `SYSEX_END`
- `MAINSTEP_CALLBACK`
- `PATCHAIN_CALLBACK`
- `LENGTH_CALLBACK`
- `SCALE_CALLBACK`
- `STEP_ACK`
- `STEP_CALLBACK`
- `CALLBACK_ACK`

Representative wait sites are in [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c:1069), [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c:1387), [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c:1523), and [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c:1653).

One dropped callback byte can therefore deadlock the user-visible load operation.

## Risky Data Pathways

## 1. AVR -> STM32 globals burst during `.all` / `.prf` load

Source:

- `menu_sendAllGlobals()` in [menu.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Menu/menu.c:3343)

Risk:

- This sends many normal 3-byte `SEQ_CC` messages with no receiver credit limit.
- Several of those commands provoke immediate reply traffic from STM32.
- During `.all` / `.prf` load, AVR sets `frontParser_rxDisable=1`, so normal response traffic is not meaningfully processed during the burst.

Examples of reply-generating globals:

- `SEQ_SET_ACTIVE_TRACK` leads STM32 to send track rotation / transpose updates in [frontPanelParser.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/MIDI/frontPanelParser.c:1323)
- some LED/query style flows also send data back immediately

This is the clearest example of one direction producing a burst while the reverse direction is effectively unmanaged.

## 2. AVR -> STM32 voice parameter bursts during kit / all / perf load

Source:

- `preset_readDrumVoice()` in [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c:429)

Risk:

- After `SEQ_LOAD_VOICE`, AVR sends many `MIDI_CC`, `CC_2`, `CC_LFO_TARGET`, `CC_VELO_TARGET`, and macro messages.
- These are not individually credit-metered.
- STM32 caches them while `seq_voicesLoading` is set, but that does not meter UART ingress.

## 3. AVR -> STM32 pattern metadata SysEx writes

Sources:

- pattern scale: [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c:1013)
- pattern length: [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c:1135)
- pattern chain: [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c:1331)
- main step data: [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c:1466)

Risk:

- These do use item-by-item callbacks, which is better than raw blasting.
- But the callback byte itself is unprotected.
- If low-priority reverse traffic shares the same FIFO and a callback byte is dropped, the wait loop still deadlocks.

## 4. AVR -> STM32 step data transfer

Source:

- [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c:1593)

Risk:

- This is the most disciplined existing path: one step record, then `STEP_ACK`.
- Even here, reliability still depends on callback bytes surviving shared FIFO pressure.

## 5. STM32 -> AVR readback/save data

Sources:

- `SYSEX_REQUEST_STEP_DATA`
- `SYSEX_REQUEST_MAIN_STEP_DATA`
- `SYSEX_REQUEST_PATTERN_DATA`

Handled in [frontPanelParser.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/MIDI/frontPanelParser.c:371)

Risk:

- This direction is already closer to explicit flow control because AVR requests an item and STM32 responds.
- The remaining danger is interference from unrelated UI chatter on the same return path.

## 6. Background STM32 -> AVR UI chatter

Sources include:

- sequencer LED/status traffic in [sequencer.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/Sequencer/sequencer.c)
- immediate front-panel replies in [frontPanelParser.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/MIDI/frontPanelParser.c:1034)

Risk:

- This traffic is low-priority during preset bulk load.
- Today it shares the same TX FIFO as critical callback bytes.
- That means “nice to have” UI messages can crowd out “must arrive” callback bytes.

## Why Phase 2 Should Be Reverted

The current Phase 2 change makes STM32 drain the entire front RX FIFO in one call:

- [Uart.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/MIDI/Uart.c:152)

That improved throughput in theory, but it also compressed message handling into tighter bursts.

In this protocol, tighter bursts are dangerous because:

1. TX FIFO writes are still best-effort with silent drop.
2. Callback bytes still share the same pipe as low-priority replies.
3. AVR still waits forever when a callback disappears.

So Phase 2 should be backed out as part of the explicit-flow-control work.

Recommended rollback:

- Restore `uart_processFront()` to its pre-Phase-2 single-byte behavior first.
- Keep the Phase 1 parser fixes in place:
  - explicit `SYSEX_START` reset
  - corrected PAT_CHAIN ACK placement

## Proposed Explicit Flow Control Design

## Design principles

1. Receiver-driven, not sender-assumed.
2. Meter logical items, not raw bytes.
3. Separate critical callback traffic from optional UI chatter.
4. Reuse current preset loops where possible.
5. Minimal blast radius: no checksum/framing redesign in this pass.

## High-level model

Use two different strategies, because the two directions are not symmetric:

1. **AVR -> STM32 bulk writes**:
   add explicit credit grants from STM32.
2. **STM32 -> AVR bulk readback/save**:
   keep AVR request/response pacing, but silence unrelated return traffic during the session.

That gives robust metering without rewriting every save path from scratch.

## New protocol layer

Reserve new `SEQ_CC` subcommands for flow control.

Suggested values:

```c
#define SEQ_FLOW_BEGIN   0x5a
#define SEQ_FLOW_GRANT   0x5b
#define SEQ_FLOW_END     0x5c
#define SEQ_FLOW_ABORT   0x5d
```

Suggested channel ids:

```c
#define FLOW_CH_GLOBALS      0x01
#define FLOW_CH_VOICE_PARAM  0x02
#define FLOW_CH_PAT_SCALE    0x03
#define FLOW_CH_PAT_LENGTH   0x04
#define FLOW_CH_PAT_CHAIN    0x05
#define FLOW_CH_MAINSTEP     0x06
#define FLOW_CH_STEPDATA     0x07
#define FLOW_CH_READBACK     0x08
```

Meaning:

- `SEQ_FLOW_BEGIN`: start managed transfer for one channel
- `SEQ_FLOW_GRANT`: receiver grants N logical items
- `SEQ_FLOW_END`: sender declares channel complete
- `SEQ_FLOW_ABORT`: either side aborts and clears channel state

## Item units

Credits should represent one logical application item, not arbitrary byte counts.

Examples:

- `FLOW_CH_GLOBALS`: 1 credit = 1 normal 3-byte global message
- `FLOW_CH_VOICE_PARAM`: 1 credit = 1 parameter/target message
- `FLOW_CH_PAT_SCALE`: 1 credit = 1 scale item
- `FLOW_CH_PAT_LENGTH`: 1 credit = 1 length item
- `FLOW_CH_PAT_CHAIN`: 1 credit = 1 complete `(next, repeat)` pair
- `FLOW_CH_MAINSTEP`: 1 credit = 1 main-step tuple
- `FLOW_CH_STEPDATA`: 1 credit = 1 step record

This matches how the existing preset loops are already structured.

## STM32 responsibilities

When STM32 receives `SEQ_FLOW_BEGIN(channel)`:

1. Enter `comm_bulkMode = 1`.
2. Mark the active `comm_flowChannel`.
3. Suppress nonessential `uart_sendFrontpanelByte()` UI chatter while the session is active.
4. Send `SEQ_FLOW_GRANT(channel, initialCredits)`.

During the session:

1. Consume incoming items for that channel.
2. After each item is fully applied or safely cached, decrement internal pending count.
3. When enough capacity is free, send another `SEQ_FLOW_GRANT`.

When STM32 receives `SEQ_FLOW_END(channel)`:

1. Clear channel state.
2. Exit quiet mode if no other bulk session is active.

Critical note:

`SEQ_FLOW_GRANT`, `SYSEX_START`, `SYSEX_END`, and existing per-item callback/ACK bytes should be treated as **priority traffic**.

## AVR responsibilities

Before a bulk write burst:

1. Send `SEQ_FLOW_BEGIN(channel)`.
2. Wait for `SEQ_FLOW_GRANT(channel, N)`.
3. Send up to `N` items only.
4. When credits reach zero, wait for more grant.
5. After the channel completes, send `SEQ_FLOW_END(channel)`.

This turns the current implicit “send and hope” behavior into explicit receiver-controlled pacing.

## Reverse direction handling

For STM32 -> AVR bulk readback/save:

Do not invent a second full credit system unless testing proves it is needed.

Instead:

1. AVR remains the requester.
2. Each request gets one response item.
3. While `FLOW_CH_READBACK` is active, STM32 suppresses unrelated UI chatter.
4. AVR exits the session with `SEQ_FLOW_END(FLOW_CH_READBACK)`.

That is already close to receiver pacing, and it avoids a larger redesign.

## Priority and quiet mode

Explicit credits alone are not enough if low-priority STM32 reply traffic can still fill the TX FIFO.

So the proposal requires a second rule:

During a managed bulk session:

1. Suppress best-effort UI traffic from:
   - LED updates
   - track rotation echoes
   - step parameter echoes
   - pattern parameter echoes
2. Allow only:
   - flow-control messages
   - existing required callback bytes
   - session-closing messages

This is the minimum needed to keep callback/grant traffic meaningful.

## Implementation shape

## Step 1: Revert Phase 2

Change:

- restore single-byte `uart_processFront()` behavior in [Uart.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/MIDI/Uart.c:152)

Keep:

- Phase 1 parser fixes

## Step 2: Add comm state

On STM32:

```c
volatile uint8_t comm_bulkMode;
volatile uint8_t comm_flowChannel;
volatile uint8_t comm_rxCredits;
volatile uint8_t comm_quietUi;
```

On AVR:

```c
volatile uint8_t comm_flowChannel;
volatile uint8_t comm_txCredits;
volatile uint8_t comm_flowActive;
```

## Step 3: Add flow-control message handling

Implement `SEQ_FLOW_BEGIN/GRANT/END/ABORT` in:

- AVR parser/send side
- STM32 `frontParser_handleSeqCC()`

## Step 4: Convert highest-risk write paths first

Convert in this order:

1. `menu_sendAllGlobals()`
2. `preset_readDrumVoice()`
3. `preset_readPatternMainStep()`
4. `preset_readPatternLength()`
5. `preset_readPatternScale()`
6. `preset_readPatternChain()`
7. `preset_readPatternStepData()`

Rationale:

- `menu_sendAllGlobals()` is the clearest uncontrolled burst during `.all`.
- voice param bursts are next.
- step data already has per-step ACK pacing and is less urgent than globals.

## Step 5: Add quiet-mode suppression

Gate nonessential STM32 -> AVR sends in:

- `uart_sendFrontpanelByte()`
- or at selected call sites in sequencer/front parser

Rule:

- If `comm_quietUi` is active, drop or defer best-effort UI messages.
- Do not block:
  - flow-control bytes
  - required SysEx callbacks
  - `CALLBACK_ACK`
  - sample-upload ACK path if still needed

## Step 6: Only then reconsider throughput changes

After explicit flow control and quiet mode exist:

1. Re-test with original single-byte STM32 drain.
2. If more throughput is still needed, try a bounded drain.
3. Do not retry unbounded full-drain until proven safe under the new metering layer.

## Why this is stronger than bounded drain

Bounded drain is only a pacing heuristic.

This proposal is stronger because it:

1. Lets the receiver decide how many items may be sent.
2. Separates critical callback/grant traffic from optional UI chatter.
3. Provides a defined session lifecycle.
4. Reduces dependence on accidental main-loop timing.

It addresses the actual problem:

- not merely “too many bytes processed”
- but “too many unscheduled, mixed-priority messages sharing a lossy FIFO path”

## Residual limitations

This still does not solve:

1. Silent overflow outside managed sessions.
2. Missing timeouts on AVR waits.
3. Lack of framing/checksum.
4. `CALLBACK_ACK` semantics that currently clear both STM32 FIFOs.

So this proposal should be considered a strong stabilization layer, not the final end-state protocol.

## Recommended Next Implementation Order

1. Revert Phase 2 only.
2. Implement `SEQ_FLOW_BEGIN/GRANT/END/ABORT`.
3. Add STM32 quiet mode during managed sessions.
4. Convert `menu_sendAllGlobals()` to explicit credits.
5. Convert voice parameter bursts.
6. Convert metadata SysEx loops.
7. Convert step-data path last, only if needed.
8. Add AVR wait timeouts after flow control is working.

## Phased Implementation Plan

This section turns the design into a practical implementation order with narrow checkpoints.

### Phase A: Stabilize Baseline

Code changes:

1. Revert Phase 2 full-drain in `uart_processFront()`.
2. Keep Phase 1 parser fixes.

Success criteria:

1. Known-good `.all` load succeeds again.
2. Known-good `.prf` load still succeeds.
3. No new regressions in save/load/save smoke tests.

Do not add any new protocol behavior in this phase.

### Phase B: Add Flow-Control Opcodes And Session State

Code changes:

1. Add `SEQ_FLOW_BEGIN`, `SEQ_FLOW_GRANT`, `SEQ_FLOW_END`, `SEQ_FLOW_ABORT` on both MCUs.
2. Add minimal session state:
   - STM32: `comm_bulkMode`, `comm_flowChannel`, `comm_quietUi`
   - AVR: `comm_flowActive`, `comm_flowChannel`, `comm_txCredits`
3. Parse and ignore these safely before converting any existing data path.

Success criteria:

1. Firmware builds.
2. Existing load/save behavior remains unchanged because no path uses the new opcodes yet.
3. Manual spot-test confirms the new opcodes do not interfere with normal menu traffic.

### Phase C: Add STM32 Quiet Mode

Code changes:

1. Add a guard so nonessential `uart_sendFrontpanelByte()` traffic is suppressed while `comm_quietUi` is active.
2. Do not suppress:
   - `SEQ_FLOW_GRANT`
   - existing SysEx callbacks/ACKs
   - `CALLBACK_ACK`
   - `SYSEX_START`
   - `SYSEX_END`

Success criteria:

1. Normal operation outside bulk sessions is unchanged.
2. During a synthetic managed session, low-priority UI chatter is silenced.

Reason for doing this early:

- Quiet mode protects the return path before we start depending on grants.

### Phase D: Convert `menu_sendAllGlobals()`

Code changes:

1. Wrap the globals burst in `FLOW_CH_GLOBALS`.
2. AVR sends:
   - `SEQ_FLOW_BEGIN(FLOW_CH_GLOBALS)`
   - waits for grant
   - sends only granted count of global messages
   - repeats until done
   - sends `SEQ_FLOW_END(FLOW_CH_GLOBALS)`
3. STM32 grants credits in small logical units, for example `4` or `8` globals at a time.

Success criteria:

1. Known-good `.all` load succeeds repeatedly.
2. Known-good `.prf` load succeeds repeatedly.
3. Regression case from Phase 2 does not reappear.

Reason this is first:

- It is the clearest uncontrolled burst and likely the biggest contributor to callback starvation during `.all`.

### Phase E: Convert Voice Parameter Burst

Code changes:

1. Wrap `preset_readDrumVoice()` send path in `FLOW_CH_VOICE_PARAM`.
2. One credit equals one outgoing parameter/target message.
3. Keep current voice cache semantics unchanged on STM32.

Success criteria:

1. Kit load inside `.all` and `.prf` remains correct.
2. No voice-update stalls.
3. No increased load time outliers from voice-param sections.

### Phase F: Convert Metadata SysEx Writers

Code changes:

1. Convert:
   - `FLOW_CH_MAINSTEP`
   - `FLOW_CH_PAT_LENGTH`
   - `FLOW_CH_PAT_SCALE`
   - `FLOW_CH_PAT_CHAIN`
2. Preserve existing per-item callbacks for now.
3. Use credits mainly as session-level metering and quiet-mode activation, not as a replacement for every existing ACK immediately.

Success criteria:

1. `.all` and `.prf` metadata loads succeed.
2. Pattern-chain and mainstep loads remain synchronized.
3. No “Loading All” hangs.

Why preserve existing callbacks initially:

- Least-delta migration.
- Reduces the amount of simultaneously changing behavior.

### Phase G: Decide Whether Step Data Needs Conversion

Code changes:

1. Evaluate whether `preset_readPatternStepData()` needs `FLOW_CH_STEPDATA`.
2. If converted, keep `STEP_ACK` and `STEP_CALLBACK` initially.
3. Use flow control mainly to manage session quiet mode and channel ownership.

Success criteria:

1. Step-data heavy loads remain correct.
2. No slowdown or deadlock regression.

Reason to defer:

- This path already has the strongest existing pacing.
- It is not the best first place to add protocol churn.

### Phase H: Add Timeouts After Flow Control Is Stable

Code changes:

1. Add AVR timeouts to waits for:
   - `SYSEX_START`
   - `SYSEX_END`
   - `*_CALLBACK`
   - `STEP_ACK`
   - `CALLBACK_ACK`
2. On timeout, send `SEQ_FLOW_ABORT` if a managed session is active.

Success criteria:

1. Stuck transfers become recoverable.
2. LCD can surface failure instead of hard hanging forever.

Reason to defer:

- Flow control should reduce loss first.
- Timeout handling is easier to reason about once session ownership exists.

## Suggested Test Gates Between Phases

After each phase:

1. `make firmware`
2. Known-good `.all` load
3. Known-good `.prf` load
4. Save -> load -> save cycle
5. Playback-active load of `.all`

After Phases D, E, and F specifically:

1. Back-to-back preset loads without power cycle
2. Rapid UI interaction during load
3. Pattern change immediately after load

## Bottom Line

The safest robust approach is:

1. Undo the unbounded full-drain optimization.
2. Keep the Phase 1 correctness fixes.
3. Add receiver-driven credits for AVR -> STM32 bulk writes.
4. Keep STM32 -> AVR bulk readback request-driven, but silence unrelated chatter during the session.
5. Protect callback/grant traffic as first-class protocol events, not just ordinary bytes mixed into a best-effort FIFO.

That gives a meaningful explicit flow-control layer with much less churn than a full protocol redesign.
