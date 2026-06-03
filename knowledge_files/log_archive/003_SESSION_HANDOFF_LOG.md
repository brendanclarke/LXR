# Session 003 Handoff Log

## Start Context

**Project**: LXR -bc- Enhanced Firmware  
**Session goal**: Move morph computation fully onto the STM for standard operation.  
**Working repository**: `/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod`, branch `custom-develop-patload-envmod`.  
**Date closed**: 2026-06-03  

Session 003 started from the Session 002 temp-pattern baseline and the in-flight `AUDIT_MORPH_MOVE.md` plan. The user hardware-tested after most phases and provided concrete behavioral reports. By the end of the session, standard morph operation had been moved onto the STM and was hardware-confirmed sounding correct, including LFO automation targeting per-voice morph. However, the normal/temporary pattern and parameter exchange is now broken and must be the goal of Session 004. The SEQ16 button bodge to observe the temporary cache is still in place.

This log intentionally preserves the current morph/parameter-storage details because `knowledge_files/session_in_flight/AUDIT_MORPH_MOVE.md` is expected to be deleted after this handoff.

## End Of Session Block

```text
DATE: 2026-06-03
SESSION GOAL: Move morph computation fully onto the STM for standard operation.
COMPLETED: STM now owns morph amounts, morph interpolation, morph live DSP application, and LFO/velocity modulation of per-voice morph. AVR no longer performs standard live morph computation. File/menu endpoints remain AVR-visible but live morph sound state is STM-owned.
VERIFIED ON HARDWARE: YES. User verified ordinary morph operation, coarse pitch endpoint behavior, temp endpoint randomness improvement after cursor model changes, corrected modulation model, file-load sound after live-apply cache fix, and LFO automation targeting voice morph. Final user report: "yup sounds good." Normal/temporary pattern and parameter exchange remains broken.

CHANGES THIS SESSION:
- mainboard/LxrStm32/src/Sequencer/sequencer.c: Added STM morph state, per-voice morph amounts, global morph routing, one-parameter-per-main-loop morph worker, raw-indexed voice masks, STM modTargets mirror, automation target resolution, live DSP apply cache, normal/temp image-aware morph application, step automation handling for per-voice morph, and LFO/velocity modulation of morph.
- mainboard/LxrStm32/src/Sequencer/sequencer.h: Removed per-parameter valid arrays from SeqKitState, added STM morph and endpoint ingress declarations, and reduced parameter ingress modes to current image vs normal kit endpoint.
- mainboard/LxrStm32/src/main.c: Calls seq_serviceMorphInterpolation() once per main loop.
- mainboard/LxrStm32/src/MIDI/MidiMessages.h: Added FRONT_SEQ_SET_GLOBAL_MORPH and endpoint automation phase protocol constants.
- mainboard/LxrStm32/src/MIDI/frontPanelParser.c: Routes file loads to normal endpoint storage, handles raw PRF endpoint bytes, handles morph endpoint bytes, applies endpoint automation targets, resets per-voice morph/cache on file begin, and preserves temp-file-load isolation rules as much as current WIP allows.
- mainboard/LxrStm32/src/MIDI/MidiParser.c: Routes per-voice morph MIDI/automation targets into STM morph state instead of AVR live morph.
- mainboard/LxrStm32/src/DSPAudio/modulationNode.c: Routes modulation destinations PAR_MORPH_DRUM1..PAR_MORPH_HIHAT to STM morph modulation.
- front/LxrAvr/Menu/menu.c: PAR_MORPH edits now send STM global morph amount; morph endpoint edits send raw morph endpoint bytes instead of triggering AVR live morph.
- front/LxrAvr/Preset/presetManager.c: File load/dump sends endpoint arrays and resolved target sidebands to STM; ordinary file load no longer relies on AVR preset_morph() to generate live sound state.
- front/LxrAvr/frontPanelParser.c and .h: Added/updated restore/cache/control messages for endpoint and morph routing; old live morph compute paths made inert where needed.
- firmware image/FIRMWARE.BIN: Rebuilt after final STM changes.
- knowledge_files/session_in_flight/AUDIT_MORPH_MOVE.md: Updated throughout the session with findings and fixes.

KNOWN ISSUES INTRODUCED: Normal/temporary pattern and parameter exchange is broken after the morph move. The likely lead is the changed storage validity/liveness model: endpoint arrays are always valid, interpolation is worker-owned, valid arrays were removed, and temp capture no longer snapshots interpolation in the same old way. Session 004 should audit this path from copy-to-temp through temp playback/menu restore.
KNOWN ISSUES RESOLVED: Standard morph moved off AVR and onto STM; stale endpoint file loads no longer zero/misassign ordinary params; LFO target to voice morph works on load; zero-valued loaded params now land in DSP even when DSP init defaults are nonzero; continuous low-Hz overlay from over-applying DSP setters was fixed with a live-apply cache.

NEXT SESSION RECOMMENDED GOAL: Fix the normal/temporary pattern and parameter exchange after the morph move. Start from TEMP_CACHE_LOAD-IN_FLIGHT-POST_MORPH_MOVE.md and COMMS_FLOW_AUDIT-IN_FLIGHT-POST_MORPH_MOVE.md.
BLOCKERS: Requires hardware testing of SEQ16 temp cache observation, copy normal-to-temp, temp playback, temp-to-normal return, file load while temp is active, and menu endpoint pushback.

CRITICAL REMINDERS FOR NEXT SESSION:
- Do not restore the old AVR live morph path as a temp-cache fix.
- Do not reintroduce per-parameter valid arrays in SeqKitState; parameter images are always-defined sound state from zero init.
- File load should only overwrite normal endpoint arrays, not temp endpoint arrays and not interpolation arrays.
- The only writer of interpolatedParams[] should remain the STM morph worker.
- Automation target caches may be refreshed by file-load/front endpoint sidebands; that is not the same as writing interpolated parameter bytes.
- Raw endpoint parameter indices now match AVR/menu indices. Low +1 exists only when applying ordinary low CC to the STM MIDI parser/DSP path.
- The SEQ16 button bodge to observe the temporary cache is still in place.
```

