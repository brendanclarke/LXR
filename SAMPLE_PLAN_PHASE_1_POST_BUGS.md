# SAMPLE_PLAN_PHASE_1_POST_BUGS: Load Glitch and Extra Silent Sample Slot

## Scope

This note assesses two hardware observations after the Phase 1 flash-guard work:

1. First `Load: Samples` selection flashes and returns to the load menu early without loading.
2. The oscillator waveform list shows `s1`, but only `s0` contains the loaded sample.

Both behaviors existed before Phase 1. Phase 1 made flash writes fail closed, but it did not redesign the current AVR/STM sample-load handshake or oscillator sample-index handling.

## Bug 1: First `Load: Samples` Attempt Returns Early

Likely cause:

- The AVR load menu path sends `SAMPLE_CC, SAMPLE_START_UPLOAD, 0x00`, then calls `uart_waitAck()`.
- `uart_waitAck()` returns the first queued UART byte of any kind. Its ACK/NACK filter is commented out, and it has no timeout.
- The AVR also sends `SAMPLE_CC, SAMPLE_COUNT, 0x00` at startup. If the STM sample-count reply is still queued, the first byte seen by `uart_waitAck()` may be `SAMPLE_CC`, `SAMPLE_COUNT`, or the count value rather than `ACK`.
- The `SAVE_TYPE_SAMPLES` branch ignores the non-ACK case and still calls `preset_init()`, which reinitializes the AVR SD/FatFs side.
- Meanwhile, the STM side handles `FRONT_SAMPLE_START_UPLOAD` by calling `sampleMemory_init()` and `sampleMemory_loadSamples()`, which currently scans `/samples` through the STM SD stack.

Evidence in current code:

- `front/LxrAvr/Menu/menu.c`: `SAVE_TYPE_SAMPLES` sends upload, waits with `uart_waitAck()`, ignores the result, then calls `preset_init()` and requests sample count again.
- `front/LxrAvr/IO/uart.c`: `uart_waitAck()` returns the first available byte; the `if(data == ACK || data == NACK)` check is commented out.
- `front/LxrAvr/main.c`: startup requests sample count with `avrComms_sendData(SAMPLE_CC, SAMPLE_COUNT, 0x00)`.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`: `FRONT_SAMPLE_START_UPLOAD` performs the full blocking STM-side import before sending `ACK`.
- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`: current import still scans `/samples` on the STM SD stack, even though the hardware archive says the SD card is AVR-owned.

Hardware risk:

- This is not just a UI race. If the AVR exits the wait early and reinitializes SD while the STM has taken SPI pins to scan the card, both processors can touch the SD bus during the same transaction. That is exactly the ownership problem called out in `STM32F4_HARDWARE.md`.

Assessment:

- Do not invest much in the old STM-side SD import path if Phase 3 is next. Phase 3 should replace this entire path with AVR-owned SD scanning and streamed UART sample import.
- If the old path must remain testable before Phase 3, add a small interim fix: drain/parse pending UART bytes before starting sample upload, then wait only for `ACK` or `NACK` with a timeout. Non-ACK protocol bytes should be fed through `uart_checkAndParse()` or explicitly ignored only after preserving parser alignment.

Fold-in:

- This should be folded into `SAMPLE_PLAN_PHASE_3.md`, because Phase 3 replaces the unsafe handshake with an explicit import transaction and status replies.
- A temporary Phase 1 follow-up is acceptable only as a stopgap for hardware testing.

## Bug 2: Extra Silent `s1` Waveform Entry

Likely causes:

- The AVR menu range for waveform entries is count-based and appears correct for fresh edits: built-ins are `waveformNames[0][0] == 6`, and `getMaxEntriesForMenu(MENU_WAVEFORM)` returns `6 + menu_numSamples`. With one sample, valid list values should be `0..6`, so only `s0` should be selectable.
- However, `menu_setNumSamples()` only stores the new count. It does not clamp existing waveform parameter values after the count changes. If a preset, stale UI value, or previous sample count leaves a waveform parameter at value `7`, `getMenuItemNameForValue()` renders any value `>= 6` as `sN`, so value `7` displays as `s1`.
- On the STM side, the audio dispatcher has an off-by-one guard:

```c
if( osc->waveform - OSC_SAMPLE_START > sampleMemory_getNumSamples() ) return;
```

For one loaded sample, `osc->waveform == OSC_SAMPLE_START + 1` gives index `1`; `1 > 1` is false, so the STM treats `s1` as valid and reads `sampleMemory_getSampleInfo(1)`. That metadata slot is not a committed sample, so playback is silent or undefined.

Additional STM risk:

- The FM sample path does not perform the same count guard before calling `calcUserSampleOscFmBlock()`.
- Returning from `calcNextOscSampleBlock()` without zero-filling the destination buffer is also unsafe. A rejected oscillator should write silence into the block, not leave the previous buffer contents intact.

Assessment:

- The STM bounds fix belongs in Phase 2, because Phase 2 already rewrites user-sample oscillator indexing, long-sample playback state, and loop behavior.
- The AVR display/value clamp belongs in Phase 4 or as a small UI hygiene task beside Phase 2. Phase 4 already owns sample name display and waveform-menu behavior, but the audible safety should not wait for Phase 4.

Fold-in:

- Add to `SAMPLE_PLAN_PHASE_2.md`: reject `sampleIndex >= sampleMemory_getNumSamples()` in both normal and FM user-sample block paths, zero-fill output on rejection, and validate `SampleInfo.size` before reading sample flash.
- Add to `SAMPLE_PLAN_PHASE_4.md`: when `menu_setNumSamples()` updates the sample count, clamp all live waveform menu parameters and morph endpoint values that exceed the new maximum, or at minimum render out-of-range values as `---` rather than `sN`.

## Recommended Priority

Fix order:

1. Phase 2 STM oscillator guard for `sampleIndex >= sampleCount`. This prevents out-of-range metadata reads and directly explains the silent `s1`.
2. Phase 3 sample import transaction. This removes the first-load race and the hardware-unsafe STM SD access.
3. Phase 4 AVR waveform UI clamp/name display. This prevents stale values from looking like valid samples.

Near-term hardware-test workaround:

- After loading samples, request/refresh sample count and nudge any oscillator waveform value back into range. If `s1` appears with only one sample loaded, select another waveform and return to `s0`; the current edit path should clamp fresh encoder edits to the current menu range.
