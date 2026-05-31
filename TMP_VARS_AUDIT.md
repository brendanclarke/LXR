# LXR Parameter Storage & Synchronization Audit (2026-05-31)

This document provides a comprehensive technical reference for the symmetric kit data model and the multi-stage communication handshakes used to synchronize sound parameters between the STM32 (Audio Engine) and ATmega (Front Panel/Menu).

---

## 1. Technical Architecture & Data Model (STM32)

To ensure robust capture and restoration of sound states, the system uses identical `SeqKitState` structures for both the active pattern set and the temporary pattern slot.

### **Struct: `SeqKitState`**
Defined in `sequencer.h`, this struct encapsulates all parameters required to reconstruct a kit's sound and its menu state.

| Member | Type | Purpose |
|--------|------|---------|
| `frontPanelParams[]` | `uint8_t[310]` | Stores the **raw menu endpoints**. These values are pushed to the AVR to update the display. |
| `frontPanelParamsValid[]` | `uint8_t[310]` | Validity mask for the front panel endpoints. |
| `morphParams[]` | `uint8_t[310]` | Stores the **morph parameter endpoints**. |
| `morphParamsValid[]` | `uint8_t[310]` | Validity mask for the morph parameter endpoints. |
| `interpolatedParams[]` | `uint8_t[310]` | Stores the **active current-play** sound image. This is the result of morphing and is used by the sound engine for audio generation. |
| `interpolatedParamsValid[]` | `uint8_t[310]` | Validity mask for the interpolated image. |
| `frontPanelAutomationTargets` | `SeqKitAutomationTargets` | Resolved automation targets corresponding to kit/front endpoint bytes. |
| `morphParameterEndpointAutomationTargets` | `SeqKitAutomationTargets` | Resolved automation targets corresponding to morph automation target endpoint bytes. |
| `interpolatedAutomationTargets` | `SeqKitAutomationTargets` | Resolved automation targets for the active current-play image. |
| `valid` | `uint8_t` | Flag indicating if the kit data is fully captured and ready for use. |

### **Struct: `SeqKitAutomationTargets`**
Stores resolved modulation destination indices and validity masks.

- `lfoDestination[6]` (uint16_t) + `lfoDestinationValid` (bitmask)
- `velocityDestination[6]` (uint16_t) + `velocityDestinationValid` (bitmask)
- `macroDestination[4]` (uint16_t) + `macroDestinationValid` (bitmask)

### **Global Instances**
- **`seq_normalKitState`**: Holds the state for the normal pattern set (SEQ1-8).
- **`seq_tmpKitState`**: Holds the state for the temporary sequence slot (SEQ16).

---

## 2. Communication Protocols & Handshakes

The MCUs use specialized opcodes and blocking handshakes to ensure that parameter dumps happen atomically and without creating feedback loops.

### **A. STM-to-AVR Restore (Menu Sync)**
Used to update the AVR menu when switching patterns. Triggered in `sequencer.c:seq_setTmpKitActive()`.

**Handshake Sequence:**
1.  **BEGIN (0xc7)**: STM signals the start of a dump.
2.  **READY (0xc8)**: AVR sets `frontParser_restoreActive = 1`, suppresses its own outbound `MIDI_CC` traffic, and acknowledges readiness.
3.  **DATA (0xc1-0xc4)**: STM iterates through parameters.
    - Uses `PRF_RESTORE_PARAM_CC/CC2` for `parameter_values[]`.
    - Uses `PRF_RESTORE_MORPH_CC/CC2` for `parameters2[]`.
4.  **DONE (0xc6)**: STM signals completion. AVR triggers `menu_repaintAll()`.
5.  **ACK (0xc9)**: AVR acknowledges finish. STM resumes normal operation; AVR clears `frontParser_restoreActive`.

### **B. AVR-to-STM Dump (Endpoint Collection)**
Used to capture raw menu endpoints when copying to the temp slot. Triggered in `copyClearTools.c:copyClear_copyPattern()`.

**Handshake Sequence:**
1.  **ENDPOINT_BEGIN (0x65)**: AVR signals start via `SEQ_CC`.
2.  **Redirection**: STM parser sets `seq_paramIngressTarget = SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT` and zeroes out the `seq_normalKitState` buffers.
3.  **DATA (0xc1-0xc4)**: AVR sends its raw `parameter_values[]` and `parameters2[]` arrays.
4.  **ENDPOINT_END (0x66)**: AVR signals completion. STM restores ingress target to `SEQ_PARAM_INGRESS_CURRENT_IMAGE`.

---

## 3. Parameter Mapping & Offset Logic

A critical discovery during Session 002 was the requirement for an index offset to align the STM's internal sound image with the AVR's menu structure.

- **AVR Menu Index**: 0-based index used to address `parameter_values[]`.
- **STM Canonical Index**: 1-based index (+1) used in the sound engine.
- **Indices < 2**: STM internal placeholders (e.g., volume) that do not map to voice parameters.

| Direction | Action | Logic |
|-----------|--------|-------|
| **AVR -> STM (Ingress)** | Offset applied by STM parser | `paramNr = data1 + 1` (for CC < 128) |
| **STM -> AVR (Egress)** | Offset applied by STM pusher | `send(paramNr - 1)` (for CC < 128) |

---

## 4. Verified Data Flow Scenarios

### **Scenario: Copy Normal -> Temp (SEQ16)**
1.  **AVR**: Dumps raw menu endpoints (`parameter_values`, `parameters2`, mod targets) to STM.
2.  **STM**: Stores these in `seq_normalKitState.frontPanelParams` etc.
3.  **STM**: Snapshots the current live morphed sound into `seq_tmpKitState.interpolatedParams`.
4.  **STM**: **Zeroes out** `seq_tmpKitState.frontPanelParams` (the "test baseline" behavior).

### **Scenario: Pattern Switch (SEQ1-8 <-> SEQ16)**
- **Enter Temp**: STM applies `seq_tmpKitState.interpolatedParams` (Audio stays same). STM pushes Zeros to AVR (Menu clears).
- **Return Normal**: STM applies `seq_normalKitState.interpolatedParams` (Audio restores). STM pushes captured Endpoints to AVR (Menu restores).

---

## 5. Resolved Historical Failures

- **Deadlocks**: Fixed by calling `uart_processFront()` within STM blocking wait loops to allow the parser to see the READY/ACK bytes.
- **Feedback Loops**: Fixed by the `frontParser_restoreActive` flag in the AVR's `frontPanel_sendData()`.
- **Landings**: Fixed the oscillator coarse/fine landing issue by applying the `-1` egress offset.
- **RAM Bloat**: Squashed `frontParser_prfCacheLiveParams/Morph` arrays, freeing ~1.5 KB on the AVR.

---

## 6. Implementation Status (2026-05-31)

- [x] Symmetric `SeqKitState` structures implemented.
- [x] 5-Phase pushback handshake verified.
- [x] AVR egress dump verified.
- [x] Correct index offsets verified on hardware.
- [x] Sound vs. Menu isolation verified (Audio remains stable while menu zero-fills).

---

## 7. LCD Side-Effect Audit (2026-05-31)

Purpose: inspect recently touched parameter-transfer code for unrequested LCD output or debug display side effects. This pass is documentation-only; no runtime code was changed.

### Findings

1. **Confirmed unwanted restore LCD output in committed `HEAD`**
   - `front/LxrAvr/frontPanelParser.c` in `HEAD` actively writes restore status text during the STM-to-AVR restore handshake:
     - `PARAM_RESTORE_BEGIN`: `lcd_setcursor(0,0); lcd_string_F(PSTR("RESTORE BEGIN   "));`
     - `PARAM_RESTORE_DONE`: builds `DONE %d` with `sprintf`, then calls `lcd_setcursor(0,0); lcd_string(text);`
   - These LCD writes are unrelated to the handshake mechanics. The required behavior is `frontParser_restoreActive`, parameter array updates, `menu_repaintAll()`, and READY/ACK transport. The extra LCD messages can visibly overwrite normal menu content during pattern/temp transitions and should be treated as debug cruft unless explicitly requested.

2. **Current working tree has restore LCD writes commented out, but not cleanly removed**
   - Local edits currently comment out the `RESTORE BEGIN` and restore-completion LCD calls in `front/LxrAvr/frontPanelParser.c`.
   - However, the same local edit leaves `frontParser_restoreMorphCount` plus increments in the morph restore handlers. That counter is now only referenced by a commented-out `sprintf("M%d D%d ...")` debug line, so it is non-functional debug state and should be removed in any cleanup pass.

3. **`LCD_PRINT_SCREEN` is not a new restore-path regression**
   - The `LCD_PRINT_SCREEN` handler that prints `cortex says` existed before the recent parameter-transfer work (`git show 90d3f08:front/LxrAvr/frontPanelParser.c` already contains it).
   - The current working tree comments it out. That may be a separate cleanup decision, but it is not evidence of newly introduced restore-handshake LCD output.

4. **`preset_showLoadingPerf()` creates an extra active LCD write relative to checkpoint `90d3f08`**
   - `front/LxrAvr/Preset/presetManager.c` now has a helper that clears the LCD and prints `Loading Perf`.
   - The old `Loading Perf` display already existed later in `preset_loadPerf()` at checkpoint `90d3f08`; the newer code calls the helper earlier as well, before `SEQ_FILE_BEGIN`, and again at the older display point.
   - This is active LCD behavior in recently touched loader code. It is less clearly wrong than the restore debug text because `Loading Perf` is historical, but the added earlier call is still an extra display side effect and should be reviewed before keeping.

### Cleanup Recommendation

- Remove all restore-handshake LCD debug output from `frontPanelParser.c`; keep only the protocol state changes, array writes, `menu_repaintAll()`, and ACK/READY messages.
- Remove `frontParser_restoreMorphCount` if no active diagnostic output is explicitly requested.
- Review whether the earlier `preset_showLoadingPerf()` call is needed. If not explicitly needed, keep only the historical display timing or remove the helper entirely.
- Do not add LCD status messages to transfer/restore paths unless explicitly requested for a hardware-debug session.

### Hard Rule for Next Refactor Pass

No extraneous LCD writes should occur during any copy/paste execution path or any pattern-change path. This applies to normal patterns, SEQ16 temporary pattern entry/exit, copy-to-temp, copy-from-temp, and all related testing/debug variants. Pattern changes and copy/paste operations may update the menu through the normal repaint mechanisms already required by the UI state, but they must not print standalone debug/status strings such as `RESTORE BEGIN`, `DONE`, counters, dump summaries, or temporary transfer labels.

Current spot-check status:
- `front/LxrAvr/frontPanelParser.c`: restore-handshake debug LCD writes are commented out in the working tree; committed `HEAD` still contains active `RESTORE BEGIN` / `DONE %d` writes.
- `front/LxrAvr/frontPanelParser.c`: the SEQ pattern-change handler currently has no direct active LCD debug writes in the inspected block.
- `front/LxrAvr/Menu/copyClearTools.c`: `copyClear_copyPattern()`, `copyClear_copyTrack()`, and `copyClear_copyTrackPattern()` currently have no direct active LCD writes. The active LCD calls in this file are the clear-confirmation prompt (`clear [...]?`), which is separate from copy/paste execution.
- `front/LxrAvr/frontPanelParser.c`: `frontParser_restoreMorphCount` remains as dead debug cruft in the working tree and should be nuked during cleanup alongside commented LCD debug lines.

Flag for later cleanup: accumulated commented LCD/debug fragments and dead counters should be removed in one deliberate refactor pass after background `.PRF` load behavior is working, not piecemeal during the current loader work.

---

## 8. Planned Change: Route Live Parameter Ingress by Active Kit (Review Before Code)

### User Goal

During normal operation, incoming parameter edits should be stored in the interpolated/current-play parameter image for the kit that is actually playing:

- If the currently playing pattern is the temporary pattern (`SEQ_TMP_PATTERN`), normal live parameter ingress should update `seq_tmpKitState.interpolatedParams[]` and `seq_tmpKitState.interpolatedParamsValid[]`.
- If the currently playing pattern is one of the normal patterns (`SEQ1-8`), normal live parameter ingress should update `seq_normalKitState.interpolatedParams[]` and `seq_normalKitState.interpolatedParamsValid[]`.

### Current Code Reading

- `seq_storeParameterIngress()` writes through `seq_getIngressParamTarget()` / `seq_getIngressParamValidTarget()`.
- Explicit endpoint dump mode is already separate:
  - `SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT` routes to `seq_normalKitState.frontPanelParams[]`.
  - `SEQ_PARAM_INGRESS_TMP_KIT_STATE` currently routes to `seq_tmpKitState.frontPanelParams[]`.
- The default live mode, `SEQ_PARAM_INGRESS_CURRENT_IMAGE`, currently always resolves to `seq_normalKitState.interpolatedParams[]`.
- Therefore the user's assumption appears correct: normal front-panel/MIDI parameter sends currently land in the normal interpolated image even when the temp kit is active.

### Proposed Implementation Shape

1. Add a small internal helper in `sequencer.c`, for example `seq_getCurrentImageKitState()`, returning:
   - `&seq_tmpKitState` when `seq_tmpKitActive` is true.
   - `&seq_normalKitState` otherwise.

2. Update only the `SEQ_PARAM_INGRESS_CURRENT_IMAGE` branch in the ingress target helpers:
   - `seq_getIngressParamTarget()` should return `seq_getCurrentImageKitState()->interpolatedParams`.
   - `seq_getIngressParamValidTarget()` should return `seq_getCurrentImageKitState()->interpolatedParamsValid`.

3. Do not change explicit endpoint collection modes:
   - `SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT` must continue to fill `seq_normalKitState.frontPanelParams[]` / valid masks during AVR endpoint dumps.
   - Any future `.PRF` background-load mode should be handled explicitly, not by relying on active-pattern routing.

4. Consider, but do not silently fold in unless explicitly approved, the same active-kit routing for automation target ingress:
   - `seq_storeLfoDestinationIngress()`
   - `seq_storeVelocityDestinationIngress()`
   - `seq_storeMacroDestinationIngress()`
   - Pre-implementation default behavior routed those to the old single normal automation struct in `SEQ_PARAM_INGRESS_CURRENT_IMAGE`. If temp-pattern live edits include mod-target changes, those may need the same active-kit split.

5. Add minimal verification by inspection/build first:
   - Confirm all `seq_storeParameterIngress()` callers still compile.
   - Confirm the endpoint dump path still calls `seq_setIngressTarget(SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT)` before `PRF_RESTORE_PARAM_*` payloads and resets to `SEQ_PARAM_INGRESS_CURRENT_IMAGE` on `SEQ_TMP_KIT_ENDPOINT_END`.
   - Build with the normal project command after code is changed.

### Major Risks / Things to Watch

- **Do not route endpoint dumps by active pattern.** The AVR-to-STM dump used when copying to the temp slot intentionally populates normal endpoint storage. A naive change inside `seq_storeParameterIngress()` could break that by sending endpoint dump payloads into the temp interpolated image.
- **`seq_tmpKitActive` vs `seq_activePattern`:** `seq_setTmpKitActive()` owns applying/restoring kit images. Using `seq_tmpKitActive` for storage routing is probably safer than checking `seq_activePattern` directly, because it represents the kit-image switch state rather than just the sequencer pattern number. This should be verified during implementation.
- **Transition timing:** If external MIDI or front-panel edits arrive during the narrow pattern-switch handoff, they should follow the kit state that is active at the moment of ingress. The current blocking restore handshake and AVR suppression reduce front-panel risk, but external MIDI may still be possible.
- **Automation targets may remain asymmetrical if left alone.** The requested change is about interpolated parameter storage, but mod-target ingress currently shares the same ingress target concept. Leaving automation unchanged may be acceptable for the first pass, but it should be a conscious decision.
- **Temp snapshot behavior:** `seq_captureTmpKitState()` snapshots from `seq_normalKitState.interpolatedParams[]`. That is correct for normal-to-temp copy, but if future workflows copy while temp is already active, the snapshot source may need another review.

### Pre-Code Decision Needed

Proceed with the narrow parameter-only route first, or include active-kit routing for live automation target ingress in the same pass?

Answer received: include active-kit routing for live automation target ingress in the same pass as live interpolated parameter routing.

