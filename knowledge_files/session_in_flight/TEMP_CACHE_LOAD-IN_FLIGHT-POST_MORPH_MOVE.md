# TEMP CACHE / LOAD AUDIT - POST MORPH MOVE

Date: 2026-06-03  
Status: current Session 004 starting audit. Replaces stale working content in `TMP_VARS_AUDIT.md` and `PRF_ALL_LOAD_FIX_AUDIT-IN_FLIGHT.md` for post-morph-move work.

## Purpose

Normal/temporary pattern and parameter exchange is broken after Session 003 moved morph fully onto the STM. This audit consolidates the old temp-var and `.PRF` / `.ALL` load docs into a current, shorter map for fixing the temp cache in Session 004.

The old audits contain useful history but are stale in two major ways:

- They describe per-parameter valid arrays that no longer exist.
- They describe the old AVR live morph model and `SEQ_PARAM_INGRESS_NORMAL_INTERPOLATED`, which no longer exists.

Do not use those old documents as the source of truth for current code.

## Current Architectural Truth

### STM owns live sound interpolation

Session 003 moved morph computation to STM:

- `seq_serviceMorphInterpolation()` runs once per STM main loop.
- The morph worker writes `SeqKitState.interpolatedParams[]`.
- The morph worker applies live DSP values only when that normal/temp image is sounding.
- Live DSP apply is guarded by `seq_liveMorphAppliedValue` / `seq_liveMorphAppliedKnown`, not by parameter-array validity flags.

### AVR owns UI and file endpoint bytes

AVR still owns:

- SD file reads/writes.
- Menu-visible `parameter_values[]`.
- Menu-visible `parameters2[]`.
- Temporary staging arrays `parameter_values_temp[]` and `parameters2_temp[]`.

AVR does not own standard live morph computation anymore.

### Parameter arrays are always valid

Current `SeqKitState` contains:

- `frontPanelParams[]`
- `morphParams[]`
- `interpolatedParams[]`
- `frontPanelAutomationTargets`
- `morphParameterEndpointAutomationTargets`
- `interpolatedAutomationTargets`
- one remaining `valid` byte

Removed from `SeqKitState`:

- `frontPanelParamsValid[]`
- `morphParamsValid[]`
- `interpolatedParamsValid[]`

The correct model:

- A parameter array is valid from zero initialization.
- A missing file leaves the zeroed array as valid sound state.
- A file load overwrites endpoint bytes where appropriate.
- There is no sound-framework concept of a half-written parameter.
- Transfer-level completeness checks should not be used to hide storage-routing bugs.

## Current Parameter Flow

### Boot

1. STM `seq_init()` zeroes `seq_normalKitState` and `seq_tmpKitState`.
2. AVR `preset_init()` / startup path loads `P000.SND` if present.
3. AVR sends kit/front endpoint bytes into STM normal endpoint storage.
4. STM morph worker eventually computes/interpolates and applies live normal values.
5. Zero-valued params still land in DSP because the live-apply cache starts unknown.

### `.SND` normal kit load

Expected:

- AVR reads `.SND` bytes into `parameter_values[]`.
- AVR sends `FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN` with `FRONT_ONLY`.
- AVR sends `PRF_RESTORE_PARAM_CC/CC2`.
- STM writes `seq_normalKitState.frontPanelParams[]`.
- STM does not write temp arrays.
- STM does not write `interpolatedParams[]` directly.
- Worker later computes live interpolation.

### `.SND` morph endpoint load

Expected:

- AVR reads `.SND` bytes into `parameters2[]` or equivalent morph endpoint staging.
- AVR sends endpoint begin with `MORPH_ONLY`.
- AVR sends `PRF_RESTORE_MORPH_CC/CC2`.
- STM writes `seq_normalKitState.morphParams[]`.
- STM does not write temp arrays.
- Worker later computes live interpolation.

### `.PRF` / `.ALL` load

Expected post-morph model:

- File load writes normal pattern data and normal endpoint data only.
- If temp pattern is currently sounding, file load must not alter temp pattern data, temp endpoint arrays, or temp live sound.
- STM file-load ingress target is `SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT`.
- There is no `SEQ_PARAM_INGRESS_NORMAL_INTERPOLATED`.
- File load may refresh normal automation target caches from sideband target messages.
- File load invalidates the live-apply cache so first worker pass can land zero-valued normal params when normal is live.

