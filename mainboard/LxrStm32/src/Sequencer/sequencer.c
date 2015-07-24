/*
 * sequencer.c
 *
 *  Created on: 11.04.2012
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


#include "stm32f4xx.h"
#include "globals.h"
#include "DrumVoice.h"
#include "Snare.h"
#include "HiHat.h"
#include "random.h"
#include "Uart.h"
#include "MidiMessages.h"
#include "MidiVoiceControl.h"
#include "CymbalVoice.h"
#include "frontPanelParser.h"
#include "sequencer.h"
#include <string.h>
#include "usb_manager.h"
#include "clockSync.h"
#include "MidiParser.h"
#include "automationNode.h"
#include "SomGenerator.h"
#include "TriggerOut.h"


#define SEQ_PRESCALER_MASK 	0x03
#define MIDI_PRESCALER_MASK	0x04

static uint8_t seq_prescaleCounter = 0;

uint8_t seq_masterStepCnt=0;				/** keeps track of the played steps between 0 and 127 independent from the track counters*/
uint8_t seq_rollRate = 8;				// start with roll rate = 1/16
uint8_t seq_rollNote = 63;             // note roll uses - start with Dsharp5
uint8_t seq_rollVelocity = 100;
uint8_t seq_rollState = 0;					/**< each bit represents a voice. if bit is set, roll is active*/
uint8_t seq_lockNotes = 0;

static int8_t 	seq_stepIndex[NUM_TRACKS+1];	/**< we have 16 steps consisting of 8 sub steps = 128 steps.
											     each track has its own counter to allow different pattern lengths */
                                      // -bc- +1 so we don't have to use DRUM1 as a reference
                                      
uint8_t seq_rollCounter[NUM_TRACKS];       // runs a counter for every roll trigger

static uint16_t seq_tempo = 120;			/**< seq speed in bpm*/

static uint32_t	seq_lastTick = 0;			/**< stores the time the last step change occured*/
static float	seq_deltaT;					/**< time in [ms] until the next step
 	 	 	 	 	 	 	 	 	 	 	 1000ms = 1 sec
 	 	 	 	 	 	 	 	 	 	 	 1 min = 60 sec*/
uint8_t seq_activeAutomTrack=0;

uint8_t seq_delayedSyncStepFlag = 0;		//normally sync steps will only be advanced by external midi clocks in ext. sync mode
											//if the shuffle needs a delayed sync step, it is indicated here.

uint8_t seq_isSyncExternal = 0;
uint8_t seq_lastMasterStep[NUM_TRACKS];		//keeps track of the last triggered master sync step of each track


float seq_shuffle = 0;

static uint8_t seq_SomModeActive = 0;

static int8_t seq_armedArmedAutomationStep = -1;
static int8_t seq_armedArmedAutomationTrack = -1;

static uint8_t seq_mutedTracks=0;			/**< indicate which tracks are muted */
uint8_t seq_running = 0;					/**< 1 if running, 0 if stopped*/

uint8_t seq_activePattern = 0;				/**< the currently playing pattern*/
uint8_t seq_pendingPattern = 0;				/**< next pattern to play*/
uint8_t seq_recordActive = 0;				/**< set to 1 to activate the reording mode*/

uint8_t seq_selectedStep = 0;

uint8_t seq_eraseActive=0;					/**RECORD will be 1 if live erasing the active voice  */

uint8_t seq_quantisation = QUANT_16;



uint8_t seq_rndValue[NUM_TRACKS];			/**< random value for probability function*/

int8_t seq_barCounter;						/**< counts the absolute position in bars since the seq was started */

static uint8_t seq_loadPendigFlag = 0;
static uint8_t seq_loadSeqNow=0;

// --AS Allow it to be configured whether it keeps track of bar position in the song for
// the purpose of pattern changes
uint8_t seq_resetBarOnPatternChange = 0;
uint8_t switchOnNextStep = 0; // globally option - 0 is normal pattern switching, 1 is 'instant' switch

// --AS keep track of which midi notes are playing
static uint8_t midi_chan_notes[16];		    /**< what note is playing on each channel */
static uint16_t midi_notes_on=0;		    /**< which channels have a note currently playing */

const float seq_shuffleTable[16] =
{
		0.f,
		0.015625f,
		0.0625f,
		0.140625f,
		0.25f,
		0.390625f,
		0.5625f,
		0.765625f,
		1.f,
		0.984375f,
		0.9375f,
		0.859375f,
		0.75f,
		0.609375f,
		0.4375f,
		0.234375f,
};

float seq_lastShuffle = 0;

PatternSet seq_patternSet;

TempPattern seq_tmpPattern;

uint8_t seq_transpose_voiceAmount[7];
uint8_t seq_transposeOnOff;

uint8_t seq_newPatternAvailable = 0; //indicate that a new pattern has loaded in the background and we should switch

//for the automation tracks each track needs 2 modNodes
static AutomationNode seq_automationNodes[NUM_TRACKS][2];

static void seq_sendMidi(MidiMsg msg);
static void seq_sendRealtime(const uint8_t status);
static void seq_sendProgChg(const uint8_t ptn);
static void seq_eraseStepAndSubSteps(const uint8_t voice, const uint8_t mainStep);
static void seq_activateTmpPattern();
static void seq_nextStep();
static uint8_t seq_isNextStepSyncStep();
static uint8_t seq_intIsStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr);
static uint8_t seq_intIsMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern);
static void seq_resetNote(Step *step);
static void seq_setStepIndexToStart();
//------------------------------------------------------------------------------
void seq_init()
{
   int i;

   for(i=0;i<NUM_TRACKS;i++) {
      autoNode_init(&seq_automationNodes[i][0]);
      autoNode_init(&seq_automationNodes[i][1]);
   }

   memset(seq_stepIndex,0,NUM_TRACKS+1);
   memset(seq_lastMasterStep,0,NUM_TRACKS+1);
   memset(seq_transpose_voiceAmount,63,NUM_TRACKS);
   
   seq_transposeOnOff = 0;


   for(i=0;i<NUM_PATTERN;i++)
   {
      seq_patternSet.seq_patternSettings[i].changeBar 	= 0;	//default setting: zero repeats (play once then change)
      seq_patternSet.seq_patternSettings[i].nextPattern 	= i;	//default setting: repeat same pattern
      seq_clearPattern(i); // will clear all tracks in the pattern
   }

}
//------------------------------------------------------------------------------
static void seq_activateTmpPattern()
{
   memcpy(&seq_patternSet.seq_subStepPattern[seq_activePattern],&seq_tmpPattern.seq_subStepPattern,sizeof(Step)*NUM_TRACKS*NUM_STEPS);
   memcpy(&seq_patternSet.seq_mainSteps[seq_activePattern],&seq_tmpPattern.seq_mainSteps,sizeof(uint16_t)*NUM_TRACKS);
   memcpy(&seq_patternSet.seq_patternSettings[seq_activePattern],&seq_tmpPattern.seq_patternSettings,sizeof(PatternSetting));
   memcpy(&seq_patternSet.seq_patternLengthRotate[seq_activePattern],&seq_tmpPattern.seq_patternLengthRotate,sizeof(LengthRotate)*NUM_TRACKS);
}
//------------------------------------------------------------------------------
void seq_setShuffle(float shuffle)
{
   seq_shuffle = shuffle;
}
//------------------------------------------------------------------------------
void seq_setTrackLength(uint8_t trackNr, uint8_t length)
{
	//set new end marker
   if(length == 16)
      length=0;
	// --AS **PATROT this was changed from setting length on seq_activePattern to shown (or edited) pattern
   // --bc this gets changed to store scale - length is now bitmasked to use only the first 5 bits since it is 1-16
   seq_patternSet.seq_patternLengthRotate[frontParser_shownPattern][trackNr].length = length;

}
//------------------------------------------------------------------------------
void seq_setTrackScale(uint8_t trackNr, uint8_t scale)
{
   //set the scaling of the track - in powers of 2 from 0 to 7 ie we set a scale divisor of 1 to 128
   //this is stored as the upper 3 bits of length
   if(scale > 7)
      scale=7;
      
   seq_patternSet.seq_patternLengthRotate[frontParser_shownPattern][trackNr].scale = scale;
   
}

