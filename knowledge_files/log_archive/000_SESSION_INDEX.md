# LXR -bc- Enhanced Firmware — Session Index

**Project**: Fork of LXR drum machine firmware  
**Repo**: `/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod` | **Log format**: `00x_SESSION_HANDOFF_LOG.md`

---

## Quick Reference — What's In Each Log

| # | Date | Source at end | Topic |
|---|------|----------------|-------|
| 001 | 2026-05-21 | local branch `custom-develop-patload-envmod` (commit `3698612`) | build-break triage, source compatibility fixes (A1/A2/A3/B1/B2/B3), full firmware build verified |
| 002 | 2026-05-29 to 2026-06-01 | local branch `custom-develop-patload-envmod` (merged temp-pattern WIP) | temp-pattern parameter isolation, symmetric kit states, endpoint/automation storage, normal-only file loads, rate-limited endpoint restore, morph-to-STM plan |
| 003 | 2026-06-03 | local branch `custom-develop-patload-envmod` (STM morph WIP, dirty tree) | morph computation moved fully to STM for standard operation; normal/temp exchange broken and queued for session 004 |
| 004 | 2026-06-07 | local repo, user will commit/push after review | temp background loading made functional; normal/temp parameter switching, endpoint sync, retrigger glitch, automation target persistence, and phased LFO-to-morph drain fixed |
| 005 | 2026-06-09 | local repo after reset, uncommitted docs/code | targeted global morph menu-sync fix; display-only STM report on normal/temp boundary; session 005 closeout docs updated |
| 006 | 2026-06-09 | local repo, uncommitted planning docs | refactor planning and architectural alignment for STM-side preset/morph subsystems; finalized phased roadmap |
| 007 | 2026-06-09 | local repo, Phase 1 complete | refactor phase 1: carved core Preset types (KitState, ParameterMap, ParameterIngress) into new module; established ingress authority |
| 008 | 2026-06-09 | local repo, uncommitted docs | AVR startup substep toggle bug root-cause analysis and fix; identified DIN initialization mismatch and polling race |
| 009 | 2026-06-10 | local repo, Phase 2 complete | refactor phase 2: moved morph engine and live-apply suppression logic to new Preset/MorphEngine module; updated main loop and established authoritative DSP application bridge |
| 010 | 2026-06-11 | local repo, Phase 3 complete | refactor phase 3: moved endpoint restore, temp switching, and background-load session bookkeeping into Preset; parser now consumes PresetLoadCache API; build verified |
| 011 | 2026-06-12 | local repo, Phase 4 complete | refactor phase 4: moved pattern storage, pattern copy/mutation helpers, and Euclid/SOM generators into Sequencer/Pattern; sequencer.c trimmed back to scheduler/trigger role |
| 012 | 2026-06-12 | local repo, Phase 5 complete | refactor phase 5: moved front-panel UART/protocol layer into uARTFrontSYX, split opcode namespace into FrontPanelProtocol.h, smoke-tested .ALL/temp-switch flow |
| 013 | 2026-06-13 | local repo, session 013 closeout | Phase 6 cleanup/naming closeout, Phase 7+ consolidation audit, comms/temp spec reshaping, and handoff prep for session 014 |
| 014 | 2026-06-13 | local repo, Phase 7 shared-module removal | Sequencer/MIDI now read live owner state directly, the background-load finalizer stays on TempPlaybackSwitch, and the remaining parser/session bridge now lives inside frontPanelParser.c |
| 015 | 2026-06-13 | local repo, AVR encoder Timer1 compare cutover | AVR front-panel encoder now uses Timer1 compare polling; the dead 2-step API is gone, and the hardware re-test passed though reversals remain rough |
| 016 | 2026-06-13 | local repo, AVR encoder rest-FSM final | AVR main encoder now uses only `encode_stableRead4()` backed by a Timer1 16 kHz fixed-`AB=11` rest-phase FSM; hardware test passed and debug scaffolding was removed |
| 017 | 2026-06-14 | local repo, Preset rename completion + UART send split mapping | Phase 10/11 Preset cleanup was finalized, Phase 12 was confirmed as retrospective only, and the outbound front-panel send split was mapped into the comms reference |
| 018 | 2026-06-14 | local repo, UART send/receive split implementation | Front-panel send helpers were consolidated, the MIDI parser was split into channel/global ownership files, and the transitional front-panel load/session bridge moved into `PresetLoadCache` |
| 019 | 2026-06-14 | local repo, AVR encoder retune + acceleration | Main encoder tuning moved to ~32 kHz Timer1 sampling with six-sample phase filtering, narrow rest-jump recovery, stronger button debounce, and edit-mode acceleration |
| 020 | 2026-06-14 | local repo, refactor finalization and protocol split closeout | Removed obsolete `PresetLoadCache`, finalized STM/AVR receive/send protocol filenames, removed legacy shims, and superseded the stale preset/MIDI UART audits |

