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
