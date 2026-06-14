# Encoder Audit - Session 019

Date: 2026-06-14

## Session 019 Test Changes

### Test 1 - 16 kHz / 3 Samples

Implemented first and hardware-tested as better for slow counter-clockwise movement:

- `front/LxrAvr/encoder.c`: raised `ENCODER_PHASE_STABLE_SAMPLES` from `2` to `3`.

At the original Timer1 compare sample rate this changed the phase acceptance window from roughly `122 us` to roughly `183 us`. The intent was to make very slow contact chatter less likely to be accepted as a real quadrature state while preserving the Session 016 Timer1 fixed-`AB=11` rest-phase FSM.

Hardware result:

- Slow behavior seemed better.
- Fast spins did not catch enough increments.

### Test 2 - 32 kHz / 4 Samples

Implemented next for hardware test:

- `front/LxrAvr/encoder.c`: raised `ENCODER_SAMPLE_HZ` from `16000UL` to `32000UL`.
- `front/LxrAvr/encoder.c`: raised `ENCODER_PHASE_STABLE_SAMPLES` from `3` to `4`.
- `front/LxrAvr/encoder.c`: changed Timer1 from prescaler 64 to prescaler 8 so the requested 32 kHz rate quantizes closely on a 20 MHz ATmega644.
- `front/LxrAvr/encoder.c`: raised `ENCODER_BUTTON_STABLE_SAMPLES` from `48` to `192`. The first 32 kHz draft used `96` to preserve the old debounce time, then this was doubled again after encoder-switch double-clicks were observed.

At 20 MHz, Timer1 CTC with prescaler 8 and `OCR1A = 77` runs at roughly `32.05 kHz`. Four phase samples at that rate produce a phase acceptance window of roughly `125 us`, close to the original Session 016 timing but with more samples in the decision. The intended test is whether the extra sample resolution recovers fast-spin increments without bringing back the slow counter-clockwise misses.

Hardware result:

- Fast-spin behavior was improved enough to keep exploring the 32 kHz sampler.
- Very slow movement could again miss increments/decrements about half the time.

### Test 3 - 32 kHz / 6 Samples Plus Rest-Jump Recovery

Implemented next for hardware test:

- `front/LxrAvr/encoder.c`: raised `ENCODER_PHASE_STABLE_SAMPLES` from `4` to `6`.
- `front/LxrAvr/encoder.c`: kept `ENCODER_SAMPLE_HZ = 32000UL`.
- `front/LxrAvr/encoder.c`: kept Timer1 prescaler 8 and `ENCODER_BUTTON_STABLE_SAMPLES = 192`.
- `front/LxrAvr/encoder.c`: added a narrow rest-jump recovery path to the FSM.

At roughly `32.05 kHz`, six phase samples produce an acceptance window of about `187 us`, close to the earlier `16 kHz / 3 samples` test that improved slow behavior.

The FSM recovery handles only this specific missing-intermediate-state shape:

```text
11 -> 00 -> 10 -> 11
```

or the opposite-direction equivalent:

```text
11 -> 00 -> 01 -> 11
```

The first jump from fixed rest `AB=11` to the opposite state `AB=00` is illegal by strict quadrature rules, so the old FSM discarded the whole physical click. The new path records that one rest-to-opposite jump as pending. If the next filtered transition moves legally out of `AB=00`, the FSM resumes as if the skipped middle states had been seen. It still requires a same-direction return to fixed rest before emitting a detent.

Any other illegal transition still resets the FSM without recovery.

## Symptom

The reported regression is directional and speed-dependent:

- Turning the main encoder very slowly counter-clockwise can produce multiple physical clicks with no visible value decrement.
- This feels like encoder performance has degraded again after the previous encoder work.

## Short Finding

At the start of this audit, I did not find any front-panel AVR encoder source changes after the Session 016 encoder fix.

The then-current `front/LxrAvr` encoder source was still the Session 016 design:

- `front/LxrAvr/encoder.c` uses Timer1 compare A sampling at `ENCODER_SAMPLE_HZ = 16000UL`.
- `ENCODER_REST_STATE` is still fixed to `0x03` (`AB=11`).
- The only public rotation read is still `encode_stableRead4()`.
- `front/LxrAvr/main.c` still drains exactly that read once per main-loop pass and passes it to `menu_parseEncoder()`.
- No later commits from Session 017 or 018 changed `front/LxrAvr/encoder.c`, `encoder.h`, `main.c`, `config.h`, or other AVR front-panel source.

So the most likely explanation is not a recent refactor accidentally changing the encoder path. The symptom fits a latent edge case in the Session 016 rest-anchored FSM: a very slow counter-clockwise click can be discarded if contact chatter or asymmetric phase settling causes one illegal filtered quadrature transition during the click.

## Evidence Checked

### Git/source state

`git log -- front/LxrAvr/encoder.c front/LxrAvr/encoder.h front/LxrAvr/main.c front/LxrAvr/config.h` shows the latest commit touching the encoder path is:

```text
a4fbe1a session 016: fafo encoder improvements
```

There is no diff from that commit to current `HEAD` for:

- `front/LxrAvr/encoder.c`
- `front/LxrAvr/encoder.h`
- `front/LxrAvr/main.c`
- `front/LxrAvr/config.h`
- `front/LxrAvr/Makefile`

The current dirty tree contains STM32/MIDI/UART refactor work and a rebuilt firmware image, but no modified AVR front-panel source files.

### Timer ownership

`rg` over `front/LxrAvr` shows Timer1 is still only used by the encoder:

- `encode_init()` configures Timer1 CTC mode and enables `TIMSK1 |= (1 << OCIE1A)`.
- `ISR(TIMER1_COMPA_vect)` lives only in `encoder.c`.
- The coarse system tick remains Timer2.
- Timer0 is not used by the current encoder implementation.

So there is no obvious timer conflict introduced after Session 016.

### Main-loop path

The current main loop still does:

```c
const int8_t encoderValue = encode_stableRead4();
const uint8_t button = encode_readButton();
menu_parseEncoder(encoderValue, button);
```

`menu_parseEncoder()` then inverts the raw encoder sign:

```c
inc = (int8_t)(inc * -1);
```

That sign inversion is old behavior in the menu layer; I did not find a new slow-counter-clockwise gate there.

## Current Decoder Behavior

The decoder is intentionally strict:

1. Timer1 samples PC0/PC1.
2. Each phase must hold the same value for `ENCODER_PHASE_STABLE_SAMPLES`.
3. A filtered state change is handed to `encoder_handleStateChange()`.
4. The FSM only starts a detent when a legal transition leaves the fixed rest state `AB=11`.
5. It emits a detent only if four legal same-direction transitions return to `AB=11`.
6. Any illegal transition resets the FSM.
7. A valid transition that does not start from rest is ignored while the FSM is idle.

For the fixed `AB=11` rest phase, the expected counter-clockwise sequence, assuming negative raw direction, is:

```text
11 -> 01 -> 00 -> 10 -> 11
```

That produces one raw negative detent, which the menu layer inverts into the intended UI direction.

## Failure Mechanism That Matches The Symptom

The vulnerable case is an illegal filtered jump while leaving rest.

For example, on a slow counter-clockwise click the ideal sequence starts:

```text
11 -> 01 -> 00 -> 10 -> 11
```

But if the A/B contacts settle asymmetrically or chatter long enough to pass the two-sample filter, the decoder can see:

```text
11 -> 00 -> 10 -> 11
```

The first transition `11 -> 00` is illegal because both bits changed. The current FSM resets on that illegal transition. Since it is now sitting at `00`, the following legal transitions are ignored because they did not leave the rest state. When the encoder reaches `11`, no detent is emitted.

That produces exactly the user-visible behavior:

- the hardware physically clicked,
- the click happened slowly enough for a bad intermediate contact state to become "stable",
- the decoder intentionally discarded the whole detent,
- the UI value did not decrement.

This also explains why the issue can be more obvious in one direction. With a fixed rest phase of `AB=11`, clockwise and counter-clockwise leave rest on different contacts. If one contact path bounces or settles worse than the other, the strict rest-FSM will lose more clicks in that direction.