---

## Session Summaries

### 001 — Build Recovery + Session Memory Baseline (2026-05-21)
Session 001 established project context docs, audited build failures, applied the requested source-level compatibility fixes, and verified a successful end-to-end `make clean && make firmware` build in the active repository. Session closeout added the first formal handoff log and a full MEMORY baseline for subsequent sessions.
- **Find here**: build failure root cause, A/B fix groups, toolchain requirements draft, repository-path mismatch lesson, successful firmware image output

### 002 — Temp Pattern Parameter Isolation + Morph Move Prep (2026-05-29 to 2026-06-01)
Session 002 completed encoder work, restored the `.ALL` / `.PRF` load and parameter-pushback baseline, then advanced the temporary-pattern model: symmetric normal/temp `SeqKitState` storage, kit/front and morph parameter endpoint capture, three resolved automation-target images per kit, normal-only file-load routing, lazy temp initialization, per-track endpoint sync, low-CC offset fixes, endpoint/menu restore chirp isolation, queued/rate-limited endpoint restore, and the audit plan for moving morph computation fully onto STM.
- **Find here**: encoder completion, comms/load checkpoint, restore handshake, +1/-1 parameter offset, SEQ16 temp pattern, symmetric `SeqKitState`, endpoint dump/copy-to-temp, automation target image storage, normal-only file loads, temp edit isolation, per-track temp/normal endpoint sync, endpoint restore chirp, rate-limited restore, AVR morph-state audit, STM morph move plan

### 003 — STM-Owned Morph Move + Post-Morph Temp Cache Handoff (2026-06-03)
Session 003 moved standard morph computation fully onto STM: STM now owns global/per-voice morph state, one-parameter-per-main-loop interpolation, live DSP morph application, LFO/velocity modulation of voice morph, raw endpoint-index storage, automation-target image refresh, and the live-apply cache needed for zero-valued file parameters to land in DSP. The session also removed per-parameter valid arrays from `SeqKitState`; sound parameter arrays are always-defined from zero init, file loads write normal endpoint arrays only, and the morph worker is the only intended writer of interpolation arrays. Standard morph was hardware-confirmed, but the normal/temporary pattern and parameter exchange is broken after the move and is the recommended Session 004 goal.
- **Find here**: STM-owned morph, per-voice morph, global morph command, raw endpoint indexing, automation target sidebands, LFO target to voice morph, valid-array removal, file-load endpoint-to-DSP flow, live-apply cache, zero-valued parameter apply bug, waveform Drum 2 fix, temp exchange broken, SEQ16 temp cache observation bodge, post-morph Session 004 audit plan

### 004 — Temp Background Loading Stabilization + Morph/Automation Repair (2026-06-07)
Session 004 made the post-morph temp/background-load model functional: copy-to-temp became an STM-side pattern/parameter snapshot, switching normal/temp became source selection rather than staging, AVR endpoint restore now keeps the menu synced on boundaries, legacy load hold/cache paths were neutralized for temp loading, the retrigger-like glitch was removed by rebuilding the live-apply path, LFO target assignments to voice morph now survive normal/temp switches, velocity-to-morph is a trigger-time one-shot, and LFO-to-morph is serviced by a phased morph drain so only one interpolation/apply runs per STM main-loop pass. Session 004 also identified the future `/Preset/` refactor boundary and created canonical refactor/comms knowledge for Session 006/007.
- **Find here**: STM-only copy-to-temp, normal/temp source switching, endpoint push-up/menu sync, first-switch freeze removal, sequence LED restore after load, retrigger glitch diagnostics/fix, raw selector vs resolved automation target coherence, LFO/velocity target to voice morph, phased morph drain, AVR temp buffer naming, preset/morph refactor plan, UART/protocol redundancy notes

