# PRESET_CONSOLIDATION_AUDIT

Date: 2026-06-13
Status: Phase 7 implementation landed in Session 014; Phase 8 completed and closed in Session 015; Phase 9 implementation landed in Session 017; Phase 10 implementation landed in Session 018; Phase 11 rename completion landed in Session 019; the remaining `seq_` surface is compatibility-only outside `/Preset/`, and the UART/MIDI split tracking now lives in `MIDI_UART_SPLIT_AUDIT.md`

## Purpose

This audit is the next step after the Phase 6 cleanup and naming pass.

Phase 6 removed the obvious naming debt and the parser-facing `PresetLoadCache`
surface. The next step is more structural: remove the remaining redundant
preset/session machinery, collapse the core preset ownership model, and then
leave only the truly asynchronous workers split out.

This document is intentionally detailed because the next refactor pass has two
different goals that must not get mixed together:

1. eliminate the last large redundant loader/session subsystem;
2. consolidate the remaining preset storage and routing code into a single
   authoritative core API.

## High-Level Shape

The long-term target is:

- one consolidated preset core that owns preset storage structures and the
  normal/temp routing API;
- one morph worker;
- one endpoint-restore worker;
- no separate `PresetLoadCache` subsystem;
- no separate temp-playback/session subsystem that overlaps with loading;
- no background-load mechanism that exists only to support kit data outside
  `.prf` / `.all` / `.pat`-style file flows.

The current phase order is therefore:

- Phase 7: make `PresetLoadCache` redundant and remove its mechanisms by
  routing background loads through the existing temp pattern and parameter
  structures.
- Phase 8: perform the remaining `/Preset/` consolidation work that the current
  consolidation audit already identified, including moving `ParameterArray`
  into `/Preset/` and deciding whether `ParameterMap`, `ParameterIngress`, and
  `TempPlaybackSwitch` collapse into the same core or remain thin helpers.
- Phase 9: normalize the Preset-owned public surface so exported functions and
  variables in `/Preset/` use `preset_`, not `seq_`, and catalog the remaining
  compatibility holdovers that still need a wider cutover.
- Phase 10 and beyond: continue extracting live parameter, modulation, and
  automation application out of `Sequencer`, then finish the remaining
  Preset-owned state accessors and compatibility cleanup.

## Why `PresetLoadCache` Is The First Target

`PresetLoadCache` is the biggest remaining source of overlap in the preset
layer.

It currently owns:

- deferred performance replay bookkeeping;
- pattern-pending counters;
- live snapshot mirrors;
- legacy voice-cache promotion helpers;
- a file-load ingress mode;
- a PRF session state machine;
- the finalizer that currently drains the old mixed behavior.

That is too much for a single helper file, and more importantly it duplicates
state that already exists elsewhere in `/Preset/`:

- `KitState` already owns the canonical normal and temp parameter images;
- `TempPlaybackSwitch` already owns the temp/normal boundary state;
- `MorphEngine` already owns the live parameter application cache;
- `EndpointRestore` already owns the AVR push-up side of the protocol;
- the temp pattern structures already represent the in-flight pattern state.

So the right next move is not to keep polishing `PresetLoadCache`. The right
move is to make it unnecessary.

### Session 014 progress

Session 014 has already removed the shared-cache dependency from the runtime
voice/pattern callers:

- `Sequencer` now reads live step, length, pattern, MIDI channel, note
  override, and morph state directly from the owner arrays instead of the
  shared cache mirror.
- `MidiParser` now resolves note override and MIDI channel routing directly
  from `midi_MidiChannels[]` and `midi_NoteOverride[]`.
- `MidiVoiceControl` now reads the voice channel directly from
  `midi_MidiChannels[]`.
- `TempPlaybackSwitch.h` now carries the `presetLoad_finalizeTempBackgroundLoad()`
  declaration so `sequencer.c` no longer needs the old cache header just for
  the finalizer call.

Session 014 completed the shared-module removal: `PresetLoadCache.c/.h` were
deleted, and the remaining transitional load/session bridge now lives in
`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`.

That leaves one parser-local holdover instead of a separate shared cache
module. The remaining direct-owner cleanup is now the follow-on consolidation
work, not another cache-module cutover.

### Session 014 implementation note

- deleted `mainboard/LxrStm32/src/Preset/PresetLoadCache.c`;
- deleted `mainboard/LxrStm32/src/Preset/PresetLoadCache.h`;
- moved the remaining `presetLoad_*` storage and helper API into
  `frontPanelParser.c` as the transitional bridge;
- kept `TempPlaybackSwitch.h` as the public finalizer declaration for the
  sequencer-facing call site;
- verified the STM32 build with `make -C mainboard/LxrStm32 -j4 stm32`.

## AVR-Side Processes That Currently Feed This System

The AVR side does not directly call `PresetLoadCache`, but it still initiates
and participates in the protocol flows that now feed the parser-local
transitional bridge in `frontPanelParser.c`.

### Front-panel protocol initiators

In `front/LxrAvr/frontPanelParser.c`:

- `frontPanel_prfCacheBegin()` starts the PRF cache/session handshake.
- `frontPanel_prfCacheControl()` sends the PRF cache command and waits for the
  flow acknowledgement.
- `frontPanel_flowBeginSession()`, `frontPanel_flowEndSession()`, and
  `frontPanel_flowAbortSession()` gate the file/session transport.
- `PRF_CACHE_STATUS` responses are consumed there and drive the session
  bookkeeping.

These are the AVR-facing initiators that will need to be reconnected to the new
background-load mechanism after the parser-local bridge is retired.

### AVR preset/file-load flows

In `front/LxrAvr/Preset/presetManager.c`:

- `preset_loadDrumset()`
- `preset_loadAll()`
- `preset_loadPerf()`
- `preset_loadPattern()`
- `preset_saveDrumset()`
- `preset_saveAll()`

