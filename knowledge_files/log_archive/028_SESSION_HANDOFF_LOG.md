# Session 028 Handoff Log - Background File Loading Via Temporary Data

DATE: 2026-06-21

SESSION GOAL: Finish background loading for `.pat`, `.prf`, and `.all` by using the existing normal/temporary Pattern and Preset storage, preserving uninterrupted playback while file bytes load into normal storage, and retire the temporary audit notes into durable logs/specs.

COMPLETED: Background loading via temporary data is feature-complete. The final model uses a front-panel background-swap handshake before ordinary file load, STM-side normal-to-temp copy/switch, pattern-only temp mode for `.pat`, morph-preservation fixes, second-load guards while temp playback remains active, and AVR LED/UI hints while playback is from temporary data.

VERIFIED ON HARDWARE: Yes. User tested staged behavior throughout the session and confirmed the final feature works. Specific confirmed points included background `.pat` pattern-only separation, morph preservation, repeated loads while temp playback is active, and the final temp-playback LED hint behavior.

## Changes This Session

- `front/LxrAvr/Menu/*`, `front/LxrAvr/Parameters.h`, `front/LxrAvr/Text.h` / menu text tables from the planning chain:
  - The load-page setting is the canonical 5-state background-load selector:
    - `0 = off`
    - `1 = pat`
    - `2 = prf`
    - `3 = all`
    - `4 = tot`
  - AVR names are `PAR_FILE_LOAD_BACKGROUND`, `TEXT_FILE_LOAD_BACKGROUND`, `MENU_FILE_LOAD_BACKGROUND`, `backgroundLoadNames`, and `SEQ_LOAD_BACKGROUND`.
  - The raw persisted byte and the old `0x50` setting opcode value were kept compatible. Important: this `SEQ_LOAD_BACKGROUND` / STM `FRONT_SEQ_LOAD_FAST` path is legacy setting-byte traffic and is not the new active background-swap mechanism.

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
  - Added the active background-swap opcodes:
    - `SEQ_BACKGROUND_SWAP_BEGIN = 0x6d`
    - `SEQ_BACKGROUND_SWAP_DONE = 0x6e`
  - These are distinct from the old `SEQ_LOAD_BACKGROUND` setting byte.

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`
  - Added the matching STM opcode names:
    - `FRONT_SEQ_BACKGROUND_SWAP_BEGIN = 0x6d`
    - `FRONT_SEQ_BACKGROUND_SWAP_DONE = 0x6e`
  - Exposes `frontParser_serviceBackgroundSwapAck()`.

- `front/LxrAvr/Preset/presetManager.c`
  - Added background-load mode aliases:
    - `BACKGROUND_OFF`
    - `BACKGROUND_PAT`
    - `BACKGROUND_PRF`
    - `BACKGROUND_ALL`
    - `BACKGROUND_TOT`
  - Added `preset_backgroundSwapNeeded(fileType)`.
    - Requires sequencer running.
    - Requires selected background mode to match `WTYPE_PATTERN`, `WTYPE_PERFORMANCE`, or `WTYPE_ALL`.
    - Skips background swap if AVR already knows temp playback is active.
    - Skips background swap if `menu_playedPattern == SEQ_TMP_PATTERN`.
  - Added the AVR wait state:
    - `preset_backgroundSwapDone`
    - `preset_backgroundSwapExpectedType`
    - `preset_backgroundTempPlaybackActive`
  - Added `preset_performBackgroundSwapWait(fileType)`.
    - Shows `Bckgrnd Swap...`.
    - Sends `SEQ_BACKGROUND_SWAP_BEGIN` with file type.
    - Polls `uart_checkAndParse()` until the matching `SEQ_BACKGROUND_SWAP_DONE` arrives or a timeout occurs.
    - Timeout returns to the ordinary load path rather than permanently locking the AVR.
  - `.pat`, `.prf`, and `.all` load paths call the wait helper before sending their ordinary `SEQ_FILE_BEGIN` when `preset_backgroundSwapNeeded()` is true.
  - The standard file loading screen still appears for the actual file type regardless of whether the background-swap prelude is used.
  - Added `preset_backgroundSwapDoneFromStm(fileType)`, now used by the AVR receive parser.
  - Added `preset_notePlayedPatternChanged(playedPattern)`.
    - Clears `preset_backgroundTempPlaybackActive` when STM reports a non-temp played pattern.
  - Added `preset_isBackgroundTempPlaybackActive()` so UI code can query the private temp-playback flag.
  - Added `preset_shouldPreserveMenuEndpointsDuringFileLoad()`, `preset_saveMenuEndpointsDuringFileLoad()`, and `preset_restoreMenuEndpointsDuringFileLoad()` so AVR menu endpoint arrays can be preserved when normal storage is overwritten while playback/menu state is on temp.
  - Changed file-backed kit/meta loops to use `END_OF_KIT_PARAMETERS` where the file-backed kit endpoint range is intended, so per-voice morph display controls are not treated as saved kit bytes.
  - Changed `preset_dumpEndpointsToStm()` endpoint dump loops to `END_OF_KIT_PARAMETERS` for the same reason.

- `front/LxrAvr/Preset/PresetManager.h`
  - Added prototypes for:
    - `preset_backgroundSwapDoneFromStm(uint8_t fileType)`
    - `preset_notePlayedPatternChanged(uint8_t playedPattern)`
    - `preset_isBackgroundTempPlaybackActive(void)`

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`
  - Allows `SEQ_BACKGROUND_SWAP_DONE` through while `avrCommsParser_rxDisable` is active, because file loads deliberately protect receive parsing while the AVR waits.
  - Dispatches `SEQ_BACKGROUND_SWAP_DONE` to `preset_backgroundSwapDoneFromStm()`.
  - Calls `buttonHandler_refreshTempPlaybackLedHint()` after swap done and after `SEQ_CHANGE_PAT` LED refresh.
  - Calls `preset_notePlayedPatternChanged(patMsg)` after STM pattern-change ack updates `menu_playedPattern`.
  - Keeps `menu_repaintAll()` suppressed in the relevant protected restore path so the AVR does not abort/repaint out of the file-load continuation.
  - Guards `PRF_RESTORE_PARAM_CC` / `PRF_RESTORE_PARAM_CC2` during `rxDisable` so protected file-load restore traffic cannot overwrite `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` in AVR menu storage.

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
  - Added background-swap pending state:
    - `frontParser_backgroundSwapPending`
    - `frontParser_backgroundSwapFileType`
    - `frontParser_backgroundSwapAckDelayActive`
    - `frontParser_backgroundSwapStartTick`
  - `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` now performs the STM-side preparation:
    - `pat_copyToTmpPattern(seq_activePattern)`
    - `seq_setNextPattern(SEQ_TMP_PATTERN, 0x0f)`
    - `preset_tempPlaybackSwitchState.forceInstantSwitch = 1`
    - `preset_tempPlaybackSwitchState.patternOnlyTempPlayback = (fileType == FRONT_FILE_DONE_TYPE_PATTERN)`
    - arms the delayed ACK state.
  - `frontParser_serviceBackgroundSwapAck()` now waits until playback has actually reached temp before sending the ACK:
    - `seq_activePattern == SEQ_TMP_PATTERN`
    - every `seq_perTrackActivePattern[track] == SEQ_TMP_PATTERN`
    - for `.pat`, all preset voice sources still report normal through `preset_allVoiceSourcesUseNormal()`
    - for `.prf` / `.all`, all preset voice sources report temp through `preset_allVoiceSourcesUseTmp()`
  - After readiness, it waits `FRONT_BACKGROUND_SWAP_ACK_DELAY_TICKS = 400U` and sends `FRONT_SEQ_BACKGROUND_SWAP_DONE` using `frontPanelSending_sendPriorityTriplet()`.
  - The ACK is serviced from `mainboard/LxrStm32/src/main.c` each main loop pass, so STM audio/MIDI/front-panel processing continues during the AVR wait.
  - `FRONT_SEQ_FILE_BEGIN` no longer resets current global/per-voice morph amounts. For `.prf` and `.all` it keeps the parameter refresh signal by invalidating the live morph apply cache and disabling morph endpoint load application while endpoint bytes are imported. `.pat` gets no morph side effect.

