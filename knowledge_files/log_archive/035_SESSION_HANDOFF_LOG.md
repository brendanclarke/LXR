# 035 Session Handoff Log - Timestamped DIN/USB MIDI Realtime Dispatch

DATE: 2026-07-25

## Session Goal

Investigate an observed LXR MIDI-clock timing offset of approximately 2–3.5 ms
against a master sequencer, then tighten the software-controlled latency and
jitter path without moving DSP/sequencer work into an interrupt.  The user
first requested a detailed root-level plan, then requested implementation of
the dedicated realtime lane and audio-deadline dispatch.  After the first
hardware retest showed very little difference, the user requested explicit
timestamps, a DIN IRQ-priority change, and identical realtime treatment for
USB MIDI.

The root `SESSION_LOG_LATENCY_FIX.md` contains the in-session plan and progress
notes.  It may be deleted later; this handoff is the durable complete record.

## Investigation Findings

### Original DIN path

Before this session, every DIN MIDI byte took this path:

```text
USART2 RXNE IRQ -> fifo_midiRx -> uart_processMidi() removes one byte per
main-loop pass -> midiParser_parseUartData(F8) -> seq_sync()/sync_tick() ->
later audio-buffer render -> ping-pong DMA -> I2S/DAC output
```

Important findings:

- `USART2_IRQHandler()` only enqueued raw bytes; it did not parse or timestamp
  them.
- `uart_processMidi()` removed only one byte per loop.  A realtime clock could
  therefore wait behind unrelated cooperative-loop work.
- The main loop previously rendered an audio block before normal MIDI service
  and also after USB/front-panel work.  A clock-triggered voice update could
  miss a newly freed render buffer and wait another block.
- The active STM build uses `DMA_MODE_ACTIVE`, overriding `OUTPUT_DMA_SIZE` to
  16 frames.  At `REAL_FS ~= 44002.76 Hz`, one audio block is about 364 us.
- The audio system is ping-pong DMA.  Even a perfectly scheduled trigger still
  has block-level and I2S/DAC pipeline delay after it reaches firmware.
- DIN serial reception itself takes about 320 us for one MIDI byte.  No receive
  queue change can make an output precede a MIDI clock that has not yet arrived.
- The old front-panel RX drain remains deliberately unbounded and must not be
  changed casually: a past unbounded front-RX change froze a known-good `.ALL`
  load.

The initial post-change waveform retest showed very little visible difference.
That is consistent with the remaining fixed serial, DMA, I2S, and DAC delay
dominating the capture.  The first queue change primarily removes
cooperative-loop jitter, not fixed output latency.

### USB path finding

USB MIDI did not use the DIN UART path.  `usbd_midi_DataOut()` parsed USB MIDI
packets and appended all complete messages, including realtime, to the ordinary
`usb_MidiMessages[]` ring.  The main loop later consumed one message via
`usb_getMidi()` after front-panel work.  Therefore the first DIN-only change
would have no effect on a USB-clock test.

USB MIDI is bulk endpoint traffic.  Firmware now captures realtime at the USB
OUT callback, but host scheduling and USB frame/packet timing remain an
unavoidable input-side timing limit.

## Design Decisions And Constraints

### Realtime capture versus transport work

The receive ISRs remain capture-only.  They may classify a one-byte realtime
status, timestamp it, and append it to a bounded queue.  They must not call:

- `seq_sync()` or `sync_midiStartStop()`;
- voice/DSP code;
- routing or MIDI transmit functions;
- USB/front-panel parsing; or
- any wait/flow-control function.

This preserves audio predictability and MIDI running-status correctness.
System-realtime messages may legally occur between channel-message data bytes;
removing them from the ordinary raw DIN stream does not alter parser state.

### Separate queues, not a shared producer queue

DIN and USB use independent 16-slot single-producer/single-consumer queues:

- DIN producer: `USART2_IRQHandler()`; consumer: main-loop audio deadline.
- USB producer: `usbd_midi_DataOut()`; consumer: main-loop audio deadline.

They are intentionally not combined.  USB and USART run in different interrupt
contexts, so one shared queue would require a multi-producer synchronization
scheme and introduce a race/preemption concern.  Each source retains arrival
order.  The render service drains DIN, then USB; concurrent clock masters are
unsupported and have no meaningful cross-source musical ordering.

Full queues preserve old entries and increment an overflow counter rather than
silently coalescing or overwriting a clock.  Normal MIDI-clock rates leave
very large queue headroom.

