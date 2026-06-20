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
#include "uARTFrontSYX/frontPanelReceivingProtocol.h"
#include "uARTFrontSYX/frontPanelSendingProtocol.h"
#include "MidiMessages.h"
#include "MidiOutputControl.h"
#include "CymbalVoice.h"
#include "Preset/EndpointRestore.h"
#include "Preset/TempPlaybackSwitch.h"
#include "sequencer.h"
#include <string.h>
#include "clockSync.h"
#include "MidiParser.h"
#include "automationNode.h"
#include "SomGenerator.h"
#include "TriggerOut.h"
#include "Preset/ParameterArray.h"
#include "modulationNode.h"

#define SEQ_PRESCALER_MASK 	0x03
#define MIDI_PRESCALER_MASK	0x04

static uint8_t seq_prescaleCounter = 0;

uint8_t seq_masterStepCnt=0;				/** keeps track of the played steps between 0 and 127 independent from the track counters*/
uint8_t seq_rollRate = 8;				// start with roll rate = 1/16
uint8_t seq_tempRate = 8;           // change roll on quant step, if avail
uint8_t seq_rollNote = 63;             // note roll uses - start with Dsharp5
uint8_t seq_rollVelocity = 100;
uint8_t seq_rollTriggered = 0;         /**< eacn bit ... user has triggered a roll - process on step*/
uint8_t seq_rollPlayedEarly = 0;       // roll triggered just after quant - play and note
uint8_t seq_rollState = 0;					/**< each bit represents a voice. if bit is set, roll is active*/
uint8_t seq_rollMode = ROLL_MODE_ALL;        //0=trig, 1=nte, 2=vel, 3=bth, 4=all                                      
uint8_t seq_rollCounter[NUM_TRACKS];       // runs a counter for every roll trigger
uint8_t seq_kitResetFlag=0;
uint8_t seq_skipFirstRoll=0;

uint8_t seq_voicesLoading=0;
uint8_t seq_newVoiceAvailable=0;
uint8_t seq_tracksLocked=0;
uint8_t seq_loadFastMode=0;

uint8_t seq_loopLength=0;
uint8_t seq_pendingLoopLength=0;

uint8_t seq_loopMasterStart=0;
int8_t seq_loopCurrentPosition=0;

int8_t seq_loopStartStepPosition[NUM_TRACKS+1];
int8_t seq_loopActiveStepPosition[NUM_TRACKS+1];

int8_t seq_loopUpdateFlag=0;

int8_t 	seq_stepIndex[NUM_TRACKS+1];	/**< we have 16 steps consisting of 8 sub steps = 128 steps.
											     each track has its own counter to allow different pattern lengths */
                                      // -bc- +1 so we don't have to use DRUM1 as a reference

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
uint8_t seq_perTrackActivePattern[7];
uint8_t seq_recordActive = 0;				/**< set to 1 to activate the reording mode*/

uint8_t seq_selectedStep = 0;

uint8_t seq_eraseActive=0;					/**RECORD will be 1 if live erasing the active voice  */

uint8_t seq_quantisation = QUANT_16;
uint8_t seq_stepsPerQuant = 8;



uint8_t seq_rndValue[NUM_TRACKS];			/**< random value for probability function*/

int8_t seq_barCounter;						/**< counts the absolute position in bars since the seq was started */

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

uint8_t seq_transpose_voiceAmount[7];
uint8_t seq_transposeOnOff;

//for the automation tracks each track needs 2 modNodes
static AutomationNode seq_automationNodes[NUM_TRACKS][2];

static void seq_sendRealtime(const uint8_t status);
static void seq_sendProgChg(const uint8_t ptn);
static void seq_eraseStepAndSubSteps(const uint8_t voice, const uint8_t mainStep);
static void seq_nextStep();

static uint8_t seq_isNextStepSyncStep();
static void seq_resetNote(Step *step);
static void seq_setStepIndexToStart();
static inline uint8_t seq_voiceMidiChannel(uint8_t voice)
{
   return (voice < 8) ? midi_MidiChannels[voice] : 0;
}

static inline uint8_t seq_voiceNoteOverride(uint8_t voice)
{
   return (voice < 7) ? midi_NoteOverride[voice] : 0;
}

