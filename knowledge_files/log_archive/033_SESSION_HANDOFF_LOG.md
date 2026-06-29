# 033 Session Handoff Log - Kit Reload Restore And Euclid Temp-Track Rollback

DATE: 2026-06-29

## Session Goal

This session started with one concrete firmware goal: re-implement kit reload (`SHIFT+PLAY`) around the STM temporary preset storage so it no longer fights `.prf` / `.all` background loading. The user also wanted one file-level safety guard: `PAR_VOICE_DECIMATION_ALL` must never load or boot as `0`; any imported `0` should be treated as `127`, and startup should always seed the menu copy to `127`.

The second half of the session shifted several times. The original request was to extend pattern realign behavior, but that work went through multiple rejected attempts. The user eventually hard-reset the repository, kept only the planning document, and replaced the goal with a new `SHIFT+PERF` Euclid-page workflow:

- on the Euclid/rotation page, the first edit to any touched track should snapshot that track into temporary pattern storage before the edit;
- subsequent edits to that same track during the same visit should not snapshot again;
- leaving the page commits;
- pressing `SHIFT+PERF` again without leaving should restore every touched track from temp back into normal and reset the displayed rotation values for those restored tracks.

This handoff records both the committed kit-reload/decimation work and the current uncommitted Euclid rollback WIP that remained in the local tree at session end.

## Completed

### 1. `SHIFT+PLAY` / `PATCH_RESET` Now Reloads From STM Temporary Preset Storage

Committed in `43f5006`.

Durable behavior now implemented:

- ordinary load-menu `.snd` kit loads mirror kit/front endpoints into both STM normal and STM temporary preset storage;
- ordinary load-menu individual instrument loads mirror only kit/front endpoints into both normal and temp;
- ordinary morph loads mirror only morph endpoints into both normal and temp;
- `.prf` and `.all` loads without background loading mirror both kit/front and morph endpoints into both normal and temp;
- protected `.prf` / `.all` background loads still preserve the old audible temp preset image during the load itself;
- once playback later returns to normal after a manual pattern change, STM immediately resnapshots temp from the newly loaded normal preset so temp once again matches the last loaded preset image;
- `PATCH_RESET` now means “copy STM temporary preset endpoints back into STM normal preset storage and reapply the live normal image”;
- `SHIFT+PLAY` is disabled while protected `.prf` / `.all` temp preset playback is still active.

Important architectural decision that survived the session:

- the old AVR `parameter_values_fileLoadSnapshot[]` / `parameters2_fileLoadSnapshot[]` arrays were not removed;
- they still act as AVR file-read staging buffers for legacy import paths;
- they are no longer the authoritative reload source for `SHIFT+PLAY`;
- STM temporary preset storage is now the authoritative reload source.

Key implementation points:

- `mainboard/LxrStm32/src/Preset/KitState.c/.h`
  - added `preset_copyKitEndpoints(...)` so callers can mirror kit/front only, morph only, or both endpoint groups without open-coding `PresetKitState` layout copies.
- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c/.h`
  - added explicit temp-preset-playback state and the reload/resnapshot helpers:
    - `preset_isTempPresetPlaybackActive()`
    - `preset_setTempPresetPlaybackActive()`
    - `preset_reloadNormalFromTemporaryPreset()`
    - `preset_resnapshotTemporaryPresetFromNormal()`
    - `preset_reapplyCurrentPlaybackSources()`
- `mainboard/LxrStm32/src/Preset/ParameterIngress.c/.h`
  - added the small endpoint-ingress suppression guard used while endpoint subsets are being copied between preset images.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
  - updated ordinary file-load ingress so non-background parameter-bearing loads mirror the loaded endpoint subset into temp immediately;
  - background `.prf` / `.all` loads preserve temp during the load, then arm a deferred “resnapshot temp from normal” step for when playback returns to normal;
  - inbound `PATCH_RESET` now calls `preset_reloadNormalFromTemporaryPreset()` and ignores the request if temp preset playback is still active.
- `front/LxrAvr/Menu/menu.c`
  - `menu_reloadKit()` now only sends `PATCH_RESET`;
  - it no longer restores `parameter_values_fileLoadSnapshot[]` into live menu state locally;
  - it returns early if AVR knows protected temp preset playback is still active.
- `front/LxrAvr/Preset/presetManager.c/.h`
  - added the narrow AVR-side flag for “temp preset playback active” so the UI can disable reload only for `.prf` / `.all` protected temp playback, not for pattern-only `.pat` temp playback;
  - retained the file-load snapshot arrays as file-read staging, but removed them from reload semantics.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c/.h`
  - inbound `PATCH_RESET` on AVR is now inert historical compatibility only; AVR no longer treats it as “restore my local snapshots.”

