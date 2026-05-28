# PRF_LOAD_CORRECTION_AUDIT

Date: 2026-05-27

## Purpose

This document replaces the earlier deferred `.PRF` correction strategy.

The previous approach tried to keep the newly loaded `.PRF` from leaking by selectively deferring messages and blocking morph senders. `P005.PRF` proves that this is not a strong enough boundary: per-voice morph automation can still cause new sound data to affect the currently playing kit before the intended manual pattern-change commit.

The corrected strategy is cache-first:

- when a `.PRF` is loaded while the sequencer is playing, STM32 creates an explicit live-performance cache;
- playback, morph, morph automation, MIDI routing, and the AVR menu continue to use the cached live state;
- the new `.PRF` may be written into normal STM32 locations during the load, but those locations are not read by the live engine while cache mode is active;
- on manual pattern change or stop, STM32 switches cleanly out of cache mode and the newly loaded `.PRF` becomes live;
- the behavior is controlled by the existing internal policy flag so a later global option can choose between "cache while playing" and ".ALL-like immediate load."

The current internal policy flag is:

```c
frontParser_deferPerfLoadCacheUntilPatternChange
```

## Current Failure

Repro case:

1. Load a previous `.ALL` or `.PRF`.
2. Start the sequencer.
3. Load `P005.PRF`.
4. Some morph-related sound parameters from `P005.PRF` bleed into the currently playing kit before pattern change.
5. After a manual pattern change, the session may recover, but the early parameter bleed is already wrong.

Important detail:

- `P005.PRF` uses per-voice morph as an automation target.
- The problem is architectural, not just one missing guard around `sequencer_sendVMorph()`.
- While the AVR is reading the new file, AVR-side menu/morph buffers can temporarily contain the new `.PRF`; if a live morph command reaches the AVR during that window, it can calculate and send new interpolated sound parameters early.

There is also an independent bug in the checkpoint code:

```c
// mainboard/LxrStm32/src/Sequencer/sequencer.c
if(param2>=PAR_MORPH_DRUM1&&param2<=PAR_MORPH_HIHAT)
{
   voiceNum = (uint8_t)(0x01<<((uint8_t)(param1-PAR_MORPH_DRUM1)));
   sequencer_sendVMorph(voiceNum, val2);
}
```

The `param2` branch computes the voice from `param1`. That should be fixed early because it can misroute per-voice morph automation independently of the PRF cache work.

## Existing Code Landmarks

AVR:

- `front/LxrAvr/Preset/presetManager.c`
  - `preset_loadPerf()`
  - sends `SEQ_FILE_BEGIN` / `SEQ_FILE_DONE`
  - reads metadata, kit defaults, kit morph targets, drumset meta, and all 8 performance patterns
- `front/LxrAvr/Menu/menu.c`
  - `menu_sendAllGlobals()`
  - sends voice MIDI channels and other globals
- `front/LxrAvr/frontPanelParser.c`
  - receives morph/parameter updates from STM32
  - `frontParser_rxDisable` is used during file load

STM32:

- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
  - handles `FRONT_SEQ_FILE_BEGIN` / `FRONT_SEQ_FILE_DONE`
  - currently has the checkpoint deferred PRF machinery:
    - `frontParser_deferPerfLoadCacheUntilPatternChange`
    - `frontParser_deferredPerfLoadActive`
    - `frontParser_deferredPerfProtectedPattern`
    - `frontParser_applyDeferredVoiceCache()`
    - pattern staging through `frontParser_shouldStagePattern()`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
  - reads the active pattern during playback
  - applies manual pattern changes
  - calls `frontParser_applyDeferredVoiceCache()` at pattern change in the checkpoint implementation
  - emits per-voice morph automation through `sequencer_sendVMorph()`
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
  - handles global morph CC and forwards morph changes to AVR

## Correct Target Behavior

### Loading `.PRF` While Stopped

No behavior change.

- `.PRF` loads directly.
- Kit, morph, metadata, globals, and selected pattern data apply immediately.
- The cache pathway is not entered.

### Loading `.ALL` While Playing

No semantic change from the flow-control work.

- `.ALL` is allowed to load into normal state.
- It is not governed by the special `.PRF` live-cache behavior.

### Loading `.PRF` While Playing, Cache Policy Enabled

Expected behavior:

- the currently sounding kit stays unchanged;
- the currently sounding morph state stays usable;
- per-voice morph automation keeps affecting only the old/current performance;
- manual morph keeps affecting only the old/current performance;
- voice MIDI channels and other playback-affecting globals stay at the old/current values;
- the single currently playing pattern stays unchanged;
- the new `.PRF` is received into normal STM32 storage while normal storage is not read by live playback;
- the AVR menu shows the old/current live state after the load completes;
- on manual pattern change or stop, STM32 exits cache mode and the new `.PRF` becomes live;
- after commit, AVR menu values are refreshed to match the newly live `.PRF`.

### Loading `.PRF` While Playing, Cache Policy Disabled

Later global-option behavior:

- `.PRF` behaves like `.ALL`;
- new state applies immediately while playing.

This document only needs to preserve the internal flag and keep that branch possible.

## Corrected Protocol

The names below are placeholders. They can be implemented as new `SEQ_CC` subcommands next to `SEQ_FILE_BEGIN`, `SEQ_FILE_DONE`, and the existing flow-control opcodes.