These are the file-load and save flows that currently drive the load protocol
traffic.

The important observation is that the AVR side is mostly an initiator and menu
preserver here, not the canonical owner of the load-session state.

### AVR menu preservation during file load

Also in `presetManager.c`:

- `preset_shouldPreserveMenuEndpointsDuringFileLoad()`
- `preset_saveMenuEndpointsDuringFileLoad()`
- `preset_restoreMenuEndpointsDuringFileLoad()`

These helpers are important because they show that some loads are really
temporary menu-preservation operations rather than true storage-mode changes.
That distinction matters for Phase 7 because pattern-only background loads
should not disturb parameter storage mode.

## Phase 7: Eliminate `PresetLoadCache`

### Goal

Make `PresetLoadCache` completely redundant by moving the surviving behavior
into the real owners, deleting the shared cache module, and leaving only the
parser-local bridge in `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c`
as a temporary holdover until the follow-on consolidation pass absorbs it.

### What Changes

The current background-load bridge is doing three different jobs at once:

- holding a shared PRF session cache;
- mirroring live pattern and MIDI state;
- replaying deferred performance traffic after a file-load bracket.

Phase 7 will split those responsibilities back into the modules that already
own the actual data:

- `KitState` remains the normal/temp kit image owner.
- `PatternData` remains the normal/temp pattern owner.
- `TempPlaybackSwitch` remains the active-source and boundary-switch owner.
- `ParameterIngress` remains the live-vs-restore router.
- `EndpointRestore` remains the AVR push-up and display-sync worker.
- `frontPanelParser.c` keeps only parser-side load flow and thin compatibility
  routing while the direct-owner calls are being wired.

Direct kit-only background or delayed loading is removed entirely. If kit data
is loaded by itself, that should either be a foreground operation or part of a
larger preset/file flow; it should not continue to exist as its own
background-load mode.

### Implementation Plan

1. Remove the shared cache module and its public surface.
   - Delete `mainboard/LxrStm32/src/Preset/PresetLoadCache.c`.
   - Delete `mainboard/LxrStm32/src/Preset/PresetLoadCache.h`.
   - Remove the `presetLoad_*` declarations from the callers that only used the
     shared cache API.
   - Move the remaining `presetLoad_*` helper API into
     `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c` as the
     transitional bridge while the direct-owner calls are wired.

2. Replace the live snapshot mirror with direct owner reads.
   - Delete `presetLoad_capturePrfStmLiveSnapshot()`,
     `presetLoad_prfCacheUseLivePattern()`,
     `presetLoad_prfCacheTrackUsesLivePattern()`,
     `presetLoad_prfCacheLiveStep()`,
     `presetLoad_prfCacheLiveMainSteps()`,
     `presetLoad_prfCacheLiveLengthRotate()`,
     `presetLoad_prfCacheLivePatternSetting()`,
     `presetLoad_prfCacheLiveMidiChannel()`,
     `presetLoad_prfCacheLiveNoteOverrideValue()`,
     `presetLoad_prfCacheTakeLiveVMorphFlag()`, and
     `presetLoad_prfCacheLiveVMorphAmountValue()`.
   - Have `Sequencer`, `MidiParser`, and `MidiVoiceControl` read the live state
     directly from `seq_patternSet`, `seq_perTrackActivePattern`,
     `seq_stepIndex`, `midi_MidiChannels`, `midi_NoteOverride`,
     `seq_vMorphAmount`, and `seq_vMorphFlag` instead of consulting a cached
     mirror.
   - Keep the protected temp-pattern logic in the real pattern owner rather
     than in a shadow snapshot.

3. Rewire file-load ingress around explicit load kinds.
   - Keep the ingress mode toggles in `ParameterIngress` and
     `TempPlaybackSwitch` as the only shared routing state.
   - `.prf` and `.all` loads write the normal kit and pattern images through the
     existing owner APIs.
   - `.pat` loads stage pattern data only and must not switch parameter
     read/write away from normal storage.
   - If a single discriminator is still needed, add it to the parser-side load
     flow, not as a new shared cache struct.

4. Fold the deferred-performance path into direct owner calls.
   - Keep the deferred `MidiMsg` replay queue only as a transition aid inside
     the parser while the direct-owner path is being connected.
   - Delete the queue and replay helpers once the parser can route those
     messages straight into `ParameterIngress`, `PatternData`, and the existing
     AVR restore path.
   - Do not recreate the old performance cache in a new module.

5. Leave the AVR initiators thin.
   - `frontPanel_prfCacheBegin()`, `frontPanel_prfCacheControl()`,
     `frontPanel_flowBeginSession()`, `frontPanel_flowEndSession()`,
     `frontPanel_flowAbortSession()`, `preset_loadDrumset()`,
     `preset_loadAll()`, `preset_loadPerf()`, and `preset_loadPattern()` should
     remain only as initiation/acknowledgement layers.
   - They must not own background-load state or reconstruct a second session
     model.
   - When the backend no longer needs them, they can collapse into the direct
     owner calls rather than a shared load-cache API.

6. Preserve the async workers that still belong.
   - Keep `MorphEngine` and `EndpointRestore` separate.
   - Keep `TempPlaybackSwitch` only as long as it still reads more clearly than
     folding the boundary logic into the core preset API.

### Current transitional parser-local surface

The functions below are the transitional surface that replaced the deleted
shared module in Session 014 and now need to be folded into the real owners:

- Session control and teardown:
  - `presetLoad_beginFileLoadIngress()`
  - `presetLoad_endFileLoadIngress()`
  - `presetLoad_clearDeferredPerfLoad()`
  - `presetLoad_clearPrfCacheSession()`
  - `presetLoad_clearPrfRuntimeFlags()`
  - `presetLoad_resetPrfPendingCounters()`
  - `presetLoad_prfCacheSessionActive()`
  - `presetLoad_prfCacheCanExit()`