- `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.c`
  - `pat_copyToTmpPattern(srcPattern)` is the background-swap copy primitive.
  - If sequencer is stopped, it copies the selected source pattern to `SEQ_TMP_PATTERN`.
  - If sequencer is running, it copies each track from `seq_perTrackActivePattern[track]`, preserving individual track-pattern playback.
  - It copies current pattern settings, forces temp hold settings, and calls `preset_captureTmpKitState()` so the matching normal kit image is captured into temp.

- `mainboard/LxrStm32/src/Preset/KitState.c`
  - `preset_captureTmpKitState()` copies normal kit endpoint params, morph endpoint params, interpolated params, automation target images, global morph amount, and per-voice morph base/current amounts into `preset_tmpKitState`, then marks temp valid.

- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.h/.c`
  - `PresetTempPlaybackSwitchState` now includes:
    - `forceInstantSwitch`
    - `patternOnlyTempPlayback`
  - `preset_setTempPlaybackActive(active)` owns normal/temp preset source selection and endpoint/global-morph report push-up when preset source changes.
  - `preset_allVoiceSourcesUseTmp()` and `preset_allVoiceSourcesUseNormal()` support background-swap ACK readiness.

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
  - Pattern switch code honors `forceInstantSwitch`, so background swap moves to `SEQ_TMP_PATTERN` immediately after copy rather than waiting for bar boundary or the global instant-switch option.
  - It does not add special sequencer-position mutations. The normal instant pattern switch path preserves current sequencer position.
  - For `.pat` background loads, `patternOnlyTempPlayback` prevents `preset_setTempPlaybackActive()` and `preset_updateVoiceSourcesForPatternChange()`, so pattern playback moves to temp while parameters remain normal.
  - Clears `forceInstantSwitch` after the switch executes.
  - Raises the normal temp-boundary ack flag when the normal/temp pattern boundary changes.

- `mainboard/LxrStm32/src/Preset/MorphEngine.c`
  - `preset_setGlobalMorphAmount()` now mirrors user global morph edits into normal storage if the current editable/audible kit is temp.
  - `preset_setVoiceMorphAmount()` now mirrors user per-voice morph edits into normal storage if that voice's current morph kit is temp.
  - Automation/velocity/LFO morph paths were not broadened into normal storage mirroring; the mirror applies to intentional user global/per-voice morph amount edits.

- `front/LxrAvr/Parameters.h` and `mainboard/LxrStm32/src/Preset/ParameterArray.h`
  - Introduced/used `END_OF_KIT_PARAMETERS` before `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT`.
  - `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` remain before `END_OF_SOUND_PARAMETERS` so automation/modulation parameter domains can still see them, but file-backed kit reads/writes/dumps use `END_OF_KIT_PARAMETERS`.

- `front/LxrAvr/buttonHandler.c/.h`
  - Removed the deprecated load-page voice-button fast-load / background PRF cache shortcut. Background loading is now only the explicit normal/temp swap mechanism.
  - Added `buttonHandler_refreshTempPlaybackLedHint()`.
  - Added `buttonHandler_tempPlaybackPerfBlink` so the temp hint only clears `LED_MODE2` when the temp hint owns that blink.
  - While temp playback is active:
    - non-PERF modes flash `LED_MODE2` as a PERF/temp-playback hint while preserving the current mode LED;
    - PERF mode flashes all eight SELECT LEDs;
    - `SHIFT+PERF` is ignored so PAT_GEN is not entered while temp playback remains active.

- `front/LxrAvr/ledHandler.c`
  - Increased `NUM_OF_BLINKABLE_LEDS` from `6` to `8` so all SELECT LEDs can blink simultaneously in PERF mode.

- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
  - Updated in this closeout to describe completed `0x6d/0x6e` background-swap handshake, ACK pass-through during `rxDisable`, and legacy `0x50` status.

- `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`
  - Updated in this closeout to describe current background-load storage/switch semantics, `.pat` pattern-only behavior, morph preservation, and temp-playback UI state.

- `MEMORY.md`
  - Updated in this closeout so Session 028 is the current background-load reference and stale “future automate temp-pattern switch/background-load” wording is removed.

## Debugging / Decision History Preserved From Temporary Audits

### Initial menu and ACK plan

`TEMP_LOAD_MENU_AUDIT.md` established the five-value background-load selector and compatibility rule: the same stored byte persisted in `.cfg` / globals storage, but with background terminology and menu text. It also deliberately deferred runtime behavior to later passes.

`TEMP_LOAD_ACK_AUDIT.md` established the new active handshake:

```text
AVR -> STM: SEQ_CC / SEQ_BACKGROUND_SWAP_BEGIN / fileType
STM -> AVR: FRONT_SEQ_CC / FRONT_SEQ_BACKGROUND_SWAP_DONE / fileType
```

with opcode values `0x6d` and `0x6e`. The first implementation used a dummy STM delay, then later passes replaced the dummy delay with real normal-to-temp copy, instant switch, readiness check, and final delayed ACK.

### Copy-to-temp and instant switch

`FILE_LOAD_SWAP_PART2_AUDIT.md` established that background swap should only run while the sequencer is playing. If stopped, ordinary loads run without background-swap opcodes. The STM copy uses existing copy-to-temp primitives rather than emulating button presses or duplicating copy logic.

`BG_LOAD_SWITCH_AUDIT.md` established that after the copy is complete, STM must immediately switch playback to temp using the existing instant switch path, preserving current sequencer positions. No extra position rewrites should be introduced.

The main early-exit debugging lesson from that audit:

- The observed first-attempt `.ALL` failure was not an STM copy failure.
- The first failed load had already copied/switch to temp.
- A repeat load worked because the first attempt left playback on temp, causing AVR to skip the background-swap prelude on the second try.
- `menu_repaintAll()` inside AVR receive processing could abort/repaint the file-load continuation before the normal `Loading All` repaint, and suppressing that protected repaint let the file load proceed.
- The deprecated load-page `preset_loadPerf()` voice-button shortcut was removed because it belonged to old load-fast / background PRF cache behavior, not the new normal/temp mechanism.

### Morph and file-load preservation

`FILE_LOAD_MORPH_CHANGE_BUG_AUDIT.md` diagnosed the `.pat` audible morph change:

```text
AVR .PAT load
-> SEQ_FILE_BEGIN / WTYPE_PATTERN
-> STM FRONT_SEQ_FILE_BEGIN
-> old unconditional seq_resetVoiceMorphAmountsToGlobal()
-> every individual voice morph overwritten with global morph
```

The fix was to remove file-load morph amount mutation. `.pat` gets no morph side effect. `.prf` and `.all` rely on normal endpoint writes plus live morph apply-cache invalidation so loaded parameters are recomputed from the existing current morph amounts.

`AUDIT_PER_VOICE_MORPH_PAT_BG_TWEAKS.md` captured three later fixes:

- user global/per-voice morph amount edits while temp is active mirror into normal storage;
- `.pat` background loading separates pattern temp playback from parameter temp playback;
- AVR per-voice morph display values survive `.all` / `.prf` file loads by using the new `END_OF_KIT_PARAMETERS` file-backed boundary and the AVR `rxDisable` restore guard.

Important retained rule:

- Morph amount controls are not saved file parameters and should not be overwritten by file loads.
- `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` must stay inside `END_OF_SOUND_PARAMETERS` for automation/modulation domains, but file-backed kit loops should use `END_OF_KIT_PARAMETERS`.

### Repeated loads while already on temp

`AUDIT_BG_LOAD_TWEAKS2.md` established the AVR-side `preset_backgroundTempPlaybackActive` rule:

- First `.pat` / `.prf` / `.all` load while playing normal can do background swap.
- Subsequent `.pat` / `.prf` / `.all` load while playback remains on temp must skip the background-swap copy/switch and proceed like background load is off, writing normal storage while temp stays audible.
- The flag clears only when STM reports a non-`SEQ_TMP_PATTERN` played pattern.

### LED/UI hint

`AUDIT_BG_LOAD_LED_HINT.md` established the final temp-playback UI behavior:

- Outside PERF mode, flash PERF LED (`LED_MODE2`) while temp playback is active.
- In PERF mode, flash all eight SELECT LEDs.
- Disable `SHIFT+PERF` while temp playback is active.
- Preserve existing shifted-mode blink behavior by tracking whether the temp hint owns the PERF blink.

## Current Background Load Runtime Flow

### Sequencer stopped

```text
User loads .pat/.prf/.all
-> preset_backgroundSwapNeeded() returns 0
-> no SEQ_BACKGROUND_SWAP_BEGIN
-> no Bckgrnd Swap prelude
-> ordinary loading screen and ordinary file load run
```

### First background-capable load while playing normal

```text
AVR validates file header
-> preset_backgroundSwapNeeded() matches PAR_FILE_LOAD_BACKGROUND and file type
-> AVR shows Bckgrnd Swap...
-> AVR sends SEQ_BACKGROUND_SWAP_BEGIN + fileType
-> STM pat_copyToTmpPattern(seq_activePattern)
   - running: copies currently audible per-track pattern sources
   - also captures normal kit into temp through preset_captureTmpKitState()
