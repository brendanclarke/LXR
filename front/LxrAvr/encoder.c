/************************************************************************/
/*                                                                      */
/*                      Reading rotary encoder                          */
/*                      one and four step encoders supported            */
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
    // Timer 1 — CTC, prescaler 64, OCR1A = 77 => ~250 us period.
    // Used as the encoder sampling ISR in stable mode.
    // ----------------------------------------------------------------
    TCCR1A = 0;
    TCCR1B = (1 << WGM12) | (1 << CS11) | (1 << CS10);   // CTC, prescaler 64
    OCR1A  = (uint16_t)(F_CPU / 64 / 4000) - 1;           // ~250 us
    TIMSK1 |= (1 << OCIE1A);

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
//   Encoder polling runs from TIMER1_COMPA_vect instead of TIMER0.
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
// Timer 1 CTC ISR — stable driver only
//
// Same Gray-code polling as the legacy path, but sampled by Timer 1
// instead of Timer 0. This keeps the stable driver on a dedicated timer
// while preserving the original encoder semantics.
// -----------------------------------------------------------------------
ISR( TIMER1_COMPA_vect )
{
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
}

#endif /* ENC_USE_STABLE_DRIVER == 1 */


// -----------------------------------------------------------------------
// Read functions
// Both read functions are identical in both driver modes — the only
// difference is which timer accumulated enc_delta (Timer0 vs Timer1).
// cli/sei ensures an atomic snapshot of the volatile enc_delta from the
// main loop.
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


int8_t encode_stableRead4( void )       // read four step encoders
{
    static int16_t detent_accum;
    int16_t raw;
    int8_t val = 0;
    cli();
    raw       = enc_delta;
    enc_delta = 0;
    sei();

    if( raw == 0 )
        return 0;

    // If the user reverses direction, throw away the stale partial turn
    // instead of forcing the new direction to work through old residue.
    if( (detent_accum > 0 && raw < 0) ||
        (detent_accum < 0 && raw > 0) )
    {
        detent_accum = 0;
    }

    detent_accum += raw;

    while( detent_accum >= 4 )
    {
        val++;
        detent_accum -= 4;
    }

    while( detent_accum <= -4 )
    {
        val--;
        detent_accum += 4;
    }

    return val;
}


// get the button value — returns true if button is pressed
uint8_t encode_readButton()
{
    return (buttonValue == 0);
}