### 005 — Global Morph Menu Sync Closeout + Audit Cleanup (2026-06-09)
Session 005 closed the remaining global morph menu-sync bug after the reset: STM now reports the selected kit image's `globalMorphAmount` back to AVR only on actual normal/temp boundary restores, and AVR updates menu state without echoing the change back into STM sound state. The session kept the scope narrow, documented the fix in the session logs, and left the broader preset/background-load refactor for later.
- **Find here**: display-only global morph report opcodes, normal/temp boundary restore behavior, AVR menu update without STM echo, session 005 closeout logs and audit notes

### 006 — Refactor Planning + Phased Roadmap Finalization (2026-06-09)
Session 006 finalized the comprehensive phased refactor plan for the STM-side preset, morph, and pattern subsystems. Key decisions were made to unify background loading into a single overwriteable session model, retire legacy voice-cache promotion paths, and strictly separate UART transport from protocol parsing. The resulting roadmap served as the authoritative guide for the upcoming implementation phases.
- **Find here**: refactor phased roadmap, module boundaries for `/Preset/` and `/uARTFrontSYX/`, single background-load session model, transport vs protocol separation decisions, retired legacy load paths

### 007 — Refactor Phase 1: Core Preset Module Carve-out (2026-06-09)
Session 007 implemented Phase 1 of the architectural refactor. It established the new `mainboard/LxrStm32/src/Preset/` directory and moved the core sound-state structures (`SeqKitState`), parameter mapping logic, and ingress authority into `KitState`, `ParameterMap`, and `ParameterIngress` modules. `Sequencer` remains a compatibility façade for this phase, while `MidiParser` and `frontPanelParser` were migrated to use the new `preset_*` ingress APIs directly. Extensive documentation was added to all new and moved code to ensure ownership and intent are clear for future phases.
- **Find here**: Preset module structure, KitState ownership, ParameterMap resolution, ParameterIngress authority, sequencer compatibility façade, deep comment requirements

### 008 — AVR Startup Substep Toggle Bug Fix (2026-06-09)
Session 008 identified and fixed a bug where the AVR would incorrectly toggle all substeps of the first pattern's first step on startup. The root cause was a mismatch between the software button-state mirror (initialized to 'all pressed') and the actual hardware state (all released), combined with a polling order that processed SELECT buttons before the SHIFT button. This resulted in a phantom "shifted release" event on the first scan. The fix involved synchronizing the software mirror with the hardware state in `din_init()` before the main loop begins.
- **Find here**: AVR startup bug, DIN initialization, button-state mirror synchronization, phantom release events, polling order dependency, hardware/software state alignment

### 009 — Refactor Phase 2: Morph Engine and Live-Apply (2026-06-10)
Session 009 implemented Phase 2 of the architectural refactor. The morph engine, interpolation worker, phased LFO-to-morph drain, and live-apply suppression cache were moved into the new `mainboard/LxrStm32/src/Preset/MorphEngine` module. All relocated logic was renamed with the `preset_` prefix. A new authoritative DSP application bridge, `preset_applySingleParameterValue()`, was established in `ParameterIngress.c`. The `sequencer.h` header was updated with compatibility wrappers to ensure the rest of the codebase remains functional during the transition.
- **Find here**: Morph engine relocation, interpolation worker, live-apply cache, phased morph drain, DSP application bridge, preset API renaming, sequencer compatibility wrappers

### 010 — Refactor Phase 3: Endpoint Restore, Temp Switching, and Background-Load Finalizer (2026-06-11)
Session 010 completed the Phase 3 ownership split. Endpoint restore policy moved into `Preset/EndpointRestore`, temp/normal source selection and switch state moved into `Preset/TempPlaybackSwitch`, and the PRF/background-load cache moved into `Preset/PresetLoadCache`. The parser now imports the new cache API directly, including the pending-counter reset helper, and the Phase 3 build was verified with `make stm32 -j4`.
- **Find here**: Endpoint restore ownership, temp-switch state split, background-load session cache, parser-to-PresetLoadCache wiring, pending-counter reset helper, build verification