//------------------------------------------------------------------------------
uint8_t seq_getTrackLength(uint8_t trackNr)
{
	// --AS **PATROT this was changed from getting seq_activePattern to shown (or edited) pattern
   uint8_t r=seq_patternSet.seq_patternLengthRotate[frontParser_shownPattern][trackNr].length;
   if(r==0)
      return 16;
   return r;
}
//------------------------------------------------------------------------------
uint8_t seq_getTrackScale(uint8_t trackNr)
{
   uint8_t r=seq_patternSet.seq_patternLengthRotate[frontParser_shownPattern][trackNr].scale;
   return r;
}
//------------------------------------------------------------------------------
// **PATROT
void seq_setTrackRotation(uint8_t trackNr, const uint8_t newRot)
{
	// frontParser_shownPattern contains the pattern that is shown (being edited) on the front at the time this is called
	// seq_activePattern is the pattern that is now playing
   LengthRotate *lr=&seq_patternSet.seq_patternLengthRotate[frontParser_shownPattern][trackNr];

   if(newRot == lr->rotate)
      return;

	// if sequencer is running, move the current step position to compensate for the rotation
   if(seq_running) {
      int8_t len = 0x1f & (lr->length);
      if(len==0)
         len=16;
   
   	// this is how many main steps we need to move in one direction, negative means
   	// move back positive is move forward
      int8_t offset=(((int8_t)newRot) % len) - (((int8_t)lr->rotate) % len);
   
        // if adding the offset to si would push it over the edge either way we need to wrap it
   	// offset will always be less than len
      int16_t si = seq_stepIndex[trackNr] + (offset*8);
      if(si < 0 )
         si += (len*8);
      else if(si >= (len*8))
         si -= (len*8);
      seq_stepIndex[trackNr]=(int8_t)si;
   }

	//set new rotation value
   lr->rotate=newRot;
   
}
//------------------------------------------------------------------------------
// **PATROT
uint8_t seq_getTrackRotation(uint8_t trackNr)
{
   return seq_patternSet.seq_patternLengthRotate[frontParser_shownPattern][trackNr].rotate;
}
//------------------------------------------------------------------------------
static void seq_calcDeltaT(uint16_t bpm)
{
	//--- calc deltaT ----
	// f�r 4/4tel takt -> 1 beat = 4 main steps = 4*8 = 32 sub steps
	// 120 bpm 4/4tel = 120 * 1 beat / 60sec = 120 * 32 in 60 sec;
   seq_deltaT 	= (1000*60)/bpm; 	//bei 12 = 500ms = time for one beat
   seq_deltaT /= 96.f; //we run the internal clock at 96ppq -> seq clock 32 ppq == prescaler 3, midi clock 24 ppq == prescale 4
   seq_deltaT *= 4;
	//32.f;					// time for one beat / number of steps per beat

	//--- calc shuffle ---
   if(seq_shuffle != 0)
   {
   	//every 2nd and 4th 16th note in a beat is shifted full
   	//=> step 8 and step 24
   	//every 2nd 16th note in half a beat
   	//every beat has 32 steps => half = 16
      uint8_t stepInHalfBeat = seq_masterStepCnt&0xf;
      const float shuffleFactor = seq_shuffleTable[stepInHalfBeat] * seq_shuffle;
      const float originalDeltaT = seq_deltaT;
   
      seq_deltaT += shuffleFactor * originalDeltaT * 16.f;
      seq_deltaT -= seq_lastShuffle * originalDeltaT * 16.f;
   
      seq_lastShuffle = shuffleFactor;
   }
}
//------------------------------------------------------------------------------
void seq_setBpm(uint16_t bpm)
{
   seq_tempo 	= bpm;
	//seq_calcDeltaT(bpm);
   lfo_recalcSync();
}
//------------------------------------------------------------------------------
uint16_t seq_getBpm()
{
   return seq_tempo;
}
//------------------------------------------------------------------------------
void seq_sync()
{
   sync_tick();
}
//------------------------------------------------------------------------------
void seq_setNextPattern(const uint8_t patNr)
{
   seq_pendingPattern = patNr;
   seq_loadPendigFlag = 1;
   seq_loadSeqNow = 1;
}
//------------------------------------------------------------------------------
static void seq_sendMidi(MidiMsg msg)
{
	//send to usb midi
   usb_sendMidi(msg);

	//send to hardware midi out jack
   uart_sendMidi(msg);

}


