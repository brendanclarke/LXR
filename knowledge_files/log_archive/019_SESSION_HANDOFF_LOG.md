# LXR -bc- Enhanced Firmware — Session 019 Handoff Log

**Project**: LXR -bc- Enhanced Firmware  
**Session goal**: Re-examine AVR front-panel main encoder performance, diagnose very slow counter-clockwise missed decrements, tune the decoder, and document the final encoder state.  
**Last session summary**: Session 018 consolidated front-panel send helpers, split the MIDI parser into channel/global files, moved the transitional receive load/session cache into `PresetLoadCache`, and passed user smoke tests for key front-panel and MIDI paths.  
**Working repository**: `/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod` on branch `custom-develop-patload-envmod`; tree already contained unrelated Session 018 dirty work and generated firmware output.  
**Constraints today**: Keep code changes focused on the AVR encoder/menu path and durable docs; do not revive legacy encoder read modes, PCINT decoding, Timer0 sampling, or temporary LCD/debug hooks.

Key files to be aware of:
- `front/LxrAvr/encoder.c`
- `front/LxrAvr/encoder.h`
- `front/LxrAvr/config.h`
- `front/LxrAvr/Menu/menu.c`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`
- `knowledge_files/hardware_archive/front/AVR_HARDWARE.md`
- `knowledge_files/hardware_archive/front/AVR_SETUP_ALLOCATION.md`
- `MEMORY.md`

## Session 019 Summary

Session 019 re-opened the AVR main encoder after the user reported that very slow counter-clockwise clicks could again fail to decrement values. The first audit found no AVR encoder source regression after Session 016: the source still used the Timer1 fixed-`AB=11` rest-phase FSM, and the later Sessions 017/018 refactors did not touch `front/LxrAvr/encoder.c`, `encoder.h`, `main.c`, `config.h`, or the AVR Makefile.

The issue was therefore diagnosed as a latent mechanical/filtering edge in the strict Session 016 FSM. If a very slow click let contact chatter pass the two-sample filter as a rest-to-opposite illegal transition, such as `11 -> 00`, the old FSM reset and ignored the rest of that physical detent. That matched the reported symptom: the hardware clicked, but no raw detent reached `enc_delta`, so the UI did not move.

The final Session 019 implementation keeps the core Session 016 architecture intact but retunes it:

- Timer1 compare A remains the only encoder/button sampler.
- PC0/PC1 are still the immutable phase inputs, PC2 is still the active-low button.
- The physical rest phase remains fixed at `AB=11` (`ENCODER_REST_STATE = 0x03`).
- `encode_stableRead4()` remains the raw detent read path.
- Legacy `encode_read1()`, `encode_read4()`, `encode_stableRead1()`, `encode_stableRead2()`, `ENC_USE_STABLE_DRIVER`, PCINT decoding, and Timer0 encoder sampling remain intentionally unsupported.
- Sampling now runs at roughly 32.05 kHz rather than roughly 16.45 kHz.
- Phase filtering now requires six stable samples.
- Button debounce now uses 192 samples in the Timer1 ISR.
- The FSM now has a narrow rest-jump recovery path for `11 -> 00 -> adjacent -> 11` contact sequences.
- Edit-mode parameter acceleration is available, but raw detent reads remain unaccelerated.

The user hardware-tested the final combination and reported it good. `ENC_ACCEL_MAX_MULT` was reduced from the proposed 5 to 4 because 5 felt too fast overall.

## Initial Audit Findings

The initial audit was written to `ENCODER_AUDIT.md` and then folded into this handoff. It found:

- `git log -- front/LxrAvr/encoder.c front/LxrAvr/encoder.h front/LxrAvr/main.c front/LxrAvr/config.h` showed the latest encoder-path source change before this session was `a4fbe1a`, the Session 016 encoder fix.
- There was no diff from that commit to then-current `HEAD` for `encoder.c`, `encoder.h`, `main.c`, `config.h`, or `front/LxrAvr/Makefile`.
- The dirty tree contained STM32/MIDI/UART refactor work and rebuilt firmware images, but no AVR encoder source modifications.
- Timer1 was still only used by the encoder. Timer2 remained the coarse system tick. Timer0 was not used by the current encoder implementation.
- The main loop still read:

```c
const int8_t encoderValue = encode_stableRead4();
const uint8_t button = encode_readButton();
menu_parseEncoder(encoderValue, button);
```

`menu_parseEncoder()` still inverted the raw sign before applying menu behavior:

```c
inc = (int8_t)(inc * -1);
```

That inversion was old menu-layer behavior and was not the source of the slow counter-clockwise miss.

## Decoder Failure Mechanism

The Session 016 decoder was intentionally strict:

1. Timer1 samples PC0/PC1.
2. Each phase must hold the same value for `ENCODER_PHASE_STABLE_SAMPLES`.
3. A filtered state change is handed to `encoder_handleStateChange()`.
4. The FSM only starts a detent when a legal transition leaves fixed rest `AB=11`.
5. It emits a detent only if four legal same-direction transitions return to `AB=11`.
6. Any illegal transition resets the FSM.
7. A valid transition that does not start from rest is ignored while the FSM is idle.

For fixed rest `AB=11`, the expected counter-clockwise sequence, assuming negative raw direction, is:

```text
11 -> 01 -> 00 -> 10 -> 11
```

If slow contact chatter or asymmetric phase settling caused this filtered sequence instead:

```text
11 -> 00 -> 10 -> 11
```

the old FSM saw `11 -> 00` as illegal because both bits changed. It reset, then ignored the later legal transitions because they no longer left the rest state. The physical click was therefore discarded. This explained why very slow turns were vulnerable: slow movement lets contact chatter dwell long enough to become the filtered state.

## Test Sequence

### Test 1: 16 kHz / 3 Samples

The first tuning pass kept the Session 016 sample rate and raised `ENCODER_PHASE_STABLE_SAMPLES` from 2 to 3.

At the original actual Timer1 rate of about 16.45 kHz, the phase acceptance window changed from about 122 us to about 183 us.

Hardware result:

- Slow counter-clockwise behavior seemed better.
- Fast spins did not catch enough increments.

### Test 2: 32 kHz / 4 Samples

The next tuning pass increased the intended sample rate and sample count:

- `ENCODER_SAMPLE_HZ` moved from `16000UL` to `32000UL`.
- `ENCODER_PHASE_STABLE_SAMPLES` moved from 3 to 4.
- Timer1 moved from prescaler 64 to prescaler 8 so the requested 32 kHz quantized closely on a 20 MHz ATmega644.
- `ENCODER_BUTTON_STABLE_SAMPLES` was first moved to 96 to preserve the old debounce time, then doubled again to 192 after encoder-switch double clicks were observed.

At 20 MHz, Timer1 CTC with prescaler 8 and `OCR1A = 77` runs at about 32.05 kHz. Four samples at that rate were close to the original Session 016 acceptance window.

Hardware result:

- Fast-spin behavior improved enough to keep exploring the 32 kHz sampler.
- Very slow movement could again miss increments/decrements about half the time.

### Test 3: 32 kHz / 6 Samples Plus Rest-Jump Recovery

The final decoder tuning pass:

- Kept `ENCODER_SAMPLE_HZ = 32000UL`.
- Kept Timer1 prescaler 8.
- Raised `ENCODER_PHASE_STABLE_SAMPLES` from 4 to 6.
- Kept `ENCODER_BUTTON_STABLE_SAMPLES = 192`.
- Added a narrow rest-jump recovery path.

At the actual ~32.05 kHz rate, six stable samples give a phase acceptance window of about 187 us, close to the earlier 16 kHz / 3-sample test that improved slow behavior.

The recovery handles only this shape:

```text
11 -> 00 -> 10 -> 11
```

or the opposite-direction equivalent:

```text
11 -> 00 -> 01 -> 11
```

Implementation details:

- `oppositeRest` is computed as `enc_rest_state ^ 0x03`, so with fixed rest `11`, opposite rest is `00`.
- If `encoder_decodeStep(previous, current)` returns zero and the transition was from rest to opposite rest, the FSM records `enc_rest_jump_pending = 1`.
- If the next transition starts at opposite rest and moves legally to an adjacent state, the FSM resumes by setting `enc_fsm_dir = step` and `enc_fsm_phase = 3`.
- The decoder still requires a same-direction return to fixed rest before adding a detent.
- Any other illegal transition still resets the FSM without recovery.

Hardware result:

- The user reported this was "pretty good".
- The remaining desired improvement was edit acceleration for fast value changes.

## Final Encoder Timing

Final production/test constants in `front/LxrAvr/encoder.c`:

```c
#define ENCODER_SAMPLE_HZ 32000UL
#define ENCODER_PHASE_STABLE_SAMPLES 6
#define ENCODER_BUTTON_STABLE_SAMPLES 192
#define ENCODER_REST_STATE 0x03
#define ENCODER_TIMER_PRESCALE 8UL
#define ENCODER_TIMER_CLOCK_BITS (1 << CS11)
```

Timer calculation at `F_CPU = 20000000UL`:

- Timer clock: 20 MHz / 8 = 2,500,000 Hz.
- `OCR1A = (F_CPU / 8 / 32000) - 1`, truncated to 77.
- Actual ISR frequency: 2,500,000 / (77 + 1) = about 32,051.28 Hz.
- ISR period: about 31.2 us.
- Six-sample phase acceptance window: about 187 us.
- 192-sample button debounce window: about 6 ms.

## Acceleration Implementation

After Test 3 felt good, edit-mode acceleration was added.

The user requested config-driven acceleration for a 24-detent encoder:

```c
#define ENC_ACCEL_MIN_REV_PER_SEC 1
#define ENC_ACCEL_MAX_REV_PER_SEC 2
#define ENC_ACCEL_MAX_MULT 5
```

Hardware testing found max multiplier 5 too fast, so the final `front/LxrAvr/config.h` setting is:

```c
#define ENC_ACCEL_MIN_REV_PER_SEC 1
#define ENC_ACCEL_MAX_REV_PER_SEC 2
#define ENC_ACCEL_MAX_MULT 4
```

Implementation details in `encoder.c`:

- Speed is measured from complete emitted detents only, not raw quadrature transitions.
- The encoder is treated as 24 detents per revolution.
- The speed estimate uses a 4-detent moving average of detent intervals.
- A saturating interval counter runs in the Timer1 ISR and stops at the no-acceleration threshold to avoid wraparound artifacts after idle time.
- The average resets on direction reversal.
- Any interval slower than `ENC_ACCEL_MIN_REV_PER_SEC` resets the multiplier to 1.
- At or above `ENC_ACCEL_MAX_REV_PER_SEC`, the multiplier is `ENC_ACCEL_MAX_MULT`.
- Between those speeds, the integer multiplier scales linearly from 1 to `ENC_ACCEL_MAX_MULT`, with rounding.
- `encode_stableRead4()` still returns raw detents.
- `encode_getAccelerationMultiplier()` exposes the current multiplier to the menu layer.

Thresholds at nominal `ENCODER_SAMPLE_HZ = 32000UL`:

- 1 rev/s = 24 detents/s = one detent every 41.7 ms = about 1333 Timer1 ticks.
- 2 rev/s = 48 detents/s = one detent every 20.8 ms = about 666 Timer1 ticks.
- The code uses these target-rate constants for threshold math.

Application details in `Menu/menu.c`:

- Acceleration is applied only inside the edit-mode branch of `menu_parseEncoder()`.
- Normal parameter editing and shift/morph parameter editing both use the accelerated delta.
- Menu navigation, load/save handling, copy/clear target selection, and other non-edit uses remain unaccelerated.
- The helper `menu_applyEncoderAcceleration()` multiplies the signed delta and clamps to `INT8_MIN..INT8_MAX`.

## Endpoint Clamp Fix

After acceleration was added, the user reported that fast downward spins to zero felt like they hung briefly at 2 and 1 before settling on 0. The cause was the old unsigned wrap-prevention logic in both normal and shift parameter edit paths:

```c
if(*paramValue >= abs(inc))
    *paramValue += inc;
