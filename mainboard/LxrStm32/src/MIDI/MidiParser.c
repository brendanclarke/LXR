/*
 * MidiParser.c
 *
 *  Created on: 02.04.2012
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


#include "MidiParser.h"
#include "MidiNoteNumbers.h"
#include "DrumVoice.h"
#include "Snare.h"
#include "config.h"
#include "HiHat.h"
#include "CymbalVoice.h"
#include "uARTFrontSYX/Uart.h"
#include "uARTFrontSYX/frontPanelReceivingProtocol.h"
#include "ChannelMidiParser.h"
#include "GlobalMidiParser.h"
#include "clockSync.h"
#include "sequencer.h"
#include "Preset/ParameterIngress.h"
#include "mixer.h"
#include "valueShaper.h"
#include "modulationNode.h"
#include "usb_manager.h"
// front-panel opcodes are owned by uARTFrontSYX/frontPanelReceivingProtocol.h
 #define MORPH_CC        0xac
 #define VOICE_CC			0xb4
   #define BANK_GLOBAL 0x7F
// banks 1-6 plus global stack to allow for multiple voices stacked on the same
// MIDI channel to respond to the same bank change command

// above BANK_GLOBAL it doesn't matter - we reset the command anyway
#define MORPH_OP 0x81
// there is space in here to add more long operations - pattern change
// must have the highest priority
#define PATTERN_CHANGE_OP 0xAF
#define NULL_OP 0x00

/* Parser-visible caches for live MIDI, LFO, and velocity state. */
MidiMsg midi_midiCache[256];
MidiMsg midi_midiKit[256];
uint8_t midi_midiCacheAvailable[256];
uint8_t midi_midiLfoCache[6];
uint8_t midi_kitLfoCache[6];
uint8_t midi_midiLfoCacheAvailable[6];
uint8_t midi_midiVeloCache[6];
uint8_t midi_kitVeloCache[6];
uint8_t midi_midiVeloCacheAvailable[6];
uint8_t midi_unused;

static inline uint8_t midiParser_voiceMidiChannel(uint8_t voice)
{
   return (voice < 8) ? midi_MidiChannels[voice] : 0;
}

static inline uint8_t midiParser_voiceNoteOverride(uint8_t voice)
{
   return (voice < 7) ? midi_NoteOverride[voice] : 0;
}

/* Parser-owned MIDI roll state. Raw 0 disables shifted roll notes; raw 1..127
   is the positive semitone offset above the normal trigger note. Hold counters
   allow overlapping roll-note presses for the same voice. */
static uint8_t midiParser_rollNoteOffsetRaw = 0;
static uint8_t midiParser_rollHoldCount[7] = {0};

/* Roll-note helpers. Matching is defined against the normal note that would
   trigger the voice, then shifted upward by the current positive offset. MIDI
   note bounds are checked before every shifted comparison. */
static uint8_t midiParser_rollOffsetEnabled(void)
{
   return midiParser_rollNoteOffsetRaw != 0;
}

static uint8_t midiParser_rollOffsetValue(void)
{
   return midiParser_rollNoteOffsetRaw;
}

static uint8_t midiParser_shiftedNoteInRange(uint8_t baseNote,
                                             uint8_t offset,
                                             uint8_t *shiftedNote)
{
   if(offset == 0 || baseNote > (uint8_t)(127 - offset))
      return 0;

   *shiftedNote = (uint8_t)(baseNote + offset);
   return 1;
}

static uint8_t midiParser_shiftedBaseInRange(uint8_t incomingNote,
                                             uint8_t offset,
                                             uint8_t *baseNote)
{
   if(offset == 0 || incomingNote < offset)
      return 0;

   *baseNote = (uint8_t)(incomingNote - offset);
   return 1;
}

static uint8_t midiParser_rollNoteMatches(uint8_t incomingNote,
                                          uint8_t baseNote)
{
   uint8_t shiftedNote;

   return midiParser_shiftedNoteInRange(baseNote,
                                        midiParser_rollOffsetValue(),
                                        &shiftedNote)
      && shiftedNote == incomingNote;
}

static void midiParser_rollVoiceOn(uint8_t voice)
{
   if(voice >= 7)
      return;

   if(midiParser_rollHoldCount[voice] == 0)
      seq_rollMidiChange(voice, 1);

   if(midiParser_rollHoldCount[voice] != 0xff)
      ++midiParser_rollHoldCount[voice];
}