```c
SEQ_PRF_CACHE_BEGIN          // AVR asks STM32 to enter PRF live-cache mode
SEQ_PRF_AVR_SNAPSHOT_BEGIN   // AVR begins sending live AVR-owned state to STM32 cache
SEQ_PRF_AVR_SNAPSHOT_END     // AVR finished sending live AVR-owned state
SEQ_PRF_STM_SNAPSHOT_READY   // STM32 has captured its own live state
SEQ_PRF_PENDING_BEGIN        // AVR is about to send new PRF data to normal locations
SEQ_PRF_PENDING_DONE         // AVR finished sending new PRF data
SEQ_PRF_CACHE_COMMIT         // STM32 switches out of cache mode, normally internally triggered
SEQ_PRF_CACHE_ABORT          // load failed; STM32 restores/keeps cached live state and leaves cache mode
```

ACKs should use the existing load-session flow-control mechanism where possible. The important rule is that AVR must not mutate/load further until STM32 confirms each boundary that protects live playback.

## Full Load Sequence

### 1. AVR Announces `.PRF` Cache Intent

AVR detects:

- `preset_loadPerf()` is loading a full `.PRF`;
- `menu_sequencerRunning != 0`;
- `frontParser_deferPerfLoadCacheUntilPatternChange` is enabled on STM32.

AVR sends:

```text
SEQ_PRF_CACHE_BEGIN
```

STM32:

- clears any stale PRF cache/session state;
- records the currently active pattern as the protected live pattern;
- disables cache-unsafe morph traffic while the cache is being prepared;
- clears pending per-voice morph flags that might have been generated from half-loaded state;
- prepares storage for:
  - live kit default values;
  - live morph target/default values;
  - current interpolated/applied values;
  - live MIDI/global playback settings;
  - exactly one protected pattern;
- ACKs only after the cache mode boundary is active.

### 2. AVR Sends Current AVR-Owned Live State To STM32 Cache

Before AVR lets the new `.PRF` overwrite its menu/morph arrays, it sends the current live values that STM32 cannot safely reconstruct.

Minimum snapshot:

- kit default parameters from `parameter_values`;
- kit morph target/default values from `parameters2`;
- LFO destination metadata;
- velocity destination metadata;
- drumset metadata needed for morph/menu correctness;
- macro/morph target metadata if used by STM32 or AVR restore;
- voice MIDI channels;
- voice MIDI note overrides;
- any other globals that affect live playback and are owned by the AVR menu.

STM32 stores these values in the PRF live cache and ACKs:

```text
SEQ_PRF_AVR_SNAPSHOT_END ack
```

This snapshot is not the new `.PRF`. It is the old/current performance.

### 3. STM32 Captures Current STM-Owned Live State

STM32 snapshots the state it already owns:

- active protected pattern:
  - main steps;
  - substep data;
  - automation targets and values;
  - length, scale, rotation;
  - pattern chain settings if needed for the protected pattern;
- per-track active pattern selection for the currently sounding state;
- current interpolated/applied voice parameter values;
- current global morph amount;
- current per-voice morph amounts;
- current voice MIDI channel runtime values;
- current note override runtime values;
- relevant sequencer flags needed to keep playback stable.

STM32 then sends:

```text
SEQ_PRF_STM_SNAPSHOT_READY
```

At this point live playback must read from the PRF cache, not from the normal locations that the new `.PRF` will modify.

### 4. AVR Sends New `.PRF` To Normal STM32 Locations

AVR sends:

```text
SEQ_PRF_PENDING_BEGIN
```

Then `preset_loadPerf()` continues reading the file mostly as it does now:

- performance metadata;
- globals;
- pattern main steps;
- shuffle;
- pattern lengths/scales;
- pattern chain;
- kit defaults;
- kit morph targets;
- voice data;
- drumset metadata;
- all 8 pattern step sets.

STM32 receives this into normal `.PRF` locations.

Critical rule:

- while PRF cache mode is active, live playback/morph/automation must not read those normal locations.

This means normal-location writes are acceptable only after the following live readers have explicit cache-aware paths:

- sequencer pattern reads;
- automation target/value reads;
- `seq_vMorphAmount[]` and per-voice morph dispatch;
- global morph dispatch;
- voice MIDI channel/note reads used for currently sounding notes;
- DSP parameter reads or any delayed "new voice available" application path;
- frontpanel/menu restore messages.

### 5. AVR Ends The Pending `.PRF` Send

AVR sends:

```text
SEQ_PRF_PENDING_DONE
```

STM32:

- marks the new `.PRF` as fully received in normal storage;
- keeps playing from PRF cache;
- re-enables morph and morph automation against cached live data only;
- sends cached live state back to AVR so the menu matches what is actually sounding.

AVR:

- unlocks the menu only after the live restore stream is complete;
- displays the cached old/current performance;
- remains aware that a pending `.PRF` commit is waiting for pattern change/stop.

### 6. Deferred Live Period

Until manual pattern change or stop:

- STM32 keeps playing from PRF cache;
- manual morph affects cached live values only;
- per-voice morph automation affects cached live values only;
- new `.PRF` normal storage is inert;
- menu values show the cached live performance, not the pending `.PRF`.

### 7. Commit On Manual Pattern Change Or Stop

When STM32 registers a manual pattern change, or sequencer stop:

- STM32 atomically exits PRF cache mode;
- normal STM32 locations become live;
- protected pattern cache is released;
- cached live morph automation state is cleared or reconciled so it cannot fire one last stale morph event;
- the requested pattern change proceeds into the newly loaded `.PRF`.

STM32 then sends the newly live `.PRF` values back to AVR:

- kit defaults;
- morph target/default values;
- voice MIDI channels;
- voice notes;
- current pattern metadata/step display values as needed.

