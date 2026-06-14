/*
 * GlobalMidiParser.h
 *
 * MIDI-wide system message helpers split out of MidiParser.c.
 */

#ifndef GLOBALMIDIPARSER_H_
#define GLOBALMIDIPARSER_H_

#include <stdint.h>

#include "MidiMessages.h"

/* Global MIDI system ownership. This module handles clock/MTC start-stop
   behavior that is shared across all voices. */
uint8_t globalMidiParser_handleSystemMessage(MidiMsg msg);

/* Global-channel CC router. The global channel owns the bank/morph/reset
   ladder and its sequencer side effects. */
void globalMidiParser_MIDIccHandler(MidiMsg msg, uint8_t updateOriginalValue);

/* Shared MTC watchdog used by the sequencer and parser coordination layer. */
void midiParser_checkMtc(void);

#endif /* GLOBALMIDIPARSER_H_ */
