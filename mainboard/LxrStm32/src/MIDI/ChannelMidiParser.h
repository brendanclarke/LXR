/*
 * ChannelMidiParser.h
 *
 * MIDI channel helpers and front-panel echo helpers split out of MidiParser.c.
 */

#ifndef CHANNELMIDIPARSER_H_
#define CHANNELMIDIPARSER_H_

#include <stdint.h>

#include "MidiMessages.h"

/* Channel-owned note routing. These helpers keep the per-voice trigger and
   note-override behavior out of the broad parser. */
void channelMidiParser_noteOn(uint8_t voice, uint8_t note, uint8_t vel, uint8_t do_rec);
void channelMidiParser_noteOff(uint8_t voice, uint8_t note, uint8_t vel, uint8_t do_rec);

/* Channel-owned CC router. This module owns the voice-scoped ladder and the
   front-panel echo helpers used by those cases. */
void channelMidiParser_MIDIccHandler(MidiMsg msg, uint8_t updateOriginalValue);

void channelMidiParser_sendBankChange(uint8_t bankCode, uint8_t value);
void channelMidiParser_sendParameterEcho(uint16_t paramNr, uint8_t value);
void channelMidiParser_sendPatchReset(void);
void channelMidiParser_sendVoiceActivity(uint8_t voice);

#endif /* CHANNELMIDIPARSER_H_ */
