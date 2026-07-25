# Session Plan: MIDI-Clock Latency and Jitter Reduction

## Scope and decision

This session implements the following two changes, irrespective of the next
audio measurement:

1. Put MIDI system-realtime bytes (`F8`, `FA`, `FB`, `FC`) on a dedicated,
   bounded receive lane at the USART2 receive boundary.
2. Dispatch that lane immediately before the main loop renders a newly freed
   audio DMA buffer.

This document is a plan only.  No firmware source is changed by the creation
of this file.  Append implementation notes, build results, and the user's
post-change audio-test result to the progress log at the end; do not replace
the design record below.

## Problem statement

The existing DIN MIDI path is:

```text
DIN MIDI byte
  -> USART2_IRQHandler: append every byte to fifo_midiRx
  -> later main-loop uart_processMidi(): remove one byte
  -> midiParser_parseUartData(F8): seq_sync()
  -> sync_tick(): seq_resetDeltaAndTick() / seq_nextStep()
  -> later main-loop audio render
  -> ping-pong DMA + I2S/DAC output
```

`USART2_IRQHandler()` intentionally does no parsing, but `uart_processMidi()`
removes only one ordinary MIDI byte per main-loop pass.  The byte can therefore
wait behind unrelated main-loop work.  The clock-triggered voice state can also
miss the audio buffer that is about to be rendered, adding another variable
block interval.  These are software scheduling sources of jitter independent
of the fixed serial, DMA, I2S, and DAC delay.

The plan does **not** attempt to make audio occur before a received MIDI clock,
does not add predictive clocking, and does not alter audio block size.  Those
are separate decisions after the new timing baseline is measured.

## Invariants and non-goals

- Keep all DSP, float arithmetic, sequencer stepping, routing, and MIDI TX out
  of `USART2_IRQHandler()`.
- Preserve MIDI running-status correctness: system-realtime messages may occur
  between any channel-message bytes, so removing them from the ordinary parser
  stream must not change its state.
- Preserve the existing receive-filter condition
  `midiParser_txRxFilter & 0x02` and external-sync condition
  `seq_getExtSync()` for Start/Continue/Stop/Clock.
- Preserve the existing DIN-MIDI realtime routing semantics to MIDI/USB output.
- Leave USB MIDI on its existing `MidiMsg` path.  This session targets the DIN
  USART latency shown by the code audit.
- Do not change the front-panel RX drain.  An unbounded front RX drain is
  presently known to be unsafe if reintroduced or expanded blindly.
- Do not use a shared counter that coalesces clocks: every received clock byte
  must remain an individually ordered event.
- Do not allow a full realtime queue to overwrite an older event silently.
  Record overflow explicitly for diagnosis; normal operation has vastly more
  than enough queue headroom at MIDI-clock rates.

## Detailed code-change plan

### Change 1 — Add a DIN realtime receive queue

**Files:**

- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.h`

**Implementation:**

Add a small private single-producer/single-consumer ring in `Uart.c`, for
example eight or sixteen `uint8_t` slots with volatile read/write indices and a
volatile overflow counter.  The producer is `USART2_IRQHandler()` only; the
consumer is the STM main loop only.  The ring is separate from `fifo_midiRx`.

In the USART2 RXNE branch, after reading USART2 data:

- if `(data & 0xf8) == 0xf8`, append the byte to the realtime ring;
- otherwise retain the current `fifo_bufferIn(&fifo_midiRx, data)` behaviour.

The realtime class deliberately includes Active Sensing and Reset as well as
Clock, Start/Continue, and Stop.  The dispatcher will currently act on only
the same messages that the old parser acts on, while preserving routing for all
realtime bytes.  This makes classification protocol-correct and avoids an
ordinary FIFO ordering hazard when an otherwise ignored realtime status occurs
between channel bytes.

Do not call `seq_sync()`, `sync_midiStartStop()`, `uart_sendMidi()`,
`usb_sendMidi()`, or any parser helper from the ISR.

**Comment text to place beside the queue state:**

```c
/* DIN system-realtime SPSC queue.  USART2 RX is its sole producer and the
   main-loop audio-deadline service is its sole consumer.  Realtime status
   bytes may legally interrupt MIDI running status, so keeping them out of the
   ordinary byte FIFO cannot alter channel-message assembly.  The queue holds
   complete one-byte events in arrival order; overflow is counted rather than
   discarding or coalescing a clock silently. */