### 011 — Refactor Phase 4: Pattern Ownership and Generator Relocation (2026-06-12)
Session 011 completed Phase 4 of the architectural refactor. Pattern storage and mutation now live in `Sequencer/Pattern/PatternData.c/.h`, the Euclid/SOM generator files were moved under the same directory, and `sequencer.c` was trimmed back to the real-time scheduler/trigger role while retaining the compatibility façade in `sequencer.h`. Build wiring was updated for the new pattern submodule and the firmware build remained green after the move.
- **Find here**: PatternData ownership, pattern copy/clear/mutation helpers, Euclid/SOM generator relocation, sequencer façade trim, build wiring, stale dependency cleanup

### 012 — Refactor Phase 5: Front-Panel Transport/Protocol Relocation (2026-06-12)
Session 012 completed the Phase 5 front-panel transport/protocol split. The front-panel UART transport and parser now live under `uARTFrontSYX/`, the opcode namespace moved into `FrontPanelProtocol.h` behind a compatibility include from `MidiMessages.h`, and the build remained green after the move. The session also smoke-tested `.ALL` load, morph, parameter change, copy to temp, load new `.ALL`, and switch-back flow; MIDI-specific verification remains for later. `PresetLoadCache` continues to own PRF/background-load session state, while the remaining legacy hold/unhold cleanup is deferred to the next phase.
- **Find here**: front-panel UART transport relocation, parser relocation, opcode namespace split, compatibility include, build wiring, hardware smoke test, next-phase legacy cleanup

### 013 — Session 013 Closeout: Phase 6 Completion and Phase 7 Handoff (2026-06-13)
Session 013 closed out the Phase 6 cleanup/naming pass and then turned the refactor into its next-phase handoff. The remaining useful material from the old refactor docs was migrated into the new Phase 7+ consolidation audit plus the two session-in-flight specs for UART comms and temporary/pattern/parameter load behavior. Session 013 also wrote the detailed handoff log, updated the session index, and refreshed MEMORY so the next session can start from the new canonical docs instead of the retired planning files.
- **Find here**: Phase 6 closeout, `PRESET_CONSOLIDATION_AUDIT.md`, `COMMS_FLOW_SPEC.md`, `TEMPORARY_PAT_PARAM_LOAD_SPEC.md`, handoff log and index updates, retired planning-doc cleanup

### 014 — Session 014: Phase 7 Shared-Module Removal (2026-06-13)
Session 014 finished the Phase 7 shared-module removal. `Sequencer`, `MidiParser`, and `MidiVoiceControl` now read live voice, pattern, and morph state directly from the owning modules, `presetLoad_finalizeTempBackgroundLoad()` stays on the `TempPlaybackSwitch`-facing interface so `sequencer.c` no longer needs the old cache header, and `PresetLoadCache.c/.h` were deleted in favor of the parser-local transitional bridge inside `frontPanelParser.c`. The code still has one remaining parser-local holdover to retire in the follow-on consolidation work, but the shared cache module itself is gone and the build is green.
- **Find here**: direct owner reads, live pattern/morph cutover, `TempPlaybackSwitch` finalizer interface, parser-local transitional bridge, shared cache deletion, build verified

### 015 — AVR Encoder Timer1 Compare Cutover + API Pruning (2026-06-13)
Session 015 closed the AVR front-panel encoder follow-up by replacing the fragile stable-driver experiment with Timer1 compare polling, removing the dead 2-step encoder surface, and making the 4-step reader direction-aware again. The user re-tested the hardware, confirmed it works, and accepted the remaining rough feel on reversals and fast spins so the encoder work could be parked for now.
- **Find here**: Timer1 compare encoder path, dead 2-step API removal, direction-aware detent accumulator, hardware re-test, log/memory refresh

### 016 — AVR Encoder Fixed-Rest FSM Finalization (2026-06-13)
Session 016 completed the main encoder stabilization. The final AVR encoder path exposes only `encode_stableRead4()` for rotation, removes legacy/selectable read modes, samples PC0/PC1/PC2 from `TIMER1_COMPA_vect` at 16 kHz, and emits detents only after a legal sequence leaves and returns to the fixed hardware rest phase `AB=11`. Temporary boot LCD diagnostics identified the true rest phase and were removed after the user confirmed the final behavior works well on hardware.
- **Find here**: final Timer1 16 kHz encoder FSM, fixed `ENCODER_REST_STATE = 0x03`, `encode_stableRead4()`-only API, removed debug hooks, hardware-approved encoder behavior, updated hardware archive