## Why "Very Slowly" Matters

The original Session 016 phase filter required two consecutive samples at roughly 16 kHz:

```text
2 samples ~= 125 us
```

That is excellent for responsiveness, but it is permissive for a mechanical encoder. At very slow movement, contact chatter can dwell long enough to be accepted as real state. The FSM then sees a legal-looking but mechanically accidental state transition.

Fast motion can sometimes hide this because chatter states are shorter relative to the movement through the detent. Slow motion gives the bad state time to become filtered state.

The current Test 2 firmware instead samples at roughly `32.05 kHz` and requires four consecutive samples, giving a similar acceptance window but with finer sampling granularity.

## Things That Look Unlikely

### Recent Session 017/018 STM refactors

The later STM32 UART/MIDI refactors could affect parameter echoing or menu-state synchronization in general, but they did not change the AVR encoder source. They also do not own Timer1.

The speed/direction-specific nature of the symptom points more strongly at the AVR quadrature decoder than at STM parameter overwrite.

### Main-loop starvation

If the AVR main loop were briefly busy, `enc_delta` should accumulate complete detents in the Timer1 ISR and apply them later. That would feel delayed or batched, not like repeated very slow counter-clockwise clicks disappearing. Slow movement also gives the main loop more time, not less.

### Detent counter overflow

`enc_delta` is an `int8_t`, but slow movement is not a saturation case. Overflow/saturation is not a plausible cause of "very slow multiple clicks no decrement."

## Most Likely Root Cause

The current Session 016 rest-phase FSM is too strict for the observed slow counter-clockwise contact behavior.

It solved the earlier false-count and between-detent wiggle problems by only accepting perfect rest-to-rest quadrature cycles. The tradeoff is that any accepted illegal transition inside a physical click discards the entire click. With only a two-sample phase filter, very slow contact chatter can become an accepted illegal transition.

## Recommended Next Diagnostic

Before changing behavior again, add temporary counters or a short LCD/debug readout for:

- emitted positive detents,
- emitted negative detents,
- illegal transitions,
- direction reversals inside one detent,
- illegal transitions specifically from `AB=11`,
- ignored legal transitions while the FSM is idle but not at rest.

The key confirmation would be: when the user performs a very slow counter-clockwise click that produces no decrement, the debug counters should show either an illegal transition from rest or a direction-reversal reset before the next rest.

This can be done without reintroducing the old PCINT or Timer0 encoder implementations.

## Fix Options

### Option 1: Increase phase stability samples

Raise `ENCODER_PHASE_STABLE_SAMPLES` from `2` to `3` or `4`.

Pros:

- Minimal code change.
- Keeps the Session 016 rest-FSM architecture.
- Reduces acceptance of very short chatter states.

Cons:

- May make very fast spins less responsive.
- If the mechanical chatter lasts longer than 250 us, this will not fully solve the slow-click loss.

### Option 2: Add rest-jump recovery

Teach the FSM to remember an illegal two-bit jump from rest and resolve it if the next transition proves the direction.

Example:

```text
11 -> 00 -> 10 -> 11
```

The illegal `11 -> 00` is ambiguous by itself, but the next step `00 -> 10` implies the skipped sequence was probably counter-clockwise:

```text
11 -> 01 -> 00 -> 10 -> 11
```

Pros:

- Directly targets the missing-click symptom.
- Preserves the rest-to-rest detent rule.

Cons:

- More complex.
- Needs careful hardware testing to avoid reintroducing wiggle counts.

### Option 3: Hybrid accumulator with rest-gated emission

Return to a transition accumulator, but only emit when the state is back at fixed rest.

Pros:

- More tolerant of minor phase irregularity than the current strict FSM.
- Still prevents between-detent UI changes.

Cons:

- This is closer to the pre-Session-016 behavior that had rough reversal/fast-spin issues.
- Needs a clear reversal/reset policy.

## Recommendation

Start with instrumentation, then try `ENCODER_PHASE_STABLE_SAMPLES = 3` as the first small hardware test. If slow counter-clockwise skips remain, implement a narrowly scoped rest-jump recovery rather than replacing the whole Timer1 rest-phase architecture.