```

**Comment text to place beside the ISR classification:**

```c
/* Keep the USART IRQ bounded: classify and enqueue only.  Clock transport is
   prioritized here so USB/front-panel/main-loop work cannot delay its capture,
   but sequencer state and MIDI routing remain main-loop work and therefore
   never execute at USART interrupt priority. */
```

**Inputs:** raw received USART2 byte, RXNE interrupt context.

**Outputs:** one ordered byte in either the realtime queue or the existing
ordinary MIDI FIFO; an incremented diagnostic overflow counter if the
realtime queue is full.

**Affiliates:** `USART2_IRQHandler()`, `fifo_midiRx`, `fifo_bufferIn()`,
`MidiMessages.h` realtime constants, Cortex-M volatile SPSC access rules.

**Acceptance checks:** realtime bytes do not enter `fifo_midiRx`; ordinary
channel, SysEx, and MTC bytes still do; FIFO-full handling leaves the oldest
realtime events intact and makes loss observable.

### Change 2 — Extract the main-loop realtime dispatcher from the byte parser

**Files:**

- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiParser.h`

**Implementation:**

Extract the current `(data & 0xf8) == 0xf8` branch of
`midiParser_parseUartData()` into one exported main-loop-only helper, proposed
name:

```c
void midiParser_handleDinRealtime(uint8_t data);
```

It must retain the exact old effects:

- `MIDI_START` and `MIDI_CONTINUE` call `sync_midiStartStop(1)` only when DIN
  receive is enabled and external sync is active.
- `MIDI_STOP` calls `sync_midiStartStop(0)` under the same condition.
- `MIDI_CLOCK` calls `seq_sync()` under the same condition.
- Routing to DIN MIDI and/or USB is retained for every realtime byte exactly
  as the existing fall-through branch does.

After extraction, retain a compatibility call from
`midiParser_parseUartData()` when it is passed a realtime byte.  This makes the
function correct for direct callers and protects against future paths that may
still send a raw realtime byte to it.  Under the new USART2 path that branch is
normally not reached, because the UART ISR routes realtime to its dedicated
queue.

**Comment text to place beside the exported declaration/definition:**

```c
/* Consume one DIN system-realtime byte in main-loop context.  Input is a
   complete status byte captured by the USART realtime queue; output is the
   same external-sync transport action and optional DIN/USB forwarding that
   the legacy raw-byte parser performed.  This helper intentionally owns no
   queue state and performs no timing capture, allowing the UART transport to
   prioritize arrival without moving sequencer or TX work into an ISR. */
```

**Inputs:** one complete realtime MIDI status byte in main-loop context.

**Outputs:** sequencer start/stop/clock action when enabled, and optional
routed MIDI/USB message; no parser running-status mutation.

**Affiliates:** `midiParser_txRxFilter`, `seq_getExtSync()`, `seq_sync()`,
`sync_midiStartStop()`, `uart_sendMidi()`, `usb_sendMidi()`,
`globalMidiParser_handleSystemMessage()` for the separate USB `MidiMsg` path.

**Acceptance checks:** DIN Start/Continue/Stop/Clock behavior and MIDI/USB
thru routing remain unchanged; USB system messages continue to be handled by
`globalMidiParser_handleSystemMessage()` and are not accidentally dispatched
twice.

### Change 3 — Add a bounded realtime-queue service API

**Files:**

- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.h`

**Implementation:**

Add an exported service function, proposed name:

```c
void uart_serviceMidiRealtime(void);
```

It drains the realtime SPSC queue in FIFO order and passes each byte to
`midiParser_handleDinRealtime()`.  Its queue-drain bound must be explicit.  A
small fixed maximum equal to the queue capacity is acceptable because MIDI
realtime bytes are sparse; alternatively, process until empty with a capacity
that is compile-time small.  It must never inspect or drain `fifo_midiRx`.

Place the queue-drain implementation outside the ISR.  The main loop thus
gets the exact old sequencer and routing behavior, but at an audio-aware
service point instead of an arbitrary ordinary-MIDI service point.

**Comment text to place beside the API:**

```c
/* Drain captured DIN realtime events before an audio render.  Input is the
   bounded SPSC queue written by USART2 RX; output is ordered calls to the
   MIDI realtime dispatcher.  This service deliberately excludes ordinary
   MIDI bytes, preventing parameter/SysEx bursts from extending the
   clock-to-render critical section. */
```

**Inputs:** queued realtime bytes captured by `USART2_IRQHandler()`.

**Outputs:** ordered main-loop realtime dispatch; empty or reduced realtime
queue.

**Affiliates:** realtime queue state, `midiParser_handleDinRealtime()`,
`uart_processMidi()` (which remains ordinary-MIDI-only), audio render ordering
in `main.c`.

**Acceptance checks:** back-to-back clock events remain ordered; normal MIDI
is not consumed by this API; this function remains bounded and contains no
front-panel or DSP render work.

### Change 4 — Dispatch realtime events at the audio-render deadline

**Files:**

- `mainboard/LxrStm32/src/main.c`

**Implementation:**

Replace the two duplicated `if (bCurrentSampleValid != SAMPLE_VALID)` render
sites with one small local helper, proposed name:

```c
static inline void serviceAudioRenderDeadline(void);
```

The helper must:

1. Return immediately if no DMA buffer needs rendering.
2. Call `uart_serviceMidiRealtime()` while the newly freed buffer is still
   available.
3. Call the existing `calcNextSampleBlock()` exactly once.

Call this helper first in the main loop, before `usb_tick()`, and again after
incoming ordinary/USB/front-panel work.  Do not otherwise reorder ordinary MIDI
parsing, front-panel parsing, background-swap service, USB MIDI parsing,
`seq_tick()`, trigger handling, or Preset service in this session.  Moving the
first render check ahead of `usb_tick()` is intentional: once DMA has released
a buffer, no unrelated USB work may delay the realtime dispatch/render pair.
This remains a narrow audio-deadline ordering change, not a main-loop rewrite.

The ordering is intentionally `realtime dispatch -> render`, not the reverse.
When external clock causes `seq_sync()` to call `seq_nextStep()`, its voice
trigger state is therefore visible to the exact buffer being rendered.  A
clock that arrives after the helper has started rendering safely waits for the
next DMA opportunity; it never races the DMA-owned buffer.

**Comment text to place beside the helper:**

```c
/* Render only at a DMA-owned buffer deadline.  DIN realtime is dispatched
   immediately before rendering so a received external clock can update voice
   trigger state for this newly free buffer rather than waiting behind ordinary
   MIDI, front-panel parsing, or the next render opportunity.  Do not add
   unbounded protocol parsing here: this is the audio timing critical path. */