static void midiParser_rollVoiceOff(uint8_t voice)
{
   if(voice >= 7 || midiParser_rollHoldCount[voice] == 0)
      return;

   --midiParser_rollHoldCount[voice];
   if(midiParser_rollHoldCount[voice] == 0)
      seq_rollMidiChange(voice, 0);
}

static void midiParser_applyRollVoiceMask(uint8_t voiceMask,
                                          uint8_t isNoteOff)
{
   uint8_t voice;

   for(voice = 0; voice < 7; ++voice)
   {
      if(voiceMask & (uint8_t)(1u << voice))
      {
         if(isNoteOff)
            midiParser_rollVoiceOff(voice);
         else
            midiParser_rollVoiceOn(voice);
      }
   }
}

void midi_clearCache()
{
   /* Clear the parser-owned live MIDI cache. */
   uint16_t i;
   midiParser_clearMidiRollHolds();
   for (i=0;i<256;i++)
   {
      midi_midiCacheAvailable[i]=0;
   }
   for(i=0;i<6;i++)
   {
      midi_midiLfoCache[i]=0;
      midi_midiLfoCacheAvailable[i]=0;
      midi_midiVeloCache[i]=0;
      midi_midiVeloCacheAvailable[i]=0;
   }
}

/* Release every parser-owned MIDI roll hold. Mapping changes and parser-cache
   resets use this so a later note-off cannot be stranded under old routing. */
void midiParser_clearMidiRollHolds(void)
{
   uint8_t voice;

   for(voice = 0; voice < 7; ++voice)
   {
      if(midiParser_rollHoldCount[voice] != 0)
      {
         midiParser_rollHoldCount[voice] = 0;
         seq_rollMidiChange(voice, 0);
      }
   }
}

/* Set the global MIDI roll-note offset. A changed offset invalidates every
   outstanding shifted-note hold, so MIDI roll ownership is released before
   the new raw value is installed. */
void midiParser_setRollNoteOffset(uint8_t rawOffset)
{
   rawOffset &= 0x7f;

   if(rawOffset != midiParser_rollNoteOffsetRaw)
   {
      midiParser_clearMidiRollHolds();
      midiParser_rollNoteOffsetRaw = rawOffset;
   }
}

static union {
   uint8_t value;
   struct {
      unsigned usb2midi:1;
      unsigned usb2usb:1;   // not used
      unsigned midi2midi:1;
      unsigned midi2usb:1;
      unsigned :4;
   } route;
} midiParser_routing = {0};

/* High nibble is TX, low nibble is RX. */
uint8_t midiParser_txRxFilter = 0xFF;

enum State
{
MIDI_STATUS,  		// waiting for status byte
MIDI_DATA1,  		// waiting for data byte1
MIDI_DATA2,  		// waiting for data byte2
SYSEX_DATA,  		// read sysex data byte
IGNORE				// set when unknown status byte received, stays in ignore mode until next known status byte
};

// 2^(1/12) factor for 1 semitone
#define SEMITONE_UP 1.0594630943592952645618252949463f
#define SEMITONE_DOWN 0.94387431268169349664191315666753f

#define NUM_LFO 6
/* Track which voice each LFO display slot is bound to. */
uint8_t midiParser_selectedLfoVoice[NUM_LFO] = {0,0,0,0,0,0};

#if 0
// -- AS for debugging
void midiDebugSend(uint8_t b1, uint8_t b2)
{
uart_sendMidiByte(0xF2);
uart_sendMidiByte(b1&0x7F);
uart_sendMidiByte(b2&0x7F);
}
#endif

//----------------------------------------------------------
#if 0
inline uint16_t calcSlopeEgTime(uint8_t data2)
{
float val = (data2+1)/128.f;
return data2>0?val*val*data2*128:1;
}
#endif
//-----------------------------------------------------------
/* Voice/channel state owned by the parser and read by the split helpers. */
uint8_t midi_MidiChannels[8];	// the currently selected midi channel for each voice (element 7 is global channel)

//--AS note overrides for each voice
uint8_t midi_NoteOverride[7];
//uint8_t midi_mode; //--AS not used anymore
MidiMsg midiMsg_tmp;				// buffer message where the incoming data is stored
// these two are used only when building up a midi message
//static uint8_t msgLength;					// number of following data bytes expected for current status
static uint8_t parserState = IGNORE;	// state of the parser state machine. Set to what it's expecting next
									// we set it to ignore initially so that any random data we get before
									// a valid msg header is ignored