- Deferred performance replay:
  - `presetLoad_isDeferredPerfControlMessage()`
  - `presetLoad_shouldDeferPerfMessage()`
  - `presetLoad_cacheDeferredPerfMessage()`
  - `presetLoad_applyDeferredPerfMessages()`
  - `presetLoad_shouldStagePattern()`
  - `presetLoad_markDeferredPatternPending()`
- Live snapshot / state mirroring:
  - `presetLoad_capturePrfStmLiveSnapshot()`
  - `presetLoad_prfCacheUseLivePattern()`
  - `presetLoad_prfCacheTrackUsesLivePattern()`
  - `presetLoad_prfCacheLiveStep()`
  - `presetLoad_prfCacheLiveMainSteps()`
  - `presetLoad_prfCacheLiveLengthRotate()`
  - `presetLoad_prfCacheLivePatternSetting()`
  - `presetLoad_prfCacheLiveMidiChannel()`
  - `presetLoad_prfCacheLiveNoteOverrideValue()`
  - `presetLoad_prfCacheTakeLiveVMorphFlag()`
  - `presetLoad_prfCacheLiveVMorphAmountValue()`
- Legacy voice-cache promotion:
  - `presetLoad_clearVoiceCache()`
  - `presetLoad_clearHeldVoiceLoad()`
  - `presetLoad_unholdVoice()`
  - `presetLoad_uncacheVoice()`
  - `presetLoad_releaseVoiceCache()`
  - `presetLoad_unholdLoadedVoice()`
  - `presetLoad_voiceCachePending()`
  - `presetLoad_applyPendingVoiceCache()`
  - `presetLoad_finalizeTempBackgroundLoad()`

The first two families are the strongest candidates for direct replacement by
the temp pattern / parameter machinery. The last family is mostly legacy
compatibility residue that should disappear once the new route is stable.
Session 014 already cut the runtime caller side over to the direct owner reads;
what remains here is the transitional parser/session path.

### AVR-facing implementation strategy

The AVR initiators do not need to keep the old semantics forever.

They can be kept as thin stubs while the backend is disconnected from
`PresetLoadCache` and the parser-local transitional bridge, as long as they
preserve the user-visible control flow and do not try to emulate the old
session cache in the AVR layer.

That means:

- the initiators can still exist while the backend is being rewired;
- they should not own loader state locally;
- they should not grow a second background-load model;
- they should be reconnected to the new temp-switch / core preset API once the
  replacement path exists.

### The `.pat` wrinkle

Pattern background loading is the one special case that must be called out
explicitly.

`.pat` background loading should never switch preset parameter read/write away
from normal storage.

That means:

- pattern data can be staged in pattern-owned temp structures;
- parameter ingress should remain on normal storage for `.pat` background loads;
- temp/normal parameter ownership and pattern temp/normal ownership must stay
  separate;
- the new interface may need a small explicit mode bit or load-kind enum to
  describe "pattern-only background load" without implying parameter storage
  migration.

This is the one place where the current interfaces may need a small shape
change, even though the ownership rule itself is straightforward.

### The future menu target

Do not implement this now, but this is the intended UI target after the backend
is simplified:

- group name: `Bkground`
- long name: `FileLoad`
- values:
  - `0` = `off`
  - `1` = `pat`
  - `2` = `prf`
  - `3` = `all`
  - `4` = `al3`

This future control should replace the current "File Load Fast" concept rather
than adding another mode on top of it.

### Phase 7 completion result

- Completed in Session 014: `PresetLoadCache.c/h` no longer exist.
- Completed in Session 014: the shared-module `presetLoad_*` surface is gone.
- The parser-local bridge in `frontPanelParser.c` is now the remaining
  transitional holdover until the follow-on consolidation pass absorbs it.
- Pattern-only background loading keeps parameter read/write on normal storage.
- The codebase has one parser-owned transitional bridge, not a second shared
  cache module.

### Scope Check

- The parser-local bridge in `frontPanelParser.c` is the Phase 7
  transitional holdover, not a separate shared module.
- `ParameterArray.c/h` is now part of the completed Phase 8 landing zone, not
  a Phase 7 cache-removal add-on.

## Phase 8: Consolidate The Remaining `/Preset/` Split (Completed)

Phase 8 is now complete. The material below captures the structural cleanup
that Session 015 finished so the remaining Phase 9 naming work has the exact
final shape in view.

### Primary target

Create a single consolidated preset ownership API for the storage model and the
non-async routing logic.

The exact file name can be decided during implementation, but the intended
shape is:

- one authoritative core file pair that owns preset storage structures and the
  main preset routing API;
- separate async workers only where they are actually justified.

### Phase 8 coordination plan

Phase 8 should be treated as one coordinated structural pass, not as a loose
collection of file moves.

Recommended order:

1. Move `ParameterArray.c/h` under `/Preset/` first.
   - This is the widest include fan-out and the clearest ownership fix.
   - Keep the public include path stable during the move if a compatibility
     shim is needed.
   - Do not combine the move with a semantic rename unless the old include path
     can stay as a bridge for one transition step.
2. Decide whether `ParameterMap.c/h` stays public or becomes an internal
   implementation detail of the consolidated preset core.
   - If it stays visible, it should be a thin helper, not a second subsystem.
   - If it collapses, the parameter-classification API should live next to the
     consolidated preset state instead of as a free-standing mapping layer.
3. Collapse `ParameterIngress.c/h` around a single routing entry point.
   - Keep live-vs-restore routing, normal-vs-temp storage selection, and
     automation-sideband refresh in one place.
   - Avoid splitting the ownership rules between the new core and a second
     router.