AVR menu now shows the new `.PRF`.

### 8. Abort Path

If file open/read fails, flow-control aborts, or an ACK times out:

- AVR sends `SEQ_PRF_CACHE_ABORT` if possible;
- STM32 keeps or restores the cached live state;
- normal partially loaded `.PRF` state must not become live;
- AVR restores menu values from cached live state;
- transport keeps running if it was running.

## Data Structures

### STM32 PRF Cache Session

Add an explicit cache session object, initially in `frontPanelParser.c` or a small new module if it grows.

Suggested shape:

```c
typedef enum {
   PRF_CACHE_IDLE = 0,
   PRF_CACHE_PREPARING,
   PRF_CACHE_RECEIVING_AVR_LIVE,
   PRF_CACHE_LIVE_ACTIVE,
   PRF_CACHE_RECEIVING_PENDING,
   PRF_CACHE_PENDING_READY,
   PRF_CACHE_COMMITTING,
   PRF_CACHE_ABORTING
} PrfCacheState;
```

Suggested fields:

- `state`;
- `enabledByPolicy`;
- `protectedPattern`;
- `pendingReady`;
- cached live kit defaults;
- cached live morph targets;
- cached live LFO/velocity destinations;
- cached live drumset/macro metadata;
- cached live voice MIDI channels;
- cached live voice notes;
- cached live morph amount and per-voice morph amounts;
- cached live pattern data for one pattern;
- validity bits per category, so incomplete cache data cannot be treated as live.

The cache must be small enough for STM32 RAM. Do not duplicate all 8 patterns.

### AVR Session State

AVR needs a small state flag around `preset_loadPerf()`:

- normal load;
- deferred PRF cache load;
- aborting deferred PRF cache load.

AVR should not need a full second permanent performance copy if the live menu state is sent to STM32 before the file load mutates the menu arrays, and STM32 restores it afterward. Temporary AVR snapshots should be avoided unless a specific phase proves they are needed.

## Phased Implementation Plan

Each phase should compile cleanly and have a hardware test point.

### Phase A: Clean Baseline And Isolated Morph Automation Fix

Status: implemented 2026-05-28.

Goal:

- no architecture change yet;
- fix the known `param2` per-voice morph routing bug;
- document/confirm that the repo is back at the checkpoint implementation before adding the new cache path.

Code changes:

- in `seq_parseAutomationNodes()`, compute the `param2` morph voice from `param2`, not `param1`;
- no new cache behavior yet.

Test:

- build AVR and full firmware;
- load `P005.PRF` while stopped and confirm per-voice morph automation routes to the expected voice;
- load `.ALL`/`.PRF` while playing and confirm behavior is not worse than checkpoint.

Expected result:

- this does not solve the bleed by itself;
- it removes one independent misrouting variable before cache work begins.

### Phase B: Protocol Skeleton With No Live Data Redirection

Status: implemented 2026-05-28.

Goal:

- add the explicit `.PRF` cache state machine and ACK boundaries without changing playback behavior yet.

Code changes:

- add new `SEQ_PRF_*` / `FRONT_SEQ_PRF_*` message IDs;
- AVR sends `SEQ_PRF_CACHE_BEGIN` before `SEQ_FILE_BEGIN` for full `.PRF` loads while running;
- STM32 enters `PRF_CACHE_PREPARING`, records protected pattern, ACKs;
- AVR sends `SEQ_PRF_PENDING_BEGIN` / `SEQ_PRF_PENDING_DONE` around the existing load;
- STM32 logs/marks state transitions but does not redirect live reads yet;
- abort path clears the state.

Test:

- build;
- load `.PRF` while stopped: no cache state entered;
- load `.PRF` while playing: state machine enters/exits predictably;
- `.ALL` load path unchanged;
- no new lockups.

Expected result:

- parameter bleed may still occur;
- this phase proves the protocol boundaries without relying on them for sound correctness yet.

### Phase C: AVR-Owned Live Snapshot To STM32

Status: implemented 2026-05-28.

Goal:

- before AVR reads the new `.PRF`, STM32 receives enough old/current menu-side data to restore the AVR menu and support cached morph.

Code changes:

- add an AVR snapshot sender called before `.PRF` file data mutates `parameter_values` / `parameters2`;
- send kit defaults, morph targets, LFO/velocity destinations, drumset metadata, voice MIDI channels, voice notes, and required globals;
- STM32 stores these in PRF cache with validity bits;
- STM32 ACKs only after the snapshot is complete.
- AVR restore uses explicit default-param and morph-param restore messages, because the old Cortex-to-front parameter messages cannot update `parameters2[]` without also clobbering `parameter_values[]`.
- STM32 sends the restore stream with TX FIFO backpressure; this phase is allowed to be slower rather than dropping restore bytes.

Test:

- load `.PRF` while playing;
- after file load completes but before pattern change, STM32 sends old/current values back to AVR;
- menu shows the old/current performance values, not the newly loaded `.PRF`;
- no attempt yet to make playback fully cache-read safe.

Expected result:

- menu bleed should be reduced or eliminated;
- sound bleed may still exist until live readers are redirected.

### Phase D: STM32-Owned Live Snapshot

Status: implemented 2026-05-28.

Goal:

- STM32 captures everything it knows about the currently playing performance before new `.PRF` data enters normal locations.

Code changes:

- copy exactly one protected pattern out of `seq_patternSet`;
- snapshot current per-track active pattern state;
- snapshot current voice MIDI runtime arrays;
- snapshot current note override arrays;
- snapshot morph amount/per-voice morph amount state;
- snapshot any current applied/interpolated values needed by cached morph/manual morph.
- Phase D currently captures at `SEQ_PRF_PENDING_BEGIN`, immediately before the new `.PRF` starts writing normal STM32 locations.
- The STM-owned snapshot cache is intentionally held as `volatile` in this phase so the compiler cannot discard it before Phase E starts reading from it.

Test:

- load `.PRF` while playing;
- inspect/diagnose that protected pattern and voice MIDI state are cached correctly;
- no cache-read redirection yet unless Phase D can safely include pattern reads only.

Expected result:

- cache is complete enough to support live playback in later phases.

### Phase E: Redirect Sequencer Pattern And Automation Reads To Cache

Status: implemented 2026-05-28.

Goal:

- while PRF cache mode is active, the currently playing pattern and its automation come only from the protected-pattern cache.

Code changes:

- add helper accessors for active step/mainstep/length/scale/rotation/pattern settings;
- use helpers in the playback path instead of direct `seq_patternSet` access for live reads;
- make `seq_parseAutomationNodes()` read cached step data while cache mode is active;
- keep writes from the incoming `.PRF` pointed at normal locations.
- The Phase E implementation redirects live playback reads only. Editor/UI reads and pattern editing writes still use normal storage until later phases.

Test:

- load `P005.PRF` while playing after another `.ALL`/`.PRF`;
- current pattern content and automation remain old/current until manual pattern change;
- no crash on pattern change.

Expected result:

- pattern automation should no longer read pending `.PRF` step targets early.

### Phase F: Redirect Morph And Voice Runtime Reads To Cache

Status: implemented.

Goal:

- manual morph, global morph, per-voice morph automation, voice MIDI channels, and voice note overrides use cached live values during the deferred period.

Code changes:

- route `sequencer_sendVMorph()` through cache-aware live morph handling;
- route global `MORPH_CC` handling through cache-aware live morph handling;
- prevent AVR morph commands generated during cache mode from using pending `.PRF` menu arrays;
- ensure outgoing live restore messages to AVR carry cached old/current values;
- make note-off/note-on paths use cached voice MIDI channels while cache mode is active if normal arrays have been overwritten by the new `.PRF`.

Implemented notes:

- added STM32 PRF-cache runtime accessors for:
  - live MIDI channel;
  - live note override;
  - live per-voice morph amount;
  - one-shot live per-voice morph flag drain.
- switched sequencer note-on/prog-change/per-voice morph flush paths to those accessors.
- switched MIDI parser note/global/voice channel dispatch to those accessors.
- switched MIDI voice note-off channel lookup to those accessors.
- normal `.PRF` writes still land in the normal parameter/pattern locations, but these runtime paths do not read those overwritten values while PRF cache mode is active.

Test:

- `P005.PRF` repro:
  - load previous `.ALL` or `.PRF`;
  - start sequencer;
  - load `P005.PRF`;
  - confirm current kit does not change;
  - confirm per-voice morph automation still works on the old/current kit;
  - confirm voice MIDI channels do not change until commit.

Expected result:

- the main bleed should be gone.

### Phase G: Full Pending PRF Commit On Pattern Change/Stop

Status: implemented, but failed user test on 2026-05-28.

Goal:

- switch cleanly from cached live state to the newly loaded `.PRF`.

Code changes:

- replace the checkpoint `frontParser_applyDeferredVoiceCache()` commit semantics with PRF cache commit semantics;
- on manual pattern change or stop:
  - leave cache mode;
  - clear stale cached morph flags;
  - normal STM32 locations become live;
- apply any deferred voice availability/unhold state that is still needed;
- request/send AVR menu refresh for the new live `.PRF`.

Implemented notes:

- STM32 `frontParser_applyDeferredVoiceCache()` now commits a pending PRF cache session when `SEQ_PRF_PENDING_DONE` has completed:
  - replays deferred non-voice performance messages;
  - applies pending voice cache/unhold state;
  - copies staged protected-pattern data back into the normal pattern slot;
  - clears stale live-cache/per-voice morph flags;
  - exits PRF cache mode so normal STM32 locations become live.
- STM32 calls the same commit path on stop, including stops initiated outside the normal front-panel `RUN_STOP` command path.
- STM32 suppresses automatic next-pattern chaining while PRF cache mode is active so only a deliberate pattern change or stop can commit the pending `.PRF`.
- AVR captures the newly loaded `.PRF` menu/default/morph arrays before STM restores the old live menu state.
- AVR applies that pending menu snapshot on the STM pattern-change or stop acknowledgement, then clears the old locked-load voice mask so the legacy `.prf` pattern-change loader does not reopen/reapply from the closed file.

Test:

- load `P005.PRF` while playing;
- confirm old/current kit plays until manual pattern change;
- manually change pattern;
- confirm new `.PRF` kit, morph, voice MIDI channels, and selected pattern become live;
- no crash.

Expected result:

- deferred `.PRF` behavior matches the intended musical workflow.

Observed result after Phase G test:

- stopped `.ALL` and stopped `.PRF` loads still work;
- `.ALL` while playing still works;
- `.PRF` while playing still does not leak into the current sound;
- after loading a normal `.PRF` while playing and then manually changing pattern, no new voice parameters or pattern data become live;
- with `P005.PRF`, the manual pattern change appears to stall the UI briefly, consistent with deferred morph-related work replaying, but the sound still does not switch to the pending `.PRF`;
- user observation: none of the 8 patterns in the pattern set appear to have been replaced by the `.PRF`.