---

## 9. Mini-Audit: Automation Target Storage vs Endpoint / Morph / Interpolated Images

### Question

The front-panel kit endpoints, morph parameter endpoints, and interpolated/current-play parameters may each imply different automation target endpoint assignments. Are storage locations reserved for all three images for both normal-pattern play and temporary-pattern play? Are automation target endpoint assignments received from and sent to the AVR at the correct stages alongside their appropriate parameter arrays?

### Short Answer

Pre-implementation answer: no, not fully. At the time of this mini-audit, the STM32 data model reserved separate byte arrays for all three parameter images per kit, but it only reserved one resolved automation-target struct per kit. See Section 13 for the implemented fix.

### Storage Findings

At audit time, `SeqKitState` had these arrays for both `seq_normalKitState` and `seq_tmpKitState`:

- `frontPanelParams[]` / `frontPanelParamsValid[]`
- `morphParams[]` / `morphParamsValid[]`
- `interpolatedParams[]` / `interpolatedParamsValid[]`

But each kit had only one resolved automation target struct:

- `automation.lfoDestination[6]`
- `automation.velocityDestination[6]`
- `automation.macroDestination[4]`
- corresponding valid masks

That means there is no explicit reserved STM storage for:

- normal front-endpoint automation targets vs normal morph automation target endpoints vs normal interpolated/current-play automation targets;
- temp front-endpoint automation targets vs temp morph automation target endpoints vs temp interpolated/current-play automation targets.

The endpoint byte arrays could contain the menu selector bytes for target parameters (`PAR_TARGET_LFO*`, `PAR_VEL_DEST_*`, `PAR_MAC*_DST*`), but the resolved destinations actually applied to STM modulation nodes were stored only in the single per-kit automation struct.

### Receive-Path Findings

- AVR endpoint dump currently sends:
  - `parameter_values[]` via `PRF_RESTORE_PARAM_CC/CC2`;
  - `parameters2[]` via `PRF_RESTORE_MORPH_CC/CC2`;
  - one set of resolved mod targets via `CC_VELO_TARGET`, `CC_LFO_TARGET`, and `MACRO_CC`.
- That resolved mod-target dump is computed from `parameter_values[]`, not separately from `parameters2[]`.
- STM receives the resolved mod-target messages through:
  - `seq_storeVelocityDestinationIngress()`
  - `seq_storeLfoDestinationIngress()`
  - `seq_storeMacroDestinationIngress()`
- In `SEQ_PARAM_INGRESS_CURRENT_IMAGE`, these previously defaulted to the old single normal automation struct.
- In `SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT`, these previously routed to the old single normal automation struct.
- In `SEQ_PARAM_INGRESS_TMP_KIT_STATE`, these previously routed to the old single temporary automation struct.

Therefore, the current receive path can store one resolved automation assignment set for normal and one for temp, but not one per image type inside each kit.

### Send / Restore Findings

- STM-to-AVR restore currently pushes `frontPanelParams[]` and `morphParams[]` back to the AVR menu via `PRF_RESTORE_PARAM_*` and `PRF_RESTORE_MORPH_*`.
- STM-to-AVR restore does not send a separate resolved automation-target stream (`CC_LFO_TARGET`, `CC_VELO_TARGET`, `MACRO_CC`) back to the AVR during `seq_pushKitEndpointsToFront()`.
- This is probably acceptable for display restoration, because the AVR menu stores selector bytes in `parameter_values[]` and `parameters2[]`.
- It is not sufficient if the AVR needs resolved target-side effects during restore, or if a morph automation target endpoint requires a distinct resolved target assignment to be active/applied.

### Morph-Specific Findings

- AVR morph editing stores morph parameter endpoint bytes in `parameters2[]`.
- For morph automation target endpoint edits, the AVR often suppresses direct `CC_LFO_TARGET` / `CC_VELO_TARGET` sends and instead calls `preset_morph(...)`, which sends interpolated parameter values as normal MIDI/CC bytes.
- `preset_morph()` interpolates and sends ordinary parameter bytes, but it does not appear to send the separate resolved target messages (`CC_LFO_TARGET`, `CC_VELO_TARGET`, `MACRO_CC`) for the interpolated/morphed destination selection.
- This means morph automation target endpoint selector bytes can be present in `parameters2[]`, but the resolved STM modulation-node destinations may not be separately captured/applied as a morph parameter endpoint or interpolated target image.

### Implication for the Next Code Change

For the immediate requested change, live `SEQ_PARAM_INGRESS_CURRENT_IMAGE` routing should update both:

- active kit `interpolatedParams[]`;
- active kit `automation`.

That will fix live temp-vs-normal storage for current-play parameter edits and current-play automation target edits.

It will not, by itself, create full three-way automation storage for front endpoint, morph parameter endpoint, and interpolated image. A complete model would require expanding `SeqKitState` to hold distinct automation structs, for example:

- `frontPanelAutomation`
- `morphParameterEndpointAutomation`
- `interpolatedAutomation`

for both normal and temp kits, plus explicit routing rules for endpoint dump, morph parameter endpoint dump, and live/current ingress. That is a larger refactor and should not be folded into the immediate background-load work unless explicitly chosen.

### Recommendation

Superseded by Section 10. The earlier smaller step would have been:

- route live/current parameter ingress by active kit;
- route live/current automation target ingress by active kit;
- leave endpoint-dump routing unchanged;
- document the missing three-image automation model as known technical debt for a later morph-harmony/data-model refactor.

After the follow-up audit, the better first code change is to resolve the three-image automation target storage model before narrowing the live-ingress routing.

### Follow-Up: AVR-Side Morph Automation Target Endpoint Storage

AVR storage for morph automation target endpoint bytes exists only in the broad sense that `parameters2[]` is a second sound-parameter array. It can hold morph parameter endpoint selector bytes for parameters such as `PAR_VEL_DEST_*`, `PAR_TARGET_LFO*`, and possibly macro destination parameters where the UI allows them. There does not appear to be a separate AVR-side resolved automation-target structure for morph automation target endpoints.

Important details from the AVR side:

- `parameter_values[]` is the original/front endpoint byte array.
- `parameters2[]` is the morph parameter endpoint byte array.
- Normal target edits usually send resolved target sideband messages:
  - `CC_VELO_TARGET`
  - `CC_LFO_TARGET`
  - `MACRO_CC`
- Morph automation target endpoint edits generally do not send those same resolved target sideband messages. In the inspected edit paths:
  - when `isMorphParam` is false, target edits send the explicit resolved sideband message;
  - when `isMorphParam` is true, velocity/LFO target edit paths call `preset_morph(...)` rather than sending `CC_VELO_TARGET` / `CC_LFO_TARGET`;
  - `preset_morph(...)` sends interpolated parameter bytes as `MIDI_CC` / `CC_2`, but does not send explicit resolved target sideband messages;
  - one shift-edit path explicitly forces velocity/LFO/macro target parameters to `isMorphParam = 0`, so those target assignments are not morph-edited through that path at all.

Therefore the rule appears to be:

- Original kit automation target assignment is sent when the AVR is editing/loading/dumping `parameter_values[]`, because the relevant code computes a resolved destination from `parameter_values[]` and sends `CC_VELO_TARGET`, `CC_LFO_TARGET`, or `MACRO_CC`.
- Morph automation target endpoint assignment is not consistently sent as a resolved target assignment. It may be stored as a byte in `parameters2[]`, and it may be part of the raw `PRF_RESTORE_MORPH_*` endpoint dump, but the AVR does not appear to compute and send a separate resolved destination from `parameters2[]` at the morph parameter endpoint stage.
- During morph interpolation, the AVR sends interpolated parameter bytes, but that is not equivalent to the explicit resolved target sideband protocol currently used for LFO/velocity/macro destination assignment.

STM-side confirmation:

- The STM receives explicit target sideband messages in `frontPanelParser.c` and applies them with `modNode_setDestination(...)`.
- The STM `MidiParser.c` stores incoming ordinary CC/CC2 bytes via `seq_storeParameterIngress(...)`, but the normal CC/CC2 path does not appear to reconstruct all resolved destination assignments that the sideband messages set. For example, LFO target selector parameters are commented out in `ParameterArray.c`, and target assignment is handled by `FRONT_CC_LFO_TARGET`.

Implication: before resolving the storage model, the firmware needs an explicit choice for automation target semantics:

1. **Endpoint-only semantics:** keep automation target assignments tied to the original/front endpoint, do not interpolate automation target destinations, and document that `parameters2[]` automation target endpoint selector bytes are display/file data only.
2. **Morph automation target endpoint semantics:** add explicit storage and transport for resolved automation targets computed from morph automation target endpoints in `parameters2[]`.
3. **Interpolated/current-play semantics:** compute resolved target assignments from the interpolated/current-play image and store/apply those as the active automation target set.

The current code is a hybrid: original/front endpoint resolved automation targets are handled explicitly; morph automation target endpoints and interpolated/current-play automation target destinations are not fully handled as first-class resolved automation images.

### Follow-Up: AVR Storage Sufficiency for Automation Target Endpoint Bytes

Statement assessed:

> The velocity, LFO automation, and macro modulation targets are contained in `parameter_values[]` and `parameters2[]` in the AVR and no additional storage arrays are necessary to contain the necessary parameters for these targets for the AVR.

Assessment: correct for AVR-side endpoint byte storage, with one important distinction.

- `parameter_values[]` is the AVR's original/front endpoint byte storage.
- `parameters2[]` is the AVR's morph parameter endpoint byte storage.
- Velocity automation target endpoint selectors are represented by `PAR_VEL_DEST_1` through `PAR_VEL_DEST_6`, which are normal parameter indices stored in those arrays.
- LFO automation target endpoint selectors are represented by `PAR_TARGET_LFO1` through `PAR_TARGET_LFO6`, plus related voice-selection parameters, which are also normal parameter indices stored in those arrays.
- Macro modulation target endpoint selectors are represented by `PAR_MAC1_DST1`, `PAR_MAC1_DST2`, `PAR_MAC2_DST1`, and `PAR_MAC2_DST2`, with amount parameters beside them, and are also normal parameter indices that fit inside the existing parameter arrays.

The AVR also has `parameter_values_temp[]` and `parameters2_temp[]`, but these are loader/staging buffers, not a separate canonical target-storage model.

The AVR does not need additional canonical storage arrays merely to remember the endpoint selector bytes for velocity, LFO automation, or macro modulation targets. The existing arrays can hold the required endpoint parameters.

The distinction is resolved destination transport:

- The values stored in `parameter_values[]` / `parameters2[]` are selector bytes, often indices into `modTargets[]`.
- When the AVR needs to inform the STM32 which actual modulation destination to use, it derives a resolved destination from those selector bytes via `modTargets[]` and sends sideband messages such as `CC_VELO_TARGET`, `CC_LFO_TARGET`, and `MACRO_CC`.
- Therefore the missing model is not AVR-side endpoint storage. The missing model is explicit STM-side storage/transport/application of resolved automation target assignments for front endpoint vs morph automation target endpoint vs interpolated/current-play image.

Conclusion: no additional AVR arrays are required for the endpoint parameter values themselves. Any fix should focus on when/how those existing AVR endpoint bytes are translated into resolved target messages and how the STM stores/applies the resulting resolved automation targets.

---

## 10. Planned Storage Model: Three Automation-Target Images per Kit

This section supersedes the earlier recommendation to only route live/current automation target ingress by active kit. The next code change should first resolve the missing three-image automation-target storage model on the STM32.

### Required STM32 Storage

Each `SeqKitState` should carry resolved automation target storage for all three parameter images:

- `frontPanelAutomationTargets`
  - Resolved automation targets corresponding to `frontPanelParams[]`.
  - These are derived from the AVR `parameter_values[]` endpoint bytes.

- `morphParameterEndpointAutomationTargets`
  - Resolved automation targets corresponding to `morphParams[]`.
  - These are derived from the AVR `parameters2[]` morph parameter endpoint bytes.

- `interpolatedAutomationTargets`
  - Resolved automation targets corresponding to `interpolatedParams[]`.
  - These represent the current-play automation target assignments actually used by the sound engine.

Because there are two kit states, this creates six resolved automation target images:

- `seq_normalKitState.frontPanelAutomationTargets`
- `seq_normalKitState.morphParameterEndpointAutomationTargets`
- `seq_normalKitState.interpolatedAutomationTargets`
- `seq_tmpKitState.frontPanelAutomationTargets`
- `seq_tmpKitState.morphParameterEndpointAutomationTargets`
- `seq_tmpKitState.interpolatedAutomationTargets`

Implemented in Section 13: the payload shape is now named `SeqKitAutomationTargets`.

Each resolved automation target image should contain this payload shape:

- `lfoDestination[6]`
- `velocityDestination[6]`
- `macroDestination[4]`
- corresponding valid masks for those three arrays

### AVR Enum Parameters That Define Automation Target Endpoints

These are endpoint parameter bytes already stored in AVR `parameter_values[]` and `parameters2[]`. They are the bytes that must travel with the corresponding endpoint image.

Velocity automation target endpoint parameters:

- `PAR_VEL_DEST_1`
- `PAR_VEL_DEST_2`
- `PAR_VEL_DEST_3`
- `PAR_VEL_DEST_4`
- `PAR_VEL_DEST_5`
- `PAR_VEL_DEST_6`

LFO automation target endpoint parameters:

- `PAR_VOICE_LFO1`
- `PAR_VOICE_LFO2`
- `PAR_VOICE_LFO3`
- `PAR_VOICE_LFO4`
- `PAR_VOICE_LFO5`
- `PAR_VOICE_LFO6`
- `PAR_TARGET_LFO1`
- `PAR_TARGET_LFO2`
- `PAR_TARGET_LFO3`
- `PAR_TARGET_LFO4`
- `PAR_TARGET_LFO5`
- `PAR_TARGET_LFO6`

Macro modulation target endpoint parameters:

- `PAR_MAC1_DST1`
- `PAR_MAC1_DST2`
- `PAR_MAC2_DST1`
- `PAR_MAC2_DST2`

Adjacent macro amount parameters are not automation target endpoints, but they remain ordinary parameters in the byte arrays and must continue to travel with those arrays:

- `PAR_MAC1_DST1_AMT`
- `PAR_MAC1_DST2_AMT`
- `PAR_MAC2_DST1_AMT`
- `PAR_MAC2_DST2_AMT`

### Operation: Copy Normal Pattern to Temporary Pattern

Trigger path:

- AVR: `copyClear_copyPattern()`
- AVR: `preset_dumpNormalEndpointsToStm()`
- STM: `FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN` / payload / `FRONT_SEQ_TMP_KIT_ENDPOINT_END`
- STM: `seq_copyPattern()` then `seq_captureTmpKitState()`

AVR to STM storage during endpoint dump:

- AVR `parameter_values[]` should be received into `seq_normalKitState.frontPanelParams[]`.
- Automation target endpoint bytes listed above that arrive via `PRF_RESTORE_PARAM_CC/CC2` should therefore be stored in `seq_normalKitState.frontPanelParams[]`.
- Resolved automation targets computed from AVR `parameter_values[]` should be received/stored into `seq_normalKitState.frontPanelAutomationTargets`.

- AVR `parameters2[]` should be received into `seq_normalKitState.morphParams[]`.
- Automation target endpoint bytes listed above that arrive via `PRF_RESTORE_MORPH_CC/CC2` should therefore be stored in `seq_normalKitState.morphParams[]`.
- Resolved automation targets computed from AVR `parameters2[]` should be received/stored into `seq_normalKitState.morphParameterEndpointAutomationTargets`.

STM local snapshot during `seq_captureTmpKitState()`:

- Current-play sound parameter bytes should be snapshotted into `seq_tmpKitState.interpolatedParams[]`.
- Current-play resolved automation targets should be snapshotted into `seq_tmpKitState.interpolatedAutomationTargets`.
- This snapshot should represent what is actually sounding at the moment of copy.

Temporary endpoint baseline during the current test phase:

- `seq_tmpKitState.frontPanelParams[]` is intentionally zero-filled for the menu test baseline.
- `seq_tmpKitState.morphParams[]` is intentionally zero-filled for the menu test baseline.
- The corresponding `seq_tmpKitState.frontPanelAutomationTargets` and `seq_tmpKitState.morphParameterEndpointAutomationTargets` should be cleared or marked invalid to match those zero endpoint arrays, unless a later explicit temp-endpoint behavior is chosen.