```

With acceleration, a value of 2 and a delta of -4 failed the `2 >= 4` check, so the parameter did not change until the multiplier dropped or the delta became smaller.

The final fix added `menu_applyEncoderDeltaToByte()`:

- It copies the current `uint8_t` value into a signed `int16_t`.
- It applies the signed encoder delta.
- It clamps the temporary to `0..255`.
- It writes the clamped byte back.
- Existing dtype-specific clamps still run afterward, so parameters with minimum 1 still settle to 1, while ordinary 0-based parameters land cleanly on 0.

The user hardware-tested that follow-up and reported "That's good."

## Files Changed

- `front/LxrAvr/encoder.c`
  - Moved encoder sample target to 32 kHz.
  - Changed Timer1 CTC prescaler to 8.
  - Raised phase stability requirement to six samples.
  - Raised encoder button debounce to 192 samples.
  - Added rest-to-opposite jump recovery while preserving fixed-rest detent emission.
  - Added complete-detent-based acceleration tracking and multiplier calculation.

- `front/LxrAvr/encoder.h`
  - Added `encode_getAccelerationMultiplier()`.
  - Kept `encode_stableRead4()` as the raw rotation read.

- `front/LxrAvr/config.h`
  - Added `ENC_ACCEL_MIN_REV_PER_SEC`.
  - Added `ENC_ACCEL_MAX_REV_PER_SEC`.
  - Added `ENC_ACCEL_MAX_MULT`, final hardware-tested value 4.

- `front/LxrAvr/Menu/menu.c`
  - Applies acceleration only for edit-mode normal/shift parameter changes.
  - Added signed clamp helper for accelerated byte deltas so downward edits land on zero rather than stalling near zero.

- `firmware image/FIRMWARE.BIN`
  - Rebuilt for hardware testing.

- `knowledge_files/log_archive/000_SESSION_INDEX.md`
  - Added Session 019 summary and updated the current encoder cross-session fact.

- `knowledge_files/log_archive/019_SESSION_HANDOFF_LOG.md`
  - Created this handoff.

- `knowledge_files/hardware_archive/front/AVR_HARDWARE.md`
  - Updated the encoder reference from Session 016 timing to the final Session 019 timing and behavior.

- `knowledge_files/hardware_archive/front/AVR_SETUP_ALLOCATION.md`
  - Updated Timer1 allocation, interrupt period, startup note, and encoder ISR description.

- `MEMORY.md`
  - Updated current-status and encoder reference notes for the Session 019 encoder implementation.

## Verification

Commands run successfully:

```bash
make -C front/LxrAvr avr -j4
make firmware
```

The AVR build still emits known toolchain warnings for IO-register array-bounds, existing fallthrough, and an existing sign-conversion warning in `menu_resetActiveParameter()`. No fatal errors occurred.

Hardware verification during the session:

- 16 kHz / 3 samples improved slow counter-clockwise behavior but missed too much on fast spins.
- 32 kHz / 4 samples improved fast spins but brought back slow miss behavior.
- 32 kHz / 6 samples plus rest-jump recovery was reported "pretty good".
- Acceleration with max multiplier 5 was too fast; max multiplier 4 felt good.
- Endpoint clamp fix resolved the fast downward stall near 2/1/0.
- Final user result: "That's good."

## End of session block

```
DATE: 2026-06-14
SESSION GOAL: Re-examine AVR front-panel encoder performance, diagnose slow counter-clockwise missed decrements, tune the decoder, and document the final implementation.
COMPLETED: The encoder was retuned to Timer1 ~32.05 kHz sampling with six-sample phase filtering, fixed AB=11 rest anchoring, narrow rest-jump recovery, 192-sample button debounce, and edit-mode-only acceleration. Acceleration is config-driven, based on complete emitted detents, and the endpoint clamp path was fixed so accelerated downward edits land cleanly on zero. Durable index, hardware archive, and MEMORY notes were refreshed.
VERIFIED ON HARDWARE: Yes. The user tested the decoder tuning, acceleration behavior, max multiplier 4, and the 2/1/0 endpoint clamp fix, and reported the final behavior is good.

