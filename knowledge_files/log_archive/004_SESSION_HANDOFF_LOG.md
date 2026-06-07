# Session 004 Handoff Log

DATE: 2026-06-07

SESSION GOAL: Complete background loading of `.ALL` and `.PRF` files without interrupting playback by making STM-side temp pattern/parameter storage usable after the Session 003 morph move.

COMPLETED:

- Made copy-to-temp conform to the STM-owned model:
  - copy-to-temp copies normal STM pattern and parameter state into STM temp storage;
  - switching into or out of temp selects the existing normal/temp source image;
  - switching does not copy parameter data.
- Restored audible normal/temp switching after the morph move.
- Separated normal/temp per-voice playback state well enough for hardware testing:
  - pattern source;
  - kit/front endpoint params;
  - morph endpoint params;
  - interpolation baseline;
  - automation target images;
  - global/per-voice morph state.
- Restored AVR/front-panel endpoint sync on normal/temp transitions.
- Removed the first-switch front-panel freeze after file load by neutralizing legacy voice hold/cache release behavior for temp-background loading.
- Fixed the post-load normal/temp switch LED/chaselight stall.
- Removed the retrigger-like audio glitch heard on file load, morph load, and normal/temp switching.
- Fixed global morph scaling so values above 127 are no longer ignored/collapsed incorrectly.
- Fixed dropped LFO target assignment to voice morph across normal/temp switches.
- Fixed velocity target to voice morph by making it a trigger-time one-shot morph value set.
- Fixed LFO target to voice morph audio breakup by moving LFO morph application into the phased morph drain.
- Wrote consolidation notes for the future STM-side `/Preset/` refactor and UART/comms simplification.

VERIFIED ON HARDWARE:

- Yes, by user report throughout the session.
- Confirmed:
  - normal/temp audio switching works;
  - per-track normal/temp switching works;
  - endpoint/menu sync works for most normal/temp transitions;
  - first-switch file-load freeze is gone;
  - retrigger-like glitch is gone;
  - LFO target assignments to voice morph survive after the raw/resolved target coherence fix;
  - velocity-to-morph no longer crashes after special casing;
  - phased LFO-to-morph drain survives and does not break up audio.

CHANGES THIS SESSION:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
  - Made temp kit capture a direct STM-side clone of normal state.
  - Added normal/temp-owned morph amount state to `SeqKitState` usage.
  - Adjusted source switching so voice source state is published before parameter/automation apply.
  - Removed dangerous broad shared-parameter apply from temp switch boundary.
  - Rebuilt live parameter apply families to avoid retrigger/envelope side effects.
  - Added coherent raw/resolved LFO destination storage:
    - inverse destination-to-selector resolver;
    - target-voice selector inference;
    - `seq_storeLfoDestinationIngress(...)` now updates raw endpoint arrays and resolved target structs together.
  - Added velocity-to-voice-morph one-shot trigger-time apply.
  - Added phased LFO-to-voice-morph morph drain.
  - Prevented velocity morph destinations from being installed into the generic velocity modulation matrix.

- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
  - Adjusted temp/file-load paths so legacy voice cache promotion does not block or corrupt temp-background operation.
  - Guarded direct live velocity target assignment and legacy cache release so `PAR_MORPH_*` velocity destinations are not installed into `velocityModulators[]`.
  - Preserved endpoint push-up/restore behavior for AVR menu sync.

- `mainboard/LxrStm32/src/DSPAudio/modulationNode.c`
  - Added recognition of LFO mod nodes.
  - Stopped LFO `PAR_MORPH_*` destinations from triggering immediate full voice-parameter morph application through `modNode_vMorph(...)`.
  - Left non-LFO VMORPH modulation behavior available where still needed.

- `front/LxrAvr/...`
  - Earlier session work touched AVR copy/temp/file paths so copy-to-temp no longer forces AVR endpoint dump staging.
  - AVR temp-named arrays remain file-read/menu scratch buffers, not temp playback storage.

- Documentation:
  - `MORPH_AUTOMATION_ASSIGN_DROPPED_BUG.md` captured the detailed bug hunt and will be superseded by `AUDIT_PRESET-MORPH_REFACTOR.md`.
  - `TEMP_SWITCH_WALK.md` captured the switch path and will be superseded by `AUDIT_PRESET-MORPH_REFACTOR.md`.
  - `AUDIT_TEMP_AUDIO_DROPOUT.md` captured the temp/retrigger investigations and will be superseded by `AUDIT_PRESET-MORPH_REFACTOR.md`.
  - `AUDIT_REFACTOR_TARGETS.md` was extended with non-preset refactor targets and protocol redundancy notes.
  - `knowledge_files/hardware_archive/ATMEGA_STM32F4_COMMS_AUDIT.md` was updated with Session 004 protocol knowledge.

KNOWN ISSUES INTRODUCED:

- SEQ16 remains intentionally bodged as the temp-pattern/keyhole button until after the future preset/morph refactor.
- Some current names are misleading and should be cleaned up later:
  - `frontPanelParams[]` really means kit/front endpoint params;
  - `morphParams[]` really means morph endpoint params;
  - AVR `parameter_values_temp[]` / `parameters2_temp[]` are file-read scratch buffers, not temp playback.
- `seq_applySharedParameterValues()` remains unused in the current build shape.
- Existing compiler warnings remain:
  - `midi_envPosition[]` loop bounds;
  - `seq_lastMasterStep` memset bounds;
  - existing `frontPanelParser.c` unused/fallthrough warnings;
  - linker RWX warning.
- Global morph value still does not reliably push up to AVR menu on normal/temp switch; user flagged this for Session 005.

KNOWN ISSUES RESOLVED:

- Complete audio dropout on switch to temp after Session 003 morph move.
- Audio staying silent after switching back to normal.
- First normal/temp switch after `.ALL` / morph load causing several-second front-panel freeze.
- Front-panel LEDs/chaselight stuck after load/copy/temp/normal sequence.
- Retrigger-like audio glitch on file load, morph load, and normal/temp switch.
- LFO target to voice morph assignment dropped on normal/temp switch.
- LFO target assignment appearing correct in menu but wrong in STM audio state.
- Velocity target to voice morph crashing STM.
- LFO target to voice morph doing too much work per LFO update and breaking up audio.
- Global morph scaling limited to 0-127.

NEXT SESSION RECOMMENDED GOAL:

- Session 005 should handle minor issues before the large refactor:
  - encoder phase imbalance;
  - global morph push-up to AVR menu on normal/temp switch;
  - automate the temp pattern switch/background-load process;
  - add global parameter switches for background loading;
  - any additional small hardware findings.

BLOCKERS:

- Hardware retesting after the final documentation/code-comment pass.
- Decision on exactly when to start the `/Preset/` refactor: probably Session 006 or 007.
- Keep SEQ16 temp keyhole in place until after the refactor so the current observation/control path remains available.

CRITICAL REMINDERS FOR NEXT SESSION:

- Never run git commands.
- Do not remove the SEQ16 temp-pattern keyhole yet.
- Do not reintroduce AVR as canonical temp playback owner.
- Do not reintroduce per-parameter valid arrays in `SeqKitState`.
- Copy-to-temp is an STM-side clone; switch-to-temp is source selection.
- Endpoint push-up to AVR is still required at every audible normal/temp boundary.
- Raw selector bytes and resolved automation target structs must remain coherent.
- LFO-to-voice-morph belongs in the phased morph drain; velocity-to-voice-morph is one-shot at trigger time.
- The future refactor should move STM-side preset/morph/automation storage and APIs into `mainboard/LxrStm32/src/Preset/`.
