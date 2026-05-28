/*
 * config.h
 *
 * Created: 23.10.2012 10:15:14
 *  Author: Julian
 */ 


#ifndef CONFIG_H_
#define CONFIG_H_

#define USE_SD_CARD 1		//set to "1" if the SD reader is connected to the AVR, "0" if the cortex uses it
#define USE_DRUM_MAP_GENERATOR 0


#define UART_DEBUG_ECHO_MODE 0
#define DEBUG_CRASH_MODE 0

// -----------------------------------------------------------------------
// Encoder driver selection
// 0 = original Dannegger polling (~1ms), backward compatible
// 1 = PCINT1 + Timer 1 stable driver with time-gated debounce
//     and consecutive-direction confirmation (encode_stableRead4 only)
// -----------------------------------------------------------------------
#define ENC_USE_STABLE_DRIVER  0

// Minimum time between accepted encoder edges, in Timer 1 ticks.
// Timer 1: prescaler 8 at 20 MHz = 400 ns per tick.
// 1250 ticks = 500 us. Increase if bounce persists; decrease if response lags.
// Only used when ENC_USE_STABLE_DRIVER == 1.
#define ENC_DEBOUNCE_TICKS  1250

// Number of consecutive same-direction edges required before a count is
// committed to enc_delta. 3 is conservative; 2 is more responsive.
// At max human turn speed (5 rev/s, 24 PPR) inter-edge time is ~8 ms;
// 3 confirmations at 500 us each = 1.5 ms = ~19% of inter-edge time.
// Only used when ENC_USE_STABLE_DRIVER == 1.
#define ENC_CONFIRM_COUNT   3

#endif /* CONFIG_H_ */