## What Was Actually Accomplished

Session 003 completed the intended architectural move for standard morph:

- AVR retains UI, file I/O, and menu-visible endpoint bytes in `parameter_values[]` and `parameters2[]`.
- STM owns global and per-voice morph state.
- STM owns the interpolation worker that writes `interpolatedParams[]`.
- STM applies live DSP values from its own endpoint/interpolated storage.
- Step automation and modulation targets can set or modulate per-voice morph on STM.
- LFO/velocity modulation of a per-voice morph destination modulates between the stored interpolation baseline and the full morph endpoint without overwriting `interpolatedParams[]`.

The move was not just a direct port of AVR `preset_morph(...)`. Several intermediate models failed or were corrected:

- A cursor-restart/dirty-state model was removed after user clarification. The final worker walks parameters sequentially in the background rather than restarting or grouping by voice as a dirty job.
- A first modulation implementation was corrected. Velocity/LFO modulation must not write stored interpolation values; it only produces a temporary live DSP value between the stored interpolated baseline and morph endpoint.
- Automation target selector parameters and morph amount parameters are carved out of ordinary interpolation. Selector bytes return the kit/front endpoint value to avoid a parameter being both a morph generator and a morphed destination selector. Morph amount parameters are control inputs, not ordinary morphable sound outputs.
- LFO target voice selector parameters are also carved out.
- The old AVR-to-STM/STM-to-AVR low-CC offset assumptions from Session 002 were corrected. Stored endpoint bytes are raw AVR/menu parameter indices on both MCUs. Low `+1` only applies when turning a raw parameter index into an ordinary live STM MIDI CC for the `midiParser_ccHandler(...)` path.

## Hardware-Tested Outcome

The user reported these final outcomes:

- Ordinary morph works well enough to close Session 003.
- LFO automation targeting voice morph works after loading a performance file.
- The previously observed `PAR_OSC_WAVE_DRUM2` boot mismatch was fixed. The menu and endpoint file said waveform `0`, while DSP init had left Drum 2 at waveform `2`; the live-apply cache fix made zero-valued file params land in DSP.
- The continuous low-Hz overlay caused by continuously applying every parameter was fixed by replacing that blunt behavior with a live-apply cache.
- Normal/temporary pattern and parameter exchange is broken and must be handled in Session 004.

## Core Current STM Data Model

`SeqKitState` now has always-defined byte arrays:

```c
uint8_t frontPanelParams[END_OF_SOUND_PARAMETERS];
uint8_t morphParams[END_OF_SOUND_PARAMETERS];
uint8_t interpolatedParams[END_OF_SOUND_PARAMETERS];
SeqKitAutomationTargets frontPanelAutomationTargets;
SeqKitAutomationTargets morphParameterEndpointAutomationTargets;
SeqKitAutomationTargets interpolatedAutomationTargets;
uint8_t valid;
```

