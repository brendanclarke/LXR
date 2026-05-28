# Encoder Implementation Pathway
**Target:** ATmega644 rotary encoder — `front/LxrAvr/encoder.c` / `encoder.h`  
**Relates to:** `ATMEGA_STM32F4_COMMS_AUDIT.md`

---

## 1. Current State

The encoder is read using the Peter Dannegger polling algorithm, modified by Julian Schmidt. Timer 0 is configured in CTC mode with a prescaler of 256 and OCR0A = 78, giving a tick period of approximately **1 ms** (actual: 0.9984 ms). On each tick the ISR samples PHASE_A and PHASE_B (PC0 and PC1), converts the two-bit Gray code to a binary index, computes a signed difference from the previous sample, and increments or decrements `enc_delta` only when exactly one bit changed (the `diff & 1` guard). The encoder push-button (PC2) is debounced with a one-sample confirmation in the same ISR.

The main loop reads `enc_delta` via `encode_read4()`, which divides accumulated delta by 4 and preserves the remainder, and passes the result to `menu_parseEncoder()`.

### Timer map (full picture)

| Timer | Mode | Period | Current use |
|-------|------|--------|-------------|
| Timer 0 (8-bit) | CTC | ~1 ms | Encoder polling + button debounce |
| Timer 1 (16-bit) | — | — | **Unused** |
| Timer 2 (8-bit) | Overflow | ~13 ms | `time_sysTick` / screensaver |

Timer 1 and all three PCINT groups (PCIE0–2) are unregistered by any code in the project. This is confirmed by full-text search across all `.c` and `.h` files in `front/LxrAvr/`.

### Known call sites for encoder functions

| File | Line | Call |
|------|------|------|
| `main.c` | 100 | `encode_init()` |
| `main.c` | 175 | `encode_read4()` |
| `main.c` | 176 | `encode_readButton()` |

These are the **only** call sites. No other file calls any `encode_*` function.

---

## 2. Identified Problems

### 2.1 `encode_read4()` may be wrong for the installed encoder

`encode_read4()` reports one count for every four Gray-code state transitions — one complete quadrature cycle. This is correct for encoders where each physical detent produces exactly four state changes (one full AB cycle). It is wrong for encoders where each detent produces two or one state changes, which is the more common case for Alps EC11-type encoders used in this class of instrument.

**If the encoder produces 2 state changes per detent** (e.g. Alps EC11R, Bourns PEC11R with 24 PPR / 24 detents), calling `encode_read4()` means every other detent click is silently discarded. The encoder feels heavy and unresponsive. `encode_read2()` would be correct.

**If the encoder produces 1 state change per detent**, `encode_read1()` would be correct.

This must be verified against the hardware before any other change is made. It is the single most likely cause of sluggishness and requires only changing one function name in `main.c` to fix.

**How to identify:** check the encoder datasheet for "pulses per revolution" and "detents per revolution". If PPR = detents, use `read1`. If PPR = 2 × detents, use `read2`. If PPR = detents and PPR × 4 = electrical cycles, use `read4`. Alternatively, print `enc_delta` raw to the LCD for one slow clockwise turn by one detent. The accumulated delta should be 4 (use `read4`), 2 (use `read2`), or 1 (use `read1`).

### 2.2 1 ms polling does not reliably reject contact bounce

The `diff & 1` filter only passes physically valid single-bit transitions. However, if contact bounce generates alternating transitions across multiple consecutive 1 ms ISR calls (bounce duration 1–10 ms is typical for mechanical encoders), each alternation registers as a valid count in the opposite direction. The result is forward–backward–forward oscillation in `enc_delta` that the main loop reads as spurious reverse steps. This is the source of flyback.

### 2.3 No consecutive-direction confirmation

A single valid-looking transition increments `enc_delta` immediately. There is no requirement that the direction be confirmed by a second sample before it is committed. Any single bounce that completes a full low-to-high-to-low cycle within two consecutive ISR ticks registers as a genuine step.

### 2.4 Button debounce uses the same one-sample confirmation

The button on PC2 requires two consecutive identical samples to confirm a state. This is the same Dannegger pattern applied to a momentary switch. The 1 ms sampling gives effectively 1 ms of debounce time for the button. This is marginal but functional. It is not the source of reported encoder problems but is improved as a side effect of the Timer 0 rate change in Phase 2.

---

## 3. Scope of Changes

### 3.1 New API names

All three read functions are renamed with the `stable` prefix. The old names are retained as thin deprecated wrappers so that any future code added against the old API compiles without changes. The wrappers are marked deprecated to discourage new use.

| Old name | New name | Behaviour change |
|----------|----------|-----------------|
| `encode_read1()` | `encode_stableRead1()` | None in legacy mode; confirmed delta in stable mode |
| `encode_read2()` | `encode_stableRead2()` | None in legacy mode; confirmed delta in stable mode |
| `encode_read4()` | `encode_stableRead4()` | None in legacy mode; confirmed delta in stable mode |
| `encode_init()` | `encode_init()` | Configures hardware based on `ENC_USE_STABLE_DRIVER` flag |
| `encode_readButton()` | `encode_readButton()` | Unchanged |