### 017 — Preset Rename Completion + UART Send Split Mapping (2026-06-14)
Session 017 completed the remaining Preset-owned rename sweep, removed the last Preset-specific `seq_` aliases, and confirmed that the remaining Sequencer lookup helpers are orchestration rather than missed ownership split points. The session also expanded the comms audit so the outbound front-panel transmit work is clearly mapped to the future `frontPanelSendingProtocol` split, and refreshed the temporary load/spec docs so the permanent reference material carries the useful planning notes.
- **Find here**: `PresetKitState` / `PresetAutomationTargets` rename, `PresetEndpointRestoreRequest`, temp-switch alias removal, `sequencer.h` compatibility cleanup, Phase 12 retrospective, direct front-panel send inventory, `frontPanelSendingProtocol` planning, comms/temp spec refresh

### 018 — UART Send/Receive Split Implementation (2026-06-14)
Session 018 implemented the current comms split pass: the front-panel send helpers were consolidated behind `frontPanelSendingProtocol`, the MIDI parser was divided into channel and global ownership files, and the transitional front-panel load/session bridge moved out of `frontPanelParser.c` into `PresetLoadCache`. The parser, sequencer, restore, and voice-control call sites were rewired to the new helpers, and the user re-tested the most important front-panel and MIDI paths with no regressions reported.
- **Find here**: `PresetLoadCache` bridge extraction, `frontPanelSendingProtocol` helper expansion, `ChannelMidiParser` and `GlobalMidiParser` split, `frontPanelParser` receive ownership, `MidiVoiceControl` LED delegation, restore/send call site rewiring, build verification, front-panel/MIDI smoke tests

### 019 — AVR Encoder Retune and Edit Acceleration (2026-06-14)
Session 019 re-examined the AVR main encoder after slow counter-clockwise clicks again missed decrements. The audit found no post-Session-016 AVR source regression, then tuned the Timer1 sampler to about 32.05 kHz with six stable phase samples, kept fixed `AB=11` rest anchoring, added a narrow recovery for filtered rest-to-opposite contact jumps, doubled button debounce enough to stop encoder-switch double clicks, and added config-driven edit-mode acceleration based on complete emitted detents. The final hardware test was reported good, with `ENC_ACCEL_MAX_MULT` set to 4 and endpoint clamp handling fixed so accelerated downward edits land cleanly on zero.
- **Find here**: current AVR encoder timing, six-sample phase filter, `AB=11` rest-FSM preservation, rest-jump recovery, 192-sample button debounce, edit-mode-only acceleration, acceleration config defines, endpoint clamp fix, hardware archive refresh

### 020 — Refactor Finalization: Cache Removal and Protocol Split Closeout (2026-06-14)
Session 020 reconciled the stale preset and MIDI/UART audits with the actual code, then finished the remaining architecture objectives. The recreated `PresetLoadCache.c/.h` from Session 018 was removed as obsolete rather than rehomed, active `presetLoad_*` cache APIs were removed, file-load receive paths now write directly to normal Preset/Pattern storage, and normal/temp Preset/Pattern switching remains the only staging model. STM receive code was renamed to `uARTFrontSYX/frontPanelReceivingProtocol.c/.h`, AVR protocol code was split into `frontPanelReceivingProtocol.c/.h` and `frontPanelSendingProtocol.c/.h`, and the old parser/protocol shim headers were removed after include/project redirection. STM, AVR, and aggregate firmware builds were verified, and the user hardware-tested Steps 1-3 successfully.
- **Find here**: `PresetLoadCache` removal, direct normal-storage file loads, STM `frontPanelReceivingProtocol` rename, AVR receive/send protocol split, legacy shim removal, deprecated PRF/cache traffic status, superseded audit references, build and hardware verification


---

## Key Cross-Session Facts (quick lookup)

