/*
 * frontPanelParser.c
 *
 * Created: 27.04.2012 12:04:00
 *  Author: Julian
 */ 
#include "frontPanelParser.h"
#include "Menu/menu.h"
#include "Menu/copyClearTools.h"
#include <stdio.h>

#include "ledHandler.h"
#include "IO/uart.h"
#include "buttonHandler.h"
#include "front.h"
//debug
#include <stdlib.h>
#include "Hardware/lcd.h"
#include "Hardware/timebase.h"
#include <util/atomic.h>
#include "Preset/PresetManager.h"
#include <util/delay.h>
//--

static uint8_t frontParser_rxCnt=0;
volatile MidiMsg frontParser_midiMsg;
static uint16_t frontParser_nrpnNr = 0;

volatile uint8_t frontParser_restoreActive = 0; // RESTORE: True during a canonical parameter dump from STM
static uint16_t frontParser_restoreCount = 0; // DEBUG

uint8_t frontPanel_sysexMode = 0;
uint8_t frontParser_sysexCallback = 0;
uint8_t frontParser_rxDisable=0;

volatile uint8_t frontParser_newSeqDataAvailable = 0;
volatile StepData frontParser_stepData;
//sysex buffer used to store 7 byte sysex chunks
volatile uint8_t frontParser_sysexBuffer[7];

uint8_t frontParser_nameIndex = 0;
uint8_t frontPanel_longOp;
uint8_t frontPanel_morphArray;
uint8_t frontPanel_morphAvail=0;
uint8_t frontPanel_wait = 0;

// case definitions for long ops that get dealt with once per main() loop
// bitwise definitions to decide what to do for multiple ops
#define BANK_1 0x01
#define BANK_2 0x02
#define BANK_3 0x04
#define BANK_4 0x08
#define BANK_5 0x10
#define BANK_6 0x20
#define BANK_7 0x40
#define BANK_GLOBAL 0x80
// banks 1-6 plus global stack to allow for multiple voices stacked on the same
// MIDI channel to respond to the same bank change command

// above BANK_GLOBAL it doesn't matter - we reset the command anyway
#define MORPH_OP 0x81
// there is space in here to add more long operations - pattern change
// must have the highest priority
#define PATTERN_CHANGE_OP 0xAF
#define NULL_OP 0x00
uint8_t frontPanel_longData;

#define FLOW_WAIT_TICKS 384

static uint8_t comm_flowActive = 0;
static uint8_t comm_flowChannel = 0;
static uint8_t comm_txCredits = 0;
static uint8_t comm_flowAckPending = 0;
static uint8_t comm_flowAckChannel = 0;
static uint8_t comm_flowFailed = 0;
static uint8_t comm_prfCacheStatusCommand = 0;
static uint8_t comm_prfCacheStatusValue = PRF_CACHE_REJECTED;

static uint8_t frontPanel_flowIsCommand(uint8_t command)
{
   return (command >= SEQ_FLOW_BEGIN) && (command <= SEQ_FLOW_ABORT);
}

static uint8_t frontPanel_flowUsesCredit(uint8_t status, uint8_t data1)
{
   return !(status == SEQ_CC && frontPanel_flowIsCommand(data1));
}

static uint16_t frontPanel_flowNow()
{
   uint16_t now;

   ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
   {
      now = time_sysTick;
   }

   return now;
}

static void frontPanel_handleFlowMessage()
{
   uint8_t channel = frontParser_midiMsg.data2;
   uint8_t credits = 0;

   switch(frontParser_midiMsg.data1)
   {
      case SEQ_FLOW_GRANT:
         channel = (uint8_t)((frontParser_midiMsg.data2 >> 4) & 0x07);
         credits = (uint8_t)(frontParser_midiMsg.data2 & 0x0f);
         if(credits == 0)
            break;

         if(comm_flowAckPending && (channel == comm_flowAckChannel))
            comm_flowAckPending = 0;

         if(comm_flowActive && (channel == comm_flowChannel))
         {
            comm_txCredits = credits;
            comm_flowFailed = 0;
         }
         break;

      case SEQ_FLOW_ABORT:
         comm_flowActive = 0;
         comm_txCredits = 0;
         comm_flowFailed = 1;
         if(channel == comm_flowAckChannel)
            comm_flowAckPending = 0;
         break;

      default:
         break;
   }
}

static uint8_t frontPanel_waitForFlowAck(uint8_t channel)
{
   uint16_t start = frontPanel_flowNow();

   while(comm_flowAckPending)
   {
      uart_checkAndParse();
      if((uint16_t)(frontPanel_flowNow() - start) > FLOW_WAIT_TICKS)
      {
         frontPanel_sendData(SEQ_CC, SEQ_FLOW_ABORT, channel);
         comm_flowAckPending = 0;
         comm_flowActive = 0;
         comm_txCredits = 0;
         comm_flowFailed = 1;
         return 0;
      }
   }

   return !comm_flowFailed;
}

static uint8_t frontPanel_flowSendAndWait(uint8_t command, uint8_t channel)
{
   comm_flowAckChannel = channel;
   comm_flowAckPending = 1;
   frontPanel_sendData(SEQ_CC, command, channel);

   return frontPanel_waitForFlowAck(channel);
}

static uint8_t frontPanel_waitForCredit()
{
   uint16_t start = frontPanel_flowNow();

   while(comm_flowActive && (comm_txCredits == 0))
   {
      uart_checkAndParse();
      if((uint16_t)(frontPanel_flowNow() - start) > FLOW_WAIT_TICKS)
      {
         frontPanel_sendData(SEQ_CC, SEQ_FLOW_ABORT, comm_flowChannel);
         comm_flowActive = 0;
         comm_txCredits = 0;
         comm_flowFailed = 1;
         return 0;
      }
   }

   return !comm_flowFailed;
}