Important: the per-parameter valid arrays are gone:

- `frontPanelParamsValid[]` removed.
- `morphParamsValid[]` removed.
- `interpolatedParamsValid[]` removed.

The remaining `valid` byte is not a per-parameter validity model. Do not rebuild parameter semantics around it without a fresh design decision.

### Validity Decision

The user explicitly rejected a "half-written parameter data" model. The accepted model is:

- Sound parameter arrays are always live and always valid from init.
- At init, normal and temporary parameter structs are zeroed.
- If `P000.SND` exists at boot, the AVR loads it and sends its bytes into the STM normal kit/front endpoint array.
- If `P000.SND` is absent, the zeroed normal endpoint array is still a valid sound state.
- There is no per-parameter "missed" or "invalid" marker in the sound framework.
- Any transfer completeness concern belongs in the transfer framework, not inside sound state storage.

This is critical for Session 004: do not solve temp-cache problems by bringing back per-parameter valid flags. The bug should be found in routing, timing, cache invalidation, endpoint/interpolation ownership, or menu restore sequencing.

## File Load To DSP Flow After Session 003

The intended flow from file bytes to DSP is:

1. AVR reads file bytes into `parameter_values[]` and/or `parameters2[]`.
2. AVR sends endpoint byte dumps to STM:
   - `PRF_RESTORE_PARAM_CC/CC2` for kit/front endpoint bytes.
   - `PRF_RESTORE_MORPH_CC/CC2` for morph parameter endpoint bytes.
3. STM stores those bytes into normal endpoint storage:
   - `seq_normalKitState.frontPanelParams[]`
   - `seq_normalKitState.morphParams[]`
4. File load must not write:
   - `seq_tmpKitState.frontPanelParams[]`
   - `seq_tmpKitState.morphParams[]`
   - either normal or temp `interpolatedParams[]`
5. AVR sends resolved automation target sidebands in explicit phases:
   - front endpoint target phase updates `seq_normalKitState.frontPanelAutomationTargets`
   - morph endpoint target phase updates `seq_normalKitState.morphParameterEndpointAutomationTargets`
   - front endpoint target phase also refreshes `seq_normalKitState.interpolatedAutomationTargets` so live routing is not stale after file load
6. STM morph worker later reads endpoints, performs interpolation, writes `interpolatedParams[]`, updates interpolated automation target caches when selector params are touched, and applies DSP if that image/voice is live.

The exact invariant:

- AVR/file ingress writes endpoint arrays.
- Morph worker writes interpolation arrays.
- Live DSP apply is guarded by the live-apply cache, not by parameter validity.

## Raw Parameter Indexing Rule

Session 002 had an apparent rule that STM canonical indices were `+1` for low parameters. Session 003 proved that was wrong for endpoint storage after the morph move.

Current rule:

- File bytes, AVR `parameter_values[]`, AVR `parameters2[]`, STM endpoint arrays, and PRF restore opcodes all use raw AVR/menu parameter indices.
- Example: `PAR_OSC_WAVE_DRUM2 == 2`; `P000.SND` value for param 2 is stored at STM endpoint index 2.
- Only ordinary low live MIDI CC application to `midiParser_ccHandler(...)` uses `param + 1`.
- Example: raw `PAR_OSC_WAVE_DRUM2 == 2` becomes live CC `OSC_WAVE_DRUM2 == 3`.

Do not apply low `+1/-1` to `PRF_RESTORE_PARAM_CC`, `PRF_RESTORE_MORPH_CC`, or endpoint arrays.

## Morph Worker Details

`seq_serviceMorphInterpolation()` is called once per STM main loop from `main.c`.

The current worker:

- advances a global parameter cursor every call;
- finds the synth voice mask for the current parameter;
- chooses normal or temp kit image based on the source state of the synth voice;
- computes either:
  - kit/front endpoint for automation selector carve-outs; or
  - interpolation between kit/front endpoint and morph endpoint using the per-voice morph amount;
- writes the computed value to the selected kit's `interpolatedParams[param]`;
- updates `interpolatedAutomationTargets` for target selector params;
- applies the live value to DSP only if the corresponding image/voice is live and the live-apply cache says the value has not already been sent.

The worker no longer uses dirty flags as the driving model. The user explicitly requested a background sequential pass instead of a cursor-restart mechanism.