-> STM sets pending pattern to SEQ_TMP_PATTERN and forceInstantSwitch
-> Sequencer immediately switches to temp using existing instant switch path
-> STM waits until temp playback readiness is true
-> STM waits 400 ticks
-> STM sends SEQ_BACKGROUND_SWAP_DONE + fileType as priority traffic
-> AVR receives done despite rxDisable
-> AVR marks temp playback active and continues ordinary file load
-> file payload writes normal storage
```

### `.pat` background load

```text
FRONT_SEQ_BACKGROUND_SWAP_BEGIN / WTYPE_PATTERN
-> pattern temp copy and SEQ_TMP_PATTERN playback are used
-> preset_tempPlaybackSwitchState.patternOnlyTempPlayback = 1
-> sequencer does not call preset_setTempPlaybackActive()
-> sequencer does not update voice source state to temp
-> readiness checks all preset voice sources remain normal
-> .pat payload writes normal pattern storage
-> audible parameters remain normal throughout
```

### Second load while still playing temp

```text
AVR preset_backgroundTempPlaybackActive == 1
-> preset_backgroundSwapNeeded() returns 0 for .pat/.prf/.all
-> no copy-to-temp, no instant switch, no Bckgrnd Swap prelude
-> ordinary file load writes normal storage while temp remains audible
```

### Leaving temp playback

```text
STM sends SEQ_CHANGE_PAT with a normal pattern
-> AVR updates menu_playedPattern
-> preset_notePlayedPatternChanged() clears preset_backgroundTempPlaybackActive
-> future matching file loads may use background swap again
```

## Build Verification

Builds were run throughout the session. Final verified commands after the LED hint pass:

```sh
make -C front/LxrAvr -j4 avr
make firmware
```

Both passed and rebuilt `front/LxrAvr/LxrAvr.bin` / `firmware image/FIRMWARE.BIN`. Earlier staged passes also verified STM builds with `make -C mainboard/LxrStm32 -j4 stm32`. Warnings observed were the existing AVR warning set: SD/port array-bounds noise, known fallthrough warnings, and unused variables/functions in legacy/commented regions.

## Known Issues Introduced

None currently known from hardware retest.

## Known Issues / Cleanup Candidates Not Solved Here

- `SEQ_LOAD_BACKGROUND` / `FRONT_SEQ_LOAD_FAST` at `0x50` still exists as a legacy setting path and still writes `seq_loadFastMode` on STM. It is not the active background-load mechanism. Future cleanup may remove/neutralize it after confirming no current voice-load behavior depends on `seq_loadFastMode`.
- Commented PRF cache opcode/helper surface remains as historical context in AVR/STM protocol files and sending helpers. It should not be reactivated for background loading.
- The old commented `PAR_CACHE_FOR_PERF` note remains historical cache wording unless later cleanup deletes it.

## Known Issues Resolved

- Background `.pat` no longer changes parameters or morph amounts.
- `.prf` / `.all` parameter-bearing loads recompute from loaded endpoints without resetting current global/per-voice morph amount state.
- AVR per-voice morph menu display no longer zeroes after `.prf` / `.all` loads.
- Repeated `.pat` / `.prf` / `.all` loads while already playing temp no longer recopy normal storage into temp.
- Temp playback is visible on the front panel and `SHIFT+PERF` is disabled while temp playback is active.
- Deprecated load-fast / background PRF cache voice-button shortcut was removed from active load-page behavior.

## Next Session Recommended Goal

Do a small cleanup pass only if desired: remove or neutralize the remaining legacy `SEQ_LOAD_BACKGROUND` / `FRONT_SEQ_LOAD_FAST` / `seq_loadFastMode` surface and delete the commented PRF-cache historical source blocks once the user is comfortable losing that breadcrumb context.

## Blockers

No functional blockers. Any cleanup of legacy cache/load-fast symbols should be treated as separate low-risk cleanup and retested because those symbols are old and easy to confuse with the new background-load feature.

## Critical Reminders For Next Session

- Background loading is now the `0x6d/0x6e` normal-to-temp swap handshake plus ordinary file load into normal storage. Do not revive the old cache model.
- `.pat` background loading is pattern-only. Never switch preset parameter ownership to temp for `.pat`.
- Current global and per-voice morph amounts are live performance state, not file-backed kit parameters. File loads must not reset them.
- `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` must remain before `END_OF_SOUND_PARAMETERS` for automation/modulation domains, but file-backed kit loops/dumps must use `END_OF_KIT_PARAMETERS`.
- If temp playback is already active, AVR must skip another background swap for `.pat`, `.prf`, or `.all` until STM reports playback returned to a normal pattern.
- `frontParser_serviceBackgroundSwapAck()` must remain non-blocking in the STM main loop. The AVR may wait; STM audio/MIDI/front-panel servicing must keep running.
- Do not add new sequencer-position code for background swap. The existing instant switch path preserves the current position.
- Keep priority send for `FRONT_SEQ_BACKGROUND_SWAP_DONE`; quiet UI/file-load receive states must not suppress the ACK.

## End Of Session Block

```text
DATE: 2026-06-21
SESSION GOAL: Complete background file loading through existing temporary Pattern/Preset storage while preserving uninterrupted playback.
COMPLETED: Implemented and hardware-confirmed the 0x6d/0x6e background-swap handshake, STM normal-to-temp copy and instant temp switch, pattern-only .pat behavior, morph preservation, repeated-load temp guard, and temp-playback LED/UI hints.
VERIFIED ON HARDWARE: yes; user confirmed final background-loading feature works after staged retests.

