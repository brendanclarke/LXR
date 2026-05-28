/************************************************************************/
/*                                                                      */
/*                      Reading rotary encoder                          */
/*                      one, two and four step encoders supported       */
/*                                                                      */
/*              Author: Peter Dannegger                                 */
/*              Modified: Julian Schmidt                                 */
/*                                                                      */
/************************************************************************/
#include <avr/io.h>
#include <avr/interrupt.h>
#include "config.h"
 
// target: ATmega644
//------------------------------------------------------------------------
#ifndef F_CPU
#define F_CPU 20000000UL
#endif
 
#define PIN_A           (1<<PC0)
#define PIN_B           (1<<PC1)
#define PIN_BUTTON      (1<<PC2)
 
#define PHASE_A         (PINC & (1<<PC0))
#define PHASE_B         (PINC & (1<<PC1))
#define ENCODER_BUTTON  (PINC & (1<<PC2))

#define ECODER_PORT     PORTC
#define ENCODER_DDR     DDRC    //pin direction i/o
 
//------------------------------------------------------------------------
// functions
//------------------------------------------------------------------------
void encode_init( void );

// Timer 0 CTC ISR — encoder polling (legacy mode) and button debounce
ISR( TIMER0_COMPA_vect );

#if ENC_USE_STABLE_DRIVER == 1
// PCINT1 ISR — edge-triggered quadrature decode with time-gated debounce
// Active only when ENC_USE_STABLE_DRIVER == 1.
// Handles PC0 (PCINT8) and PC1 (PCINT9). PC2 (button) is excluded from
// PCMSK1 and filtered in the ISR; button events do not produce encoder counts.
ISR( PCINT1_vect );
#endif

// Stable read functions.
// encode_stableRead4: one count per four Gray-code transitions (one full
//   quadrature cycle). Use for encoders where 1 detent = 4 state changes.
// encode_stableRead2: one count per two transitions.
// encode_stableRead1: one count per transition.
// In legacy mode (ENC_USE_STABLE_DRIVER == 0) the behaviour of all three
// is identical to the original encode_read* functions.
int8_t encode_stableRead1( void );
int8_t encode_stableRead2( void );
int8_t encode_stableRead4( void );

// Deprecated wrappers — retained for backward compatibility only.
// New code should call encode_stableRead*() directly.
static inline __attribute__((deprecated("use encode_stableRead1")))
int8_t encode_read1(void) { return encode_stableRead1(); }

static inline __attribute__((deprecated("use encode_stableRead2")))
int8_t encode_read2(void) { return encode_stableRead2(); }

static inline __attribute__((deprecated("use encode_stableRead4")))
int8_t encode_read4(void) { return encode_stableRead4(); }

// get the state of the encoder button
uint8_t encode_readButton();