### Timestamping

`midiParser_initRealtimeTimestamp()` enables the STM32F407 Cortex-M4 DWT cycle
counter at boot.  `midiParser_captureRealtimeTimestamp()` returns the 32-bit
cycle count and is safe in the short receive paths.  Every successful realtime
queue write stores `{ status, timestamp }`; dispatch subtracts it from a fresh
DWT timestamp and retains last/max queue-to-dispatch latency diagnostics.  The
subtraction is unsigned and remains valid across normal counter wrap.

The repository's old CMSIS header declares `CoreDebug` but omits DWT.  The
standard Cortex-M4 DWT addresses are therefore documented locally in
`MidiParser.c`:

- CTRL: `0xE0001000`
- CYCCNT: `0xE0001004`

Timestamping measures firmware queue latency; it does **not** make a voice
start at a fractional sample offset.  Sample-accurate event placement would
require a distinct mixer/voice scheduling design and was not added.

### IRQ priority decision

`main()` now calls `NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1)` before
audio, DIN UART, and USB initialization, making priority interpretation
explicit instead of relying on the USB BSP to set it later.

The intended order is:

| Source | Preemption priority | Subpriority | Meaning |
|---|---:|---:|---|
| Audio DMA | 0 | 0 | Highest pending work; must not be preempted by MIDI capture. |
| DIN USART2 | 0 | 1 | Can preempt USB, but not a running audio DMA IRQ; does timestamp + queue only. |
| USB OTG | 1 | existing 3 | USB packet handling remains below DIN/audio. |

`MIDI_UART_IRQ_PREPRIO` and `MIDI_UART_IRQ_SUBRIO` live in `Uart.h` with the
reason for this arrangement.  Do not move sequencer or DSP work into USART2 to
"take advantage" of the priority increase.

## Completed Changes

### `mainboard/LxrStm32/src/uARTFrontSYX/Uart.c/.h`

- Added a 16-slot DIN `MidiRealtimeEvent` SPSC queue storing status and DWT
  timestamp, producer/consumer indices, overflow count, and last/max dispatch
  latency diagnostics.
- Added `uart_queueMidiRealtime()` as the private capture-only queue writer.
- `USART2_IRQHandler()` now classifies `(data & 0xf8) == 0xf8` as realtime and
  sends those bytes to the dedicated queue.  All other bytes retain the
  existing `fifo_midiRx` path.
- `uart_processMidi()` remains a one-byte ordinary MIDI service.  Its comment
  now explicitly documents that realtime is excluded and running status is
  preserved.
- Added exported `uart_serviceMidiRealtime()`, which drains the bounded queue
  at the render deadline, records dispatch latency, and calls
  `midiParser_handleDinRealtime()` in main-loop context.
- Initializes realtime queue/diagnostic state before USART2 RX is enabled.
- Added documented `MIDI_UART_IRQ_PREPRIO = 0` and
  `MIDI_UART_IRQ_SUBRIO = 1`; USART2 now uses them instead of `0x0f/0x0f`.

### `mainboard/LxrStm32/src/MIDI/MidiParser.c/.h`

- Extracted DIN system-realtime handling from `midiParser_parseUartData()` into
  `midiParser_handleDinRealtime(uint8_t data)`.
- Preserved behavior for Start/Continue/Stop/Clock: they still require the DIN
  receive-filter clock bit (`midiParser_txRxFilter & 0x02`) and external sync
  (`seq_getExtSync()`), then call the same `sync_midiStartStop()` or
  `seq_sync()` path.
- Preserved DIN-to-DIN/USB realtime routing in main-loop context.  The helper
  creates a zero-length `MidiMsg`, avoiding the legacy uninitialized message
  length risk in this path.
- Kept a compatibility call from `midiParser_parseUartData()` for any direct
  caller that still supplies a raw realtime byte.
- Added timestamp enable/read APIs and documented the locally defined DWT
  register addresses required by the legacy CMSIS header gap.

### `mainboard/LxrStm32/src/Hardware/USB/usb_midi_core.c` and `usb_manager.h`

- Added a separate 16-slot timestamped USB realtime SPSC queue with overflow
  and last/max dispatch-latency diagnostics.
- Added private `usb_queueMidiRealtime()` and exported
  `usb_serviceMidiRealtime()`.
- In `usbd_midi_DataOut()`, complete `0xf8..0xff` USB-MIDI messages now queue
  immediately at the callback boundary rather than entering
  `usb_MidiMessages[]`.
