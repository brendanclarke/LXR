/*
 * frontPanelParser.c
 *
 *  Created on: 27.04.2012
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



#include "frontPanelParser.h"
#include "MidiMessages.h"
#include "FrontPanelProtocol.h"
#include "frontPanelSendingProtocol.h"
#include "MidiParser.h"
#include "Preset/EndpointRestore.h"
#include "Preset/MorphEngine.h"
#include "Preset/ParameterArray.h"
#include "sequencer.h"
#include "PatternData.h"
#include "Preset/ParameterIngress.h"
#include "Uart.h"
#include "SD_Manager.h"
#include "EuklidGenerator.h"
#include "config.h"
#include "mixer.h"

#include "DrumVoice.h"
#include "CymbalVoice.h"
#include "HiHat.h"
#include "Snare.h"
#include "SomGenerator.h"
#include "TriggerOut.h"
#include <string.h>

void frontParser_handleMidiMessage(void);
static void frontParser_handleSysexData(unsigned char data);
static void frontParser_handleSeqCC();

#define FLOW_INITIAL_GRANT 4
#define FLOW_ACK_CREDITS 1
#define FRONT_FILE_DONE_TYPE_PERFORMANCE 8
#define FRONT_FILE_DONE_TYPE_ALL 9

static uint8_t comm_loadSessionActive = 0;
static uint8_t comm_quietUi = 0;
static uint8_t comm_flowActive = 0;
static uint8_t comm_flowChannel = 0;
static uint8_t comm_flowBudgetRemaining = 0;
static uint8_t frontParser_fileLoadIngressActive = 0;
static uint8_t frontParser_fileLoadBracketActive = 0;

static void frontParser_beginFileLoadIngress(uint8_t bracketed)
{
   frontParser_fileLoadIngressActive = 1;
   if(bracketed)
      frontParser_fileLoadBracketActive = 1;

   preset_setIngressTarget(SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT);
   preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
}

static void frontParser_endFileLoadIngress(void)
{
   frontParser_fileLoadIngressActive = 0;
   frontParser_fileLoadBracketActive = 0;
   preset_setIngressTarget(SEQ_PARAM_INGRESS_CURRENT_IMAGE);
   preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
}

static void frontParser_clearVoiceLoading(uint8_t voice)
{
   if(voice >= 5)
      seq_voicesLoading &= (uint8_t)~0x60;
   else
      seq_voicesLoading &= (uint8_t)~(0x01 << voice);
}

extern MidiMsg frontParser_midiMsg;
extern uint8_t frontParser_sysexActive;

/* Flow-control bookkeeping for the transitional receive-side parser. */
static uint8_t frontParser_isFlowMessage(uint8_t status, uint8_t data1)
{
   return (status == FRONT_SEQ_CC)
      && (data1 >= FRONT_SEQ_FLOW_BEGIN)
      && (data1 <= FRONT_SEQ_FLOW_ABORT);
}

static void frontParser_sendFlowGrant(uint8_t channel, uint8_t credits)
{
   if((frontParser_sysexActive != SYSEX_INACTIVE) && (channel != FLOW_CH_LOAD_SESSION))
   {
      comm_flowActive = 0;
      comm_flowBudgetRemaining = 0;
      return;
   }

   frontPanelSending_sendFlowGrant(channel, credits);
}

static void frontParser_sendFlowGrantWait(uint8_t channel, uint8_t credits)
{
   frontPanelSending_sendFlowGrantWait(channel, credits);
}

static void frontParser_sendPrfCacheStatus(uint8_t command, uint8_t status)
{
   frontPanelSending_sendPrfCacheStatus(command, status);
}

static void frontParser_suspendCreditFlowForSysex()
{
   comm_flowActive = 0;
   comm_flowBudgetRemaining = 0;
}

static void frontParser_flowMessageApplied()
{
   if(!comm_flowActive || frontParser_isFlowMessage(frontParser_midiMsg.status, frontParser_midiMsg.data1))
      return;

   if(comm_flowBudgetRemaining)
      comm_flowBudgetRemaining--;

   if(comm_flowBudgetRemaining == 0)
   {
      comm_flowBudgetRemaining = FLOW_INITIAL_GRANT;
      frontParser_sendFlowGrant(comm_flowChannel, FLOW_INITIAL_GRANT);
   }
}

uint8_t frontParser_isQuietUi()
{
   /* Query whether the parser is currently suppressing ordinary UI traffic. */
   return comm_quietUi;
}
/* Front-panel receive state: byte counter, current message assembly, sysex
   mode, and the current display/track selection. */
uint8_t frontParser_rxCnt=0;

/* Currently selected parameter / current assembled MIDI message. */
MidiMsg frontParser_midiMsg;

/* Currently active front-panel track and the visible sequence display state. */
uint8_t frontParser_activeFrontTrack=0;

/* Sysex mode gates all non-sysex traffic while a front-panel transfer is open. */
uint8_t frontParser_sysexActive=0;
/* Two 7-bit bytes assembled into one 14-bit value. */
uint16_t frontParser_twoByteData=0;
static uint8_t frontParser_globalMorphLsb = 0;

uint8_t frontParser_sysexBuffer[16];

/* Counter for incoming sequencer step data packets. */
uint16_t frontParser_sysexSeqStepNr=0;

/* Current track/pattern display selection and step-copy source. */
uint8_t frontParser_activeTrack=0;	/** the active track on the Frontpanel. track specific messages refer to the track selected with this command*/
uint8_t frontParser_shownPattern = 0;
uint8_t frontParser_activeStep=0;

uint8_t frontParser_stepCopySource=0;

/* Main front-panel receive entry point. */
void frontParser_parseUartData(unsigned char data)
{

	//TODO der ganze sysex kram kann sicher noch optimiert werden
	//das das nicht andauernd abgefragt werden muss

	// if high bit is set, a new message starts
   if(data&0x80)
   {
   	//reset the byte counter
      frontParser_rxCnt = 0;
      frontParser_midiMsg.status = data;
      if(data==SYSEX_START)
      {
         frontParser_suspendCreditFlowForSysex();
         frontParser_sysexActive = SYSEX_ACTIVE_MODE_NONE;
         uart_clearFrontFifo();
         
      	//send SYSEX_START as ACK
         frontPanelSending_sendSysexStartAck();
      }
      else if(data==SYSEX_END)
      {
         frontPanelSending_sendSysexEndAck();
         frontParser_sysexActive = SYSEX_INACTIVE;
      }
      else if(data==PATCH_RESET)
      {
         seq_newVoiceAvailable=0x7f;
      }
      else if(data==FRONT_CALLBACK_ACK)
      {
         frontPanelSending_sendCallbackAck();
      }
      else
      {
         frontParser_sysexActive = SYSEX_INACTIVE;
      }
   }
   else // data byte
   {
      if(frontParser_midiMsg.status == SYSEX_START)
      {
         frontParser_handleSysexData(data);
      }
      else if(frontParser_rxCnt==0) //normal operation
      {
      	//parameter nr
         frontParser_midiMsg.data1 = data;
         frontParser_rxCnt++;
      }
      else
      {
      	//parameter value
         frontParser_midiMsg.data2 = data;
      	//message received. process the midi message
         frontParser_handleMidiMessage();
      }
   }
};

