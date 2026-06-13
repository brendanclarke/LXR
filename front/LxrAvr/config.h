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
// 1 = Timer 1 compare driver with the same quadrature polling logic
//     as the legacy path, but driven from Timer 1 instead of Timer 0
// -----------------------------------------------------------------------
#define ENC_USE_STABLE_DRIVER  1

#endif /* CONFIG_H_ */