No AVR-side extra arrays are needed for this operation. The AVR endpoint bytes remain in `parameter_values[]` and `parameters2[]`; the new storage is STM-side resolved automation target storage.

### Operation: Switch Playback to Temporary Pattern

Trigger path:

- STM pattern change sets `seq_activePattern = SEQ_TMP_PATTERN`.
- STM calls `seq_setTmpKitActive(1)`.

STM engine state:

- Apply `seq_tmpKitState.interpolatedParams[]` to the sound engine.
- Apply `seq_tmpKitState.interpolatedAutomationTargets` to the modulation nodes.
- Do not apply `seq_tmpKitState.frontPanelAutomationTargets` or `seq_tmpKitState.morphParameterEndpointAutomationTargets` to the engine during this transition, because those correspond to endpoint/menu images, not the current-play image.

STM to AVR menu restore:

- Send `seq_tmpKitState.frontPanelParams[]` up to AVR `parameter_values[]` using the existing restore payload path.
- The automation target endpoint enum bytes listed above should therefore land in AVR `parameter_values[]` when present/valid.
- Send `seq_tmpKitState.morphParams[]` up to AVR `parameters2[]` using the existing morph parameter endpoint restore payload path.
- The automation target endpoint enum bytes listed above should therefore land in AVR `parameters2[]` when present/valid.

Resolved automation target structs do not need to be sent to the AVR as separate storage. The AVR stores endpoint selector bytes in `parameter_values[]` and `parameters2[]`; resolved destination messages are only needed when the STM must apply destinations.

### Operation: Switch Playback Back to Normal Pattern Set

Trigger path:

- STM pattern change sets `seq_activePattern` to a normal pattern.
- STM calls `seq_setTmpKitActive(0)`.

STM engine state:

- Apply `seq_normalKitState.interpolatedParams[]` to the sound engine.
- Apply `seq_normalKitState.interpolatedAutomationTargets` to the modulation nodes.
- Do not apply `seq_normalKitState.frontPanelAutomationTargets` or `seq_normalKitState.morphParameterEndpointAutomationTargets` to the engine during this transition, unless the desired behavior is explicitly changed to endpoint-only playback.

STM to AVR menu restore:

- Send `seq_normalKitState.frontPanelParams[]` up to AVR `parameter_values[]`.
- The automation target endpoint enum bytes listed above should therefore restore into AVR `parameter_values[]`.
- Send `seq_normalKitState.morphParams[]` up to AVR `parameters2[]`.
- The automation target endpoint enum bytes listed above should therefore restore into AVR `parameters2[]`.

Again, resolved automation target structs do not need to be sent to AVR storage. The AVR's canonical storage remains `parameter_values[]` and `parameters2[]`.

### Protocol Implication

The existing parameter endpoint byte protocol already distinguishes front endpoint bytes from morph parameter endpoint bytes:

- `PRF_RESTORE_PARAM_CC/CC2` maps to `parameter_values[]` / `frontPanelParams[]`.
- `PRF_RESTORE_MORPH_CC/CC2` maps to `parameters2[]` / `morphParams[]`.

Resolved automation target sideband messages currently do not carry enough image identity by themselves. `CC_VELO_TARGET`, `CC_LFO_TARGET`, and `MACRO_CC` need either:

- an explicit surrounding phase/target selector telling STM whether following resolved target messages belong to front endpoint, morph automation target endpoint, or interpolated/current-play image; or
- distinct restore opcodes for each automation-target image.

Without that distinction, adding the STM arrays alone would not be enough; incoming resolved target messages would still be ambiguous.

### Expected Result After This Refactor

- AVR remains responsible for endpoint selector byte storage in `parameter_values[]` and `parameters2[]`.
- STM gains explicit resolved automation target storage for front endpoint, morph parameter endpoint, and interpolated/current-play images for both normal and temporary kit states.
- Copy-to-temp captures normal endpoint and morph parameter endpoint automation assignments separately, then snapshots the current-play automation assignments for the temporary pattern.
- Temp entry applies temp interpolated automation targets to audio while restoring temp endpoint bytes to the AVR menu.
- Return to normal applies normal interpolated automation targets to audio while restoring normal endpoint bytes to the AVR menu.

---

## 11. Implementation Assessment: Endpoint Storage Is Copy-To-Temp Only

The next code phase should treat endpoint storage as an explicit copy-to-temp collection mode, not as a general behavior of live parameter ingress or pattern switching.

Assessment of proposed behavior: correct.

Required storage model:

- Normal kit state must contain three parameter byte images:
  - kit/front endpoint parameters;
  - interpolated/current-play parameters;
  - morph parameter endpoint parameters.
- Normal kit state must contain three corresponding resolved automation target images:
  - kit/front endpoint automation targets;
  - interpolated/current-play automation targets;
  - morph automation target endpoints.
- Temporary kit state must contain the same three parameter byte images and three resolved automation target images.

Allowed non-interpolated storage case:

- The STM should store to kit/front endpoint and morph parameter endpoint locations only while handling the explicit "copy normal pattern to temporary pattern" transfer.
- In that operation, the AVR already sends complete endpoint parameter byte sets:
  - `parameter_values[]` maps to normal kit/front endpoint parameter storage;
  - `parameters2[]` maps to normal morph parameter endpoint storage.
- The automation target endpoint parameters contained in those byte streams must be stored with the same endpoint image as their surrounding parameter bytes.
- Resolved automation target messages received in this copy-to-temp endpoint-transfer context must be routed to the matching endpoint automation target image:
  - resolved targets derived from `parameter_values[]` go to normal kit/front endpoint automation target storage;
  - resolved targets derived from `parameters2[]` go to normal morph automation target endpoint storage.

Copy-to-temp STM behavior:

- Copy normal interpolated/current-play parameter bytes into temporary interpolated/current-play parameter storage.
- Copy normal interpolated/current-play resolved automation targets into temporary interpolated/current-play automation target storage.
- Zero temporary kit/front endpoint parameter bytes and mark/clear their validity according to the existing temp menu-baseline behavior.
- Zero or invalidate temporary kit/front endpoint automation target storage to match the zeroed endpoint bytes.
- Zero temporary morph parameter endpoint bytes and mark/clear their validity according to the existing temp menu-baseline behavior.
- Zero or invalidate temporary morph automation target endpoint storage to match the zeroed endpoint bytes.

Normal live operation:

- Parameter ingress outside the copy-to-temp endpoint-transfer context should store only into the currently active interpolated/current-play parameter image.
- Resolved automation target ingress outside the copy-to-temp endpoint-transfer context should store only into the currently active interpolated/current-play automation target image.
- If normal pattern-set playback is active, this means `seq_normalKitState.interpolatedParams[]` and `seq_normalKitState.interpolatedAutomationTargets`.
- If temporary pattern playback is active, this means `seq_tmpKitState.interpolatedParams[]` and `seq_tmpKitState.interpolatedAutomationTargets`.

Pattern switching:

- Switching playback from the normal pattern set to the temporary pattern should not create new endpoint-storage writes.
- Switching playback from the temporary pattern back to the normal pattern set should not create new endpoint-storage writes.
- Normal playback should apply normal interpolated/current-play parameters and normal interpolated/current-play automation targets.
- Temporary pattern playback should apply temporary interpolated/current-play parameters and temporary interpolated/current-play automation targets.
- Endpoint images may still be restored to the AVR menu through the existing front-panel restore path, but that is UI state restoration, not new STM endpoint capture.

Implementation consequence:

- The STM ingress target state must clearly distinguish ordinary live/current-play ingress from the copy-to-temp endpoint-transfer window.
- The copy-to-temp endpoint-transfer window is the only place where incoming parameter bytes or resolved automation target messages may populate kit/front endpoint or morph parameter endpoint STM storage.
- Any future background `.PRF` load path should get its own explicit storage mode rather than reusing ordinary pattern-switch or live-ingress behavior.

---

## 12. Protocol Bracket Assessment: Existing Phase Markers

Question: do existing messages clearly define bracketed phases for AVR-to-STM endpoint transfer, and for STM-to-AVR endpoint restore during temporary pattern entry/exit?

Short answer:

- There is an existing whole-transfer bracket for AVR-to-STM copy-to-temp endpoint collection.
- There is an existing whole-transfer handshake for STM-to-AVR endpoint restore.
- Raw kit/front endpoint bytes and morph parameter endpoint bytes are distinguished by their payload opcodes.
- There are not currently separate sub-brackets for resolved automation target messages belonging to kit/front endpoint vs morph automation target endpoint images.

### AVR to STM: Copy Normal Pattern to Temporary Pattern

Existing whole-transfer bracket:

- AVR sends `SEQ_CC, SEQ_TMP_KIT_ENDPOINT_BEGIN, 0`.
- STM handles `FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN` by setting ingress to `SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT`, zeroing normal endpoint buffers, and clearing the current single normal automation struct.
- AVR sends endpoint byte payloads.
- AVR sends `SEQ_CC, SEQ_TMP_KIT_ENDPOINT_END, 0`.
- STM handles `FRONT_SEQ_TMP_KIT_ENDPOINT_END` by restoring ingress to `SEQ_PARAM_INGRESS_CURRENT_IMAGE`.

Existing raw endpoint byte distinction inside that bracket:

- `PRF_RESTORE_PARAM_CC` / `PRF_RESTORE_PARAM_CC2` identify kit/front endpoint bytes from AVR `parameter_values[]`.
- `PRF_RESTORE_MORPH_CC` / `PRF_RESTORE_MORPH_CC2` identify morph parameter endpoint bytes from AVR `parameters2[]`.

That is enough to store raw parameter bytes into separate STM parameter arrays during the copy-to-temp transfer.

Current resolved automation target limitation:

- The current AVR dump sends one resolved automation target sideband stream after both endpoint byte arrays.
- That stream is computed from `parameter_values[]`, not from `parameters2[]`.
- The sideband messages are `CC_VELO_TARGET`, `CC_LFO_TARGET`, and `MACRO_CC`.
- Those sideband opcodes do not themselves indicate whether they belong to kit/front endpoint, morph automation target endpoint, or interpolated/current-play storage.
- Therefore, the existing copy-to-temp bracket identifies "this is an endpoint dump", but it does not by itself identify which endpoint image a resolved automation target sideband belongs to.

Conclusion for AVR-to-STM:

- Existing messages are sufficient for bracketing the full copy-to-temp endpoint transfer.
- Existing payload opcodes are sufficient for separating raw kit/front endpoint bytes from raw morph parameter endpoint bytes.
- Existing messages are not sufficient for independently storing resolved kit/front endpoint automation targets and resolved morph automation target endpoints unless the code adds a reliable subphase rule.

Possible subphase options for the next code pass:

- Add explicit subphase messages around resolved kit/front endpoint automation target sidebands and resolved morph automation target endpoint sidebands.
- Add distinct sideband/restore opcodes for each resolved automation target endpoint image.
- Use strict message ordering only if it is made explicit and documented, for example: parameter_values bytes, resolved kit/front endpoint automation targets, parameters2 bytes, resolved morph automation target endpoints. This would be more fragile than explicit subphase messages because correctness would depend on stream order rather than a named phase.

### STM to AVR: Temporary Pattern Entry/Exit Endpoint Restore

Existing whole-transfer handshake:

- STM sends `PARAM_RESTORE_BEGIN`.
- AVR replies `PARAM_RESTORE_READY`.
- STM sends endpoint byte payloads.
- STM sends `PARAM_RESTORE_DONE`.
- AVR replies `PARAM_RESTORE_ACK`.

Existing raw endpoint byte distinction inside that handshake:

- `PRF_RESTORE_PARAM_CC` / `PRF_RESTORE_PARAM_CC2` restore AVR `parameter_values[]`.
- `PRF_RESTORE_MORPH_CC` / `PRF_RESTORE_MORPH_CC2` restore AVR `parameters2[]`.

Current STM restore behavior:

- `seq_pushKitEndpointsToFront()` sends both kit/front endpoint bytes and morph parameter endpoint bytes within one restore transaction.
- The code currently interleaves kit/front and morph parameter endpoint byte payloads by parameter index.
- There are no separate kit/front endpoint begin/end or morph parameter endpoint begin/end messages inside the restore transaction.

Conclusion for STM-to-AVR:

- Existing messages are sufficient for restoring raw endpoint bytes to the AVR during temporary pattern entry/exit, because each payload opcode tells the AVR which array to update.
- Separate endpoint sub-brackets are not required for the current AVR restore behavior.
- STM-to-AVR restore does not send resolved automation target structs as separate storage, and it does not need to as long as AVR canonical storage remains `parameter_values[]` and `parameters2[]`.

Implementation implication:

- For the next code phase, the copy-to-temp AVR-to-STM path is the only direction that needs new or stricter phase identity for resolved automation target endpoint storage.
- Pattern switching STM-to-AVR restore can keep using the existing `PARAM_RESTORE_BEGIN` / `PARAM_RESTORE_DONE` transaction and `PRF_RESTORE_PARAM_*` / `PRF_RESTORE_MORPH_*` payload split for endpoint byte restoration.

---

## 13. Implementation Pass: Three Automation Target Images + Bracketed Copy Transfer

Status: implemented and build-checked on 2026-05-31.

Files changed:

- `mainboard/LxrStm32/src/Sequencer/sequencer.h`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiMessages.h`
- `front/LxrAvr/Preset/presetManager.c`
- `front/LxrAvr/frontPanelParser.h`

### STM32 Storage Changes

`SeqKitState` now has three resolved automation target images:

- `frontPanelAutomationTargets`
- `morphParameterEndpointAutomationTargets`
- `interpolatedAutomationTargets`

The old single per-kit automation storage field has been removed. The payload type is now named `SeqKitAutomationTargets`.

### AVR-to-STM Copy-To-Temp Bracketing

New sequencer command:

- AVR: `SEQ_TMP_KIT_AUTOMATION_PHASE`
- STM: `FRONT_SEQ_TMP_KIT_AUTOMATION_PHASE`

Phase values:

- `*_AUTOMATION_NONE`
- `*_AUTOMATION_FRONT_ENDPOINT`
- `*_AUTOMATION_MORPH_ENDPOINT`

Implemented copy-to-temp transfer order:

- AVR sends `SEQ_TMP_KIT_ENDPOINT_BEGIN`.
- AVR sends all `parameter_values[]` bytes using `PRF_RESTORE_PARAM_CC/CC2`.
- AVR sends automation phase `FRONT_ENDPOINT`.
- AVR sends resolved velocity, LFO, and macro target sidebands derived from `parameter_values[]`.
- AVR sends all `parameters2[]` bytes using `PRF_RESTORE_MORPH_CC/CC2`.
- AVR sends automation phase `MORPH_ENDPOINT`.
- AVR sends resolved velocity, LFO, and macro target sidebands derived from `parameters2[]`.
- AVR sends automation phase `NONE`.
- AVR sends `SEQ_TMP_KIT_ENDPOINT_END`.

STM storage behavior during this bracket:

- `PRF_RESTORE_PARAM_CC/CC2` stores raw endpoint bytes into `seq_normalKitState.frontPanelParams[]`.
- `PRF_RESTORE_MORPH_CC/CC2` stores raw endpoint bytes into `seq_normalKitState.morphParams[]`.
- Resolved automation target sidebands in `FRONT_ENDPOINT` phase store into `seq_normalKitState.frontPanelAutomationTargets`.
- Resolved automation target sidebands in `MORPH_ENDPOINT` phase store into `seq_normalKitState.morphParameterEndpointAutomationTargets`.
- Resolved automation target sidebands in endpoint-transfer phases are not applied to live modulation nodes.

### Live / Interpolated Routing

Outside the copy-to-temp endpoint-transfer bracket:

- Parameter ingress stores into the currently active interpolated/current-play parameter image.
- Resolved automation target ingress stores into the currently active interpolated/current-play automation target image.
- If normal pattern playback is active, current ingress targets `seq_normalKitState.interpolatedParams[]` and `seq_normalKitState.interpolatedAutomationTargets`.
- If temporary pattern playback is active, current ingress targets `seq_tmpKitState.interpolatedParams[]` and `seq_tmpKitState.interpolatedAutomationTargets`.

### Copy-To-Temp Snapshot Behavior

`seq_captureTmpKitState()` now:

- snapshots normal interpolated/current-play parameter bytes into temporary interpolated/current-play parameter storage;
- snapshots normal interpolated/current-play automation targets into temporary interpolated/current-play automation target storage;
- zeroes temporary kit/front endpoint bytes and marks them valid for the temp menu baseline;
- zeroes temporary morph parameter endpoint bytes and marks them valid for the temp menu baseline;
- clears temporary kit/front endpoint automation target storage;
- clears temporary morph automation target endpoint storage.

### Playback Switching Behavior

Normal-to-temp playback switch:

- Applies `seq_tmpKitState.interpolatedParams[]`.
- Applies `seq_tmpKitState.interpolatedAutomationTargets`.
- Restores temporary endpoint bytes to AVR menu storage through the existing STM-to-AVR restore handshake.

Temp-to-normal playback switch:

- Applies `seq_normalKitState.interpolatedParams[]`.
- Applies `seq_normalKitState.interpolatedAutomationTargets`.
- Restores normal endpoint bytes to AVR menu storage through the existing STM-to-AVR restore handshake.

### STM-to-AVR Restore Rules

The existing restore handshake remains:

- `PARAM_RESTORE_BEGIN`
- `PARAM_RESTORE_READY`
- payload
- `PARAM_RESTORE_DONE`
- `PARAM_RESTORE_ACK`

The payload phase is now explicit in code comments and ordering:

- `PRF_RESTORE_PARAM_CC/CC2` restores AVR `parameter_values[]`.
- `PRF_RESTORE_MORPH_CC/CC2` restores AVR `parameters2[]`.

No resolved automation target structs are sent to AVR as separate storage. The AVR receives the automation target endpoint selector bytes through the correct parameter arrays.

### Build Check

Both firmware sub-builds completed successfully:

- `make -C front/LxrAvr avr`
- `make -C mainboard/LxrStm32 stm32`

Observed warnings appear to be pre-existing project warnings unrelated to this change set.

---

## 14. AVR Menu Fix: SHIFT View/Edit of Automation Target Endpoint Parameters

Status: implemented and AVR build-checked on 2026-05-31.

Problem:

- In single-parameter edit display, the AVR menu only read `parameters2[]` while SHIFT was held when `menu_activePage <= VOICE7_PAGE`.
- That let some voice-page parameters display morph parameter endpoint values, but excluded performance-page macro modulation target endpoint parameters.
- `menu_encoderChangeShiftParameter()` also explicitly forced `isMorphParam = 0` for velocity destination, LFO target, LFO voice selector, and macro destination parameters.
- Result: SHIFT-editing automation target endpoint parameters changed or displayed `parameter_values[]`, not `parameters2[]`, for the exact parameters needed by the morph automation target endpoint model.

Implemented behavior:

- In single-parameter edit display, SHIFT now reads `parameters2[]` for any `parNr < END_OF_SOUND_PARAMETERS`.
- In SHIFT encoder edit mode, any `parNr < END_OF_SOUND_PARAMETERS` now edits `parameters2[]`.
- The old automation-target exceptions in `menu_encoderChangeShiftParameter()` were removed.
- LFO voice selector edits now update the paired `PAR_TARGET_LFO*` value in the same endpoint image:
  - normal edit updates `parameter_values[PAR_TARGET_LFO*]`;
  - SHIFT edit updates `parameters2[PAR_TARGET_LFO*]`.
- LFO target selector range limiting now uses `parameters2[PAR_VOICE_LFO*]` while SHIFT-editing, so morph parameter endpoint voice/target pairing is internally consistent.

Files changed:

- `front/LxrAvr/Menu/menu.c`

Build check:

- `make -C front/LxrAvr avr` completed successfully.

Observed warning:

- Existing `menu_resetActiveParameter()` unsigned conversion warning remains; unrelated to this change.

---

## 15. Open Issue: Live Morph Does Not Resolve Morph Automation Target Endpoints

Status: confirmed by code reading on 2026-05-31. No code change in this section.

Question assessed:

- Do morph automation target endpoint parameters actually affect the resolved automation destinations during live morph interpolation?

Answer:

- Not fully. The AVR can now view/edit/store the morph automation target endpoint selector bytes in `parameters2[]`, and the copy-to-temp endpoint dump can send resolved target images for those bytes.
- However, the normal live morph path does not currently translate interpolated automation target selector bytes into resolved automation destination sideband messages.

Evidence:

- `preset_morph()` interpolates `parameter_values[paramNumber]` and `parameters2[paramNumber]`, then sends the result as ordinary `MIDI_CC` / `CC_2`.
- It does not send `CC_VELO_TARGET`, `CC_LFO_TARGET`, or `MACRO_CC` based on the interpolated selector value.
- Velocity destination selector params are present in the voice masks, so their raw selector byte may be sent during `preset_morph()`, but the explicit resolved velocity target sideband is not sent.
- LFO target selector params are present in the voice masks, but STM `ParameterArray.c` leaves `PAR_VOICE_LFO*` / `PAR_TARGET_LFO*` as commented-out selector params; the actual LFO destination assignment is handled by `CC_LFO_TARGET`.
- Macro modulation target endpoint params are performance-level params, not part of the voice masks used by `preset_morph()`.
- `menu_processSpecialCaseValues()` routes macro morph behavior through `menu_vMorph(...)` using `parameter_values[PAR_MAC*_DST*]`, not `parameters2[PAR_MAC*_DST*]`.
- Kit/voice load paths explicitly send resolved target sidebands from `parameter_values[]`.

Conclusion:

- During ordinary live morph, resolved automation target assignments still effectively follow the kit/front endpoint sideband assignments from `parameter_values[]`.
- The morph automation target endpoint selector bytes in `parameters2[]` are now valid storage/display/copy-transfer data, but they are not yet first-class live morph destination selectors.
- This behavior is intentional for the current firmware state and should be treated as a high-risk invariant.

Danger flag:

- It will be **VERY dangerous** to deviate from this live morph behavior casually.
- Any future change that makes morph automation target endpoints affect live resolved automation destinations must be designed and tested as an explicit behavior change, not smuggled into storage, copy/paste, menu display, or background-load work.
- In particular, do not make `preset_morph()` start sending resolved automation destination sideband messages merely because the endpoint storage model now preserves morph automation target endpoint bytes.

Implication for a future fix:

- If morph automation target endpoints should affect live sound as morph changes, `preset_morph()` or a neighboring helper must compute the resolved destination from the current interpolated selector byte and send the correct sideband message:
  - `CC_VELO_TARGET` for `PAR_VEL_DEST_*`;
  - `CC_LFO_TARGET` for `PAR_TARGET_LFO*` plus the paired `PAR_VOICE_LFO*` context;
  - `MACRO_CC` for `PAR_MAC*_DST*`, if macro destination morph behavior is desired.
- This should be treated as a separate behavior change from the endpoint storage/copy-transfer work.

---

## 16. Implementation Pass: Temporary Pattern Inherits Endpoint Images on Copy

Status: implemented on 2026-05-31.

Goal:

- When copying a pattern from the normal pattern set to the temporary pattern, the STM should preserve the endpoint images it just received from the AVR for both normal and temporary kit states.
- The temporary pattern should no longer get zeroed kit/front endpoint or morph parameter endpoint arrays as a test baseline.

Implemented behavior in `seq_captureTmpKitState()`:

- Temporary interpolated/current-play parameter bytes are still captured from normal interpolated/current-play storage.
- Temporary interpolated/current-play automation targets are still copied from normal interpolated/current-play automation target storage.
- Temporary kit/front endpoint bytes now copy from `seq_normalKitState.frontPanelParams[]`.
- Temporary kit/front endpoint validity now copies from `seq_normalKitState.frontPanelParamsValid[]`.
- Temporary morph parameter endpoint bytes now copy from `seq_normalKitState.morphParams[]`.
- Temporary morph parameter endpoint validity now copies from `seq_normalKitState.morphParamsValid[]`.
- Temporary kit/front endpoint automation targets now copy from `seq_normalKitState.frontPanelAutomationTargets`.
- Temporary morph automation target endpoints now copy from `seq_normalKitState.morphParameterEndpointAutomationTargets`.

Result:

- On copy-to-temp, normal and temporary parameter structs both retain the kit/front endpoint image, morph parameter endpoint image, and their corresponding resolved automation target images received from the AVR.
- Entering the temporary pattern still applies the temporary interpolated/current-play image to sound.
- Entering the temporary pattern now restores the inherited temporary endpoint images to AVR menu storage instead of zero-filled endpoint images.

Files changed:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`

Build check:

- `make -C mainboard/LxrStm32 stm32` completed successfully.

Observed warnings:

- Existing `seq_init()` bounds warnings and linker RWX segment warning remain; unrelated to this change.

---

## 17. Planned Change: File Loads Must Populate Only Normal Parameter Storage

Status: planning note only on 2026-05-31. No code change in this section.

User requirement:

- Any parameter data received from a file load must always land in the normal pattern set parameter storage.
- File-load parameter ingress must never write to temporary parameter storage.
- If the temporary pattern is currently playing, file load operations must produce no audible parameter or automation-target changes at all.
- This applies to raw sound parameters, LFO automation target sidebands, velocity automation target sidebands, and macro modulation target sidebands.

Current risk found by code reading:

- STM parameter ingress currently defaults to `SEQ_PARAM_INGRESS_CURRENT_IMAGE`.
- `SEQ_PARAM_INGRESS_CURRENT_IMAGE` resolves through `seq_getCurrentImageKitState()`.
- Therefore, when `seq_tmpKitActive` is true, current-image ingress writes to `seq_tmpKitState.interpolatedParams[]` and `seq_tmpKitState.interpolatedAutomationTargets`.
- That is correct for live edits while the temporary pattern is the sounding pattern, but it is wrong for file-load streams.
- The `seq_voicesLoading` path stores incoming `MIDI_CC`, `FRONT_CC_2`, `FRONT_CC_LFO_TARGET`, and `FRONT_CC_VELO_TARGET` through the same ingress helpers, so voice-load file data can also be misdirected to temporary storage while the temporary pattern is active.
- `frontParser_uncacheVoice()` later applies cached file-load parameters and cached LFO/velocity automation target destinations to live nodes. If the temporary pattern is playing, that delayed apply would violate the no-audible-change rule unless it is guarded.

Planned storage rule:

- Add an explicit STM ingress target for normal interpolated/current-play storage, distinct from "currently sounding image".
- Working name: `SEQ_PARAM_INGRESS_NORMAL_INTERPOLATED`.
- Under that target:
  - raw parameter bytes store to `seq_normalKitState.interpolatedParams[]`;
  - validity bytes store to `seq_normalKitState.interpolatedParamsValid[]`;
  - LFO automation target sidebands store to `seq_normalKitState.interpolatedAutomationTargets.lfoDestination[]`;
  - velocity automation target sidebands store to `seq_normalKitState.interpolatedAutomationTargets.velocityDestination[]`;
  - macro modulation target sidebands store to `seq_normalKitState.interpolatedAutomationTargets.macroDestination[]`.

Planned bracketing rule:

- On STM receipt of file-load begin messages, force parameter ingress to `SEQ_PARAM_INGRESS_NORMAL_INTERPOLATED`.
- On STM receipt of file-load completion or abort messages, restore parameter ingress to `SEQ_PARAM_INGRESS_CURRENT_IMAGE`.
- The existing candidates to audit/use are:
  - `FRONT_SEQ_FILE_BEGIN`;
  - `FRONT_SEQ_FILE_DONE`;
  - `FRONT_SEQ_LOAD_VOICE`;
  - `FRONT_SEQ_UNHOLD_VOICE`;
  - `FRONT_SEQ_PRF_CACHE_BEGIN`;
  - `FRONT_SEQ_PRF_CACHE_ABORT`;
  - pending/deferred performance-load messages if they carry file parameter payload.
- If these existing messages do not bracket every AVR file-load parameter stream reliably, add a small explicit file-parameter-ingress bracket rather than relying on playback state.

Planned live-apply rule:

- Replace sideband checks that currently use only:
  - `seq_getIngressTarget() == SEQ_PARAM_INGRESS_CURRENT_IMAGE`
- with a helper that means "this ingress is allowed to touch live sound now."
- For ordinary live edits, that helper should still allow current behavior.
- For file-load ingress while the temporary pattern is active, that helper must return false.
- For file-load ingress while the normal pattern set is active, existing audible behavior can remain unless a later requirement says file loads should always be fully backgrounded even during normal playback.

Planned voice-cache rule:

- File-load voice cache data must be treated as normal-pattern-set data.
- While the temporary pattern is active:
  - `frontParser_unholdVoice()` may move cached bytes into the normal kit cache image;
  - `frontParser_uncacheVoice()` must not apply those cached bytes or cached LFO/velocity automation target destinations to live voice nodes.
- When playback returns to the normal pattern set, normal storage and normal cached/applied state should be the source of sound.
- The cache-release path is a major risk area because it can make a file load audible after the original receive bracket has ended.

Non-goals for this change:

- Do not change copy-to-temporary-pattern behavior from section 16.
- Do not change the live morph behavior flagged in section 15.
- Do not add LCD/debug/status writes for this path.
- Do not alter temporary pattern parameter storage during any file load.

Implementation files expected:

- `mainboard/LxrStm32/src/Sequencer/sequencer.h`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
- Possibly `mainboard/LxrStm32/src/MIDI/MidiMessages.h` and AVR message headers only if existing file-load brackets are insufficient.

---

## 18. Implementation Pass: File Loads Target Normal Images Only

Status: implemented on 2026-05-31.

Goal:

- File-load parameter data must always populate the normal pattern set parameter image.
- File-load pattern data must always populate `seq_patternSet`, not `seq_tmpPattern`.
- While the temporary pattern is sounding, file loads must not change the audible temporary sound.

Implemented STM parameter ingress behavior:

- Added `SEQ_PARAM_INGRESS_NORMAL_INTERPOLATED`.
- Under this ingress target:
  - raw parameter bytes store to `seq_normalKitState.interpolatedParams[]`;
  - validity bytes store to `seq_normalKitState.interpolatedParamsValid[]`;
  - LFO automation target sidebands store to `seq_normalKitState.interpolatedAutomationTargets`;
  - velocity automation target sidebands store to `seq_normalKitState.interpolatedAutomationTargets`;
  - macro modulation target sidebands store to `seq_normalKitState.interpolatedAutomationTargets`.
- Added `seq_shouldApplyIngressToLive()`:
  - current-image ingress still applies live;
  - normal-interpolated file-load ingress applies live only when the temporary kit is not active;
  - endpoint ingress remains store-only.
- Added `seq_isTmpKitActive()` for parser-side cache-release decisions.

Implemented STM file-load bracketing behavior:

- `FRONT_SEQ_FILE_BEGIN` now forces normal-interpolated ingress.
- `FRONT_SEQ_FILE_DONE`, `FRONT_SEQ_FLOW_ABORT`, and `FRONT_SEQ_PRF_CACHE_ABORT` restore current-image ingress.
- `FRONT_SEQ_LOAD_VOICE` also forces normal-interpolated ingress so standalone voice-load payloads are captured correctly.
- Deferred performance-load replay is also wrapped in normal-interpolated ingress, so cached file payload does not replay into the temporary image.

Implemented STM no-temp-pattern behavior:

- `frontParser_shouldStagePattern()` now returns false during file-load ingress.
- Result: file-load sysex writes main steps, steps, pattern length, pattern scale, and pattern chain data directly into `seq_patternSet`.
- File-load sysex no longer uses `seq_tmpPattern` as a staging buffer.
- This is intentional per the new rule that file loads should never touch temporary pattern data.

Implemented STM no-audible-temp-change behavior:

- Incoming normal-interpolated file-load parameters are stored only while the temporary kit is active.
- LFO, velocity, and macro destination sidebands are stored only while the temporary kit is active.
- Delayed voice-cache release now calls a wrapper:
  - if the temporary kit is active, cached file-load voice data is cleared without applying to live voice/modulation nodes;
  - otherwise the existing cache apply path is used.

