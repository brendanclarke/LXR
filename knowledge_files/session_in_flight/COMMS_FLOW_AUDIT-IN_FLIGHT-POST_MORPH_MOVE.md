# COMMS / FLOW AUDIT - POST MORPH MOVE

Date: 2026-06-03  
Status: current communication audit after Session 003 morph move. Replaces `COMMS_FLOW_AUDIT-IN_FLIGHT.md` for future work.

## Purpose

This document preserves the current communications baseline without carrying stale assumptions from pre-morph temp/cache work.

Important framing:

- The immediate Session 004 goal is temp cache / load exchange, not broad transport hardening.
- Current failures should be assumed semantic/storage until proven transport-related.
- Flow control and quiet mode remain relevant, but the morph move changed what parameter traffic means.

## Current Communication Baseline

The dual-MCU link remains UART-based, MIDI-like status/data framing at 500000 baud.

Implemented comms protections from earlier sessions still exist:

- AVR 3-byte sends avoid waiting inside an atomic block for TX space.
- Load sessions use `SEQ_FLOW_BEGIN`, `SEQ_FLOW_GRANT`, `SEQ_FLOW_END`, and `SEQ_FLOW_ABORT`.
- STM quiet mode suppresses optional UI/status traffic during load sessions while priority messages still pass.
- Globals, voice-param, and drum-meta bursts can be credit-metered.
- Old SysEx/callback waits still exist and are still a deferred hardening task.

## Current Parameter Meaning After Morph Move

Parameter traffic changed semantically in Session 003:

- `PRF_RESTORE_PARAM_CC/CC2` and `PRF_RESTORE_MORPH_CC/CC2` are endpoint storage traffic.
- They use raw AVR/menu parameter indices.
- They are not live morph-computed values.
- File load routes this traffic to STM normal endpoint storage.
- File load should not write temp storage.
- File load should not write `interpolatedParams[]`.

Ordinary live low CC still uses the old STM MIDI parser low-CC enum offset:

- raw endpoint param `N < 128` becomes live CC `N + 1` when calling `midiParser_ccHandler(...)`.
- do not apply this offset to endpoint transport.

## Current Key Opcodes

Endpoint byte transport:

- `PRF_RESTORE_PARAM_CC`
- `PRF_RESTORE_PARAM_CC2`
- `PRF_RESTORE_MORPH_CC`
- `PRF_RESTORE_MORPH_CC2`

Endpoint brackets:

- AVR `SEQ_TMP_KIT_ENDPOINT_BEGIN`
- AVR `SEQ_TMP_KIT_ENDPOINT_END`
- STM `FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN`
- STM `FRONT_SEQ_TMP_KIT_ENDPOINT_END`

Endpoint automation target phase:

- AVR `SEQ_TMP_KIT_AUTOMATION_PHASE`
- STM `FRONT_SEQ_TMP_KIT_AUTOMATION_PHASE`
- phase values:
  - `NONE`
  - `FRONT_ENDPOINT`
  - `MORPH_ENDPOINT`

Global morph:

- AVR sends `SEQ_CC, SEQ_SET_GLOBAL_MORPH, value`.
- STM receives `FRONT_SEQ_SET_GLOBAL_MORPH` and updates all six per-voice morph amounts.
- Session 005 added `SEQ_REPORT_GLOBAL_MORPH_LSB/MSB` and
  `FRONT_SEQ_REPORT_GLOBAL_MORPH_LSB/MSB` for display-only menu sync when the
  active normal/temp kit image changes. These opcodes do not alter STM morph
  state and should not be routed back through the edit path.

File load:

- `SEQ_FILE_BEGIN` / `FRONT_SEQ_FILE_BEGIN`
- `SEQ_FILE_DONE` / `FRONT_SEQ_FILE_DONE`

PRF cache / deferred WIP remains present:

- `SEQ_PRF_CACHE_BEGIN`
- `SEQ_PRF_PENDING_BEGIN`
- `SEQ_PRF_PENDING_DONE`
- `SEQ_PRF_CACHE_ABORT`
- `SEQ_PRF_AVR_SNAPSHOT_BEGIN`
- `SEQ_PRF_AVR_SNAPSHOT_END`
- `SEQ_PRF_RESTORE_AVR_LIVE`

Treat the PRF cache state machine as WIP until Session 004 reconciles it with the post-morph temp storage model.

## Flow-Control Historical Warnings To Preserve

### Do not reintroduce unbounded STM RX drain

A previous unbounded full-drain of STM front RX caused known-good `.ALL` load to freeze at `Loading All`. If RX throughput is revisited, use bounded drain experiments only after the temp-cache path is stable.

### Do not change PAT_CHAIN ACK placement casually

The AVR sender expects byte-by-byte callbacks for pattern chain `(next, repeat)` data. Pair-level ACK was previously diagnosed as wrong for current sender behavior.

### Old permanent waits remain

Still deferred:

- `SYSEX_START`
- `SYSEX_END`
- `MAINSTEP_CALLBACK`
- `LENGTH_CALLBACK`
- `SCALE_CALLBACK`
- `PATCHAIN_CALLBACK`
- `STEP_ACK`
- `STEP_CALLBACK`
- `CALLBACK_ACK`
- `frontPanel_holdForBuffer()`

