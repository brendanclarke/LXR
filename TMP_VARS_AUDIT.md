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
| `morphParams[]` | `uint8_t[310]` | Stores the **morph target endpoints**. |
| `morphParamsValid[]` | `uint8_t[310]` | Validity mask for the morph target endpoints. |
| `interpolatedParams[]` | `uint8_t[310]` | Stores the **active current-play** sound image. This is the result of morphing and is used by the sound engine for audio generation. |
| `interpolatedParamsValid[]` | `uint8_t[310]` | Validity mask for the interpolated image. |
| `automation` | `SeqTmpKitAutomation` | Nested struct for modulation destinations. |
| `valid` | `uint8_t` | Flag indicating if the kit data is fully captured and ready for use. |

### **Struct: `SeqTmpKitAutomation`**
Stores raw modulation destination indices.

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