/* This CMSIS revision omits the DWT declarations even though STM32F407 has
   the standard Cortex-M4 DWT block. These architectural addresses are DWT
   CTRL and CYCCNT; keeping them local prevents a legacy-header gap from
   leaking into transport interfaces. */
#define MIDI_DWT_CTRL    (*(volatile uint32_t *)0xE0001000UL)
#define MIDI_DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004UL)
#define MIDI_DWT_CYCCNTENA (1UL << 0)

/* The DWT cycle counter timestamps realtime MIDI at IRQ receive boundaries.
   It is deliberately independent of systick_ticks: SysTick is 250 us
   resolution and is used by transport tempo logic, while this counter records
   sub-block queue latency for diagnostics without changing event timing. */
void midiParser_initRealtimeTimestamp(void)
{
   CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
   MIDI_DWT_CYCCNT = 0;
   MIDI_DWT_CTRL |= MIDI_DWT_CYCCNTENA;
}

/* Read the enabled DWT cycle counter for one realtime transport event. The
   caller owns storage and wrap-safe latency subtraction; this function has no
   parser side effects and is safe in the short capture-only IRQ paths. */
uint32_t midiParser_captureRealtimeTimestamp(void)
{
   return MIDI_DWT_CYCCNT;
}

//-----------------------------------------------------------
/* Convert a MIDI value to the parser's detune factor. */
float midiParser_calcDetune(uint8_t value)
{
//linear interpolation between 1(no change) and semitone up/down)
   float frac = (value/127.f -0.5f);
   float cent = 1;
   if(cent>=0)
   {
      cent += frac*(SEMITONE_UP - 1);
   }
   else
   {
      cent += frac*(SEMITONE_UP - 1);
   }
   return cent;
}


