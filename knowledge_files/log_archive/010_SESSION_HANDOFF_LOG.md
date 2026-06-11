DATE: 2026-06-11
SESSION GOAL: Complete Phase 3 of the architectural refactor (Move Endpoint Restore And Temp Switching).
COMPLETED: Endpoint restore policy, temp/normal source switching, and the background-load session cache now live under `mainboard/LxrStm32/src/Preset/`. `frontPanelParser.c` now consumes `PresetLoadCache.h` instead of owning the PRF/session bookkeeping locally, and the Phase 3 split builds successfully with `make stm32 -j4`.

### Detailed Refactor Results (Phase 3)

#### Functions and State Moved Into `Preset/`
- **Endpoint Restore (`EndpointRestore.c`)**:
  - `seq_pushSingleParameterToFront()` -> `preset_pushSingleParameterToFront()`
  - `seq_pushSingleMorphParameterToFront()` -> `preset_pushSingleMorphParameterToFront()`
  - `seq_pushKitEndpointsToFront()` -> `preset_pushKitEndpointsToFront()`
  - `seq_pushKitEndpointsToFrontWithGlobalMorphReport()` -> `preset_pushKitEndpointsToFrontWithGlobalMorphReport()`
  - `seq_pushKitEndpointVoiceMaskToFront()` -> `preset_pushKitEndpointVoiceMaskToFront()`
  - `seq_pushKitEndpointVoiceMaskToFrontInternal()` -> `preset_pushKitEndpointVoiceMaskToFrontInternal()`
  - `seq_pushGlobalMorphToFront()` -> `preset_pushGlobalMorphToFront()`
  - `seq_pushEndpointUpdateForVoiceSourceChange()` -> `preset_pushEndpointUpdateForVoiceSourceChange()`
  - `seq_maybePushKitEndpointsToFrontWithGlobalMorphReport()` -> `preset_maybePushKitEndpointsToFrontWithGlobalMorphReport()`
  - `seq_endpointRestorePopRequest()` -> `preset_endpointRestorePopRequest()`
  - `seq_endpointRestoreClearCurrent()` -> `preset_endpointRestoreClearCurrent()`
  - `seq_endpointRestoreWaitTimedOut()` -> `preset_endpointRestoreWaitTimedOut()`
  - `seq_endpointRestoreSendNextFull()` -> `preset_endpointRestoreSendNextFull()`
  - `seq_endpointRestoreSendNextMasked()` -> `preset_endpointRestoreSendNextMasked()`
  - `seq_endpointRestoreSendNext()` -> `preset_endpointRestoreSendNext()`
  - `seq_endpointRestoreBusy()` -> `preset_endpointRestoreBusy()`
  - `seq_serviceEndpointRestore()` -> `preset_serviceEndpointRestore()`
- **Temp Switching (`TempPlaybackSwitch.c`)**:
  - `seq_captureTmpKitState()` -> `preset_captureTmpKitState()`
  - `seq_trackPatternUsesTmp()` -> `preset_trackPatternUsesTmp()`
  - `seq_synthVoiceUsesTmpFromTrackPatterns()` -> `preset_synthVoiceUsesTmpFromTrackPatterns()`
  - `seq_allVoiceSourcesUseTmp()` -> `preset_allVoiceSourcesUseTmp()`
  - `seq_allVoiceSourcesUseNormal()` -> `preset_allVoiceSourcesUseNormal()`
  - `seq_applyVoiceSource()` -> `preset_applyVoiceSource()`
  - `seq_markVoiceSourceTarget()` -> `preset_markVoiceSourceTarget()`
  - `seq_updateVoiceSourcesForPatternChange()` -> `preset_updateVoiceSourcesForPatternChange()`
- **Background-Load Finalizer (`PresetLoadCache.c`)**:
  - PRF/load-session state, deferred perf replay, live snapshot cache, and pending counter bookkeeping now live in `PresetLoadCache`.
  - `frontParser_resetPrfPendingCounters()` was promoted into the new cache API so the parser can rely on the Preset-owned session model.

#### Parser Changes
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c` now includes `Preset/PresetLoadCache.h`.
- The parser no longer owns `frontParser_deferPerfLoadCacheUntilPatternChange` as a local translation-unit variable.
- The parser now uses the Preset-owned cache API for pending-counter resets and background-load session bookkeeping.

#### KitState Change
- `mainboard/LxrStm32/src/Preset/KitState.c/.h`
  - Added `preset_captureTmpKitState()` so the temp-image clone is owned by the same module that owns the temp boundary.

#### Build Verification
- Verified with `make stm32 -j4` in `mainboard/LxrStm32`.

### Known Issues / Notes
- No new functional regressions were introduced during the split.
- Existing warnings remain in `PresetLoadCache.c` around the inactive restore helper and fallthrough annotations, but the build is green.

### Next Session Recommendation
- Phase 4: move pattern data and generators into `Sequencer/Pattern`.