//------------------------------------------------------------------------------
static void seq_parseAutomationNodes(uint8_t track, Step* stepData)
{  
   uint8_t param1 = stepData->param1Nr;
   uint8_t param2 = stepData->param2Nr;
   uint8_t val1 = stepData->param1Val;
   uint8_t val2 = stepData->param2Val;
   if(val1)
   {
      if(param1>=PAR_LOAD_DRUM1&&param1<=PAR_LOAD_HIHAT)
      {
            uart_sendFrontpanelByte(FRONT_SEQ_VOICE_LOAD);
            uart_sendFrontpanelByte((uint8_t)(param1-PAR_LOAD_DRUM1));
            uart_sendFrontpanelByte(val1);
      }
   }
   if(val2)
   {
      if(param2>=PAR_LOAD_DRUM1&&param2<=PAR_LOAD_HIHAT)
      {
            uart_sendFrontpanelByte(FRONT_SEQ_VOICE_LOAD);
            uart_sendFrontpanelByte((uint8_t)(param2-PAR_LOAD_DRUM1));
            uart_sendFrontpanelByte(val2);
      }
   }

   {
	   //set new destination
      autoNode_setDestination(&seq_automationNodes[track][0], param1);
      autoNode_setDestination(&seq_automationNodes[track][1], param2);
	   //set new mod value
      autoNode_updateValue(&seq_automationNodes[track][0], val1);
      autoNode_updateValue(&seq_automationNodes[track][1], val2);
   }
}
//------------------------------------------------------------------------------
void seq_triggerVoice(uint8_t voiceNr, uint8_t vol, uint8_t note)
{
   uint8_t midiChan; // which midi channel to send a note on
   uint8_t midiNote; // which midi note to send

   if(voiceNr > 6) 
      return;

   seq_parseAutomationNodes(voiceNr, &seq_patternSet.seq_subStepPattern[seq_activePattern][voiceNr][seq_stepIndex[voiceNr]]);

	//turn the trigger off before sending the next one
   if(voiceNr>=5)
   {
   	//hihat channels choke each other
      trigger_triggerVoice(5, TRIGGER_OFF);
      trigger_triggerVoice(6, TRIGGER_OFF);
   } 
   else {
      trigger_triggerVoice(voiceNr, TRIGGER_OFF);
   }
   if (vol>0){
   //--AS if a note is on for that channel send note-off first
      voiceControl_noteOff(voiceNr);
   
   //Trigger internal synth voice
      voiceControl_noteOn(voiceNr, note, vol);
   }
   midiChan = midi_MidiChannels[voiceNr];

	//--AS the note that is played will be whatever is received unless we have a note override set
	// A note override is any non-zero value for this parameter
   if(midi_NoteOverride[voiceNr] == 0)
      midiNote = note;
   else
      midiNote = midi_NoteOverride[voiceNr];

	//send the new note to midi/usb out
   seq_sendMidiNoteOn(midiChan, midiNote,
      	seq_patternSet.seq_subStepPattern[seq_activePattern][voiceNr][seq_stepIndex[voiceNr]].volume&STEP_VOLUME_MASK);
}
//------------------------------------------------------------------------------
uint8_t seq_getTransposedNote(uint8_t voice, uint8_t step, uint8_t note)
{
   uint8_t retNote = note;
   uint8_t transpose = 63;
   if (seq_transposeOnOff)
   {
      // transpose is on - use track value
      transpose = seq_transpose_voiceAmount[voice];
      if (seq_recordActive)
      {
         // set the step's transpose amount to the current voice transpose value
         seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].transpose=seq_transpose_voiceAmount[voice];  
      }
   }
   else if (seq_recordActive) // transpose is off, but record is active - transpose based on step value
   {
      transpose = seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].transpose;
   }
   
   if (transpose!=63)
   { // legitimate transpose value - do the transpose
      if (note<(63-transpose)) // transposing would result in a note less than zero!
      {
         retNote = 0;
      }
      else
      {
         retNote = (uint8_t)(note+(transpose-63));
      }
      if (retNote>127)
      {
         retNote=127;
      }
   }
   
   return retNote;
}
//------------------------------------------------------------------------------
static uint8_t seq_determineNextPattern()
{
   const PatternSetting * const p=&seq_patternSet.seq_patternSettings[seq_activePattern];
   if( (seq_barCounter>0)&&(seq_barCounter % (p->changeBar+1) == 0) )
      return p->nextPattern;
   else
      return seq_activePattern;
}
//------------------------------------------------------------------------------
static void seq_nextStep()
{

   if(!seq_running)
      return;

   seq_masterStepCnt++;

	//---- calc master step position. max value is 127. also take in regard the pattern length -----
	// track 0 determines the master step position
   uint8_t masterStepPos;
   uint8_t seqlen;
   uint8_t seqscale;
   uint8_t activeScaledStep;
	//if( (((seq_stepIndex[0]+1) &0x7f) == 0) ||
	//    (((seq_patternSet.seq_subStepPattern[seq_activePattern][0][seq_stepIndex[0]+1]).note & PATTERN_END_MASK)>=PATTERN_END_MASK) )


   if ( ( (seq_stepIndex[NUM_TRACKS]+1) & 0x7f) == 0)
   {
      masterStepPos = 0;
   	//a bar has passed
      if (seq_barCounter>=127)
         seq_barCounter = 0;
      else
         seq_barCounter++;
   }
   else
   {
      masterStepPos = seq_stepIndex[NUM_TRACKS]+1;
   }

	//-------- check if a pattern switch is necessary --------
   if(masterStepPos == 0||(switchOnNextStep && seq_loadSeqNow))
   {
      if(seq_activePattern == seq_pendingPattern)
      {
      	//check pattern settings if we have to auto change patterns
         seq_pendingPattern = seq_determineNextPattern();
         if(seq_pendingPattern >= SEQ_NEXT_RANDOM)
         {
            uint8_t limit = seq_pendingPattern - SEQ_NEXT_RANDOM +2;
            uint8_t rnd = GetRngValue() % limit;
            seq_pendingPattern = rnd;
         }
      }
   
   	// a new pattern is about to start
   	// set pendingPattern active
      if((seq_activePattern != seq_pendingPattern) || seq_loadPendigFlag)
      {
      	//--AS if this setting is active and the user has manually changed patterns,
      	// reset the bar counter. uncommenting the below will cause it to only reset
      	// when a manual pattern change is invoked. to me the whole auto pattern change modulo stuff
      	// above is a bit broken.
         if(/*seq_loadPendigFlag &&*/ seq_resetBarOnPatternChange)
            seq_barCounter=0;
      	// --AS TODO we need to also reset barCounter to 0 when the end of a repetition set of a pattern plays
      	// EVEN IF the pattern is set to play itself again, this will facilitate having the bits that play
      	// certain steps only on certain intervals of bar counter
      
         seq_loadPendigFlag = 0;
      	//first check if 2 new pattern is available
         if(seq_newPatternAvailable)
         {
            seq_newPatternAvailable = 0;
            seq_activateTmpPattern();
         }
      
         seq_activePattern = seq_pendingPattern;
      
      	//reset pattern position to pattern rotate starting position for the active pattern --AS **PATROT
         if (masterStepPos == 0){
            seq_setStepIndexToStart();
         }
         else {
            seq_loadSeqNow=0; // pattern switch was initiated as 'instant' from front panel, reset flag
            if(seq_resetBarOnPatternChange)
               seq_barCounter = -1; // -bc- bar counter needs to be -1 to get set to 0 on first bar change
                                    // after 'instant' switch
         }
      	//send the ack message to tell the front that a new pattern starts playing
         uart_sendFrontpanelByte(FRONT_SEQ_CC);
         uart_sendFrontpanelByte(FRONT_SEQ_CHANGE_PAT);
         uart_sendFrontpanelByte(seq_activePattern);
      
      	// --AS send a pattern change message to midi/usb out
         seq_sendProgChg(seq_activePattern);
      
      
      	// --AS all notes off here since we are switching patterns
         voiceControl_noteOff(0xFF);
      }
   }

	//---------- now check if the master track is at a full beat position to flash the start/stop button --------
   if((masterStepPos&31) == 0)
   {
   	//&32 <=> %32
   	//a quarter beat occured (multiple of 32 steps in the 128 step pattern)
   	//turn on the start/stop led (beat indicator)
      uart_sendFrontpanelByte(FRONT_STEP_LED_STATUS_BYTE);
      uart_sendFrontpanelByte(FRONT_LED_PULSE_BEAT);
      uart_sendFrontpanelByte(1);
   }
   else if ((masterStepPos&31) == 1)
   {
   	//TODO datenmenge zur front reduzieren
   	//turn it of again on the next step
      uart_sendFrontpanelByte(FRONT_STEP_LED_STATUS_BYTE);
      uart_sendFrontpanelByte(FRONT_LED_PULSE_BEAT);
      uart_sendFrontpanelByte(0);
   }

	//--------- Time to process the single tracks -------------------------
   trigger_clockTick(seq_stepIndex[NUM_TRACKS]+1);
   
   int i;
   for(i=0;i<NUM_TRACKS;i++)
   {
   
            
   	// --AS **PATROT we now use this for length
      seqlen=seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].length;
      seqscale=seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].scale;
      
      if(!seqlen)
         seqlen=16;
      
      // for scaled patterns - is this step one we want to process
      activeScaledStep = !(masterStepPos & (0xff >> (8-seqscale) )); 
   	//increment the step index
      if (activeScaledStep)
         seq_stepIndex[i]++;
      
      if((seq_stepIndex[i] / 8) == seqlen || (seq_stepIndex[i] & 0x7f) == 0)
      {
      	//if end is reached reset track to step 0
         seq_stepIndex[i] = 0;
      }
   
   
      if(seq_SomModeActive)
      {
         som_tick(seq_stepIndex[NUM_TRACKS],seq_mutedTracks);
      
      } 
      else {
      	//if track is not muted
         if(!(seq_mutedTracks & (1<<i) ) )
         {
         	//if main step (associated with current substep) is active
            if(seq_intIsMainStepActive(i,seq_stepIndex[i]/8,seq_activePattern)) {
            
            	// --AS **RECORD if we are in erase mode (shift clear while record and playing)
            	// and this is the active track on the front, we erase the note value
            	// only do so if we are on a main step while erase is active. in this case, the main step and
            	// all it's substeps are erased.
               if(seq_eraseActive && i==frontParser_activeTrack && seq_stepIndex[i]%8==0) {
               	// erase the main step and all substeps
                  seq_eraseStepAndSubSteps(frontParser_activeTrack,seq_stepIndex[i]/8);
               } 
               else
               // if sub-step is active
                  if(seq_intIsStepActive(i,seq_stepIndex[i],seq_activePattern))
                  {
                  //PROBABILITY
                  //every 8th step a new random value is generated
                  //thus every sub step block has only one random value to compare against
                  //allows randomisation of rolls by chance
                  
                     if((seq_stepIndex[i] & 0x07) == 0x00) //every 8th step
                     {
                        seq_rndValue[i] = GetRngValue()&0x7f;
                     }
                  
                     if( (seq_rndValue[i]) <= seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].prob )
                     {
                        uint8_t vol = seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].volume&STEP_VOLUME_MASK;
                        uint8_t note = seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].note;
                        note = seq_getTransposedNote(i, seq_stepIndex[i], note);
                        if(activeScaledStep)
                           seq_triggerVoice(i,vol,note);
                     }
                  } // if sub step is active
            } // if main step is active
         } // if this track is not muted
      }
   
   	//---- check if the roll mode has to trigger the voice
      if(seq_rollRate!=0xff) //not in oneshot mode
      {
         if(seq_rollState & (1<<i))
         {
         	//check if roll is active
            
            seq_rollCounter[i]--;
            if(seq_rollCounter[i]==0||seq_rollCounter[i]>192)
            {
               uint8_t vol;
               uint8_t note;
               // rolling, not recording - use roll control when notes are not locked
               // rolling, recording - use roll control for both if notes not locked, always record velocity
               if (seq_lockNotes)
               {
                  note = seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].note;
                  if (seq_recordActive)
                     vol = seq_rollVelocity;
                  else
                     vol =  seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].volume&0x7f;
               }
               else
               {
                  note = seq_rollNote;
                  vol  = seq_rollVelocity;
               }
               note = seq_getTransposedNote(i, seq_stepIndex[i], note);
               seq_triggerVoice(i,vol,note);
               seq_addNote(i,vol,note);
               seq_rollCounter[i] = seq_rollRate;
            } // end if rollCounter
            
         }// end if seq_rollState
         
      }//end oneshot
   
   }
   
   // increment the reference step index
   seq_stepIndex[NUM_TRACKS] = (seq_stepIndex[NUM_TRACKS]+1) & 0x7f;

	//send message to frontpanel
	//to display the current step
   uart_sendFrontpanelByte(FRONT_STEP_LED_STATUS_BYTE);
   uart_sendFrontpanelByte(FRONT_CURRENT_STEP_NUMBER_CC);
   uart_sendFrontpanelByte(seq_stepIndex[frontParser_activeTrack]);

	// --AS check mtc, which might stop the sequencer if we haven't seen one in a while
   midiParser_checkMtc();

}
//------------------------------------------------------------------------------
uint8_t seq_getExtSync()
{
   return seq_isSyncExternal;
}
//------------------------------------------------------------------------------
void seq_setExtSync(uint8_t isExt)
{
   seq_isSyncExternal = isExt;
}
//------------------------------------------------------------------------------
void seq_armAutomationStep(uint8_t stepNr, uint8_t track,uint8_t isArmed)
{
   if(isArmed) {
      seq_armedArmedAutomationStep 	= stepNr;
      seq_armedArmedAutomationTrack 	= track;
   } 
   else {
      seq_armedArmedAutomationStep 	= -1;
      seq_armedArmedAutomationTrack 	= -1;
   }
}
//------------------------------------------------------------------------------
void seq_setDeltaT(float delta)
{
   seq_deltaT = delta;
}
//------------------------------------------------------------------------------