//-----------------------------------------------------------
/* Parse incoming MIDI messages and route them to the shared helpers. */
void midiParser_parseMidiMessage(MidiMsg msg)
{

// route message if needed
   if(midiParser_routing.value) {
      if(msg.bits.source==midiSourceUSB) {
         if(midiParser_routing.route.usb2midi){
         // route to midi out port
            uart_sendMidi(msg);
         }
      } 
      else if(msg.bits.source==midiSourceMIDI) {
         if(midiParser_routing.route.midi2midi) {
         // route to midi out port
            uart_sendMidi(msg);
         }
         if(midiParser_routing.route.midi2usb) {
         // route to usb out port
            usb_sendMidi(msg);
         }
      }
   }

   if(msg.bits.sysxbyte)
      return; // no further action needed. we don't interpret sysex data right now

// --AS FILT filter messages here. Filter inline below to be more optimal
// we are interested in the low nibble since we are Rx here


   if((msg.status & 0xF0) == 0XF0) {
   // BC: !!!NB!!! for midi jack input, system realtime messages 
   // are dealt with at the top of midiParser_parseUartData(),
   // to avoid conflicts with channel-specific messages. These may
   // not need repeating here, or may only exist for USB messages.
   
   // non-channel specific messages (system messages)
      globalMidiParser_handleSystemMessage(msg);
      return;
   
   } 
   else { // channel specific message
      const uint8_t msgonly =msg.status & 0xF0;
      const uint8_t chanonly=(msg.status&0x0F)+1;
   
      if((msgonly & 0xE0) == 0x80) {
      // note on or note off message (one of these two only)
         if(midiParser_txRxFilter & 0x01) {
            uint8_t normalConsumed = 0;
            uint8_t rollVoiceMask = 0;
            const uint8_t isNoteOff = (msgonly == NOTE_OFF);
            uint8_t v;

            /* Run the existing global and voice note consumers first. A voice
               override mismatch is not a consumed note: ChannelMidiParser
               receives the call for legacy behavior but returns without a
               trigger, allowing a shifted override note to reach roll. */
            if(midiParser_voiceMidiChannel(7) == chanonly)
            {
               const uint8_t activeTrackOverride =
                  midiParser_voiceNoteOverride(frontParser_activeTrack);

               if(!activeTrackOverride)
               {
                  normalConsumed = 1;
                  if(isNoteOff)
                     channelMidiParser_noteOff(frontParser_activeTrack,
                                               msg.data1,
                                               msg.data2,
                                               1);
                  else
                     channelMidiParser_noteOn(frontParser_activeTrack,
                                              msg.data1,
                                              msg.data2,
                                              1);
               }
               else
               {
                  for(v = 0; v < 7; ++v)
                  {
                     if(midiParser_voiceNoteOverride(v) == msg.data1)
                     {
                        normalConsumed = 1;
                        if(isNoteOff)
                           channelMidiParser_noteOff(v, msg.data1, msg.data2, 1);
                        else
                           channelMidiParser_noteOn(v, msg.data1, msg.data2, 1);
                     }
                  }
               }
            }

            /* Additionally check each assigned voice channel, preserving the
               existing active-track recording distinction. */
            for(v = 0; v < 7; ++v)
            {
               if(midiParser_voiceMidiChannel(v) == chanonly)
               {
                  const uint8_t noteOverride = midiParser_voiceNoteOverride(v);

                  if(noteOverride == 0 || noteOverride == msg.data1)
                     normalConsumed = 1;

                  if(isNoteOff)
                  {
                     if(v == frontParser_activeTrack)
                        channelMidiParser_noteOff(v, msg.data1, msg.data2, 1);
                     else
                        channelMidiParser_noteOff(v, msg.data1, msg.data2, 0);
                  }
                  else
                  {
                     if(v == frontParser_activeTrack)
                        channelMidiParser_noteOn(v, msg.data1, msg.data2, 1);
                     else
                        channelMidiParser_noteOn(v, msg.data1, msg.data2, 0);
                  }
               }
            }

            /* Last-consumer MIDI roll-note path. Normal note routing wins; only
               unconsumed literal NOTE_ON/NOTE_OFF messages are tested against
               the positive-offset roll map. */
            if(!normalConsumed && midiParser_rollOffsetEnabled())
            {
               const uint8_t offset = midiParser_rollOffsetValue();
               uint8_t baseNote;

               if(midiParser_voiceMidiChannel(7) == chanonly)
               {
                  const uint8_t activeTrackOverride =
                     midiParser_voiceNoteOverride(frontParser_activeTrack);

                  if(!activeTrackOverride)
                  {
                     if(frontParser_activeTrack < 7
                        && midiParser_shiftedBaseInRange(msg.data1,
                                                         offset,
                                                         &baseNote))
                     {
                        rollVoiceMask |= (uint8_t)(1u << frontParser_activeTrack);
                     }
                  }
                  else
                  {
                     for(v = 0; v < 7; ++v)
                     {
                        const uint8_t noteOverride =
                           midiParser_voiceNoteOverride(v);
                        if(noteOverride
                           && midiParser_rollNoteMatches(msg.data1,
                                                         noteOverride))
                        {
                           rollVoiceMask |= (uint8_t)(1u << v);
                        }
                     }
                  }
               }

               for(v = 0; v < 7; ++v)
               {
                  const uint8_t noteOverride = midiParser_voiceNoteOverride(v);

                  if(midiParser_voiceMidiChannel(v) != chanonly)
                     continue;

                  if(!noteOverride)
                  {
                     if(midiParser_shiftedBaseInRange(msg.data1,
                                                      offset,
                                                      &baseNote))
                     {
                        rollVoiceMask |= (uint8_t)(1u << v);
                     }
                  }
                  else if(midiParser_rollNoteMatches(msg.data1, noteOverride))
                  {
                     rollVoiceMask |= (uint8_t)(1u << v);
                  }
               }

               midiParser_applyRollVoiceMask(rollVoiceMask, isNoteOff);
            }
         } // check midi filter
         
      } 
      else if(msgonly==PROG_CHANGE) 
      {
         
      // --AS respond to prog change and change patterns. This responds only when global channel matches the PC message's channel.
         //send the ack message to tell the front that a new pattern starts playing
         if(midiParser_txRxFilter & 0x08)
         {
            if(chanonly == midiParser_voiceMidiChannel(7))
            {
               if(msg.data1<16)
               {
                  seq_setNextPattern(msg.data1&0x07,0x7f);
                  if(msg.data1>7)
                  {
                     seq_newVoiceAvailable=0x7f;
                  }
               }   
            }
            uint8_t i;
            for(i=0;i<NUM_TRACKS;i++) // set individual track patterns with PC on that channel
            {
               if(chanonly == midiParser_voiceMidiChannel(i))
               {
                  seq_setNextPattern(msg.data1&0x07,i);
                  seq_newVoiceAvailable&=(0x01<<i);
               }
            }   
         }
      } 
      else if(msgonly==MIDI_CC){
      // respond to CC message. 
         midiParser_MIDIccHandler(msg,1); // send with 1 to record value to either 
                                       // automation or kit param, 0 for DSP only
      } 
      else {
      // anything else
      // TODO MIDI_PITCH_WHEEL ?
      }
   } // channel specific vs non channel specific

}