Implemented AVR bracketing behavior:

- Added `SEQ_FILE_BEGIN/WTYPE_KIT` at the start of kit file load after the file is opened and the name is read.
- Added `SEQ_FILE_BEGIN/WTYPE_PATTERN` at the start of pattern file load before pattern sysex payload begins.
- Existing `SEQ_FILE_BEGIN` messages for all/performance loads remain unchanged.
- Existing `SEQ_FILE_DONE` messages close the brackets.

Files changed:

- `mainboard/LxrStm32/src/Sequencer/sequencer.h`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
- `front/LxrAvr/Preset/presetManager.c`

Build check:

- `make -C mainboard/LxrStm32 stm32` completed successfully.
- `make -C front/LxrAvr avr` completed successfully.

Observed warnings:

- STM build still reports pre-existing warnings in mixer, oscillator, modulation node, sequencer init bounds, trigger memset, main exit recursion, MidiParser fallthrough, and frontPanelParser unused/fallthrough areas.
- AVR build still reports the pre-existing `preset_readDrumVoice()` fallthrough warning.

---

## 19. Follow-Up Fix: File Load Still Reached Temporary Morph Parameter Endpoint / Voice Cache

Status: implemented on 2026-05-31.

User observation:

- Loading from file while playing the temporary pattern still produced audible changes.
- Suspicion: temporary kit/front endpoints appeared preserved, but the temporary interpolation set and temporary morph parameter endpoint set seemed to be overwritten.

Code-read result:

- Temporary interpolated parameter storage was not directly overwritten by the new normal-interpolated ingress target.
- `SEQ_PARAM_INGRESS_NORMAL_INTERPOLATED` correctly routes raw parameter bytes to `seq_normalKitState.interpolatedParams[]`.
- However, the morph parameter endpoint receive path still had a stale two-way routing rule:
  - `SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT` -> normal morph parameter endpoint;
  - everything else -> temporary morph parameter endpoint.
- Because file-load ingress now uses `SEQ_PARAM_INGRESS_NORMAL_INTERPOLATED`, any `PRF_RESTORE_MORPH_*` bytes received under that mode would incorrectly land in `seq_tmpKitState.morphParams[]`.

Implemented morph parameter endpoint fix:

- `PRF_RESTORE_MORPH_CC/CC2` now routes to temporary morph parameter endpoint storage only when ingress target is explicitly `SEQ_PARAM_INGRESS_TMP_KIT_STATE`.
- All other ingress targets, including normal kit endpoint and normal-interpolated file-load ingress, route to `seq_normalKitState.morphParams[]`.

Second audible-change source found:

- `frontParser_unholdVoice()` still promoted loaded voice cache into `midi_midiKit[]`, `midi_kitLfoCache[]`, and `midi_kitVeloCache[]`.
- It also set `seq_newVoiceAvailable`.
- While the temporary pattern is active, that can become audible later through trigger-time `frontParser_uncacheVoice()` even if the raw STM parameter bytes were stored in the normal parameter image.

Implemented voice-cache fix:

- Added `frontParser_unholdLoadedVoice()`.
- While the temporary kit is active, loaded voice cache is cleared instead of being promoted into live kit cache or marked for trigger-time apply.
- While the normal kit is active, existing unhold behavior is preserved.
- Deferred voice unhold now uses this wrapper too.

Current expected behavior after this pass:

- File-load raw parameter bytes go to normal interpolated parameter storage.
- File-load morph parameter endpoint bytes go to normal morph parameter endpoint storage.
- File-load voice cache should no longer become audible through unhold or later trigger-time cache apply while the temporary pattern is active.
- Temporary kit/front endpoint storage, temporary interpolated storage, and temporary morph parameter endpoint storage should remain untouched by file-load parameter streams unless the code is in an explicit temporary-pattern copy/switch operation.

Files changed:

- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`

Build check:

- `make -C mainboard/LxrStm32 stm32` completed successfully.

Observed warnings:

- Existing frontPanelParser warnings remain: unused `frontParser_sendPrfRestoreParam`, unused local in `frontParser_sendPrfLiveRestore`, and pre-existing fallthrough warnings.
- Linker RWX segment warning remains.

---

## 20. Planned Change: File Loads Populate Normal Endpoint Groups Explicitly

Status: plan written before code on 2026-05-31.

User requirement:

- No file load may write to the temporary parameter struct.
- No file load may write to temporary pattern data.
- `.all` and `.prf` file loads must populate all three normal parameter groups:
  - normal kit/front endpoint image;
  - normal interpolated/current-play image at the current morph setting;
  - normal morph parameter endpoint image.
- Normal `.kit` file loads must populate:
  - normal kit/front endpoint image;
  - normal interpolated/current-play image at the current morph setting.
- "Load morph" for `.kit` files must populate:
  - normal morph parameter endpoint image;
  - normal interpolated/current-play image at the current morph setting.
- Selective `.kit` endpoint loads must not clear or overwrite the opposite endpoint group.

Current code shape:

- File-load `MIDI_CC` / `CC_2` / sideband traffic now uses `SEQ_PARAM_INGRESS_NORMAL_INTERPOLATED`, so interpolation bytes land in normal interpolated storage.
- File-load pattern sysex no longer stages through `seq_tmpPattern`.
- However, normal endpoint images are not automatically populated by ordinary `preset_morph()` traffic. The AVR needs to explicitly send endpoint bytes to the STM.
- The existing `SEQ_TMP_KIT_ENDPOINT_BEGIN/END` bracket stores raw endpoint bytes in normal endpoint storage on STM, but its current begin handler clears both endpoint groups every time.
- That clear-both behavior is correct for full copy-to-temp endpoint dumps, but too broad for normal `.kit` vs "load morph" `.kit` file loads.

Planned protocol rule:

- Reuse the existing endpoint byte opcodes:
  - `PRF_RESTORE_PARAM_CC/CC2` for kit/front endpoint bytes from `parameter_values[]`;
  - `PRF_RESTORE_MORPH_CC/CC2` for morph parameter endpoint bytes from `parameters2[]`.
- Extend the existing endpoint begin message's data byte into a clear/select mode:
  - full endpoint dump clears and receives both endpoint groups;
  - kit/front-only dump clears and receives only kit/front endpoint storage;
  - morph-only dump clears and receives only morph parameter endpoint storage.
- Keep resolved automation target sidebands bracketed with the existing automation phase byte:
  - kit/front endpoint sidebands use the front endpoint phase;
  - morph automation target endpoint sidebands use the morph automation target endpoint phase.
- On endpoint bracket end during file-load ingress, restore ingress to normal-interpolated file-load mode, not current-image mode.

Planned AVR load behavior:

- `.all` and `.prf`:
  - read both file endpoint source arrays;
  - copy selected voice/meta data into `parameter_values[]` and `parameters2[]` as existing load semantics require;
  - send interpolated/current-play values through existing `preset_morph()` calls;
  - send a full endpoint dump to STM so normal kit/front and normal morph parameter endpoint images are both populated.
- normal `.kit`:
  - read file data into `parameter_values_temp[]`;
  - copy selected voice/meta data into `parameter_values[]`;
  - send interpolated/current-play values through existing `preset_morph()` calls;
  - send only the kit/front endpoint dump to STM.
- "load morph" `.kit`:
  - read file data into `parameters2_temp[]`;
  - copy selected voice/meta data into `parameters2[]`;
  - send interpolated/current-play values through existing `preset_morph()` calls;
  - send only the morph parameter endpoint dump to STM.

Planned AVR meta fix:

- `preset_readDrumsetMeta(1)` currently copies only a few nonvoice morph parameter endpoint bytes.
- For endpoint storage to be complete, it should copy the same nonvoice parameter range into `parameters2[]` that the normal path copies into `parameter_values[]`.
- It should still avoid adding live morph automation destination behavior; endpoint storage is not the same as changing live morph destination resolution.

Non-goals:

- Do not change live morph destination behavior flagged in section 15.
- Do not add LCD/debug/status writes.
- Do not make file loads write temporary parameter storage or temporary pattern storage.

---

## 21. Implementation Pass: File Loads Populate Normal Endpoint Groups

Status: implemented on 2026-05-31.

Implemented protocol behavior:

- Added endpoint dump mode constants on AVR and STM:
  - both endpoint groups;
  - kit/front endpoint only;
  - morph parameter endpoint only.
- The existing endpoint begin message now uses its data byte as the endpoint dump mode.
- STM endpoint begin now clears only the endpoint group selected by the mode.
- STM endpoint end now restores the surrounding ingress context:
  - during file load, it returns to normal-interpolated ingress;
  - outside file load, it returns to current-image ingress.

Implemented AVR endpoint dump behavior:

- `preset_dumpNormalEndpointsToStm()` now calls a shared selective endpoint dump helper with "both endpoint groups" mode.
- The selective helper sends:
  - `PRF_RESTORE_PARAM_CC/CC2` plus front endpoint automation sidebands for kit/front endpoint dumps;
  - `PRF_RESTORE_MORPH_CC/CC2` plus morph automation target endpoint sidebands for morph parameter endpoint dumps.
- Normal `.kit` loads call the helper in kit/front-only mode.
- "Load morph" `.kit` loads call the helper in morph-only mode.
- `.all` and `.prf` loads call the helper in both-endpoints mode.

Implemented AVR morph parameter endpoint meta copy:

- `preset_readDrumsetMeta(1)` now copies the same nonvoice parameter range into `parameters2[]` that the normal path copies into `parameter_values[]`.
- This keeps the morph parameter endpoint image complete for file-load endpoint dumps.
- This is endpoint/menu storage only and does not change live morph destination behavior.

Expected behavior after this pass:

- `.all` and `.prf` file loads populate normal kit/front endpoints, normal morph parameter endpoints, and normal interpolated/current-play storage.
- Normal `.kit` file loads populate normal kit/front endpoints and normal interpolated/current-play storage while preserving the existing normal morph parameter endpoint image.
- "Load morph" `.kit` file loads populate normal morph parameter endpoints and normal interpolated/current-play storage while preserving the existing normal kit/front endpoint image.
- No file-load path should intentionally write the temporary parameter struct or temporary pattern data.

Files changed:

- `front/LxrAvr/frontPanelParser.h`
- `front/LxrAvr/Preset/presetManager.c`
- `mainboard/LxrStm32/src/MIDI/MidiMessages.h`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`

Build check:

- `make -C front/LxrAvr avr` completed successfully.
- `make -C mainboard/LxrStm32 stm32` completed successfully.

Observed warnings:

- AVR build still reports pre-existing warnings in `main.c`, `menu.c`, `buttonHandler.c`, `uart.c`, `presetManager.c`, and `frontPanelParser.c`.
- STM build still reports pre-existing warnings in mixer, oscillator, modulation node, sequencer init bounds, main exit recursion, MidiParser fallthrough, frontPanelParser unused/fallthrough areas, and linker RWX segment warning.

---

## 22. Investigation: Temp Pattern Can Be Active While Temp Kit Image Is Not Active

Status: investigation note only on 2026-05-31. No code change in this section.

User observation:

- Load a `.all` file.
- Switch to the temporary pattern without doing a copy-to-temporary-pattern operation.
- Edit sound parameters in the AVR menu.
- Switch back to the normal pattern set.
- The normal sound parameters have changed, but they should not have changed.

Likely root cause:

- Pattern identity and kit/parameter image identity can get out of sync.
- `FRONT_SEQ_CHANGE_TMP_PAT` calls `seq_setNextPattern(SEQ_TMP_PATTERN, ...)`.
- On pattern execution, `seq_activePattern` is set to `SEQ_TMP_PATTERN`, then `seq_setTmpKitActive(seq_activePattern == SEQ_TMP_PATTERN)` is called.
- `seq_setTmpKitActive(1)` returns immediately if `seq_tmpKitState.valid` is false.
- When it returns early, `seq_activePattern` can still be `SEQ_TMP_PATTERN`, but `seq_tmpKitActive` remains false.
- `seq_getCurrentImageKitState()` uses `seq_tmpKitActive`, not `seq_activePattern`, to choose between normal and temporary parameter images.
- Therefore, in this mismatched state, normal/current-image parameter ingress still points at `seq_normalKitState`, even though the sequencer is playing the temporary pattern number.

Evidence in code:

- `seq_setTmpKitActive(1)` early return condition:
  - `if(seq_tmpKitActive || !seq_tmpKitState.valid) return;`
- `seq_getCurrentImageKitState()`:
  - returns `seq_tmpKitState` only when `seq_tmpKitActive` is true;
  - otherwise returns `seq_normalKitState`.
- Menu parameter edits come through `MIDI_CC` / `FRONT_CC_2` and call `midiParser_ccHandler(..., 1)`.
- `midiParser_ccHandler(..., 1)` calls `seq_storeParameterIngress(...)`.
- Outside file-load or endpoint brackets, ingress target is `SEQ_PARAM_INGRESS_CURRENT_IMAGE`.
- `SEQ_PARAM_INGRESS_CURRENT_IMAGE` writes through `seq_getCurrentImageKitState()`.
- If `seq_tmpKitActive` is false, those menu edits update normal interpolated parameter storage.
- When switching back to the normal pattern set, `seq_setTmpKitActive(0)` also returns early if `seq_tmpKitActive` is already false, so there is no normal-parameter restore event to undo the edits.

Why this appears after `.all` load:

- `.all` load now correctly populates normal kit/front endpoints, normal morph parameter endpoints, and normal interpolated/current-play storage.
- It does not create or validate a temporary kit image.
- `seq_tmpKitState.valid` is only set by `seq_captureTmpKitState()`, which currently happens when copying from a normal pattern to the temporary pattern.
- If the user switches to the temporary pattern without such a copy, the temp pattern number may become active while the temp kit state is invalid.
- If a stale temp kit state exists from an earlier copy, behavior may differ: the stale temp kit may activate. That is a separate risk, but the observed normal-parameter mutation is explained by the invalid-temp-kit case.

Important distinction:

- This is not evidence that file-load parameter streams are directly writing temporary parameter storage.
- It is evidence that the live-edit path can still be normal-current-image while the active pattern number is temporary.
- In other words, the isolation invariant currently depends on `seq_tmpKitActive`, but user-visible temporary-pattern playback can exist without `seq_tmpKitActive`.

Likely fix direction:

- Prevent `SEQ_TMP_PATTERN` playback from entering a half-active state.
- Possible approaches:
  - reject/ignore a switch to `SEQ_TMP_PATTERN` when `seq_tmpKitState.valid` is false;
  - auto-initialize `seq_tmpKitState` from the current normal parameter image before allowing the switch;
  - set a separate "temporary pattern selected but no temp kit" policy and make parameter ingress follow `seq_activePattern`, not `seq_tmpKitActive`.
- The safest behavior depends on desired UX:
  - if switching to temp without copy should be impossible, reject the switch;
  - if switching to temp without copy should create an editable temporary sound sandbox, initialize temp kit state at switch time;
  - if temporary pattern data may be edited without sound-parameter isolation, document that explicitly, but this conflicts with the current user requirement.

Guardrail for the next code change:

- Parameter ingress during temporary pattern playback must not write normal parameter storage, regardless of whether the temporary kit image was created by copy-to-temp or by some future fallback initialization.
- Any fix should preserve the file-load rule: file loads still target normal parameter and pattern storage only, never temporary storage.

---

## 23. Implementation Pass: Lazy Temporary Kit Initialization on Temp Pattern Entry

Status: implemented on 2026-05-31.

Goal:

- Prevent `SEQ_TMP_PATTERN` playback from entering a state where parameter ingress still writes to normal parameter storage.
- Allow switching to the temporary pattern without an explicit copy-to-temporary-pattern operation while still keeping menu edits isolated from the normal parameter image.
- Preserve the rule that file loads never write temporary parameter storage or temporary pattern data.

Implemented behavior:

- `seq_setTmpKitActive(1)` no longer returns early just because `seq_tmpKitState.valid` is false.
- If the temporary kit image is invalid when entering `SEQ_TMP_PATTERN`, STM now calls `seq_captureTmpKitState()` first.
- That lazy capture initializes the temporary parameter struct from the current normal parameter images:
  - temporary interpolated/current-play parameter bytes;
  - temporary interpolated/current-play automation target image;
  - temporary kit/front endpoint bytes and automation target image;
  - temporary morph parameter endpoint bytes and morph automation target endpoint image.
