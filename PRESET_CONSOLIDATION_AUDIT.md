# PRESET_CONSOLIDATION_AUDIT

Date: 2026-06-13
Status: Phase 7 implementation landed in Session 014; the shared cache module is gone and Phase 8 now owns the remaining `/Preset/` consolidation work

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
  into `/Preset/`.
- Phase 9 and beyond: protocol and parser cleanup outside `/Preset/`.

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
- `ParameterArray.c/h` still looks like a Phase 8 item; it is not a cheap add-on
  to the cache-removal pass.

## Phase 8: Consolidate The Remaining `/Preset/` Split

Phase 8 is the rest of the cleanup that the current audit already anticipated.
It should happen after Phase 7 because it is much easier to consolidate the
remaining preset API once the loader/session redundancy is gone.

### Primary target

Create a single consolidated preset ownership API for the storage model and the
non-async routing logic.

The exact file name can be decided during implementation, but the intended
shape is:

- one authoritative core file pair that owns preset storage structures and the
  main preset routing API;
- separate async workers only where they are actually justified.

### What Phase 8 should absorb

#### `ParameterArray.c/h`

Move `mainboard/LxrStm32/src/MIDI/ParameterArray.c/h` into `/Preset/`.

Why:

- it is part of preset storage and parameter application;
- it is not a MIDI transport concern;
- it already depends on preset-backed parameter metadata and DSP ownership.

This is one of the clearest moves in the consolidation pass.

#### `ParameterMap.c/h`

Fold the parameter classification helpers into the consolidated preset core, or
keep them as thin internal helpers only if that keeps the final header
manageable.

The key point is that `ParameterMap` is metadata for the same ownership domain
as `KitState`; it should not remain a separate conceptual island.

#### `ParameterIngress.c/h`

Fold the raw ingress routing into the consolidated preset core, or make it a
very narrow helper around the same core state.

This should be the one place that decides:

- whether an incoming byte is live or restore traffic;
- whether it lands in normal or temp storage;
- whether automation selector bytes should refresh the matching sideband
  structures.

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

The exact reduction can be staged, but the goal is explicit state, not more
alias variables.

### Phase 8 exit criteria

- `ParameterArray.c/h` lives under `/Preset/`.
- The remaining preset storage and routing code is centralized into one core
  API.
- The loader/session redundancy from Phase 7 is gone, so the remaining preset
  files are either the core API or true async workers.
- `TempPlaybackSwitch` is no longer a second overlapping state machine unless
  a very narrow boundary helper is still justified.

## Phase 9 And Beyond: Protocol And Parser Cleanup

Once `/Preset/` is consolidated, the remaining cleanup outside it can happen in
smaller, easier-to-review steps.

### `uARTFrontSYX` split

The current `mainboard/LxrStm32/src/uARTFrontSYX/` layout should eventually be
reshaped into six files total:

- `Uart.c`
- `Uart.h`
- `frontPanelReceivingProtocol.c`
- `frontPanelReceivingProtocol.h`
- `frontPanelSendingProtocol.c`
- `frontPanelSendingProtocol.h`

This means the current three protocol-facing files:

- `frontPanelParser.c`
- `frontPanelParser.h`
- `FrontPanelProtocol.h`

should be eliminated or replaced as part of the split.

The point of this later phase is to make the send/receive split obvious in the
filesystem and in the code paths, not to keep the parser doing everything.

### MIDI parser split

Keep general MIDI processing in `MidiParser.c`, but add the channel/global
split around it:

- `ChannelMidiParser.c`
- `ChannelMidiParser.h`
- `GlobalMidiParser.c`
- `GlobalMidiParser.h`

These should mostly be stubbed at first.

Do not redirect the call graph to them too early.

The intent is:

- `MidiParser.c` continues to own the broad MIDI stream handling;
- channel-specific handling can eventually move into the channel parser;
- global-channel handling can eventually move into the global parser;
- the first implementation pass should preserve current behavior and only make
  the ownership split visible.

### Phase 9 exit criteria

- `uARTFrontSYX` has a visible send/receive split in the file layout.
- The MIDI parser split exists without forcing behavior changes too early.
- The `/Preset/` refactor is stable enough that protocol cleanup no longer has
  to carry the loader/session redesign at the same time.

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

## Summary

The consolidation strategy is now:

- Phase 7: remove `PresetLoadCache` as a shared module and keep only the
  parser-local transitional bridge until the direct-owner rewrite is complete,
  reusing the existing temp pattern and parameter structures instead of a
  separate cache/session model. Session 014 already cut the Sequencer and MIDI
  runtime callers over to direct owner reads and deleted the shared cache
  module; the remaining work is to retire the parser-local bridge.
- Phase 8: consolidate the remaining `/Preset/` files into one core ownership
  layer plus the true async workers, including `ParameterArray` into `/Preset/`.
- Phase 9 and beyond: split the front-panel protocol and MIDI parser into
  cleaner send/receive and channel/global layers.

That is the cleanest path to the single consolidated preset API you want
without trying to solve every cleanup problem at once.