## Current Endpoint Dump Protocol

AVR endpoint dump to STM:

1. `SEQ_CC, SEQ_TMP_KIT_ENDPOINT_BEGIN, endpointMode`
2. optional front endpoint bytes:
   - `PRF_RESTORE_PARAM_CC/CC2`
   - store into `seq_normalKitState.frontPanelParams[]`
3. optional front endpoint automation phase:
   - `SEQ_TMP_KIT_AUTOMATION_PHASE, FRONT_ENDPOINT`
   - `CC_VELO_TARGET`, `CC_LFO_TARGET`, `MACRO_CC`
   - store into `seq_normalKitState.frontPanelAutomationTargets`
   - also refresh `seq_normalKitState.interpolatedAutomationTargets`
4. optional morph endpoint bytes:
   - `PRF_RESTORE_MORPH_CC/CC2`
   - store into `seq_normalKitState.morphParams[]`
5. optional morph endpoint automation phase:
   - `SEQ_TMP_KIT_AUTOMATION_PHASE, MORPH_ENDPOINT`
   - store into `seq_normalKitState.morphParameterEndpointAutomationTargets`
6. `SEQ_TMP_KIT_AUTOMATION_PHASE, NONE`
7. `SEQ_TMP_KIT_ENDPOINT_END`

At endpoint end, STM calls `seq_applyNormalEndpointAutomationTargets()` so normal live target routing is not stale.

## Current STM Ingress Modes

Current `sequencer.h` modes:

```c
#define SEQ_PARAM_INGRESS_CURRENT_IMAGE 0
#define SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT 1
```

Removed modes:

- `SEQ_PARAM_INGRESS_TMP_KIT_STATE`
- `SEQ_PARAM_INGRESS_NORMAL_INTERPOLATED`

Implication:

- There is no generic file-load-to-interpolation route.
- There is no direct AVR-to-temp endpoint route.
- Current-image live edits choose active normal/temp endpoint storage in `seq_storeParameterIngress(...)`.
- File-load/endpoints choose normal endpoint storage.

## Current Normal/Temp Model

`seq_normalKitState` and `seq_tmpKitState` both contain:

- kit/front endpoint bytes;
- morph endpoint bytes;
- worker-owned interpolation bytes;
- three automation target cache images.

Current voice source state is tracked per synth voice:

- normal source;
- temp source;
- hi-hat tracks share synth voice 6 behavior.

The morph worker chooses the normal or temp kit image based on `seq_voiceSourceState[synthVoice]`.

## Known Broken Area

The user ended Session 003 with:

- standard morph works;
- normal/temporary pattern and parameter exchange is broken;
- SEQ16 button bodge to observe the temporary cache remains in place.

The exact failure mode is not fully characterized in this handoff. Session 004 should start by reproducing and naming the failure before editing code.

## High-Probability Leads

### 1. Temp capture no longer snapshots interpolation

Session 003 changed `seq_captureTmpKitState()` so interpolation remains worker-owned. It now copies endpoint images rather than snapshotting all current interpolated values in the old manner.

This may be correct architecturally, but it changes temp behavior:

- temp playback may begin before `seq_tmpKitState.interpolatedParams[]` has converged;
- live-apply cache may suppress or delay values after source switch;
- temp menu endpoint arrays may not reflect the intended observed temp cache.

Audit exact current code before changing.

### 2. Endpoint restore sends whole arrays

Per-parameter valid flags were removed. Endpoint restore to AVR now sends the full endpoint arrays.

This is intended, but it changes menu restore:

- zero is no longer "missing";
- every endpoint byte, including zeros, is authoritative;
- stale or incorrectly copied zero endpoint arrays will visibly overwrite menu state.

This is a likely reason temp menu exchange looks wrong.

### 3. File load always targets normal endpoint storage

This is intended:

- file loads should never touch temp storage;
- temp playback should stay isolated during normal file load.

But it means if temp cache uses normal endpoint arrays as a staging area for copy or restore, that route may now conflict with file-load semantics.