- After lazy initialization, normal temp activation continues:
  - apply temporary interpolated/current-play parameters to DSP;
  - apply temporary interpolated/current-play automation targets;
  - push temporary endpoint images to AVR menu storage;
  - set `seq_tmpKitActive = 1`.

Expected behavior:

- If the user switches to the temporary pattern without copying first, the temporary kit image becomes a sandbox cloned from current normal storage.
- Menu edits during temporary pattern playback should route through `SEQ_PARAM_INGRESS_CURRENT_IMAGE` into `seq_tmpKitState`, because `seq_tmpKitActive` is now true.
- Switching back to a normal pattern should restore normal interpolated/current-play parameters and normal endpoint menu images.
- File loads still populate normal storage only; this lazy temporary capture happens on temp pattern entry, not during file load.

Files changed:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`

Build check:

- `make -C mainboard/LxrStm32 stm32` completed successfully.

Observed warnings:

- Existing sequencer init bounds warnings and linker RWX segment warning remain.

---

## 24. Cross-Image Write Path Audit After Lazy Temp Kit Initialization

Status: investigation note on 2026-05-31. No code change in this section.

Question:

- Are there any remaining cases where a change while playing the temporary pattern can write the normal STM parameter storage struct, other than file load?
- Are there any remaining cases where a change while playing the normal pattern set can write the temporary STM parameter storage struct, other than copy-to-temporary-pattern?

Short answer:

- No confirmed remaining destination-routing bug found in the current code after section 23.
- The normal live-edit path now appears to route to the expected STM parameter image:
  - temporary pattern playback writes the temporary interpolated/current-play parameter image;
  - normal pattern playback writes the normal interpolated/current-play parameter image.

Why the main live-edit path looks isolated:

- Outside file-load and endpoint brackets, parameter ingress target is `SEQ_PARAM_INGRESS_CURRENT_IMAGE`.
- `SEQ_PARAM_INGRESS_CURRENT_IMAGE` resolves through `seq_getCurrentImageKitState()`.
- `seq_getCurrentImageKitState()` chooses `seq_tmpKitState` only when `seq_tmpKitActive` is true, otherwise `seq_normalKitState`.
- Section 23 changed temp pattern entry so `seq_setTmpKitActive(1)` lazily creates a valid temporary kit image and then activates it.
- Therefore the previous bad state, "temporary pattern number is active but `seq_tmpKitActive` is false", should no longer occur for normal temp-pattern entry.

Checked write paths:

- Normal menu parameter edits:
  - `MIDI_CC` and `FRONT_CC_2` call `midiParser_ccHandler(..., 1)` during normal live operation.
  - `midiParser_ccHandler(..., 1)` calls `seq_storeParameterIngress(...)`.
  - With current-image ingress, that stores to normal or temporary interpolated/current-play storage according to `seq_tmpKitActive`.
- External/live MIDI parameter changes:
  - `MidiParser.c` also stores through `seq_storeParameterIngress(...)` when `updateOriginalValue` is true.
  - That uses the same current-image routing.
- LFO automation target, velocity automation target, and macro modulation target assignment changes:
  - `FRONT_CC_LFO_TARGET`, `FRONT_CC_VELO_TARGET`, and `FRONT_CC_MACRO_TARGET` store through `seq_storeLfoDestinationIngress(...)`, `seq_storeVelocityDestinationIngress(...)`, and `seq_storeMacroDestinationIngress(...)`.
  - Their storage destination comes from `seq_getIngressAutomationTarget()`.
  - In current-image mode, that resolves to the active image's interpolated/current-play automation target image.
- Endpoint transfer traffic:
  - `PRF_RESTORE_PARAM_*` uses the current explicit ingress target.
  - `PRF_RESTORE_MORPH_*` stores morph parameter endpoint bytes into the explicitly selected endpoint image.
  - Endpoint brackets intentionally select normal endpoint storage for normal/file endpoint dumps.
  - The only intended normal-to-temporary parameter image copy remains `seq_captureTmpKitState()`, which is the copy-to-temporary-pattern operation and the lazy temp initialization path from section 23.
- Switching between normal and temporary playback:
  - `seq_applyParameterValues(...)` calls `midiParser_ccHandler(..., 0)`.
  - Because `updateOriginalValue` is zero, this applies sound values without writing the STM normal or temporary parameter storage structs.
  - Automation target application also applies modulation destinations from the chosen image without copying between normal and temporary storage.
- File load:
  - Still explicitly excluded from this question.
  - Current file-load ingress is forced to `SEQ_PARAM_INGRESS_NORMAL_INTERPOLATED`, and file-load pattern staging is blocked from `seq_tmpPattern`.

Residual caveat:

- `frontParser_uncacheVoice()` can apply delayed voice cache entries later by calling `midiParser_ccHandler(..., 1)`.
- That delayed apply still stores to the current image at the time it is applied, so it does not appear to create a direct normal/temp destination-routing bug.
- However, stale cache data is a data-source risk: if a future path leaves old cache entries marked available, those values could be applied to whichever image is active later.
- The current file-load guards try to avoid this while the temporary kit is active by clearing cached loaded voice data instead of applying it.
- Future refactor rule: any new delayed cache, unhold, or trigger-time parameter apply path must either clear stale file-load/cache data on image changes or prove that it stores only to the intended current image.

AVR-side note:

- `parameter_values[]` and `parameters2[]` on the AVR remain global menu/source arrays, not separate normal/temporary STM storage structs.
- STM pushes the selected endpoint image back to those AVR arrays when switching normal/temporary playback so the menu view follows the selected STM image.
- This AVR global-array behavior is expected and is separate from the STM storage isolation requirement audited here.

Conclusion:

- With the section 23 lazy temp initialization in place, I do not currently see another ordinary live-edit case that should cross-write between `seq_normalKitState` and `seq_tmpKitState`.
- The remaining place to stay suspicious is delayed cached voice data, not because its destination is wrong today, but because stale cache availability could make the right destination receive old data.

---

## 25. Audit: Chirp/Glitch When Switching Normal and Temporary Parameter Images

Status: investigation/theory note on 2026-05-31. No code change in this section.

Question:

- Why is there often an audible chirp/glitch when switching back and forth between the temporary pattern and the normal pattern set?
- How might it be suppressed?
- Could `frontParser_uncacheVoice()` play a useful role?

Current switch sequence:

- Pattern switching is handled inside `seq_nextStep()`.
- When the pending pattern becomes active, STM calls:
  - `seq_activePattern = seq_normalizePatternNumber(seq_pendingPattern);`
  - `seq_setTmpKitActive(seq_activePattern == SEQ_TMP_PATTERN);`
  - `seq_newPatternExecuted = 1;`
- `seq_setTmpKitActive(1)` applies the temporary interpolated/current-play parameter image with `seq_applyParameterValues(...)`, then applies the temporary interpolated/current-play automation target image.
- `seq_setTmpKitActive(0)` applies the normal interpolated/current-play parameter image, then applies the normal interpolated/current-play automation target image.
- After returning from `seq_setTmpKitActive(...)`, the pattern-change block calls `voiceControl_noteOff(0xFF)`.

Primary chirp theory:

- The full parameter image is being applied while one-shot drum voices may still be sounding.
- `seq_applyParameterValues(...)` sends many parameter messages through `midiParser_ccHandler(..., 0)`.
- The CC handlers directly touch live synthesis state:
  - oscillator coarse/fine tuning;
  - oscillator wave selection;
  - filter frequency and filter drive;
  - envelope positions and envelope-related values;
  - LFO frequency, amount, phase/sync/retrigger values;
  - modulation destinations and macro destination behavior in related paths.
- Abruptly changing those values on an already-decaying drum voice can create a discontinuity that sounds like a chirp/click/zap.
- `voiceControl_noteOff(0xFF)` currently happens after the parameter image switch, so it cannot protect the currently sounding audio from the parameter discontinuity.

Important note about `voiceControl_noteOff(0xFF)`:

- In `MidiVoiceControl.c`, all-voice note-off clears the active MIDI voice bitmap and sends MIDI note-off.
- It does not appear to hard-kill or ramp down the internal drum synthesis voices.
- Therefore, simply moving all-voice note-off earlier may help external MIDI bookkeeping, but it probably will not fully suppress the internal chirp unless paired with an actual internal voice mute/ramp/kill mechanism.

Secondary chirp theory:

- Switching images currently reapplies every valid parameter, not only changed parameters.
- Even if the temporary image and normal image contain the same value for a parameter, sending that value again may still have an audible side effect because many handlers are imperative live-state setters, not harmless assignments.
- This means a chirp can happen even when the apparent parameter values are the same, especially after file loads or lazy temporary initialization have marked large portions of an image valid.

Automation target contribution:

- `seq_applyAutomationTargets(...)` immediately calls `modNode_setDestination(...)` and `modNode_updateValue(...)` for valid LFO, velocity, and macro destinations.
- Retargeting a modulation node while audio is sounding can also cause a sudden jump if the previous and new destinations or last modulation values differ.
- Even if the destination values match, the update call may still write into live parameter destinations.

Could `frontParser_uncacheVoice()` help?

- Yes, conceptually, but it should not be reused blindly.
- `frontParser_uncacheVoice(voice)` already implements a useful pattern:
  - cached per-voice parameter messages are applied immediately before a voice is triggered;
  - `seq_triggerVoice()` checks `seq_newVoiceAvailable` and calls `frontParser_uncacheVoice(...)` before `voiceControl_noteOn(...)`;
  - this means parameter changes can be moved to a moment where the voice is about to start, instead of being applied globally while the old voice tail is still sounding.
- A temp/normal image switch could use a similar strategy:
  - do not bulk-apply all voice parameters at pattern switch time;
  - stage the destination image's per-voice parameters into a switch cache;
  - mark affected voices in `seq_newVoiceAvailable`;
  - apply each voice's parameter subset through the existing trigger-time path before that voice's next hit.
- This could suppress chirps caused by changing per-voice oscillator/filter/envelope state during the previous voice tail.

Risks if using `frontParser_uncacheVoice()` directly:

- It currently reads from the global `midi_midiCache[]`, `midi_midiLfoCache[]`, and `midi_midiVeloCache[]` file/voice-load cache machinery.
- Reusing that machinery for normal/temp image switching could collide with file-load cache state unless the ownership rules are made very explicit.
- It applies only the voice parameter masks and cached LFO/velocity destinations. It does not naturally cover every global/shared parameter or macro destination assignment.
- It calls `midiParser_ccHandler(..., 1)`, which writes through current-image ingress. That is correct only if the current image has already been selected deliberately.
- The hihat voice mapping has special `voice == 6` to `voice--` behavior and shared bit handling; any new switch-cache scheme must preserve that.

Safer suppression options:

- Option A: Delta apply.
  - Track the currently applied live parameter image.
  - On normal/temp switch, send only parameters whose values or valid bits differ from the current live image.
  - This is likely low-risk and may reduce chirps from redundant setter side effects.
  - It will not fully suppress legitimate large parameter changes while a voice tail is sounding.
- Option B: Defer per-voice parameters to next trigger.
  - Keep global image selection immediately, but stage voice-local parameters for trigger-time application using a `frontParser_uncacheVoice()`-like path.
  - This is likely the most musically clean approach for drum voices.
  - It requires separating voice-local parameters from global/shared parameters and automation target sidebands.
- Option C: Add a very short internal audio mute/ramp around bulk image apply.
  - A tiny DSP output ramp-down, apply, then ramp-up could hide discontinuities.
  - This may be simpler than full per-voice deferral, but it could still be audible as a dip and must not disturb timing.
- Option D: Reorder and quiet.
  - Move any actual internal voice quieting before parameter apply.
  - Current `voiceControl_noteOff(0xFF)` is probably insufficient for internal audio, so this would need a real synth-voice quiet/ramp mechanism, not just MIDI note-off bookkeeping.
- Option E: Hybrid.
  - Use delta apply for global/shared parameters and automation target sidebands.
  - Use trigger-time deferral for voice-local oscillator/filter/envelope/LFO parameters.
  - Use a short ramp only for the few remaining global operations that are still discontinuous.

Recommended direction:

- First implement or audit a live-image delta layer so identical values are not resent on every temp/normal switch.
- Then consider a dedicated temp/normal switch cache modeled after `frontParser_uncacheVoice()`, rather than overloading the file-load cache directly.
- The dedicated cache should make ownership explicit:
  - source is `seq_normalKitState.interpolatedParams` or `seq_tmpKitState.interpolatedParams`;
  - destination is live synth state at next trigger;
  - storage structs are not modified by the delayed apply;
  - file-load cache flags are not reused unless fully cleared and bracketed.

Current conclusion:

- The chirp is most likely not an LCD/UART/display problem.
- It is most likely caused by live DSP parameter discontinuities during `seq_setTmpKitActive(...)`, amplified by the fact that the code reapplies whole valid images rather than changed values only.
- `frontParser_uncacheVoice()` is relevant as a proven trigger-time application pattern, but a direct reuse would be risky. A dedicated switch-time voice-parameter cache inspired by it looks safer.

---

## 26. Follow-Up: Can STM Know a Voice Is Off From Envelope State?

Status: investigation note on 2026-05-31. No code change in this section.

Short answer:

- There is not currently a clean public STM helper that says "this internal synth voice is off because its decay envelope has finished."
- There is enough DSP-side state to build one.

Existing MIDI/note bookkeeping:

- `voiceControl_isVoicePlaying(voice)` exists in `MidiVoiceControl.c`.
- It reads the `active_voices` bitmap set by `voiceControl_noteOn(...)` and cleared by `voiceControl_noteOff(...)`.
- This is not a reliable decay-envelope-tail detector.
- `voiceControl_noteOff(0xFF)` clears the bitmap and sends MIDI note-off, but does not prove the internal drum audio tail is silent.

Existing envelope state:

- The main per-voice amplitude envelope type is `SlopeEg2`.
- `SlopeEg2` stores:
  - `value`;
  - `state`;
  - `EG_STOPPED`, `EG_A`, `EG_D`, and `EG_REPEAT` phase constants.
- `slopeEg2_calc(...)` returns zero when stopped and drives `value` down to zero in decay.
- It does not currently set `state = EG_STOPPED` when the decay reaches zero; it sets `value = 0` and returns zero.

Where voice code already observes silence:

- Drum voices calculate `voiceArray[voiceNr].ampFilterInput = slopeEg2_calc(&voiceArray[voiceNr].oscVolEg)`.
- Snare/cymbal/hihat calculate `egValueOscVol = slopeEg2_calc(&...oscVolEg)`.
- In trigger gate mode only, these voice calculators check the calculated amplitude value for zero and then turn the trigger off and call `voiceControl_noteOff(...)`.
- That means there is already local "envelope has reached zero" knowledge, but it is used only inside voice render/update code and only for trigger gate cleanup.

Useful implementation implication:

- A STM-side quiet-check helper is feasible and should probably read DSP amplitude envelope state directly, not `voiceControl_isVoicePlaying(...)`.
- A simple version could consider a voice quiet when its amplitude envelope value/output is at or below a small threshold.
- For drums 1-3, use `voiceArray[i].oscVolEg.value` or the smoothed `ampFilterInput`/`volEgValueBlock` depending on whether the goal is envelope-quiet or actual output-quiet.
- For snare/cymbal/hihat, use `snareVoice.oscVolEg.value`, `cymbalVoice.oscVolEg.value`, and `hatVoice.oscVolEg.value`, or their calculated `egValueOscVol`.

Caution:

- "Envelope value is zero" is not identical to "all audible output is zero."
- Transient generators and filters may have their own residual state.
- If the goal is to avoid chirps from applying voice parameters, amplitude-envelope quiet is still probably the right first gate.
- If the goal is absolute click-free audio silence, a tiny output ramp/mute is still stronger.

Design note for chirp suppression:

- A switch-time deferral scheme could keep per-voice parameter changes pending until that voice's amplitude envelope is quiet or until the next trigger.
- `frontParser_uncacheVoice()` remains useful as a trigger-time model, but the quiet test should come from DSP envelope state, not from the MIDI active-voice bitmap.

---

## 27. Assessment: Per-Voice Deferred Interpolated Parameter Source Switching

Status: design assessment on 2026-05-31. No code change in this section.

Proposed behavior:

- Replace the single audible interpolated-parameter source decision with a six-slot STM-side state array, one slot per synth voice.
- Use four states:
  - state 1: voice plays from normal interpolated parameters;
  - state 2: voice currently plays from normal interpolated parameters, switch to temporary interpolated parameters at next opportunity;
  - state 3: voice plays from temporary interpolated parameters;
  - state 4: voice currently plays from temporary interpolated parameters, switch to normal interpolated parameters at next opportunity.
- On playback switch to the temporary pattern:
  - voices currently sounding move to state 2;
  - voices already quiet move to state 3 and may receive temporary interpolated voice parameters immediately.
- On playback switch back to the normal pattern set:
  - voices currently sounding move to state 4;
  - voices already quiet move to state 1 and may receive normal interpolated voice parameters immediately.
- "Next opportunity" means:
  - the voice reaches the existing decay-complete condition used by the trigger gate cleanup code;
  - or the voice is about to retrigger, in which case pending source parameters are applied immediately before `voiceControl_noteOn(...)`.

Assessment:

- This is a good direction for suppressing chirps caused by changing oscillator/filter/envelope/LFO voice parameters while a previous drum tail is still audible.
- It can be implemented STM-side only.
- It should not require changing kit/front endpoint storage, morph parameter endpoint storage, or AVR endpoint push-up behavior.
- It should treat `seq_tmpKitActive` as the current pattern/edit/storage image selector, not as proof that every synth voice is already sounding from that image.
- A new per-synth-voice audible-source state array should decide which interpolated voice parameter image has actually been applied to each synth voice.

Important voice-count issue:

- The sequencer has seven trigger tracks/voice numbers, but the sound engine has six synth parameter voices.
- Tracks 5 and 6 are closed/open hihat triggers sharing the same hihat synth parameter set.
- Existing code already maps `voice == 6` to voice slot 5 in `frontParser_getVoicePresetMask(...)` and `frontParser_uncacheVoice(...)`.
- Therefore the proposed array of six is correct for synth parameter source state.
- Any trigger-time "next opportunity" from sequencer voice 5 or 6 must resolve to synth voice slot 5.

Important trigger gate wording:

- The useful detection is the same envelope-zero condition used inside the trigger gate cleanup blocks.
- It should not require trigger gate mode itself to be enabled.
- If the implementation literally waits only when `trigger_isGateModeOn()` is true, the deferral scheme will fail in normal trigger pulse mode.
- The helper should reuse the same local facts:
  - drums 1-3: `ampFilterInput` / amplitude envelope output reaches zero;
  - snare/cymbal/hihat: `egValueOscVol` / amplitude envelope output reaches zero.

Where the "voice became off" event can come from:

- Current decay-complete checks are local to:
  - `calcDrumVoiceAsync(...)`;
  - `Snare_calcAsync(...)`;
  - `Cymbal_calcAsync(...)`;
  - `HiHat_calcAsync(...)`.
- Those functions already check the envelope output for zero when trigger gate mode is active.
- A clean implementation can add a lightweight helper/event there, for example "notify sequencer that synth voice slot N is envelope-quiet."
- The heavy work of applying pending parameters should probably not be done directly in the DSP/audio calculation path.
- Safer pattern:
  - voice DSP code sets a small pending/off flag;
  - sequencer/main-side code notices the flag and applies the pending source image for that voice outside the tight audio calculation path.

Trigger-time application:

- `seq_triggerVoice(...)` is the right second "next opportunity."
- It already calls `frontParser_uncacheVoice(...)` before `voiceControl_noteOn(...)` when `seq_newVoiceAvailable` is set.
- A dedicated normal/temp source switch apply should happen at the same conceptual point:
  - normalize sequencer voice 6 to synth voice slot 5;
  - if that synth voice is in state 2, apply temporary interpolated voice parameters and set state 3;
  - if that synth voice is in state 4, apply normal interpolated voice parameters and set state 1;
  - then continue to `voiceControl_noteOn(...)`.

Do not directly overload file-load voice cache:

- `frontParser_uncacheVoice(...)` is still a useful model, but its backing storage is the existing file/voice-load cache machinery.
- For normal/temp source switching, a dedicated apply function is safer:
  - source should be `seq_normalKitState.interpolatedParams` or `seq_tmpKitState.interpolatedParams`;
  - parameter list should be the existing per-voice preset masks;
  - `midiParser_ccHandler(..., 0)` should be used so applying the audible source does not rewrite STM storage;
  - file-load cache flags should not be reused.

Automation target handling:

- Per-voice LFO automation target and velocity automation target assignments can follow the same six-slot audible-source state as voice parameters.
- Macro modulation targets are global/shared and do not fit a six-voice state model.
- A policy is needed for global/shared interpolated parameters and global/shared automation target assignments:
  - apply immediately, preferably with delta checks;
  - defer until all six synth voices are quiet;
  - or keep current immediate behavior and accept that some global changes may still chirp.
- This is the biggest unresolved design point before implementation.

Global/nonvoice parameter issue:

- Existing voice masks cover the six synth voices, but not every interpolated parameter.
- Some parameters are kit/global/shared rather than per-voice.
- The per-voice source array should not silently ignore those, but it also cannot assign them independently per voice.
- Recommended first pass:
  - implement per-voice deferred apply only for parameters covered by the six voice masks;
  - keep global/shared interpolated parameters on immediate apply with delta suppression;
  - document any remaining chirp as likely coming from global/shared changes or automation target sidebands.

Endpoint/AVR behavior:

- Kit/front endpoint bytes, morph parameter endpoint bytes, and AVR `parameter_values[]` / `parameters2[]` push-up should remain tied to the selected pattern image, as now.
- The proposed four-state array is only about when interpolated/current-play voice parameters are applied to the live synth voices.
- This distinction is important: the menu may show the selected image immediately while an old voice tail continues using previously applied voice parameters until its next opportunity.

Implementation risk summary:

- Low conceptual risk for per-voice oscillator/filter/envelope parameters.
- Medium risk around hihat track 5/6 mapping and ensuring both triggers share synth voice slot 5 state.
- Medium risk around doing too much work in DSP/audio calculation code; prefer event flags.
- Medium/high risk around global/shared interpolated parameters and macro modulation targets because they do not map cleanly to six independent voice slots.

Conclusion:

- The proposed design is sound for voice-local interpolated parameters and is likely a better chirp-suppression strategy than bulk image apply.
- Before code, choose the explicit policy for global/shared interpolated parameters and macro modulation targets.
- The trigger gate code should supply the envelope-zero definition of "off," but the implementation should use that definition regardless of whether trigger gate mode is enabled.

---

## 28. Revised Assessment: No DSP Off Detection, One-Bar Stale Timeout

Status: design assessment on 2026-05-31. No code change in this section.

User refinement:

- Do not key the deferred switch on DSP envelope state or trigger gate cleanup.
- If a synth voice stays in stale state 2 or 4 for more than one full bar without retriggering, switch it blindly at that point.
- Therefore, the code only needs:
  - the per-synth-voice source state;
  - trigger-time source apply;
  - a future sequencer-position timeout for stale states.
- Changing either hihat track into or out of the temporary pattern should force both hihat tracks to change together because they share synth voice slot 5.

Revised state behavior:

- The six-slot state array remains the right model:
  - state 1: synth voice plays from normal interpolated voice parameters;
  - state 2: synth voice still plays from normal interpolated voice parameters, pending switch to temporary;
  - state 3: synth voice plays from temporary interpolated voice parameters;
  - state 4: synth voice still plays from temporary interpolated voice parameters, pending switch to normal.
- Because there is no off detection, pattern-source changes should not attempt to classify voices as currently sounding or quiet.
- On an affected normal-to-temporary source transition:
  - state 1 becomes state 2;
  - state 2 stays state 2;
  - state 3 stays state 3;
  - state 4 should become state 2 or resolve according to the newest requested direction.
- On an affected temporary-to-normal source transition:
  - state 3 becomes state 4;
  - state 4 stays state 4;
  - state 1 stays state 1;
  - state 2 should become state 4 or resolve according to the newest requested direction.
- The pending state resolves at the first of:
  - the synth voice is about to be retriggered;
  - the stale timeout reaches one full bar.

One-bar timeout plan:

- `seq_masterStepCnt` is a `uint8_t` and advances once per sequencer step.
- One full 128-step bar can be represented modulo-256 as `start + 128`.
- For each synth voice, store a stale-start or stale-deadline marker when entering state 2 or 4.
- A robust check can use unsigned modulo arithmetic, for example "elapsed since stale start is at least 128 steps."
- This avoids relying on `seq_barCounter`, which is reset or altered by stop/start and some pattern-change paths.
- This also avoids trying to infer audio silence.

Trigger-time resolution:

- `seq_triggerVoice(...)` remains the correct immediate changeover point.
- Before `voiceControl_noteOn(...)`, normalize the sequencer voice number to synth voice slot:
  - sequencer voices 0-4 map directly;
  - sequencer voices 5 and 6 both map to synth voice slot 5.
- If the synth voice state is 2:
  - apply the temporary interpolated voice-parameter subset for that synth voice;
  - apply per-voice temporary LFO and velocity automation target assignments;
  - set state 3.
- If the synth voice state is 4:
  - apply the normal interpolated voice-parameter subset for that synth voice;
  - apply per-voice normal LFO and velocity automation target assignments;
  - set state 1.
- Use `midiParser_ccHandler(..., 0)` for parameter application so the live apply does not rewrite STM storage.

Timeout resolution:

- Once per sequencer step is enough to check stale states.
- If state 2 has been stale for one full bar:
  - apply the temporary interpolated voice-parameter subset;
  - apply the temporary per-voice LFO and velocity automation target assignments;
  - set state 3.
- If state 4 has been stale for one full bar:
  - apply the normal interpolated voice-parameter subset;
  - apply the normal per-voice LFO and velocity automation target assignments;
  - set state 1.
- This blind timeout may still change a long-decaying sound, but only after a full bar. It is simpler and avoids touching DSP voice/off logic.

Global/shared parameter policy:

- User agreed with the earlier recommendation:
  - per-voice masked interpolated parameters defer by the six-slot source-state array;
  - global/shared interpolated parameters apply immediately;
  - global/shared immediate apply should use delta suppression where practical.
- Macro modulation targets remain global/shared and should not be driven by the six-slot voice state.
- Per-voice LFO automation target and velocity automation target assignments can follow the voice-state deferral.

Hihat special case:

- The sequencer has seven tracks, but tracks 5 and 6 share the same hihat synth voice and the same voice parameter mask.
- A temporary/normal source split between tracks 5 and 6 is impossible at the synth-voice level.
- Therefore, when either hihat track is changed into or out of the temporary pattern, both hihat tracks should be treated as changing image together.
- This can be special-cased around per-track pattern changes:
  - if voice 5 is switched across the normal/temporary boundary, also switch voice 6 pending/active pattern source consistently;
  - if voice 6 is switched across the normal/temporary boundary, also switch voice 5 pending/active pattern source consistently.
- This special case should only be required for transitions into or out of `SEQ_TMP_PATTERN`; ordinary per-track normal-pattern changes can retain existing behavior unless code review shows the same shared-hihat conflict there.

Important interaction with current global temp activation:

- Current `seq_setTmpKitActive(...)` bulk-applies the full selected interpolated image.
- The deferred design requires changing that behavior:
  - endpoint push-up to AVR still happens immediately as now;
  - `seq_tmpKitActive` still changes immediately for edit/storage routing;
  - global/shared interpolated parameters may apply immediately;
  - voice-local interpolated parameters should not bulk-apply for voices entering pending state 2 or 4.
- In other words, selected image and per-voice audible image are intentionally allowed to differ temporarily.

Assessment:

- This refinement simplifies implementation by removing DSP/off detection.
- It also removes the risk of doing parameter work in audio calculation functions.
- The main remaining risks are:
  - correctly splitting voice-local masked parameters from global/shared parameters;
  - correctly pairing hihat tracks on normal/temporary boundary changes;
  - making timeout math robust across normal bar changes, instant pattern changes, and stop/start.

Conclusion:

- The revised plan is coherent and ready to become a code plan.
- Use a six-slot state array plus one-bar stale timers.
- Resolve pending states either at trigger-time or when the stale timer expires.
- Do not use DSP envelope state, trigger gate state, or MIDI active-voice bitmap to decide whether a voice is off.

---

## 29. Implementation Pass: Deferred Per-Voice Interpolated Source Switching

Status: implemented on 2026-05-31.

Implemented STM-side structure:

- Added a six-slot synth voice source state array in `sequencer.c`.
- Added a one-bar stale timer per synth voice using `seq_masterStepCnt` modulo arithmetic.
- Added local sequencer-owned copies of the six existing voice parameter masks.
- Added helper logic to normalize sequencer trigger voices 5 and 6 to shared hihat synth voice slot 5.

Implemented source apply split:

- Voice-local interpolated parameters are applied from the six voice masks only.
- Voice-local apply uses `midiParser_ccHandler(..., 0)` so applying the audible source does not rewrite STM parameter storage.
- Per-voice LFO automation target and velocity automation target assignments follow the same deferred voice source apply.
- Shared/global interpolated parameters are applied immediately on normal/temporary image selection.
- Macro modulation target assignments are treated as shared/global and apply immediately with the selected image.

Implemented switch behavior:

- `seq_setTmpKitActive(...)` no longer bulk-applies voice-local interpolated parameters.
- It still:
  - lazily creates the temporary parameter image when needed;
  - applies shared/global interpolated parameters;
  - applies macro modulation target assignments;
  - pushes kit/front endpoint and morph parameter endpoint images to the AVR as before.
- Pattern changes compare old per-track active patterns to new per-track active patterns and mark affected synth voices pending:
  - normal to temporary becomes state 2;
  - temporary to normal becomes state 4.
- Pending states resolve:
  - immediately before `voiceControl_noteOn(...)` in `seq_triggerVoice(...)`;
  - or after one full bar if no retrigger occurs.

Hihat special case:

- `seq_setNextPattern(...)` now treats normal/temporary boundary changes for hihat tracks 5 and 6 as a pair.
- If either hihat track is switched into or out of `SEQ_TMP_PATTERN`, both hihat tracks receive the same pending pattern source.
- Ordinary normal-to-normal per-track changes are left alone unless they cross the temporary-pattern boundary.

Additional edge case handled:

- Per-track temporary playback can request temporary voice parameters while global `seq_activePattern` remains a normal pattern.
- In that case, `seq_setTmpKitActive(1)` is not called.
- The new pending voice-source logic therefore also lazily creates `seq_tmpKitState` before any deferred per-voice apply from the temporary image.

Build check:

- `make -B -C mainboard/LxrStm32 stm32` completed successfully.

Observed warnings:

- Existing STM warnings remain in USB core, mixer, oscillator, modulation node, sequencer init bounds, Euklid generator, FATFS, TriggerOut, main exit recursion, system clock indentation, MidiParser fallthrough, frontPanelParser unused/fallthrough areas, and linker RWX segment warning.

Runtime testing should focus on:
  - full normal-to-temporary and temporary-to-normal switching;
  - per-track temporary switching for tracks 0-4;
  - hihat track 5/6 temporary boundary switching;
  - a voice that does not retrigger for more than one bar after switching;
  - file load while temporary playback is active, to make sure normal-only file-load storage remains intact.

---

## 30. Implementation Pass: Endpoint Menu Sync for Per-Track Temp Boundary Changes

Status: implemented on 2026-05-31.

Goal:

- When an individual track changes between the normal pattern set and the temporary pattern, the AVR menu endpoint arrays must follow that track's audible/source side.
- This applies to both:
  - kit/front endpoint parameters;
  - morph parameter endpoint parameters.
- This does not change the deferred live-audio interpolated parameter switching from section 29.

Implemented behavior:

- After per-track active patterns are committed, STM compares old per-track pattern sources against new per-track pattern sources.
- For synth voices that crossed the normal/temporary boundary, STM sends the endpoint parameters covered by that synth voice's mask back to AVR:
  - `PRF_RESTORE_PARAM_*` for kit/front endpoint bytes;
  - `PRF_RESTORE_MORPH_*` for morph parameter endpoint bytes.
- The same `PARAM_RESTORE_BEGIN/READY/DONE/ACK` handshake is used for the partial endpoint push.
- The local six voice masks are reused so the endpoint push matches the deferred voice-local interpolated parameter split.

Full-image collapse rule:

- If the per-track change leaves all six synth voices on the temporary side:
  - state 2 or state 3 for every synth voice;
  - STM sends the full temporary kit/front endpoint and morph parameter endpoint images to AVR.
- If the per-track change leaves all six synth voices on the normal side:
  - state 1 or state 4 for every synth voice;
  - STM sends the full normal kit/front endpoint and morph parameter endpoint images to AVR.
- This prevents global/shared and nonvoice endpoint values from staying stale after the last mixed-source voice crosses the boundary.

Mixed-source behavior:

- While normal and temporary source voices are mixed, only changed synth voice endpoint masks are pushed.
- This keeps global/shared endpoint values unchanged until the source set fully collapses to one side.
- Hihat remains paired by the existing section 29 special case: tracks 5 and 6 share synth voice slot 5, so either hihat track crossing the boundary pushes the hihat endpoint mask for the selected side.

Global active-pattern guard:

- Full normal-to-temporary or temporary-to-normal active-pattern switches already call `seq_setTmpKitActive(...)`, which performs a full endpoint push.
- The new endpoint sync is therefore enabled for per-track boundary changes and skipped for the global active-pattern switch path to avoid redundant full pushes.

Files changed:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`

