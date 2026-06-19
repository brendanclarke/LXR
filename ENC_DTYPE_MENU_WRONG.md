# Encoder DTYPE_MENU Clamp Diagnosis

## Symptom

Encoder edit mode selects the correct parameter, but when that parameter is `DTYPE_MENU`, the edited value jumps/clamps to the last item in that menu list.

## Diagnosis

This is not the live-record `+1/-1` parameter-index bug. It is a parameter value-domain mismatch.

The encoder path assumes `parameter_values[paramNr]` for `DTYPE_MENU` is already a compact list index:

- valid range is `0..getMaxEntriesForMenu(menuId)-1`
- one encoder detent should add or subtract one list index

The relevant encoder path is:

```c
menu_applyEncoderDeltaToByte(paramValue, inc);
...
if(*paramValue >= numEntries)
   *paramValue = (uint8_t)(numEntries-1);
```

So if the stored value is a raw byte like `64`, `127`, or `255` instead of a list index, the first encoder movement leaves it above `numEntries` and the clamp forces it to the last item.

The pot path does not expose this because it maps the raw pot byte into a list index before storing:

```c
return (uint8_t)(frac * (numEntries-1));
```

## Likely Source

Some load/restore/report path is likely writing raw bytes directly into `parameter_values[]` for `DTYPE_MENU` parameters. Candidate receive paths include `PARAM_CC`, `PARAM_CC2`, `PRF_RESTORE_PARAM_CC`, and `PRF_RESTORE_PARAM_CC2`, which assign `data2` directly.

## Fix Shape

Normalize or validate `DTYPE_MENU` values before encoder delta is applied, or normalize incoming/restored menu values when they are written into `parameter_values[]`. This should be handled separately from live-record automation destination indexing.

## Work Notes

Applied the encoder-local fix in `front/LxrAvr/Menu/menu.c`.

- Added `menu_getDtypeMenuEntryCount()` so both encoder edit paths use the same menu-entry lookup.
- Added `menu_normalizeDtypeMenuEncoderValue()` before applying encoder delta.
- The normalizer leaves already-valid compact list indices alone.
- If a `DTYPE_MENU` value is out of compact range, it treats the stored value as a raw byte and maps it proportionally into `0..numEntries-1` before applying the one-detent encoder movement.
- Wired the same normalization into both `menu_encoderChangeParameter()` and `menu_encoderChangeShiftParameter()`, so regular endpoint edits and shift/morph endpoint edits behave the same.
- Tightened the existing `DTYPE_MENU` clamp in both paths so `numEntries == 0` cannot underflow to `255`.

This is intentionally separate from the live-record automation parameter-index fix. The encoder was selecting the correct parameter; it was starting from a value outside the compact menu-index domain.

## Verification

- `make -C front/LxrAvr avr -j4` passes.
- The AVR build still reports the existing `menu_resetActiveParameter` sign-conversion warning at `menu.c:3306`.
- `make firmware` passes and rebuilds `firmware image/FIRMWARE.BIN`.
