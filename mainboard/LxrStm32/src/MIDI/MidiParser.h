/*
 * MidiParser.h
 *
 *  Created on: 13.04.2012
 * ------------------------------------------------------------------------------------------------------------------------
 *  Copyright 2013 Julian Schmidt
 *  Julian@sonic-potions.com
 * ------------------------------------------------------------------------------------------------------------------------
 *  This file is part of the Sonic Potions LXR drumsynth firmware.
 * ------------------------------------------------------------------------------------------------------------------------
 *  Redistribution and use of the LXR code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *       - The code may not be sold, nor may it be used in a commercial product or activity.
 *
 *       - Redistributions that are modified from the original source must include the complete
 *         source code, including the source code for all components used by a binary built
 *         from the modified sources. However, as a special exception, the source code distributed
 *         need not include anything that is normally distributed (in either source or binary form)
 *         with the major components (compiler, kernel, and so on) of the operating system on which
 *         the executable runs, unless that component itself accompanies the executable.
 *
 *       - Redistributions must reproduce the above copyright notice, this list of conditions and the
 *         following disclaimer in the documentation and/or other materials provided with the distribution.
 * ------------------------------------------------------------------------------------------------------------------------
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 *   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *   USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ------------------------------------------------------------------------------------------------------------------------
 */


#ifndef MIDIPARSER_H_
#define MIDIPARSER_H_

#include "stm32f4xx.h"
#include "globals.h"
#include "MidiMessages.h"
#include "MidiOutputControl.h"

/* Stream parser entry point. This remains the top-level MIDI byte coordinator
   while channel/global ownership lives in the split helper modules. */
void midiParser_parseUartData(unsigned char data);

void midiParser_parseMidiMessage(MidiMsg msg);
void midiParser_MIDIccHandler(MidiMsg msg, uint8_t updateOriginalValue);
float midiParser_calcDetune(uint8_t value);
/* Watchdog for the MIDI timecode start/stop path. */
void midiParser_checkMtc();

#if 0
void midiDebugSend(uint8_t b1, uint8_t b2);
#endif

/* Routing selection for the MIDI I/O bridge. */
void midi_setRouting(uint8_t value);

/* Per-direction MIDI filter bits. */
void midi_setFilter(uint8_t is_tx, uint8_t value);

/* Last seen CC values used by automation and live-apply feedback. */
extern uint8_t frontParser_originalCcValues[0xff];

/* MIDI parser caches shared with the receive and voice-control code. */
extern MidiMsg midi_midiCache[256];
extern MidiMsg midi_midiKit[256];
extern uint8_t midi_midiCacheAvailable[256];
extern uint8_t midi_midiLfoCache[6];
extern uint8_t midi_kitLfoCache[6];
extern uint8_t midi_midiLfoCacheAvailable[6];
extern uint8_t midi_midiVeloCache[6];
extern uint8_t midi_kitVeloCache[6];
extern uint8_t midi_midiVeloCacheAvailable[6];

extern uint8_t midi_envPosition[6];
extern uint8_t midi_unused;

void midi_clearCache();

/* Voice MIDI channel table. Element 7 is the global channel. */
extern uint8_t midi_MidiChannels[8]; // last element is global channel
extern uint8_t midi_NoteOverride[7];
extern uint8_t midi_KitChange[6];
//extern uint8_t midi_mode; --AS not used anymore

//enum MIDI_modeEnum
//{
//	MIDI_MODE_TRIGGER,
//	MIDI_MODE_NOTE,
//} MidiModes;

/* High nibble is TX, low nibble is RX. See midi_setFilter() for the
   bit layout. */
extern uint8_t midiParser_txRxFilter;

#endif /* MIDIPARSER_H_ */