//------------------------------------------------------------
#define NRPN_MUTE_1 1000
#define NRPN_MUTE_7 1006
//------------------------------------------------------------
void frontPanel_sendMacro(uint8_t whichMacro,uint8_t value)
{
// this function only sends top level macro values - 'amount' values are handled as normal cc's
// 'destination' values are handled in the menu code for DTYPE_AUTOM_TARGET

/* MACRO_CC message structure
byte1 - status byte 0xaa as above
byte2, data1 byte: xttaaa-b : tt= top level macro value sent (2 macros exist now, we can do 2 more if we want)
                              aaa= macro destination value sent (4 destinations exist now, can do 8)
                              b=macro mod target value top bit
                              I have left a blank bit above this to make it easier to make more than 255 kit parameters
                              if we ever want to take on that can of worms
                              
byte3, data2 byte: xbbbbbbb : b=macro mod target value lower 7 bits or top level value full
*/
   uint8_t data1;
   if (whichMacro==1){
      data1=0x20;
      frontPanel_sendData(MACRO_CC, data1, value);
   }
   else if (whichMacro==2){
      data1=0x40;
      frontPanel_sendData(MACRO_CC, data1, value);
   }
}
//------------------------------------------------------------
void frontPanel_updatePatternLeds()
{
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   frontPanel_sendData(LED_CC, LED_QUERY_SEQ_TRACK, value);
}
//------------------------------------------------------------
void frontPanel_updateActiveStepLeds()
{
   /*
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   frontPanel_sendData(LED_CC, LED_QUERY_SEQ_TRACK, value);
   */
}
//------------------------------------------------------------
void frontPanel_updateSubstepLeds()
{
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   frontPanel_sendData(LED_CC, LED_ALL_SUBSTEP, value);
}
//------------------------------------------------------------
void frontPanel_holdForBuffer()
{
  // start and end sysex to make sure mainboard is caught up on messages and can respond
// frontParser_midiMsg.status = 0;
frontPanel_wait=1;
frontPanel_sendByte(CALLBACK_ACK);
while(frontPanel_wait)
{
    uart_checkAndParse();
}
}
//------------------------------------------------------------
uint8_t frontPanel_flowBeginSession()
{
   return frontPanel_flowBegin(FLOW_CH_LOAD_SESSION);
}
//------------------------------------------------------------
uint8_t frontPanel_flowEndSession()
{
   return frontPanel_flowEnd(FLOW_CH_LOAD_SESSION);
}
//------------------------------------------------------------
uint8_t frontPanel_flowBegin(uint8_t channel)
{
   comm_flowActive = (channel != FLOW_CH_LOAD_SESSION);
   comm_flowChannel = channel;
   comm_txCredits = 0;
   comm_flowFailed = 0;

   if(!frontPanel_flowSendAndWait(SEQ_FLOW_BEGIN, channel))
      return 0;

   if(channel != FLOW_CH_LOAD_SESSION)
      comm_flowActive = 1;

   return 1;
}
//------------------------------------------------------------
uint8_t frontPanel_flowEnd(uint8_t channel)
{
   uint8_t ret = frontPanel_flowSendAndWait(SEQ_FLOW_END, channel);

   if(channel == comm_flowChannel)
   {
      comm_flowActive = 0;
      comm_txCredits = 0;
   }

   return ret && !comm_flowFailed;
}
//------------------------------------------------------------
uint8_t frontPanel_flowFailed()
{
   return comm_flowFailed;
}
//------------------------------------------------------------
void frontPanel_flowAbortSession()
{
   frontPanel_sendData(SEQ_CC, SEQ_FLOW_ABORT, FLOW_CH_LOAD_SESSION);
   comm_flowAckPending = 0;
   comm_flowActive = 0;
   comm_txCredits = 0;
   comm_flowFailed = 1;
}
//------------------------------------------------------------
uint8_t frontPanel_prfCacheBegin(uint8_t fileType)
{
   comm_prfCacheStatusCommand = 0;
   comm_prfCacheStatusValue = PRF_CACHE_REJECTED;

   if(!frontPanel_prfCacheControl(SEQ_PRF_CACHE_BEGIN, fileType))
      return PRF_CACHE_REJECTED;

   if(comm_prfCacheStatusCommand != SEQ_PRF_CACHE_BEGIN)
      return PRF_CACHE_REJECTED;

   return comm_prfCacheStatusValue;
}
//------------------------------------------------------------
uint8_t frontPanel_prfCacheControl(uint8_t command, uint8_t fileType)
{
   comm_flowAckChannel = FLOW_CH_LOAD_SESSION;
   comm_flowAckPending = 1;
   frontPanel_sendData(SEQ_CC, command, fileType);

   return frontPanel_waitForFlowAck(FLOW_CH_LOAD_SESSION);
}
//------------------------------------------------------------
void frontParser_parseNrpn(uint8_t value)
{
   uint16_t paramNr=frontParser_nrpnNr+128;

   if(paramNr < NUM_PARAMS)
      parameter_values[paramNr] = value;
	
   if( (paramNr >= PAR_TARGET_LFO1) && (paramNr <= PAR_TARGET_LFO6) )
   {
   	//**LFO receive nrpn translate --AS TODO this needs to be checked. I'm not sure what needs to happen here.
   	// It seems like the assumption is that in this case, value represents an encoded menupage value (this code
   	// used to call the now defunct getModTargetValue)
   
   	//LFO
      uint8_t lfoNr = (uint8_t)(paramNr-PAR_TARGET_LFO1);
      if(lfoNr>5)lfoNr=5;
   
   	// value (might) represents an actual parameter number, we need to convert to index into modTargets
      parameter_values[paramNr]=paramToModTarget[value];
   	// this was the old code
   	//since the LFO target calculation from the index number needs to know about the menu structure (menuPages)
   	//we need to send back the right target param number to the cortex
   	//value = getModTargetValue(parameter_values[frontParser_nrpnNr+128],
   	//		(uint8_t)(parameter_values[PAR_VOICE_LFO1+lfoNr]-1));
   
      uint8_t upper = (uint8_t)(((value&0x80)>>7) | (((lfoNr)&0x3f)<<1));
      uint8_t lower = value&0x7f;
      frontPanel_sendData(CC_LFO_TARGET,upper,lower);
   }
   else if ( (paramNr >= PAR_VEL_DEST_1) && (paramNr <= PAR_VEL_DEST_6) )
   {
   	//**VELO receive nrpn translate to parameter. --AS TODO this needs to be checked as well
   
   	// value (might) represents an actual parameter number, we need to convert to index into modTargets
      parameter_values[paramNr]=paramToModTarget[value];
   
   	// old code
   	//uint8_t param = parameter_values[frontParser_nrpnNr+128];
   	//if(param > (NUM_SUB_PAGES * 8 -1))
   	//param = (NUM_SUB_PAGES * 8 -1);
   	//uint8_t value = getModTargetValue(param, (uint8_t)(frontParser_nrpnNr+128 - PAR_VEL_DEST_1));
   			
      uint8_t upper,lower;
      upper = (uint8_t)((uint16_t)((value&0x80)>>7) | (((paramNr-PAR_VEL_DEST_1)&0x3f)<<1));
      lower = value&0x7f;
      frontPanel_sendData(CC_VELO_TARGET,upper,lower);
   
   } 
   else if ( (frontParser_nrpnNr >= NRPN_MUTE_1) && (frontParser_nrpnNr <= NRPN_MUTE_7) )
   {
      const uint8_t voice = (uint8_t)(frontParser_nrpnNr - NRPN_MUTE_1);
      const uint8_t onOff = value;
      buttonHandler_muteVoice(voice,onOff);
   	
   }
}
//------------------------------------------------------------
void frontPanel_ccHandler()
{
	//get the real parameter number from the cc number
   const uint8_t parNr =(uint8_t)( frontParser_midiMsg.data1 - 1);
	
   if(parNr == NRPN_DATA_ENTRY_COARSE) {
      frontParser_parseNrpn(frontParser_midiMsg.data2);
   }
	
   if(parNr == NRPN_FINE) {
      frontParser_nrpnNr &= 0x80; //clear lower 7 bit
      frontParser_nrpnNr |= frontParser_midiMsg.data2;
   }
		
   if(parNr == NRPN_COARSE) {
      frontParser_nrpnNr &= 0x7f; //clear upper 7 bit
      frontParser_nrpnNr |= (uint16_t)(frontParser_midiMsg.data2<<7);
   }
	
	
	//set the parameter value
   parameter_values[parNr] = frontParser_midiMsg.data2;
	
	//repaint the LCD
   menu_repaint();
	
}
//------------------------------------------------------------
void frontPanel_parseData(uint8_t data)
{
   uint8_t i;
	// if high byte set a new message starts
   if(data&0x80)
   {	
      if(data==PATCH_RESET)
      {
         for(i=0;i<END_OF_MORPH_PARAMETERS;i++)
         {
            parameter_values[i]=parameter_values_temp[i];
         }
         menu_repaintAll();
      }
      else if(data==CALLBACK_ACK)
      {
         frontPanel_wait = 0;  
      }
      else
      {
      	//reset the byte counter
         frontParser_rxCnt = 0;
         frontParser_midiMsg.status = data;
      }
   
   }
   else
   {
      if(frontParser_midiMsg.status == SYSEX_START)
      {
       
         switch(frontPanel_sysexMode)
         {
            case SYSEX_BEGIN_PATTERN_TRANSMIT:
               {
                  if(data==SYSEX_BEGIN_PATTERN_TRANSMIT)
                  {
                     frontParser_sysexCallback=STEP_CALLBACK;
                  }
                  else if(data==SYSEX_STEP_ACK)
                  {
                     frontParser_sysexCallback=STEP_ACK;
                  }
               }
               break;
            case SYSEX_SEND_MAIN_STEP_DATA:
               {
                  if(data==SYSEX_SEND_MAIN_STEP_DATA)
                  
                  {
                     frontParser_sysexCallback=MAINSTEP_CALLBACK;
                  }
               }
               break;
            case SYSEX_SEND_PAT_CHAIN_DATA:
               {
                  if(data==SYSEX_SEND_PAT_CHAIN_DATA)
                  
                  {
                     frontParser_sysexCallback=PATCHAIN_CALLBACK;
                  }
               }
               break;
         
            case SYSEX_SEND_PAT_LEN_DATA:
               {
                  if(data==SYSEX_SEND_PAT_LEN_DATA)
                  
                  {
                     frontParser_sysexCallback=LENGTH_CALLBACK;
                  }
               }
               break;
            case SYSEX_SEND_PAT_SCALE_DATA:
               {
                  if(data==SYSEX_SEND_PAT_SCALE_DATA)
                  {
                     frontParser_sysexCallback=SCALE_CALLBACK;
                  }
               }
               break;
            case SYSEX_REQUEST_STEP_DATA:
               {
               //we are expecting step packages send from the sequencer
               //1st 7 lower nibble 7 bit messages
               //then an upper nibble 7 bit message containing the missing 7 upper bits
               //char text[5];
                  if(frontParser_rxCnt<7)
                  {
                     frontParser_sysexBuffer[frontParser_rxCnt++] = data;
                  //	lcd_setcursor(5,2);
                  //	itoa(frontParser_rxCnt,text,10);
                  //	lcd_string(text);
                  
                  }
                  else
                  {
                  //now we have to distribute the MSBs to the sysex data
                     for(int i=0;i<7;i++)
                     {
                     
                        frontParser_sysexBuffer[i] |= (uint8_t)((data&(1<<i))<<(7-i));
                     
                     
                     }
                     frontParser_stepData.volume = frontParser_sysexBuffer[0];
                     frontParser_stepData.prob = frontParser_sysexBuffer[1];
                     frontParser_stepData.note = frontParser_sysexBuffer[2];
                     frontParser_stepData.param1Nr = frontParser_sysexBuffer[3];
                     frontParser_stepData.param1Val = frontParser_sysexBuffer[4];
                     frontParser_stepData.param2Nr = frontParser_sysexBuffer[5];
                     frontParser_stepData.param2Val = frontParser_sysexBuffer[6];
                  
                  //signal that a ne data chunk is available
                     frontParser_newSeqDataAvailable = 1;
                  //reset receive counter for next chunk
                     frontParser_rxCnt = 0;
                  
                  //lcd_setcursor(5,2);
                  //lcd_string("complete");
                  }					
               
               }
               break;
            case SYSEX_REQUEST_PATTERN_DATA:
               {
                  if(frontParser_rxCnt<1)
                  {
                  //1st byte
                     frontParser_sysexBuffer[frontParser_rxCnt++] = data;
                  } 
                  else {
                  //2nd byte
                     frontParser_sysexBuffer[frontParser_rxCnt++] = data;
                  
                     uint8_t next = frontParser_sysexBuffer[0];
                     uint8_t repeat = frontParser_sysexBuffer[1];
                  //we abuse the stepData struct to store the pattern data
                     frontParser_stepData.volume = next;
                     frontParser_stepData.prob = repeat;
                  
                  //signal that a new data chunk is available
                     frontParser_newSeqDataAvailable = 1;
                  //reset receive counter for next chunk
                     frontParser_rxCnt = 0;
                  }
               
               }
               break;
            case SYSEX_REQUEST_MAIN_STEP_DATA:
               {
                  if(frontParser_rxCnt<4)
                  {
                  //1st 2 nibbles + last 2 bit
                     frontParser_sysexBuffer[frontParser_rxCnt++] = data;             
                  } 
                  else {
                  // scale information
                     frontParser_sysexBuffer[frontParser_rxCnt++] = data;               
                  
                  // package the main step data so we know what to do with
                  
                     uint16_t mainStepData = frontParser_sysexBuffer[0] |
                     (uint16_t)(frontParser_sysexBuffer[1]<<7) |
                     (uint16_t)(frontParser_sysexBuffer[2]<<14);
                  //we abuse the stepData struct to store the main step data and the scale
                     frontParser_stepData.volume = (uint8_t)(mainStepData>>8);
                     frontParser_stepData.prob = (uint8_t)(mainStepData&0xff);
                     frontParser_stepData.note = frontParser_sysexBuffer[3]; // this is the length data from uart
                     frontParser_stepData.param1Nr = frontParser_sysexBuffer[4]; // this is the scale data from uart
                  
                  
                  //signal that a new data chunk is available
                     frontParser_newSeqDataAvailable = 1;
                  //reset receive counter for next chunk
                     frontParser_rxCnt = 0;
                  }
               }
               break;  
            default:
               break;
         }
      }
      else if(frontParser_rxCnt==0)
      {
         if(!frontParser_rxDisable
            || (frontParser_midiMsg.status == SEQ_CC)
            || (frontParser_midiMsg.status == PRF_CACHE_STATUS)
            || (frontParser_midiMsg.status == PARAM_RESTORE_BEGIN)
            || (frontParser_midiMsg.status == PARAM_RESTORE_DONE)
            || (frontParser_midiMsg.status == PRF_RESTORE_PARAM_CC)
            || (frontParser_midiMsg.status == PRF_RESTORE_PARAM_CC2)
            || (frontParser_midiMsg.status == PRF_RESTORE_MORPH_CC)
            || (frontParser_midiMsg.status == PRF_RESTORE_MORPH_CC2))
         {
      	   //parameter nr
            frontParser_midiMsg.data1 = data;
            frontParser_rxCnt++;
         }
      }
      else
      {
      	//parameter value
         frontParser_midiMsg.data2 = data;
         frontParser_rxCnt=0;

         if(frontParser_rxDisable)
         {
            if((frontParser_midiMsg.status == SEQ_CC) && frontPanel_flowIsCommand(frontParser_midiMsg.data1))
               frontPanel_handleFlowMessage();
            else if(frontParser_midiMsg.status == PRF_CACHE_STATUS)
            {
               comm_prfCacheStatusCommand = frontParser_midiMsg.data1;
               comm_prfCacheStatusValue = frontParser_midiMsg.data2;
            }
            else if((frontParser_midiMsg.status == PARAM_RESTORE_BEGIN)
               || (frontParser_midiMsg.status == PARAM_RESTORE_DONE)
               || (frontParser_midiMsg.status == PRF_RESTORE_PARAM_CC)
               || (frontParser_midiMsg.status == PRF_RESTORE_PARAM_CC2)
               || (frontParser_midiMsg.status == PRF_RESTORE_MORPH_CC)
               || (frontParser_midiMsg.status == PRF_RESTORE_MORPH_CC2))
            {
               /* RESTORE: Allow these messages to fall through to the processor even when rxDisable is true. */
            }
            else
            {
               return;
            }
         }
      	//process the received data
         if(frontParser_midiMsg.status == MIDI_CC) //sound cc data from cortex 
         {
            frontPanel_ccHandler();
         }
         else
         {
            if(frontParser_midiMsg.status == PRESET_NAME)
            {
            	
               if(frontParser_midiMsg.data2 & 0x40)
               {
                  frontParser_nameIndex = 0;
               }
            	
               preset_currentName[frontParser_nameIndex] =(char)(
                  	(frontParser_midiMsg.data1&0x7f) |
                  	((frontParser_midiMsg.data2&0x7f)<<7));
               frontParser_nameIndex++;
               frontParser_nameIndex &= 0x7; //wrap at 8
               if(frontParser_nameIndex==0)
               {
                  menu_repaintAll();
               }						
            					
            } 
            else if(frontParser_midiMsg.status == SET_P1_DEST)
            {
            	//**AUTOM - translate cortex value to mod target index
            	// a value of FF means no automation (on the back end)
               uint8_t dst=(uint8_t)((frontParser_midiMsg.data1<<7) | frontParser_midiMsg.data2);
               if(dst==0xFF)
                  dst=0;
               parameter_values[PAR_P1_DEST] = paramToModTarget[dst];
               menu_repaintAll();
            }
            else if(frontParser_midiMsg.status == SET_P2_DEST)
            {
            	//**AUTOM - translate cortex value to mod target index
               uint8_t dst=(uint8_t)((frontParser_midiMsg.data1<<7) | frontParser_midiMsg.data2);
               if(dst==0xFF)
                  dst=0;
               parameter_values[PAR_P2_DEST] = paramToModTarget[dst];
               menu_repaintAll();
            }
            else if(frontParser_midiMsg.status == SET_P1_VAL)
            {
               parameter_values[PAR_P1_VAL] = (uint8_t)((frontParser_midiMsg.data1<<7) | frontParser_midiMsg.data2);
               menu_repaintAll();
            }
            else if(frontParser_midiMsg.status == SET_P2_VAL)
            {
               parameter_values[PAR_P2_VAL] = (uint8_t)((frontParser_midiMsg.data1<<7) | frontParser_midiMsg.data2);
               menu_repaintAll();
            }
            else if(frontParser_midiMsg.status == SEQ_CC)
            {
               switch(frontParser_midiMsg.data1)
               {
               	
                  case SEQ_FLOW_BEGIN:
                  case SEQ_FLOW_GRANT:
                  case SEQ_FLOW_END:
                  case SEQ_FLOW_ABORT:
                     frontPanel_handleFlowMessage();
                     break;

                  case SEQ_SET_PAT_BEAT:
                     parameter_values[PAR_PATTERN_BEAT] = frontParser_midiMsg.data2;
                     menu_repaint();
                     break;	
                  case SEQ_SET_PAT_NEXT:
                     parameter_values[PAR_PATTERN_NEXT] = frontParser_midiMsg.data2;
                     menu_repaint();
                     break;
               	
                  case SEQ_TRACK_LENGTH:
                     parameter_values[PAR_TRACK_LENGTH] = frontParser_midiMsg.data2;
                     menu_repaint();
                     break;
                  case SEQ_TRACK_SCALE:
                     parameter_values[PAR_TRACK_SCALE] = frontParser_midiMsg.data2;
                     menu_repaint();
                     break;
               
               	// **PATROT - receive rotation value from back for active track
                 
                  case SEQ_TRACK_ROTATION:
                     parameter_values[PAR_TRACK_ROTATION] = frontParser_midiMsg.data2;
                     menu_repaint();
                     if ((buttonHandler_getMode() == SELECT_MODE_PERF)&&shiftState)
                     {  // rotation amount updated while viewing rotation - update the display
                        led_clearAllBlinkLeds();
                        led_setBlinkLed((uint8_t) (LED_STEP1 + parameter_values[PAR_TRACK_ROTATION]), 1);
                        led_setBlinkLed((uint8_t) (LED_VOICE1 + menu_getActiveVoice()), 1);
                        uint8_t viewedPattern = menu_getViewedPattern();
                        led_setBlinkLed((uint8_t)((viewedPattern == SEQ_TMP_PATTERN) ? LED_STEP16 : (LED_PART_SELECT1 + viewedPattern)), 1);
                        
                     }
                     break;
                     
                  case SEQ_TRANSPOSE:
                     parameter_values[PAR_TRANSPOSE] = frontParser_midiMsg.data2;
                     menu_repaint();
                     break;
               	
                  case SEQ_EUKLID_LENGTH:
                     parameter_values[PAR_EUKLID_LENGTH] = frontParser_midiMsg.data2;
                     menu_repaint();
                     break;
               	
                  case SEQ_EUKLID_STEPS:
                     parameter_values[PAR_EUKLID_STEPS] = frontParser_midiMsg.data2;
                     menu_repaint();
                     break;
               	
                  case SEQ_EUKLID_ROTATION:
                     parameter_values[PAR_EUKLID_ROTATION] = frontParser_midiMsg.data2;
                     menu_repaint();
                     break;
                  
                  case SEQ_EUKLID_SUBSTEP_ROTATION:
                     parameter_values[PAR_EUKLID_SUBSTEP_ROTATION] = frontParser_midiMsg.data2;
                     menu_repaint();
                     break;
               
                  case SEQ_VOLUME:
                     parameter_values[PAR_STEP_VOLUME] = frontParser_midiMsg.data2;
                     menu_repaintAll();
                     break;
               	
                  case SEQ_PROB:
                     parameter_values[PAR_STEP_PROB] = frontParser_midiMsg.data2;
                     menu_repaintAll();
                     break;
               	
                  case SEQ_NOTE:
                     parameter_values[PAR_STEP_NOTE] = frontParser_midiMsg.data2;
                     menu_repaintAll();
                     break;
               	
               	/*
               	case SEQ_GET_ACTIVE_PAT:
               	//only in mute mode relevant
               	//led_clearSequencerLeds9_16();
               	led_clearSelectLeds();
               	// set led to show active pattern
               	led_setValue(1,LED_PART_SELECT1+frontParser_midiMsg.data2);
               	break;
               	*/
               	
                  case SEQ_CHANGE_PAT:
                     if(frontParser_midiMsg.data2 > 15) 
                        return;
                 	//ack message that the sequencer changed to the requested pattern
                     uint8_t patMsg = frontParser_midiMsg.data2;
                     if(patMsg != SEQ_TMP_PATTERN)
                        patMsg &= 0x07;
                  	// if a 'perf' or 'all' load locked the kit, un-lock and load
                     if(preset_workingVoiceArray)
                     {
                        for (i=0;i<(NUM_TRACKS-1);i++)
                     {
                      if(preset_workingVoiceArray&(0x01<<i))
                        {  
                           preset_readDrumVoice(i, 0);
                           preset_readDrumVoice(i, 1);
                           preset_workingVoiceArray = (uint8_t)(preset_workingVoiceArray&(~(0x01<<i)));
                        }
                       
                     }
                     preset_workingVoiceArray = 0;
                     }
                     led_setBlinkLed((uint8_t)((patMsg == SEQ_TMP_PATTERN) ? LED_STEP16 : (LED_PART_SELECT1+patMsg)),0);
                  	//clear last pattern led
                  	
                     if( (parameter_values[PAR_FOLLOW]) ) {
                     	
                        if( menu_activePage != PATTERN_SETTINGS_PAGE)
                        {
                           menu_setShownPattern(patMsg);
                           led_clearSequencerLeds();
                        	//query current sequencer step states and light up the corresponding leds 
                           frontPanel_updatePatternLeds();
                           frontPanel_sendData(SEQ_CC,SEQ_REQUEST_PATTERN_PARAMS,patMsg);
                        } 
                        else {
                        	//store the pending pattern update for shift button release handler
                           menu_shownPattern = frontParser_midiMsg.data2;
                        }								
                     }	
                     menu_playedPattern = patMsg;						
                  	
                     if( (buttonHandler_getMode() == SELECT_MODE_PERF) || (buttonHandler_getMode() == SELECT_MODE_PAT_GEN) )
                     {
                     	//only show pattern changes when in performance mode
                     		
                     	//led_clearSequencerLeds9_16();
                        led_clearSelectLeds();
                        led_clearAllBlinkLeds();
                     	// re init the LEDs shwoing active/viewed pattern
                        led_initPerformanceLeds();
                     	//led_setValue(1,LED_PART_SELECT1+frontParser_midiMsg.data2);
                     }
                     
                     break;
                  case SEQ_RUN_STOP:
                 	// --AS This tells the front that the sequencer has started/stopped due to MTC msg
                  	// we simply use this to turn on/off the led and cause the next press of start
                 	// button to act properly
                     buttonHandler_setRunStopState(frontParser_midiMsg.data2);
                     break;
               	
                  case LED_QUERY_SEQ_TRACK:
                  //this message is only send by the frontpanel, so it doesnt need to handle it
                     break;
               
               
               };						
            }
            else if(frontParser_midiMsg.status == SAMPLE_CC)
            {
               switch(frontParser_midiMsg.data1)
               {
                  case SAMPLE_COUNT:
                     menu_setNumSamples(frontParser_midiMsg.data2);
                     break;
               
                  default:
                     break;
               }
            }
            
            /* -bc- additions to front interpreter start here*/
            //-------------------------------------------------
            /* RESTORE PUSH-UP PROCESS (AVR Side)
               This mechanism handles the incoming parameter dump from STM32.

               Handshake Logic:
               1. AVR receives PARAM_RESTORE_BEGIN.
                  - Sets frontParser_restoreActive = 1.
                  - Sends PARAM_RESTORE_READY back to STM.
               2. While frontParser_restoreActive is 1, frontPanel_sendData() suppresses all 
                  outbound parameter traffic to prevent feedback loops (where restored display 
                  values are misinterpreted as user edits and sent back to STM).
               3. AVR receives PRF_RESTORE_PARAM_CC/CC2 messages and updates parameter_values[].
                  - Crucially, it does NOT update parameters2[] (morph endpoint) to avoid 
                    corrupting the morph state during a display-only synchronization.
               4. AVR receives PARAM_RESTORE_DONE.
                  - Calls menu_repaintAll() to update the display.
                  - Sends PARAM_RESTORE_ACK back to STM.
                  - Clears frontParser_restoreActive = 0.
            */
            else if(frontParser_midiMsg.status == PARAM_CC)
            {
               parameter_values[frontParser_midiMsg.data1]=frontParser_midiMsg.data2;
               parameters2[frontParser_midiMsg.data1]=frontParser_midiMsg.data2;
               menu_repaint();
            
            }
            
            else if(frontParser_midiMsg.status == PARAM_CC2)
            {
               parameter_values[frontParser_midiMsg.data1+128]=frontParser_midiMsg.data2;
               parameters2[frontParser_midiMsg.data1+128]=frontParser_midiMsg.data2;
               menu_repaint();
            
            }
            else if(frontParser_midiMsg.status == PARAM_RESTORE_BEGIN)
            {
               // RESTORE: Start of a canonical parameter dump from STM.
               lcd_setcursor(0,0);
               lcd_string_F(PSTR("RESTORE BEGIN   "));
               frontParser_restoreActive = 1;
               frontParser_restoreCount = 0;
               // Inform STM we are ready to receive and have suppressed outbound traffic.
               frontPanel_sendData(PARAM_RESTORE_READY, 0, 0);
            }
            else if(frontParser_midiMsg.status == PRF_RESTORE_PARAM_CC)
            {
               // RESTORE: Update main parameters only. Do not touch parameters2 (morph endpoint)
               // to avoid corrupting morph state during a display-only synchronization.
               parameter_values[frontParser_midiMsg.data1]=frontParser_midiMsg.data2;
               frontParser_restoreCount++;
            }
            else if(frontParser_midiMsg.status == PRF_RESTORE_PARAM_CC2)
            {
               uint16_t paramNr = (uint16_t)(frontParser_midiMsg.data1+128);
               if(paramNr < NUM_PARAMS)
               {
                  parameter_values[paramNr]=frontParser_midiMsg.data2;
                  frontParser_restoreCount++;
                  // RESTORE: Parameters2 mirroring explicitly removed for restore dumps.
               }
            }
            else if(frontParser_midiMsg.status == PRF_RESTORE_MORPH_CC)
            {
               // RESTORE: Morph-specific restore messages still update parameters2.
               if(frontParser_midiMsg.data1 < END_OF_SOUND_PARAMETERS)
               {
                  parameters2[frontParser_midiMsg.data1]=frontParser_midiMsg.data2;
                  frontParser_restoreCount++;
               }
            }
            else if(frontParser_midiMsg.status == PRF_RESTORE_MORPH_CC2)
            {
               uint16_t paramNr = (uint16_t)(frontParser_midiMsg.data1+128);
               if(paramNr < END_OF_SOUND_PARAMETERS)
               {
                  parameters2[paramNr]=frontParser_midiMsg.data2;
                  frontParser_restoreCount++;
               }
            }
            else if(frontParser_midiMsg.status == PARAM_RESTORE_DONE)
            {
               // RESTORE: End of dump. Repaint the full menu to reflect new values.
               char text[17];
               sprintf(text, "DONE %d      ", frontParser_restoreCount);
               lcd_setcursor(0,0);
               lcd_string(text);

               menu_repaintAll();
               // Inform STM we have finished and re-enabled normal operation.
               frontPanel_sendData(PARAM_RESTORE_ACK, 0, 0);
               frontParser_restoreActive = 0;
            }
            else if(frontParser_midiMsg.status == PRF_CACHE_STATUS)
            {
               comm_prfCacheStatusCommand = frontParser_midiMsg.data1;
               comm_prfCacheStatusValue = frontParser_midiMsg.data2;
            }
            
            else if(frontParser_midiMsg.status == BANK_CHANGE_CC)
            {
               if (frontParser_midiMsg.data1&&(frontPanel_longOp<PATTERN_CHANGE_OP) )
               {
                  // we have a valid message and we're not waiting for a pattern change to finish
                  // all bits in data1 are set - global bank change operation
                  if ( frontParser_midiMsg.data1==0x7f ) 
                  // global bank change request or, all voice channels set the same
                  {
                     // this is a time-consuming operation, cache it and deal
                     // with only one per loop of main()
                     frontPanel_longOp=BANK_GLOBAL;
                     frontPanel_longData=frontParser_midiMsg.data2;
                     
                  
                  }
                  // individual voice bank-change
                  
                  else if (frontPanel_longOp!=BANK_GLOBAL)
                  // don't override global bank changes
                  {
                     // stack the operations so multiple voice bank changes can take place
                     
                     frontPanel_longOp=frontParser_midiMsg.data1;
                     frontPanel_longData=frontParser_midiMsg.data2;
                  
                  }
               }
            }
            
            
            // morph operation
            else if( (frontParser_midiMsg.status == MORPH_CC)||(frontParser_midiMsg.status == VOICE_MORPH) )
            {  
               // morph is a low-priority opertaion (because we might be getting a LOT
               // from the Mod Wheel) - if we get a bank change or program change, 
               // we cache Morph while those happen
               
               if(frontPanel_longOp!=NULL_OP)
               {
                  frontPanel_morphArray = frontParser_midiMsg.data1;
                  frontPanel_longData=(uint8_t)(frontParser_midiMsg.data2<<1);
                  frontPanel_morphAvail=1;
               }
               else
               {
                  // this is a time-consuming operation, cache it and deal
                  // with only one per loop of main()
                  frontPanel_longOp=MORPH_OP;
                  
                  frontPanel_morphArray = frontParser_midiMsg.data1;
                  // bit shift from MIDI CC values
                  frontPanel_longData=(uint8_t)(frontParser_midiMsg.data2<<1);
               }
            }
            
            
            //-------------------------------------------------
            /* -bc- additions to front interpreter end here*/
            
            
            
            else if(frontParser_midiMsg.status == LED_CC)
            {
               uint8_t offset=0;
               switch(frontParser_midiMsg.data1)
               {
                  case LED_CURRENT_STEP_NR: 
                     {
                     
                        if(frontParser_midiMsg.data2 >=128) 
                           return;
                     
                        uint8_t shownPattern = menu_getViewedPattern();
                        uint8_t playedPattern = menu_playedPattern;
                     
                        if(shownPattern == playedPattern) {
                        //only update chaselight LED when it step edit mode
                           if( (menu_activePage < MENU_MIDI_PAGE) || menu_activePage == PERFORMANCE_PAGE ||menu_activePage == SEQ_PAGE || menu_activePage == EUKLID_PAGE) {
                              led_setActive_step(frontParser_midiMsg.data2);
                           }							
                        } 
                        else {
                           led_clearActive_step();
                        }								
                     								
                     }							
                     break;
               	
               
               	
                  case LED_PULSE_BEAT:
                     if(frontParser_midiMsg.data2!=0)
                     {
                        led_setValue(1,LED_START_STOP);
                     }
                     else
                     {
                        led_setValue(0,LED_START_STOP);
                     }							
                     break;
                  case LED_SEQ_SUB_STEP:
                     if(buttonHandler_getShift() || buttonHandler_getMode() == SELECT_MODE_STEP)
                     {
                     	//parse sub steps
                     	
                     	
                        uint8_t stepNr = frontParser_midiMsg.data2 & 0x7f;
                        uint8_t subStepRange = buttonHandler_selectedStep;
                     	//check if received step is a valid sub step
                        if( (stepNr >= subStepRange) && (stepNr<(subStepRange+8)) )
                        {
                           stepNr = (uint8_t)(stepNr - subStepRange);
                           led_setValue(1,(uint8_t)(LED_PART_SELECT1+stepNr));
                        }
                     	
                     	
                     }
                     break;
                  
                  case LED_SEQ_MAIN_FOUR:
                     offset=(uint8_t)(offset+4);
                  case LED_SEQ_MAIN_THREE:
                     offset=(uint8_t)(offset+4);
                  case LED_SEQ_MAIN_TWO:
                     offset=(uint8_t)(offset+4);
                  case LED_SEQ_MAIN_ONE:
                     {
                     	//parse sub steps
                        uint8_t i;
                        uint8_t ledArray=(frontParser_midiMsg.data2 & 0x0f);
                        for (i=0;i<4;i++)
                        {
                           led_setValue( (uint8_t)(ledArray&(0x01<<i)),(uint8_t)(LED_STEP1+i+offset));
                        }
                     }
                     
                  
                     break;                  
                  case LED_SEQ_SUB_STEP_LOWER:
                     if ( ( (menu_activePage<=VOICE7_PAGE)&&(shiftState||copyClear_getCopyMode()) ) || (menu_activePage==SEQ_PAGE) || (menu_activePage==EUKLID_PAGE))
                     {
                     	//parse sub steps
                        uint8_t i;
                        uint8_t ledArray=(frontParser_midiMsg.data2 & 0x0f);
                        for (i=0;i<4;i++)
                        {
                           led_setValue( (uint8_t)(ledArray&(0x01<<i)),(uint8_t)(LED_PART_SELECT1+i));
                        }
                     }
                     break;
                  
                  case LED_SEQ_SUB_STEP_UPPER:
                     if ( ( (menu_activePage<=VOICE7_PAGE)&&(shiftState||copyClear_getCopyMode()) ) || (menu_activePage==SEQ_PAGE) || (menu_activePage==EUKLID_PAGE))
                     {
                     	//parse sub steps
                        uint8_t i;
                        uint8_t ledArray=(frontParser_midiMsg.data2 & 0x0f);
                        for (i=0;i<4;i++)
                        {
                           led_setValue( (uint8_t)(ledArray&(0x01<<i)),(uint8_t)(LED_PART_SELECT1+4+i));
                        }
                     }
                     break;
                  
                  case LED_SEQ_BUTTON:
                     {
                        if(menu_activePage != PERFORMANCE_PAGE) //do not show active steps on perf. page
                        {
                        //limit to 16 steps
                           uint8_t stepNr = (uint8_t)((frontParser_midiMsg.data2&0x7f)/8); //limit to 127
                        
                           led_setValue(1,(uint8_t)(LED_STEP1+stepNr));
                        }
                     }		
                  
                     break;
                  case LCD_PRINT_SCREEN:
                     {
                        
                        char text[8];
                        lcd_clear();
                        lcd_home();
                        lcd_string_F(PSTR("cortex says"));
                        lcd_setcursor(0,2);
                        itoa((int)frontParser_midiMsg.data2,text,10);
                        lcd_string(text);
                        _delay_ms(2000);
                     }
                     break;
               }						
            }								
            else if(frontParser_midiMsg.status == NOTE_ON)
            {
               if(frontParser_midiMsg.data1 > 6) 
                  return;
            	//only SELECT_MODE_VOICE and SELECT_MODE_MUTE
               led_pulseLed((uint8_t)(LED_VOICE1+frontParser_midiMsg.data1));
            }
         
         }				
      }
   }
}
//------------------------------------------------------------
void midiMsg_checkLongOps()
{
   if (frontPanel_longOp){
   
      if (frontPanel_longOp==BANK_GLOBAL)
      {
         if (parameter_values[PAR_LOAD_PERF_ON_BANK]){
            preset_loadPerf(frontPanel_longData,0x7f); // preset, isAll, release kitlock, voiceArray
            if(frontPanel_morphAvail)
            {
               frontPanel_longOp=MORPH_OP;
               frontPanel_morphAvail=0;
            }
            else
               frontPanel_longOp=NULL_OP; 
		 
	    buttonHandler_handleModeButtons(SELECT_MODE_PERF);
	    menu_repaint();
         }
         else {
            preset_loadDrumset(frontPanel_longData,0x7f,0);
            menu_repaint();
            if(frontPanel_morphAvail)
            {
               frontPanel_longOp=MORPH_OP;
               frontPanel_morphAvail=0;
            }
            else
               frontPanel_longOp=NULL_OP;
         }
         
         
      }
      else if (frontPanel_longOp==PATTERN_CHANGE_OP)
      {
         // nothing to see here yet
         if(frontPanel_morphAvail)
         {
            frontPanel_longOp=MORPH_OP;
            frontPanel_morphAvail=0;
         }
         else
            frontPanel_longOp=NULL_OP;
      }
         
      else if (frontPanel_longOp<BANK_GLOBAL)
      // we have (possibly multiple) voice bank changes
      {
        
         if (parameter_values[PAR_LOAD_PERF_ON_BANK]){
            preset_loadPerf(frontPanel_longData,frontPanel_longOp);
            menu_repaint();
            if(frontPanel_morphAvail)
            {
               frontPanel_longOp=MORPH_OP;
               frontPanel_morphAvail=0;
            }
            else
               frontPanel_longOp=NULL_OP;            
         }
         else {
            preset_loadDrumset(frontPanel_longData,frontPanel_longOp,0);
            menu_repaint();
            if(frontPanel_morphAvail)
            {
               frontPanel_longOp=MORPH_OP;
               frontPanel_morphAvail=0;
            }
            else
               frontPanel_longOp=NULL_OP; 
         }
         
      }
      
      
      else if (frontPanel_longOp==MORPH_OP)
      {
         parameter_values[PAR_MORPH]=frontPanel_longData;
         preset_morph(frontPanel_morphArray,frontPanel_longData);
         morphValue=frontPanel_longData;
         menu_repaint();
         if(frontPanel_morphAvail)
         {
            frontPanel_longOp=MORPH_OP;
            frontPanel_morphAvail=0;
         }
         else
            frontPanel_longOp=NULL_OP;
      }
      
      else
      {
         frontPanel_longOp=NULL_OP;
      }
   }
   
}
//------------------------------------------------------------
void frontPanel_sendMidiMsg(MidiMsg msg)
{
   while(uart_putc(msg.status) == 0); 
	//data 1 - parameter number
   while(uart_putc(msg.data1) == 0);
	//data 2 - value	
   while(uart_putc(msg.data2) == 0);
}