/*This is called from IRQ handler when an external clock tick is received
 * master steps are used to keep the sync with the external clocks
 * a master step is a step that is directly triggered by the external clock signal.
 * non master steps are derived from the internaly calculated phase accumulator.
 * spacing is defined by the prescaler value
 * - with 32ppq every step is a master step
 * - with 4ppq only every 8th step is a master step
 * We set the next step index to a value - 1 because seq_nextStep() will
 * increment the value itself
 */
void seq_triggerNextMasterStep(uint8_t stepSize)
{
   uint8_t i, sn, len;
   for(i=0;i<NUM_TRACKS;i++) {
      len = seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].length;
      if(!len) // length of 0 means length of 16 (since we are using 4 bits)
         len=16;
      len *= 8; // need length in steps
   
      if(seq_lastMasterStep[i] == 0) // need to set it so next step will inc it to 0
         sn=len-1; // set to last step before wrap around, effectively 0
      else
         sn=seq_lastMasterStep[i]-1; // adjust for seq_nextStep
   
   	// establish the next step for this track for the sequencer
      seq_stepIndex[i] = sn;
   
   	// save the position where we will trigger on the next external clock tick.
   	// wrap around if we would exceed our track length
      seq_lastMasterStep[i] += stepSize;
      if(seq_lastMasterStep[i] >= len)
         seq_lastMasterStep[i] -= len;
   
   	//set time to next step to zero, forcing the sequencer to process the next step now
      seq_setDeltaT(-1);
   }
   seq_stepIndex[NUM_TRACKS]=sn;
}
//------------------------------------------------------------------------------
void seq_resetDeltaAndTick()
{
	//if there are unplayed steps jump over them
   while(!seq_isNextStepSyncStep())
   {
      seq_nextStep();
   }

	//is shuffle delay necessary?

   if(seq_shuffle != 0)
   {
   	//seq_deltaT = 0;^
   
      seq_deltaT 	= (1000*60)/seq_tempo; 	//bei 12 = 500ms = time for one beat
      seq_deltaT /= 96.f; //we run the internal clock at 96ppq -> seq clock 32 ppq == prescaler 3, midi clock 24 ppq == prescale 4
      seq_deltaT *= 4;
   
      seq_lastTick = systick_ticks;
   
      uint8_t stepInHalfBeat = seq_masterStepCnt&0xf;
      const float shuffleFactor = seq_shuffleTable[stepInHalfBeat] * seq_shuffle;
      const float originalDeltaT = seq_deltaT;
   
      seq_deltaT = shuffleFactor * originalDeltaT * 16.f;
      seq_lastShuffle = shuffleFactor;
   
      if(seq_deltaT <= 0)
      {
         seq_nextStep();
         seq_lastTick = systick_ticks;
         seq_calcDeltaT(seq_tempo);
      } 
      else
      {
         seq_delayedSyncStepFlag =1;
      }
   
      seq_prescaleCounter = 0;
   
   }
   else
   {
   	//play next sync step
      seq_nextStep();
   
      seq_lastTick = systick_ticks;
      seq_calcDeltaT(seq_tempo);
   
      seq_prescaleCounter = 0;
   }


}
//------------------------------------------------------------------------------
/** call periodically to check if the next step has to be processed */
void seq_tick()
{
   if(seq_deltaT == -1)
   {
      seq_deltaT = 32000;
      seq_nextStep();
   
      return;
   }
   if(systick_ticks-seq_lastTick >= seq_deltaT)
   {
   
      float rest = systick_ticks-seq_lastTick - seq_deltaT;
      seq_lastTick = systick_ticks;
      seq_calcDeltaT(seq_tempo);
      seq_deltaT = seq_deltaT - rest;
   
      if((seq_prescaleCounter%SEQ_PRESCALER_MASK) == 0)
      {
      	//for external sync we have a ratio of 3/4 ppq/steps
      	//so when the 3rd ppq is received we have to activate the 4th step etc
      	//advance only 2 steps automatically, then wait for sync message
      
         if(seq_getExtSync()) {
            if(seq_isNextStepSyncStep()==0) {
               seq_delayedSyncStepFlag = 0;
            
               seq_nextStep();
            }
         } 
         else {
            seq_nextStep();
         }
      }
   
      if(!seq_getExtSync()) //only send internal MIDI clock to output when external sync is off
      {
         if((seq_prescaleCounter%MIDI_PRESCALER_MASK) == 0)
         {
            seq_sendRealtime(MIDI_CLOCK);
         }
      }
      seq_prescaleCounter++;
      if(seq_prescaleCounter>=12)seq_prescaleCounter=0;
   }


}
//------------------------------------------------------------------------------
void seq_setQuantisation(uint8_t value)
{
   seq_quantisation = value;
}
//------------------------------------------------------------------------------
void seq_toggleStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
   if((seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume&STEP_ACTIVE_MASK)==0)
   {
      seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume |= STEP_ACTIVE_MASK;
   } 
   else {
      seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume &= ~STEP_ACTIVE_MASK;
   }
}
//------------------------------------------------------------------------------
void seq_toggleMainStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
   seq_patternSet.seq_mainSteps[patternNr][voice] ^= (1<<stepNr);
}
//------------------------------------------------------------------------------
static void seq_setMainStep(uint8_t patternNr, uint8_t voice, uint8_t stepNr, uint8_t onOff)
{
   if(onOff)
   {
      seq_patternSet.seq_mainSteps[patternNr][voice] |= (1<<stepNr);
   }
   else
   {
      seq_patternSet.seq_mainSteps[patternNr][voice] &= ~(1<<stepNr);
   }
}
//------------------------------------------------------------------------------
// --AS this appears unused
//void seq_setStep(uint8_t voice, uint8_t stepNr, uint8_t onOff)
//{
//	if(onOff)
//	{
//		seq_patternSet.seq_subStepPattern[seq_activePattern][voice][stepNr].volume |= STEP_ACTIVE_MASK;
//	}
//	else
//	{
//		seq_patternSet.seq_subStepPattern[seq_activePattern][voice][stepNr].volume &= ~STEP_ACTIVE_MASK;
//	}
//}
//------------------------------------------------------------------------------
uint8_t seq_isRunning() {
   return seq_running;
}