```

**Inputs:** `bCurrentSampleValid` written by the high-priority DMA ISR and any
captured DIN realtime events.

**Outputs:** one freshly rendered audio buffer containing all eligible
external-clock trigger state; `bCurrentSampleValid` returns to `SAMPLE_VALID`.

**Affiliates:** `calcNextSampleBlock()`, `bCurrentSampleValid`,
`DMA1_Stream7_IRQHandler()`, `uart_serviceMidiRealtime()`, `seq_sync()`,
`seq_resetDeltaAndTick()`, `seq_nextStep()`.

**Acceptance checks:** no audio buffer is rendered twice; no invalid buffer is
left unrendered; external-clock steps continue to occur only with the original
filter and external-sync conditions; no blocking or unbounded parser work is
introduced into the render deadline.

### Change 5 — Update the authoritative timing documentation

**Files:**

- `MEMORY.md`
- `knowledge_files/log_archive/XXX_SESSION_HANDOFF_LOG.md` (new session
  number selected when implementation is completed)
- `knowledge_files/hardware_archive/main/STM32F4_SETUP_ALLOCATION.md`

**Implementation:**

Record the new receive/dispatch path and the deliberate ownership split:
USART2 IRQ captures system-realtime bytes; the main loop dispatches them only
at an audio render deadline.  Correct the audio-block statement to reflect
the active `DMA_MODE_ACTIVE` override: the compiled `OUTPUT_DMA_SIZE` is 16,
not the initial 32 definition.  The planning document remains the running
implementation log until closeout, after which durable findings belong in the
session handoff and the root plan can be retained or removed by explicit user
direction.

**Comment/documentation text:**

```text
DIN MIDI realtime is captured separately at USART2 RX and dispatched in main
context immediately before a newly freed audio DMA buffer is rendered.  This
protects clock-event ordering and reduces cooperative-loop jitter without
running sequencer/DSP work in an ISR.  The active DMA mode renders 16 frames
per block at approximately 44.003 kHz (about 364 us per block).
```

**Affiliates:** `config.h`, `AudioCodecManager.h`, `Uart.c`, `MidiParser.c`,
`main.c`, the existing audio/IRQ audit.

## Implementation sequence

1. Reconfirm the worktree is clean except for this plan and record its status
   below.
2. Implement Changes 1–3 together; compiling an intermediate state where the
   ISR diverts realtime but no main-loop service exists is not acceptable.
3. Implement Change 4 and review the two render call sites to ensure both use
   the same helper.
4. Inspect the diff for ISR safety, volatile SPSC correctness, routing parity,
   accidental USB-path changes, and duplicate dispatch.
5. Build STM32, then run the top-level firmware build.
6. Hardware smoke test: external MIDI Start/Stop/Continue, DIN clock sync,
   MIDI thru/USB routing when enabled, ordinary channel-note input, and a
   front-panel/file-load regression check.
7. Repeat the dual-waveform timing capture with the same source, kit, output
   routing, sample rate, and onset measurement method.  Record mean, minimum,
   maximum, and peak-to-peak offset for at least 100 hits.
8. Update the durable session log and `MEMORY.md` only after the change and
   hardware test are complete.

## Risks and review checklist

- **Queue overflow:** diagnostic counter required; do not silently drop the
  newest or overwrite the oldest clock.
- **IRQ/main concurrency:** queue indices must be single-byte volatile values
  on this target; producer and consumer may only write their own index.
- **Order:** Start/Continue/Stop and Clock must preserve arrival order within
  the realtime queue.
- **Routing:** the extracted helper must retain the old realtime forwarding
  behavior, including bytes with no sequencer action.
- **Parser state:** ordinary running-status state must never see or require a
  realtime byte.
- **Audio:** the deadline helper must not call USB, front-panel, endpoint
  restore, morph interpolation, or any blocking send/wait function.
- **Scope:** do not change IRQ priorities, DMA block size, predictive clocking,
  or front-panel drain policy in this implementation.

## Progress log

### 2026-07-25 — plan created

- Scope approved by user: implement the dedicated realtime MIDI receive lane
  and audio-deadline dispatch after planning; no firmware code changes in this
  planning step.
- Investigation basis recorded: USART2 currently queues all bytes, ordinary
  MIDI service removes one byte per loop, and audio rendering currently occurs
  before/after unrelated main-loop services without a realtime dispatch point.
- Worktree status at plan creation: unrelated pre-existing entries
  `.DS_Store` (modified) and `GLOTMP.CFG` (untracked) were present; this plan
  does not alter either file.  `SESSION_LOG_LATENCY_FIX.md` is the only file
  created for this planning request.

### 2026-07-25 — implementation in progress

- Added the planned 16-slot USART2 single-producer/single-consumer realtime
  queue, explicit overflow counter, and main-loop queue-service API in the
  STM UART transport.
- Extracted the DIN realtime behavior from the raw-byte parser into
  `midiParser_handleDinRealtime()`.  The raw parser retains a compatibility
  call for any direct caller; USART2 no longer feeds realtime bytes through
  the ordinary FIFO.
- Added `serviceAudioRenderDeadline()` in `main.c`; it dispatches captured
  realtime events before rendering a DMA-freed buffer.  The first service is
  intentionally at the top of the loop, before `usb_tick()`, so USB work
  cannot delay a ready audio buffer.  The second service remains after normal
  input work to catch a deadline reached during that work.
- No IRQ priority, DMA size, front-panel drain policy, predictive clocking, or
  DSP algorithm was changed.  Source review and build verification remain
  pending.

### 2026-07-25 — implementation and build verification complete

- Reviewed the final path: USART2 captures only realtime status bytes into the
  new 16-slot queue; `uart_processMidi()` remains the one-byte ordinary MIDI
  parser service; queue dispatch happens only inside the DMA render-deadline
  helper.
- Confirmed the realtime helper preserves the former DIN behavior: Start and
  Continue arm playback, Stop stops it, Clock calls `seq_sync()` when DIN RX
  and external sync are enabled, and enabled MIDI/USB forwarding remains
  main-loop work.  USB MIDI remains on `globalMidiParser_handleSystemMessage()`
  and is not double-dispatched.
- `git diff --check` passed.
- `make -C mainboard/LxrStm32 -j4 stm32` passed.  It emitted only the
  repository's pre-existing warning classes (legacy inline intrinsics,
  duplicate `const`, known sequencer bounds warnings, recursive `_exit`, and
  linker RWX segment warning); no warning was introduced by this change.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN` from the
  verified STM32 binary and the existing AVR binary.