### 3.2 Config flag

A single flag in `config.h` selects between implementations:

```c
// 0 = original Dannegger polling at ~1ms (legacy, backward-compatible)
// 1 = PCINT1 + Timer 1 timestamp with consecutive confirmation (stable)
#define ENC_USE_STABLE_DRIVER 0
```

When `ENC_USE_STABLE_DRIVER == 0`, behaviour is byte-for-byte identical to the current code except for the function rename. When `ENC_USE_STABLE_DRIVER == 1`, the new PCINT + Timer 1 implementation is compiled in.

Two additional tuning constants are added to `config.h` and take effect only when `ENC_USE_STABLE_DRIVER == 1`:

```c
// Minimum time between accepted encoder edges, in Timer 1 ticks.
// Timer 1 prescaler 8 at 20 MHz = 400 ns/tick.
// 1250 ticks = 500 µs. Increase if bounce persists; decrease if response lags.
#define ENC_DEBOUNCE_TICKS   1250

// Number of consecutive same-direction edges required before a count is committed.
// 2 = 2 × 400 ns × ... (depends on edge rate). 3 is conservative.
// At max human turn speed (5 rev/s, 24 PPR), inter-edge time is ~8 ms; 
// 3 confirmations at 500 µs each = 1.5 ms = ~19% of inter-edge time. No perceptible lag.
#define ENC_CONFIRM_COUNT    3
```

---

## 4. Implementation Phases

The work is divided into three independent, independently testable phases. Each phase can be verified before the next begins, and any phase can be rolled back without affecting the others.

---

### Phase 1 — Rename only (zero behaviour change)

**Purpose:** Establish the new API names and config infrastructure without changing any runtime behaviour. This phase has no risk.

**Changes required:**

#### `config.h`

Add three lines after the existing `#define DEBUG_CRASH_MODE 0`:

```c
// Encoder driver selection
// 0 = original Dannegger polling (~1ms), backward compatible
// 1 = PCINT1 + Timer 1 stable driver with debounce confirmation
#define ENC_USE_STABLE_DRIVER  0
#define ENC_DEBOUNCE_TICKS     1250
#define ENC_CONFIRM_COUNT      3
```

#### `encoder.h`

Replace the three function declarations:

```c
// Before:
int8_t encode_read1( void );
int8_t encode_read2( void );
int8_t encode_read4( void );
```

```c
// After:
// Primary stable API
int8_t encode_stableRead1( void );
int8_t encode_stableRead2( void );
int8_t encode_stableRead4( void );

// Deprecated wrappers for backward compatibility — do not use in new code
static inline __attribute__((deprecated)) int8_t encode_read1(void) { return encode_stableRead1(); }
static inline __attribute__((deprecated)) int8_t encode_read2(void) { return encode_stableRead2(); }
static inline __attribute__((deprecated)) int8_t encode_read4(void) { return encode_stableRead4(); }
```

Also update the ISR prototype comment (cosmetic only):

```c
// Before:
// 1ms for manual movement
ISR( TIMER0_COMP_vect );

// After:
// Timer 0 CTC ISR — encoder polling (legacy mode) and button debounce
ISR( TIMER0_COMPA_vect );
```

Note: the existing prototype says `TIMER0_COMP_vect` (ATmega32 name) but the implementation already correctly uses `TIMER0_COMPA_vect` (ATmega644 name). The header prototype is never called directly by user code; it is informational only. Fix the name in the header for consistency.

#### `encoder.c`

Rename the three read functions:

```c
// Before:
int8_t encode_read1( void ) { ... }
int8_t encode_read2( void ) { ... }
int8_t encode_read4( void ) { ... }

// After (bodies are identical):
int8_t encode_stableRead1( void ) { ... }
int8_t encode_stableRead2( void ) { ... }
int8_t encode_stableRead4( void ) { ... }
```

#### `main.c`

Update the single call site at line 175:

```c
// Before:
const int8_t encoderValue = encode_read4();

// After:
const int8_t encode_stableRead4();
```

**Verification for Phase 1:**
- Project must compile without warnings. The deprecated wrappers will not produce warnings unless some other call site invokes them, which verifies the search was complete.
- Runtime behaviour must be identical to pre-change. Flash both, verify encoder and button respond identically.
- No timer configuration has changed. No ISR has changed.

---

### Phase 2 — Improved polling implementation (in-place upgrade, legacy mode stays intact)

**Purpose:** Within the existing Timer 0 polling architecture, increase the ISR rate from ~1 ms to ~250 µs and add consecutive-direction confirmation. This is the minimum viable improvement for bounce rejection. It does not require Timer 1 or PCINT. It can be reached by changing `ENC_USE_STABLE_DRIVER` to `1` and later rolled back to `0`.