Diagnosis:

- The Phase G implementation still relies too much on the checkpoint-era deferred performance machinery.
- `frontParser_shouldStagePattern()` still routes writes for the protected/current pattern into `seq_tmpPattern` while a deferred performance load is active. That was correct before the explicit live cache existed, but it is wrong for the intended PRF-cache workflow. The protected live pattern is already safe in the PRF cache, so the incoming `.PRF` version of that same pattern must be allowed into normal pattern storage.
- `frontParser_shouldDeferPerfMessage()` still swallows many incoming `.PRF` messages into `frontParser_deferredPerfMsgCache` rather than treating the normal locations as the pending `.PRF` image.
- Voice parameters are a special case: the actual DSP voice fields are live audio state, so they cannot simply be overwritten as "normal storage" unless all sound generation reads from a cache. The current code therefore caches voice writes in `midi_midiCache[]` / LFO / velocity caches. That is safe, but it means the PRF commit must explicitly apply those caches as the normal/live image at the commit boundary.
- The AVR-side Phase G menu snapshot is also not the exact intended protocol. It makes the AVR menu switch after a commit ack, but the intended design is that STM32 sends the now-live, uncached normal values back to AVR after the STM32 commit is complete.

Conclusion:

- Phase G solved "do not leak before commit" but did not solve "pending `.PRF` becomes live on commit."
- The next work should be a delta/replacement for Phase G, not Phase H.

### Phase G2: Correct Pending PRF Write Boundary

Status: implemented for pattern write boundary on 2026-05-28; failed user test on 2026-05-28.

Goal:

- while PRF cache mode is active, the currently playing state is read only from the live cache;
- all incoming `.PRF` pattern data for all 8 patterns is written to normal pattern storage, including the pattern that was playing at load start;
- incoming `.PRF` kit/global/morph data is held in pending storage where direct writes would alter live audio state;
- on manual pattern change or stop, the pending `.PRF` image becomes the normal/live image in one explicit commit path.

Code changes:

- split the old deferred-performance behavior from the new PRF-cache behavior:
  - keep checkpoint behavior for non-PRF or cache-policy-disabled cases;
  - add PRF-cache-specific tests before `frontParser_shouldStagePattern()` and `frontParser_shouldDeferPerfMessage()`.
- change pattern receive behavior during active PRF cache:
  - `SYSEX_RECEIVE_MAIN_STEP_DATA`;
  - `SYSEX_RECEIVE_PAT_CHAIN_DATA`;
  - `SYSEX_RECEIVE_PAT_LEN_DATA`;
  - `SYSEX_RECEIVE_PAT_SCALE_DATA`;
  - `SYSEX_RECEIVE_STEP_DATA`;
  - automation target/value receive paths.
- those pattern receive paths must write incoming `.PRF` data to `seq_patternSet` for the addressed pattern/track/step, including the protected pattern.
- remove PRF-cache use of `seq_tmpPattern` as the pending `.PRF` destination. `seq_tmpPattern` can remain for legacy running-load behavior, but not for the explicit PRF cache path.
- keep live playback using `frontParser_prfCacheLivePattern` until commit; do not weaken the live-read accessors added in Phases E/F.
- keep voice and kit parameters from leaking by introducing an explicit PRF pending-voice/kit commit path:
  - either continue using `midi_midiCache[]`, `midi_midiLfoCache[]`, and `midi_midiVeloCache[]` as pending storage during PRF cache mode, but make the commit path deterministic and independent of old `frontParser_deferredPerfMsgCache`;
  - or introduce dedicated `frontParser_prfPending*` storage for voice params, LFO targets, velocity targets, morph targets, macro targets, globals, voice MIDI channels, and note overrides.
- do not replay morph automation while the pending image is incomplete. During commit:
  - disable live cache reads only after pending pattern storage and pending kit/morph/global storage have been applied;
  - clear stale live-cache morph flags;
  - clear normal `seq_vMorphFlag`;
  - then allow morph automation/manual morph to run against the new normal image.

Test:

- load `.ALL`;
- press play;
- load a normal `.PRF`;
- confirm no audible/menu leak while load is in progress or after load completes;
- manually change to a pattern that is visibly/audibly different in the `.PRF`;
- confirm that pattern data, kit params, morph params, voice MIDI channels, and note overrides become live.

Expected result:

- the normal `.PRF` image is present in STM32 before commit;
- the single protected live pattern is the only pattern cached for playback during the deferred period;
- all 8 patterns from the `.PRF` are available immediately after commit.

Implemented delta:

- added an STM32 PRF-cache session-active helper.
- changed `frontParser_shouldStagePattern()` so explicit PRF cache mode never stages incoming pattern data into `seq_tmpPattern`.
- changed `SYSEX_RECEIVE_MAIN_STEP_DATA` so the secondary "active pattern while running" staging branch is disabled during PRF cache mode.
- changed `SYSEX_BEGIN_PATTERN_TRANSMIT` completion logic so PRF cache mode does not mark a staged temp pattern available.
- left voice/kit parameter deferral in place for this pass; those are still applied at commit through the existing pending voice cache path.

Observed result after Phase G2 test:

- behavior is unchanged from failed Phase G;
- after `.ALL -> play -> .PRF -> manual pattern change`, no pattern data or sound data from the `.PRF` becomes live;
- the `.PRF` load appears shorter than `.ALL` while playing, which is suspicious because explicit PRF cache mode adds snapshot/restore traffic and should not obviously be shorter.

Updated diagnosis:

- The G2 receiver-side boundary change was necessary but not sufficient.
- The failure now looks more likely to be earlier than the commit boundary:
  - the AVR may not be entering the explicit PRF cache protocol for this test path;
  - or the AVR may not be transmitting the full `.PRF` pattern payload under the active `menu_voiceArray` / `preset_workingVoiceArray` mask;
  - or STM32 may be receiving the payload through the legacy deferred path rather than the explicit PRF cache path.
- Current AVR cache entry is gated by `menu_sequencerRunning`:
  - `preset_loadPerf()` sends `SEQ_PRF_CACHE_BEGIN` only when `preset_workingVoiceArray >= 0x3f && menu_sequencerRunning`;
  - STM32 legacy deferral is gated independently by `seq_isRunning()`;
  - if those two running flags disagree, STM32 can protect/defer the `.PRF` while AVR never sends the cache snapshot/pending protocol.
- The full `.PRF` sender also depends on `preset_workingVoiceArray` for pattern transmission:
  - `preset_readPatternMainStep()` sends only tracks whose bits are set in `preset_workingVoiceArray`;
  - `preset_loadPerf()` sends step data only for tracks whose bits are set in `preset_workingVoiceArray`;
  - full load normally expects `menu_voiceArray == 0x7f`, but any stale/narrow mask could make the transfer shorter and incomplete.
- Therefore the next fix should not be another commit-path tweak. First prove that:
  - AVR entered PRF cache mode;
  - STM32 accepted PRF cache mode;
  - AVR transmitted all expected PRF pattern tracks/patterns;
  - STM32 counted/recorded those writes into normal `seq_patternSet`.

### Phase G2B: Prove And Harden PRF Cache Entry/Transmit

Goal:

- make it impossible for `.PRF` while playing to fall into a half-old/half-new legacy path silently.

Code changes:

- add an explicit STM32 cache-entry acknowledgement that distinguishes "accepted PRF cache" from "ignored because STM32 is not running/cache policy off."
  - Do not rely on a generic flow grant alone for this decision.
  - AVR must only send the live snapshot and pending PRF protocol if STM32 explicitly reports cache accepted.
- on AVR, compute a local `prfCacheRequested`/`prfCacheAccepted` pair:
  - requested for full `.PRF` loads where the user's intended voice mask is full performance;
  - accepted only after STM32 confirms it is actually running and in PRF cache mode.
- if AVR requested cache but STM32 does not accept:
  - either fall back to normal stopped-style `.PRF` load only if STM32 says it is not running;
  - or abort the PRF load rather than allowing legacy deferred behavior while playing.
- for explicit PRF cache loads, force the pending pattern payload to be full-pattern-set:
  - all 7 tracks;
  - all 8 patterns;
  - main steps, length, scale, chain, and step data.
- add temporary or permanent STM32 counters for the PRF cache receive window:
  - mainstep records received;
  - step records received;
  - length/scale/chain records received;
  - protected-pattern records written to normal storage.
- at `SEQ_PRF_PENDING_DONE`, validate minimum expected counts for a full `.PRF` pending load.
  - If counts are wrong, abort cache mode and report failure instead of committing an empty/partial pending image.

Test:

- `.ALL -> play -> .PRF` should visibly take the explicit cache path.
- Load time should include cache handshake and all-pattern transmit.
- After manual pattern change, at least pattern data should become live.
- If cache entry is not accepted or counts are wrong, the load should fail visibly rather than silently preserving old data.

Expected result:

- no more silent "nothing lands" state;
- before revisiting kit/morph commit behavior, we can prove that the pending `.PRF` pattern image exists on STM32.

Implementation status:

- Implemented explicit `PRF_CACHE_STATUS` reply from STM32 for `SEQ_PRF_CACHE_BEGIN`.
- AVR now treats PRF cache mode as active only after `PRF_CACHE_ACCEPTED`; rejected cache entry falls back to the normal load path.
- The AVR parser now accepts `PRF_CACHE_STATUS` while `frontParser_rxDisable` is set during a load.
- Accepted PRF cache loads force `voiceArray` and `preset_workingVoiceArray` to `0x7f` so all 7 tracks and all 8 patterns are transmitted.
- STM32 now counts mainstep, step, length, scale, chain, and protected-pattern writes during `PRF_CACHE_RECEIVING_PENDING`.
- `SEQ_PRF_PENDING_DONE` validates the full pending payload and sends `SEQ_FLOW_ABORT` instead of granting if the pending image is incomplete.
- AVR flow waits now return failure when STM32 aborts, so an incomplete pending image cannot silently proceed.
- Build check passed:
  - `make -C mainboard/LxrStm32 stm32`
  - `make -C front/LxrAvr avr`
  - `make firmware`

Observed result after Phase G2B test:

- behavior is still unchanged;
- after `.ALL -> play -> .PRF -> manual pattern change`, no `.PRF` pattern data or sound data becomes live;
- no visible failure path was triggered, so the counter/entry hardening did not expose or fix the core model error;
- this means the current implementation is still too entangled with the legacy deferred perf machinery and/or the cache exit path is not a simple source switch from cached playback back to normal loaded data.

Updated diagnosis:

- The desired behavior is simple, but the implementation has become too complicated:
  - `.PRF` stopped load already works;
  - `.PRF` running load should cache the currently playing state;
  - while cached, playback should read only cached state;
  - the `.PRF` file should load into the normal stopped-load destinations;
  - after the load is complete and valid, the next pattern change should only switch playback source from cache back to normal data.