//-----------------------------------------------------------


/* Split CC router. Global CC0/CC1 intentionally keep the existing channel
   parser bank/morph behavior; Global CC2-127 use the alternate global table. */
void midiParser_MIDIccHandler(MidiMsg msg, uint8_t updateOriginalValue)
{
   const uint8_t chanonly = (msg.status & 0x0F) + 1;

   if(chanonly == midiParser_voiceMidiChannel(7))
   {
      if(msg.data1 == BANK || msg.data1 == MOD_WHEEL)
         channelMidiParser_MIDIccHandler(msg, updateOriginalValue);
      else
         globalMidiParser_MIDIccHandler(msg, updateOriginalValue);

      return;
   }

   channelMidiParser_MIDIccHandler(msg, updateOriginalValue);
}
/* Cache a new status byte and prime the running-status state machine. */
void midiParser_handleStatusByte(unsigned char data)
{
// we received a channel voice/mode byte. set the status as appropriate
   switch(data&0xF0) {
   // 2 databyte messages
      case NOTE_OFF:
      case NOTE_ON:
      case MIDI_CC:
      case MIDI_PITCH_WHEEL:
      case MIDI_AT:
         midiMsg_tmp.status = data;	// store the new status byte
         parserState = MIDI_DATA1;	//status received, next should be data byte 1
         midiMsg_tmp.bits.length=2;// status is followed by 2 data bytes
         break;
   
   // 1 databyte messages
      case PROG_CHANGE:
      case CHANNEL_PRESSURE:
         midiMsg_tmp.status = data;	// store the new status byte
         parserState = MIDI_DATA1;	//status received, next should be data byte 1
         midiMsg_tmp.bits.length=1;// status is followed by 1 data bytes
         break;
   
   // messages we don't care about right now, and don't know how to handle or passthru (Are there any?).
      default:
         parserState = IGNORE;	// throw away any data bytes until next message
         midiMsg_tmp.bits.length=0;
      
         break;
   }
}
//-----------------------------------------------------------
/* Consume one DIN system-realtime byte in main-loop context. Input is a
   complete status byte captured by the USART realtime queue; output is the
   same external-sync transport action and optional DIN/USB forwarding that
   the legacy raw-byte parser performed. This helper intentionally owns no
   queue state and performs no timing capture, allowing the UART transport to
   prioritize arrival without moving sequencer or TX work into an ISR. */
void midiParser_handleDinRealtime(uint8_t data)
{
   switch(data)
   {
      case MIDI_START:
      case MIDI_CONTINUE:
         if((midiParser_txRxFilter & 0x02) && seq_getExtSync())
            sync_midiStartStop(1);
         break;

      case MIDI_STOP:
         if((midiParser_txRxFilter & 0x02) && seq_getExtSync())
            sync_midiStartStop(0);
         break;

      case MIDI_CLOCK:
         if((midiParser_txRxFilter & 0x02) && seq_getExtSync())
            seq_sync();
         break;

      default:
         break;
   }

   if(midiParser_routing.value)
   {
      MidiMsg rtMsg = {0};
      rtMsg.status = data;
      if(midiParser_routing.route.midi2midi)
         uart_sendMidi(rtMsg);
      if(midiParser_routing.route.midi2usb)
         usb_sendMidi(rtMsg);
   }
}