4. Narrow or fold `TempPlaybackSwitch.c/h`.
   - This module already carries the boundary state machine; the open question
     is whether that state belongs in the core or in a tiny helper.
   - If it survives, it should own one explicit state struct rather than a pile
     of exported flags.
5. Replace the remaining flat `seq_*` transition flags with an explicit
   boundary/state object.
   - The state object should capture pending pattern selection, per-track
     pending selection, load gating, and ack/execution state together.
   - Keep the machine observable through query/update helpers, not through a
     scatter of globals.
6. Leave `MorphEngine.c/h` and `EndpointRestore.c/h` separate.
   - Their async ownership, queueing, and timing make them different from the
     storage/routing collapse work.
   - Do not broaden Phase 8 into worker rewrites.

### Session 015 Result

Session 015 completed Phase 8:

- `ParameterArray.c/.h` is now the preset-owned home for the parameter table
  and the voice/selector classification helpers.
- `ParameterMap.c` and `ParameterMap.h` are both gone.
- `mainboard/LxrStm32/src/MIDI/ParameterArray.h` is gone as well, so the old
  MIDI-side include path no longer exists.
- `TempPlaybackSwitch` now stores the boundary flags in one explicit state
  object and keeps the legacy names only as macros.
- The STM32 build is green after a clean rebuild.

`ParameterIngress` is already narrow enough for the structural phase and is
now treated as the stable router boundary for the follow-on naming work.

Any further `ParameterIngress` extraction would be a later functional
refinement, not a Phase 8 dependency.

### What Phase 8 absorbed

#### `ParameterArray.c/h`

Move `mainboard/LxrStm32/src/MIDI/ParameterArray.c/h` into `/Preset/`.

Why:

- it is part of preset storage and parameter application;
- it is not a MIDI transport concern;
- it already depends on preset-backed parameter metadata and DSP ownership.

This is one of the clearest moves in the consolidation pass.

Coordination notes:

- The include fan-out currently reaches `main.c`, `sequencer.c`,
  `uARTFrontSYX/frontPanelParser.c`, `DSPAudio/modulationNode.h`,
  `DSPAudio/DrumVoice.c`, and `Preset/KitState.h`.
- Because of that fan-out, this move likely needs either a compatibility
  header or a very small include-path bridge while the call sites are updated.
- Keep the move semantically neutral; do not mix it with a behavior change or
  a data-model rename in the same patch if that can be avoided.

Session 015 progress:

- `mainboard/LxrStm32/src/Preset/ParameterArray.c/.h` now owns the parameter
  table and the parameter-classification helpers.
- `mainboard/LxrStm32/src/Preset/ParameterMap.c` has been deleted.
- `mainboard/LxrStm32/src/Preset/ParameterMap.h` has been deleted as well, so
  the old helper module no longer survives as a compatibility shell.
- `mainboard/LxrStm32/src/MIDI/ParameterArray.h` has been deleted, and the
  callers that were touched now include `Preset/ParameterArray.h` directly.
- The deprecation path has therefore completed for the parameter-table move.

#### `ParameterMap.c/h`

Fold the parameter classification helpers into the consolidated preset core, or
keep them as thin internal helpers only if that keeps the final header
manageable.

The key point is that `ParameterMap` is metadata for the same ownership domain
as `KitState`; it should not remain a separate conceptual island.

Coordination notes:

- `ParameterMap` is already consumed by `MorphEngine`, `ParameterIngress`, and
  the compatibility wrappers in `sequencer.h`, so the public shape is already
  mostly about preset ownership, not transport.
- The preferred outcome is to make the mapping logic an internal detail of the
  new core, or at least a very thin helper that does not look like a separate
  subsystem.
- Do not duplicate the canonical selector and voice classification logic in
  more than one file during the transition.

Implementation status:

- The helper implementations now live in `Preset/ParameterArray.c`.
- `ParameterMap.h` no longer exists.
- The next cleanup step is no longer a Phase 8 concern; `ParameterIngress`
  stays as the stable narrow router unless a later phase decides to extract it
  further.

#### `ParameterIngress.c/h`

Fold the raw ingress routing into the consolidated preset core, or make it a
very narrow helper around the same core state.

This should be the one place that decides:

- whether an incoming byte is live or restore traffic;
- whether it lands in normal or temp storage;
- whether automation selector bytes should refresh the matching sideband
  structures.

Coordination notes:

- This is the policy boundary that keeps live-vs-restore and normal-vs-temp
  behavior coherent.
- If this module survives, it should be a tiny routing layer around the core,
  not a second policy engine.
- Keep the automation sideband refresh close to the route decision so raw
  selector bytes and resolved endpoint structures do not drift apart.

#### `TempPlaybackSwitch.c/h`

Collapse or sharply narrow this module.

The current file already owns pattern-boundary state and source-selection state,
so it is closer to a session/boundary layer than to a tiny helper.

Phase 8 should decide one of two outcomes:

- fold it into the consolidated preset core; or
- keep only a minimal boundary helper if a separate file still improves
  readability after the loader redundancy is gone.

The important thing is that it should not remain a second large state-machine
module beside the new consolidated core.

Coordination notes:

- The current exported globals are already acting like a state-machine
  boundary, so the first goal is to stop exposing them as a loose pile of
  externs.
- If the module survives, it should own a single explicit state object, with
  setters and getters layered on top of it.
- The fields that should travel together are the selection state
  (`seq_pendingPattern`, `seq_perTrackPendingPattern[]`), the transition state
  (`seq_newPatternAvailable`, `seq_newPatternExecuted`), the load gate
  (`seq_loadPendingFlag`, `seq_loadSeqNow`), and the temp-boundary ack
  (`seq_tmpBoundaryPatternSwitchAck`).
- The parser currently touches some of those flags directly, so the call sites
  will need to move to the new API in the same pass that introduces the state
  object.

Implementation status:

- The temp-switch booleans now live inside one
  `SeqTempPlaybackSwitchState` struct.