- The current approach still allows old deferred perf staging/replay concepts to participate in the PRF cache path.
- That is the wrong shape. For PRF cache mode, the old deferred path should be bypassed, not cooperated with.
- The next delta should be a simplifying replacement over the G/G2/G2B commit logic, while preserving useful protocol/cache pieces already built.

### Phase G2C: Simplify PRF Cache State Machine

Goal:

- replace the current PRF cache/deferred-perf hybrid with a small, explicit source-selection model:
  - `cacheActive`: playback reads cached live state only;
  - `pendingValid`: normal STM32 data contains a complete loaded `.PRF` image and is safe to switch to.

Core rule:

- pattern change is not allowed to exit PRF cache unless both `cacheActive` and `pendingValid` are true.

State model:

```c
PRF_CACHE_OFF
PRF_CACHE_LIVE_ACTIVE      // cache is valid; pending .PRF is still loading or not yet validated
PRF_CACHE_PENDING_VALID    // cache still plays; normal data is fully loaded and safe
PRF_CACHE_ABORTING
```

Implementation delta:

- keep the existing message IDs and useful snapshot helpers:
  - `SEQ_PRF_CACHE_BEGIN`;
  - `SEQ_PRF_AVR_SNAPSHOT_BEGIN` / `SEQ_PRF_AVR_SNAPSHOT_END`;
  - `SEQ_PRF_PENDING_BEGIN` / `SEQ_PRF_PENDING_DONE`;
  - `SEQ_PRF_RESTORE_AVR_LIVE`;
  - existing live cache structs/accessors.
- remove PRF cache dependence on legacy deferred perf commit behavior:
  - do not use `frontParser_deferredPerfLoadActive` as the commit authority for PRF cache;
  - do not use `seq_tmpPattern` for PRF cache protected-pattern writes;
  - do not rely on `frontParser_applyDeferredPerfMessages()` to make a `.PRF` cache commit happen.
- when STM32 accepts `SEQ_PRF_CACHE_BEGIN`:
  - capture the current active playback state into the PRF live cache;
  - set `cacheActive = 1`;
  - set `pendingValid = 0`;
  - store the currently playing/protected pattern number;
  - playback reads from cache immediately and continues doing so while the `.PRF` load proceeds.
- while `cacheActive` is true:
  - all sequencer playback, morph automation, per-voice morph, MIDI channel, note override, and current pattern reads must use cached values;
  - incoming `.PRF` data writes to normal STM32 locations exactly like stopped-load data;
  - active-pattern staging logic must be disabled for PRF cache mode, because playback is protected by source selection, not by write deferral.
- on `SEQ_PRF_PENDING_BEGIN`:
  - begin the pending load window;
  - reset validation counters if they remain useful;
  - do not change playback source.
- on `SEQ_PRF_PENDING_DONE`:
  - if the pending image is complete enough to trust, set `pendingValid = 1` and move to `PRF_CACHE_PENDING_VALID`;
  - do not switch playback yet;
  - do not replay deferred messages;
  - do not apply `seq_tmpPattern`;
  - keep cached playback active until a later pattern change.
- on pattern change:
  - if `cacheActive && pendingValid`, clear cache mode and let normal sequencer data become the playback source;
  - then allow the existing pattern change to proceed against normal `seq_patternSet`;
  - notify/restore AVR menu values from the now-live normal data in Phase G3;
  - if `cacheActive && !pendingValid`, keep using cache and do not switch to normal data.
- on stop:
  - if `cacheActive && pendingValid`, it is acceptable to exit cache and switch to normal data;
  - if `cacheActive && !pendingValid`, keep cache or abort to a known-safe state; do not switch to partial normal data.
- on load abort/failure:
  - clear `pendingValid`;
  - send/keep cached live values as needed so playback/menu remain coherent;
  - do not switch to normal loaded data.

Files likely touched:

- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
  - simplify `PrfCacheState`;
  - split cache-active and pending-valid checks;
  - bypass legacy deferred perf commit/staging while PRF cache is active;
  - make pattern-change cache exit a source switch only.
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
  - verify all playback reads use PRF cache accessors while cache active;
  - ensure pattern change can clear PRF cache only after pending valid.
- `front/LxrAvr/Preset/presetManager.c`
  - keep full `.PRF` transmit path and cache snapshot path;
  - avoid relying on AVR-side pending commit as the authoritative switch.
- `front/LxrAvr/frontPanelParser.c`
  - keep menu restore for cached live values;
  - defer full post-commit normal-data menu refresh to Phase G3.

Test:

- stopped `.PRF` load still works exactly as before.
- stopped `.ALL` load still works exactly as before.
- running `.ALL` load still works as already tested.
- running `.PRF` load:
  - audible playback does not change during the load;
  - P005-style morph automation does not leak during the load;
  - a pattern change before `pendingValid` does not exit cache;
  - a pattern change after `pendingValid` switches to the newly loaded `.PRF` normal data.

Expected result:

- the implementation matches the intended three-step behavior:
  - cache current playback;
  - load `.PRF` normally into normal data;
  - switch playback source back to normal data on a safe pattern-change boundary.

Implementation status:

- STM32 PRF cache state has been simplified to:
  - `PRF_CACHE_IDLE`;
  - `PRF_CACHE_RECEIVING_AVR_LIVE`;
  - `PRF_CACHE_LIVE_ACTIVE`;
  - `PRF_CACHE_RECEIVING_PENDING`;
  - `PRF_CACHE_PENDING_VALID`;
  - `PRF_CACHE_ABORTING`.