## Live-Apply Cache

The first fix for zero-valued file params was to apply every processed parameter every pass. That caused a continuous loud low-Hz tone overlay because some DSP setters are not harmless when repeatedly poked.

Final fix:

- `seq_liveMorphAppliedValue[image][param]`
- `seq_liveMorphAppliedKnown[image][param]`

This cache is outside `SeqKitState`. It is not a parameter validity system. It answers only:

"Has this morph apply path already sent this exact value to live DSP for this normal/temp image?"

Rules:

- On boot/init, cache is unknown.
- On file begin, cache is invalidated.
- First computed value applies even when it is zero.
- Later equal values do not continuously reapply.

This fixed the `P000.SND` Drum 2 waveform case:

- file/menu endpoint had `PAR_OSC_WAVE_DRUM2 = 0`;
- `DrumVoice_init()` had initialized Drum 2 waveform to `2`;
- old code saw `interpolatedParams[2] == 0` and skipped applying;
- new cache sees no live apply is known and sends the zero once.

## Automation Target Resolution And Voice Morph

STM now mirrors the AVR `modTargets[]` selector-to-parameter map in `sequencer.c`. This lets STM resolve selector bytes without asking AVR to compute live morph.

Relevant behavior:

- AVR endpoint byte arrays still contain selector bytes.
- AVR file-load endpoint dumps send resolved target sidebands bracketed by endpoint phase.
- STM stores resolved target sidebands into the corresponding automation target image.
- Front endpoint sidebands also refresh `interpolatedAutomationTargets` so file-loaded LFO/velocity destinations can take effect immediately.

Example: `P005.PRF` LFO1 targeting voice 6 morph.

Current decode during the session:

- `P005.PRF` version 5.
- Perf kit endpoint offset: `74`.
- Perf morph endpoint offset: `586`.
- Kit endpoint LFO1:
  - `PAR_VOICE_LFO1 = 6`
  - `PAR_TARGET_LFO1 = 210`
  - current `modTargets[210] -> PAR_MORPH_HIHAT`
  - `PAR_AMOUNT_LFO1 = 115`
  - `PAR_FREQ_LFO1 = 94`
  - `PAR_WAVE_LFO1 = 2`

Load/apply path:

1. AVR reads `.PRF` and populates `parameter_values[]` / `parameters2[]`.
2. AVR dumps endpoint bytes to STM normal endpoint storage.
3. AVR sends `SEQ_TMP_KIT_AUTOMATION_FRONT_ENDPOINT`.
4. AVR sends `CC_LFO_TARGET` for LFO1 resolved from `parameter_values[PAR_TARGET_LFO1]`.
5. STM receives `FRONT_CC_LFO_TARGET` during endpoint ingress. Since live apply is suppressed during endpoint copy, it stores the target without touching the modulation node immediately.
6. STM stores the resolved target into `seq_normalKitState.frontPanelAutomationTargets.lfoDestination[0]` and refreshes `seq_normalKitState.interpolatedAutomationTargets.lfoDestination[0]`.
7. At `SEQ_TMP_KIT_ENDPOINT_END`, STM calls `seq_applyNormalEndpointAutomationTargets()`.
8. If the normal image is live, LFO1's modulation node destination is set to `PAR_MORPH_HIHAT`.
9. When LFO1 runs, `modulationNode.c` routes `PAR_MORPH_HIHAT` to STM per-voice morph modulation for synth voice 6.
10. `seq_modulateVoiceMorphAmount(...)` computes live values between the stored interpolation baseline and morph endpoint and applies them to DSP without overwriting `interpolatedParams[]`.

The user confirmed this now sounds correct.

## Removed / Retired AVR Morph Behavior

Important practical result:

- AVR `preset_morph(...)` may still exist in source as a helper/legacy function, but standard live morph should not rely on it.
- AVR `PAR_MORPH` menu edits send `SEQ_SET_GLOBAL_MORPH` to STM.
- STM global morph writes all six per-voice morph amounts.
- Step automation for `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` updates STM per-voice morph directly.
- Modulation destinations for `PAR_MORPH_*` update live morph via STM modulation routing.

Do not reintroduce a live AVR morph stream to work around temp-cache bugs.

## Temp Cache / Normal-Temporary Exchange Status

This is the major unresolved problem.

Normal/temporary pattern and parameter exchange was already fragile in Session 002. Session 003 changed the data model enough that the old temp exchange behavior is no longer trustworthy.