- Non-realtime USB MIDI keeps the existing `usb_MidiMessages[]` path exactly:
  channel messages, system-common/MTC, and SysEx remain available through
  `usb_getMidi()` and are parsed by the existing main-loop code.
- USB realtime dispatch reconstructs a zero-length, `midiSourceUSB` `MidiMsg`
  and calls `midiParser_parseMidiMessage()`, preserving USB routing and
  `GlobalMidiParser` system-message semantics.
- USB realtime queue state resets before the OUT endpoint is prepared.

### `mainboard/LxrStm32/src/main.c`

- Added `serviceAudioRenderDeadline()` and replaced both duplicated render
  checks with it.
- It returns unless `bCurrentSampleValid != SAMPLE_VALID`; when a DMA buffer is
  free, it services DIN realtime, then USB realtime, then calls
  `calcNextSampleBlock()` exactly once.
- The first deadline service moved to the top of the main loop before
  `usb_tick()`.  The second remains after ordinary DIN/front-panel/USB message
  work, catching a DMA deadline reached during that work.
- It intentionally does not run ordinary MIDI, front-panel parsing, endpoint
  restore, morph interpolation, or blocking protocol work in the
  clock-to-render critical section.
- Added early priority-group setup and DWT timestamp initialization.

### Documentation and build artifact

- `SESSION_LOG_LATENCY_FIX.md` was created at repository root as the detailed
  plan/progress record.  This handoff supersedes it for durable knowledge; the
  user may delete the root file later.
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md` and
  `knowledge_files/comms_spec_reference/MIDI_TABLE.md` are updated in this
  closeout to describe the realtime capture/dispatch boundary.
- `firmware image/FIRMWARE.BIN` was rebuilt from the STM32 result and the
  existing AVR binary.

## Verification

### Static/build verification

- `git diff --check` passed after each completed implementation pass.
- `make -C mainboard/LxrStm32 -j4 stm32` passed after the final timestamp and
  USB changes.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.
- The final linked ELF exports:
  - `USART2_IRQHandler`
  - `midiParser_initRealtimeTimestamp`
  - `midiParser_captureRealtimeTimestamp`
  - `uart_serviceMidiRealtime`
  - `usb_serviceMidiRealtime`
- Final build output retained only the repository's existing linker warning
  about an RWX load segment.  Earlier routine builds also emitted the known
  legacy inline, duplicate-const, Sequencer-bounds, and recursive-`_exit`
  warnings; no new source warning/error remained after the final build.

### Hardware status

- The user performed a waveform retest after the initial DIN realtime queue
  and audio-deadline change and reported very little difference.
- The user accepted the design direction for the subsequent timestamp,
  priority, and USB extension, but no explicit post-extension hardware timing
  capture is recorded in this closeout.
- Do not claim a measured latency reduction until the current firmware is
  flashed and tested.

Recommended hardware matrix:

1. DIN MIDI Start, Continue, Stop, and clock sync.
2. DIN ordinary notes/running status and enabled MIDI/USB routing.
3. USB MIDI Start, Continue, Stop, and clock sync.
4. USB ordinary notes/routing plus a front-panel/file-load regression check.
5. Repeat the original dual-waveform capture for DIN; run a separate USB-clock
   capture if USB clock support matters.  Record mean/min/max/peak-to-peak
   offset over at least 100 hits with the same source, output, sample rate, and
   onset method.

## Remaining Limits And Next Decisions

This work removes ordinary-MIDI queue/cooperative-loop delay from realtime
capture and makes its firmware delay observable.  It cannot remove:

- DIN byte serialization time;
- time while a higher/equal-priority ISR is already running;
- wait to the next 16-frame render deadline (up to about 364 us);
- ping-pong DMA/I2S/DAC pipeline latency; or
- host USB bulk packet/frame scheduling.

If the fixed offset is still unacceptable after the current hardware test, the
next feature must be separately designed and approved:

1. sample-offset audio event scheduling inside a rendered block, or
2. a stable-clock predictive PLL/latency-compensation mode.

Neither belongs in the timestamp queue as a casual follow-up.  A predictive
mode must have explicit Start/Stop/dropout/tempo-change behavior and must not
fire a hit early on an unreliable clock.

## Repository State At Session End

Current branch: `master`.

Session-owned functional changes:

- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiParser.h`
- `mainboard/LxrStm32/src/Hardware/USB/usb_midi_core.c`
- `mainboard/LxrStm32/src/Hardware/USB/usb_manager.h`
- `mainboard/LxrStm32/src/main.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.h`
- `firmware image/FIRMWARE.BIN`