- STM32 now captures its live playback snapshot immediately when `SEQ_PRF_CACHE_BEGIN` is accepted, before any `.PRF` file data can write normal locations.
- PRF cache mode now bypasses legacy deferred perf message caching:
  - `frontParser_shouldDeferPerfMessage()` returns false during PRF cache sessions;
  - `SEQ_FILE_BEGIN` does not set `frontParser_deferredPerfLoadActive` when PRF cache mode is already active;
  - active-pattern sysex staging remains disabled while PRF cache mode is active.
- `SEQ_PRF_PENDING_DONE` now sets `pendingValid`/`PRF_CACHE_PENDING_VALID` after the pending image validates, but does not switch playback source.
- `frontParser_applyDeferredVoiceCache()` now treats PRF cache exit as:
  - only allowed when the pending image is valid;
  - apply normal pending voice cache;
  - clear PRF cache/source selection;
  - no deferred-message replay;
  - no `seq_tmpPattern` apply.
- `SEQ_FILE_DONE` during PRF cache mode no longer applies pending voice cache immediately.
- Build check passed:
  - `make -C mainboard/LxrStm32 stm32`
  - `make -C front/LxrAvr avr`
  - `make firmware`

### Phase 1 Reset: Immediate PRF/ALL Load With Morph Gate

Goal:

- stop pursuing PRF live-cache behavior for now;
- restore `.PRF` loading so it applies immediately, matching the `.ALL` load pathway;
- disable morph and morph automation while `.PRF` or `.ALL` file data is in flight.

Implementation status:

- AVR `preset_loadPerf()` no longer sends the PRF cache protocol:
  - no `SEQ_PRF_CACHE_BEGIN`;
  - no AVR snapshot;
  - no `SEQ_PRF_PENDING_BEGIN` / `SEQ_PRF_PENDING_DONE`;
  - no delayed AVR-side PRF commit snapshot.
- `.PRF` now sends only the normal `SEQ_FILE_BEGIN`, payload, then `SEQ_FILE_DONE` path.
- `.ALL` now also sends `SEQ_FILE_BEGIN` before its payload so STM32 can apply the same load-time morph gate.
- STM32 defaults `frontParser_deferPerfLoadCacheUntilPatternChange` to `0`, preventing the old performance-load deferral/cache path from engaging.
- STM32 sets `seq_morphLoadDisabled = 1` on `.PRF`/`.ALL` `SEQ_FILE_BEGIN` and clears it on `SEQ_FILE_DONE`.
- While `seq_morphLoadDisabled` is set:
  - sequencer morph automation targets are ignored;
  - pending per-voice morph flags are cleared;
  - `sequencer_sendVMorph()` is suppressed;
  - MIDI mod-wheel morph forwarding to AVR is suppressed;
  - `modNode_vMorph()` is suppressed.
- Build check passed:
  - `make -C mainboard/LxrStm32 stm32`
  - `make -C front/LxrAvr avr`
  - `make firmware`

Test:

- stopped `.PRF` and `.ALL` loads should work normally;
- running `.PRF` and `.ALL` loads should apply immediately as before/current `.ALL` behavior;
- P005-style morph automation should not send/apply morph while the load is in progress;
- morph should resume after `SEQ_FILE_DONE`.

### Phase G3: STM-To-AVR Post-Commit Refresh

Goal:

- after STM32 exits PRF cache mode, AVR menu values are refreshed from the now-live normal STM32 state, not from an AVR-side guess.

Code changes:

- add a post-commit restore/report command distinct from `SEQ_PRF_RESTORE_AVR_LIVE`;
- on commit, STM32 sends the uncached normal/live values for:
  - kit default parameters;
  - kit morph parameters;
  - voice MIDI channels and note overrides;
  - relevant globals/metadata;
  - current pattern metadata/step view state needed by the menu.
- AVR applies these values and repaints after the STM32 commit ack.
- remove or downgrade the AVR-side `preset_deferredPrfCommitPending` snapshot to a temporary fallback only.

Test:

- after the manual pattern-change commit, inspect menu pages for voice parameters, morph parameters, voice MIDI channels, and pattern data;
- confirm menu matches the new sound and the new pattern, not the cached old sound.

### Phase H: Abort, Timeout, And Regression Pass

Goal:

- make failure paths boring.

Code changes:

- implement `SEQ_PRF_CACHE_ABORT`;
- clear cache state on file read failure, timeout, flow abort, or unexpected `SEQ_FILE_DONE`;
- ensure `.ALL` load and stopped `.PRF` load do not enter cache mode;
- verify SRAM/flash cost.

Test matrix:

- `P000.ALL -> play -> P005.PRF -> pattern change`;
- `P001.ALL -> play -> P000.ALL`;
- `P001.PRF -> play -> P005.PRF`;
- stopped `.PRF` load;
- stopped `.ALL` load;
- partial/failed `.PRF` load if practical;
- fast repeated `.PRF` load attempts while playing.

Expected result:

- transport does not lock;
- current sound does not bleed;
- menu matches sounding state during deferred period and new state after commit.

## Implementation Notes

- Do not rely on suppressing one sender. The boundary must be "live reads come from cache" versus "pending `.PRF` writes go to normal storage."
- Do not duplicate all 8 patterns. Cache exactly the current protected pattern.
- Do not unlock the AVR menu after `.PRF` load until STM32 has restored the cached live display state.
- Do not let morph automation run while the cache is incomplete. It can resume only after the cache is valid.
- Keep the global/internal policy flag so a later settings-page option can choose this behavior.
- Keep phase tests narrow. If a phase still allows bleed by design, state that before testing so a known failure is not mistaken for a regression.