Build check:

- `make -C mainboard/LxrStm32 stm32` completed successfully.

Observed warnings:

- Existing sequencer init bounds warnings and linker RWX segment warning remain.

---

## 31. Implementation Pass: Voice-Mask Offset Fix and Temp-Playback File-Load Menu Preserve

Status: implemented on 2026-05-31; bench retest needed.

Masked per-track endpoint resend finding:

- The local STM voice masks are copied from the AVR/menu parameter numbers.
- For low CC sound parameters, AVR/menu indices are one lower than the STM canonical parameter image.
- Full normal/temporary endpoint pushes walk the STM canonical arrays directly, so they use the correct offset path.
- Mixed per-track endpoint pushes were using raw mask entries directly against STM storage and then sending them through the canonical restore helper. That can read/push the slot one parameter low for low CC entries.
- Fix: all voice-mask users now canonicalize low mask entries with `+1` before checking STM validity arrays, applying interpolated voice parameters, or sending masked kit/front endpoint and morph parameter endpoint bytes back to AVR. CC2 entries are left unchanged.

Scope of the offset fix:

- `seq_isVoiceParameter(...)` now compares canonicalized mask entries.
- Deferred per-voice interpolated parameter apply now reads canonicalized STM storage slots.
- Per-track masked kit/front endpoint resend now reads canonicalized STM storage slots and lets the existing restore sender subtract the wire offset once.
- Per-track masked morph parameter endpoint resend uses the same canonicalized path.

