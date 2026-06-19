# Encoder List Mismatch Error Notes

Date: 2026-06-18 (revised)  
Session: 027  
Status: fix applied; AVR build verified. Hardware re-test pending.

## Symptom

The AVR menu displays text-list parameters inconsistently between pot editing and encoder editing. The clearest reported case is oscillator waveform:

- pot/list display can show `SIN`;
- entering edit with the encoder and turning once can jump to `CYM` or `NOI`;
- the class of affected parameters is `DTYPE_MENU`, not numeric, on/off, or mod-target parameters;
- examples include oscillator waveform, LFO waveform, transient waveform, and quantize values.

## Root Cause

**Encoder acceleration is applied to `DTYPE_MENU` parameters, causing multi-entry jumps in text lists where each detent should advance by exactly one entry.**

### How acceleration corrupts list navigation

The encoder call chain in edit mode is:

```
menu_parseEncoder()
  → menu_applyEncoderAcceleration(inc)     // multiplies inc by up to 4
  → menu_encoderChangeParameter(inc)
    → menu_applyEncoderDeltaToByte(paramValue, inc)  // *paramValue += accelerated_inc
    → DTYPE_MENU clamp: *paramValue = min(*paramValue, numEntries-1)
```

When the encoder multiplier is e.g. 4, a single CW detent produces `inc = 4`. For a `DTYPE_MENU` parameter whose stored value is a list index (0 = SIN for `MENU_WAVEFORM`), `menu_applyEncoderDeltaToByte` sets the value to 0 + 4 = 4 (= NOI), skipping over TRI, SAW, REC entirely. A second detent pushes it to 8, which the DTYPE_MENU clamp caps to 5 (= CYM).

The clamp only prevents overflow past the end of the list; it does **not** prevent the acceleration multiplier from producing a delta larger than 1. Since the list index range is small (typically 5–18 entries), even a multiplier of 2 can skip entries, and a multiplier of 4 at fast spin can jump from the first entry to near the end of a short list.

### Why pot edits don't show this problem

The pot path (`menu_parseKnobValue` → `getDtypeValue`) maps the raw pot ADC byte `0..255` onto `0..(numEntries-1)` using `frac * (numEntries-1)`. It is a **proportional mapping**, not an incremental delta. The pot position always maps to a specific list index regardless of the previous value, so it cannot skip entries.

## Disproven Theories

### Enumeration mismatch between pot raw bytes and list indices

**This was the leading suspicion in the Session 026 version of this document but is not the cause.**

Both the pot and encoder paths store **list indices** (0-based) in `parameter_values[paramNr]`:

- **Pot**: `getDtypeValue()` for `DTYPE_MENU` computes `(uint8_t)(frac * (numEntries-1))`, which is a list index in `0..numEntries-1`.
- **Encoder**: `menu_encoderChangeParameter()` increments/decrements the existing list index in `parameter_values[paramNr]` and then clamps to `numEntries-1`.

Both paths store the same 0-based list index format. The save/load round-trip also preserves this format because `presetManager.c` writes and reads `parameter_values[i]` directly. There is no raw-byte-vs-list-index divergence in storage.

### PROGMEM count reads

`getMaxEntriesForMenu()` reads count bytes from PROGMEM tables with normal array access, e.g. `waveformNames[0][0]`. On AVR, reading PROGMEM without `pgm_read_byte()` is technically incorrect. However:

- GCC on AVR sometimes elides `pgm_read_byte()` for `static const` arrays when the optimizer can prove the address is in flash and converts the access implicitly.
- The count values are read correctly at runtime (the display shows the right number of entries and the correct names).
- Changing these reads to `pgm_read_byte()` was attempted in Session 026 and did not solve the problem.

The PROGMEM access style is a code hygiene issue but not the cause of the list mismatch.

### Active slot divergence between pot and encoder

`menu_parseKnobValue()` resolves `paramNr` from the physical `potNr` on the active page and does **not** move the encoder cursor (`menuIndex`). `menu_encoderChangeParameter()` resolves `paramNr` from `menuIndex & MASK_PARAMETER`.

If the user touches a pot to read/edit one parameter, then presses the encoder button to enter edit mode, the encoder will edit its cursor's parameter, not the pot-touched parameter. This is a real UX inconsistency but does **not** cause the list jump symptom (because the jump is seen when editing the same parameter that was just displayed).

## Full Code Path Analysis

### Display path

`menu_repaintGeneric()` → reads `parameter_values[parNr]` (a list index) → for `DTYPE_MENU`, calls `getMenuItemNameForValue(menuId, curParmVal, buf)`. The display function accesses the name table at `[curParmVal + 1]` (skipping the count in entry 0).

For `MENU_WAVEFORM` specifically, if `curParmVal < waveformNames[0][0]` (i.e. < 6) the fixed waveform name is used; otherwise a sample waveform label "s0", "s1", ... is constructed. This works correctly with the 0-based list index.

### Pot path

