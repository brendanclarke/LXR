/************************************************************************/
/*                                                                      */
/*                      Reading rotary encoder                          */
/*                      one, two and four step encoders supported       */
/*                                                                      */
/*              Author: Peter Dannegger                                 */
/*                                                                      */
/************************************************************************/
#include "encoder.h"

                // target: ATmega644
//------------------------------------------------------------------------

volatile int8_t     enc_delta;          // -128 ... 127
static int8_t       last;
static int8_t       lastButton;
volatile uint8_t    buttonValue;


void encode_init( void )
{
    // port init — set PC0, PC1, PC2 as inputs with pull-ups
    uint8_t pins = PIN_A |
                   PIN_B |
                   PIN_BUTTON;

    ENCODER_DDR &= (uint8_t)~pins;     // configure as input
    ECODER_PORT |= pins;               // enable internal pull-ups

    // Capture power-on Gray code state
    int8_t new = 0;
    if( PHASE_A )
        new = 3;
    if( PHASE_B )
        new ^= 1;                       // convert gray to binary
    last      = new;                    // power on state
    enc_delta = 0;

    // ----------------------------------------------------------------
    // Timer 0 — CTC, prescaler 64, OCR0A = 78 => ~250 us period.
    // Used for button debounce in both driver modes.
    // In legacy mode (ENC_USE_STABLE_DRIVER == 0) also handles encoder
    // polling. Faster than the original prescaler-256 (~1 ms) setup;
    // this benefits button debounce in both modes at no cost.
    // ----------------------------------------------------------------
    TCCR0A  =  (1 << WGM01);                       // CTC mode
    TCCR0B  =  (1 << CS01) | (1 << CS00);          // prescaler 64
    OCR0A   =  (uint8_t)(F_CPU / 64.0f * 0.000250f); // = 78 => ~250 us
    TIMSK0 |=  (1 << OCIE0A);

#if ENC_USE_STABLE_DRIVER == 1

    // ----------------------------------------------------------------
    // Timer 1 — free-running 16-bit counter, prescaler 8.
    // Tick period: 8 / 20000000 = 400 ns. Overflow: ~26.2 ms.
    // Used as timestamp source for PCINT1 debounce gating.
    // No ISR, no output compare, no input capture — TCNT1 is read-only
    // by the PCINT1 handler. Safe to share with future subsystems that
    // also only read TCNT1.
    // ----------------------------------------------------------------
    TCCR1A = 0;
    TCCR1B = (1 << CS11);              // prescaler 8
    TCNT1  = 0;

    // ----------------------------------------------------------------
    // PCINT1 — enable PC0 (PCINT8) and PC1 (PCINT9) only.
    // PC2 (button) is deliberately excluded from PCMSK1; the ISR also
    // guards against it explicitly for future robustness.
    // ----------------------------------------------------------------
    PCMSK1 |= (1 << PCINT8) | (1 << PCINT9);
    PCICR  |= (1 << PCIE1);

#endif /* ENC_USE_STABLE_DRIVER == 1 */

    lastButton  = 0;
    buttonValue = 1;
}


// -----------------------------------------------------------------------
// Timer 0 CTC ISR — ~250 us period
//
// Legacy mode (ENC_USE_STABLE_DRIVER == 0):
//   Polls PHASE_A / PHASE_B and accumulates enc_delta on every valid
//   single-bit Gray-code transition. Identical to original Dannegger
//   algorithm; only the Timer 0 rate has changed (256->64 prescaler).
//
// Stable mode (ENC_USE_STABLE_DRIVER == 1):
//   Encoder polling block is excluded; enc_delta is driven by PCINT1_vect.
//   Button debounce runs unchanged in both modes.
// -----------------------------------------------------------------------
ISR( TIMER0_COMPA_vect )
{
#if ENC_USE_STABLE_DRIVER == 0

    int8_t new, diff;

    new = 0;
    if( PHASE_A )
        new = 3;
    if( PHASE_B )
        new ^= 1;                               // convert gray to binary
    diff = (int8_t)(last - new);                // difference last - new
    if( diff & 1 ){                             // bit 0 = valid transition
        last      = new;                        // store new as next last
        enc_delta = (int8_t)(enc_delta + (diff & 2) - 1); // bit 1 = dir
    }

#endif /* ENC_USE_STABLE_DRIVER == 0 */

    // button debounce — two consecutive matching samples required
    if( ENCODER_BUTTON == lastButton )
    {
        buttonValue = (uint8_t)lastButton;
    }
    lastButton = ENCODER_BUTTON;
}