//------------------------------------------------------------------------------
void seq_setRunning(uint8_t isRunning)
{
   seq_running = isRunning;
	//jump to 1st step if sequencer is stopped
   if(!seq_running)
   {
   
   	// --AS reset all track rotations to 0. We are not saving rotated value. it's a performance tool.
      uint8_t i;
      for(i=0;i<NUM_TRACKS;i++) {
         seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].rotate=0;
      	// let the front know this is happening
         uart_sendFrontpanelByte(FRONT_SEQ_CC);
         uart_sendFrontpanelByte(FRONT_SEQ_TRACK_ROTATION);
         uart_sendFrontpanelByte(seq_getTrackRotation(i));
      }
   
   	//reset song position bar counter
      seq_lastShuffle = 0;
      seq_barCounter = 0;
      seq_masterStepCnt = 0;
   	//so the next seq_tick call will trigger the next step immediately
      seq_deltaT = 0;
      seq_sendRealtime(MIDI_STOP);
   
   	//--AS send notes off on all channels that have notes playing and reset our bitmap to reflect that
      voiceControl_noteOff(0xFF);
   
      trigger_reset(0);
      trigger_allOff();
   
   
   	// --AS if mtc was doing it's thing, tell it to stop it.
      midiParser_checkMtc();
   } 
   else {
      seq_prescaleCounter = 0;
      seq_sendRealtime(MIDI_START);
      trigger_reset(1);
   }

	// set start points back to default (happens on start and stop. needs to happen on start
	// in case the user has entered a rotate value while stopped)
   seq_setStepIndexToStart();

}
//------------------------------------------------------------------------------
static uint8_t seq_intIsStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
   return ((seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume & STEP_ACTIVE_MASK) > 0);
}
// --AS above will be inlined, below is for ext linkage
uint8_t seq_isStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
   return seq_intIsStepActive(voice,stepNr,patternNr);
}

//------------------------------------------------------------------------------
static uint8_t seq_intIsMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern)
{
   return (seq_patternSet.seq_mainSteps[pattern][voice] & (1<<mainStepNr)) > 0;
}
// --AS above is inlined below for ext linkage
uint8_t seq_isMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern)
{
   return seq_intIsMainStepActive(voice, mainStepNr, pattern);
}

//------------------------------------------------------------------------------
void seq_setMute(uint8_t trackNr, uint8_t isMuted)
{
   if(trackNr==7)
   {
   	//unmute all
      seq_mutedTracks = 0;
   } 
   else {
   	//mute/unmute tracks
      if(isMuted) {
      	//mute track
         seq_mutedTracks |= (1<<trackNr);
      	// --AS turn off the midi note that may be playing on that track
         voiceControl_noteOff(midi_MidiChannels[trackNr]);
      } 
      else {
      	//unmute track
         seq_mutedTracks &= ~(1<<trackNr);
      }
   }
};
//------------------------------------------------------------------------------
uint8_t seq_isTrackMuted(uint8_t trackNr)
{
   if(seq_mutedTracks & (1<<trackNr) )
   {
      return 1;
   }
   return 0;
}
//------------------------------------------------------------------------------
// given a pattern and a track:
// this sends the main step info (which main steps are on/off) in addition
// to the length of the track
void seq_sendMainStepInfoToFront(uint16_t stepNr)
{
	//the absolute number of patterns
   const uint8_t currentPattern	= stepNr / 7;
   const uint8_t currentTrack  	= stepNr - currentPattern*7;

   uint16_t dataToSend = seq_patternSet.seq_mainSteps[currentPattern][currentTrack];

   uart_sendFrontpanelSysExByte(  dataToSend	  & 0x7f); //1st 7 bit
   uart_sendFrontpanelSysExByte( (dataToSend>>7) & 0x7f); //2nd 7 bit
   uart_sendFrontpanelSysExByte( (dataToSend>>14)& 0x7f); //last 2 bit

	// send the track length
   uart_sendFrontpanelSysExByte(seq_patternSet.seq_patternLengthRotate[currentPattern][currentTrack].length);
   uart_sendFrontpanelSysExByte(seq_patternSet.seq_patternLengthRotate[currentPattern][currentTrack].scale);

}
//--------------------------------------------------------------------
/** this one is more complicated than the 14 bit upper/lower nibble when transmitting the requested step number via SysEx.
 * because we have to transmit seven 8-bit values that make up the StepStruct we have to pack the data clever into the 7-bit
 * SysEx packets.
 *
 * first we send the lower 7-bit of all 7 values
 * then we transmit an additional 7-bit value containing all the MSBs from the previous seven values.
 *
 * 1st - (value1 & 0x7f)
 * 2nd - (value2 & 0x7f)
 * 3rd - (value3 & 0x7f)
 * 4th - (value4 & 0x7f)
 * 5th - (value5 & 0x7f)
 * 6th - (value6 & 0x7f)
 * 7th - (value7 & 0x7f)
 *
 * 8th - (0 MSB7 MSB6 MSB5 MSB4 MSB3 MSB2 MSB1)
 */