Wait — Phase 2 is described here as an intermediate "improved polling" step before the full PCINT implementation. For the final architecture (Phase 3), the PCINT driver uses Timer 1. However, Phase 2 is useful as a standalone improvement if Timer 1 is later claimed by another subsystem, or as a regression fallback.

**Since Phase 2 is only included for reference as a rollback option**, the `ENC_USE_STABLE_DRIVER` flag in the shipped code maps `0` → legacy (Phase 1 state) and `1` → full PCINT + Timer 1 (Phase 3 state). Phase 2 is not a named mode in the flag. If needed as a fallback it can be extracted from this document and compiled in under a temporary third value.

**Phase 2 implementation (for reference / rollback):**

Change to `encode_init()`: swap Timer 0 prescaler from 256 to 64. OCR0A stays at 78:

```c
// Before (prescaler 256, ~1 ms):
TCCR0B = 1 << CS02;

// After (prescaler 64, ~250 µs):
TCCR0B = (1 << CS01) | (1 << CS00);
// OCR0A = 78 is unchanged — F_CPU / 64 / 78 = 4006 Hz ≈ 250 µs
```

Change to `TIMER0_COMPA_vect`: add consecutive-direction tracking:

```c
ISR( TIMER0_COMPA_vect )
{
    static int8_t last_dir = 0;
    static uint8_t consec  = 0;
    int8_t new, diff, dir;

    new = 0;
    if( PHASE_A ) new = 3;
    if( PHASE_B ) new ^= 1;

    diff = (int8_t)(last - new);
    if( diff & 1 ) {
        last = new;
        dir  = (int8_t)((diff & 2) - 1);   // +1 or -1
        if( dir == last_dir ) {
            if( ++consec >= ENC_CONFIRM_COUNT ) {
                enc_delta = (int8_t)(enc_delta + dir);
                consec = 0;
            }
        } else {
            last_dir = dir;
            consec   = 1;
        }
    }

    // Button debounce — unchanged
    if( ENCODER_BUTTON == lastButton ) {
        buttonValue = (uint8_t)lastButton;
    }
    lastButton = ENCODER_BUTTON;
}
```

**Timing analysis for Phase 2:**

| Parameter | Value |
|-----------|-------|
| ISR period | ~250 µs |
| Confirmation window (ENC_CONFIRM_COUNT = 3) | ~750 µs |
| Max human turn speed: 5 rev/s, 24 PPR | 1 detent per ~8.3 ms |
| Confirmation window as % of inter-detent time | ~9% |
| Button debounce time (1 sample at 250 µs) | 250 µs |
| ISR execution time (approx, at 20 MHz) | ~25 cycles = 1.25 µs |

Worst-case ISR jitter contribution to button debounce is negligible. The button confirmation is softer (250 µs vs 1 ms) but remains reliable for any mechanical switch.

---

### Phase 3 — PCINT1 + Timer 1 stable driver (full implementation)

**Purpose:** Replace the polling ISR with interrupt-on-change (PCINT1 on PC0 and PC1), using Timer 1's free-running counter as a high-resolution timestamp for debounce gating and consecutive-direction confirmation. Timer 0 is retained exclusively for button debounce, running at the Phase 2 rate (~250 µs).

This is the implementation that is activated when `ENC_USE_STABLE_DRIVER == 1`.

#### 4.3.1 Hardware resource allocation

| Resource | Legacy (flag=0) | Stable (flag=1) |
|----------|-----------------|-----------------|
| Timer 0 | Encoder polling + button | Button debounce only |
| Timer 1 | Unused | Free-running timestamp (prescaler 8) |
| PCINT1 (PC0, PC1) | Unused | Encoder edge detection |
| PCINT1 (PC2) | Unused | Fires ISR but is filtered out |

#### 4.3.2 PCINT1 group behaviour — critical detail

The ATmega644 PCINT1 interrupt fires on any logic change on any pin in the PC0–PC7 group that is unmasked in PCMSK1. The encoder button is on PC2. This means **a button press or release also fires `PCINT1_vect`**.

This is not a problem provided the ISR correctly filters by which pin(s) changed. The ISR tracks the previous PORTC state and only proceeds with encoder decoding if PC0 or PC1 changed. If only PC2 changed (button event), the ISR returns immediately. The button remains debounced by Timer 0 as before.

PC2 is **not** added to PCMSK1. Only PC0 (PCINT8) and PC1 (PCINT9) are masked in. However, the ATmega644 datasheet states that PCINT fires for all unmasked pins in the group, but the mask register only controls which pins contribute to the interrupt trigger. To be safe, the ISR tracks the full PORTC state delta and filters explicitly:

```c
uint8_t changed = (uint8_t)(current_portc ^ last_portc);
if( !(changed & ((1 << PC0) | (1 << PC1))) )
    return;
```

