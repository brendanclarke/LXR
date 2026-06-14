/*
 * frontPanelSendingProtocol.c
 *
 * AVR front-panel outbound packet helpers and send-side flow-control state.
 */

#include "frontPanelSendingProtocol.h"

#include "IO/uart.h"
#include "Hardware/timebase.h"
#include "Menu/menu.h"
#include <util/atomic.h>

#define FLOW_WAIT_TICKS 384

static uint8_t frontPanel_wait = 0;
static uint8_t comm_flowActive = 0;
static uint8_t comm_flowChannel = 0;
static uint8_t comm_txCredits = 0;
static uint8_t comm_flowAckPending = 0;
static uint8_t comm_flowAckChannel = 0;
static uint8_t comm_flowFailed = 0;
static uint8_t comm_prfCacheStatusCommand = 0;
static uint8_t comm_prfCacheStatusValue = PRF_CACHE_REJECTED;

uint8_t frontPanelSending_isFlowCommand(uint8_t command)
{
   return (command >= SEQ_FLOW_BEGIN) && (command <= SEQ_FLOW_ABORT);
}

static uint8_t frontPanel_flowUsesCredit(uint8_t status, uint8_t data1)
{
   return !(status == SEQ_CC && frontPanelSending_isFlowCommand(data1));
}

static uint16_t frontPanel_flowNow(void)
{
   uint16_t now;

   ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
   {
      now = time_sysTick;
   }

   return now;
}

void frontPanelSending_handleCallbackAck(void)
{
   frontPanel_wait = 0;
}

void frontPanelSending_handleFlowMessage(uint8_t command, uint8_t data)
{
   uint8_t channel = data;
   uint8_t credits = 0;

   switch(command)
   {
      case SEQ_FLOW_GRANT:
         channel = (uint8_t)((data >> 4) & 0x07);
         credits = (uint8_t)(data & 0x0f);
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

void frontPanelSending_handlePrfCacheStatus(uint8_t command, uint8_t status)
{
   comm_prfCacheStatusCommand = command;
   comm_prfCacheStatusValue = status;
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

static uint8_t frontPanel_waitForCredit(void)
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
void frontPanel_updatePatternLeds(void)
{
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   frontPanel_sendData(LED_CC, LED_QUERY_SEQ_TRACK, value);
}
//------------------------------------------------------------
void frontPanel_updateActiveStepLeds(void)
{
   /*
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   frontPanel_sendData(LED_CC, LED_QUERY_SEQ_TRACK, value);
   */
}
//------------------------------------------------------------
void frontPanel_updateSubstepLeds(void)
{
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   frontPanel_sendData(LED_CC, LED_ALL_SUBSTEP, value);
}
//------------------------------------------------------------
void frontPanel_holdForBuffer(void)
{
  // start and end sysex to make sure mainboard is caught up on messages and can respond
// frontParser_command.status = 0;
frontPanel_wait=1;
frontPanel_sendByte(CALLBACK_ACK);
while(frontPanel_wait)
{
    uart_checkAndParse();
}
}
//------------------------------------------------------------
uint8_t frontPanel_flowBeginSession(void)
{
   return frontPanel_flowBegin(FLOW_CH_LOAD_SESSION);
}
//------------------------------------------------------------
uint8_t frontPanel_flowEndSession(void)
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
uint8_t frontPanel_flowFailed(void)
{
   return comm_flowFailed;
}
//------------------------------------------------------------
void frontPanel_flowAbortSession(void)
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
   if(frontParser_isRestoreActive() && status != PARAM_RESTORE_READY && status != PARAM_RESTORE_ACK)
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