CHANGES THIS SESSION:
- AVR preset loading: background-swap wait, temp-playback state flag, file-kind gating, menu endpoint preservation, END_OF_KIT_PARAMETERS file-backed boundary, and per-voice morph display preservation.
- AVR comms: SEQ_BACKGROUND_SWAP_DONE rxDisable pass-through, swap-done dispatch, pattern-change temp-state clearing, per-voice morph restore guard, and LED hint refresh calls.
- STM receive/sequencer/preset/pattern: background-swap begin handler, copy current audible pattern/kit to temp, force instant switch, pattern-only .pat mode, delayed readiness ACK, and non-mutating file-load morph handling.
- AVR UI/LED: temp-playback LED hint, SHIFT+PERF disable, blink-slot increase to 8.
- Docs: session 028 handoff, session index, comms flow spec, temporary pattern/parameter load spec, and MEMORY refreshed.

KNOWN ISSUES INTRODUCED: none known.
KNOWN ISSUES RESOLVED: background .pat no longer changes parameters/morph; .all/.prf no longer zero per-voice morph menu values; repeated loads while temp playback is active skip redundant copy-to-temp; temp playback is visibly indicated on AVR.

NEXT SESSION RECOMMENDED GOAL: optional cleanup of legacy load-fast/cache naming and commented PRF-cache opcode surface.
BLOCKERS: none for the completed feature; cleanup should be hardware-smoke-tested if performed.

CRITICAL REMINDERS FOR NEXT SESSION:
- Active background loading is the 0x6d/0x6e normal-to-temp swap handshake, not the old 0x50 load-fast/cache setting path.
- .pat background loading must remain pattern-only and keep parameters normal.
- File loads must not mutate current global or per-voice morph amounts.
- Use END_OF_KIT_PARAMETERS for file-backed kit bytes; keep voice morph controls inside END_OF_SOUND_PARAMETERS for runtime automation domains.
```