/* Initialize the sequencer runtime, transport, and live playback caches. */
void seq_init()
{
   int i;
   
   for(i=0;i<NUM_TRACKS;i++) 
   {
      seq_perTrackActivePattern[i]=0;
      preset_tempPlaybackSwitchState.perTrackPendingPattern[i]=0;
   }
   
   for(i=0;i<NUM_TRACKS;i++) {
      autoNode_init(&seq_automationNodes[i][0]);
      autoNode_init(&seq_automationNodes[i][1]);
      midi_envPosition[i]=0;
   }

   for(i=0;i<256;i++)
   {
      midi_midiCacheAvailable[i]=0;
   }
   memset(seq_stepIndex,0,NUM_TRACKS+1);
   memset(seq_lastMasterStep,0,NUM_TRACKS+1);
   memset(seq_transpose_voiceAmount,63,NUM_TRACKS);
   memset(&preset_tmpKitState,0,sizeof(preset_tmpKitState));
   memset(&preset_normalKitState, 0, sizeof(preset_normalKitState));

   preset_resetLiveMorphApplyCache();
   memset(preset_vMorphAmount, 0, sizeof(preset_vMorphAmount));

   for(i=0;i<SEQ_SYNTH_VOICES;i++)
   {
      preset_voiceSourceState[i] = SEQ_VOICE_SOURCE_NORMAL;
   }
   preset_setIngressTarget(PRESET_PARAM_INGRESS_CURRENT_IMAGE);
   preset_tmpKitActive = 0;
   midi_clearCache();
   seq_transposeOnOff = 0;


   pat_initPatternData();

}
/* Set the global shuffle amount used by the transport clock. */
void seq_setShuffle(float shuffle)
{
   seq_shuffle = shuffle;
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
/* Set the transport tempo in beats per minute. */
void seq_setBpm(uint16_t bpm)
{
   seq_tempo 	= bpm;
	//seq_calcDeltaT(bpm);
   lfo_recalcSync();
}
//------------------------------------------------------------------------------
/* Read the current transport tempo in beats per minute. */
uint16_t seq_getBpm()
{
   return seq_tempo;
}
//------------------------------------------------------------------------------
/* Service the sync source and advance the transport if needed. */
void seq_sync()
{
   sync_tick();
}
//------------------------------------------------------------------------------
/* Queue a pattern switch request for one track or for the whole pattern.
   patNr: requested destination pattern, including temp and random sentinels.
   voice: target track selector, or 0x0f to broadcast the same request to all
   tracks. The request is staged in the temp-playback switch state and is not
   applied immediately. */
void seq_setNextPattern(const uint8_t patNr, uint8_t voice)
{
   uint8_t nextPattern = pat_normalizePatternNumber(patNr);
   if(voice>=0x0f)
   {
      preset_tempPlaybackSwitchState.pendingPattern = nextPattern;
      uint8_t i;
      for(i=0;i<NUM_TRACKS;i++)
      {
         preset_tempPlaybackSwitchState.perTrackPendingPattern[i]=nextPattern;
      }
   }
   else if((voice == 5 || voice == 6)
           && (nextPattern == SEQ_TMP_PATTERN
               || preset_tempPlaybackSwitchState.perTrackPendingPattern[5] == SEQ_TMP_PATTERN
               || preset_tempPlaybackSwitchState.perTrackPendingPattern[6] == SEQ_TMP_PATTERN
               || seq_perTrackActivePattern[5] == SEQ_TMP_PATTERN
               || seq_perTrackActivePattern[6] == SEQ_TMP_PATTERN))
   {
      /* TEMP PATTERN: closed/open hihat tracks share one synth voice and one
         parameter mask. A normal/temp boundary change for either hihat track
         must keep both tracks on the same source side. */
      preset_tempPlaybackSwitchState.perTrackPendingPattern[5] = nextPattern;
      preset_tempPlaybackSwitchState.perTrackPendingPattern[6] = nextPattern;
   }
   else
      preset_tempPlaybackSwitchState.perTrackPendingPattern[voice] = nextPattern;
   preset_tempPlaybackSwitchState.loadPendingFlag = 1;
   preset_tempPlaybackSwitchState.loadSeqNow = 1;
}
//------------------------------------------------------------------------------
static void seq_parseAutomationNodes(uint8_t track, Step* stepData)
{  
   uint8_t param1 = stepData->param1Nr;
   uint8_t param2 = stepData->param2Nr;
   uint8_t val1 = stepData->param1Val;
   uint8_t val2 = stepData->param2Val;

   if(preset_morphLoadDisabled)
   {
      if(param1>=PAR_MORPH_DRUM1&&param1<=PAR_MORPH_HIHAT)
         param1 = 0;
      if(param2>=PAR_MORPH_DRUM1&&param2<=PAR_MORPH_HIHAT)
         param2 = 0;
      preset_vMorphFlag = 0;
   }

   if(param1)
   {
      if(param1>=PAR_MORPH_DRUM1&&param1<=PAR_MORPH_HIHAT)
      {
         /* Voice morph step automation only sets the per-voice morph amount.
            It is consumed here so the generic automation node does not also
            treat PAR_MORPH_* as an ordinary DSP parameter target. */
         seq_setVoiceMorphAutomationValue((uint8_t)(param1-PAR_MORPH_DRUM1), val1);
         param1 = 0;
      }
   }
   if(param2)
   {
      if(param2>=PAR_MORPH_DRUM1&&param2<=PAR_MORPH_HIHAT)
      {
         /* See param1 path above: morph automation is control-state only. */
         seq_setVoiceMorphAutomationValue((uint8_t)(param2-PAR_MORPH_DRUM1), val2);
         param2 = 0;
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
static Step* seq_liveStepForTrack(uint8_t track, uint8_t step)
{
   if(track >= NUM_TRACKS)
      track = 0;

   return pat_getStepPtr(seq_perTrackActivePattern[track], track, step);
}
//------------------------------------------------------------------------------
static LengthRotate seq_liveLengthRotateForTrack(uint8_t track)
{
   if(track >= NUM_TRACKS)
      track = 0;

   return *pat_getLengthRotatePtr(seq_perTrackActivePattern[track], track);
}
//------------------------------------------------------------------------------
static uint8_t seq_liveMainStepActive(uint8_t track, uint8_t mainStep)
{
   if(track >= NUM_TRACKS)
      track = 0;

   return (pat_getMainSteps(seq_perTrackActivePattern[track], track) & (1<<mainStep)) > 0;
}
//------------------------------------------------------------------------------
static uint8_t seq_liveStepActive(uint8_t track, uint8_t step)
{
   return (seq_liveStepForTrack(track, step)->volume & STEP_ACTIVE_MASK) > 0;
}
//------------------------------------------------------------------------------
/* Trigger one voice from the current sequencer state.
   voiceNr selects the track source, vol is the output velocity, and note is
   the final MIDI/synth note after transpose and overrides. */
void seq_triggerVoice(uint8_t voiceNr, uint8_t vol, uint8_t note)
{
   uint8_t midiChan; // which midi channel to send a note on
   uint8_t midiNote; // which midi note to send
   Step *stepData;

   if(voiceNr > 6) 
      return;

   if(seq_tracksLocked&(0x01<<voiceNr))
      return;
   
   if(seq_newVoiceAvailable&(0x01<<voiceNr))
   {
      if(seq_loadFastMode||preset_tempPlaybackSwitchState.newPatternExecuted)
      {
         if(voiceNr>=5)
            seq_newVoiceAvailable &= (uint8_t)~0x60;
         else
            seq_newVoiceAvailable &= (uint8_t)~(0x01<<voiceNr);

         if(seq_newVoiceAvailable==0)
            preset_tempPlaybackSwitchState.newPatternExecuted=0;
      }
   }
   
   stepData = seq_liveStepForTrack(voiceNr, seq_stepIndex[voiceNr]);
   seq_parseAutomationNodes(voiceNr, stepData);

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
   uint8_t midiChannel = seq_voiceMidiChannel(voiceNr);
   if(midiChannel)
   {
      midiChan = midiChannel-1;
   
   //--AS the note that is played will be whatever is received unless we have a note override set
   // A note override is any non-zero value for this parameter
      uint8_t noteOverride = seq_voiceNoteOverride(voiceNr);
      if(noteOverride == 0)
         midiNote = note;
      else
         midiNote = noteOverride;
   
   //send the new note to midi/usb out
      seq_sendMidiNoteOn(midiChan, midiNote,
         stepData->volume&STEP_VOLUME_MASK);
   }
}
//------------------------------------------------------------------------------
/* Apply the current transpose offset for one voice.
   voice: track index.
   note: source note before transpose. Returns the adjusted note. */
uint8_t seq_getTransposedNote(uint8_t voice, uint8_t note)
{
   uint8_t retNote = note;
   uint8_t transpose = 63;
   if (seq_transposeOnOff)
   {
      // transpose is on - use track value
      transpose = seq_transpose_voiceAmount[voice];
   
      
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
   }   
   return retNote;
}
//------------------------------------------------------------------------------
static uint8_t seq_determineNextPattern()
{
   PatternSetting p;

   p = *pat_getPatternSettingPtr(seq_activePattern);

   if( (seq_barCounter>0)&&(seq_barCounter % (p.changeBar+1) == 0) )
   {
      if((seq_activePattern == SEQ_TMP_PATTERN) && (p.nextPattern == SEQ_TMP_PATTERN))
         return SEQ_TMP_PATTERN;
      return p.nextPattern;
   }
   else
      return seq_activePattern;
}
/* Capture the current transport position as the start of a loop range. */
void seq_startLoop()
{
   uint8_t i;
   
   seq_loopMasterStart = seq_masterStepCnt;
   seq_loopCurrentPosition = 0;
   
   for(i=0;i<NUM_TRACKS+1;i++)
   {
      // unscaled tracks end up 1 step ahead when they are looped. the -1 is a fix to see if it 
      // puts them in correct time
      seq_loopStartStepPosition[i] = 0x7f&(seq_stepIndex[i]-1);
      seq_loopActiveStepPosition[i] = 0x7f&(seq_stepIndex[i]-1);
   }
   seq_loopCurrentPosition=0;
}
//------------------------------------------------------------------------------
static void seq_nextStep()
{
   int i;
   if(!seq_running)
      return;
   if(
      seq_loopUpdateFlag==1 && // flag to loop - must also have either no quant or this is quant step 
      ( !( (seq_stepIndex[NUM_TRACKS]+1)%seq_stepsPerQuant)  )
      )
   {
      seq_startLoop();
      seq_loopUpdateFlag=0;
   }   
   
   //---- do we need to do a voice morph
   if(!(seq_stepIndex[NUM_TRACKS]%4))
   {
      uint8_t vMorphFlag = preset_vMorphFlag;
      preset_vMorphFlag = 0;
      if(vMorphFlag)
      {
         for (i=0;i<7;i++)
         {
            if (vMorphFlag&(0x01<<i))
               sequencer_sendVMorph((uint8_t)(0x01<<i), preset_vMorphAmount[i]);
         
         }
      }
   }
   
   seq_masterStepCnt++;
   
	//---- calc master step position. max value is 127. also take in regard the pattern length ----//
   uint8_t masterStepPos;
   uint8_t loopMasterStepPos=0;
   
   uint8_t seqlen;
   uint8_t seqscale;
   
   uint8_t activeScaledStep;
   uint8_t loopActiveScaledStep;
   
   int8_t *stepAcPtr = seq_stepIndex;
   
   uint8_t voiceTriggered=0; // did we already trigger this voice?
   
	//if( (((seq_stepIndex[0]+1) &0x7f) == 0) ||
	//    (((pat_patternSet.pat_subStepPattern[seq_activePattern][0][seq_stepIndex[0]+1]).note & PATTERN_END_MASK)>=PATTERN_END_MASK) )
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
   
   if(seq_loopLength)
   {
      stepAcPtr = seq_loopActiveStepPosition;
         
      seq_loopCurrentPosition++;
      if (seq_loopCurrentPosition>=seq_loopLength)
         seq_loopCurrentPosition-=seq_loopLength;
      
      if(seq_loopCurrentPosition)
      {
         if ( ( (seq_loopActiveStepPosition[NUM_TRACKS]+1) & 0x7f) )
            loopMasterStepPos = seq_loopActiveStepPosition[NUM_TRACKS]+1;
         else
            loopMasterStepPos = 0;
      }
      else // loop reached end, reset
      {
         loopMasterStepPos = seq_loopMasterStart;
         for(i=0;i<NUM_TRACKS+1;i++)
         {
            seq_loopActiveStepPosition[i] = seq_loopStartStepPosition[i];
         }   
      }  
   }   
   
   
   
   //-------- random pattern switch only gets calculated once per bar ------//
   if( (!masterStepPos)&&(seq_activePattern == preset_tempPlaybackSwitchState.pendingPattern) )
   {
   	//check pattern settings if we have to auto change patterns
      preset_tempPlaybackSwitchState.pendingPattern = seq_determineNextPattern();
      if((preset_tempPlaybackSwitchState.pendingPattern >= SEQ_NEXT_RANDOM)
         && !((seq_activePattern == SEQ_TMP_PATTERN) && (preset_tempPlaybackSwitchState.pendingPattern == SEQ_TMP_PATTERN)))
      {
         uint8_t limit = preset_tempPlaybackSwitchState.pendingPattern - SEQ_NEXT_RANDOM +2;
         uint8_t rnd = GetRngValue() % limit;
         preset_tempPlaybackSwitchState.pendingPattern = rnd;
      }
   }
   
	//-------- check if a pattern switch is necessary --------//
   uint8_t forceInstantSwitch =
      (switchOnNextStep || preset_tempPlaybackSwitchState.forceInstantSwitch)
      && preset_tempPlaybackSwitchState.loadSeqNow;
   if( (!masterStepPos)||forceInstantSwitch)
   {
      if((seq_activePattern != preset_tempPlaybackSwitchState.pendingPattern) || preset_tempPlaybackSwitchState.loadPendingFlag)
      {
         if(seq_resetBarOnPatternChange)
            seq_barCounter=0;
      
      	// first check if new pattern is available
         // bc - preset_tempPlaybackSwitchState.newPatternAvailable is now a register. if a complete pattern switch if necessary, 
         // it will be 0xff. if only some voices need switch, they will be represented by single bits
         // 0x01 for drum1, 0x02 for drum 2, 0x04, etc.
         
         if(preset_tempPlaybackSwitchState.newPatternAvailable)
         {
            pat_activateTmpPattern();
            preset_tempPlaybackSwitchState.newPatternAvailable = 0;
         }

         uint8_t oldTrackPattern[NUM_TRACKS];
         for(i=0;i<NUM_TRACKS;i++)
         {
            oldTrackPattern[i] = seq_perTrackActivePattern[i];
         }
         uint8_t oldActivePattern = seq_activePattern;
         uint8_t newActivePattern = pat_normalizePatternNumber(preset_tempPlaybackSwitchState.pendingPattern);
         uint8_t activePatternChanged = (oldActivePattern != newActivePattern);
         uint8_t tmpBoundaryPatternChanged = 0;
         
         seq_activePattern = newActivePattern;
         preset_setTempPlaybackActive(seq_activePattern == SEQ_TMP_PATTERN);
         preset_tempPlaybackSwitchState.newPatternExecuted=1;
         if (preset_tempPlaybackSwitchState.loadPendingFlag)
         {
            // manual switching - button switch sets all per track pending
            euklid_clearRotation();
            for (i=0;i<NUM_TRACKS;i++)
            {
               seq_perTrackActivePattern[i]=pat_normalizePatternNumber(preset_tempPlaybackSwitchState.perTrackPendingPattern[i]);
            }
         }
         else
         {
            // next pat switching - load pending pattern to per tracks
            euklid_clearRotation();
            for (i=0;i<NUM_TRACKS;i++)
            {
            
               seq_perTrackActivePattern[i]=pat_normalizePatternNumber(preset_tempPlaybackSwitchState.pendingPattern);
            }
         
         }

         preset_updateVoiceSourcesForPatternChange(oldTrackPattern, !activePatternChanged);
         tmpBoundaryPatternChanged =
            (preset_trackPatternUsesTmp(oldActivePattern)
             != preset_trackPatternUsesTmp(newActivePattern));
         for(i=0;i<NUM_TRACKS;i++)
         {
            if(preset_trackPatternUsesTmp(oldTrackPattern[i])
               != preset_trackPatternUsesTmp(seq_perTrackActivePattern[i]))
            {
               tmpBoundaryPatternChanged = 1;
            }
         }
         if(tmpBoundaryPatternChanged)
            preset_tempPlaybackSwitchState.tmpBoundaryPatternSwitchAck = 1;
      
      	//reset pattern position to pattern rotate starting position for the active pattern --AS **PATROT
         if (masterStepPos == 0){
            seq_setStepIndexToStart();
         }
         else {
            preset_tempPlaybackSwitchState.loadSeqNow=0; // pattern switch was initiated as 'instant' from front panel, reset flag
            if(seq_resetBarOnPatternChange)
               seq_barCounter = -1; // -bc- bar counter needs to be -1 to get set to 0 on first bar change
                                    // after 'instant' switch
         }
         preset_tempPlaybackSwitchState.forceInstantSwitch = 0;
         
      	// --AS send a pattern change message to midi/usb out
         seq_sendProgChg(seq_activePattern);
         
      	// --AS all notes off here since we are switching patterns
         voiceControl_noteOff(0xFF);
         
         frontPanelSending_sendPatternChange(seq_activePattern);
         
         preset_tempPlaybackSwitchState.loadPendingFlag = 0;
         if(!tmpBoundaryPatternChanged)
            seq_realign(); // realign at non-temp-boundary pattern changes
      }
   }

	//---------- now check if the master track is at a full beat position to flash the start/stop button --------
   if((masterStepPos&31) == 0)
   {
   	//&32 <=> %32
   	//a quarter beat occured (multiple of 32 steps in the 128 step pattern)
   	//turn on the start/stop led (beat indicator)
      frontPanelSending_sendBeatLed(1);
   }
   else if ((masterStepPos&31) == 1)
   {
   	//TODO datenmenge zur front reduzieren
   	//turn it of again on the next step
      frontPanelSending_sendBeatLed(0);
   }

   //---------- now check if this is a quantize step ---------------------------------------------------------
   if (!(seq_stepIndex[NUM_TRACKS]%seq_stepsPerQuant))
   {
      if (seq_rollRate != seq_tempRate) // roll rate change - queue all rolls on quant step and update
      {
         for(i=0;i<NUM_TRACKS;i++)
         {
            if (seq_rollState & (1<<i))
               seq_rollCounter[i] = 0;      
         }
         seq_rollRate = seq_tempRate;
      }
   
   }
   

	//--------- Time to process the single tracks -------------------------
   trigger_clockTick(seq_stepIndex[NUM_TRACKS]+1);
   
   for(i=0;i<NUM_TRACKS;i++)
   {
      LengthRotate lr;
      Step *stepData;
      voiceTriggered=0;
   	// --AS **PATROT we now use this for length
      lr = seq_liveLengthRotateForTrack(i);
      seqlen=lr.length;
      seqscale=lr.scale;
      
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
      
      if(seq_loopLength)
      {
         loopActiveScaledStep = !(loopMasterStepPos & (0xff >> (8-seqscale) ));
         if (loopActiveScaledStep)
            seq_loopActiveStepPosition[i]++;
            
         if((seq_loopActiveStepPosition[i] / 8) == seqlen || (seq_loopActiveStepPosition[i] & 0x7f) == 0)
         {
         	//if end is reached reset track to step 0
            seq_loopActiveStepPosition[i] = 0;
         }
      }
      //--------- Tracks @ proper stap positions, process roll -------------------------
      if( !(seq_rollState & (1<<i))&&(seq_rollTriggered & (1<<i)) ) // start new roll command received
      {
         voiceTriggered = seq_setRoll(i,1);// deals with setting roll on/off, triggering 1-shot, 
                                           // and quantizing and loading step countdown timer
                                           // for 1-shot, voiceTriggered=1,rollState bit=0
                                           // rollTriggered bit=0. All others, voiceTriggered
                                           // depends on quantize, rollstate=1,rollTriggered=1
                                           // when roll is released, rollTriggered=0 and handled
      }
      else if( (seq_rollState & (1<<i))&&!(seq_rollTriggered & (1<<i)) ) // stop roll command received
      {
         seq_setRoll(i,0); // turn off roll
      }
      
      if(!voiceTriggered)
      {
         if(seq_rollState & (1<<i)) // roll is active
         {
            voiceTriggered = seq_checkRollStep(i);// will: 1. check if roll counter=0, if so...
                                                  // 2. switch through different roll modes 3. load
                                                  // appropriate note, velo values. 4. trigger voice,
                                                  // add note as appropriate, set triggered. 5. reset                                                
                                                  // roll counter to = roll rate. 
                                                  // Then, decrement roll counter (always)
                                                  // return triggered
              
         }
      }
      if(seq_SomModeActive)
      {
         som_tick(seq_stepIndex[NUM_TRACKS],seq_mutedTracks);
      
      } 
      else if (!voiceTriggered)
      {
      	//if track is not muted
         if(!(seq_mutedTracks & (1<<i) ) )
         {
         	//if main step (associated with current substep) is active
            if(seq_liveMainStepActive(i,stepAcPtr[i]/8)) {
            
            	// --AS **RECORD if we are in erase mode (shift clear while record and playing)
            	// and this is the active track on the front, we erase the note value
            	// only do so if we are on a main step while erase is active. in this case, the main step and
            	// all it's substeps are erased.
               if(seq_eraseActive && i==frontParser_activeTrack && stepAcPtr[i]%8==0) 
               {
               	// erase the main step and all substeps
                  seq_eraseStepAndSubSteps(frontParser_activeTrack,stepAcPtr[i]/8);
               } 
               else if((seq_liveStepActive(i,stepAcPtr[i]))&&(activeScaledStep)&&(!voiceTriggered))
               {
                  //PROBABILITY
                  //every 8th step a new random value is generated
                  //thus every sub step block has only one random value to compare against
                  //allows randomisation of rolls by chance
                  
                  if((stepAcPtr[i] & 0x07) == 0x00) //every 8th step
                  {
                     seq_rndValue[i] = GetRngValue()&0x7f;
                  }
               
                  stepData = seq_liveStepForTrack(i, stepAcPtr[i]);
                  if( (seq_rndValue[i]) <= stepData->prob )
                  {
                     uint8_t vol = stepData->volume&STEP_VOLUME_MASK;
                     uint8_t note = stepData->note;
                     note = seq_getTransposedNote(i, note);
                     seq_triggerVoice(i,vol,note);
                     if(seq_loopLength&&seq_recordActive)
                        seq_addNote(i,vol,note);
                     
                  } // end if seq_rndValue
               } // end else if stepactive
            } // end if mainStepActive
         } // end if not muted
      } // end else if not triggered     
      if(seq_rollState&0x01<<i)
         seq_rollCounter[i]--;
      //else
        // seq_rollCounter[i]=seq_rollRate;       
   } // end for i=voice
   
   // increment the reference step index
   seq_stepIndex[NUM_TRACKS] = (seq_stepIndex[NUM_TRACKS]+1) & 0x7f;
   
   if(seq_loopLength)
   {
      seq_loopActiveStepPosition[NUM_TRACKS] = (seq_loopActiveStepPosition[NUM_TRACKS]+1) & 0x7f;
      if(!seq_loopCurrentPosition) // if the loop reset, see if a length change required
         seq_loopLength=seq_pendingLoopLength;           
   }

	//send message to frontpanel
	//to display the current step
   frontPanelSending_sendCurrentStepLed(stepAcPtr[frontParser_activeTrack]);

	// --AS check mtc, which might stop the sequencer if we haven't seen one in a while
   midiParser_checkMtc();

}
//------------------------------------------------------------------------------
/* Read whether external sync is enabled. */
uint8_t seq_getExtSync()
{
   return seq_isSyncExternal;
}
//------------------------------------------------------------------------------
/* Enable or disable external sync mode. */
void seq_setExtSync(uint8_t isExt)
{
   seq_isSyncExternal = isExt;
}
//------------------------------------------------------------------------------
/* Arm or clear one captured automation step.
   stepNr: step index to capture when the arm is active.
   track: track index whose automation lane is being armed.
   isArmed: non-zero to store the step location, zero to clear it. */
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
/* Override the next transport interval directly.
   delta: interval in milliseconds until the next transport step. */
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
/* Advance the external-clock master step markers by one step size. */
void seq_triggerNextMasterStep(uint8_t stepSize)
{
   uint8_t i, sn, len;
   for(i=0;i<NUM_TRACKS;i++) {
      len = seq_liveLengthRotateForTrack(i).length;
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
/* Recalculate the transport delta after a clock jump and process overdue work. */
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
/* Advance the sequencer when enough time has elapsed for the next step. */
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
/* Select the quantisation grid used for recording and roll timing. */
void seq_setQuantisation(uint8_t value)
{
   seq_quantisation = value;
   switch(seq_quantisation)
   {
      case QUANT_8:
         seq_stepsPerQuant = 16;
         break;
   
      case QUANT_16:
         seq_stepsPerQuant = 8;
         break;
   
      case QUANT_32:
         seq_stepsPerQuant = 4;
         break;
   
      case QUANT_64:
         seq_stepsPerQuant = 2;
         break;
      default:
         seq_stepsPerQuant = 1;
         break;
   }
}
//------------------------------------------------------------------------------
// --AS this appears unused
//void seq_setStep(uint8_t voice, uint8_t stepNr, uint8_t onOff)
//{
//	if(onOff)
//	{
//		pat_patternSet.pat_subStepPattern[seq_activePattern][voice][stepNr].volume |= STEP_ACTIVE_MASK;
//	}
//	else
//	{
//		pat_patternSet.pat_subStepPattern[seq_activePattern][voice][stepNr].volume &= ~STEP_ACTIVE_MASK;
//	}
//}
//------------------------------------------------------------------------------
/* Read whether the transport is currently running. */
uint8_t seq_isRunning() {
   return seq_running;
}

//------------------------------------------------------------------------------
/* Start or stop the transport and update the live playback state. */
void seq_setRunning(uint8_t isRunning)
{
   seq_running = isRunning;
   //jump to 1st step if sequencer is stopped
   if(!seq_running)
   {
   	// --AS reset all track rotations to 0. We are not saving rotated value. it's a performance tool.
      uint8_t i;
      for(i=0;i<NUM_TRACKS;i++) {
         pat_getLengthRotatePtr(seq_perTrackActivePattern[i], i)->rotate=0;
      	// let the front know this is happening
         frontPanelSending_sendTrackRotationReply(i);
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
   frontPanelSending_sendRunStop(isRunning);
}
/* Mute or unmute a track, or all tracks when trackNr is 7. */
void seq_setMute(uint8_t trackNr, uint8_t isMuted)
{
   
   if(trackNr==7)
   {
      if (isMuted)
      {
         uint8_t idx = 0;
         seq_mutedTracks = 0xFF;
         for (idx=0;idx<7;idx++)
         {
            uint8_t midiChannel = seq_voiceMidiChannel(idx);
            if(midiChannel)
               voiceControl_noteOff(midiChannel-1);
         }
      }
      else
      {
         //unmute all
         seq_mutedTracks = 0;
      }
   } 
   else {
   	//mute/unmute tracks
      if(isMuted) {
      	//mute track
         seq_mutedTracks |= (1<<trackNr);
      	// --AS turn off the midi note that may be playing on that track
         uint8_t midiChannel = seq_voiceMidiChannel(trackNr);
         if(midiChannel)
            voiceControl_noteOff(midiChannel-1);
      } 
      else {
      	//unmute track
         seq_mutedTracks &= ~(1<<trackNr);
      }
   }
};
//------------------------------------------------------------------------------
/* Read whether a track is currently muted. */
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
/* Send the main-step mask and length information for one step to the front panel. */
void seq_sendMainStepInfoToFront(uint16_t stepNr)
{
   frontPanelSending_sendMainStepInfo(stepNr);
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
/* Send the full step payload for one step index to the front panel. */
void seq_sendStepInfoToFront(uint16_t stepNr)
{
   frontPanelSending_sendStepInfo(stepNr);
}
//-------------------------------------------------------------------------------
/* Trigger one roll hit for the requested voice.
   voice: track index to fire.
   Returns non-zero when the trigger path actually emitted playback. */
uint8_t seq_rollTrig(uint8_t voice)
{
   uint8_t triggered = 0;
   Step *stepData = seq_liveStepForTrack(voice, seq_stepIndex[voice]);

   uint8_t vol = stepData->volume&0x7f;
   uint8_t note = stepData->note&0x7f;
      
   uint8_t stepActive = seq_liveMainStepActive(voice,(seq_stepIndex[voice]>>3)) &
      		seq_liveStepActive(voice,seq_stepIndex[voice]);
   
   switch(seq_rollMode)
   {
      case ROLL_MODE_TRIG:
         seq_triggerVoice(voice,vol,note);
         if(seq_recordActive)
         {
            seq_addNote(voice,vol,note);
         }
         triggered = 1;
         break;
      case ROLL_MODE_NOTE:
         if(stepActive)
         {
            note = seq_rollNote;
            seq_triggerVoice(voice,vol,note);
            if(seq_recordActive)
            {
               seq_addNote(voice,vol,note);
            }
            triggered = 1;
         }
         break;
      case ROLL_MODE_VELOCITY:
         if(stepActive)
         {
            vol = seq_rollVelocity;
            seq_triggerVoice(voice,vol,note);
            if(seq_recordActive)
            {
               seq_addNote(voice,vol,note);
            }
            triggered = 1;
         }
         break;
      case ROLL_MODE_BOTH:
         if(stepActive)
         {
            vol = seq_rollVelocity;
            note = seq_rollNote;
            seq_triggerVoice(voice,vol,note);
            if(seq_recordActive)
            {
               seq_addNote(voice,vol,note);
            }
            triggered = 1;
         }
         break;
      case ROLL_MODE_ALL:
         vol = seq_rollVelocity;
         note = seq_rollNote;
         seq_triggerVoice(voice,vol,note);
         if(seq_recordActive)
         {
            seq_addNote(voice,vol,note);
         }
         triggered = 1;
         break;
      default:
         break;
   }                  
   return triggered;
}
//-------------------------------------------------------------------------------
/* Record a change in roll button state for one voice. */
void seq_rollChange(uint8_t voice, uint8_t onOff) // a message about changing roll state was received
                                                  // note it and let the next step deal
{
   if(voice >= 7) 
   {
      return;
   }
   if(onOff) // setting roll on for this voice
   {
      seq_rollTriggered |= (1<<voice);
   }
   else 
   { // onOff is zero, turn roll off for this voice
      seq_rollTriggered &= ~(1<<voice);
      seq_rollCounter[voice] = seq_rollRate;
   }
}
//-------------------------------------------------------------------------------
/* Apply the current roll state for one voice and report whether it triggered. */
uint8_t seq_setRoll(uint8_t voice, uint8_t onOff)// called processing step if roll changed since last step
                                                 // deals with setting roll on/off, triggering 1-shot, 
                                                 // and quantizing and loading step countdown timer
                                                 // for 1-shot, voiceTriggered=1,rollState bit=0
                                                 // rollTriggered bit=0. All others, voiceTriggered
                                                 // depends on quantize, rollstate=1,rollTriggered=1
                                                 // when roll is released, rollTriggered=0 and handled
{  
   uint8_t triggered = 0;
   if(voice >= 7) 
      return triggered;
   if(onOff!=1)
   {
      seq_rollState &= ~(1<<voice);
      seq_rollPlayedEarly &= ~(1<<voice);
      return triggered;  
   }
   if(seq_rollRate == 0xff) // deal with one-shots
   {
      triggered = seq_rollTrig(voice); // trig the voice
      seq_rollCounter[voice] = 15; // set this to default 1/16 so user can change roll OTF
      seq_rollState |= (1<<voice); // we've dealt with this, let seq know we shouldn't repeat every step
      return triggered;
   }
   
   if(!seq_quantisation) // no quantization, deal with this immediately
   {
      //triggered = seq_rollTrig(voice); // trig the voice
      seq_rollCounter[voice] = 0;//seq_rollRate; // set counter
      seq_rollState |= (1<<voice);
      return triggered;
   }
   else if (!(seq_stepIndex[NUM_TRACKS]%seq_stepsPerQuant)) // quantization is on and at quant position
   {  
      seq_rollCounter[voice] = 0;//seq_rollRate; // set counter
      seq_rollState |= (1<<voice);
      return triggered;
   }
   else if (!seq_skipFirstRoll)
   {
      if ( (seq_stepIndex[NUM_TRACKS]%seq_stepsPerQuant)<(seq_stepsPerQuant/2 - 1) )
      {
         if ( !(seq_rollPlayedEarly & (1<<voice)) ) // if early roll hasn't played already
         {
            triggered = seq_rollTrig(voice);
            seq_rollPlayedEarly |= (1<<voice);
         }   
            
      }
      return triggered;
   }
   
   return 0;
}// end func
//--------------------------------------------------------------------------------
/* Check whether a roll trigger should fire on the current step. */
uint8_t seq_checkRollStep(uint8_t voice) // called every step if roll active for voice
                                         // will: 1. check if roll counter=0, if so...
                                         // 2. switch through different roll modes 3. load
                                         // appropriate note, velo values. 4. trigger voice,
                                         // add note as appropriate, set triggered. 5. reset                                                
                                         // roll counter to = roll rate. 
                                         // Then, decrement roll counter (always)
                                         // return triggered
{
   uint8_t triggered = 0;
   if(!seq_rollCounter[voice])
   {
      triggered = seq_rollTrig(voice);
      seq_rollCounter[voice] = seq_rollRate;
   }
   return triggered;
}
//--------------------------------------------------------------------------------
/* Set the roll playback note. */
void seq_setRollNote(uint8_t note)
{  
   seq_rollNote = note;
}
//--------------------------------------------------------------------------------
/* Set the roll playback velocity. */
void seq_setRollVelocity(uint8_t velocity)
{
   seq_rollVelocity = velocity;
}
//--------------------------------------------------------------------------------
/* Set the roll playback rate. */
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
         seq_tempRate = 0xff;
         break;
      case 1: // dotted bar
         seq_tempRate = 192;
         break;
   
      case 2: // bar
         seq_tempRate = 128;
         break;
   
      case 3:// dotted half
         seq_tempRate = 96;
         break;
   
      case 4:// half
         seq_tempRate = 64;
         break;
   
      case 5:// dotted quarter
         seq_tempRate = 48;
         break;
   
      case 6:// 1/4
         seq_tempRate = 32;
         break;
   
      case 7:// dotted 8th
         seq_tempRate = 24;
         break;
   
      case 8:// 1/8
         seq_tempRate = 16;
         break;
   
      case 9: // dotted 16th
         seq_tempRate = 12;
         break;
   
      case 10:// 1/16
         seq_tempRate = 8;
         break;
   
      case 11:// dotted 32nd
         seq_tempRate = 6;
         break;
   
      case 12:// 1/32
         seq_tempRate = 4;
         break;
   
      case 13:// dotted 64th
         seq_tempRate = 3;
         break;
      
      case 14://1/64
         seq_tempRate = 2;
         break;
      
      case 15://1/128
         seq_tempRate = 1;
         break;
   }
   if (!seq_quantisation)
   {
      seq_rollRate = seq_tempRate;
   }   


}
//------------------------------------------------------------------------
/** quantize a step to the seq_quantisation value*/
#define QUANT(x) (NUM_STEPS/x)
int8_t seq_quantize(int8_t step, uint8_t track)
{
   uint8_t quantisationMultiplier=1;
   uint8_t scale=pat_getLengthRotatePtr(seq_perTrackActivePattern[track], track)->scale;
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
      /*if( seq_isMainStepActive(voice,quantizedStep/8,seq_perTrackActivePattern[voice]) &&
      		seq_isStepActive(voice,quantizedStep,seq_perTrackActivePattern[voice]))
      {*/
      if(seq_activeAutomTrack == 0) {
         Step *step = pat_getStepPtr(seq_perTrackActivePattern[voice], voice, quantizedStep);
         step->param1Nr = dest;
         step->param1Val = value;
      } 
      else {
         Step *step = pat_getStepPtr(seq_perTrackActivePattern[voice], voice, quantizedStep);
         step->param2Nr = dest;
         step->param2Val = value;
      }
      
      if(!pat_isStepActive(voice,quantizedStep,seq_perTrackActivePattern[voice])&&(quantizedStep%8))
         pat_getStepPtr(seq_perTrackActivePattern[voice], voice, quantizedStep)->volume=0;
         
      //}
   }

   if( (seq_armedArmedAutomationStep	!= -1) && (seq_armedArmedAutomationTrack != -1) )
   {
   	//step button is held down
   	//-> set step automation parameters
      if(seq_activeAutomTrack == 0) {
         Step *step = pat_getStepPtr(seq_perTrackActivePattern[voice],
                                           seq_armedArmedAutomationTrack,
                                           seq_armedArmedAutomationStep);
         step->param1Nr = dest;
         step->param1Val = value;
      } 
      else {
         Step *step = pat_getStepPtr(seq_perTrackActivePattern[voice],
                                           seq_armedArmedAutomationTrack,
                                           seq_armedArmedAutomationStep);
         step->param2Nr = dest;
         step->param2Val = value;
      }
   }
}
//------------------------------------------------------------------------
void seq_recordAutomationMidiDestination(uint8_t voice, uint16_t dest, uint8_t value)
{
   uint8_t rawDest;

   if(dest == NO_AUTOMATION)
      rawDest = NO_AUTOMATION;
   else if(dest > 0 && dest < 128)
      rawDest = (uint8_t)(dest - 1);
   else
      rawDest = (uint8_t)dest;

   seq_recordAutomation(voice, rawDest, value);
}
//------------------------------------------------------------------------
void seq_addNote(uint8_t trackNr,uint8_t vel, uint8_t note)
{
   uint8_t targetPattern;
   Step *stepPtr;
	//only record notes when seq is running and recording
   if(seq_running && seq_recordActive)
   {
      int8_t unquantizedStep = seq_stepIndex[trackNr];
      int8_t quantizedStep = seq_quantize(unquantizedStep, trackNr);
   
   
   	// --AS **RECORD fix for recording across patterns
      if(quantizedStep==0 && seq_stepIndex[trackNr] > (NUM_STEPS/2)) 
      {
      	// this means that we hit a note in 2nd half of the bar and quantization pushed
      	// the note to position 0 of the next bar.
      	// need to see if there is about to be a pattern change so that the note
      	// ends up on 0 of the next pattern
         targetPattern=seq_determineNextPattern();
      
      } 
      else
         targetPattern=seq_perTrackActivePattern[trackNr];
   
   	//special care must be taken when recording midi notes!
   	//since per default the 1st substep of a mainstep cluster is always active
   	//we will get double notes when a substep other than ss1 is recorded
      if(!pat_isMainStepActive(trackNr, quantizedStep/8, targetPattern))
      {
      	//if the mainstep is not active, we clear the 1st substep
      	//to prevent double notes while recording
         pat_getStepPtr(targetPattern, trackNr, (uint8_t)((quantizedStep/8)*8))->volume 	&= ~STEP_ACTIVE_MASK;
      }
   
   	//set the current step in the requested track active
      if (vel==0)
         stepPtr=pat_getStepPtr(targetPattern, trackNr, unquantizedStep);
      else
         stepPtr=pat_getStepPtr(targetPattern, trackNr, quantizedStep);
      
   
      stepPtr->note 		= note;				// note (--AS was SEQ_DEFAULT_NOTE)
   
      stepPtr->volume	= vel;				// new velocity
      stepPtr->prob		= 127;				// 100% probability
      stepPtr->volume 	|= STEP_ACTIVE_MASK;
   
   	//activate corresponding main step
      pat_setMainStep(targetPattern, trackNr, quantizedStep/8,1);
   
      if( (frontParser_shownPattern == targetPattern) && ( frontParser_activeTrack == trackNr) )
      {
      	//update front sub step LED
         frontPanelSending_sendTriplet(FRONT_STEP_LED_STATUS_BYTE,
                                       FRONT_LED_SEQ_SUB_STEP,
                                       quantizedStep);
      	//update front main step LED
         frontPanelSending_sendTriplet(FRONT_STEP_LED_STATUS_BYTE,
                                       FRONT_LED_SEQ_BUTTON,
                                       quantizedStep);
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
   pat_setMainStep(seq_perTrackActivePattern[voice], voice, mainStep,0);

	// turn off all substeps
   for(i=(uint8_t)(mainStep*8);i<(uint8_t)((mainStep+1)*8);i++) {
      seq_resetNote(pat_getStepPtr(seq_perTrackActivePattern[voice], voice, i));
   }

	// first substep needs to be made active
   pat_getStepPtr(seq_perTrackActivePattern[voice], voice, (uint8_t)(mainStep*8))->volume |= STEP_ACTIVE_MASK;

	//if( (frontParser_shownPattern == seq_activePattern) && ( frontParser_activeTrack == voice) )
	//{
		// --AS todo if the pattern is shown on the front, update the leds
		// 		figure out. Right now it clears the LED's after shift is released... which is fine for now.
	//}

}
/* Apply transpose values to the active per-track source patterns. */
void seq_writeTranspose()
{
   uint8_t i,k,transposeAmt,trnNote;
   if (seq_transposeOnOff)
   {
      for (k=0;k<NUM_TRACKS;k++)
      {
         transposeAmt = seq_transpose_voiceAmount[k];
         if (transposeAmt!=63)
         {
            for (i=0;i<128;i++) // for all steps
            {
               // legitimate transpose value - do the transpose
               Step *step = pat_getStepPtr(seq_perTrackActivePattern[k], k, i);
               trnNote = step->note;
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
               step->note = trnNote;
            }
         }   
         seq_transpose_voiceAmount[k]=63;
      }
   }   
}   

//------------------------------------------------------------------------
/* Enable or disable live recording. */
void seq_setRecordingMode(uint8_t active)
{
   seq_recordActive = active;
}

/* Enable or disable live erasing. */
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
}
/* Select which automation lane receives live recordings. */
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

/* Send note-off messages to one MIDI channel or to every active channel. */
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
            outputControl_sendMidi(msg);
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
      outputControl_sendMidi(msg);
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
   outputControl_sendMidi(msg);
}

/* Send a note on message. This will filter out these messages if appropriate
 */
/* Send a MIDI note-on message with the requested channel, note, and velocity. */
void seq_sendMidiNoteOn(const uint8_t channel, const uint8_t note, const uint8_t veloc)
{
   static MidiMsg msg = {0,0,0, {0,0,2}};
	// --AS FILT filter out note msgs if appropriate
   if((midiParser_txRxFilter & 0x10)==0)
      return;

   msg.status=NOTE_ON | channel;
   msg.data1=note;
   msg.data2=veloc;
   outputControl_sendMidi(msg);
   
   // bc - if velocity is zero, send note off also
   if (!veloc)
   {
      msg.status=NOTE_OFF | channel;
      msg.data1=note;
      msg.data2=veloc;
      outputControl_sendMidi(msg);
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
   uint8_t midiChannel = seq_voiceMidiChannel(7);
   if(midiChannel)
   {
      static MidiMsg msg = {0,0,0, {0,0,1}};
   
   // --AS FILT filter out PC msgs if appropriate
      if((midiParser_txRxFilter & 0x80)==0)
         return;
   
      msg.status = PROG_CHANGE | (midiChannel-1);
      msg.data1=ptn;
      msg.bits.length=1;
      outputControl_sendMidi(msg);
   }
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
      LengthRotate lr = seq_liveLengthRotateForTrack(i);
   	// adjust rot in case the pattern length is less than the rotated amount
   	// len is 0-15 where a value of 0 means 16
      rot=lr.rotate;
      len=lr.length;
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
      LengthRotate lr = seq_liveLengthRotateForTrack(i);
      length=lr.length;
      if (!length)
         length=16;
      length = length << 3;
      scale=lr.scale;
      
      
      trackSteps=(stepsFromZero>>scale)%length;
      
      seq_stepIndex[i] = trackSteps;
      // resetting the stepindex also resets rotation
      pat_getLengthRotatePtr(seq_perTrackActivePattern[i], i)->rotate = 0;
   }
   
   // make sure the front panel knows that roation has been reset
   frontPanelSending_sendTrackRotationValue(0);
   
   seq_midiNoteOff(0xff);
}

/* Push one live voice-morph value to the Preset routing layer. */
void sequencer_sendVMorph(uint8_t voiceArray, uint8_t morphAmount)
{
   if(preset_morphLoadDisabled)
      return;

   seq_setVoiceMorphMaskAutomationValue(voiceArray, morphAmount);
}