- Hardware verification remains required: DIN Start/Continue/Stop and clock
  sync; ordinary DIN notes and running status; enabled DIN-to-MIDI/USB routing;
  a front-panel/file-load regression check; then the requested same-method
  dual-waveform timing capture.  Do not claim timing improvement until that
  capture is complete.

### 2026-07-25 — post-hardware-test path audit

- User reports very little waveform difference.  Source and linked-ELF audit
  confirms that the DIN path is active in the built STM32 image:
  `USART2_IRQHandler`, `uart_serviceMidiRealtime`, and
  `midiParser_handleDinRealtime` are present in `LxrStm32.elf`; the IRQ sends
  status bytes in the `0xf8..0xff` realtime class to `midiRealtimeQueue`, not
  to `fifo_midiRx`; and `serviceAudioRenderDeadline()` calls the queue service
  before `calcNextSampleBlock()` whenever DMA frees a render buffer.
- This implementation has **no event timestamp** and makes **no NVIC priority
  change**.  USART2 remains preemption priority `0x0f`; audio DMA remains
  priority `0`; USB IRQs remain priority `1`.  The queue records byte order,
  not arrival time.  Any description of the completed work as timestamped or
  higher-priority capture would be inaccurate.
- The remaining timing stages for DIN MIDI are: serial reception to RXNE
  (about 320 us for one MIDI byte), delayed entry to the still-low-priority
  USART ISR while a higher-priority ISR runs, waiting for the next
  DMA-freed-buffer deadline (up to one 16-frame block, about 364 us), the
  already-rendered ping-pong buffer (about one further block), and I2S/DAC
  pipeline delay.  The new queue removes ordinary FIFO/front-panel/USB
  cooperative-loop delay, but it cannot remove those fixed stages.
- If the tested master reaches the LXR through USB MIDI rather than the DIN
  USART2 port, this implementation is not used at all: USB clock messages
  remain on `usb_getMidi()` -> `midiParser_parseMidiMessage()` ->
  `globalMidiParser_handleSystemMessage()`.
- No code change was made during this audit.  A next timing-improvement design
  must explicitly choose whether to add high-priority/timestamped DIN capture,
  sample-offset audio events, or predictive fixed-latency compensation; these
  have different ISR, DSP-deadline, and musical-clock tradeoffs.