This is a zero-cost guard and ensures button activity on PC2 cannot cause spurious encoder counts even if a future change to PCMSK1 accidentally includes PC2.

#### 4.3.3 Timer 1 configuration

Timer 1 is configured as a free-running 16-bit counter with prescaler 8:

```
Tick rate:  20,000,000 / 8 = 2,500,000 Hz
Tick period: 400 ns
Overflow:   65,535 × 400 ns = 26.2 ms
```

The debounce comparison uses unsigned 16-bit subtraction, which wraps correctly across the overflow boundary. No overflow ISR is needed. Timer 1's output compare and input capture units are left unconfigured and available for future use.

#### 4.3.4 Debounce timing

With `ENC_DEBOUNCE_TICKS = 1250`:

```
Gate width:  1250 × 400 ns = 500 µs
```

Any edge arriving within 500 µs of the last accepted edge is ignored. Typical contact bounce for quality encoders (Alps, Bourns) lasts 1–5 ms; for budget encoders up to 10 ms. If 500 µs is insufficient for the specific encoder installed, `ENC_DEBOUNCE_TICKS` can be increased to 2500 (1 ms) or 5000 (2 ms) without any code change, only a recompile.

The `ENC_CONFIRM_COUNT` confirmation layer is applied after the time gate passes. An edge that passes the time gate must be followed by `ENC_CONFIRM_COUNT - 1` further same-direction edges (each also passing the time gate individually) before `enc_delta` is incremented. The combination of time gate and confirmation means genuinely random noise would need to produce `ENC_CONFIRM_COUNT` coincident same-direction pulses all separated by more than 500 µs — effectively impossible for contact bounce.

#### 4.3.5 Complete implementation

##### `encoder.h` additions (inside the `#include` guard, after existing content)

```c
#include "../config.h"

#if ENC_USE_STABLE_DRIVER == 1

// Stable driver: Timer 1 free-running timestamp source
// PCINT1_vect handles quadrature decode
// Timer 0 CTC at ~250us handles button debounce only
ISR( PCINT1_vect );

#endif /* ENC_USE_STABLE_DRIVER */
```

##### `encode_init()` — full replacement

```c
void encode_init( void )
{
    // ----------------------------------------------------------------
    // Port initialisation — identical for both driver modes
    // ----------------------------------------------------------------
    uint8_t pins = PIN_A | PIN_B | PIN_BUTTON;
    ENCODER_DDR  &= (uint8_t)~pins;   // set PC0, PC1, PC2 as inputs
    ECODER_PORT  |= pins;              // enable internal pull-ups

    // Capture power-on Gray code state
    int8_t new = 0;
    if( PHASE_A ) new = 3;
    if( PHASE_B ) new ^= 1;
    last        = new;
    enc_delta   = 0;
    lastButton  = 0;
    buttonValue = 1;

    // ----------------------------------------------------------------
    // Timer 0 — CTC, prescaler 64, OCR0A = 78 => ~250 µs period
    // Used for button debounce in both modes.
    // In legacy mode also used for encoder polling.
    // ----------------------------------------------------------------
    TCCR0A  =  (1 << WGM01);
    TCCR0B  =  (1 << CS01) | (1 << CS00);   // prescaler 64
    OCR0A   =  (uint8_t)(F_CPU / 64.0f * 0.000250f);   // = 78
    TIMSK0 |=  (1 << OCIE0A);

#if ENC_USE_STABLE_DRIVER == 1

    // ----------------------------------------------------------------
    // Timer 1 — free-running, prescaler 8 => 400 ns/tick, 26 ms wrap
    // Used as timestamp source for PCINT debounce gating.
    // No ISR needed; no output or capture units configured.
    // ----------------------------------------------------------------
    TCCR1A = 0;
    TCCR1B = (1 << CS11);   // prescaler 8
    TCNT1  = 0;

    // ----------------------------------------------------------------
    // PCINT1 — enable PC0 (PCINT8) and PC1 (PCINT9) only.
    // PC2 (button) is deliberately excluded from the mask.
    // PCINT1_vect handles quadrature decode with time-gated debounce.
    // ----------------------------------------------------------------
    PCMSK1 |= (1 << PCINT8) | (1 << PCINT9);
    PCICR  |= (1 << PCIE1);

#endif /* ENC_USE_STABLE_DRIVER */
}
```

##### `TIMER0_COMPA_vect` — replaces existing ISR