Do not reintroduce the old read modes, PCINT decoder, or Timer0 encoder path. The current regression report does not point to those being needed; it points to making the existing Timer1 fixed-rest decoder more tolerant of slow mechanical contact behavior.

## Acceleration Implementation Plan

Acceleration should be implemented only after the raw decoder test is acceptable. It should not be used to mask missed detents, because acceleration can make skipped raw counts harder to reason about.

Status: implemented after Test 3 was reported as pretty good.

The acceleration configuration is now in `front/LxrAvr/config.h`:

```c
#define ENC_ACCEL_MIN_REV_PER_SEC 1
#define ENC_ACCEL_MAX_REV_PER_SEC 2
#define ENC_ACCEL_MAX_MULT 5
```

The encoder hardware has 24 detents per full rotation. The speed estimate should be based on complete emitted detents, not raw quadrature edges.

Recommended measurement:

- Maintain a saturating interval counter in the Timer1 encoder ISR.
- On each emitted complete detent, record the saturated interval since the previous emitted detent.
- Smooth speed over the last 4 detent intervals, resetting the average on direction reversal or after a timeout.
- Four detents is one sixth of a revolution. At 1 rev/s this covers about `167 ms`; at 2 rev/s it covers about `83 ms`.

Threshold math:

- `1 rev/s` equals `24 detents/s`, or one detent every `41.7 ms`.
- `2 rev/s` equals `48 detents/s`, or one detent every `20.8 ms`.
- At roughly `32.05 kHz`, those are about `1335` and `668` Timer1 ISR ticks per detent.

Multiplier policy:

- Below `ENC_ACCEL_MIN_REV_PER_SEC`, multiplier is `1`.
- At or above `ENC_ACCEL_MAX_REV_PER_SEC`, multiplier is `ENC_ACCEL_MAX_MULT`.
- Between those speeds, scale linearly from `1` to `ENC_ACCEL_MAX_MULT`.

Application policy:

- Keep `encode_stableRead4()` as the raw detent read so diagnostics remain honest.
- Expose a small encoder speed/multiplier helper, or store the current multiplier alongside the raw read.
- Apply acceleration only in edit/value-change contexts such as `menu_encoderChangeParameter()` and `menu_encoderChangeShiftParameter()`.
- Do not accelerate menu navigation, load/save selection, or other screens where a skipped item would feel destructive.
- Clamp after acceleration using the existing parameter dtype/range logic.

First implementation target:

- Add the config defines above.
- Track a 4-detent moving average or simple EMA in the encoder module.
- Add a helper that returns an integer multiplier from `1` to `ENC_ACCEL_MAX_MULT`.
- Multiply the signed encoder delta only when edit mode is active and a parameter value is being changed.

Implemented behavior:

- `front/LxrAvr/encoder.c` tracks speed from complete emitted detents only.
- The speed estimate uses a 4-detent moving average of detent intervals.
- The interval counter saturates at the no-acceleration threshold, avoiding wraparound artifacts after idle periods.
- The average resets on direction reversal.
- Any detent interval slower than `ENC_ACCEL_MIN_REV_PER_SEC` resets acceleration back to multiplier `1`.
- `encode_stableRead4()` still returns raw detents.
- `encode_getAccelerationMultiplier()` exposes the current integer multiplier.
- `front/LxrAvr/Menu/menu.c` applies the multiplier only when edit mode is active and a normal or shifted parameter value is being changed.
- Menu navigation, load/save handling, copy/clear target selection, and other non-edit encoder uses remain unaccelerated.

Endpoint clamp follow-up:

- Hardware test with `ENC_ACCEL_MAX_MULT = 4` felt good overall, but fast downward edits could hesitate at `2` or `1` before reaching `0`.
- Root cause was the old uint8 wrap-prevention path: when an accelerated negative delta was larger than the current value, it refused the edit instead of clamping to zero.
- `front/LxrAvr/Menu/menu.c` now applies encoder deltas through a signed temporary and clamps to `0..255` before the existing dtype-specific clamps run.
- This fixes the `2 -> 1 -> 0` endpoint hesitation without changing navigation acceleration policy.