void seq_sendStepInfoToFront(uint16_t stepNr)
{

	//decode the step number
	//the step number is between 0 and NUMBER_STEPS*NUM_TRACKS*NUM_PATTERN = 128*7*8 = 7168 steps
	// stepNr / 128 = absolute number of patterns (each track has 8 patterns * 7 tracks)
	// numPatterns / 8 = the current track

	//the absolute number of patterns
   const uint8_t absPat 			= stepNr/128;
   const uint8_t currentTrack 		= absPat / 8;
   const uint8_t currentPattern 	= absPat - currentTrack*8;
   const uint8_t currentStep		= stepNr - absPat*128;

	//encode the data and send it back
   Step *dataToSend = &seq_patternSet.seq_subStepPattern[currentPattern][currentTrack][currentStep];

   uart_sendFrontpanelSysExByte(dataToSend->volume	& 0x7f);
   uart_sendFrontpanelSysExByte(dataToSend->prob	& 0x7f);
   uart_sendFrontpanelSysExByte(dataToSend->note	& 0x7f);

   uart_sendFrontpanelSysExByte(dataToSend->param1Nr	& 0x7f);
   uart_sendFrontpanelSysExByte(dataToSend->param1Val	& 0x7f);

   uart_sendFrontpanelSysExByte(dataToSend->param2Nr	& 0x7f);
   uart_sendFrontpanelSysExByte(dataToSend->param2Val	& 0x7f);

	//now the MSBs from all 7 values
   uart_sendFrontpanelSysExByte( 	(((dataToSend->volume 	& 0x80)>>7) |
      							((dataToSend->prob	 	& 0x80)>>6) |
      							((dataToSend->note	 	& 0x80)>>5) |
      							((dataToSend->param1Nr	& 0x80)>>4) |
      							((dataToSend->param1Val	& 0x80)>>3) |
      							((dataToSend->param2Nr	& 0x80)>>2) |
      							((dataToSend->param2Val	& 0x80)>>1))&0x7f
      							);
}
//-------------------------------------------------------------------------------
void seq_setRoll(uint8_t voice, uint8_t onOff)
{
   if(voice >= 7) 
      return;
   uint8_t note = seq_getTransposedNote(voice, seq_stepIndex[voice], seq_rollNote);
   if(onOff) {
      seq_rollState |= (1<<voice);
      if(seq_rollRate == 0xff) {
      	//trigger one shot
         seq_triggerVoice(voice,seq_rollVelocity,seq_rollNote);
      	//record roll notes
         seq_addNote(voice,seq_rollVelocity,note);
      }
      else if (seq_quantisation) // quantization selected, roll start gets tricky
      {
         uint8_t whereStep,quantMult;
         /*enum Seq_QuantisationEnum
         {
         	NO_QUANTISATION,
         	QUANT_8,
         	QUANT_16,
         	QUANT_32,
         	QUANT_64,
         };*/
         quantMult = 0x01<<(seq_quantisation+2); // how many steps are in 1 quantize unit
         whereStep = seq_stepIndex[NUM_TRACKS]%quantMult;
         if (whereStep<(quantMult/2)) // just missed the quantized division, trigger immediate and short-load 1st roll count
         {
            seq_rollCounter[voice] = (seq_rollRate-whereStep)&0x7f;
            seq_triggerVoice(voice,seq_rollVelocity,seq_rollNote);
      	   //record roll notes
            seq_addNote(voice,seq_rollVelocity,note);
         }
         else // triggered before the quantize division. short-load the first roll count and let counter deal with it
         {
            seq_rollCounter[voice] = whereStep;
         }
      }
      else // not one-shot or roll quantized, trigger and start counter
      {
         seq_rollCounter[voice] = seq_rollRate;
         seq_triggerVoice(voice,seq_rollVelocity,seq_rollNote);
      	//record roll notes
         seq_addNote(voice,seq_rollVelocity,note);
      }
   } 
   else {
      seq_rollState &= ~(1<<voice);
      seq_rollCounter[voice] = seq_rollRate;
   }
};
//--------------------------------------------------------------------------------
void seq_setRollNote(uint8_t note)
{  
   seq_rollNote = note;
}
//--------------------------------------------------------------------------------
void seq_setRollVelocity(uint8_t velocity)
{
   seq_rollVelocity = velocity;
}
//--------------------------------------------------------------------------------
void seq_setRollRate(uint8_t rate)
{
	/*roll rates
      
			0 - one shot immediate trigger
         1 - dotted bar (196 steps)
			2 - 1/1
         3 - dotted half (96 steps)
			4 - 1/2
         5 - dotted quarter (48 steps)
			6 - 1/4
			7 - dotted 8th (24 steps)
			8 - 1/8
			9 - dotted 16th (12 steps)
			10 - 1/16
			11 - dotted 32nd (6 steps)
			12 - 1/32
			13 - dotted 64th (3 steps)
			14 - 1/64
			15 - 1/128
         
		 */

   switch(rate)
   {
      case 0: // one shot
         seq_rollRate = 0xff;
         break;
      case 1: // dotted bar
         seq_rollRate = 192;
         break;
   
      case 2: // bar
         seq_rollRate = 128;
         break;
   
      case 3:// dotted half
         seq_rollRate = 96;
         break;
   
      case 4:// half
         seq_rollRate = 64;
         break;
   
      case 5:// dotted quarter
         seq_rollRate = 48;
         break;
   
      case 6:// 1/4
         seq_rollRate = 32;
         break;
   
      case 7:// dotted 8th
         seq_rollRate = 24;
         break;
   
      case 8:// 1/8
         seq_rollRate = 16;
         break;
   
      case 9: // dotted 16th
         seq_rollRate = 12;
         break;
   
      case 10:// 1/16
         seq_rollRate = 8;
         break;
   
      case 11:// dotted 32nd
         seq_rollRate = 6;
         break;
   
      case 12:// 1/32
         seq_rollRate = 4;
         break;
   
      case 13:// dotted 64th
         seq_rollRate = 3;
         break;
      
      case 14://1/64
         seq_rollRate = 2;
         break;
      
      case 15://1/128
         seq_rollRate = 1;
         break;
   }


}
//------------------------------------------------------------------------
/** quantize a step to the seq_quantisation value*/
#define QUANT(x) (NUM_STEPS/x)
static int8_t seq_quantize(int8_t step, uint8_t track)
{
   uint8_t quantisationMultiplier=1;
   uint8_t scale=seq_patternSet.seq_patternLengthRotate[seq_activePattern][track].scale;
   switch(seq_quantisation)
   {
      case QUANT_8:
         quantisationMultiplier = QUANT(8);
         break;
   
      case QUANT_16:
         quantisationMultiplier = QUANT(16);
         break;
   
      case QUANT_32:
         quantisationMultiplier = QUANT(32);
         break;
   
      case QUANT_64:
         quantisationMultiplier = QUANT(64);
         break;
   
      case NO_QUANTISATION:
      default:
         return step;
         break;
   }
   // adjust for track scale
   quantisationMultiplier = quantisationMultiplier>>scale;
   if (quantisationMultiplier<1)
   {
      quantisationMultiplier = 1;
   }
	//now calc the quantisation
   float frac = step/(float)quantisationMultiplier;
   int8_t itg = (int8_t)frac;
   frac = frac - itg;

   if(frac>=0.5f)
   {
      return ((itg + 1)*quantisationMultiplier)&0x7f;
   }
   return itg*quantisationMultiplier;
}
//------------------------------------------------------------------------
void seq_recordAutomation(uint8_t voice, uint8_t dest, uint8_t value)
{
   if(seq_recordActive)
   {
      uint8_t quantizedStep = seq_quantize(seq_stepIndex[voice], voice);
   
   	//only record to active steps
      if( seq_intIsMainStepActive(voice,quantizedStep/8,seq_activePattern) &&
      		seq_intIsStepActive(voice,quantizedStep,seq_activePattern))
      {
         if(seq_activeAutomTrack == 0) {
            seq_patternSet.seq_subStepPattern[seq_activePattern][voice][quantizedStep].param1Nr = dest;
            seq_patternSet.seq_subStepPattern[seq_activePattern][voice][quantizedStep].param1Val = value;
         } 
         else {
            seq_patternSet.seq_subStepPattern[seq_activePattern][voice][quantizedStep].param2Nr = dest;
            seq_patternSet.seq_subStepPattern[seq_activePattern][voice][quantizedStep].param2Val = value;
         }
      }
   }

   if( (seq_armedArmedAutomationStep	!= -1) && (seq_armedArmedAutomationTrack != -1) )
   {
   	//step button is held down
   	//-> set step automation parameters
      if(seq_activeAutomTrack == 0) {
         seq_patternSet.seq_subStepPattern[seq_activePattern]
                                           [seq_armedArmedAutomationTrack]
                                            [seq_armedArmedAutomationStep].param1Nr = dest;
         seq_patternSet.seq_subStepPattern[seq_activePattern]
                                           [seq_armedArmedAutomationTrack]
                                            [seq_armedArmedAutomationStep].param1Val = value;
      } 
      else {
         seq_patternSet.seq_subStepPattern[seq_activePattern]
                                           [seq_armedArmedAutomationTrack]
                                            [seq_armedArmedAutomationStep].param2Nr = dest;
         seq_patternSet.seq_subStepPattern[seq_activePattern]
                                           [seq_armedArmedAutomationTrack]
                                            [seq_armedArmedAutomationStep].param2Val = value;
      }
   }
}
//------------------------------------------------------------------------
void seq_addNote(uint8_t trackNr,uint8_t vel, uint8_t note)
{
   uint8_t targetPattern;
   Step *stepPtr;
	//only record notes when seq is running and recording
   if(seq_running && seq_recordActive)
   {
      const int8_t unquantizedStep = seq_stepIndex[trackNr];
      int8_t quantizedStep;
      if (vel)
      {
         quantizedStep = seq_quantize(unquantizedStep, trackNr);
      }
      else
      {
         quantizedStep = unquantizedStep;
      }
   
   	// --AS **RECORD fix for recording across patterns
      if(quantizedStep==0 && seq_stepIndex[trackNr] > (NUM_STEPS/2)) {
      	// this means that we hit a note in 2nd half of the bar and quantization pushed
      	// the note to position 0 of the next bar.
      	// need to see if there is about to be a pattern change so that the note
      	// ends up on 0 of the next pattern
         targetPattern=seq_determineNextPattern();
      
      } 
      else
         targetPattern=seq_activePattern;
   
   	//special care must be taken when recording midi notes!
   	//since per default the 1st substep of a mainstep cluster is always active
   	//we will get double notes when a substep other than ss1 is recorded
      if(!seq_intIsMainStepActive(trackNr, quantizedStep/8, targetPattern))
      {
      	//if the mainstep is not active, we clear the 1st substep
      	//to prevent double notes while recording
         seq_patternSet.seq_subStepPattern[targetPattern][trackNr][(quantizedStep/8)*8].volume 	&= ~STEP_ACTIVE_MASK;
      }
   
   	//set the current step in the requested track active
      if (vel)
         stepPtr=&seq_patternSet.seq_subStepPattern[targetPattern][trackNr][quantizedStep];
      else
         stepPtr=&seq_patternSet.seq_subStepPattern[targetPattern][trackNr][quantizedStep];
      
      if (seq_lockNotes!=1)
      {
         stepPtr->note 		= note;				// note (--AS was SEQ_DEFAULT_NOTE)
      }
      stepPtr->volume		= vel;				// new velocity
      stepPtr->prob		= 127;				// 100% probability
      stepPtr->volume 	|= STEP_ACTIVE_MASK;
   
   	//activate corresponding main step
      seq_setMainStep(targetPattern, trackNr, quantizedStep/8,1);
   
      if( (frontParser_shownPattern == targetPattern) && ( frontParser_activeTrack == trackNr) )
      {
      	//update front sub step LED
         uart_sendFrontpanelByte(FRONT_STEP_LED_STATUS_BYTE);
         uart_sendFrontpanelByte(FRONT_LED_SEQ_SUB_STEP);
         uart_sendFrontpanelByte(quantizedStep);
      	//update front main step LED
         uart_sendFrontpanelByte(FRONT_STEP_LED_STATUS_BYTE);
         uart_sendFrontpanelByte(FRONT_LED_SEQ_BUTTON);
         uart_sendFrontpanelByte(quantizedStep);
      }
   }
}