### 2. `PAR_VOICE_DECIMATION_ALL` Is Now Forced Safe At The File Boundary And At Startup

Committed in `43f5006`.

Durable behavior now implemented:

- any imported kit/morph/performance/all payload that contains `PAR_VOICE_DECIMATION_ALL == 0` is rewritten to `127` immediately at the AVR file-import boundary;
- this means broken on-disk `0` never propagates into live menu state, STM endpoint dumps, temp snapshots, or later re-saves;
- AVR startup now seeds `parameter_values[PAR_VOICE_DECIMATION_ALL] = 127` immediately after the zero-fill in `menu_init()`, so no-SD or failed-load boot paths also start from the safe value.

Files changed:

- `front/LxrAvr/Preset/presetManager.c`
  - added `preset_normalizeImportedKitParams()` and applied the `0 -> 127` clamp at the import boundary.
- `front/LxrAvr/Menu/menu.c`
  - seeds the startup menu copy to `127`.

The user re-tested this behavior and reported it was working.

### 3. The First Pattern-Realign Approach Was Discarded

The middle of the session involved several attempts to make “pattern realign” physically rotate Euclid tracks back into place and then zero the rotation values. Those attempts were repeatedly rejected in hardware testing:

- some versions only corrected sub-step rotation;
- some versions left certain tracks one main step late;
- one version only zeroed the displayed rotation numbers without actually restoring pattern data;
- another version followed the wrong UI path by confusing “hold `SHIFT` while already on PERF page” with the real Euclid entry path “hold `SHIFT`, then press `PERF`.”

The user then hard-reset the repository, explicitly keeping only `PAT_REALIGN_AUDIT.md`, and replaced the feature request with a new Euclid-page temp-backup workflow.

Important consequence:

- none of the earlier in-place rotation-reset experiments should be considered canonical code;
- the durable local code state after the reset is only the later temp-backup/rollback implementation described below.

### 4. Current Local WIP: `SHIFT+PERF` Euclid Edits Use One-Visit Temp Track Backups

Uncommitted local work still present at session end.

Durable intent of the surviving implementation:

- the real scope is `SELECT_MODE_PAT_GEN` / `EUKLID_PAGE`, entered by holding `SHIFT` and pressing `PERF`;
- on first edit to a given track during one visit, STM copies that normal track into `SEQ_TMP_PATTERN` before the edit;
- subsequent edits to that same track during the same page visit do not copy again;
- leaving the page commits by clearing the per-visit bookkeeping without restoring;
- pressing `SHIFT+PERF` again while already in Euclid mode restores every touched track from temp back into the normal pattern and restores the cached Euclid UI values for those tracks.

Final surviving code path:

- `front/LxrAvr/buttonHandler.c`
  - keys rollback/commit detection off `wasPatgenMode` instead of `menu_getActivePage()`;
  - if PAT_GEN was already active and the user presses `PERF` again with `SHIFT` held, AVR now sends `SEQ_EUKLID_RESET_RESTORE_VISIT`;
  - if PAT_GEN was active and the user leaves that mode, AVR sends `SEQ_EUKLID_RESET_END_VISIT`.