Session documentation changes:

- `SESSION_LOG_LATENCY_FIX.md` (temporary root plan; user may remove later)
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
- `knowledge_files/comms_spec_reference/MIDI_TABLE.md`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`
- `knowledge_files/log_archive/035_SESSION_HANDOFF_LOG.md`
- `MEMORY.md`

Pre-existing/unrelated file at session start:

- `GLOTMP.CFG` (untracked global-config snapshot; inspected but not modified).

## End Of Session Block

```
DATE: 2026-07-25
SESSION GOAL: Reduce MIDI-clock latency/jitter by separating realtime from ordinary DIN traffic, dispatching it at the audio render deadline, then add timestamping, explicit DIN IRQ priority, and USB realtime support.
COMPLETED: Added timestamped bounded realtime queues for DIN and USB; moved realtime dispatch immediately before DMA-freed audio-buffer render; extracted/preserved DIN realtime parser behavior; set explicit audio/DIN/USB IRQ grouping and priority; added DWT queue-latency diagnostics; rebuilt firmware and documented the timing model.
VERIFIED ON HARDWARE: partial. The initial DIN queue/deadline version was retested and showed little waveform difference. The final timestamp/priority/USB extension is build-verified only; a new hardware timing capture is still required.

CHANGES THIS SESSION:
- `mainboard/LxrStm32/src/uARTFrontSYX/Uart.c/.h`: timestamped DIN realtime SPSC queue, bounded main-loop service, overflow/latency diagnostics, and USART2 audio-adjacent priority.
- `mainboard/LxrStm32/src/MIDI/MidiParser.c/.h`: extracted DIN realtime dispatcher, DWT cycle-counter setup/read APIs, and documented local DWT register mapping for legacy CMSIS.
- `mainboard/LxrStm32/src/Hardware/USB/usb_midi_core.c`: timestamped USB realtime SPSC queue at the MIDI OUT callback; non-realtime USB queue path retained.
- `mainboard/LxrStm32/src/Hardware/USB/usb_manager.h`: USB realtime service declaration.
- `mainboard/LxrStm32/src/main.c`: explicit NVIC grouping/DWT boot setup and DIN+USB realtime dispatch before each required audio render.
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`, `MIDI_TABLE.md`: realtime capture/dispatch rules.
- `knowledge_files/log_archive/000_SESSION_INDEX.md`, `035_SESSION_HANDOFF_LOG.md`, `MEMORY.md`: session closeout.

KNOWN ISSUES INTRODUCED: None confirmed. The new DWT counters are diagnostic-only and are not exported to UI/telemetry; inspect them with a debugger if queue-latency numbers are required.
KNOWN ISSUES RESOLVED: Realtime DIN/USB clock no longer waits behind the ordinary respective MIDI queues before it can reach the render deadline; DIN capture now has explicit priority relative to audio and USB; firmware queue delay is timestamped.

NEXT SESSION RECOMMENDED GOAL: Flash the current image and perform the DIN/USB timing and regression matrix. If fixed lateness remains the problem, design either sample-offset events or a guarded predictive compensation mode from measurements.
BLOCKERS: No build blocker. Quantitative hardware capture of the final firmware is required before selecting any fixed-latency compensation strategy.

CRITICAL REMINDERS FOR NEXT SESSION:
- Keep receive ISRs capture-only: no `seq_sync`, routing, TX, DSP, or waits in USART2/USB receive context.
- Keep DIN and USB realtime queues separate SPSC structures; do not merge them into an unsafe multi-producer queue.
- `serviceAudioRenderDeadline()` must dispatch realtime before `calcNextSampleBlock()` and must remain free of ordinary parser/front-panel/restore work.
- Audio DMA is 0/0, DIN USART2 is 0/1, USB is 1 under `NVIC_PriorityGroup_1`; do not lower audio below capture or add expensive work to USART2.
- DWT timestamps measure firmware capture-to-dispatch delay only. They do not provide sample-offset playback or compensate fixed DAC/USB-host latency.
- Ordinary DIN parser and USB `usb_getMidi()` paths must continue to exclude only realtime status bytes; preserve MTC, SysEx, channel, and running-status behavior.
- The old front-panel RX drain must not be made unboundedly more aggressive; it previously froze `.ALL` load.
```