// This will build up the midi message and hand it off to
// parseMidiMessage when it's complete
/* Byte-stream parser for raw MIDI UART input. */
void midiParser_parseUartData(unsigned char data)
{

   if(data&0x80) { // High bit is set -  its either a status or a system message.
   // regardless of current state, we blindly start a new message without questioning it
      if((data&0xf8)==0xf8) // direct callers retain legacy realtime handling
      {
         midiParser_handleDinRealtime(data);
         return; // realtime has no follow-up data and must not alter parser state
      }
      midiMsg_tmp.bits.sysxbyte=0;
      if( (data&0xf0) == 0xf0) { // system message
         midiMsg_tmp.status = data;
         switch(data) {
            case SYSEX_START: // get into sysex receive mode. any more bytes received until this status changes are considered
            		  // to be sysex data. we still need to parse this sysex start, in case we are routing it
               parserState = SYSEX_DATA;
               midiMsg_tmp.bits.length=0;
               goto parseMsg; // we will still parse it in case we are doing a passthru
            case SYSEX_END:	  // get out of sysex mode
               if(parserState==SYSEX_DATA) {
                  parserState=MIDI_STATUS;
                  midiMsg_tmp.bits.length=0;
                  goto parseMsg; // we will still parse it in case we are doing a passthru
               } 
               else {
               // spurious sysex end msg received. ignore it
               }
               break; //
         // 1 byte payload messages
            case MIDI_SONG_SEL:		// passthru only
            case MIDI_MTC_QFRAME: 	// mtc chunk
               parserState = MIDI_DATA1; 	// we expect the nugget of mtc frame info
               midiMsg_tmp.bits.length=1;// we expect 1 data byte
               break;
         
         // 0 byte payload messages (we will assume that any system message
         // other than those above has 0 byte payload)
            default:
               midiMsg_tmp.bits.length=0;
               goto parseMsg;
         }
      } 
      else { //status byte (channel specific message containing a channel number)
         midiParser_handleStatusByte(data);
      }
   } 
   else { // high bit is not set - it's a data byte
   
      switch(parserState)	{
         case MIDI_STATUS: // we are expecting status msg, but got data, so running status may be in effect
            if(midiMsg_tmp.bits.length) {
               midiMsg_tmp.data1 = data;
               if(midiMsg_tmp.bits.length==2)
                  parserState=MIDI_DATA2;
               else {
                  midiMsg_tmp.data2=0;
                  goto parseMsg;
               }
            } 
            else {
               break; // last msg had 0 payload, so wtf is this? just ignore it
            }
            break;
      
         case MIDI_DATA1:
            midiMsg_tmp.data1 = data;
            if(midiMsg_tmp.bits.length==2) {
            // we need another byte before we can do anything meaningful
               parserState = MIDI_DATA2;
            } 
            else { // it must be 1
               goto parseMsg;
            }
            break;
         case MIDI_DATA2:
            midiMsg_tmp.data2 = data;
            goto parseMsg; // message complete
         case SYSEX_DATA: // we are in sysex mode
            midiMsg_tmp.bits.sysxbyte=1;
            midiMsg_tmp.status = data; // status will contain the sysex byte
            midiMsg_tmp.bits.length=0;
            goto parseMsg;
         default: //we are expecting no data byte, but we got one.
         // ignore it
            break;
      } // switch parserState
   } // if high bit is set

   return;

parseMsg:
// we get here if we have proudly received a message that we want to do something with
   if(parserState != SYSEX_DATA) // we are still in sysex receive mode
      parserState = MIDI_STATUS; // next byte should be a new message, or we don't care about it
   midiMsg_tmp.bits.source=midiSourceMIDI;
   midiParser_parseMidiMessage(midiMsg_tmp);

}

// 0 - Off - nothing to nothing
// 1 - U2M - usb in to midi out
// 2 - M2M - midi in to midi out
// 3 - A2M - usb in and midi in to midi out
// 4 - M2U - midi in to usb out
// 5 - M2A - midi in to usb out and midi out

/* Configure the MIDI routing matrix. */
void midi_setRouting(uint8_t value)
{
   midiParser_routing.value=0;

   switch(value) {
      case 1:
         midiParser_routing.route.usb2midi=1;
         break;
      case 2:
         midiParser_routing.route.midi2midi=1;
         break;
      case 3:
         midiParser_routing.route.usb2midi=1;
         midiParser_routing.route.midi2midi=1;
         break;
      case 4:
         midiParser_routing.route.midi2usb=1;
         break;
      case 5:
         midiParser_routing.route.midi2midi=1;
         midiParser_routing.route.midi2usb=1;
         break;
      default:
      case 0:
      
         break;
   }

}

/* Configure the TX/RX MIDI filter bitfields. */
void midi_setFilter(uint8_t is_tx, uint8_t value)
{

   if(is_tx) // set the high nibble to value
      midiParser_txRxFilter = (value << 4) | (midiParser_txRxFilter & 0x0F);
   else // set the low nibble to value
      midiParser_txRxFilter = (value & 0x0F) | (midiParser_txRxFilter & 0xF0);

}