Temp-playback `.all` / `.prf` menu preservation:

- When `.all` or `.prf` is loaded while `menu_playedPattern == SEQ_TMP_PATTERN`, AVR saves the current endpoint ranges of `parameter_values[]` and `parameters2[]`.
- The load still reads file endpoint data and still dumps those loaded endpoint images to STM normal parameter storage.
- Immediately after the STM endpoint dump, AVR restores the saved `parameter_values[]` and `parameters2[]` endpoint ranges so the menu continues to represent the currently-playing temporary parameter set.
- Cleanup restore also runs on early exit if the save already happened.

Important expected behavior:

- Loading `.all` / `.prf` during temporary playback may update normal storage and normal pattern data, but must not replace the AVR menu's active temp kit/front endpoint or morph parameter endpoint arrays.
- Full normal/temporary pattern switching remains the path that intentionally refreshes the menu from the selected full endpoint image.

---

## 32. Investigation: AVR-Down Traffic During Normal/Temporary Pattern Switches

Status: investigation note only on 2026-06-01. No code change in this section.

Question:

- During a pattern switch into or out of `SEQ_TMP_PATTERN`, is anything coming back down from AVR that can touch sound parameters?
- This matters because the audible chirp may not be caused by the deferred per-voice source states alone.

Plain pattern-switch request path:

- User pattern switch from AVR sends only:
  - `SEQ_CC / SEQ_CHANGE_PAT` for normal pattern set;
  - `SEQ_CC / SEQ_CHANGE_TMP_PAT` for the temporary pattern.
- STM receives those in `frontParser_handleSeqCC()` and calls `seq_setNextPattern(...)`.
- That request path does not itself send parameter CCs, automation target assignments, macro destination assignments, or endpoint data down from AVR.

STM switch execution path:

- STM commits the pending pattern in `seq_nextStep()`.
- For full normal/temporary active-pattern switches, STM calls `seq_setTmpKitActive(...)`.
- Current behavior there:
  - applies shared/global interpolated parameters and macro modulation target assignments immediately;
  - marks voice-local interpolated parameter source changes through the six voice-source states;
  - pushes full kit/front endpoint and morph parameter endpoint images up to AVR for menu storage.
- The endpoint push is STM-to-AVR, not AVR-to-STM parameter authority.
- During `PARAM_RESTORE_BEGIN` / `PARAM_RESTORE_DONE`, AVR suppresses ordinary outbound traffic in `frontPanel_sendData(...)`; only `PARAM_RESTORE_READY` and `PARAM_RESTORE_ACK` are allowed back down.
- Therefore the endpoint menu restore should not feed restored menu bytes back down as live parameter edits.

Confirmed AVR-down messages after STM pattern-change ACK:

- STM sends `SEQ_CC / SEQ_CHANGE_PAT` back to AVR as the pattern-change ACK.
- AVR `frontPanelParser.c` handles that ACK.
- If follow mode is enabled and the active page permits following, AVR calls `menu_setShownPattern(patMsg)`.
- `menu_setShownPattern(...)` sends `SEQ_CC / SEQ_SET_SHOWN_PATTERN` back down to STM.
- STM handles `SEQ_SET_SHOWN_PATTERN` as:
  - if the requested shown pattern is different, update `frontParser_shownPattern`;
  - if the requested shown pattern is already shown, call `seq_realign()`.
- `seq_realign()` adjusts per-track step indices, clears track rotation, sends a rotation update to AVR, and calls `seq_midiNoteOff(0xff)`.

Risk from ACK/follow path:

- This is not a parameter resend, but it is an AVR-triggered STM state/audio event after the pattern switch.
- It can occur when AVR already considers the target pattern to be the shown pattern. The temporary pattern is a likely case if the user selected/viewed the temporary pattern before switching playback to it.
- Because `seq_nextStep()` already calls `seq_realign()` after every pattern change, the ACK/follow path can cause a second realign/note-off close to the same switch.
- This is a stronger chirp suspect than an AVR parameter dump in the ordinary switch path.

Other possible parameter-affecting path:

- AVR ACK handling contains an older load-completion path guarded by `preset_workingVoiceArray`.
- If `preset_workingVoiceArray` is nonzero when a `SEQ_CHANGE_PAT` ACK arrives, AVR calls `preset_readDrumVoice(...)` for selected voices.
- `preset_readDrumVoice(...)` sends `SEQ_LOAD_VOICE`, voice parameter CCs through `preset_morph(...)`, automation target sidebands, and `SEQ_UNHOLD_VOICE`.
- This is a real parameter-down path, but it should only be active as part of a pending file-load handoff. It should not fire on ordinary normal/temporary pattern switching when `preset_workingVoiceArray == 0`.

Voice cache / uncache path:

- `seq_triggerVoice()` can call `frontParser_uncacheVoice(...)` only when `seq_newVoiceAvailable` has a bit set for that voice.
- Those bits are set by file-load voice unhold/cache paths and patch reset, not by ordinary temp/normal pattern selection.
- Therefore trigger-time uncache is not expected during a clean pattern switch unless a previous load/cache path left pending voice data.
- If it does fire, it can apply cached voice parameters through `midiParser_ccHandler(..., 1)` and affect sound/storage. That is a stale-cache/file-load issue, not an inherent temp/normal switch requirement.

Assessment about states 2 and 4:

- I did not find ordinary AVR-down sound-parameter traffic that states 2 and 4 are protecting against.
- States 2 and 4 currently protect against switching voice-local interpolated parameters mid-tail by delaying the apply until trigger or stale timeout.
- If the chirp is caused by the trigger-time parameter apply itself, dropping states 2 and 4 may be reasonable, but it should be treated as an audio-behavior simplification, not as a fix for AVR parameter resend.
- The higher-priority suspect for "something coming back down from AVR" is `SEQ_SET_SHOWN_PATTERN -> seq_realign() -> seq_midiNoteOff(0xff)`, especially when the temporary pattern is already the viewed pattern.

Suggested next refactor target:

- Prevent the AVR follow/display ACK path from causing `seq_realign()` during a pattern-change acknowledgement.
- Pattern switching already realigns on STM; display synchronization should update `frontParser_shownPattern` and request display data without a second audio/timing realign.
- Separately, consider whether per-voice source states can collapse to normal/temp only after the realign/note-off double-touch is removed or disabled.

---

## 33. Implementation Pass: Collapse Voice Source States and Suppress Temp-Boundary Realign

Status: implemented on 2026-06-01; bench retest needed.

Goal:

- Test whether the audio chirp is caused or aggravated by trigger-time voice source apply and/or extra realign behavior.
- Keep the useful part of the split source model:
  - each synth voice can still source interpolated parameters from normal or temporary storage independently;
  - hihat remains a shared synth voice source for tracks 5 and 6.
- Drop the stale transition states:
  - no state 2;
  - no state 4;
  - no one-bar stale timer;
  - no source apply on trigger.

Implemented voice-source behavior:

- The six-slot source array is now two-state only:
  - `SEQ_VOICE_SOURCE_NORMAL`;
  - `SEQ_VOICE_SOURCE_TMP`.
- When a track crosses the normal/temporary boundary, its synth voice immediately applies the selected interpolated voice parameters and voice-local automation target assignments.
- Per-track mixed normal/temporary playback still works because the source decision is per synth voice.
- Full-image collapse rules for endpoint menu sync now check only all-normal or all-temporary source states.

Implemented realign behavior:

- `seq_nextStep()` now detects whether the committed pattern change crosses the temporary-pattern boundary:
  - full active-pattern normal-to-temporary or temporary-to-normal;
  - individual track normal-to-temporary or temporary-to-normal;
  - hihat paired boundary changes through the existing track 5/6 special case.
- If the committed switch crosses that boundary, STM skips the usual `seq_realign()` at the end of the pattern-change block.
- STM also sets a one-shot flag for the following AVR ACK/follow message.
- When AVR follow/display sends `SEQ_SET_SHOWN_PATTERN` back down after that temp-boundary switch, STM consumes the one-shot flag and suppresses the duplicate `seq_realign()` that would otherwise happen if the shown pattern already matched.

Expected test behavior:

- Switching into or out of the temporary pattern should perform the parameter source swap on STM without the deferred trigger-time source apply.
- Switching into or out of the temporary pattern should not perform either of the two realign calls identified in section 32:
  - the direct STM pattern-change realign;
  - the AVR ACK/follow duplicate realign.
- Non-temporary pattern changes keep the existing realign behavior.

Files changed:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/Sequencer/sequencer.h`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`

Build check:

- `make -C mainboard/LxrStm32 stm32` completed successfully.

Observed warnings:

- Existing STM warnings remain in mixer, oscillator, modulation node, sequencer init bounds, Euklid generator, TriggerOut, main exit recursion, MidiParser fallthrough, frontPanelParser unused/fallthrough areas, and linker RWX segment warning.

---

## 34. Investigation: Remaining Chirp Candidates After State/Realign Removal

Status: investigation note only on 2026-06-01. No code change in this section.

Current observation:

- Removing stale states 2/4 and suppressing temp-boundary realign improved the chirp but did not eliminate it.

Candidate 1: immediate raw DSP parameter discontinuities.

- On a normal/temporary source change, STM now calls `seq_applyVoiceSource(...)` for the affected synth voices.
- That function applies every masked interpolated voice parameter through `midiParser_ccHandler(..., 0)`.
- `midiParser_ccHandler(...)` writes many DSP fields directly:
  - oscillator waveform and pitch-related `modNodeValue`;
  - mod oscillator frequency/gain;
  - filter frequency/resonance;
  - envelope attack/decay/slope;
  - volume, pan, drive, mix, and similar float/uint fields.
- These writes are not smoothed and can happen while a voice tail is audible.
- This is currently the strongest remaining candidate: a hard value step in oscillator/filter/amp/distortion state can click or chirp even without an explicit phase reset.

Candidate 2: automation target reassignment side effects.

- Voice-local LFO/velocity automation target assignments are applied with `modNode_setDestination(...)` plus `modNode_updateValue(...)`.
- `modNode_setDestination(...)` calls `modNode_resetTargets()`.
- `modNode_resetTargets()` restores the original value for all velocity and LFO modulation destinations, not just the single destination being changed.
- Therefore a normal/temporary source switch that changes an automation target can cause broader parameter restoration side effects than the single voice parameter payload suggests.
- This is a strong candidate if the chirp is most obvious on kits using LFO/velocity automation or macro modulation.

Candidate 3: macro modulation target reassignment on full temp-boundary switch.

- `seq_setTmpKitActive(...)` applies shared automation target assignments immediately.
- Macro modulation target assignments are treated as shared/global.
- Applying macro destination changes also uses `modNode_setDestination(...)`, so it can hit the same `modNode_resetTargets()` behavior.
- If normal and temporary images differ in macro modulation assignments, a full temp-boundary switch can rewrite modulated parameter state globally.

Candidate 4: endpoint menu restore traffic and handshake load.

- Full normal/temporary switches still push kit/front endpoint and morph parameter endpoint images from STM to AVR.
- This should not feed parameter writes back into STM because AVR suppresses outbound traffic during restore, but it is a blocking UART transaction in the pattern-change path.
- Audio IRQs should continue, but the transaction may still add control-rate timing pressure.
- A useful isolation test would be to temporarily disable endpoint pushes to AVR during temp-boundary switching and see whether the chirp changes. This would be a test-only branch, not desired final behavior.

Candidate 5: all-notes-off / active voice bookkeeping.

- `seq_nextStep()` still calls `voiceControl_noteOff(0xFF)` on every pattern change, including temp-boundary changes.
- `voiceControl_noteOff(0xFF)` clears the MIDI active-voice bitmap and sends MIDI note-off, but it does not directly stop the internal DSP voices or reset oscillator phase.
- This is less likely to be the internal chirp source, but it can affect external MIDI and later note-off bookkeeping.

Candidate 6: true oscillator/LFO phase reset.

- Direct oscillator phase resets are visible in the voice trigger functions, for example `Drum_trigger`, `Snare_trigger`, `Cymbal_trigger`, and `HiHat_trigger`.
- Those are normal note-trigger behavior and should not happen merely because of a parameter source swap.
- LFO retrigger also occurs on voice trigger.
- I did not find a direct switch-time oscillator or LFO phase reset in the normal/temporary source-swap path itself.
- Therefore "phase reset in DSP/floats" is possible on the next actual note trigger, but not the most likely explanation for a chirp exactly at the pattern-source switch.

Likely next experiments:

- First isolation test: skip or defer voice-local parameter application on temp-boundary switch while keeping pattern data switch active. If the chirp disappears, raw DSP parameter discontinuity is confirmed.
- Second isolation test: apply voice-local parameters but skip automation target reassignment. If the chirp improves, `modNode_setDestination()` / `modNode_resetTargets()` is implicated.
- Third isolation test: keep audio parameter swap but temporarily suppress STM-to-AVR endpoint menu restore. If the chirp improves, restore transaction timing is implicated.
- Fourth experiment, if raw parameter discontinuity is confirmed: separate "trigger-safe" voice parameters from "tail-sensitive" voice parameters, applying the latter at note-on or with a short ramp/smoothing strategy.