`menu_parseKnobValue(potNr, potValue)`:
1. Resolves `paramNr` from `menuPages[menu_activePage][activePage].bot1 + potNr + isOn2ndPage`.
2. Calls `getDtypeValue(potValue, paramNr)`.
   - For `DTYPE_MENU`: reads `menuId` from upper nibble of dtype byte, calls `getMaxEntriesForMenu(menuId)`, returns `(uint8_t)(frac * (numEntries-1))`.
3. Stores the returned list index in `parameter_values[paramNr]`.
4. Sends the list index via `MIDI_CC` / `CC_2` / global handler.

For `MENU_WAVEFORM` with 6 entries and `menu_numSamples = 0`:
- `numEntries = 6`
- `getDtypeValue` returns `0..5` proportional to pot position.
- Stored value 0 = SIN, 1 = TRI, ..., 5 = CYM.

### Encoder path

`menu_encoderChangeParameter(inc)` (where `inc` is already accelerated by `menu_applyEncoderAcceleration`):
1. Resolves `paramNr` from `menuIndex & MASK_PARAMETER` via `menuPages[menu_activePage][activePage].bot1 + activeParameter`.
2. Calls `menu_applyEncoderDeltaToByte(paramValue, inc)` which does `int16_t v = *paramValue; v += inc; clamp(v, 0, 255); *paramValue = v`.
3. For `DTYPE_MENU`: reads `menuId` from upper nibble, calls `getMaxEntriesForMenu(menuId)`, clamps `*paramValue` to `numEntries-1`.
4. Sends the value via CC/CC2/global.

The clamp in step 3 functions correctly as an overflow guard: it prevents the stored index from exceeding `numEntries-1`. But step 2 applies the accelerated delta **before** the clamp, so the value can jump by 2, 4, or more in a single edit step.

## Affected Parameters

All `DTYPE_MENU` parameters are affected. The full list from `menu.c` `parameter_dtypes[]`:

| Menu ID | Parameter Set | Num Entries (fixed) |
|---------|--------------|---------------------|
| `MENU_WAVEFORM` (2) | OSC waveform (Drum1-3, Snare, Cym1, HH1), Mod OSC waveform (Drum1-3, Cym2-3, HH2-3) | 6 (plus samples) |
| `MENU_LFO_WAVES` (6) | LFO waveform (LFO1-6) | 18 |
| `MENU_RETRIGGER` (7) | LFO retrigger (LFO1-6) | 7 |
| `MENU_SYNC_RATES` (5) | LFO sync rate (LFO1-6) | 12 |
| `MENU_FILTER` (1) | Filter type (voices 1-6) | 8 |
| `MENU_TRANS` (10) | Transient waveform (voices 1-6) | 14 |
| `MENU_AUDIO_OUT` (3) | Audio output (voices 1-6) | 6 |
| `MENU_ROLL_RATES` (4) | Roll rate | 16 |
| `MENU_NEXT_PATTERN` (8) | Next pattern | 15 |
| `MENU_TRACK_SCALE` (15) | Track scale | 8 |
| `MENU_MIDI` (11) | Roll mode*, MIDI mode (unused) | varies |
| `MENU_SEQ_QUANT` (9) | Quantisation | 5 |
| `MENU_MIDI_ROUTING` (12) | MIDI routing | 6 |
| `MENU_MIDI_FILTERING` (13) | MIDI TX/RX filter | 16 |
| `MENU_PPQ` (14) | Clock prescaler in/out | 5 |
| `MENU_FILE_LOAD_BACKGROUND` (0) | Background load mode | 5 |

\* `PAR_ROLL_MODE` reuses `MENU_MIDI` for its menu ID with the `midiModes` table (5 entries: trg, nte, vel, bth, all).

### Short-list parameters most visibly affected

Parameters with ≤ 6 entries show the most dramatic jumps:
- **Waveform** (6 entries): SIN → NOI or CYM in one tick
- **Audio Out** (6 entries): St1 → L1 or R1 in one tick
- **Quantisation** (5 entries): off → 32 or 64 in one tick
- **PPQ** (5 entries): 1 → 8 or 16 in one tick
- **Background Load** (5 entries): off → prf or all in one tick

Parameters with more entries (LFO waveform: 18, roll rate: 16, MIDI filter: 16) still skip entries but the effect is less extreme per-detent.

## Fix

The correction is to **skip acceleration for `DTYPE_MENU` parameters**. The encoder edit path should check the dtype before applying acceleration:

In `menu_parseEncoder()` (around line 2526-2532):
```c
} else if(editModeActive) {
    // edit mode is active so change the value of the current parameter
    inc = menu_applyEncoderAcceleration(inc);  // BUG: applied unconditionally
    if (buttonHandler_getShift())
        menu_encoderChangeShiftParameter(inc);
    else
        menu_encoderChangeParameter(inc);
}
```

The dtype of the active parameter should be checked, and acceleration should be bypassed when the dtype low nibble is `DTYPE_MENU`:

```c
} else if(editModeActive) {
    uint16_t paramNr = pgm_read_word(&menuPages[menu_activePage][activePage].bot1
                                     + (menuIndex & MASK_PARAMETER));
    uint8_t dtype = pgm_read_byte(&parameter_dtypes[paramNr]) & 0x0F;
    if(dtype != DTYPE_MENU)
        inc = menu_applyEncoderAcceleration(inc);
    if (buttonHandler_getShift())
        menu_encoderChangeShiftParameter(inc);
    else
        menu_encoderChangeParameter(inc);
}
```

This also needs a corresponding guard in the shift-encoder path (`menu_encoderChangeShiftParameter`) if any `DTYPE_MENU` parameters are reachable from the shift-encoder path. Based on current code, shift-encoder edits morph endpoint values; `DTYPE_MENU` parameters in the morph endpoint table (`parameters2[]`) would also need protection.

### Alternative: guard inside `menu_applyEncoderDeltaToByte`

An alternative approach is to check dtype inside `menu_encoderChangeParameter()` and `menu_encoderChangeShiftParameter()` and pass `inc = (inc < 0) ? -1 : (inc > 0) ? 1 : 0` for DTYPE_MENU. This is more localized but requires repeating the guard in both functions.

### Recommended approach

Guard in `menu_parseEncoder()` at the acceleration call site, since that's where the decision belongs (acceleration policy, not value application). This keeps `menu_encoderChangeParameter()` focused on value change/clamp/send logic.

No changes to `getDtypeValue()`, `getMaxEntriesForMenu()`, `getMenuItemNameForValue()`, or `menu_applyEncoderDeltaToByte()` were needed.

### Applied Change (Session 027)

In `menu_parseEncoder()`, the acceleration call is now guarded by a dtype check. The active parameter is resolved, its dtype low nibble is read, and `menu_applyEncoderAcceleration()` is only called when the dtype is not `DTYPE_MENU`. The guarded block was inserted at the `editModeActive` branch before the shift/change dispatch, so both `menu_encoderChangeParameter()` and `menu_encoderChangeShiftParameter()` receive the unaccelerated `inc` for text-list parameters.

```c
} else if(editModeActive) {
    {
        /* -bc- 027: DTYPE_MENU parameters are list indices.
           Acceleration should skip entries one at a time,
           so bypass the multiplier for text-list types. */
        const uint8_t activeParameter = menuIndex & MASK_PARAMETER;
        const uint8_t activePage      = (menuIndex & MASK_PAGE) >> PAGE_SHIFT;
        const uint16_t parNr = pgm_read_word(
            &menuPages[menu_activePage][activePage].bot1 + activeParameter);
        const uint8_t dtype = pgm_read_byte(&parameter_dtypes[parNr]) & 0x0F;
        if (dtype != DTYPE_MENU)
            inc = menu_applyEncoderAcceleration(inc);
    }
    if (buttonHandler_getShift())
        menu_encoderChangeShiftParameter(inc);
    else
        menu_encoderChangeParameter(inc);
}
```

AVR build (`make -C front/LxrAvr avr -j4`) passes with the usual pre-existing `MASK_PARAMETER` sign-conversion warning only.

## Secondary Issue: Active Slot Divergence

When the user touches a pot, `menu_parseKnobValue()` resolves the parameter from the physical pot number and updates `parameter_values[paramNr]` without moving `menuIndex`. The encoder cursor remains on its previous position. When the user then presses the encoder button to enter edit mode, `menu_encoderChangeParameter()` uses the encoder cursor position to resolve `paramNr`.

If the pot and the encoder cursor point to different parameters, the user will see the displayed value (from the encoder cursor's parameter) change unexpectedly when editing, because they think they're editing the pot-touched parameter.

This is a separate UX issue from the acceleration bug. The fix would require `menu_parseKnobValue()` to move `menuIndex` to the touched parameter, but that is a behavioral change that should be evaluated separately and is not required to resolve the list mismatch symptom.

## What Was Tried Before (Session 026) And Reverted

| Change | Why Reverted |
|--------|-------------|
| Skipping acceleration for `DTYPE_MENU` only in `menu_encoderChangeParameter()` | Was reported not to fix the problem; likely because acceleration was already multiplied before the function was called, or because the test parameter had a stale/out-of-range starting value that the clamp alone couldn't fix. |
| Adding `pgm_read_byte()` to `getMaxEntriesForMenu()` count reads | Did not change behavior; GCC already handled the PROGMEM access correctly. |
| Various PROGMEM count corrections | No effect. |

## Recommended Verification

Before committing a fix, instrument one `DTYPE_MENU` parameter (e.g., `PAR_OSC_WAVE_DRUM1`) in the encoder path to confirm:

1. Starting value in `parameter_values[paramNr]`
2. `inc` value received by `menu_encoderChangeParameter()`
3. `encode_getAccelerationMultiplier()` return value at the time of edit
4. Value after `menu_applyEncoderDeltaToByte()`
5. Value after DTYPE_MENU clamp

The expected trace for a correct fix: `inc` should be ±1 when `menu_encoderChangeParameter()` receives it, regardless of spin speed.