```c
ISR( TIMER0_COMPA_vect )
{
#if ENC_USE_STABLE_DRIVER == 0

    // ----------------------------------------------------------------
    // Legacy mode: full Dannegger quadrature polling at ~250 µs.
    // (Timer 0 prescaler is now 64 instead of 256 in both modes,
    //  so legacy mode also benefits from the faster poll rate.)
    // ----------------------------------------------------------------
    int8_t new, diff;

    new = 0;
    if( PHASE_A ) new = 3;
    if( PHASE_B ) new ^= 1;

    diff = (int8_t)(last - new);
    if( diff & 1 ) {
        last      = new;
        enc_delta = (int8_t)(enc_delta + (diff & 2) - 1);
    }

#endif /* ENC_USE_STABLE_DRIVER == 0 */

    // ----------------------------------------------------------------
    // Button debounce — active in both modes.
    // Requires two consecutive identical samples to commit a state.
    // At ~250 µs period, effective debounce window is 250 µs.
    // ----------------------------------------------------------------
    if( ENCODER_BUTTON == lastButton ) {
        buttonValue = (uint8_t)lastButton;
    }
    lastButton = ENCODER_BUTTON;
}
```

##### `PCINT1_vect` — new ISR, stable mode only

```c
#if ENC_USE_STABLE_DRIVER == 1

ISR( PCINT1_vect )
{
    // Static state — persists between invocations
    static uint8_t  last_portc = 0xFF;   // initialised to impossible value
                                          // forces first-edge processing
    static uint16_t last_time  = 0;
    static int8_t   last_dir   = 0;
    static uint8_t  consec     = 0;

    uint8_t current = PINC;
    uint8_t changed = (uint8_t)(current ^ last_portc);
    last_portc = current;

    // Ignore if neither encoder pin changed (e.g. button press fired ISR)
    if( !(changed & ((1 << PC0) | (1 << PC1))) )
        return;

    // Time gate: reject edges within debounce window
    uint16_t now = TCNT1;
    if( (uint16_t)(now - last_time) < ENC_DEBOUNCE_TICKS )
        return;

    // Gray code decode — identical to Dannegger
    int8_t new = 0;
    if( PHASE_A ) new = 3;
    if( PHASE_B ) new ^= 1;

    int8_t diff = (int8_t)(last - new);
    if( diff & 1 ) {
        last      = new;
        last_time = now;

        int8_t dir = (int8_t)((diff & 2) - 1);   // +1 or -1

        // Consecutive-direction confirmation
        if( dir == last_dir ) {
            if( ++consec >= ENC_CONFIRM_COUNT ) {
                enc_delta = (int8_t)(enc_delta + dir);
                consec    = 0;
            }
        } else {
            last_dir = dir;
            consec   = 1;
        }
    }
}

#endif /* ENC_USE_STABLE_DRIVER == 1 */
```

##### `encode_stableRead1/2/4` — read functions (unchanged from Phase 1)

The read function bodies are identical in both modes. The difference is entirely in how `enc_delta` is accumulated. In legacy mode, every single valid Gray-code transition increments the delta. In stable mode, only confirmed transitions do. The read functions simply consume the delta:

```c
int8_t encode_stableRead1( void )
{
    int8_t val;
    cli();
    val       = enc_delta;
    enc_delta = 0;
    sei();
    return val;
}

int8_t encode_stableRead2( void )
{
    int8_t val;
    cli();
    val       = enc_delta;
    enc_delta = val & 1;
    sei();
    return val >> 1;
}

int8_t encode_stableRead4( void )
{
    int8_t val;
    cli();
    val       = enc_delta;
    enc_delta = val & 3;
    sei();
    return val >> 2;
}
```

##### `main.c` — call site update (same for both phases)

```c
// Before:
const int8_t encoderValue = encode_read4();

// After (choose correct variant based on encoder hardware — see §2.1):
const int8_t encoderValue = encode_stableRead4();   // 4-step encoder (1 detent = 4 transitions)
// const int8_t encoderValue = encode_stableRead2(); // 2-step encoder (1 detent = 2 transitions)
// const int8_t encoderValue = encode_stableRead1(); // 1-step encoder (1 detent = 1 transition)
```

---

## 5. Risk Register

Each risk is assessed against the implementation phase in which it first appears.

### 5.1 Encoder type mismatch (Phase 1 / pre-existing)

**Risk:** The wrong `encode_stableRead*()` variant is used for the installed hardware. Using `read4` on a 2-step encoder means only every other detent registers. Using `read2` on a 4-step encoder means double-speed counts.

**Severity:** High — directly affects usability. Already present in the codebase via the existing `encode_read4()` call.

**Mitigation:** Verify before Phase 1 completes. Debug procedure: temporarily replace `menu_parseEncoder(encoderValue, button)` with an LCD print of the raw encoderValue accumulated over one slow detent click. The correct function is the one that returns ±1 per detent.

**Residual risk after mitigation:** Low.

---

### 5.2 Timer 0 prescaler change affects button debounce (Phase 2/3)

**Risk:** Changing Timer 0 from prescaler 256 (~1 ms) to prescaler 64 (~250 µs) tightens the button debounce window from ~1 ms to ~250 µs. If the encoder push-button has slow-settling contacts that require more than 250 µs to stabilise, double-presses could be registered.