- `front/LxrAvr/Menu/menu.c`
  - `menu_enterPatgenMode()` now sends `SEQ_EUKLID_RESET_BEGIN_VISIT` only on real entry into `EUKLID_PAGE`;
  - it does not re-send `BEGIN_VISIT` on in-page track changes;
  - this guard fixed the bug where rollback only restored the most recently edited track because each track change cleared the STM touched-track mask.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
  - `SEQ_EUKLID_RESET` (`0x47`) was kept and given explicit `data2` meanings:
    - `0x01` legacy clear rotation/cache state for file/preset operations
    - `0x02` begin Euclid visit
    - `0x03` end/commit Euclid visit
    - `0x04` restore current Euclid visit
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c/.h`
  - added visit state:
    - `frontParser_euklidSnapshotVisitActive`
    - `frontParser_euklidSnapshotTrackMask`
    - `frontParser_euklidSnapshotPattern`
    - per-track cached `steps`, `rotation`, and `sub-step rotation` arrays;
  - added helpers:
    - `frontParser_beginEuklidSnapshotVisit()`
    - `frontParser_endEuklidSnapshotVisit()`
    - `frontParser_snapshotEuklidTrackIfNeeded()`
    - `frontParser_restoreEuklidSnapshotTracks()`
  - the four Euclid mutators now all call the shared snapshot helper before editing:
    - `FRONT_SEQ_EUKLID_LENGTH`
    - `FRONT_SEQ_EUKLID_STEPS`
    - `FRONT_SEQ_EUKLID_ROTATION`
    - `FRONT_SEQ_EUKLID_SUBSTEP_ROTATION`
  - restore copies every touched track from `SEQ_TMP_PATTERN` back into the original normal pattern, then refreshes the active-track LEDs and Euclid reply.
- `mainboard/LxrStm32/src/Sequencer/Pattern/EuklidGenerator.c/.h`
  - added `euklid_restoreTrackState(...)` to restore the cached Euclid control/UI values after temp track data has already been copied back;
  - this helper intentionally does not rotate the pattern data again.

Why the final fix needed the `menu_enterPatgenMode()` guard:

- the first WIP implementation began the visit every time `menu_enterPatgenMode()` ran;
- while already on the Euclid page, selecting a different track also calls `menu_enterPatgenMode()`;
- that meant every track change cleared the STM touched-track mask;
- rollback then only knew about the most recently edited track;
- the final local fix sends `BEGIN_VISIT` only when `menu_activePage != EUKLID_PAGE`.

## Verification

### Committed Kit-Reload / Decimation Work

- `make -C mainboard/LxrStm32 -j4 stm32` passed during implementation.
- `make -C front/LxrAvr avr -j4` passed during implementation.
- The user re-tested the decimation fix and accepted it.
- The user stated the repository was committed and pushed after the kit-reload / decimation pass.

### Current Local Euclid Rollback WIP

- `make -C mainboard/LxrStm32 -j4 stm32` passed after the Euclid-page work.
- `make -C front/LxrAvr avr -j4` passed after the Euclid-page work.
- The final local multi-track rollback fix has not been explicitly hardware-confirmed in this final state inside the conversation.

## Repository State At Session End

Current branch:

- `dev-realign-reload`

Current durable commit already in the branch:

- `43f5006` — `put kit-reload (shift+play function) back in. also, init with decimate_all at 127 and ignore if this is 0 in files`

Current local uncommitted WIP at session end:

- `front/LxrAvr/Menu/menu.c`
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
- `front/LxrAvr/buttonHandler.c`
- `mainboard/LxrStm32/src/Sequencer/Pattern/EuklidGenerator.c`
- `mainboard/LxrStm32/src/Sequencer/Pattern/EuklidGenerator.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`
- `PAT_REALIGN_AUDIT.md`
- `firmware image/FIRMWARE.BIN`

## End Of Session Block

COMPLETED: Restored `SHIFT+PLAY` around STM temporary preset storage, made `PATCH_RESET` an STM-owned temp-to-normal reload, added the `PAR_VOICE_DECIMATION_ALL` `0 -> 127` import clamp plus `127` startup default, documented the discarded pattern-realign attempts, and left a current local WIP where `SHIFT+PERF` Euclid edits snapshot touched tracks into temp once per visit and restore them all on re-press.
VERIFIED ON HARDWARE: partial. The committed kit-reload/decimation pass was user-retested and accepted. The final local Euclid multi-track rollback WIP was build-verified but still needs fresh hardware confirmation in this exact state.

CHANGES THIS SESSION:
- `mainboard/LxrStm32/src/Preset/KitState.c/.h`: added endpoint-subset copy helpers for normal/temp preset mirroring and reload.
- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c/.h`: added temp-preset-playback state plus `PATCH_RESET` reload/resnapshot helpers.
- `mainboard/LxrStm32/src/Preset/ParameterIngress.c/.h`: added endpoint-ingress suppression used during preset image copies.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`: routed file loads into the new mirror/resnapshot behavior, made inbound `PATCH_RESET` STM-owned, and added the Euclid visit/snapshot/restore WIP.
- `front/LxrAvr/Menu/menu.c`: changed `menu_reloadKit()` to send `PATCH_RESET` only, seeded global decimation to `127`, and added the Euclid visit-entry guard.
- `front/LxrAvr/Preset/presetManager.c/.h`: kept AVR snapshot arrays as file-read staging only, added temp-preset-playback tracking, and clamped imported decimation `0` to `127`.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c/.h`: retired AVR-side snapshot restore semantics for inbound `PATCH_RESET` and defined Euclid visit-control constants on `SEQ_EUKLID_RESET`.
- `front/LxrAvr/buttonHandler.c`: added the `wasPatgenMode`-based Euclid rollback/commit signaling.
- `mainboard/LxrStm32/src/Sequencer/Pattern/EuklidGenerator.c/.h`: added `euklid_restoreTrackState()` for rollback without re-rotating pattern data.
- `KIT_RELOAD_AUDIT.md`: written and carried through the committed temp-preset reload implementation.
- `PAT_REALIGN_AUDIT.md`: retained as the durable narrative/planning doc through the reset and final Euclid-page WIP.