- The legacy `seq_*` names remain as macros for compatibility, but the actual
  storage is no longer a pile of stand-alone globals.
- `TempPlaybackSwitch.c` and the parser/sequencer callers now compile against
  the single state object without changing the runtime behavior.

### What should stay separate

#### `MorphEngine.c/h`

Keep this separate.

Reason:

- it is an async morph drain worker;
- it owns live-apply suppression;
- it is not the same kind of code as storage or load/session routing.

#### `EndpointRestore.c/h`

Keep this separate.

Reason:

- it is an async AVR push-up worker;
- it owns a queue and handshake state machine;
- it is not part of the storage model itself.

### Structural collapse candidates

The main consolidation candidates in Phase 8 are not just file moves; they are
also state-shape reductions.

#### Candidate 1: `ParameterArray` as a true preset-owned storage table

Likely outcome:

- move the file into `/Preset/`;
- keep a compatibility include if the fan-out is too broad to update in one
  step;
- consider a later rename only after the include churn has settled.

Why this matters:

- it removes the last obvious "MIDI directory" ownership mismatch from the
  preset data model;
- it makes the storage table line up with the other preset-owned structures;
- it reduces the risk of future code recreating a second parameter table in a
  different subsystem.

#### Candidate 2: `ParameterMap` as an internal detail of the core

Likely outcome:

- collapse the helper into the new core, or keep it as a private helper file
  that is not conceptually separate;
- avoid a public header unless the remaining call sites genuinely need one.

Why this matters:

- the mapping logic is pure metadata for the same preset ownership domain as
  `KitState`;
- a separate public module would keep the conceptual split alive even after the
  file move work is done.

#### Candidate 3: `ParameterIngress` as the canonical routing entry point

Likely outcome:

- keep one routing API that decides live vs restore and normal vs temp
  placement;
- if the core absorbs it, the ingress module should disappear rather than
  becoming a second router.

Why this matters:

- this is the policy boundary that keeps parameter writes coherent;
- duplicate ingress policy would make the normal/temp and live/restore rules
  drift apart again.

#### Candidate 4: `TempPlaybackSwitch` as a compact boundary state object

Likely outcome:

- either fold the module into the core or reduce it to a tiny helper around a
  single explicit state object;
- replace direct extern use with query/update functions.

Why this matters:

- the module is already carrying pattern-boundary and source-selection logic;
- the current global flags are the most obvious place where a single state
  object would remove ambiguity.

#### Candidate 5: the remaining `seq_*` transition globals as one explicit machine

Likely outcome:

- group the transition flags into one state struct rather than several related
  booleans;
- expose the state through helpers so the transition rules stay visible and
  testable.

Why this matters:

- it reduces the chance that one caller updates half the transition state and
  leaves the rest stale;
- it makes the phase boundary between selection, execution, and acknowledgement
  explicit instead of implicit.

### What Phase 8 should remove

Phase 8 should actively reduce the number of flat globals and compatibility
flags that remain after Phase 7.

Good candidates for structification or collapse:

- `seq_loadPendingFlag`
- `seq_loadSeqNow`
- `seq_newPatternAvailable`
- `seq_newPatternExecuted`
- `seq_tmpBoundaryPatternSwitchAck`
- the remaining `seq_*` pattern/session flags that still encode one small
  state machine as a pile of booleans
- the remaining flat snapshot mirrors in the old load/session logic
- direct external writes to the temp-boundary transition flags where a setter
  would make the ownership clearer

The exact reduction can be staged, but the goal is explicit state, not more
alias variables.

### Phase 8 exit criteria

- `ParameterArray.c/h` lives under `/Preset/`.
- The remaining preset storage and routing code is centralized into one core
  API.
- `ParameterMap` is gone rather than merely hidden behind a compatibility
  shell.
- `TempPlaybackSwitch` now exposes one explicit state object instead of a pile
  of exported globals.
- `ParameterIngress` is the remaining narrow router candidate, and its final
  shape can be judged independently now that the other consolidation slices are
  in place.
- The loader/session redundancy from Phase 7 is gone, so the remaining preset
  files are either the core API or true async workers.
- `TempPlaybackSwitch` is no longer a second overlapping state machine unless
  a very narrow boundary helper is still justified.

## Phase 9: Normalize Preset-Owned Prefixes

Phase 9 is the naming cleanup pass that follows the structural Phase 8 work.
It exists to catalog the remaining stale `seq_` / `Seq` ownership signals in
`/Preset/` and remove the ones that are clearly just internal state names, while
leaving compatibility entry points intact until the broader API cutover is
ready. The rule for exported Preset API is now stricter: public functions and
variables from `/Preset/` should be `preset_`-prefixed, not `seq_`-prefixed.

### Goal

Make the mutable Preset-owned state read as Preset-owned state, not as state
that Sequencer still controls directly.

### What belongs here

- Rename the explicit state objects, queues, cursors, and mailboxes that are
  already Preset-owned but still carry `seq_` names.
- Rename the exported Preset entry points so `/Preset/` exports read as
  `preset_` APIs, even if compatibility wrappers continue to exist elsewhere in
  the Sequencer facade for one transition step.
- Keep the public compatibility wrappers stable when they are still needed by
  the old call graph.
- Catalog the remaining `Seq`-prefixed type names and the larger restore-queue
  mirror separately so they can be removed in a deliberate follow-up pass.

### First cleanup slice

- `TempPlaybackSwitch` already exposes `preset_tempPlaybackSwitchState`; the
  remaining Phase 9 task is to rename the exported helper surface so the
  boundary logic is no longer advertised as `seq_`-owned.
- `EndpointRestore` should rename the queue, cursor, phase, and wait-state
  storage away from `seq_` so the ownership boundary is visible in the storage
  names themselves.