#if ENC_USE_STABLE_DRIVER == 1

// -----------------------------------------------------------------------
// PCINT1 ISR — edge-triggered, stable driver only
//
// Fires on any logic change on an unmasked pin in the PCINT1 group
// (PC0 and PC1 only — PC2/button is not in PCMSK1).
//
// Two-layer noise rejection:
//   1. Time gate: edges arriving within ENC_DEBOUNCE_TICKS Timer-1 ticks
//      of the last accepted edge are discarded (default 500 us at 400
//      ns/tick). Absorbs contact bounce completely for most encoders.
//   2. Consecutive-direction confirmation: ENC_CONFIRM_COUNT same-
//      direction edges must be seen before enc_delta is incremented.
//      Eliminates residual flyback that passes the time gate.
//
// enc_delta is only written here (stable mode); TIMER0_COMPA_vect does
// not touch it in stable mode. The read functions still use cli/sei to
// atomically snapshot and clear enc_delta from the main loop.
// -----------------------------------------------------------------------
ISR( PCINT1_vect )
{
    // one-shot baseline init: seed last_portc from real pin state on the
    // first invocation so the first real edge decodes correctly.
    static uint8_t  initialised = 0;
    static uint8_t  last_portc  = 0;
    static uint16_t last_time   = 0;
    static int8_t   last_dir    = 0;
    static uint8_t  consec      = 0;

    uint8_t current = PINC;

    if( !initialised )
    {
        last_portc  = current;
        last_time   = TCNT1;
        initialised = 1;
        return;
    }

    // guard: only proceed if an encoder pin actually changed
    uint8_t changed = (uint8_t)(current ^ last_portc);
    last_portc = current;

    if( !(changed & ((1 << PC0) | (1 << PC1))) )
        return;

    // time gate — unsigned subtraction wraps correctly across TCNT1 overflow
    uint16_t now = TCNT1;
    if( (uint16_t)(now - last_time) < ENC_DEBOUNCE_TICKS )
        return;

    // Gray code decode — identical to Dannegger
    int8_t new = 0;
    if( PHASE_A ) new = 3;
    if( PHASE_B ) new ^= 1;

    int8_t diff = (int8_t)(last - new);
    if( diff & 1 )
    {
        last      = new;
        last_time = now;

        int8_t dir = (int8_t)((diff & 2) - 1);     // +1 or -1

        if( dir == last_dir )
        {
            if( ++consec >= ENC_CONFIRM_COUNT )
            {
                enc_delta = (int8_t)(enc_delta + dir);
                consec    = 0;
            }
        }
        else
        {
            last_dir = dir;
            consec   = 1;
        }
    }
}

#endif /* ENC_USE_STABLE_DRIVER == 1 */


// -----------------------------------------------------------------------
// Read functions
// All three are identical in both driver modes — the only difference is
// how enc_delta was accumulated (polling vs PCINT). cli/sei ensures an
// atomic snapshot of the volatile enc_delta from the main loop.
// -----------------------------------------------------------------------

int8_t encode_stableRead1( void )       // read single step encoders
{
    int8_t val;
    cli();
    val       = enc_delta;
    enc_delta = 0;
    sei();
    return val;                         // counts since last call
}


int8_t encode_stableRead2( void )       // read two step encoders
{
    int8_t val;
    cli();
    val       = enc_delta;
    enc_delta = val & 1;
    sei();
    return val >> 1;
}


int8_t encode_stableRead4( void )       // read four step encoders
{
    int8_t val;
    cli();
    val       = enc_delta;
    enc_delta = val & 3;
    sei();
    return val >> 2;
}


// get the button value — returns true if button is pressed
uint8_t encode_readButton()
{
    return (buttonValue == 0);
}
