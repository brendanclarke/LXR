# AUDIT_REFACTOR_TARGETS

Date: 2026-06-04
Status: deferred refactor targets for after temp background loading is functional

This document is intentionally not a Session 004 implementation plan. Session 004 should stay focused on making temp-backed `.ALL` / `.PRF` loading and switching work. These notes collect cleanup targets for a later pass, likely around Session 006/007.

Session 004 consolidation note:

- Detailed preset storage, morph engine, temp playback, automation target, and
  STM `/Preset/` migration knowledge has been moved into
  `AUDIT_PRESET-MORPH_REFACTOR.md`.
- This document should remain the broader "other refactors" list:
  - legacy load/cache cleanup;
  - AVR scratch-buffer naming;
  - comms/protocol redundancy;
  - code comment cleanup;
  - non-preset parser/transport risks.
- Some older preset/morph notes may remain below as historical context, but the
  canonical refactor plan for STM-side preset/morph ownership is now
  `AUDIT_PRESET-MORPH_REFACTOR.md`.

Core assumption for the later refactor:

- STM owns sound-state authority: normal/temp endpoint params, morph endpoint params, interpolated params, automation target images, global morph amount, and per-voice morph amounts.
- AVR owns UI, file I/O, menu arrays, and requests. AVR temp-named arrays are file-read/menu-preservation buffers, not temp playback storage.
- Endpoint push-up to AVR is still required at every audible normal/temp boundary so the menu follows the currently selected sound image.

Session 005 note:

- The missing global morph menu-sync on normal/temp switch is now handled by
  display-only STM-to-AVR report traffic during the restore boundary. Keep the
  boundary push-up path in place, but do not treat this as a future refactor
  target.

## High-Value Refactor Targets

### 1. Retire `frontParser_applyDeferredVoiceCache()`

Current location:

- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`

Current callers observed:

- `seq_tick()` calls `frontParser_applyDeferredVoiceCache()` when `seq_loadPendigFlag` is set during manual pattern change.
- `seq_setRunning(0)` calls it when stopping playback.
- STM parser/file-load paths set the deferred/cache state that this later drains.

Related functions and state:

- `frontParser_applyDeferredVoiceCache()`
- `frontParser_applyPendingVoiceCache()`
- `frontParser_releaseVoiceCache()`
- `frontParser_unholdLoadedVoice()`
- `frontParser_uncacheVoice()`
- `frontParser_unholdVoice()`
- `frontParser_applyDeferredPerfMessages()`
- `frontParser_cacheDeferredPerfMessage()`
- `frontParser_shouldDeferPerfMessage()`
- `frontParser_clearDeferredPerfLoad()`
- `frontParser_clearPrfRuntimeFlags()`
- `frontParser_clearPrfCacheSession()`
- `frontParser_prfCacheCanExit()`
- `frontParser_prfCacheSessionActive()`
- state: `seq_voicesLoading`, `seq_newVoiceAvailable`, `midi_midiCacheAvailable[]`, `midi_midiCache[]`, `midi_midiLfoCacheAvailable[]`, `midi_midiVeloCacheAvailable[]`, `frontParser_deferredPerfVoiceCachePending`, `frontParser_deferredPerfPatternPending`, `frontParser_deferredPerfUnholdPending`, PRF cache state.

Problem:

- The function name says "apply deferred voice cache", but the function is now a mixed finalizer.
- It can promote legacy held voice cache into live DSP, replay deferred performance messages, apply staged temp pattern data, clear deferred perf flags, clear PRF runtime flags, and exit a PRF cache session.
- For temp-backed `.PRF` / `.ALL` background loading, the live application part is not supposed to be the source of truth. The normal `SeqKitState` should already contain the loaded endpoint/interpolated/automation state, while playback can continue from temp. A switch should select a `SeqKitState`, not replay `midi_midiCache`.
- The current implementation partly acknowledges this: `frontParser_releaseVoiceCache()` and `frontParser_unholdLoadedVoice()` clear voice cache instead of applying it when `seq_isTmpKitActive()` is true.
- The first switch after file load can still hang if this mixed function drains all deferred cleanup in one foreground burst.
- Session 004 functional branch note: the old file-load voice hold/unhold promotion path has now been disabled for temp-background loading. `FRONT_SEQ_LOAD_VOICE` still uses `seq_voicesLoading` as an ingress/cache guard so incoming file bytes store into STM normal endpoint state instead of becoming live menu edits, but `FRONT_SEQ_UNHOLD_VOICE`, deferred unhold cleanup, and release-cache paths now clear the legacy cache instead of promoting it to `midi_midiKit`, setting `seq_newVoiceAvailable`, or replaying cached messages through `midiParser_ccHandler()`.

Refactor direction:

- Retire `frontParser_applyDeferredVoiceCache()` as a separate legacy
  promotion path and fold any remaining useful behavior into a single
  background-load finalizer.
- Candidate API:
  - `presetLoad_finalizeTempBackgroundLoad()`
  - `presetLoad_clearDeferredSession()`
  - `presetLoad_hasPendingDeferredWork()`
- Architecturally, there should be only one background file-load mechanism in
  flight at a time. If a newer background load begins, it can discard and
  overwrite the older in-flight one.
- The finalizer should clear cache/session bookkeeping and release locks
  without applying cached voice params into live DSP.
- Do not preserve a separate direct-load voice-cache promotion mode after the
  refactor. If any legacy behavior still matters, fold it into the unified
  background-load path.
- Do not delete the remaining session cleanup blindly. It is also holding old
  PRF cache session cleanup and deferred pattern bookkeeping.

Redundancy to remove later:

- Voice parameter masks are duplicated on AVR and STM in separate files.
- `midi_midiCache` is acting as a second preset image for file loads, while `SeqKitState` is now the intended image authority.
- `seq_newVoiceAvailable` overlaps with more explicit load/session state and should be documented or replaced by a clearer "legacy cache pending" bitset.
- `frontParser_unholdVoice()` and `frontParser_uncacheVoice()` still exist as legacy live-cache promotion helpers, but the temp-background branch should not depend on them. During the refactor, either move them into a clearly named legacy loader module or remove them once all direct kit/voice load modes have a new STM-side preset API.
- The current name `seq_voicesLoading` is now overloaded: it means "file voice payload is being received; store/cache instead of live apply", not necessarily "a voice is held awaiting later live promotion." Rename or replace it in the preset load API.

### 2. Split STM preset state out of `Sequencer` (PHASE 1 & 2 COMPLETE)

The sequencer now contains a large amount of preset-state and morph-state service code. Some of it is used by sequencing, but the ownership boundary is no longer clean.

Proposed new concrete directory:

- `mainboard/LxrStm32/src/Preset/`

The user shorthand "mainboard/src/Preset/" should probably map to this repo's actual STM path above.

Suggested files:

- `Preset/KitState.h`
  - Owns `SeqKitState` or a renamed `PresetKitState`.
  - Owns `SeqKitAutomationTargets` or renamed `PresetAutomationTargets`.
  - Owns constants for normal/temp images, voice count, endpoint image kinds, and ingress target kinds.

- `Preset/KitState.c`
  - Owns `seq_normalKitState` / `seq_tmpKitState`, or renamed equivalents.
  - Owns current-image selection helpers.
  - Owns temp image capture and validity.
  - Candidate moved functions:
    - `seq_getCurrentImageKitState()`
    - `seq_getMorphKitForImage()`
    - `seq_captureTmpKitState()`
    - `seq_isTmpKitActive()`
    - STM-side parts of `seq_setTmpKitActive()` that only select kit images and invalidate caches.

- `Preset/ParameterMap.h/.c`
  - Owns voice parameter masks and parameter classification helpers.
  - Candidate moved data/functions:
    - `seq_voiceParamMask`
    - `seq_canonicalParamFromVoiceMask()`
    - `seq_isVoiceParameter()`
    - `seq_voiceMaskForParameter()`
    - `seq_firstVoiceForMask()`
    - `seq_isAutomationTargetSelectorParam()`
    - `seq_isMorphAmountParam()`
  - Longer-term target: generate or share the AVR and STM voice mask tables from one source to remove duplication.

- `Preset/AutomationTargets.h/.c`
  - Owns resolved destination storage and application.
  - Candidate moved functions:
    - `seq_resolveAutomationTargetSelector()`
    - `seq_updateInterpolatedAutomationTarget()`
    - `seq_updateFrontAndInterpolatedAutomationTargets()`
    - `seq_applyLiveAutomationTargetSelector()`
    - `seq_storeLfoDestinationIngress()`
    - `seq_storeVelocityDestinationIngress()`
    - `seq_storeMacroDestinationIngress()`
    - `seq_applyVoiceAutomationTargets()`
    - `seq_applySharedAutomationTargets()`
    - `seq_applyNormalEndpointAutomationTargets()`

- `Preset/ParameterIngress.h/.c`
  - Owns raw endpoint byte ingress and live-edit routing.
  - Candidate moved functions:
    - `seq_setIngressTarget()`
    - `seq_getIngressTarget()`
    - `seq_shouldApplyIngressToLive()`
    - `seq_setAutomationIngressTarget()`
    - `seq_storeParameterIngress()`
    - `seq_storeMorphParameterIngress()`
    - `seq_applySingleParameterValueWithFlags()`
    - `seq_applySingleParameterValue()`
  - API goal: MIDI/front parser should call "store param into selected preset image" and not know about normal/temp arrays directly.

- `Preset/MorphEngine.h/.c` (PHASE 2 COMPLETE)
  - Owns STM-side morph state and interpolation service.
  - Candidate moved functions:
    - `seq_syncVMorphAmountMirrorsFromLiveSources()` (COMPLETE)
    - `seq_selectVoiceMorphAmountFromKit()` (COMPLETE)
    - `seq_setVoiceMorphLiveAmount()` (COMPLETE)
    - `seq_morphImageVoiceIsLive()` (COMPLETE)
    - `seq_getMorphImageForVoice()` (COMPLETE)
    - `seq_invalidateLiveMorphApplyCache()` (COMPLETE)
    - `seq_invalidateAllLiveMorphApplyCaches()` (COMPLETE)
    - `seq_resetLiveMorphApplyCache()` (COMPLETE)
    - `seq_liveMorphApplyNeeded()` (COMPLETE)
    - `seq_interpolateMorphValue()` (COMPLETE)
    - `seq_morphAutomationValueToAmount()` (COMPLETE)
    - `seq_advanceMorphScanCursor()` (COMPLETE)
    - `seq_setGlobalMorphAmount()` (COMPLETE)
    - `seq_setGlobalMorphAutomationValue()` (COMPLETE)
    - `seq_resetVoiceMorphAmountsToGlobal()` (COMPLETE)
    - `seq_setVoiceMorphAmount()` (COMPLETE)
    - `seq_setVoiceMorphAutomationValue()` (COMPLETE)
    - `seq_setVoiceMorphMaskAutomationValue()` (COMPLETE)
    - `seq_modulateVoiceMorphAmount()` (COMPLETE)
    - `seq_serviceMorphInterpolation()` (COMPLETE)
  - API goal: sequencer and modulation nodes set morph amounts; morph engine updates preset image and DSP.

- `Preset/EndpointRestore.h/.c`
  - Owns STM-to-AVR endpoint/menu push-up.
  - Candidate moved functions/state:
    - `seq_pushSingleParameterToFront()`
    - `seq_pushSingleMorphParameterToFront()`
    - `seq_pushKitEndpointsToFront()`
    - `seq_pushKitEndpointVoiceMaskToFront()`
    - endpoint restore queue and cursors
    - `seq_endpointRestorePopRequest()`
    - `seq_endpointRestoreClearCurrent()`
    - `seq_endpointRestoreWaitTimedOut()`
    - `seq_endpointRestoreSendNextFull()`
    - `seq_endpointRestoreSendNextMasked()`
    - `seq_endpointRestoreSendNext()`
    - `seq_endpointRestoreBusy()`
    - `seq_serviceEndpointRestore()`
    - `seq_pushEndpointUpdateForVoiceSourceChange()`
    - `seq_maybePushKitEndpointsToFront()`
  - API goal: sequencer requests "restore full current image" or "restore these voices"; restore module handles protocol handshake and rate limiting.

- `Preset/TempPlaybackSwitch.h/.c`
  - Owns normal/temp per-voice source tracking.
  - Candidate moved functions:
    - `seq_trackPatternUsesTmp()`
    - `seq_synthVoiceUsesTmpFromTrackPatterns()`
    - `seq_applyVoiceSource()`
    - `seq_markVoiceSourceTarget()`
    - `seq_allVoiceSourcesUseTmp()`
    - `seq_allVoiceSourcesUseNormal()`
    - `seq_updateVoiceSourcesForPatternChange()`
    - source-selection parts of `seq_setTmpKitActive()`
  - API goal: sequencer reports pattern source changes; module applies correct parameter/morph/automation source per synth voice.

- `Preset/PresetLoadCache.h/.c`
  - Owns PRF/deferred file load cache state now living in `frontPanelParser.c`.
  - Candidate moved functions:
    - `frontParser_beginFileLoadIngress()`
    - `frontParser_endFileLoadIngress()`
    - `frontParser_clearDeferredPerfLoad()`
    - `frontParser_resetPrfPendingCounters()`
    - `frontParser_clearPrfCacheSession()`
    - `frontParser_clearPrfRuntimeFlags()`
    - `frontParser_prfCacheSessionActive()`
    - `frontParser_prfCacheCanExit()`
    - `frontParser_prfCacheCountPatternWrite()`
    - `frontParser_prfPendingCountsValid()`
    - `frontParser_capturePrfStmLiveSnapshot()`
    - `frontParser_prfCacheUseLivePattern()`
    - `frontParser_prfCacheTrackUsesLivePattern()`
    - `frontParser_prfCacheLiveStep()`
    - `frontParser_prfCacheLiveMainSteps()`
    - `frontParser_prfCacheLiveLengthRotate()`
    - `frontParser_prfCacheLivePatternSetting()`
    - `frontParser_prfCacheLiveMidiChannel()`
    - `frontParser_prfCacheLiveNoteOverrideValue()`
    - `frontParser_prfCacheTakeLiveVMorphFlag()`
    - `frontParser_prfCacheLiveVMorphAmountValue()`
    - the split replacement for `frontParser_applyDeferredVoiceCache()`
  - API goal: `frontPanelParser.c` should parse protocol and delegate load/cache semantics.

- `Preset/ProtocolEndpoint.h/.c`
  - Optional thin layer for endpoint transport packet encoding/decoding.
  - Candidate moved responsibilities:
    - raw PRF restore param/morph opcode encoding
    - restore begin/done/ready/ack handshake state
    - endpoint dump begin/end and automation phase interpretation
  - This may overlap with a later broader `Comms/` split; keep small if introduced.

## Communication Modes Audit

This section folds forward still-relevant material from `knowledge_files/session_in_flight/COMMS_FLOW_AUDIT-IN_FLIGHT-POST_MORPH_MOVE.md` and the hardware comms audit, then adds the current code-level mode tree.

Common physical/protocol layer:

- Link: UART, 500000 baud, 8N1, no RTS/CTS.
- Framing: MIDI-like byte stream. Status bytes have high bit set; data bytes are 7-bit.
- Normal messages are usually 3 bytes: `status, data1, data2`.
- Bulk pattern traffic uses fake SysEx: `SYSEX_START`, one mode byte, payload data bytes, `SYSEX_END`.
- Priority endpoint restore bytes can bypass normal quiet/sysex suppression on the STM side.
- Current risk retained from hardware audit: RX/TX FIFO overflow and some old waits are still fragile; broad transport hardening should remain deferred until temp loading works.

### Mode Tree

#### A. Idle/live control mode

Purpose:

- User edits, sequencer commands, LED requests, BPM, macro/target changes, simple parameter edits.

AVR to STM initializing messages:

- None beyond the status byte of each 3-byte message.
- Examples:
  - `MIDI_CC, rawParam, value`
  - `CC_2, rawParamMinus128, value`
  - `SEQ_CC, SEQ_SET_GLOBAL_MORPH, value`
  - `SEQ_CC, SEQ_CHANGE_PAT or SEQ_CHANGE_TMP_PAT, pattern`
  - `LED_CC, LED_QUERY_SEQ_TRACK, trackPattern`
  - `CC_LFO_TARGET`, `CC_VELO_TARGET`, `MACRO_CC`

STM behavior:

- Parses single messages in `frontParser_handleMidiMessage()`.
- Parameter ingress goes through `seq_storeParameterIngress()` / live DSP apply depending on ingress mode.
- Automation target sidebands go through target ingress and optionally live mod-node apply.
- Sequencer commands go through `frontParser_handleSeqCC()`.

Concluding messages:

- Usually none.
- Some commands cause STM-to-AVR status updates, such as pattern change ACK or track LED/length/rotation replies.

Rough length:

- 3 bytes per command.
- LED query response can be multiple 3-byte messages, depending on main/substep LED state and track metadata.

Variations/refactor notes:

- `MIDI_CC` is reused as front-panel parameter update even though it is also real MIDI CC vocabulary on STM.
- Low raw params need `+1` only at live DSP application, not at endpoint storage.
- Many control/status opcodes are symmetric by value but differently named between headers.

#### B. STM-to-AVR live UI/status reflection

Purpose:

- Keep the front panel updated after sequencer changes, external MIDI edits, bank changes, pattern ACKs, and menu-related state changes.

STM initializing messages:

- 3-byte messages such as:
  - `FRONT_SEQ_CC, FRONT_SEQ_CHANGE_PAT, pat`
  - `FRONT_SEQ_CC, FRONT_SEQ_RUN_STOP, state`
  - `FRONT_STEP_LED_STATUS_BYTE, FRONT_LED_* , value`
  - `PARAM_CC/PARAM_CC2` for live parameter reflection from external MIDI paths
  - `BANK_CHANGE_CC`

AVR behavior:

- `frontPanel_parseData()` updates menu arrays, LEDs, pattern state, long-op flags, or repaint requests.

Concluding messages:

- Usually none.

Rough length:

- 3 bytes per event. Beat/step LED traffic repeats during playback.

Variations/refactor notes:

- UI/status traffic is suppressed in STM quiet mode except for priority messages.
- `PRESET_NAME` and `VOICE_CC` collide at `0xb4` on AVR; `SET_BPM` and `PRESET` collide at `0xb5`.

#### C. Callback flush / hold-for-buffer mode

Purpose:

- AVR waits for STM to catch up before continuing selected preset operations.

AVR initializing message:

- `CALLBACK_ACK` / `FRONT_CALLBACK_ACK` as a single byte `0xfd`.

STM behavior:

- On `FRONT_CALLBACK_ACK`, STM clears front FIFOs and sends `FRONT_CALLBACK_ACK` back via SysEx byte path.

AVR concluding condition:

- `frontPanel_wait` clears when AVR receives `CALLBACK_ACK`.

Rough length:

- 1 byte AVR to STM, 1 byte STM to AVR, plus FIFO flush side effects.

Variations/refactor notes:

- This is a very blunt synchronization primitive. It can discard queued outbound and inbound data on STM.
- Later refactor should replace this with a scoped drain/ack that does not clear unrelated traffic.

#### D. Flow-control session mode

Purpose:

- Software flow control for high-volume transfers and load sessions.

Messages:

- `SEQ_FLOW_BEGIN` / `FRONT_SEQ_FLOW_BEGIN`
- `SEQ_FLOW_GRANT` / `FRONT_SEQ_FLOW_GRANT`
- `SEQ_FLOW_END` / `FRONT_SEQ_FLOW_END`
- `SEQ_FLOW_ABORT` / `FRONT_SEQ_FLOW_ABORT`

Channels:

- `FLOW_CH_LOAD_SESSION`
- `FLOW_CH_GLOBALS`
- `FLOW_CH_VOICE_PARAM`
- `FLOW_CH_DRUM_META`

AVR initializing message:

- `SEQ_CC, SEQ_FLOW_BEGIN, channel`

STM status response:

- `FRONT_SEQ_CC, FRONT_SEQ_FLOW_GRANT, encodedChannelCredits`
- For load-session begin, STM sets quiet/load mode.

Concluding messages:

- AVR sends `SEQ_CC, SEQ_FLOW_END, channel`.
- STM replies with grant/ack.
- Abort path: either side can send `SEQ_FLOW_ABORT`.

Rough length:

- 3 bytes for begin, 3 bytes for grant.
- 3 bytes for end, 3 bytes for grant/ack.
- Credit-metered bursts add a grant every N accepted messages.

Variations/refactor notes:

- This partially overlaps with old SysEx per-step ACKs and restore handshakes.
- Later protocol cleanup could make this the one general transfer/session mechanism.

#### E. File-load semantic mode

Purpose:

- Tell STM that incoming traffic belongs to file load, voice load, pattern load, or endpoint load.

AVR initializing messages:

- `SEQ_CC, SEQ_FILE_BEGIN, fileType`
- Optional `SEQ_CC, SEQ_LOAD_VOICE, voice`
- Optional `SEQ_CC, SEQ_LOAD_FAST, value`

STM behavior:

- `frontParser_beginFileLoadIngress(...)` routes parameter ingress to normal endpoint storage.
- `FRONT_SEQ_FILE_BEGIN` resets or prepares file-load/morph/cache state.
- Voice loads set `seq_voicesLoading` and cache relevant live-apply messages.
- Pattern data can be staged to `seq_tmpPattern` or written into normal pattern storage depending on current protected/live pattern rules.

Concluding messages:

- `SEQ_CC, SEQ_UNHOLD_VOICE, voice`
- `SEQ_CC, SEQ_FILE_DONE, fileType`
- On abort or flow timeout, `SEQ_FLOW_ABORT`.

Rough length:

- Framing commands are small 3-byte messages.
- Actual file load length depends on included globals, pattern data, voices, metadata, and endpoint dump modes.

Variations:

- `.SND` kit load: may send one endpoint group only, front or morph.
- `.ALL`: globals, pattern data, both kit endpoint images, drum meta, and all or selected voices.
- `.PRF`: performance metadata, pattern data, both kit endpoint images, drum meta as needed, selected voices.
- Background temp flow should update STM normal storage while temp sound continues from temp storage.

Refactor notes:

- File-load semantic mode overlaps with PRF cache mode, deferred voice cache mode, endpoint dump mode, and flow session mode.
- With STM normal/temp preset images in place, PRF/ALL background load should not need a second cache-as-sound-authority path.

#### F. AVR-to-STM endpoint dump mode

Purpose:

- Send AVR kit/front and morph endpoint arrays to STM normal endpoint storage.

AVR initializing message:

- `SEQ_CC, SEQ_TMP_KIT_ENDPOINT_BEGIN, endpointMode`

Endpoint modes:

- `SEQ_TMP_KIT_ENDPOINT_BOTH`
- `SEQ_TMP_KIT_ENDPOINT_FRONT_ONLY`
- `SEQ_TMP_KIT_ENDPOINT_MORPH_ONLY`

Payload:

- Front endpoint bytes:
  - `PRF_RESTORE_PARAM_CC, rawParam, value` for params `< 128`
  - `PRF_RESTORE_PARAM_CC2, rawParamMinus128, value` for params `>= 128`
- Morph endpoint bytes:
  - `PRF_RESTORE_MORPH_CC, rawParam, value`
  - `PRF_RESTORE_MORPH_CC2, rawParamMinus128, value`
- Automation target sidebands:
  - `SEQ_CC, SEQ_TMP_KIT_AUTOMATION_PHASE, FRONT_ENDPOINT`
  - 6 velocity destination messages
  - 6 LFO destination messages
  - 4 macro destination messages
  - repeat with `MORPH_ENDPOINT` phase for morph endpoint targets

Concluding messages:

- `SEQ_CC, SEQ_TMP_KIT_AUTOMATION_PHASE, NONE`
- `SEQ_CC, SEQ_TMP_KIT_ENDPOINT_END, 0`

Rough length:

- `END_OF_SOUND_PARAMETERS` / `NUM_PARAMS` is 310.
- Front-only: begin 3 + 310 param messages * 3 + phase 3 + 16 target sidebands * 3 + none 3 + end 3 = about 987 bytes.
- Morph-only: about 987 bytes.
- Both: begin 3 + 930 front bytes + 3 + 48 front targets + 930 morph bytes + 3 + 48 morph targets + 3 + 3 = about 1968 bytes.

Variations/refactor notes:

- The opcode names `PRF_RESTORE_*` are misleading for AVR-to-STM endpoint dump. They are not only PRF and not only restore.
- Endpoint bytes and automation target sidebands are separate streams glued together by phase state. A future packet could combine parameter bytes and resolved destination metadata in one explicit endpoint transfer.

#### G. STM-to-AVR endpoint restore / menu sync mode

Purpose:

- Push STM canonical normal/temp endpoint image back to AVR so the menu follows audible source.

STM initializing message:

- `PARAM_RESTORE_BEGIN, 0, 0`

AVR status response:

- Sets `frontParser_restoreActive = 1`.
- Suppresses outbound front-panel traffic except restore handshake.
- Sends `PARAM_RESTORE_READY, 0, 0`.

STM payload:

- Front endpoint bytes via `PRF_RESTORE_PARAM_CC/CC2`.
- Morph endpoint bytes via `PRF_RESTORE_MORPH_CC/CC2`.

STM concluding message:

- `PARAM_RESTORE_DONE, 0, 0`

AVR concluding response:

- Updates menu arrays during payload.
- On done, clears restore active, repaints menu, sends `PARAM_RESTORE_ACK, 0, 0`.

Rough length:

- Full restore: begin 3 + ready 3 + 310 front messages * 3 + 310 morph messages * 3 + done 3 + ack 3 = about 1872 bytes on the link.
- Masked per-voice restore: current STM `SEQ_VOICE_PARAM_LENGTH` is 37. One voice is about begin/ready/done/ack 12 bytes + 37 front * 3 + 37 morph * 3 = about 234 bytes. Multiple voices scale by mask.
- Current implementation rate-limits payload to one parameter per `seq_serviceEndpointRestore()` service step.

Variations:

- Full restore when all voices use temp or all use normal.
- Masked restore when only some synth voices switch normal/temp source.
- Hi-hat has two tracks but one synth voice; both hi-hat tracks and voice parameters should switch together.

Refactor notes:

- This mode reuses the same `PRF_RESTORE_*` opcodes as AVR-to-STM endpoint dump in the opposite direction.
- It has its own begin/ready/done/ack handshake separate from flow-control mode.
- Later cleanup could define direction-specific names or one explicit `ENDPOINT_IMAGE_TRANSFER` mode with direction and image id.

#### H. SysEx pattern query modes

Purpose:

- AVR asks STM for pattern/step data during save/query operations.

Common initializing sequence:

- AVR sends `SYSEX_START`.
- STM echoes/acknowledges `SYSEX_START`.
- AVR sends a mode byte.

Modes:

- `SYSEX_REQUEST_STEP_DATA`
  - AVR sends 2 data bytes of step number.
  - STM sends 8 data bytes per step: 7 lower values plus packed MSB byte.
  - AVR reconstructs one `StepData`.
  - Concludes with `SYSEX_END`.

- `SYSEX_REQUEST_MAIN_STEP_DATA`
  - AVR sends 2 data bytes of step/main-step index.
  - STM sends 5 data bytes: 3 bytes main step data, 1 length, 1 scale.
  - Concludes with `SYSEX_END`.

- `SYSEX_REQUEST_PATTERN_DATA`
  - AVR sends 1 pattern number.
  - STM sends 2 data bytes: next pattern and repeat/change bar.
  - Concludes with `SYSEX_END`.

Rough length:

- Step query: about 1 start + 1 mode + 2 request + 8 response + 1 end, plus echoes/callback waits.
- Main step query: about 1 + 1 + 2 + 5 + 1.
- Pattern info query: about 1 + 1 + 1 + 2 + 1.

Refactor notes:

- These are tiny request/response packets. They do not need the same machinery as 64 KB pattern loads.
- Could become framed `PATTERN_QUERY` messages with explicit payload length.

#### I. SysEx pattern write/load modes

Purpose:

- AVR sends pattern data from SD to STM during `.PAT`, `.ALL`, and `.PRF` loads.

Common initializing sequence:

- AVR sends `SYSEX_START`.
- STM echoes/acknowledges start.
- AVR sends a mode byte.

Modes:

- `SYSEX_RECEIVE_MAIN_STEP_DATA` / AVR name `SYSEX_SEND_MAIN_STEP_DATA`
  - Per track/pattern payload: 4 bytes, info byte plus 3 bytes main step bitfield.
  - STM ACK: mode byte after each 4-byte packet.
  - Full all-pattern rough payload: 7 tracks * 8 patterns * 4 = 224 data bytes plus 56 ACK bytes.

- `SYSEX_RECEIVE_PAT_LEN_DATA`
  - Per track/pattern payload: 2 bytes, info byte plus length.
  - STM ACK after each pair.
  - Full all-pattern rough payload: 112 data bytes plus 56 ACK bytes.

- `SYSEX_RECEIVE_PAT_SCALE_DATA`
  - Per track/pattern payload: 2 bytes, info byte plus scale.
  - STM ACK after each pair.
  - Full all-pattern rough payload: 112 data bytes plus 56 ACK bytes.

- `SYSEX_RECEIVE_PAT_CHAIN_DATA`
  - Current AVR sends next and repeat as two separately ACKed bytes per pattern.
  - Full rough payload: 16 data bytes plus 16 ACK waits.
  - Hardware audit notes current ACK placement is fragile/redundant.

- `SYSEX_BEGIN_PATTERN_TRANSMIT`
  - Per track/pattern block: 128 steps * 9 data bytes = 1152 data bytes.
  - STM sends `SYSEX_STEP_ACK` after each step and a final mode callback when block is complete.
  - Full all-pattern payload: 7 * 8 * 128 * 9 = 64512 AVR-to-STM data bytes, plus 7168 per-step ACK bytes.

Concluding sequence:

- AVR sends `SYSEX_END`.
- STM exits sysex mode and echoes/acknowledges end.

Variations/refactor notes:

- Pattern metadata is split across mainstep, length, scale, chain, and step-data modes.
- This could be one framed `PATTERN_BLOCK` transfer with type, pattern, track, length, checksum, and ACK/NACK.
- Current per-step ACK prevents FIFO overflow but creates high latency. A future flow-controlled packet could ACK per chunk instead of per step.
- Do not reintroduce unbounded STM RX drain; a prior attempt froze known-good `.ALL` loading.

#### J. PRF cache / deferred performance mode

Purpose:

- Preserve or defer live performance state while loading a performance during playback.

Messages:

- `SEQ_PRF_CACHE_BEGIN`
- `SEQ_PRF_PENDING_BEGIN`
- `SEQ_PRF_PENDING_DONE`
- `SEQ_PRF_CACHE_ABORT`
- `SEQ_PRF_AVR_SNAPSHOT_BEGIN`
- `SEQ_PRF_AVR_SNAPSHOT_END`
- `SEQ_PRF_RESTORE_AVR_LIVE`
- `PRF_CACHE_STATUS`

Initializing/status:

- AVR sends cache control with `SEQ_CC`.
- STM replies with `PRF_CACHE_STATUS, command, accepted/rejected`.
- Flow grants are also used on the load-session channel.

Payload variations:

- STM live snapshot captured from current pattern/kit state.
- AVR live snapshot phase can receive endpoint bytes.
- Pending pattern writes counted by STM to validate expected mainstep/step/length/scale/chain counts.
- Restore AVR live sends endpoint restore messages back to the AVR.

Concluding messages:

- `SEQ_PRF_PENDING_DONE` for pending validity.
- `SEQ_PRF_CACHE_ABORT` or flow abort for failure.
- `frontParser_applyDeferredVoiceCache()` is one of the remaining legacy exit
  paths and should be folded into the unified background-load finalizer rather
  than kept as its own promotion mechanism.

Rough length:

- Control messages are 3 bytes each plus 3-byte status/grants.
- Snapshot/restore payload can approach endpoint restore size, depending on whether full endpoints are sent.
- Pending pattern data length is the same as the relevant SysEx pattern write modes above.

Refactor notes:

- Treat this as WIP after the STM morph move.
- It overlaps strongly with temp-backed normal storage. Once temp background
  loading is stable, the PRF cache/snapshot pieces should collapse into a
  single background-load finalizer that loads into normal image while temp
  image plays, then switches on demand.
- The snapshot/restore pieces should be separated from the retired legacy
  deferred voice-cache promotion path.

#### K. Sample upload/count mode

Purpose:

- Sample memory operations.

Messages:

- `SAMPLE_CC, SAMPLE_START_UPLOAD, 0`
- `SAMPLE_CC, SAMPLE_COUNT, 0`

STM behavior:

- Start upload stops sequencer, initializes sample memory, loads samples, locks flash, then sends `ACK`.
- Count request returns `SAMPLE_CC, FRONT_SAMPLE_COUNT, numSamples`.

Rough length:

- Control is 3 bytes each direction plus sample operation time.

Refactor notes:

- This uses bare `ACK`/`NACK` concepts from UART code and should be documented separately from preset transfer ACKs.

## Protocol Cleanup Targets

1. Rename or replace `PRF_RESTORE_*`.
   - They now carry endpoint bytes in both directions.
   - They are used for file-load ingress, temp endpoint dump, and menu restore.
   - Candidate names: `ENDPOINT_FRONT_CC`, `ENDPOINT_FRONT_CC2`, `ENDPOINT_MORPH_CC`, `ENDPOINT_MORPH_CC2`.

2. Collapse endpoint dump and endpoint restore into one explicit endpoint transfer concept.
   - Direction, image, endpoint group, and mask should be explicit.
   - Automation target sidebands should be part of the endpoint transfer, not an ambient phase.

3. Collapse pattern metadata SysEx modes.
   - Mainstep, length, scale, and chain can be one pattern metadata packet.
   - Step data can be chunked by track/pattern with length and checksum.

4. Unify handshakes.
   - Current system has flow grants, SysEx callbacks, per-step ACKs, `CALLBACK_ACK`, sample `ACK`, and parameter restore ready/ack.
   - Later protocol should pick one session/packet ACK model with timeout and abort cleanup.

5. Add explicit transfer lengths and integrity checks.
   - The fake SysEx stream has no CRC/checksum and weak resync.
   - A single dropped data byte in bulk payload can silently corrupt pattern data.

6. Keep STM RX drain changes deferred and bounded.
   - Hardware audit recommends more drain throughput, but this branch already saw unbounded drain freeze `.ALL` load.
   - If revisited, use a small fixed max bytes per pass with full `.ALL`, `.PRF`, stopped/running, and temp-switch tests.

7. Centralize parameter index conversion.
   - Raw endpoint/menu params use raw AVR indices.
   - Live DSP low params require `+1`.
   - High params do not follow the same offset rule.
   - One conversion helper would reduce off-by-one regressions.

8. Generate shared protocol constants or headers.
   - AVR and STM headers duplicate status bytes and subcommands.
   - Some names differ for the same byte.
   - Some names collide on AVR (`VOICE_CC` / `PRESET_NAME`, `SET_BPM` / `PRESET`).

9. Generate or share voice parameter masks.
   - AVR `voice*presetMask[]`, STM `voice*presetMask[]`, and STM `seq_voiceParamMask[][]` are related but duplicated.
   - Later refactor should derive them from one source table.

10. Replace hard waits and hard locks.
   - Old SysEx/callback waits and SD error `while(1)` paths remain.
   - Add one timeout family at a time after temp exchange is stable.

## Relevant Stale Comms Audit Material Folded Forward

The post-morph in-flight comms audit remains useful for these points:

- The immediate Session 004 goal is semantic/temp storage correctness, not broad transport hardening.
- Endpoint traffic after morph move is endpoint storage traffic, not live morph result traffic.
- File loads should write normal endpoint storage, not temp endpoint storage.
- File loads should not write `interpolatedParams[]` directly; the morph worker owns interpolation.
- Ordinary live low CC uses the old low-parameter `+1` offset only when calling live DSP apply.
- `SEQ_FILE_BEGIN` / `SEQ_FILE_DONE`, endpoint brackets, automation target phases, and PRF cache controls remain active protocol surfaces.
- Old SysEx/callback waits remain deferred hardening.
- Do not change PAT_CHAIN ACK placement, baud rate, or STM RX drain casually during temp-cache debugging.
- PRF cache/deferred state remains WIP and should be reconciled with the post-morph temp storage model.

The stale or misleading part to avoid carrying forward:

- Do not describe AVR temp-named arrays as temp playback staging.
- Do not assume copy-to-temp should dump AVR endpoints to STM. In the current intended model, copy-to-temp is an STM-side clone of normal pattern and parameter images.
- Do not move endpoint push-up away from temp boundaries; AVR menu sync must still happen on every transition into or out of temp.

## Additional Refactor Targets From Temp Delay / Envelope Audit

### Retire legacy load voice release caches

Current state:

- File-load voice payloads are now intended to populate STM-side normal endpoint
  storage, not create a later live release/promotion event.
- The old cache names and structures remain in `MidiParser.c/.h` and
  `frontPanelParser.c`:
  - `midi_midiCache[]`
  - `midi_midiCacheAvailable[]`
  - `midi_midiLfoCache[]`
  - `midi_midiLfoCacheAvailable[]`
  - `midi_midiVeloCache[]`
  - `midi_midiVeloCacheAvailable[]`
  - `midi_midiKit[]`
  - `midi_kitLfoCache[]`
  - `midi_kitVeloCache[]`

Refactor target:

- Remove or quarantine the legacy cache promotion API:
  - `frontParser_unholdVoice(...)`
  - `frontParser_uncacheVoice(...)`
  - `frontParser_releaseVoiceCache(...)`
  - `frontParser_unholdLoadedVoice(...)`
  - `frontParser_applyPendingVoiceCache(...)`
  - `frontParser_applyDeferredVoiceCache(...)`
- Replace it with the unified background-load/session finalizer in the future
  STM-side `/Preset/` service.
- Future hold/unhold behavior, if needed, should be a deliberate preset API
  policy or folded into the unified background-load path rather than ambient
  MIDI parser cache state.

### Envelope position mirror ownership

Current state:

- `PAR_ENVELOPE_POSITION_1..6` maps to `midi_envPosition[0..5]`.
- The same parameter path can call DSP envelope setters immediately.
- This makes envelope position unsuitable for broad bulk live apply during
  normal/temp source switching.

Refactor target:

- Move envelope-position ownership out of the generic MIDI parser path and into
  the future STM-side `/Preset/`/parameter service.
- Separate three operations:
  - store endpoint value;
  - update compatibility mirror (`midi_envPosition[]`);
  - perform the actionful DSP envelope setter.
- Add a passive mirror update API, optionally with an
  `envPositionMirrorDirty[]` / `envelopes_dirty[]` side array, so bulk restore
  can keep mirrors coherent without triggering envelopes.

### Rename preset endpoint arrays in `SeqKitState`

Current state:

- `SeqKitState.frontPanelParams[]` stores the kit/front endpoint parameter
  image on the STM.
- `SeqKitState.morphParams[]` stores the morph endpoint parameter image on the
  STM.
- Both endpoint images appear in the front-panel menu through restore/push-up,
  so the name `frontPanelParams` is misleading.
- The front panel is no longer the canonical owner of preset data after the
  STM-side morph move.

Refactor target:

- Rename `SeqKitState.frontPanelParams[]` to `kitEndpointParams[]`.
- Rename `SeqKitState.morphParams[]` to `morphEndpointParams[]`.
- Do this during the future STM-side `/Preset/` refactor, not during the current
  temp-switch stabilization pass.
- Update comments and APIs to describe endpoint ownership in preset terms, not
  AVR/front-panel terms.

### Unify raw automation selector bytes with resolved destinations

Current state:

- LFO destination sideband messages can carry resolved destination parameters
  into STM through `FRONT_CC_LFO_TARGET` / `seq_storeLfoDestinationIngress(...)`.
- Endpoint arrays store raw selector bytes:
  - current name `frontPanelParams[PAR_TARGET_LFOx]`;
  - current name `morphParams[PAR_TARGET_LFOx]`.
- Automation target structs store resolved destinations:
  - `frontPanelAutomationTargets.lfoDestination[]`;
  - `morphParameterEndpointAutomationTargets.lfoDestination[]`;
  - `interpolatedAutomationTargets.lfoDestination[]`.
- These two representations must stay coherent. A bug in session 004 allowed
  live LFO destination sidebands to update the resolved destination structs and
  DSP while leaving the raw endpoint selector bytes stale/off.

Refactor target:

- Future `/Preset/` API should expose one authoritative setter for LFO
  destination updates that updates both representations together:
  - raw endpoint voice selector;
  - raw endpoint target selector;
  - resolved automation destination;
  - interpolated target cache when appropriate.
- Avoid callers directly writing one half of the state.
- Keep an inverse resolver from resolved destination parameter back to selector
  index, or better, transport both raw selector and resolved destination in one
  explicit endpoint/update packet.
- Apply the same review to velocity and macro destination sidebands, even though
  the confirmed bug was LFO-specific.

## Communication / Protocol Redundancy Targets From Session 004

Canonical comms reference:

- `knowledge_files/hardware_archive/ATMEGA_STM32F4_COMMS_AUDIT.md`

### Raw endpoint params plus resolved automation sidebands

Current state:

- Automation target information can be transported twice:
  - raw selector endpoint bytes, such as `PAR_TARGET_LFOx`;
  - resolved sideband destination messages, such as `FRONT_CC_LFO_TARGET`.
- Session 004 confirmed that these can diverge and cause wrong normal/temp
  restore behavior.

Refactor target:

- Introduce one authoritative target-assignment transfer/update surface.
- Ideal packet should include:
  - endpoint image kind: kit/front vs morph;
  - source modulator kind: LFO, velocity, macro;
  - source index;
  - raw selector byte;
  - target voice selector where relevant;
  - resolved destination parameter.
- STM should update raw endpoint storage and resolved automation target structs
  in one API call.
- AVR should not need to send a separate sideband phase solely to reconstruct
  destination metadata.

### Endpoint bracket naming

Current state:

- `FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN`
- `FRONT_SEQ_TMP_KIT_AUTOMATION_PHASE`
- `FRONT_SEQ_TMP_KIT_ENDPOINT_END`

Problem:

- Names imply writing temp playback kit.
- In the Session 004 model, file/background endpoint dumps refresh normal
  endpoint storage while temp may be playing.

Refactor target:

- Rename protocol concepts around "endpoint restore/load session" rather than
  "tmp kit".
- Keep temp playback state separate from background file-load destination state.

### Legacy voice hold/unhold protocol

Current state:

- `FRONT_SEQ_LOAD_VOICE`
- `FRONT_SEQ_UNHOLD_VOICE`
- `seq_voicesLoading`
- `seq_newVoiceAvailable`
- old MIDI cache arrays.

Problem:

- These names imply a held voice cache will be promoted into live DSP.
- Temp-background loading now wants file bytes stored into STM normal endpoint
  images while temp continues sounding.

Refactor target:

- Fold direct/legacy voice-load promotion into the unified background-file-load
  finalization path.
- Rename ingress flags around "file payload receiving" and "background-load
  pending".
- Remove temp-background dependency on old unhold/release-cache semantics.

### Endpoint restore queue API

Current state:

- Full and masked endpoint restore work but are implemented inside sequencer
  code.

Refactor target:

- Move restore queue/handshake/rate limiting into a preset endpoint or comms
  service.
- API should be:
  - restore full selected image;
  - restore selected voice mask;
  - restore global/shared params as needed.

### Transport reliability remains unresolved

Current state:

- Original comms audit risks remain:
  - 256-byte FIFOs;
  - silent overflow;
  - blocking waits;
  - AVR atomic send deadlock risk;
  - no packet length/CRC/sequence numbers.

Refactor target:

- Do not mix this with the preset/morph move unless needed.
- When background loading is automated more deeply, revisit flow-control and
  integrity checks as a separate comms hardening pass.