//------------------------------------------------------------
void frontPanel_sendData(uint8_t status, uint8_t data1, uint8_t data2)
{
   /* RESTORE: Suppress outbound traffic while a parameter restore transaction is active
      to prevent feedback loops where pushed-up display values are sent back as authoritative edits.
      Allow READY and ACK messages to pass through. */
   if(frontParser_restoreActive && status != PARAM_RESTORE_READY && status != PARAM_RESTORE_ACK)
   {
      return;
   }

   uint8_t queued = 0;
   uint8_t usesCredit = (uint8_t)(comm_flowActive && frontPanel_flowUsesCredit(status, data1));

   if(usesCredit && !frontPanel_waitForCredit())
      return;

   while(!queued)
   {
      while(uart_txFree() < 3);

      // Keep the three-byte message contiguous, but never wait with interrupts masked.
      ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
      {
         if(uart_txFree() >= 3)
         {
            (void)uart_putc(status);
            (void)uart_putc(data1);
            (void)uart_putc(data2);
            queued = 1;
         }
      }
   }

   if(usesCredit && (comm_txCredits > 0))
      comm_txCredits--;
}
//------------------------------------------------------------
void frontPanel_sendByte(uint8_t data)
{
   while(uart_putc(data) == 0); 
}
//------------------------------------------------------------