//------------------------------------------------------
// This is called when we are in sysex mode and are receiving the sysex bytes
static void frontParser_handleSysexData(unsigned char data)
{
	//SYSEX
	// this is not a correct sysex implementation.
	// in the front panel communication it is used to send sequencer data for preset saving
	// while the preset saving is active, no other data has to be send over the uart!!!
	// so when the SYSEX_START is received, all other communication is stopped until theSYSEX_END message is received
	// no abort and no manufacturer id is supported

	//we have received sysex data
	//first we receive a mode message
	//SYSEX_REQUEST_STEP_DATA or SYSEX_SEND_STEP_DATA
	//then the corresponding data

   switch(frontParser_sysexActive) {
      case SYSEX_REQUEST_PATTERN_DATA:
      //1 byte = pattern nr
      //send back next and repeat
         frontPanelSending_sendPatternDataReply(data);
         break;
   
      case SYSEX_REQUEST_MAIN_STEP_DATA:
      //we expect a 2 byte message containing a step nr
      //which tells us which main step data to send back
         frontParser_rxCnt++;
         if(frontParser_rxCnt == 1)
         {
         //first nibble received -> upper nibble
            frontParser_twoByteData = data<<7;
         }
         else
         {
         //second nibble -> lower nibble
            frontParser_twoByteData |= data&0x7f;
         //reset rxCount for next 2 nibble message
            frontParser_rxCnt = 0;
         //we have received a complete 2 nibble step nr
         //send data back to front
            seq_sendMainStepInfoToFront(frontParser_twoByteData);
         }
         break;
   
      case SYSEX_REQUEST_STEP_DATA:
      //we expect a 2 byte message containing a step nr
      //which tells us which step data to send back
         frontParser_rxCnt++;
      
         if(frontParser_rxCnt == 1)
         {
         //first nibble received -> upper nibble
            frontParser_twoByteData = data<<7;
         }
         else
         {
         //second nibble -> lower nibble
            frontParser_twoByteData |= data&0x7f;
         //reset rxCount for next 2 nibble message
            frontParser_rxCnt = 0;
         //we have received a complete 2 nibble step nr
         //send data back to front
            seq_sendStepInfoToFront(frontParser_twoByteData);
         }
      
         break;
   
      
      case SYSEX_RECEIVE_MAIN_STEP_DATA:
         if(frontParser_rxCnt<3)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         
         //calculate the step pattern and track indices
            uint8_t currentTrack = (frontParser_sysexBuffer[0]>>3)&0x07;
            uint8_t currentPattern = (frontParser_sysexBuffer[0]&0x07);
         
            uint16_t mainStepData = frontParser_sysexBuffer[1] | frontParser_sysexBuffer[2]<<7 | frontParser_sysexBuffer[3]<<14;
         
         // File receive writes directly to normal pattern storage.
            PatternSet* patternSet = &seq_patternSet;
            patternSet->seq_mainSteps[currentPattern][currentTrack] = mainStepData;
         
            
            frontParser_rxCnt = 0;
            frontPanelSending_sendSysexReceiveAck(SYSEX_RECEIVE_MAIN_STEP_DATA);
         }
         break;
      case SYSEX_RECEIVE_PAT_CHAIN_DATA:
         if(frontParser_rxCnt<1)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
            //calculate the pattern index. we expect 'next', then all 'repeat' for pat 0-7
            const uint8_t currentPattern	= frontParser_sysexSeqStepNr;
            uint8_t next = frontParser_sysexBuffer[0];
            uint8_t repeat = frontParser_sysexBuffer[1];
            // File receive writes directly to normal pattern storage.
            PatternSet* patternSet = &seq_patternSet;
            patternSet->seq_patternSettings[currentPattern].nextPattern = next;
            patternSet->seq_patternSettings[currentPattern].changeBar = repeat;
            //inc the step counter
            frontParser_sysexSeqStepNr++;
            frontParser_rxCnt = 0;
         
         }
         frontPanelSending_sendSysexReceiveAck(SYSEX_RECEIVE_PAT_CHAIN_DATA);
         break;
      case SYSEX_RECEIVE_PAT_LEN_DATA:
      // --AS same as above but we are receiving length data for each pattern
         if(frontParser_rxCnt<1)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         //calculate the step pattern and track indices
            uint8_t currentTrack = (frontParser_sysexBuffer[0]>>3)&0x07;
            uint8_t currentPattern = (frontParser_sysexBuffer[0]&0x07);
         // File receive writes directly to normal pattern storage.
            PatternSet* patternSet = &seq_patternSet;
         
            patternSet->seq_patternLengthRotate[currentPattern][currentTrack].length = data;
            
            frontParser_rxCnt = 0;
            frontPanelSending_sendSysexReceiveAck(SYSEX_RECEIVE_PAT_LEN_DATA);
         }
         break;
         
      case SYSEX_RECEIVE_PAT_SCALE_DATA:
       // --BC same as above but we are receiving scale data for each pattern
         if(frontParser_rxCnt<1)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
         
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         //calculate the step pattern and track indices
            uint8_t currentTrack = (frontParser_sysexBuffer[0]>>3)&0x07;
            uint8_t currentPattern = (frontParser_sysexBuffer[0]&0x07);
            /*
            const uint8_t currentPattern	= frontParser_sysexSeqStepNr / 7;
            const uint8_t currentTrack  	= frontParser_sysexSeqStepNr - currentPattern*7;
            */
         
         // File receive writes directly to normal pattern storage.
            PatternSet* patternSet = &seq_patternSet;
         
            patternSet->seq_patternLengthRotate[currentPattern][currentTrack].scale = data;
         
         
            //inc the step counter
            frontParser_sysexSeqStepNr++;
            frontParser_rxCnt = 0;
         
         // File receive no longer stages a background pattern switch here.
         /*
            if( seq_isRunning() && (frontParser_sysexSeqStepNr == NUM_TRACKS*NUM_PATTERN)) {
               preset_tempPlaybackSwitchState.newPatternAvailable = 1;
            }
            */
            frontPanelSending_sendSysexReceiveAck(SYSEX_RECEIVE_PAT_SCALE_DATA);
         }
         
         break;
      
   
      case SYSEX_RECEIVE_STEP_DATA:
      // we expect a bunch of 8 byte sysex message containing new step data for the sequencer
      // beginning with step 0 up to NUMBER_STEPS*NUM_TRACKS*NUM_PATTERN = 128*7*8 = 7168 steps
         if(frontParser_rxCnt<7)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
         //now we have to distribute the MSBs to the sysex data
            uint8_t i;
            for(i=0;i<7;i++)
            {
               frontParser_sysexBuffer[i] |= ((data&(1<<i))<<(7-i));
            
            }
         
         //calculate the step pattern and track indices
            const uint8_t absPat 	= frontParser_sysexSeqStepNr/128;
            const uint8_t currentTrack 		= absPat / 8;
            const uint8_t currentPattern 	= absPat - currentTrack*8;
            const uint8_t currentStep		= frontParser_sysexSeqStepNr - absPat*128;
         
            PatternSet* patternSet = &seq_patternSet;
         /*
            if((seq_newPatternVoiceArray&(0x01<<currentTrack))==0)
            { // track is not in the array, use current step data instead
               frontParser_sysexBuffer[0] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].volume;
               frontParser_sysexBuffer[1] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].prob;
               frontParser_sysexBuffer[2] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].note;
               frontParser_sysexBuffer[3] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Nr;
               frontParser_sysexBuffer[4] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Val;
               frontParser_sysexBuffer[5] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Nr;
               frontParser_sysexBuffer[6] = patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Val;
            }
         */
         // File receive writes directly to normal pattern storage.
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].volume 	= frontParser_sysexBuffer[0];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].prob 	= frontParser_sysexBuffer[1];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].note 	= frontParser_sysexBuffer[2];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Nr = frontParser_sysexBuffer[3];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Val 	= frontParser_sysexBuffer[4];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Nr 	= frontParser_sysexBuffer[5];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Val 	= frontParser_sysexBuffer[6];
         //signal that a new data chunk is available
         //frontParser_newSeqDataAvailable = 1;
         //reset receive counter for next chunk
            frontParser_rxCnt = 0;
         
         //inc the step counter
            frontParser_sysexSeqStepNr++;
         }
         break;  
         
      case SYSEX_BEGIN_PATTERN_TRANSMIT:
        // we expect a bunch of 8 byte sysex message containing new step data for the sequencer
        // beginning with step 0 up to NUMBER_STEPS*NUM_TRACKS*NUM_PATTERN = 128*7*8 = 7168 steps
         if(frontParser_rxCnt<8)
         {
            frontParser_sysexBuffer[frontParser_rxCnt++] = data;
         }
         else
         {
            uint8_t currentTrack = (frontParser_sysexBuffer[0]>>3)&0x07;
            uint8_t currentPattern = (frontParser_sysexBuffer[0]&0x07);
            uint8_t currentStep = frontParser_sysexSeqStepNr;//frontParser_sysexBuffer[2];
            
            //now we have to distribute the MSBs to the sysex data
            uint8_t i;
            for(i=0;i<7;i++)
            {
               frontParser_sysexBuffer[i] = frontParser_sysexBuffer[i+1];
               frontParser_sysexBuffer[i] |= ((data&(1<<i))<<(7-i));
            
            }
         
            PatternSet* patternSet = &seq_patternSet;
            
         // File receive writes directly to normal pattern storage.
         
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].volume 	= frontParser_sysexBuffer[0];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].prob 	= frontParser_sysexBuffer[1];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].note 	= frontParser_sysexBuffer[2];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Nr = frontParser_sysexBuffer[3];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param1Val 	= frontParser_sysexBuffer[4];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Nr 	= frontParser_sysexBuffer[5];
            patternSet->seq_subStepPattern[currentPattern][currentTrack][currentStep].param2Val 	= frontParser_sysexBuffer[6];
            //signal that a new data chunk is available
            //frontParser_newSeqDataAvailable = 1;
            //reset receive counter for next chunk
            frontParser_rxCnt = 0;
            
            if(frontParser_sysexSeqStepNr<127)
            {
               frontParser_sysexSeqStepNr++;
               frontPanelSending_sendSysexStepAck();
            }
            else
            {
               frontPanelSending_sendSysexBeginPatternTransmitAck();
            }
         }
         break;  
      
      case SYSEX_ACTIVE_MODE_NONE:
      default:
      
      //we received a mode message -> set the active mode
         frontParser_sysexActive = data;
         frontParser_sysexSeqStepNr = 0;
         frontParser_rxCnt = 0;
         break;
   }
}
/* Handle a fully assembled front-panel message after byte decoding. */
void frontParser_handleMidiMessage(void)
{
   switch(frontParser_midiMsg.status)
   {
      case PARAM_RESTORE_READY:
         preset_tmpKitHandshakeReady = 1;
         return;

      case PARAM_RESTORE_ACK:
         preset_tmpKitHandshakeAck = 1;
         return;

      default:
         break;
   }

   switch(frontParser_midiMsg.status)
   {
      case PRF_RESTORE_PARAM_CC:
      case PRF_RESTORE_PARAM_CC2:
         {
            uint16_t paramNr = frontParser_midiMsg.data1;
            if(frontParser_midiMsg.status == PRF_RESTORE_PARAM_CC2)
               paramNr += 128;

            /* PRF_RESTORE_PARAM_* carries raw kit/front endpoint bytes. These
               are not live low MIDI CC numbers, so do not apply the +1 offset. */
            preset_storeParameterIngress(paramNr, frontParser_midiMsg.data2);
         }
         break;

      case PRF_RESTORE_MORPH_CC:
      case PRF_RESTORE_MORPH_CC2:
         {
            uint16_t paramNr = frontParser_midiMsg.data1;
            if(frontParser_midiMsg.status == PRF_RESTORE_MORPH_CC2)
               paramNr += 128;

            /* RESTORE: Store morph parameter endpoint bytes into the endpoint image
               selected by the current transfer context. These raw selector bytes are
               distinct from the resolved automation target sideband messages. */
            uint8_t currentTarget = preset_getIngressTarget();
            if(paramNr < END_OF_SOUND_PARAMETERS)
            {
               if(currentTarget == SEQ_PARAM_INGRESS_CURRENT_IMAGE)
               {
                  preset_storeMorphParameterIngress(paramNr, frontParser_midiMsg.data2);
               }
               else
               {
                  preset_normalKitState.morphEndpointParams[paramNr] = frontParser_midiMsg.data2;
               }
            }
         }
         break;

      case FRONT_CC_MACRO_TARGET: //frontParser_midiMsg.status
         {
            uint8_t applyLive = preset_shouldApplyIngressToLive();
         
            /* MACRO_CC message structure
            byte1 - status byte 0xaa as above
            byte2, data1 byte: xtta aa-b : tt= top level macro value sent (2 macros exist now, we can do 2 more if we want)
                                           aaa= macro destination value sent (4 destinations exist now, can do 8)
                                           b=macro mod target value top bit
                                           I have left a blank bit above this to make it easier to make more than 255 kit parameters
                                           if we ever want to take on that can of worms
                                          
            byte3, data2 byte: xbbb bbbb : b=macro mod target value lower 7 bits or top level value full
            */
            
            uint8_t upper = frontParser_midiMsg.data1;
            uint8_t lower = frontParser_midiMsg.data2;
            //sequencer_sendVMorph(0,(uint8_t)(lower*macroModulators[0].amount));
            if (upper&0x20)
            {
               float value = ((float)(lower))/127.f;
               // top level macro amount message received
               if(applyLive)
               {
                  modNode_updateValue(&macroModulators[0],(value));
                  modNode_updateValue(&macroModulators[1],(value));
               }
            }
            else if (upper&0x40)
            {
               float value = ((float)(lower))/127.f;
               // top level macro amount message received
               if(applyLive)
               {
                  modNode_updateValue(&macroModulators[2],(value));
                  modNode_updateValue(&macroModulators[3],(value));
               }
            }
            else
            {
               // macro destination message
               uint16_t value = (uint16_t)( ( (upper&0x03)<<8) | lower);
               uint8_t whichModDest = (uint8_t)( 0x07&(upper>>2) ); // whichModDest 0,1,2,3 mac1d1,mac1d2,mac2d1,mac2d2
               preset_storeMacroDestinationIngress(whichModDest, value);
               if(applyLive)
               {
                  modNode_setDestination(&macroModulators[whichModDest], value);
                  modNode_updateValue(&macroModulators[whichModDest],macroModulators[whichModDest].lastVal);
               }
            }
         
         }
         break; // case FRONT_CC_LFO_TARGET
      //SEQ MESSAGES
      case FRONT_SEQ_CC: // frontParser_midiMsg.status
         frontParser_handleSeqCC();
         break;
   
      case SAMPLE_CC:
         switch(frontParser_midiMsg.data1)
         {
         
            case FRONT_SAMPLE_START_UPLOAD:
               seq_setRunning(0);
               sampleMemory_init();
               sampleMemory_loadSamples();
               FLASH_Lock();
            
               frontPanelSending_sendSampleUploadAck();
               break;
         
            case FRONT_SAMPLE_COUNT:
               frontPanelSending_sendSampleCountReply();
               break;
         
            default:
               break;
         }
         break;
   
   //MIDI SYNTH MESSAGES
      case MIDI_CC: //frontParser_midiMsg.status
         // this is for parameters below 128
         {
            uint8_t rawParam = frontParser_midiMsg.data1;
            MidiMsg liveMsg = frontParser_midiMsg;

            liveMsg.data1 = (uint8_t)((rawParam + 1) & 0x7f);

            if(preset_shouldApplyIngressToLive())
            {
               preset_storeParameterIngress(rawParam, frontParser_midiMsg.data2);
               midiParser_ccHandler(liveMsg,0);

            //record automation if record is turned on
               seq_recordAutomation(frontParser_activeTrack, rawParam, frontParser_midiMsg.data2);
            }
            else
            {
               /* FILE LOAD: Store normal-image params without changing the
                  temporary sound when the temporary pattern is active. */
               preset_storeParameterIngress(rawParam, frontParser_midiMsg.data2);
            }
         }
         break;
   
   //CC2 above 127
      case FRONT_CC_2: // frontParser_midiMsg.status
         {
            if(preset_shouldApplyIngressToLive())
            {
               midiParser_ccHandler(frontParser_midiMsg,1);
               //record automation if record is turned on
               seq_recordAutomation(frontParser_activeTrack, frontParser_midiMsg.data1+128, frontParser_midiMsg.data2);
            }
            else
            {
               /* FILE LOAD: Store normal-image params without changing the
                  temporary sound when the temporary pattern is active. */
               preset_storeParameterIngress(frontParser_midiMsg.data1+128, frontParser_midiMsg.data2);
            }
         }
         break;
   
   
      case FRONT_CC_LFO_TARGET: //frontParser_midiMsg.status
         {
            uint8_t upper = frontParser_midiMsg.data1;
            uint8_t lower = frontParser_midiMsg.data2;
            uint8_t applyLive = preset_shouldApplyIngressToLive();
         // --AS **PATROT note that the only valid values for the following are listed in
         // the modTargets array in the AVR code
            uint8_t value = ((upper&0x01)<<7) | lower;
         
            uint8_t lfoNr = (upper&0xfe)>>1;
            preset_storeLfoDestinationIngress(lfoNr, value);
            
            if(!applyLive)
            {
               /* RESTORE: Endpoint-copy automation target sidebands are stored only.
                  They must not touch the currently sounding modulation nodes. */
            }
            else
            {
               switch(lfoNr)
               {
                  case 0:
                  case 1:
                  case 2:	modNode_setDestination(&voiceArray[lfoNr].lfo.modTarget, value);
                     break;
                  case 3:	modNode_setDestination(&snareVoice.lfo.modTarget,value);		
                     break;
                  case 4:	modNode_setDestination(&cymbalVoice.lfo.modTarget, value);		
                     break;
                  case 5:	modNode_setDestination(&hatVoice.lfo.modTarget, value);			
                     break;
                  default:
                     break;
               }
            }
         }
         break; // case FRONT_CC_LFO_TARGET
   
      case FRONT_SET_P1_DEST: 
         { // frontParser_midiMsg.status
         // --AS **AUTOM add 1 to the value as our cortex parameters are off by 1 for the lower 127 params
            uint8_t hi = frontParser_midiMsg.data1;
            uint8_t lo = frontParser_midiMsg.data2;
            uint8_t val = (hi<<7)|lo;
            if(val && val < 128 )
               val++;
            seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, seq_selectedStep)->param1Nr = val;
         }
         break;
      case FRONT_SET_P2_DEST: 
         { // frontParser_midiMsg.status
         //--AS **AUTOM same here
            uint8_t hi = frontParser_midiMsg.data1;
            uint8_t lo = frontParser_midiMsg.data2;
            uint8_t val = (hi<<7)|lo;
            if(val && val < 128 )
               val++;
            seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, seq_selectedStep)->param2Nr = val;
         }
         break;
      case FRONT_SET_P1_VAL: 
         { // frontParser_midiMsg.status
            uint8_t stepNr = frontParser_midiMsg.data1;
            uint8_t value = frontParser_midiMsg.data2;
            seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, stepNr)->param1Val = value;
         
         }
         break;
      case FRONT_SET_P2_VAL: 
         { // frontParser_midiMsg.status
            uint8_t stepNr = frontParser_midiMsg.data1;
            uint8_t value = frontParser_midiMsg.data2;
            seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, stepNr)->param2Val = value;
         }
         break;
   
      case FRONT_ARM_AUTOMATION_STEP: // frontParser_midiMsg.status
         {
            const uint8_t stepNr 	= frontParser_midiMsg.data1;
            const uint8_t onOff 	= frontParser_midiMsg.data2 & 0x40;
            const uint8_t trackNr 	=  frontParser_midiMsg.data2 & 0x3f;
         
            seq_armAutomationStep(stepNr,trackNr,onOff);
         
         }
         break;
   
      case FRONT_MAIN_STEP_CC: // frontParser_midiMsg.status
         {
         //data 1 = track und pattern nr
         //data 2 = step nr
            uint8_t voiceNr 	= frontParser_midiMsg.data1 >> 4;
            uint8_t patternNr 	= seq_normalizePatternNumber(frontParser_midiMsg.data1 & 0x0f);
            uint8_t stepNr 		= frontParser_midiMsg.data2 & 0x1f;
         
         
            if (frontParser_midiMsg.data2 & 0x40) // flag for force ON
            {
               seq_setMainStep(patternNr, voiceNr, stepNr, 1);
            }
            else if (frontParser_midiMsg.data2 & 0x20) // flag for force OFF
            {
               seq_setMainStep(patternNr, voiceNr, stepNr, 0);
            }
            
            else  //toggle the step in the seq
            {
               seq_toggleMainStep(voiceNr, stepNr, patternNr);
            }   
         
         //if step active send led on message to front
            frontPanelSending_sendMainStepLedReply(voiceNr, stepNr, patternNr);
         }
         break;
   
      case FRONT_STEP_CC: // frontParser_midiMsg.status
         {
         //data 1 = track und pattern nr
         //data 2 = step nr
            uint8_t voiceNr 	= frontParser_midiMsg.data1 >> 4;
            uint8_t patternNr 	= seq_normalizePatternNumber(frontParser_midiMsg.data1 & 0x0f);
            uint8_t stepNr 		= frontParser_midiMsg.data2;
         
         //toggle the step in the seq
            seq_toggleStep(voiceNr, stepNr, patternNr);
         
         }
         break;
   
      case FRONT_CC_VELO_TARGET: // frontParser_midiMsg.status
         {
            uint8_t upper = frontParser_midiMsg.data1;
            uint8_t lower = frontParser_midiMsg.data2;
            uint8_t applyLive = preset_shouldApplyIngressToLive();
         // --AS **PATROT note that the only valid values for the following are listed in
         // the modTargets array in the AVR code
            uint8_t value = ((upper&0x01)<<7) | lower;
            uint8_t velModNr = (upper&0xfe)>>1;
            preset_storeVelocityDestinationIngress(velModNr, value);
            if(!applyLive)
            {
               /* RESTORE: Endpoint-copy automation target sidebands are stored only.
                  They must not touch the currently sounding modulation nodes. */
            }
            else
            {
               /* Velocity-to-VMORPH is applied once at trigger time by the
                  sequencer; keep PAR_MORPH_* out of the generic velocity node. */
               if(value >= PAR_MORPH_DRUM1 && value <= PAR_MORPH_HIHAT)
                  modNode_setDestination(&velocityModulators[velModNr], PAR_NONE);
               else
                  modNode_setDestination(&velocityModulators[velModNr], value);
            }
	         }
         break;
   
   //VOICE option Messages
      case VOICE_CC: // frontParser_midiMsg.status
         break;
   
   //BPM MESSAGE
      case FRONT_SET_BPM: 
         { // frontParser_midiMsg.status
            uint16_t bpm = frontParser_midiMsg.data1 |(uint16_t)(frontParser_midiMsg.data2<<7);
            if(bpm == 0) {
               seq_setExtSync(1);
            }
            else {
               seq_setBpm(bpm);
            
               seq_setExtSync(0);
            }
         }
         break;
   
   //LED MESSAGES
      case FRONT_STEP_LED_STATUS_BYTE: // frontParser_midiMsg.status
         switch(frontParser_midiMsg.data1)
         {
         //send all active step numbers to frontpanel to light up corresponding LEDs
            case FRONT_LED_QUERY_SEQ_TRACK:
            {
                  uint8_t trackNr = frontParser_midiMsg.data2 >> 4;
                  uint8_t patternNr = seq_normalizePatternNumber(frontParser_midiMsg.data2 & 0x0f);
               
                  frontParser_activeFrontTrack = trackNr;
                  frontPanelSending_updateTrackLeds(trackNr, patternNr, frontParser_activeStep);
               
               //send track length back
                  frontPanelSending_sendTrackLengthReply(trackNr);
               
               // **PATROT send track rotation value back
                  frontPanelSending_sendTrackRotationReply(trackNr);
               
            }
               break;
            case FRONT_LED_ALL_SUBSTEP:
               {
                  uint8_t trackNr = frontParser_midiMsg.data2 >> 4;
                  uint8_t patternNr = seq_normalizePatternNumber(frontParser_midiMsg.data2 & 0x0f);
               
                  frontPanelSending_updateSubStepLeds(trackNr, patternNr, frontParser_activeStep);               
               }
               break;
         
            default:
               break;
         }
         break;
   } // frontParser_midiMsg.status

   frontParser_flowMessageApplied();
}
//------------------------------------------------------
// Sequencer message handler
// This is called when we have received a message with status FRONT_SEQ_CC
static void frontParser_handleSeqCC()
{
   switch(frontParser_midiMsg.data1)
   {
      case FRONT_SEQ_FLOW_BEGIN:
         if(frontParser_midiMsg.data2 == FLOW_CH_LOAD_SESSION)
         {
            comm_loadSessionActive = 1;
            comm_quietUi = 1;
            comm_flowActive = 0;
            comm_flowBudgetRemaining = 0;
            frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         }
         else
         {
            comm_flowActive = 1;
            comm_flowChannel = (uint8_t)(frontParser_midiMsg.data2 & 0x07);
            comm_flowBudgetRemaining = FLOW_INITIAL_GRANT;
            frontParser_sendFlowGrant(comm_flowChannel, FLOW_INITIAL_GRANT);
         }
         break;

      case FRONT_SEQ_FLOW_END:
         if(frontParser_midiMsg.data2 == FLOW_CH_LOAD_SESSION)
         {
            comm_loadSessionActive = 0;
            comm_quietUi = 0;
            comm_flowActive = 0;
            comm_flowBudgetRemaining = 0;
            frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         }
         else
         {
            uint8_t channel = (uint8_t)(frontParser_midiMsg.data2 & 0x07);
            if(comm_flowActive && (comm_flowChannel == channel))
            {
               comm_flowActive = 0;
               comm_flowBudgetRemaining = 0;
            }
            frontParser_sendFlowGrant(channel, FLOW_ACK_CREDITS);
         }
         break;

      case FRONT_SEQ_FLOW_ABORT:
         comm_loadSessionActive = 0;
         comm_quietUi = 0;
         comm_flowActive = 0;
         comm_flowBudgetRemaining = 0;
         frontParser_endFileLoadIngress();
         break;

      case FRONT_SEQ_FLOW_GRANT:
         break;

      case FRONT_SEQ_PRF_CACHE_BEGIN:
         /* The PRF cache/background-load regime is deprecated. File loads now
            write directly to normal storage; AVR may keep probing this opcode
            until its initiators are removed, so reject it cleanly. */
         frontParser_sendPrfCacheStatus(FRONT_SEQ_PRF_CACHE_BEGIN, PRF_CACHE_REJECTED);
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_PENDING_BEGIN:
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_PENDING_DONE:
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_CACHE_ABORT:
         frontParser_endFileLoadIngress();
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_AVR_SNAPSHOT_BEGIN:
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_AVR_SNAPSHOT_END:
         frontParser_sendFlowGrant(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_PRF_RESTORE_AVR_LIVE:
         frontParser_sendFlowGrantWait(FLOW_CH_LOAD_SESSION, FLOW_ACK_CREDITS);
         break;

      case FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN:
      {
         uint8_t endpointMode = frontParser_midiMsg.data2;

         /* RESTORE: Switch ingress target to normal kit endpoint buffer.
            Subsequent parameter and target messages will populate preset_normalKitState endpoint images.
            The data byte selects which endpoint group is being refreshed, so file
            loads can update only the kit/front endpoint or only the morph parameter
            endpoint without clearing the other side. */
         preset_setIngressTarget(SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT);
         preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);

         if(endpointMode != FRONT_SEQ_TMP_KIT_ENDPOINT_MORPH_ONLY)
         {
            memset(preset_normalKitState.kitEndpointParams, 0, END_OF_SOUND_PARAMETERS);
            memset(&preset_normalKitState.frontPanelAutomationTargets,
                   0,
                   sizeof(preset_normalKitState.frontPanelAutomationTargets));
         }

         if(endpointMode != FRONT_SEQ_TMP_KIT_ENDPOINT_FRONT_ONLY)
         {
            memset(preset_normalKitState.morphEndpointParams, 0, END_OF_SOUND_PARAMETERS);
            memset(&preset_normalKitState.morphParameterEndpointAutomationTargets,
                   0,
                   sizeof(preset_normalKitState.morphParameterEndpointAutomationTargets));
         }
         break;
      }

      case FRONT_SEQ_TMP_KIT_AUTOMATION_PHASE:
         /* RESTORE: Inside the copy-to-temp endpoint bracket, raw param opcodes
            identify parameter_values[] vs parameters2[]. Resolved automation target
            sidebands need this explicit phase marker so the STM stores them with
            the matching endpoint image and does not apply them to live audio. */
         if(frontParser_midiMsg.data2 == FRONT_SEQ_TMP_KIT_AUTOMATION_FRONT_ENDPOINT)
            preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_FRONT_ENDPOINT);
         else if(frontParser_midiMsg.data2 == FRONT_SEQ_TMP_KIT_AUTOMATION_MORPH_ENDPOINT)
            preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_MORPH_ENDPOINT);
         else
            preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
         break;

      case FRONT_SEQ_TMP_KIT_ENDPOINT_END:
         preset_applyNormalEndpointAutomationTargets();

         /* RESTORE: Switch ingress target back to the surrounding context. During
            file load, endpoint dumps are nested inside normal kit-endpoint ingress. */
         preset_setIngressTarget(frontParser_fileLoadIngressActive
                                ? SEQ_PARAM_INGRESS_NORMAL_KIT_ENDPOINT
                                : SEQ_PARAM_INGRESS_CURRENT_IMAGE);
         preset_setAutomationIngressTarget(SEQ_AUTOMATION_INGRESS_NONE);
         break;

      case FRONT_SEQ_SET_GLOBAL_MORPH:
         /* AVR menu/global morph resets all STM per-voice morph amounts. Per
            voice step automation or modulation may later overwrite individual
            voices without asking AVR to recompute morph. */
         seq_setGlobalMorphAmount(frontParser_midiMsg.data2);
         break;

      case FRONT_SEQ_SET_GLOBAL_MORPH_LSB:
         frontParser_globalMorphLsb = (uint8_t)(frontParser_midiMsg.data2 & 0x7f);
         break;

      case FRONT_SEQ_SET_GLOBAL_MORPH_MSB:
      {
         uint8_t morphAmount =
            (uint8_t)(frontParser_globalMorphLsb
               | ((frontParser_midiMsg.data2 & 0x01) << 7));
         seq_setGlobalMorphAmount(morphAmount);
         break;
      }

      case FRONT_SEQ_REQUEST_PATTERN_PARAMS:

      /* send back bar change and next pattern params from requested pattern*/
         frontPanelSending_sendPatternParamsReply(frontParser_shownPattern);
      
         break;
      case FRONT_SEQ_SET_PAT_BEAT:
         seq_getPatternSettingPtr(frontParser_shownPattern)->changeBar = frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_SET_PAT_NEXT:
         seq_getPatternSettingPtr(frontParser_shownPattern)->nextPattern = frontParser_midiMsg.data2;
         break;
   
      case FRONT_SEQ_REC_ON_OFF:
         seq_setRecordingMode(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_ERASE_ON_OFF:
         seq_setErasingMode(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_NOTE:
         seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, frontParser_activeStep)->note = frontParser_midiMsg.data2;
         break;
   
      case FRONT_SEQ_VOLUME:
         seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, frontParser_activeStep)->volume &= ~(0x7f);
         seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, frontParser_activeStep)->volume |= (frontParser_midiMsg.data2&0x7f);
         break;
   
      case FRONT_SEQ_PROB:
      
         seq_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, frontParser_activeStep)->prob = frontParser_midiMsg.data2;
         break;
         
      case FRONT_SEQ_EUKLID_RESET:
         {
            euklid_clearRotation();
         }
         break;
   
      case FRONT_SEQ_EUKLID_LENGTH:
         {
            uint8_t pattern = (frontParser_shownPattern == SEQ_TMP_PATTERN) ? SEQ_TMP_PATTERN : (frontParser_midiMsg.data2&0x07);
            uint8_t length 	= frontParser_midiMsg.data2 >> 3;
            length += 1;
            euklid_setLength(frontParser_activeTrack, pattern, length);
            frontParser_activeFrontTrack = frontParser_activeTrack;
            frontPanelSending_updateTrackLeds(frontParser_activeTrack, pattern, frontParser_activeStep);
         }
         break;
   
      case FRONT_SEQ_EUKLID_STEPS:
         {
            uint8_t steps 	= frontParser_midiMsg.data2 >> 3;
            steps += 1;
            uint8_t pattern = (frontParser_shownPattern == SEQ_TMP_PATTERN) ? SEQ_TMP_PATTERN : (frontParser_midiMsg.data2 & 0x7);
         
            euklid_setSteps(frontParser_activeTrack,steps,pattern);
            frontParser_activeFrontTrack = frontParser_activeTrack;
            frontPanelSending_updateTrackLeds(frontParser_activeTrack, pattern, frontParser_activeStep);
         }
         break;
   
      case FRONT_SEQ_EUKLID_ROTATION:
         {
            uint8_t rotation = frontParser_midiMsg.data2 >> 3;
         //rotation += 1;
            uint8_t pattern = (frontParser_shownPattern == SEQ_TMP_PATTERN) ? SEQ_TMP_PATTERN : (frontParser_midiMsg.data2 & 0x7);
         
            euklid_setRotation(frontParser_activeTrack,rotation,pattern);
            frontParser_activeFrontTrack = frontParser_activeTrack;
            frontPanelSending_updateTrackLeds(frontParser_activeTrack, pattern, frontParser_activeStep);
         }
         break;
         
      case FRONT_SEQ_EUKLID_SUBSTEP_ROTATION:
         {
            uint8_t rotation = frontParser_midiMsg.data2 >> 3;
         //rotation += 1;
            uint8_t pattern = (frontParser_shownPattern == SEQ_TMP_PATTERN) ? SEQ_TMP_PATTERN : (frontParser_midiMsg.data2 & 0x7);
         
            euklid_setSubStepRotation(frontParser_activeTrack,rotation,pattern);
            frontParser_activeFrontTrack = frontParser_activeTrack;
            frontPanelSending_updateTrackLeds(frontParser_activeTrack, pattern, frontParser_activeStep);
         }
         break;   
   
      case FRONT_SEQ_CLEAR_TRACK: 
         {
            seq_clearTrack(frontParser_midiMsg.data2, frontParser_shownPattern);
         }
         break;
   
      case FRONT_SEQ_CLEAR_PATTERN:
         seq_clearPattern(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_POSX:
         som_setX(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_POSY:
         som_setY(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_FLUX:
         som_setFlux(frontParser_midiMsg.data2/127.f);
         break;
   
      case FRONT_SEQ_SOM_FREQ:
         som_setFreq(frontParser_midiMsg.data2,frontParser_activeTrack);
         break;
   
      case FRONT_SEQ_MIDI_CHAN:
         {
            uint8_t voice = (frontParser_midiMsg.data2 >> 4)&0x07;
            uint8_t channel = (frontParser_midiMsg.data2&0x0f)+1;
            
            // --AS if midi channel changed, and a note was playing on old channel, turn it off
            if(voice < 7 && midi_MidiChannels[voice] != channel)
               voiceControl_noteOff(voice);
               
            midi_MidiChannels[voice] = channel;
            
         }
         break;
         
      case FRONT_SEQ_MIDI_CHAN_OFF:
         {
            midi_MidiChannels[frontParser_midiMsg.data2]=0;
         }
         break;
   
      // --AS not used anymore
      //case FRONT_SEQ_MIDI_MODE:
      //	midi_mode = frontParser_midiMsg.data2;
      //	break;
      
      
      //voice nr (0xf0) + autom track nr (0x0f)
      case FRONT_SEQ_CLEAR_AUTOM:
         {
            const uint8_t voice 		= frontParser_midiMsg.data2 >> 4;
            const uint8_t automTrack 	= frontParser_midiMsg.data2 &  0x0f;
            seq_clearAutomation(voice, frontParser_shownPattern, automTrack);
         }
         break;
   
      case FRONT_SEQ_COPY_TRACK:
         {
            const uint8_t src = frontParser_midiMsg.data2>>4;
            const uint8_t dst = frontParser_midiMsg.data2&0xf;
            seq_copyTrack(src,dst,frontParser_shownPattern);
         }
         break;
   
      case FRONT_SEQ_COPY_PATTERN:
         {
            const uint8_t src = frontParser_midiMsg.data2>>4;
            const uint8_t dst = frontParser_midiMsg.data2&0xf;
            seq_copyPattern(src,dst);
         }
         break;
         
      case FRONT_SEQ_COPY_TRACK_PATTERN:
         {
            const uint8_t srcNr = frontParser_midiMsg.data2>>4;
            const uint8_t dstPat = frontParser_midiMsg.data2&0xf;
            seq_copyTrackPattern(srcNr,dstPat,frontParser_shownPattern);
         }
         break;
         
      case FRONT_SEQ_COPY_SRC:
         {
            frontParser_stepCopySource = frontParser_midiMsg.data2;
         }
         break;
      
      case FRONT_SEQ_COPY_DST:
         {
            seq_copySubStep(frontParser_stepCopySource,frontParser_midiMsg.data2,frontParser_activeTrack);
         }
         break;
   
      case FRONT_SEQ_TRACK_LENGTH:
         seq_setTrackLength(frontParser_activeTrack,frontParser_midiMsg.data2);
         break;
         
      case FRONT_SEQ_TRACK_SCALE:
         seq_setTrackScale(frontParser_activeTrack,frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_TRACK_ROTATION: //**PATROT handle incoming track rotation. apply to active track
         seq_setTrackRotation(frontParser_activeTrack,frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_SHUFFLE:
         seq_setShuffle(frontParser_midiMsg.data2/127.f);
         break;
   
      case FRONT_SEQ_SELECT_ACTIVE_STEP:
         seq_selectedStep = frontParser_midiMsg.data2;
         break;
   
      case FRONT_SEQ_SET_AUTOM_TRACK:
         seq_setActiveAutomationTrack(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_SET_QUANT:
         seq_setQuantisation(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_REQUEST_EUKLID_PARAMS:
         frontPanelSending_sendEuklidParamsReply(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_SET_SHOWN_PATTERN:
         if (frontParser_midiMsg.data2==frontParser_shownPattern)
         {
            if(!preset_consumeTmpBoundaryPatternSwitchAck())
               seq_realign();
         }
         else
         {
            (void)preset_consumeTmpBoundaryPatternSwitchAck();
            frontParser_shownPattern = seq_normalizePatternNumber(frontParser_midiMsg.data2);
         }
         break;   
      case FRONT_SEQ_SET_ACTIVE_TRACK:
         if ( (frontParser_activeTrack==frontParser_midiMsg.data2)&&(!seq_isRunning()) )
            seq_triggerVoice(frontParser_activeTrack, seq_rollVelocity, seq_rollNote);
            
         frontParser_activeTrack = frontParser_midiMsg.data2;
         frontPanelSending_sendActiveTrackReply(frontParser_activeTrack);
         
         break;
   
      case FRONT_SEQ_REQUEST_STEP_PARAMS:
         {
         /* send back probability, volume and note nr*/
            frontPanelSending_sendStepParamsReply(frontParser_shownPattern,
                                                 frontParser_activeTrack,
                                                 frontParser_midiMsg.data2);
         
         
         
            frontParser_activeStep = frontParser_midiMsg.data2;
         }
         break;
   
      case FRONT_SEQ_ROLL_RATE:
         seq_setRollRate(frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_ROLL_NOTE:
         seq_setRollNote(frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_ROLL_VELOCITY:
         seq_setRollVelocity(frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_ROLL_MODE:
         if(frontParser_midiMsg.data2==ROLL_MODE_FIRST_ON)
            seq_skipFirstRoll=0;
         else if(frontParser_midiMsg.data2==ROLL_MODE_FIRST_OFF)
            seq_skipFirstRoll=1;   
         else
            seq_rollMode = frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_TRANSPOSE:
         seq_transpose_voiceAmount[frontParser_activeTrack]=frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_TRANSPOSE_ON_OFF:
         if (frontParser_midiMsg.data2==0x0f)
            seq_writeTranspose();
         else if (frontParser_midiMsg.data2<=1)
            seq_transposeOnOff = frontParser_midiMsg.data2;
         break;
   
      case FRONT_SEQ_ROLL_ON_OFF:
         {
         
            const uint8_t onOff = frontParser_midiMsg.data2 >> 4;
            const uint8_t voice = frontParser_midiMsg.data2 & 0xf;
            seq_rollChange(voice,onOff);
         }
         break;
   
      case FRONT_SEQ_CHANGE_PAT:
         //switch to one of the 8 patterns on the next pattern start
         seq_setNextPattern( (frontParser_midiMsg.data2&0x07),(frontParser_midiMsg.data2>>3) );
         break;

      case FRONT_SEQ_CHANGE_TMP_PAT:
         seq_setNextPattern(SEQ_TMP_PATTERN, (frontParser_midiMsg.data2>>3));
         break;
   
   
      case FRONT_SEQ_RUN_STOP:
         seq_setRunning(frontParser_midiMsg.data2);
         break;
   
      case FRONT_SEQ_MUTE_TRACK:
         seq_setMute(frontParser_midiMsg.data2,1);
         break;
   
      case FRONT_SEQ_UNMUTE_TRACK:
         seq_setMute(frontParser_midiMsg.data2,0);
         break;
      case FRONT_SEQ_MIDI_ROUTING:
         midiParser_setRouting(frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_MIDI_TX_FILTER:
      case FRONT_SEQ_MIDI_RX_FILTER:
         midiParser_setFilter(frontParser_midiMsg.data1==FRONT_SEQ_MIDI_TX_FILTER, frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_BAR_RESET_MODE:
      // --AS a setting of 0 is default (keep track of bars in song), a setting of 1 is
      // to reset the bar counter when a manual pattern change occurs
         seq_resetBarOnPatternChange = frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_PC_TIME_MODE:
      // a setting of 0 is default (pattern changes at end of current bar
      // a setting of 1 causes pattern to change on next step
         switchOnNextStep = frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_TRIGGER_IN_PPQ:
         switch(frontParser_midiMsg.data2)
         {
            case 0:
               trigger_prescalerClockInput = PRE_1_PPQ;
               break;
         
            default:
            case 1:
               trigger_prescalerClockInput = PRE_4_PPQ;
               break;
         
            case 2:
               trigger_prescalerClockInput = PRE_8_PPQ;
               break;
            case 3:
               trigger_prescalerClockInput = PRE_16_PPQ;
               break;
            case 4:
               trigger_prescalerClockInput = PRE_32_PPQ;
               break;
         }
         break;
      case FRONT_SEQ_TRIGGER_OUT1_PPQ:
         switch(frontParser_midiMsg.data2)
         {
            case 0:
               trigger_dividerClockOut1 = PRE_1_PPQ;
               break;
         
            default:
            case 1:
               trigger_dividerClockOut1 = PRE_4_PPQ;
               break;
         
            case 2:
               trigger_dividerClockOut1 = PRE_8_PPQ;
               break;
            case 3:
               trigger_dividerClockOut1 = PRE_16_PPQ;
               break;
            case 4:
               trigger_dividerClockOut1 = PRE_32_PPQ;
               break;
         }
      
         break;
   
      case FRONT_SEQ_TRIGGER_OUT2_PPQ:
         switch(frontParser_midiMsg.data2)
         {
            case 0:
               trigger_dividerClockOut2 = PRE_1_PPQ;
               break;
         
            default:
            case 1:
               trigger_dividerClockOut2 = PRE_4_PPQ;
               break;
         
            case 2:
               trigger_dividerClockOut2 = PRE_8_PPQ;
               break;
            case 3:
               trigger_dividerClockOut2 = PRE_16_PPQ;
               break;
            case 4:
               trigger_dividerClockOut2 = PRE_32_PPQ;
               break;
               break;
         }
         break;
   
      case FRONT_SEQ_TRIGGER_GATE_MODE:
         trigger_setGatemode(frontParser_midiMsg.data2);
         break;
         
      case FRONT_SEQ_SET_LOOP:
         seq_setLoop(frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_LOAD_VOICE:
         frontParser_beginFileLoadIngress(0);
         seq_voicesLoading |= (0x01<<frontParser_midiMsg.data2);
         break;
      case FRONT_SEQ_UNHOLD_VOICE:
         frontParser_clearVoiceLoading(frontParser_midiMsg.data2);
         if(!frontParser_fileLoadBracketActive && !seq_voicesLoading)
            frontParser_endFileLoadIngress();
         break;
      case FRONT_SEQ_LOAD_FAST:
         seq_loadFastMode=frontParser_midiMsg.data2;
         break;
      case FRONT_SEQ_FILE_BEGIN:
         frontParser_beginFileLoadIngress(1);
         seq_resetVoiceMorphAmountsToGlobal();
         seq_resetLiveMorphApplyCache();
         if((frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE)
            || (frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_ALL))
         {
            preset_morphLoadDisabled = 1;
            preset_vMorphFlag = 0;
         }
         break;
      case FRONT_SEQ_FILE_DONE:
         if((frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE)
            || (frontParser_midiMsg.data2 == FRONT_FILE_DONE_TYPE_ALL))
         {
            preset_morphLoadDisabled = 0;
            preset_vMorphFlag = 0;
         }
         seq_voicesLoading=0;
         frontParser_endFileLoadIngress();
         break;
      case FRONT_SEQ_TRACK_NOTE1:
      case FRONT_SEQ_TRACK_NOTE2:
      case FRONT_SEQ_TRACK_NOTE3:
      case FRONT_SEQ_TRACK_NOTE4:
      case FRONT_SEQ_TRACK_NOTE5:
      case FRONT_SEQ_TRACK_NOTE6:
      case FRONT_SEQ_TRACK_NOTE7:
         midi_NoteOverride[frontParser_midiMsg.data1-FRONT_SEQ_TRACK_NOTE1] = frontParser_midiMsg.data2;
         break;
      default:
         break;
   }
}