Timeout work should be done one reader family at a time after temp exchange is stable.

## Current File Load Flow After Morph Move

### AVR side

For `.PRF` / `.ALL`:

1. Start load session.
2. Send `SEQ_FILE_BEGIN`.
3. Read globals/perf metadata as appropriate.
4. Read pattern data through existing SysEx/callback paths.
5. Read morph endpoint block into `parameters2_temp`.
6. Read kit/front endpoint block into `parameter_values_temp`.
7. Copy/stage per-voice and meta data into `parameter_values[]` / `parameters2[]`.
8. Dump endpoints to STM using endpoint bracket and automation target phases.
9. Send `SEQ_FILE_DONE`.
10. End load session.

### STM side

During file load:

- `frontParser_beginFileLoadIngress(...)` sets `SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT`.
- `FRONT_SEQ_FILE_BEGIN` resets per-voice morph amounts back to cached global morph and invalidates the live morph apply cache.
- Endpoint bytes write normal endpoint arrays.
- Automation target sidebands update normal endpoint/interpolated target caches.
- `FRONT_SEQ_FILE_DONE` returns ingress to current image when appropriate.

## Why Current Temp Failure Is Probably Not First A Transport Bug

The final Session 003 state has:

- standard file load enough to support `P000.SND` boot parameter correctness;
- `P005.PRF` LFO target to voice morph applying correctly on load;
- standard morph audio sounding correct;
- no continuous low-Hz overlay after live-apply cache fix.

That suggests the transport can deliver endpoint bytes and automation target sidebands in at least the normal live path.

The broken area is specifically normal/temp pattern and parameter exchange. Likely causes:

- wrong storage target during temp cache capture;
- temp interpolation image not initialized/converged;
- endpoint restore sends full zeroed arrays;
- normal/temp source state mismatch;
- live-apply cache invalidation missing around source switches;
- PRF cache/deferred state machine still assuming the old interpolation ingress model.

Do not start Session 004 by broadly changing flow control. Start by tracing one parameter through storage.

## Current Comms Pressure Points For Session 004

### Endpoint restore to AVR

STM-to-AVR endpoint restore remains necessary for menu correctness when switching normal/temp views. After valid-array removal:

- restore sends whole endpoint arrays;
- zeros are authoritative;
- restore no longer filters to "valid" params;
- this can make temp menu values look wrong if temp endpoint arrays are zero or stale.

Question for Session 004:

- Is the menu showing the true temp endpoint array, or is the wrong array being restored?

### Endpoint dump from AVR to STM

AVR-to-STM endpoint dump currently always fills normal endpoint storage. That is correct for file load and may be correct for copy-to-temp depending on exact operation.

Question for Session 004:

- Is copy normal-to-temp relying on normal endpoint dump and then `seq_captureTmpKitState()`, and if so, what exactly gets copied to `seq_tmpKitState`?

### File-load while temp is sounding

Expected:

- file load may update AVR menu arrays;
- file load may update STM normal endpoint arrays;
- file load must not update STM temp arrays or temp live sound;
- once user leaves temp, normal file-loaded data should be available.

Question for Session 004:

- Does PRF cache/deferred load still protect temp after morph move, or does it replay messages into current image incorrectly?

### Automation target sideband phases

Sideband messages are resolved destination messages, not raw selector bytes. Their meaning depends on the current automation phase.

Question for Session 004:

- During temp copy/restore, are sidebands stored in front endpoint, morph endpoint, or interpolated target cache as intended?

## Deferred Comms Hardening Plan

Only resume this after temp cache/loading is fixed or deliberately paused:

1. Reconfirm known-good `.ALL` and `.PRF` stopped/running.
2. Add timeouts to old waits one family at a time.
3. Prove timeout cleanup clears STM quiet mode and AVR flow state.
4. Consider bounded STM RX drain only after timeout recovery works.
5. Do not mix this with file-layout fixes, parameter-map changes, or temp-cache semantics.

## Do-Not-Do List

- Do not reintroduce unbounded STM RX full-drain.
- Do not change PAT_CHAIN ACK placement without changing AVR sender.
- Do not use flow-control changes to paper over parameter routing bugs.
- Do not add broad diagnostic/LCD output unless explicitly requested.
- Do not reintroduce AVR live morph traffic.
- Do not treat PRF cache WIP state as verified after Session 003; audit it against the post-morph model.

## Good Session 004 Transport Sanity Tests

Before and after any temp-cache fix:

- Boot and confirm `P000.SND` endpoint values land in sound.
- Load `P005.PRF` and confirm LFO1 targets `PAR_MORPH_HIHAT`.
- Copy normal pattern to temp and observe SEQ16 temp cache.
- Switch temp/normal and watch whether menu restore traffic appears coherent.
- Load `.PRF` while temp is sounding and confirm temp sound does not change.
- Confirm no load freeze at `Loading All` / `Loading Perf`.

If normal load paths still work but temp exchange fails, stay in storage semantics, not broad comms.
