# REFACTOR_PHASED_PLAN

Date: 2026-06-09
Status: draft for Session 006 handoff

## Purpose

This document turns the current preset/morph/pattern refactor audit into a
phased implementation plan.

It is intended to answer three questions before code movement begins:

1. What still needs a design decision?
2. What code must move where?
3. In what order should we move it so the firmware stays buildable and debuggable?

## Canonical Inputs

- `MEMORY.md`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`
- `AUDIT_PRESET-MORPH_REFACTOR.md`
- `REFACTOR_DIAGRAM.md`
- `knowledge_files/hardware_archive/ATMEGA_STM32F4_COMMS_AUDIT.md`
- `knowledge_files/hardware_archive/main/STM32F4_HARDWARE.md`
- `knowledge_files/hardware_archive/main/STM32F4_SETUP_ALLOCATION.md`
- `knowledge_files/hardware_archive/front/AVR_HARDWARE.md`
- `knowledge_files/hardware_archive/front/AVR_SETUP_ALLOCATION.md`
- `knowledge_files/log_archive/004_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/005_SESSION_HANDOFF_LOG.md`

## Confirmed Decisions

1. Should `Preset` own the `SeqKitState` images completely, or should
   `Sequencer` keep a thin source-selection shim for pattern boundaries and
   hardware-facing restore events?
   - Preset owns these images completely. API for access by sequencer.

2. Should `uARTFrontSYX` absorb the whole front-panel parser plus the UART
   transport, or should the transport and parser be split into separate files
   inside that folder from the start?
   - Separate files.

3. Should `frontPanelParser.c` become a protocol-only layer that forwards all
   state changes into `Preset`, or should it keep any session/cache state after
   the split?
   - Parser can keep states related to communication modes, Preset should own all cache states related to parameter storage.

4. Should the `Preset` load/cache state live under `Preset/PresetLoadCache.*`
   or inside `uARTFrontSYX` as part of the front-panel protocol session model?
   - `Preset` should own the load/cache state in `Preset/PresetLoadCache.*`;
     `uARTFrontSYX` should stay thin and only parse bytes, manage transport,
     and call the preset session API.

5. Should `Sequencer/Pattern` own only pattern data plus the Euclid/SOM
   generators, or should it also own the temp-pattern switch state machine that
   currently lives in `sequencer.c`?
   - Pattern should own this, in addition to any mechanisms related to executing copyClear functions initiated by the AVR. (copy pattern, copy, track, copy step etc.)

6. How aggressive should the rename pass be in one refactor step?
   Wrapper-first migration is safer, but a direct rename is cleaner if the
   build can stay green.
   - keep the refactor safe and deliberate, with detailed risks and test objectives for anything that could go wrong at each step.

7. Should the legacy SEQ16 temp-pattern keyhole remain as a compatibility shim
   inside the new pattern module, or should it be turned into an explicit test
   hook with a clearer name?
   - yes, keep it as a shim in Pattern, tie the functionality to a separate file if possible so it is clearly separated from standard functions.

8. Should the front-panel protocol opcode names stay in the shared
   `MidiMessages.h` namespace, or move into a dedicated
   `uARTFrontSYX/FrontPanelProtocol.h` header with a compatibility include?
   - sounds good, move the opcodes there.

9. Should `uart_sendFrontpanelPriorityByteWait()` remain as a transport escape
   hatch, or be replaced by a bounded session-aware queue once the new protocol
   split is in place?
   - Keep `uart_sendFrontpanelPriorityByteWait()` in the UART layer as a
     transport primitive, not inside `frontPanelParser`.
   - The parser or session layer should decide *when* a byte is critical enough
     to require prioritized delivery, but the UART layer should still own the
     mechanics of waiting for FIFO space and enabling TX interrupts.
   - In other words: parser decides, UART executes.
   - This is the cleanest split because `uart_sendFrontpanelPriorityByteWait()`
     is fundamentally about FIFO pressure, drain timing, and interrupt plumbing.
     It does not know why the byte matters; it only knows how to force it into
     the output queue.
   - Moving that helper into `frontPanelParser` would mix protocol semantics
     with transport mechanics and make the parser harder to test, harder to
     reason about, and more likely to accumulate more special-case send logic
     over time.
   - The better long-term shape is to introduce a higher-level helper in the
     protocol/session layer later, such as `frontPanel_sendCriticalSessionByte()`
     or `frontPanelProtocol_sendPriority()`. That helper can encode the policy
     for “this message must get out now” and internally call the UART wait
     helper when the protocol actually needs it.
   - Recommended migration path:
     - keep the blocking UART helper during the early refactor so the protocol
       split stays stable;
     - move the decision-making into the parser/session layer as it becomes
       more state-aware;
     - replace the raw escape hatch later with a bounded session-aware queue if
       the protocol work shows that explicit backpressure handling is practical.
   - Main risk of keeping the helper: it can still block indefinitely if the
     FIFO never drains, so the refactor should treat it as transitional rather
     than a final design.
   - Main risk of moving it too early: we would be changing transport behavior
     and protocol ownership at the same time, which would make the refactor
     harder to verify and more fragile.

10. Should the current live-apply cache and shared-parameter validity arrays be
    kept as transitional internals, or retired once the `Preset` module owns the
    whole morph pipeline?
    - we want to try a phased retirement once the pipeline is moved into its proper place.

11. Legacy direct-load voice-cache promotion path:
    - Retire it as a separate mechanism. There should be only one background
      file-load path in flight at a time, and a newer background load may
      discard and overwrite an older in-flight one. If any part of the old
      direct-load behavior still matters, it should be folded into the unified
      background-load finalizer rather than preserved as a separate voice-cache
      promotion mode.

## Proposed Module Boundaries

### `mainboard/LxrStm32/src/Preset/`

Owns sound-state authority, endpoint storage, morph state, interpolation, and
endpoint restore behavior. It also owns the single in-flight background-load
session that can be replaced by a newer background load request.

Expected subfiles:

- `KitState.h/.c`
- `ParameterMap.h/.c`
- `ParameterIngress.h/.c`
- `MorphEngine.h/.c`
- `EndpointRestore.h/.c`
- `TempPlaybackSwitch.h/.c`
- `PresetLoadCache.h/.c`

### `mainboard/LxrStm32/src/uARTFrontSYX/`

Owns the physical front-panel UART transport and the front-panel protocol layer
that translates incoming/outgoing bytes into semantic requests.

Expected subfiles:

- `Uart.h/.c`
- `frontPanelParser.h/.c`
- optional `FrontPanelProtocol.h/.c` if the opcode namespace is split out

### `mainboard/LxrStm32/src/Sequencer/Pattern/`

Owns pattern data, pattern copy/apply helpers, temp-pattern storage, and the
pattern generation modules.

Expected subfiles:

- `PatternData.h/.c`
- `EuklidGenerator.h/.c`
- `SomData.h/.c`
- `SomGenerator.h/.c`

### `mainboard/LxrStm32/src/Sequencer/`

Keeps the real-time sequencer loop, timing, trigger scheduling, and the narrow
façade that binds pattern and preset modules together.

## Code Movement Map

| Current location | Target location | Move | Notes |
|---|---|---|---|
| `mainboard/LxrStm32/src/Sequencer/sequencer.h` | `Preset/KitState.h` | `SeqKitState`, `SeqKitAutomationTargets`, image selection helpers, ingress enums | This is the core sound-state boundary. |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | `Preset/KitState.c` | `seq_normalKitState`, `seq_tmpKitState`, capture/copy helpers, temp-activation helpers | Keep wrappers until callsites are migrated. |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | `Preset/ParameterMap.c` | Voice mask helpers, parameter classification, selector helpers | This is where human-readable parameter semantics should live. |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | `Preset/ParameterIngress.c` | `seq_setIngressTarget()`, `seq_getIngressTarget()`, ingress apply helpers | This should become the only ingress gateway for raw endpoint bytes. |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | `Preset/MorphEngine.c` | Morph amount state, interpolation, live-apply cache, LFO/velocity morph, phased drain | This is the largest functional split. |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | `Preset/EndpointRestore.c` | STM-to-AVR endpoint push-up, restore queue, handshake, rate limiting, display-only global morph report | This should own outbound restore protocol behavior. |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | `Preset/TempPlaybackSwitch.c` | Pattern source switching, voice source mapping, hihat coupling, temp-boundary handling | This isolates the temp/normal image switch logic. |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | `Sequencer/Pattern/PatternData.c` | Pattern set storage, temp pattern storage, copy helpers, setters/getters, clear/apply helpers | This is the main pattern ownership move. |
| `mainboard/LxrStm32/src/Sequencer/EuklidGenerator.*` | `Sequencer/Pattern/` | Move generator files into the pattern submodule | Generators should call pattern APIs, not the other way around. |
| `mainboard/LxrStm32/src/Sequencer/SomData.*` | `Sequencer/Pattern/` | Move SOM support files into the pattern submodule | Same ownership as Euclid. |
| `mainboard/LxrStm32/src/Sequencer/SomGenerator.*` | `Sequencer/Pattern/` | Move SOM generator files into the pattern submodule | Keep the pattern generation family together. |
| `mainboard/LxrStm32/src/MIDI/Uart.c` | `uARTFrontSYX/Uart.c` | Front-panel transport, FIFOs, IRQ handlers, send/process wrappers | This is a straight ownership move. |
| `mainboard/LxrStm32/src/MIDI/Uart.h` | `uARTFrontSYX/Uart.h` | Transport API | Human-readable names should be finalized here. |
| `mainboard/LxrStm32/src/MIDI/frontPanelParser.c` | `uARTFrontSYX/frontPanelParser.c` | Front-panel protocol state machine, parser, command dispatch | Parser should not keep preset state once the split is complete. |
| `mainboard/LxrStm32/src/MIDI/frontPanelParser.h` | `uARTFrontSYX/frontPanelParser.h` | Parser-facing public API | Keep only the external entry points that the transport or main loop needs. |
| `mainboard/LxrStm32/src/MIDI/frontPanelParser.c` | `Preset/PresetLoadCache.c` | PRF/load cache session state, deferred perf replay, live snapshot cache, single overwriteable background-load session | This is a strong candidate for removal from the protocol layer. |
| `mainboard/LxrStm32/src/MIDI/MidiMessages.h` | optional `uARTFrontSYX/FrontPanelProtocol.h` | Front-panel opcode names and semantic protocol constants | Decision needed on whether to keep a shared header or split it. |

## Functions And Variables To Flag As Possibly Redundant

| Symbol | Why it may be redundant | Likely action |
|---|---|---|
| `seq_loadPendigFlag` | Typo plus unclear ownership of load state | Rename or replace with a clearer pattern-load request flag. |
| `seq_tmpKitPushParamsToFrontEnabled` | Restore policy is already a separate concern | Move into restore/session policy or retire after the split. |
| `seq_liveSharedParams[]` | Looks like a transitional cache for live apply coherence | Keep only if the new morph engine still needs a local cache. |
| `seq_liveSharedParamsValid[]` | Duplicates validity semantics that should live in the apply cache | Likely retire with the old live-apply path. |
| `seq_liveMorphAppliedValue[][]` | Transitional dedupe cache, not core preset state | Keep only if repeated DSP applies still need suppression. |
| `seq_morphScanParam` | Pure morph-engine cursor state | Move into `MorphEngine` internals. |
| `seq_vMorphAmount[]` | May become legacy mirror state once `Preset` owns morph fully | Keep as a compatibility mirror only if needed. |
| `seq_vMorphFlag` | Trigger flag for the old morph path | Move into `MorphEngine` or fold into a higher-level event queue. |
| `seq_applySharedParameterValues()` | Appears unused in the current build shape | Confirm and delete if still dead after migration. |
| `seq_activateTmpPattern()` | Thin wrapper over temp copy/apply | Retain only if it improves readability; otherwise fold into `Pattern` APIs. |
| `frontParser_deferredPerfLoadActive` and related flags | Multiple booleans model one session concept | Replace with an explicit load-session state struct. |
| `frontParser_prfCacheLivePattern` | Snapshot cache may belong with `PresetLoadCache` instead of the parser | Move if the parser is reduced to transport/protocol only. |
| `frontParser_prfCacheLiveStepIndex[]` | Snapshot data that mirrors sequencer state | Move with the rest of the live snapshot if it stays needed. |
| `frontParser_applyDeferredVoiceCache()` | Old promotion path for load completion | Fold into the unified temp/background finalizer and retire as a separate mechanism. |
| `uart_clearFrontFifo()` | Flushes both TX and RX and can discard in-flight state | Keep only if the new protocol design still needs a hard flush. |
| `uart_sendFrontpanelPriorityByteWait()` | Blocking backpressure primitive is risky | Replace with bounded session-aware queueing if possible. |
| `frontParser_isQuietUi()` | A session state query more than a parser concern | Move into a communications/session helper if it remains useful. |

## Implementation Phases

### Phase 1: Carve The Core `Preset` Types

Goal:

- Create the new `Preset` folder and move the core sound-state types there.
- Keep the build green by using compatibility includes and wrappers.

Deliverables:

- `Preset/KitState.*`
- `Preset/ParameterMap.*`
- `Preset/ParameterIngress.*`
- initial renames for human-readable public API names
- doc comments for every exported function in the moved files
- fold passive-vs-actionful apply policy into `ParameterIngress` unless the
  refactor proves a separate `PassiveApply` helper is still needed

Exit criteria:

- The repo still builds.
- The old `sequencer.h` include path can remain as a façade, but the actual
  ownership of `SeqKitState` is now in `Preset`.

### Phase 2: Move Morph And Live-Apply Ownership

Goal:

- Move the morph engine, live-apply cache, and voice morph control into
  `Preset/MorphEngine.*`.

Deliverables:

- interpolation worker
- phased LFO-to-morph drain
- velocity-to-morph one-shot path
- global morph set/get helpers
- cache invalidation and live apply suppression helpers

Exit criteria:

- `sequencer.c` no longer owns morph interpolation logic.
- `seq_morphScanParam` and related live-apply internals are no longer public.

### Phase 3: Move Endpoint Restore And Temp Switching

Goal:

- Separate the outbound restore protocol and the temp/normal source selection
  model from the rest of the sequencer.

Deliverables:

- `Preset/EndpointRestore.*`
- `Preset/TempPlaybackSwitch.*`
- explicit temp-boundary restore path for display-only global morph reports
- split `frontParser_applyDeferredVoiceCache()` into a single temp/background
  load finalizer and retire the legacy direct-load voice-cache promotion path
- the background-load finalizer should model one in-flight load at a time, with
  newer background requests able to replace older pending ones

Exit criteria:

- The sequencer can report source changes to `Preset`, but the actual restore
  policy lives outside the real-time step engine.

### Phase 4: Move Pattern Data And Generators

Goal:

- Make `Sequencer/Pattern` the single owner of pattern data and pattern
  generation code.

Deliverables:

- `PatternData.*`
- `EuklidGenerator.*`
- `SomData.*`
- `SomGenerator.*`
- human-readable pattern APIs for get/set/copy/clear operations
- SEQ16 temp-pattern keyhole retained as a clearly named shim module if it is
  still needed for observation/control

Exit criteria:

- `sequencer.c` only calls pattern APIs, it does not define the pattern data
  model directly.

### Phase 5: Move The Front-Panel UART Layer

Goal:

- Move the UART transport and front-panel protocol code into
  `uARTFrontSYX`.

Deliverables:

- `uARTFrontSYX/Uart.*`
- `uARTFrontSYX/frontPanelParser.*`
- protocol constants split or adapted as needed
- parser-side session state cleaned up into clear groups
- front-panel protocol opcode definitions moved into
  `uARTFrontSYX/FrontPanelProtocol.h` with a compatibility include, if the
  shared `MidiMessages.h` namespace is still too noisy after the split

Exit criteria:

- `MIDI/` is no longer the home of the front-panel transport layer.
- The parser calls into `Preset` and `Sequencer/Pattern` through semantic APIs
  rather than reaching across modules for raw storage details.

### Phase 6: Remove Redundant State And Polish Naming

Goal:

- Remove transitional aliases and simplify the final API surface.

Deliverables:

- typos fixed in exported names
- redundant compatibility wrappers removed
- function-level documentation added everywhere the new modules export API
- names made human-readable and semantically correct
- AVR temp read buffers renamed to reflect file-read and menu-snapshot usage
- the comment-upgrade notes for new STM functions are applied while moving the
  code, rather than tracked as a separate audit task

Exit criteria:

- The final module boundaries are stable.
- The old names only survive where they are needed for compatibility with a
  deliberately retained façade.

## Suggested Move Order

1. Create the new directories and header skeletons.
2. Move type definitions and pure helpers first.
3. Move morph ownership second.
4. Move temp switching and restore behavior third.
5. Move pattern data and generators fourth.
6. Move the front-panel UART/protocol layer fifth.
7. Delete redundant shims and finish the rename pass last.

## Files That Will Need Include-Graph Updates

- `mainboard/LxrStm32/src/main.c`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/Sequencer/sequencer.h`
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiParser.h`
- `mainboard/LxrStm32/src/MIDI/MidiMessages.h`
- every file under `mainboard/LxrStm32/src/DSPAudio/` or `mainboard/LxrStm32/src/MIDI/`
  that currently reaches directly into sequencer-owned storage

## Acceptance Criteria For The Whole Refactor

- Build remains reproducible throughout the migration.
- All exported functions have in-place documentation.
- Sound-state ownership lives in `Preset`.
- Front-panel transport and parser code lives in `uARTFrontSYX`.
- Pattern data and generators live in `Sequencer/Pattern`.
- Human-readable API names replace the current cross-module leakage where
  practical.
- Redundant transitional state is removed after the final phase.

## Session 006 Handoff Note

This file is intentionally an outline, not an implementation log.

The next session should use this as the working checklist, answer the open
questions above, and then start the first code move only after the module
boundaries are agreed.