//------------------------------------------------------------------------
// --AS **RECORD erase a main step and all it's sub steps on the active pattern
// for the specified voice
static void seq_eraseStepAndSubSteps(const uint8_t voice, const uint8_t mainStep)
{
   uint8_t i;
	// turn off the main step
   seq_setMainStep(seq_activePattern, voice, mainStep,0);

	// turn off all substeps
   for(i=(uint8_t)(mainStep*8);i<(uint8_t)((mainStep+1)*8);i++) {
      seq_resetNote(&seq_patternSet.seq_subStepPattern[seq_activePattern][voice][i]);
   }

	// first substep needs to be made active
   seq_patternSet.seq_subStepPattern[seq_activePattern][voice][(uint8_t)(mainStep*8)].volume |= STEP_ACTIVE_MASK;

	//if( (frontParser_shownPattern == seq_activePattern) && ( frontParser_activeTrack == voice) )
	//{
		// --AS todo if the pattern is shown on the front, update the leds
		// 		figure out. Right now it clears the LED's after shift is released... which is fine for now.
	//}

}

//------------------------------------------------------------------------
void seq_setRecordingMode(uint8_t active)
{
   
   uint8_t i,k,transposeAmt,trnNote;
   if (seq_recordActive&&!active) // switching from recording to not recording
   {
      for (i=0;i<128;i++) // merge transpose into notes
      {
         for (k=0;k<NUM_TRACKS;k++)
         {
            transposeAmt = seq_patternSet.seq_subStepPattern[seq_activePattern][k][i].transpose;
            if (transposeAmt!=63)
            { // legitimate transpose value - do the transpose
               trnNote = seq_patternSet.seq_subStepPattern[seq_activePattern][k][i].note;
               if (trnNote<(63-transposeAmt)) // transposing would result in a note less than zero!
               {
                  trnNote = 0;
               }
               else
               {
                  trnNote = (uint8_t)(trnNote+transposeAmt-63);
               }
               if (trnNote>127)
               {
                  trnNote=127;
               }
               seq_patternSet.seq_subStepPattern[seq_activePattern][k][i].note = trnNote;
            }
         }
      }
   }
   seq_recordActive = active;
}

void seq_setErasingMode(uint8_t active)
{
   seq_eraseActive = active;
}

//------------------------------------------------------------------------------
// --AS reset a step to it's default state
static void seq_resetNote(Step *step)
{
   step->note 		= SEQ_DEFAULT_NOTE;
   step->param1Nr 	= NO_AUTOMATION;
   step->param1Val = 0;
   step->param2Nr	= NO_AUTOMATION;
   step->param2Val	= 0;
   step->prob		= 127;
   step->volume	= 100; // clears active bit as well
   step->transpose = 63;
}
//------------------------------------------------------------------------------
void seq_clearTrack(uint8_t trackNr, uint8_t pattern)
{
   int k;
   for(k=0;k<128;k++)
   {
      seq_resetNote(&seq_patternSet.seq_subStepPattern[pattern][trackNr][k]);
   
   	//every 1st step in a substep pattern active
      if( (k%8) == 0)
         seq_patternSet.seq_subStepPattern[pattern][trackNr][k].volume |= STEP_ACTIVE_MASK ;
   }

	// all main steps off for this track
   seq_patternSet.seq_mainSteps[pattern][trackNr] = 0;

	//**PATROT all pattern rotations off and all patterns set to length 16
   seq_patternSet.seq_patternLengthRotate[pattern][trackNr].rotate=0;

}
//------------------------------------------------------------------------------
void seq_clearPattern(uint8_t pattern)
{
   int i;
   for(i=0;i<NUM_TRACKS;i++)
      seq_clearTrack(i, pattern);
}