Known final state:

- Standard morph works.
- Normal/temp exchange is broken.
- The SEQ16 button bodge to observe the temporary cache remains in place.
- New post-morph audits were created:
  - `knowledge_files/session_in_flight/TEMP_CACHE_LOAD-IN_FLIGHT-POST_MORPH_MOVE.md`
  - `knowledge_files/session_in_flight/COMMS_FLOW_AUDIT-IN_FLIGHT-POST_MORPH_MOVE.md`

Likely leads:

- `seq_captureTmpKitState()` no longer snapshots `interpolatedParams[]` / `interpolatedAutomationTargets` in the same old way. It copies normal front/morph endpoints and automation endpoint images, leaving interpolation owned by the worker.
- File load writes only normal endpoints; if temp is live, live apply should be suppressed by normal/temp image checks.
- Menu restore still pushes endpoint arrays back to AVR, but endpoint restore no longer filters by per-parameter valid arrays and sends whole endpoint arrays.
- Temp endpoint/menu restoration and worker convergence may now be out of phase.
- Live-apply cache invalidation around normal/temp switches may need review.
- Shared/non-voice params remain a special category and are not naturally handled by the voice-parameter morph pass except through separate shared apply paths.

Session 004 should not treat this as a comms-only problem until the storage path is proven.

## Build / Verification

Final relevant build checks during Session 003:

- `make -C mainboard/LxrStm32 stm32` passed after final live-apply cache fix.
- `make firmware` passed after final live-apply cache fix.
- `git diff --check` passed.

Existing warnings remain and were not addressed:

- STM mixer inline warnings.
- duplicate `const` warnings in `modulationNode.c`.
- `midi_envPosition` / `seq_lastMasterStep` array-bound warnings in `seq_init`.
- fallthrough and unused helper warnings in front parser.
- linker warning about RWX load segment.

## Dirty Worktree Notes At Handoff

At closeout, the working tree is expected to remain dirty. Notable status from the session:

- Modified source files across AVR and STM morph/temp/comms paths.
- `firmware image/FIRMWARE.BIN` regenerated.
- `.DS_Store` modified.
- `P000.ALL` modified in the worktree; this was not intentionally edited by Codex and should not be reverted casually.
- `P000.SND` appears untracked as a root test file.

Respect user edits and test files. Do not revert unrelated dirty files unless explicitly instructed.

## Files Touched By Session 003

Primary STM files:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/Sequencer/sequencer.h`
- `mainboard/LxrStm32/src/main.c`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiMessages.h`
- `mainboard/LxrStm32/src/DSPAudio/modulationNode.c`

Primary AVR files:

- `front/LxrAvr/Menu/menu.c`
- `front/LxrAvr/Preset/presetManager.c`
- `front/LxrAvr/frontPanelParser.c`
- `front/LxrAvr/frontPanelParser.h`

Knowledge files:

- `knowledge_files/session_in_flight/AUDIT_MORPH_MOVE.md`
- `knowledge_files/log_archive/003_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`
- `MEMORY.md`
- `knowledge_files/session_in_flight/TEMP_CACHE_LOAD-IN_FLIGHT-POST_MORPH_MOVE.md`
- `knowledge_files/session_in_flight/COMMS_FLOW_AUDIT-IN_FLIGHT-POST_MORPH_MOVE.md`

## Session 004 Recommended Opening Prompt

```text
Project: LXR -bc- Enhanced Firmware
Session goal: Fix normal/temporary pattern and parameter exchange after morph was moved fully onto STM.
Last session summary: Session 003 moved standard morph fully onto STM. Standard morph and LFO target to voice morph now work on hardware. Normal/temporary pattern and parameter exchange is broken. Per-parameter valid arrays were removed; parameter arrays are always-defined from zero init; file loads write only normal endpoint arrays; interpolatedParams[] is worker-owned; live DSP apply uses a separate live-apply cache. See 003_SESSION_HANDOFF_LOG.md, TEMP_CACHE_LOAD-IN_FLIGHT-POST_MORPH_MOVE.md, and COMMS_FLOW_AUDIT-IN_FLIGHT-POST_MORPH_MOVE.md.
Working repository: /Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod on branch custom-develop-patload-envmod, dirty WIP tree.
Constraints today: Do not reintroduce AVR live morph or per-parameter valid arrays. Keep focus on temp cache loading/exchange.
```