- The exported endpoint-restore entry points should become `preset_` APIs
  while `sequencer.h` keeps any necessary `seq_` compatibility bridge for one
  transition step.
- Keep the broader restore-queue/cursor compatibility island for the follow-up
  inventory unless it can be collapsed without widening the API surface.
- Leave the `SeqKitState` and `SeqEndpointRestoreRequest` type names alone for
  now unless a rename is required to support one of the public API changes
  above.

### Phase 9 coordination plan

Phase 9 should be treated as one coordinated prefix-normalization pass, not as
ad hoc symbol scrubbing.

Recommended order:

1. Rename the private `EndpointRestore` storage first.
   - Convert the internal queue, cursor, phase, and timeout fields to
     `preset_` names.
   - Keep the queue behavior unchanged; this is a naming pass, not a policy
     rewrite.
   - Update the local helper names only where the new storage names would
     otherwise be misleading.
2. Rename the public `EndpointRestore` API.
   - `seq_serviceEndpointRestore()` should become a `preset_`-prefixed entry
     point.
   - `seq_endpointRestoreBusy()`,
     `seq_pushEndpointUpdateForVoiceSourceChange()`, and
     `seq_maybePushKitEndpointsToFrontWithGlobalMorphReport()` should follow
     the same rule.
   - Keep `sequencer.h` compatibility wrappers only long enough for the old
     callers to migrate.
3. Rename the `TempPlaybackSwitch` helper surface.
   - The temp-switch orchestration helper should stop exporting as `seq_`.
   - Because `preset_setTmpKitActive()` already exists in `KitState`, the
     renamed helper should use a distinct `preset_` verb rather than colliding
     with the storage setter.
   - The legacy `seq_*` aliases around the boundary flags can stay as
     compatibility macros until the caller migration is complete.
4. Update the direct call sites.
   - `main.c` should call the new `preset_` endpoint-restore service entry
     point.
   - `sequencer.c` should switch to the new `preset_` helper names where it
     still calls into the Preset-owned boundary logic directly.
   - Any parser-side callers that reach the restore service or temp-switch
     helpers should follow the same rename in the same pass.
5. Retire the old exported `seq_` names from `/Preset/`.
   - Leave compatibility wrappers in the Sequencer facade only where a later
     phase still needs them.
   - Once the callers are migrated, the Preset headers should advertise only
     `preset_` exports for the owned surface.

### Exit criteria

- The obvious Preset-owned mutable state no longer advertises Sequencer as the
  owner through its storage names.
- The exported Preset API uses `preset_` names, not `seq_` names.
- The remaining `seq_` / `Seq` references are either compatibility wrappers or
  explicitly cataloged holdovers for the next cleanup pass.
- The `EndpointRestore` queue/cursor state and the `TempPlaybackSwitch`
  boundary helper no longer look like Sequencer-owned internals.

### Session 015 prefix cleanup note

- `TempPlaybackSwitch` now stores its boundary flags in
  `PresetTempPlaybackSwitchState preset_tempPlaybackSwitchState`, so the owned
  state object no longer carries a sequencer-prefixed storage name.
- The AVR restore handshake mailboxes now live as
  `preset_tmpKitHandshakeReady` and `preset_tmpKitHandshakeAck`.
- The broader restore-queue/cursor prefix scrub remains cataloged for the
  follow-up Phase 9 inventory pass so we can decide whether to collapse it
  separately from the live compatibility mailboxes.
- The remaining `seq_tmpKitPushParamsToFrontEnabled` and
  `seq_endpointRestore*` queue/cursor names are still part of that inventory
  set and are now explicitly in Phase 9 scope.

### Phase 9 exit criteria

- The obvious Preset-owned mutable state no longer advertises Sequencer as the
  owner through its storage names.
- The exported Preset API uses `preset_` names, not `seq_` names.
- Any remaining `seq_` / `Seq` references are either private helpers or
  explicitly cataloged holdovers for the next cleanup pass.
- The transition wrappers in the Sequencer facade are the only place where
  temporary `seq_` API names remain acceptable.

### Session 017 Result

Session 017 completed the Phase 9 renaming pass:

- `Preset/EndpointRestore.c/.h` now export `preset_serviceEndpointRestore()`,
  `preset_endpointRestoreBusy()`,
  `preset_pushEndpointUpdateForVoiceSourceChange()`, and
  `preset_maybePushKitEndpointsToFrontWithGlobalMorphReport()`.
- `Preset/TempPlaybackSwitch.c/.h` now export
  `preset_setTempPlaybackActive()`, `preset_trackPatternUsesTmp()`,
  `preset_synthVoiceUsesTmpFromTrackPatterns()`,
  `preset_allVoiceSourcesUseTmp()`,
  `preset_allVoiceSourcesUseNormal()`,
  `preset_updateVoiceSourcesForPatternChange()`, and
  `preset_consumeTmpBoundaryPatternSwitchAck()`.
- The endpoint-restore queue, cursor, phase, and timeout state now use
  `preset_` storage names inside `EndpointRestore.c`.
- `main.c`, `frontPanelParser.c`, and `sequencer.c` now call the new
  `preset_` entry points where they reach the Preset-owned boundary logic.
- `make -C mainboard/LxrStm32 -j4 stm32` is green after the rename pass.

## Phase 10: Rehome Live Apply Helpers Into Existing Preset Boundaries

Phase 10 still removes the remaining live automation bridge from `sequencer.c`,
but the split should follow the existing Preset ownership rules instead of
creating a new automation-only file.

- if outside code needs to store or change Preset-owned data, the write should
  go through `ParameterIngress.c`;
- if Preset state needs to drive an emission elsewhere, `KitState.c` should
  expose the state accessors or setters that prompt that emission;
- live DSP application belongs with the existing Preset worker/state code that
  already owns morphing and boundary refresh behavior.

### Ownership split