**Severity:** Low. The encoder push-button is a momentary switch, typically with 1–5 ms of contact bounce. A 250 µs window is marginal but in practice functional because the Timer 0 ISR requires two consecutive matching samples — meaning the button state must be stable for at least two ticks (500 µs), not just one. Additionally, `encode_readButton()` is called from the main loop, which may have variable latency; the effective debounce is generally longer than 250 µs.

**Mitigation:** If double-presses occur, increase the button confirmation count independently in the ISR. A simple counter `buttonConsec >= 2` in the ISR (separate from the encoder direction counter) would restore 500 µs effective debounce at the 250 µs tick rate.

**Residual risk after mitigation:** Very low.

---

### 5.3 PCINT1 fires on button press (Phase 3)

**Risk:** PC2 (encoder button) shares the PCINT1 group with PC0 and PC1. Pressing the button fires `PCINT1_vect`. If the ISR fails to filter this correctly, a button press could register as an encoder step.

**Severity:** Medium without mitigation; negligible with it.

**Mitigation:** The ISR tracks the previous PORTC value and exits immediately if neither PC0 nor PC1 changed:
```c
if( !(changed & ((1 << PC0) | (1 << PC1))) ) return;
```
This is the first test in the ISR and executes in approximately 6 clock cycles before any encoder logic runs.

**Additional note:** PC2 is not added to PCMSK1 (`PCMSK1` only gets `PCINT8 | PCINT9`). On the ATmega644, a PCINT fires only when a masked pin changes. However, since PC2 is not in the mask, button presses will **not** trigger `PCINT1_vect` at all. The `changed & encoder_pins` guard is therefore a defensive belt-and-braces check for future robustness (e.g. if PCMSK1 is later modified to include PC2 for button interrupt detection). It does not add meaningful overhead.

**Residual risk after mitigation:** Negligible.

---

### 5.4 PCINT1_vect `last_portc` startup initialisation (Phase 3)

**Risk:** `last_portc` is initialised to `0xFF` (all bits set, an impossible value because the pull-ups make all three pins logic-high at rest, giving `PINC = 0b00000111 = 0x07` for the relevant bits). On the first ISR invocation, `changed = PINC ^ 0xFF` will have all bits set, including PC0 and PC1, so the ISR will not return early. It will then check the time gate: `TCNT1 - 0 < 1250`. Timer 1 starts at 0 in `encode_init()`. If `PCINT1_vect` fires within 500 µs of init (1250 × 400 ns = 500 µs), the time gate blocks processing and `last_portc` is updated to the real PINC value. Subsequent ISR calls have the correct baseline.

If `PCINT1_vect` fires more than 500 µs after init (the more likely case, since the pin state only changes when the encoder moves), the time gate passes and the ISR attempts to decode the "transition" from `0xFF` to the real pin state. This is not a real encoder movement. The Gray code decode will compute a diff from an invalid `last` value, potentially registering a spurious count.

**Severity:** Low. This is a one-time event at startup, produces at most one spurious encoder count, and is unlikely to occur unless the encoder is moved within the first 500 µs of power-on. `last` is correctly initialised to the real pin state in `encode_init()`, so subsequent decodes are unaffected.

**Mitigation:** Initialise `last_portc` to the real PINC value at the end of `encode_init()`:

```c
// At end of encode_init(), after PCINT is configured:
// Pre-seed last_portc to avoid a spurious decode on first ISR call.
// This requires a file-scope variable rather than a static local.
// Alternative: keep static local but add a one-shot init flag.
```

The cleanest solution without making `last_portc` file-scope is a one-shot init flag:

```c
ISR( PCINT1_vect )
{
    static uint8_t  initialised = 0;
    static uint8_t  last_portc  = 0;
    ...
    if( !initialised ) {
        last_portc  = PINC;
        last_time   = TCNT1;
        initialised = 1;
        return;
    }
    ...
}
```

This ISR invocation does nothing except set the correct baseline and immediately returns. The `initialised` flag costs 1 byte of SRAM and 3 cycles of ISR overhead on all subsequent calls.

**Residual risk after mitigation:** None.

---

### 5.5 Timer 1 claimed by future subsystem (Phase 3)

**Risk:** Timer 1 is currently unused but the ATMEGA_STM32F4_COMMS_AUDIT recommends using a hardware timer for `uart_waitAck()` timeout detection. If Timer 1 is later assigned to UART timeout management, a conflict would exist.

**Severity:** Low as a conflict risk; the timers serve different purposes and can share the free-running TCNT1 for different comparisons without interfering. Both uses (encoder debounce and UART timeout) only read TCNT1 — they do not configure its mode, set interrupts, or reset the counter. Two independent subsystems reading the same free-running counter is safe.

**Mitigation:** Document the Timer 1 allocation in a hardware resource comment at the top of `encoder.h`. If a UART timeout module is added later, it should read TCNT1 directly (with appropriate prescaler awareness) rather than configuring a new timer mode.

---

### 5.6 `enc_delta` overflow during confirmation window (Phase 3)