KNOWN ISSUES INTRODUCED: No confirmed new regression in the committed kit-reload/decimation path. The final local Euclid-page rollback WIP still needs hardware confirmation.
KNOWN ISSUES RESOLVED: `SHIFT+PLAY` no longer depends on AVR file-load snapshots, protected `.prf` / `.all` background loads keep temp reload semantics coherent, and broken file/startup `PAR_VOICE_DECIMATION_ALL == 0` no longer propagates.

NEXT SESSION RECOMMENDED GOAL: Hardware-test the current Euclid-page multi-track rollback WIP, then archive the final behavior into the session index/comms docs if it is accepted.
BLOCKERS: Hardware confirmation of the final local `SHIFT+PERF` multi-track restore behavior.

CRITICAL REMINDERS FOR NEXT SESSION:
- The real Euclid path is “hold `SHIFT`, then press `PERF`” -> `SELECT_MODE_PAT_GEN` -> `EUKLID_PAGE`; do not confuse this with holding `SHIFT` while already on the PERF page.
- `menu_enterPatgenMode()` also runs for in-page track changes; `SEQ_EUKLID_RESET_BEGIN_VISIT` must only be sent on real page entry or the touched-track mask will be cleared.
- Rollback/commit detection for the Euclid page must key off whether PAT_GEN mode was already active (`wasPatgenMode`), not only the instantaneous active page.
- AVR `parameter_values_fileLoadSnapshot[]` / `parameters2_fileLoadSnapshot[]` still exist as file-read staging buffers; only their old reload semantics were retired.