- `ParameterIngress.c` remains the ingress-side home for external writes into
  Preset-owned endpoint and automation storage.
- `KitState.c` remains the state-selection home for current-image, temp-image,
  and voice-source queries, plus any setter that should trigger later emission.
- `MorphEngine.c` remains the live DSP application home for automation-target
  updates and parameter replay.
- `TempPlaybackSwitch.c` remains the state machine that decides when source
  ownership changes and when live reapply work should be requested.
- `EndpointRestore.c` remains the AVR/front-panel restore emitter.

### Functions that must move or be re-homed

- `seq_applyVoiceAutomationTargets()` becomes `preset_applyVoiceAutomationTargets()`
  in the Preset live-apply path, with the current LFO-node selector logic moved
  alongside it.
- `seq_applySharedAutomationTargets()` stays private to the same live-apply
  implementation and should not remain as a Sequencer-owned helper.
- `seq_applyNormalEndpointAutomationTargets()` becomes a Preset-owned boundary
  replay helper that is called when a temp-kit or restore boundary closes.
- `seq_applySingleParameterValue()` already lives in `ParameterIngress.c`, so
  Phase 10 should only remove the Sequencer-facing call sites that still route
  through the old wrapper.

The important detail is that the live application logic should remain inside
the existing Preset modules that already own the relevant caches and state. The
goal is to remove the Sequencer facade, not to invent a separate layer just for
automation-node bridging.

### Call sites to update

- `mainboard/LxrStm32/src/Preset/MorphEngine.c` should call
  `preset_applySingleParameterValue()` directly for live parameter writes.
- `mainboard/LxrStm32/src/Preset/MorphEngine.c` should own the body of
  `preset_applyVoiceAutomationTargets()` and its shared-target helper, instead
  of relying on Sequencer to perform DSP destination updates.
- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c` should request the
  Preset live-apply helper directly when a voice source changes.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c` should call the
  Preset boundary replay helper directly when the temp-kit endpoint bracket
  closes.
- any path that still needs to know whether the live image is normal or temp
  should ask `KitState.c` instead of peeking at Sequencer globals.

### Sequencer cleanup

- remove the live-apply function bodies from `sequencer.c`;
- remove or reduce the matching `seq_` declarations in `sequencer.h` once the
  call sites have moved;
- keep `seq_parseAutomationNodes()` in `Sequencer`, because it still reads the
  step automation payload out of the pattern data;
- keep the pattern-reader and clocking helpers in `Sequencer`;
- do not move the disabled `#if 0` legacy reference block yet, because it is
  only migration history and not part of the active call graph.

### Compatibility rule

- Phase 10 must not introduce any new `seq_`-prefixed public API for live
  automation application.
- If a temporary compatibility wrapper is still needed while the call sites are
  being migrated, it must be a short-lived bridge only, not the new canonical
  entry point.
- The canonical API for the moved functionality must be the `preset_` form,
  even if the implementation lands inside an existing Preset source file.

### Explicitly deferred

- `sequencer_sendVMorph()` can stay in Sequencer for this phase. It is a thin
  dispatch helper used by the Sequencer morph loop, not the main automation
  target bridge that Phase 10 is extracting.
- The broader `seq_` facade cleanup and any final wrapper removal can wait for
  the follow-on naming pass if we want to collapse the compatibility layer more
  aggressively.

### Exit criteria

- no active Preset caller reaches Sequencer for live automation-target
  application;
- the live apply path is owned by Preset code that already manages the relevant
  state and caches;
- `MorphEngine` uses Preset-owned parameter writes directly;
- `TempPlaybackSwitch` and `frontPanelParser` trigger boundary replay through
  Preset-owned helpers, not Sequencer wrappers;
- the remaining `seq_` live-apply names, if any survive temporarily, are
  clearly compatibility-only.

### Verification

- build the firmware after the move;
- exercise a temp-kit boundary and confirm the normal endpoint replay still
  happens when the boundary closes;
- exercise a live morph/automation update and confirm voice-specific targets
  and single-parameter writes still reach the DSP;
- confirm that temp playback source changes still propagate the correct live
  targets without relying on Sequencer-owned live-apply helpers.

### Session 018 Result

Session 018 implemented the Phase 10 live-apply rehome:

- `Preset/MorphEngine.c` now exports the live automation bridge via
  `preset_applyVoiceAutomationTargets()` and
  `preset_applyNormalEndpointAutomationTargets()`.
- `Preset/MorphEngine.c` now calls `preset_applySingleParameterValue()`
  directly for live parameter emission instead of routing through the
  Sequencer wrapper.
- `Preset/TempPlaybackSwitch.c` now replays voice automation through the
  Preset-owned live bridge.
- `uARTFrontSYX/frontPanelParser.c` now calls the Preset-owned normal-endpoint
  replay helper when the temp-kit endpoint bracket closes.
- `Sequencer/sequencer.c` no longer owns the active live-apply implementations,
  and the old `seq_apply*` bridge declarations were removed from
  `sequencer.h`.
- `seq_applySingleParameterValue()` remains only as a documented compatibility
  shim in `sequencer.h`; new live-emission code now calls the Preset-owned
  entry point directly.
- `make -C mainboard/LxrStm32 -j4 stm32` is green after the move.

## Phase 11: Rename Remaining Preset Exports To `preset_`

Session 019 completed the remaining Preset-owned rename work and verified the
tree still builds cleanly.

### Completed changes

- `KitState.h` and `KitState.c` now export `PresetKitState` and
  `PresetAutomationTargets` instead of the old `Seq*` type names.
- `EndpointRestore.h` and `EndpointRestore.c` now use `PresetKitState` and
  `PresetEndpointRestoreRequest` throughout the restore queue and push-up
  helpers.
- `MorphEngine.h` and `MorphEngine.c` now use `PresetKitState` and
  `PresetAutomationTargets` in all signatures and locals.