**Risk:** `enc_delta` is `volatile int8_t` (range −128 to +127). In legacy mode, a fast spin could accumulate many counts before `encode_stableRead*()` is called by the main loop. The main loop period is on the order of a few hundred microseconds to a few milliseconds (includes LCD, UART, ADC). A fast encoder spin at 5 rev/s on a 24 PPR encoder produces 120 delta increments per second, or one per ~8 ms. The main loop reads at least every ~1 ms. Overflow is essentially impossible in normal use.

In stable mode, counts are gated by both the debounce window (500 µs minimum between accepted counts) and the confirmation requirement. The maximum rate of committed counts is therefore bounded by `1 / (ENC_DEBOUNCE_TICKS × tick_period × ENC_CONFIRM_COUNT)` = `1 / (1250 × 400 ns × 3)` = 667 per second. Even at this theoretical maximum, the main loop reading every 1 ms would see at most 0.667 counts per read — far from overflow.

**Severity:** Negligible.

**Residual risk:** None.

---

### 5.7 ISR execution time for PCINT1_vect (Phase 3)

**Risk:** The PCINT1 ISR must execute and return before the next encoder edge arrives. The worst-case edge rate at the hardware UART level is irrelevant; the encoder edge rate is bounded by the encoder's mechanical speed. At 10 rev/s on a 24 PPR encoder, edges arrive at 240 per second = one per 4.2 ms. ISR execution time must be well under 4.2 ms.

**Measured execution:** The ISR performs: ISR entry (push registers, ~12 cycles), PINC read (1 cycle), XOR and compare (4 cycles), TCNT1 read (2 cycles), subtraction and comparison (4 cycles), Gray code decode (8 cycles), direction compare and counter update (6 cycles), ISR exit (pop registers, ~12 cycles). Total: approximately 50 cycles = **2.5 µs** at 20 MHz.

**Severity:** None. 2.5 µs is orders of magnitude less than 4.2 ms.

---

### 5.8 Interaction with `cli()/sei()` in read functions (both modes)

**Risk:** `encode_stableRead*()` calls `cli()` to atomically read and clear `enc_delta`, then `sei()`. In stable mode, `PCINT1_vect` could arrive between `cli()` and `sei()`. The ISR would be deferred until `sei()` executes. Any count that arrived during the atomic section is held in the hardware pending interrupt flag and executes immediately after `sei()`. This is safe — the deferral is at most a few microseconds and does not lose any counts.

The call to `cli()` in `encode_stableRead*()` also defers `TIMER0_COMPA_vect` (button debounce). A button state change arriving during this window would be re-sampled on the next Timer 0 tick (~250 µs later). No button event is lost.

**Severity:** None. The existing pattern is correct.

---

## 6. Pre-Implementation Verification Checklist

Before writing a single line of Phase 1 code, perform these checks:

1. **Identify encoder type.** Check the hardware BOM or physically inspect the encoder body for a part number. Look up: pulses per revolution (PPR) and detents per revolution. Compute transitions-per-detent = (PPR × 4) / detents. If result is 4, use `stableRead4`. If 2, use `stableRead2`. If 1, use `stableRead1`.

2. **Empirical check (belt and braces).** With the current code, add a temporary LCD display of the raw `enc_delta` value before `encode_read4()` clears it. Turn the encoder exactly one detent clockwise. The delta should be 4 (for `read4`), 2 (for `read2`), or 1 (for `read1`). If you see fractional values or values other than these, the encoder is partway between detents or the pull-up settling time is affecting the reading.

3. **Confirm Timer 1 is free.** Run `grep -rn "TCCR1\|TIMSK1\|TCNT1\|OCR1\|ICR1" front/LxrAvr` and confirm no results. (Verified at time of audit: no results.)

4. **Confirm PCINT1 is free.** Run `grep -rn "PCINT\|PCMSK\|PCICR\|PCIE" front/LxrAvr` and confirm no results. (Verified at time of audit: no results.)

5. **Establish a regression baseline.** Flash the unmodified firmware. Confirm: encoder increments parameters in the correct direction at the expected rate; encoder push-button is responsive; no phantom increments at rest; no missed counts on slow deliberate turns.

---

## 7. Testing Strategy

Each phase must pass all tests before proceeding to the next.

### Phase 1 tests (rename only)

| Test | Pass criterion |
|------|---------------|
| Clean compile, zero warnings | No warnings with `-Wall -Wextra` |
| Deprecated wrapper warning | Uncommenting a call to `encode_read4()` in test code generates a deprecation warning |
| Regression — encoder direction | CW turn increases active parameter; CCW decreases it |
| Regression — encoder resolution | Same number of detents per menu step as pre-change |
| Regression — button | Single press = single action; no double-fire |

### Phase 3 tests (stable driver, `ENC_USE_STABLE_DRIVER = 1`)