CHANGES THIS SESSION:
- front/LxrAvr/encoder.c: moved Timer1 encoder sampling to ~32.05 kHz, raised phase filtering to six samples, raised button debounce to 192 samples, added narrow rest-jump recovery, and added complete-detent acceleration tracking.
- front/LxrAvr/encoder.h: added encode_getAccelerationMultiplier() while preserving encode_stableRead4() as the raw detent read.
- front/LxrAvr/config.h: added ENC_ACCEL_MIN_REV_PER_SEC, ENC_ACCEL_MAX_REV_PER_SEC, and ENC_ACCEL_MAX_MULT with final max multiplier 4.
- front/LxrAvr/Menu/menu.c: applies acceleration only to edit-mode parameter changes and clamps accelerated byte deltas through a signed temporary before dtype range clamps.
- firmware image/FIRMWARE.BIN: rebuilt from the updated AVR binary and current STM binary.
- knowledge_files/log_archive/000_SESSION_INDEX.md: appended Session 019 and updated the current encoder cross-session fact.
- knowledge_files/log_archive/019_SESSION_HANDOFF_LOG.md: created this detailed session handoff.
- knowledge_files/hardware_archive/front/AVR_HARDWARE.md: updated the encoder hardware/software reference to the Session 019 implementation.
- knowledge_files/hardware_archive/front/AVR_SETUP_ALLOCATION.md: updated Timer1 allocation, ISR period, startup note, and encoder timing details.
- MEMORY.md: updated current project memory for the Session 019 encoder implementation.

KNOWN ISSUES INTRODUCED: None known. The Timer1 ISR now runs roughly twice as often as Session 016, so keep the ISR small and avoid adding unrelated work there.
KNOWN ISSUES RESOLVED: Slow counter-clockwise missed detents, fast-spin undercount from the first slow-stability test, encoder-switch double-click sensitivity, and accelerated downward value edits stalling near zero.

NEXT SESSION RECOMMENDED GOAL: Resume the non-encoder firmware work from the Session 018 comms/MIDI state, unless new hardware testing reveals an encoder regression.
BLOCKERS: None for the encoder.

CRITICAL REMINDERS FOR NEXT SESSION:
- Current encoder truth is Session 019, not the older Session 016 timing: Timer1 ~32.05 kHz, six phase samples, fixed AB=11, rest-jump recovery, 192-sample button debounce.
- Keep encode_stableRead4() raw; acceleration belongs in the menu edit path only.
- Do not reintroduce legacy read modes, PCINT decoding, Timer0 encoder sampling, or temporary LCD/debug hooks.
- ENC_ACCEL_MAX_MULT is currently 4 because 5 felt too fast on hardware.
```