- `ParameterIngress.h` and `ParameterIngress.c` now use `PresetKitState` and
  `PresetAutomationTargets` in their ingress helpers.
- `TempPlaybackSwitch.h` no longer exports the temporary `seq_*` aliases for
  Preset-owned boundary state; callers use `preset_tempPlaybackSwitchState.*`
  directly.
- `TempPlaybackSwitch.c` now reads and writes `preset_voiceSourceState`
  directly.
- `sequencer.h` no longer re-exports Preset-owned state as `seq_*` aliases and
  its compatibility wrappers now return `PresetKitState*`.
- `sequencer.c`, `frontPanelParser.c`, `MidiParser.c`, and
  `modulationNode.c` were updated to use `preset_*` names directly for the
  Preset-owned morph and temp-switch state.
- `ParameterArray.c` now binds the morph parameters to `preset_vMorphAmount`.
- The STM32 build passes after the rename sweep.

### Exit criteria

- No exported `/Preset/` symbol uses a `seq_` prefix.
- `TempPlaybackSwitch.h` no longer exposes `seq_` aliases for Preset-owned
  state.
- `sequencer.h` no longer re-exports Preset-owned state or handshake mailboxes.
- `sequencer.c` no longer reaches Preset-owned state through `seq_` aliases.
- `KitState.h` no longer exports `SeqKitState` or `SeqKitAutomationTargets`,
  and `EndpointRestore` no longer uses `SeqEndpointRestoreRequest`.
- any remaining `seq_` names in the tree are Sequencer-owned APIs or
  explicitly documented compatibility shims outside `/Preset/`.

## Phase 12 Retrospective: Sequencer Boundary Review

Phase 12 is no longer an extraction target. The review question that remains is
where the line should stay between Sequencer orchestration and Preset-owned
state access.

The `seq_live*` helpers in `sequencer.c` are best understood as orchestration
helpers, not Preset accessors. They combine pattern selection, active-track
selection, and timing context before reading pattern data. That makes them a
reasonable Sequencer responsibility, even though they read Preset-owned data.

The main follow-up items to keep in mind for later review are:

- whether the remaining `seq_` compatibility wrappers in `sequencer.h` can be
  narrowed further;
- whether any of the pure pass-through wrappers should be collapsed once the
  Phase 9 rename work is complete;
- whether a future phase wants to revisit the `seq_get*` compatibility layer
  separately from the orchestration helpers.
- whether the remaining front-panel UART emitters in `sequencer.c`,
  `EndpointRestore.c`, `MidiParser.c`, `MidiVoiceControl.c`, and
  `frontPanelParser.c` should be tracked only as protocol-split work instead of
  being mistaken for more Preset ownership cleanup;
- whether the send-side protocol split should absorb the repeated
  `uart_sendFrontpanel*` composition patterns into a single reusable helper
  surface before any more ownership work is attempted.

The key conclusion for this audit is that the pattern/state lookup helpers in
`sequencer.c` should stay in Sequencer unless a later refactor reveals a more
specific ownership split.

One extra boundary note from the UART review: the remaining direct front-panel
transmit calls in the STM code are not a Phase 12 Preset target. They should be
tracked as protocol work in `MIDI_UART_SPLIT_AUDIT.md`, because they are about
packet composition and transport policy rather than about who owns the preset
state itself.

## Protocol Split Tracking

The front-panel UART / MIDI parser split work is tracked in
[MIDI_UART_SPLIT_AUDIT.md](MIDI_UART_SPLIT_AUDIT.md). That file now owns the
send/receive and MIDI channel/global split planning so this audit can stay
focused on Preset ownership and naming.

## Review Notes

Before implementation, the main questions to keep in mind are:

1. Should the consolidated preset core be named `PresetCore`, `PresetState`, or
   something even more explicit?
2. Should `TempPlaybackSwitch` disappear entirely into the consolidated core,
   or survive only as a tiny boundary helper?
3. Should the `.pat` background-load path get a dedicated mode bit, or should
   the consolidated core expose a single load-kind enum that covers pattern-only
   versus parameter+pattern loads?
4. Should `ParameterArray` be renamed as part of the move, for example to make
   it read more like preset metadata than a MIDI utility?
5. Which remaining `seq_*` compatibility wrappers are worth narrowing next, and
   which should stay as transitional glue until a later pass?

## Summary

The consolidation strategy is now:

- Phase 7: remove `PresetLoadCache` as a shared module and keep only the
  parser-local transitional bridge until the direct-owner rewrite is complete,
  reusing the existing temp pattern and parameter structures instead of a
  separate cache/session model. Session 014 already cut the Sequencer and MIDI
  runtime callers over to direct owner reads and deleted the shared cache
  module; the remaining work is to retire the parser-local bridge.
- Phase 8: consolidate the remaining `/Preset/` files into one core ownership
  layer plus the true async workers. Session 015 completed that work by moving
  `ParameterArray` into `/Preset/`, deleting the old `ParameterMap` split, and
  folding the temp-switch flags into a single state object. Phase 8 is now
  closed.
- Phase 9: normalize the Preset-owned public surface. Session 017 landed the
  `preset_` endpoint-restore and temp-switch exports, renamed the Preset-owned
  restore state storage, and kept the temporary compatibility bridge only
  where the Sequencer facade still needs it.
- Phase 10: rehome the live-apply helpers into the existing Preset boundary
  modules.
- Phase 11: rename the remaining Preset exports to `preset_`. Session 019
  completed that rename sweep and verified the build.
- Phase 12: retrospective review of the Sequencer/Preset boundary; no
  extraction target is active.
- Protocol and parser split planning now lives in
  `MIDI_UART_SPLIT_AUDIT.md`.

That is the cleanest path to the single consolidated preset API you want
without trying to solve every cleanup problem at once.