### 4. Current-image live edit routing changed

Current-image ingress outside file-load can route to normal or temp endpoint storage based on `seq_voiceSourceState` / `seq_tmpKitActive`.

Review whether live menu edits while temp is active should update:

- temp front endpoint;
- temp morph endpoint;
- temp interpolation only through worker;
- AVR menu arrays.

The old docs talked about live edits writing interpolated/current-play images. That is stale after the morph move; interpolation is worker-owned.

### 5. Shared/non-voice parameters

Most sound params are covered by voice masks. Known non-worker categories:

- `PAR_NONE`
- NRPN placeholders
- `PAR_VOICE_DECIMATION_ALL`
- `PAR_RESERVED4`
- `PAR_UNUSED01`
- `PAR_KIT_VERSION`
- `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`
- macro destination/amount slots

Session 003 added `PAR_ENVELOPE_POSITION_1..6` to the voice masks.

If temp exchange failure involves shared/non-voice params, do not assume the morph worker naturally applies them. Identify which path owns them.

### 6. Live-apply cache invalidation

The live-apply cache fixes zero-valued boot params and avoids continuous DSP setter pokes. It is invalidated at boot and file begin.

Session 004 should ask:

- Should it also invalidate on normal/temp source switch?
- Should it invalidate per voice when `seq_applyVoiceSource(...)` applies a full image?
- Could it suppress a value after another path changed DSP directly?

Do not replace it with continuous apply. That caused a loud low-Hz overlay.

### 7. Automation target cache images

File load target sidebands now update front endpoint and interpolated automation target caches for normal storage. Morph endpoint target sidebands update morph endpoint target caches.

For temp exchange, check:

- what target image is copied to temp;
- what target image is applied on temp entry;
- whether the applied image is endpoint or interpolated;
- whether AVR menu restore needs only selector bytes, not resolved targets.

## Session 004 Suggested First Audit Steps

1. Reproduce the exact temp exchange failure on hardware.
2. Name whether the failure is:
   - wrong sound after copy to temp;
   - wrong menu values after entering temp;
   - wrong temp endpoint arrays;
   - wrong interpolation arrays;
   - wrong automation target routing;
   - file load corrupting temp;
   - normal return restoring wrong values.
3. Use SEQ16 cache observation bodge to compare:
   - AVR `parameter_values[]`;
   - AVR `parameters2[]`;
   - STM normal endpoints;
   - STM temp endpoints;
   - STM interpolation image;
   - actual DSP sound.
4. Trace one concrete parameter, preferably a low waveform param and a high CC2 param.
5. Do not edit until the failed storage boundary is identified.

## Explicit Do-Not-Retry Items

- Do not reintroduce AVR live `preset_morph(...)` as a fix.
- Do not reintroduce `frontPanelParamsValid[]`, `morphParamsValid[]`, or `interpolatedParamsValid[]`.
- Do not restore `SEQ_PARAM_INGRESS_NORMAL_INTERPOLATED`.
- Do not treat zero endpoint values as absent.
- Do not route file loads into temp storage.
- Do not use `parameterArray` to reconstruct menu endpoint bytes; it points into live converted/modulated DSP state.
- Do not use `midiParser_originalCcValues` as a canonical loaded parameter store.
- Do not add LCD/debug/status output unless explicitly requested.

## Useful Test Files

Root test files present during Session 003:

- `P000.SND`: boot kit used to confirm raw parameter decode and waveform zero apply.
- `P000.ALL`: used to expose file-load and morph endpoint issues.
- `P005.PRF`: used to confirm LFO target to voice 6 morph.

These files are test artifacts and may be removed later. Do not assume their worktree status is intentional code change.

## Current Success Baseline To Preserve

Before fixing temp exchange, confirm the Session 003 success still holds:

- boot `P000.SND` Drum 2 waveform lands as `0`, not DSP init `2`;
- no continuous low-Hz overlay;
- global morph works from menu;
- per-voice morph step automation works;
- LFO1 targeting `PAR_MORPH_HIHAT` in `P005.PRF` works on load;
- LFO morph modulation depth `0` produces no modulation travel.

If any of these regress, stop and restore the STM morph invariants before working on temp exchange.