| Topic | Canonical session |
|-------|------------------|
| Header/global-definition and inline-linkage compatibility fixes for modern GCC toolchains | 001 |
| Session memory baseline and directory map bootstrapped | 001 |
| Encoder work completed successfully | 002 |
| `.ALL` / `.PRF` load checkpoint and normal-only file-load routing for temp playback | 002 |
| Comms flow-control checkpoint uses load sessions, quiet mode, and credit-metered globals/voice/meta bursts; old callback waits still need timeouts | 002 |
| SEQ16 temp pattern selector/copy/play, symmetric STM parameter images, and endpoint/menu restore behavior | 002 |
| Morph computation is STM-owned for standard operation; AVR keeps global morph/menu/file endpoint responsibilities only | 003 |
| `SeqKitState` no longer has per-parameter valid arrays; parameter arrays are always-defined sound state from zero init | 003 |
| Endpoint storage uses raw AVR/menu parameter indices; low `+1` only applies when sending ordinary live low CC to STM MIDI/DSP apply | 003 |
| Temp/background `.ALL` / `.PRF` loading can proceed while playback uses temp; normal/temp switching is STM source selection plus AVR endpoint push-up | 004 |
| Raw automation selector bytes and resolved automation target structs must be kept coherent, especially for `FRONT_CC_LFO_TARGET` sidebands | 004 |
| LFO-to-voice-morph is serviced by phased morph drain, not immediate full voice-parameter interpolation from LFO callbacks | 004 |
| Velocity-to-voice-morph is a trigger-time one-shot value set, not a generic modulation-node destination | 004 |
| Future preset/morph refactor target is `mainboard/LxrStm32/src/Preset/`; SEQ16 temp keyhole remains intentionally bodged until after that refactor | 004 |
| Global morph menu sync on normal/temp switch is handled by display-only STM-to-AVR report traffic, not file-load routing | 005 |
| Background loading is unified into a single overwriteable STM-side session model; legacy voice-cache promotion is retired | 006 |
| UART transport (Uart.c) and protocol parsing (frontPanelParser.c) are strictly separated; parser is session-aware but transport-blind | 006 |
| Core sound-state (KitState), parameter mapping, and ingress authority are now owned by the new Preset module | 007 |
| AVR `din_inputData` must be synchronized with hardware in `din_init` to prevent phantom startup events | 008 |
| Morph engine and live-apply logic are now owned by the new Preset/MorphEngine module | 009 |
| Phase 3 background-load/session ownership now lives in Preset/PresetLoadCache; frontPanelParser consumes the cache API and the pending-counter reset helper is exported there | 010 |
| Pattern storage and generators now live in Sequencer/Pattern; sequencer.c is trimmed back to the scheduler/trigger role | 011 |
| Front-panel transport and parser code now live in uARTFrontSYX; opcode namespace is isolated in FrontPanelProtocol.h behind a compatibility include | 012 |
| Phase 6 cleanup/naming is complete; the next refactor step is driven by PRESET_CONSOLIDATION_AUDIT.md and the old planning docs are no longer canonical | 013 |
| Session 014 cut Sequencer/MIDI off the shared cache header, deleted the shared PresetLoadCache module, and left the remaining parser/session bridge inside frontPanelParser.c | 014 |
| AVR front-panel encoder uses raw `encode_stableRead4()` detents backed by Timer1 ~32.05 kHz sampling, six-sample phase filtering, fixed `AB=11` rest anchoring, narrow rest-jump recovery, and edit-mode-only acceleration; do not reintroduce legacy read modes, PCINT decoding, or Timer0 encoder sampling | 019 |
| Preset-owned exports/types now use `preset_` / `Preset*`, and the remaining AVR front-panel transmit calls are protocol work for `frontPanelSendingProtocol.c/.h` | 017 |
| Front-panel send helpers are now consolidated under `frontPanelSendingProtocol`, the MIDI parser is split across `ChannelMidiParser` and `GlobalMidiParser`, and the transitional receive load/session cache lives in `PresetLoadCache` | 018 |
| `PresetLoadCache.c/.h` and the active `presetLoad_*` cache API are obsolete and removed; file loads route directly to normal storage, while normal/temp Preset and Pattern switching remains the only staging model | 020 |
| STM and AVR front-panel protocol code now use matching receive/send boundaries: STM `uARTFrontSYX/frontPanelReceivingProtocol.c/.h` plus `frontPanelSendingProtocol.c/.h`, AVR `frontPanelReceivingProtocol.c/.h` plus `frontPanelSendingProtocol.c/.h`; old parser/protocol shim headers were removed after redirection | 020 |


---

## Append Template

```
| 0NN | YYYY-MM-DD | working repository status/path | One-line topic |
```

```
### 0NN — Title (YYYY-MM-DD)
One paragraph summary.
- **Find here**: comma-separated topics
```

Add any new cross-session facts to the Key Cross-Session Facts table.