### 2026-07-25 — timestamp, IRQ-priority, and USB extension approved

- User requested implementation of the previously absent timestamp and DIN IRQ
  priority change, and requested that USB MIDI use the same audio-deadline
  realtime path.
- DIN plan: explicitly establish NVIC priority grouping before peripheral IRQ
  setup; keep audio DMA at preemption priority 0/subpriority 0; set USART2 to
  preemption priority 0/subpriority 1; retain USB at preemption priority 1.
  USART2 therefore preempts USB but not a running audio DMA ISR.  Its only
  added work remains timestamp + queue write, so sharing the top preemption
  level cannot materially consume an audio block budget.
- Timestamp plan: enable the Cortex-M DWT cycle counter once at boot and store
  its 32-bit cycle value with every queued realtime event.  Each transport
  records queue-to-dispatch latency for debugger/logic-analyser correlation.
  Timestamping does **not** claim sample-accurate event placement: that would
  require a separate mixer/voice sample-offset design and is not included here.
- USB plan: in the USB MIDI OUT callback, identify complete `0xf8..0xff`
  system-realtime messages before they enter `usb_MidiMessages[]`; timestamp
  and enqueue them in a bounded USB-specific SPSC queue.  The callback is the
  first firmware-visible boundary after a USB host packet arrives.  USB frame
  and host scheduling delay remain physical/protocol limits that firmware
  cannot remove.
- Separate DIN and USB queues are intentional.  They preserve simple SPSC
  ownership (one ISR producer plus main-loop consumer) and avoid a
  multi-producer race between USB and USART interrupt contexts.  The main
  render-deadline service drains DIN then USB; each source preserves arrival
  order.  Simultaneous clock masters are unsupported and have no meaningful
  cross-source musical order.

### 2026-07-25 — timestamp, priority, and USB implementation complete

- Enabled the Cortex-M4 DWT cycle counter once during boot and timestamped
  every successful DIN or USB realtime queue entry.  The repository's legacy
  CMSIS header lacks DWT declarations, so the standard Cortex-M4 DWT CTRL and
  CYCCNT addresses are locally documented in `MidiParser.c`; the STM32F407
  hardware provides that block.
- DIN queue entries now store `{ status, timestamp }`; dispatch records
  wrap-safe queue-to-dispatch cycle latency and a maximum-latency diagnostic.
  Timestamp capture remains bounded IRQ work and does not run sequencer/DSP.
- Set NVIC grouping before audio/UART/USB initialization.  Audio DMA remains
  preemption priority 0/subpriority 0; USART2 is now 0/1; USB remains 1.
  USART2 can preempt USB but cannot preempt a running audio DMA ISR.
- Added the independent USB realtime SPSC queue in `usb_midi_core.c`.  The USB
  OUT callback diverts complete `0xf8..0xff` events to it before the legacy
  `usb_MidiMessages[]` queue.  The main audio-deadline helper drains both DIN
  and USB realtime queues before rendering; ordinary USB MIDI still uses
  `usb_getMidi()`.
- Comments were added beside every new queue, timestamp API/register mapping,
  priority definition, USB API, IRQ classification, and audio-deadline change
  in the affected C and header files.
- Verification passed: `git diff --check`,
  `make -C mainboard/LxrStm32 -j4 stm32`, and `make firmware`.  The linked ELF
  exports `USART2_IRQHandler`, `midiParser_initRealtimeTimestamp`,
  `midiParser_captureRealtimeTimestamp`, `uart_serviceMidiRealtime`, and
  `usb_serviceMidiRealtime`.  The only build output was the repository's
  existing linker RWX-segment warning.
- Remaining hardware checks: repeat the DIN timing capture; separately test
  USB-MIDI clock timing (noting host USB framing remains a limit); test DIN and
  USB Start/Continue/Stop, routing, normal notes/running status, and a
  front-panel/file-load regression.  If fixed audio lateness remains dominant,
  the next distinct feature is sample-offset or predictive latency
  compensation—not more receive-queue priority work.
