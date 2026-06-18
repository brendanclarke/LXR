# Encoder List Mismatch Error Notes

Date: 2026-06-18  
Session: 026  
Status: unresolved. The repository was reverted after bad attempted fixes; this file records findings only.

## Symptom

The AVR menu displays text-list parameters inconsistently between pot editing and encoder editing. The clearest reported case is oscillator waveform:

- pot/list display can show `SIN`;
- entering edit with the encoder and turning once can jump to `CYM` or the end of the list;
- the class of affected parameters appears to be `DTYPE_MENU`, not numeric, on/off, or mod-target parameters;
- examples include oscillator waveform, LFO waveform, transient waveform, and quantize values.

## Confirmed Code Paths

Display path:

- `menu_repaintGeneric()` renders normal and edit-mode values from `parameter_values[parNr]`.
- For `DTYPE_MENU`, it calls `getMenuItemNameForValue(menuId, curParmVal, ...)`.
- The display expects `curParmVal` to already be a list index, not a scaled `0..255` byte.

Pot path:

- `menu_parseKnobValue()` resolves the edited `paramNr` from the active page and the physical pot number.
- `getDtypeValue()` handles `DTYPE_MENU` by mapping the raw pot byte `0..255` to `0..numEntries-1`.
- Pot edits store the mapped list index in `parameter_values[paramNr]` and send that same list index.

Encoder path:

- `menu_parseEncoder()` applies `menu_applyEncoderAcceleration()` before calling `menu_encoderChangeParameter()` in edit mode.
- `menu_encoderChangeParameter()` increments the raw stored byte in `parameter_values[paramNr]`.
- Only after incrementing does the `DTYPE_MENU` branch clamp to `numEntries-1`.
- The current checked-out code does not treat `DTYPE_MENU` as a one-step symbolic list for encoder edits; it applies the accelerated numeric delta first.

Send path:

- After editing, both pot and encoder send the stored list index through ordinary `MIDI_CC` / `CC_2` or the relevant global handler.
- No separate waveform/list protocol conversion exists after the value has been stored; the AVR and STM both appear to expect these parameters as list indexes.

## Things That Were Tried And Are Not A Complete Fix

### Encoder Acceleration

Applying encoder acceleration to `DTYPE_MENU` is a real secondary problem. A text list should step by one item per detent, not by an accelerated numeric delta.

However, fixing only this was reported not to solve the underlying mismatch. Treat acceleration as a contributor, not the root cause by itself.

### PROGMEM Count Reads

`getMaxEntriesForMenu()` reads count bytes from `PROGMEM` tables with normal array access, e.g. `waveformNames[0][0]`. That is suspicious AVR code and may deserve cleanup later.

But changing those reads to `pgm_read_byte()` did not solve the reported mismatch and was reverted. Do not present this as the root cause without fresh proof.

## Current Best Estimate

The most useful next test is to prove whether the encoder is editing the same parameter/value that the display just showed.

There are two plausible failure modes still worth checking:

1. The encoder active slot and the pot-touched/displayed slot may diverge. Pot edits use the physical `potNr`; encoder edits use `menuIndex & MASK_PARAMETER`. `menu_parseKnobValue()` does not move `menuIndex` to the touched pot. If the user reads a value changed by a pot but the encoder cursor is still on another list parameter, entering edit can appear to jump to the wrong list state.
2. The encoder may be entering `menu_encoderChangeParameter()` with a pre-edit `parameter_values[paramNr]` that is already out of range or near the end. The display path should normally reveal that, but a stale active slot or bad `parNr` would make it look like `SIN -> CYM`.

## Recommended Next Debug Step

Before another fix attempt, instrument one failing parameter path on AVR:

- active page;
- active parameter;
- resolved `paramNr`;
- dtype and `menuId`;
- displayed `curParmVal`;
- encoder `inc`;
- acceleration multiplier;
- value before increment;
- value after increment;
- value after `DTYPE_MENU` clamp.

The important question is simple:

```text
When the LCD shows OSC waveform SIN, does encoder edit actually enter with
paramNr = PAR_OSC_WAVE_DRUMx and pre-value = 0?
```

If yes, the bug is in encoder delta handling.

If no, the bug is in active-slot/display synchronization or stale parameter state.

Do not patch this again until that trace answers the question.