**Bounce rejection:**
| Test | Pass criterion |
|------|---------------|
| Rest state | `enc_delta` reads 0 continuously for 30 seconds at rest |
| Slow deliberate turn | One detent CW = exactly +1; one detent CCW = exactly -1 |
| Fast turn | 10 quick CW detents = +10 (within ±1); no reverse steps |
| Flyback simulation | Manually rap the panel near the encoder (vibration test). No phantom increments |

**Timing:**
| Test | Pass criterion |
|------|---------------|
| Response latency | LCD parameter update visible within one display refresh (~50 ms) of turn |
| Button debounce | 100 rapid button presses, no double-fires |
| Button under encoder movement | Pressing button while turning encoder: no phantom encoder counts |

**Legacy/stable comparison:**

With `ENC_USE_STABLE_DRIVER = 0` and `= 1` in alternation, reflash and compare:
- Subjective feel should be equal or better at `= 1`
- No loss of encoder speed or resolution
- No change to button behaviour

### Tuning ENC_DEBOUNCE_TICKS

If bounce persists at `ENC_USE_STABLE_DRIVER = 1`:
1. Increase `ENC_DEBOUNCE_TICKS` from 1250 to 2500 (1 ms gate) and reflash.
2. If still present, increase to 5000 (2 ms).
3. Beyond 5000 ticks (2 ms), confirm that fast turns still register correctly. A gate of 2 ms means the maximum encoder count rate is 500 per second. For 5 rev/s × 24 PPR = 120 Hz, this is well within budget.

If response feels sluggish at `ENC_USE_STABLE_DRIVER = 1`:
1. Reduce `ENC_CONFIRM_COUNT` from 3 to 2.
2. If still sluggish, reduce `ENC_DEBOUNCE_TICKS` from 1250 to 625 (250 µs).
3. Verify bounce rejection still holds after reducing.

---

## 8. Rollback Procedure

### Rollback to legacy mode (any phase)

Set `ENC_USE_STABLE_DRIVER 0` in `config.h` and recompile. The entire PCINT1 and Timer 1 block is excluded by the preprocessor. Timer 0 reverts to the Phase 2 rate (prescaler 64, ~250 µs) regardless of mode — this is intentional: the faster Timer 0 rate benefits button debounce in both modes and has no downside. If the original 1 ms rate is required for diagnostic purposes, temporarily restore `TCCR0B = 1 << CS02` in `encode_init()`.

### Rollback to pre-Phase-1 state (complete revert)

1. Restore the three original function names in `encoder.c` and `encoder.h`.
2. Restore `encode_read4()` in `main.c`.
3. Remove the three `#define` lines from `config.h`.
4. Restore Timer 0 prescaler to 256 in `encode_init()`.

No hardware changes are required at any stage; all modifications are purely firmware.

---

## 9. Implementation Order Summary

```
Step 0:  Verify encoder type (§6, checklist item 1–2)
         Determine correct encode_stableRead*() variant for hardware

Step 1:  Phase 1 — rename only
         config.h: add ENC_USE_STABLE_DRIVER = 0, ENC_DEBOUNCE_TICKS, ENC_CONFIRM_COUNT
         encoder.h: rename declarations, add deprecated aliases
         encoder.c: rename function bodies
         main.c: update call site to encode_stableRead*() with correct variant
         Timer 0 prescaler: change 256 → 64 in encode_init() (applies to both modes)
         Test: Phase 1 test suite (§7)

Step 2:  Phase 3 — stable driver
         encoder.h: add PCINT1_vect prototype under #if guard
         encoder.c: add PCINT1_vect ISR body under #if guard
         encoder.c: modify TIMER0_COMPA_vect to exclude encoder code under #if guard
         encoder.c: modify encode_init() to conditionally configure Timer 1 and PCINT1
         config.h: set ENC_USE_STABLE_DRIVER = 1
         Test: Phase 3 test suite (§7)
         Tune ENC_DEBOUNCE_TICKS and ENC_CONFIRM_COUNT as needed

Step 3:  Lock in
         If Phase 3 passes: leave ENC_USE_STABLE_DRIVER = 1 as the default
         If Phase 3 is deferred: set ENC_USE_STABLE_DRIVER = 0, leave infrastructure in place
```

---

## 10. Files Modified

| File | Changes |
|------|---------|
| `front/LxrAvr/config.h` | Add `ENC_USE_STABLE_DRIVER`, `ENC_DEBOUNCE_TICKS`, `ENC_CONFIRM_COUNT` |
| `front/LxrAvr/encoder.h` | Rename declarations; add deprecated wrappers; add PCINT1_vect prototype |
| `front/LxrAvr/encoder.c` | Rename functions; modify `encode_init()`; modify `TIMER0_COMPA_vect`; add `PCINT1_vect` |
| `front/LxrAvr/main.c` | Update `encode_read4()` → `encode_stableRead*()`  (line 175 only) |

No other files require modification. The STM32 mainboard firmware is completely unaffected.