//------------------------------------------------------------------------------
void seq_clearAutomation(uint8_t trackNr, uint8_t pattern, uint8_t automTrack)
{
   int k;

   if(automTrack==0)
   {
      for(k=0;k<128;k++)
      {
         seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param1Nr 	= NO_AUTOMATION;
         seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param1Val 	= 0;
      }
   } 
   else {
      for(k=0;k<128;k++)
      {
         seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param2Nr		= NO_AUTOMATION;
         seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param2Val	= 0;
      }
   }
}
//------------------------------------------------------------------------------
void seq_copyTrack(uint8_t srcNr, uint8_t dstNr, uint8_t pattern)
{
	// --AS todo this fn and the next are very similar. Add a new fn that takes src, dest track and src, dest pat
	// and then have these two fn's call it instead.
   int k;
   Step *src, *dst;
   for(k=0;k<128;k++)
   {
      dst=&seq_patternSet.seq_subStepPattern[pattern][dstNr][k];
      src=&seq_patternSet.seq_subStepPattern[pattern][srcNr][k];
      dst->note		= src->note;
      dst->param1Nr 	= src->param1Nr;
      dst->param1Val 	= src->param1Val;
      dst->param2Nr	= src->param2Nr;
      dst->param2Val	= src->param2Val;
      dst->prob		= src->prob;
      dst->volume		= src->volume;
   }

	// copy which main steps are on/off
   seq_patternSet.seq_mainSteps[pattern][dstNr] = seq_patternSet.seq_mainSteps[pattern][srcNr];

	// --AS copy length and rotation offset from source track
   seq_patternSet.seq_patternLengthRotate[pattern][dstNr].value =
      	seq_patternSet.seq_patternLengthRotate[pattern][srcNr].value;


}
//------------------------------------------------------------------------------
void seq_copyPattern(uint8_t src, uint8_t dst)
{
   int k,j;
   Step *psrc, *pdst;
   for(j=0;j<NUM_TRACKS;j++)
   {
      for(k=0;k<128;k++)
      {
         pdst=&seq_patternSet.seq_subStepPattern[dst][j][k];
         psrc=&seq_patternSet.seq_subStepPattern[src][j][k];
         pdst->note			= psrc->note;
         pdst->param1Nr 		= psrc->param1Nr;
         pdst->param1Val 	= psrc->param1Val;
         pdst->param2Nr		= psrc->param2Nr;
         pdst->param2Val		= psrc->param2Val;
         pdst->prob			= psrc->prob;
         pdst->volume		= psrc->volume;
         pdst->transpose   = psrc->transpose;
      }
   
      seq_patternSet.seq_mainSteps[dst][j] = seq_patternSet.seq_mainSteps[src][j];
   
   	// --AS copy length and rotation offset from source pattern for the track
      seq_patternSet.seq_patternLengthRotate[dst][j].value =
         		seq_patternSet.seq_patternLengthRotate[src][j].value;
   
   }
}
//------------------------------------------------------------------------------
void seq_copyTrackPattern(uint8_t srcNr, uint8_t dstPat, uint8_t srcPat)
// srcNr = source voice
{
   int k;
   Step *psrc, *pdst;
   for(k=0;k<128;k++)
   {
      pdst=&seq_patternSet.seq_subStepPattern[dstPat][srcNr][k];
      psrc=&seq_patternSet.seq_subStepPattern[srcPat][srcNr][k];
      pdst->note			= psrc->note;
      pdst->param1Nr 		= psrc->param1Nr;
      pdst->param1Val 	= psrc->param1Val;
      pdst->param2Nr		= psrc->param2Nr;
      pdst->param2Val		= psrc->param2Val;
      pdst->prob			= psrc->prob;
      pdst->volume		= psrc->volume;
      pdst->transpose   = pdst->transpose;
   }
   
   seq_patternSet.seq_mainSteps[dstPat][srcNr] = seq_patternSet.seq_mainSteps[srcPat][srcNr];
   
   	// --AS copy length and rotation offset from source pattern for the track
   seq_patternSet.seq_patternLengthRotate[dstPat][srcNr].value =
         		seq_patternSet.seq_patternLengthRotate[srcPat][srcNr].value;
   
}
//------------------------------------------------------------------------------
void seq_copySubStep(uint8_t src, uint8_t dst, uint8_t track)
{
   Step *psrc, *pdst;
   
      // copy the step data

         pdst=&seq_patternSet.seq_subStepPattern[seq_activePattern][track][dst];
         psrc=&seq_patternSet.seq_subStepPattern[seq_activePattern][track][src];
         pdst->note			= psrc->note;
         pdst->param1Nr 		= psrc->param1Nr;
         pdst->param1Val 	= psrc->param1Val;
         pdst->param2Nr		= psrc->param2Nr;
         pdst->param2Val		= psrc->param2Val;
         pdst->prob			= psrc->prob;
         pdst->volume		= psrc->volume;
         pdst->transpose   = psrc->transpose;
      

}
//------------------------------------------------------------------------------
void seq_setActiveAutomationTrack(uint8_t trackNr)
{
   seq_activeAutomTrack = trackNr;
}
//------------------------------------------------------------------------------
static uint8_t seq_isNextStepSyncStep()
{
   if(seq_delayedSyncStepFlag)
   {
      seq_delayedSyncStepFlag = 0;
      seq_prescaleCounter = 0;
      return 0;
   }
   // -bc- use a dummy step index at the end as reference instead of DRUM1
   if( ((seq_stepIndex[NUM_TRACKS] & 0x3) % 4) == 3) {
      return 1;
   }
   return 0;
}
//------------------------------------------------------------------------------

void seq_midiNoteOff(uint8_t chan)
{
   uint8_t i;
   MidiMsg msg;

	// we are not filtering according to tx filter because they might have turned that
	// setting on while a note was sustaining

   msg.bits.length=2;
   msg.data2=0;

   if(chan==0xff) { // all notes off
      for(i=0; i<16; i++)
         if((1<<i) & midi_notes_on) {
            msg.status=	NOTE_OFF | i;
            msg.data1=midi_chan_notes[i];
            seq_sendMidi(msg);
         }
   	// reset all
      midi_notes_on=0;
      return;
   }
	// The proper way to do a note off is with 0x80. 0x90 with velocity 0 is also used, however I think there is still
	// synth gear out there that doesn't recognize that properly.
   if((1<<chan) & midi_notes_on) {
      msg.status=	NOTE_OFF | chan;
      msg.data1=midi_chan_notes[chan];
      seq_sendMidi(msg);
   	// turn off our knowledge of that note playing
      midi_notes_on &= (~(1<<chan));
   }
}

static void seq_sendRealtime(const uint8_t status)
{
   static MidiMsg msg = {0,0,0, {0,0,0}};
	// --AS FILT filter out realtime msgs if appropriate
   if((midiParser_txRxFilter & 0x20)==0)
      return;
   msg.status=status;
   seq_sendMidi(msg);
}

/* Send a note on message. This will filter out these messages if appropriate
 */
void seq_sendMidiNoteOn(const uint8_t channel, const uint8_t note, const uint8_t veloc)
{
   static MidiMsg msg = {0,0,0, {0,0,2}};
	// --AS FILT filter out note msgs if appropriate
   if((midiParser_txRxFilter & 0x10)==0)
      return;

   msg.status=NOTE_ON | channel;
   msg.data1=note;
   msg.data2=veloc;
   seq_sendMidi(msg);
   
   // bc - if velocity is zero, send note off also
   if (!veloc)
   {
      msg.status=NOTE_OFF | channel;
      msg.data1=note;
      msg.data2=veloc;
      seq_sendMidi(msg);
   }
   
	// keep track of which notes are on so we can turn them off later
   midi_chan_notes[channel]=note;
   midi_notes_on |= (1 << channel);

}

/* This will send a prog change on the global channel and will filter
 * out the message if appropriate
 */
static void seq_sendProgChg(const uint8_t ptn)
{
   static MidiMsg msg = {0,0,0, {0,0,1}};

	// --AS FILT filter out PC msgs if appropriate
   if((midiParser_txRxFilter & 0x80)==0)
      return;

   msg.status = PROG_CHANGE | midi_MidiChannels[7];
   msg.data1=ptn;
   msg.bits.length=1;
   seq_sendMidi(msg);
}

/* **PATROT set the step starting index to the position where the pattern rotation would have it start.
 *  A pattern rotation of 0 means start at the beginning of the pattern. max value is 15.
 *  Each value represents a main step interval (which contains 8 substeps)
 *
 *  This is called when the sequencer starts/stops running, also when a pattern change takes place
 */
static void seq_setStepIndexToStart()
{
   uint8_t len, rot, i;
   for(i=0;i<NUM_TRACKS;i++) {
   	// adjust rot in case the pattern length is less than the rotated amount
   	// len is 0-15 where a value of 0 means 16
      rot=seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].rotate;
      len=seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].length;
      if(len && (rot > len))
         rot = rot % len;
   
   	// this is for external clock sync via trigger expansion kit (the ext tick will adjust this -1)
      seq_lastMasterStep[i] = (8 * rot);
   
   	// -1 here because we increment it first thing when we start
      seq_stepIndex[i] = ( 8 * rot) - 1;
   
   }
   // -bc- use a dummy step index at the end as rotation reference instead of DRUM1
   seq_stepIndex[NUM_TRACKS] = ( 8 * rot) - 1;
}

/* BC - this gets called when attempting to switch to a pattern that is already viewed.
   normally, this would do nothing, this adds the functionality that all tracks re-align to where
   they should be based on the current bar and master step count. This lets the user manually 
   create variations with length and scale changes and return to the original pattern quickly*/
void seq_realign()
{
   uint8_t length, scale, i, bar;
   if(seq_barCounter<=0)
      bar=0;
   else 
      bar=seq_barCounter-1;
   uint16_t stepsFromZero = seq_stepIndex[NUM_TRACKS] + (uint16_t)(128*bar);
   uint16_t trackSteps;
   
   for(i=0;i<NUM_TRACKS;i++) {
      length=seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].length;
      if (!length)
         length=16;
      length = length << 3;
      scale=seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].scale;
      
      
      trackSteps=(stepsFromZero>>scale)%length;
      
      seq_stepIndex[i] = trackSteps;
      // resetting the stepindex also resets rotation
      seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].rotate = 0;
   }
   
   // make sure the front panel knows that roation has been reset
   uart_sendFrontpanelByte(FRONT_SEQ_CC);
   uart_sendFrontpanelByte(FRONT_SEQ_TRACK_ROTATION);
   uart_sendFrontpanelByte(0);
   
   seq_midiNoteOff(0xff);
}