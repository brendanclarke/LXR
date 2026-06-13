/************************************************************************/
/*                                                                      */
/*                      Reading rotary encoder                          */
/*                      one and four step encoders supported            */
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

// Timer 0 CTC ISR — button debounce in both modes
ISR( TIMER0_COMPA_vect );

#if ENC_USE_STABLE_DRIVER == 1
// Timer 1 CTC ISR — quadrature decode using the same polling logic as
// legacy mode, but driven from Timer 1 instead of Timer 0.
// Active only when ENC_USE_STABLE_DRIVER == 1.
ISR( TIMER1_COMPA_vect );
#endif

// Stable read functions.
// encode_stableRead4: one count per four Gray-code transitions (one full
//   quadrature cycle). Use for encoders where 1 detent = 4 state changes.
// encode_stableRead1: one count per transition.
// In both driver modes the behaviour matches the original encode_read*
// functions; the only difference is which timer feeds enc_delta.
int8_t encode_stableRead1( void );
int8_t encode_stableRead4( void );

// Deprecated wrappers — retained for backward compatibility only.
// New code should call encode_stableRead*() directly.
static inline __attribute__((deprecated("use encode_stableRead1")))
int8_t encode_read1(void) { return encode_stableRead1(); }

static inline __attribute__((deprecated("use encode_stableRead4")))
int8_t encode_read4(void) { return encode_stableRead4(); }

// get the state of the encoder button
uint8_t encode_readButton();
