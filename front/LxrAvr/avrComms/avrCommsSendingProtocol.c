/*
 * avrCommsSendingProtocol.c
 *
 * AVR comms outbound packet helpers and send-side flow-control state.
 */

#include "avrCommsSendingProtocol.h"

#include "IO/uart.h"
#include "Hardware/timebase.h"
#include "Menu/menu.h"
#include <util/atomic.h>

#define FLOW_WAIT_TICKS 384

static uint8_t avrComms_wait = 0;
static uint8_t comm_flowActive = 0;
static uint8_t comm_flowChannel = 0;
static uint8_t comm_txCredits = 0;
static uint8_t comm_flowAckPending = 0;
static uint8_t comm_flowAckChannel = 0;
static uint8_t comm_flowFailed = 0;
//#if 0
//static uint8_t comm_prfCacheStatusCommand = 0;
//static uint8_t comm_prfCacheStatusValue = PRF_CACHE_REJECTED;
//#endif

uint8_t avrCommsSending_isFlowCommand(uint8_t command)
{
   return (command >= SEQ_FLOW_BEGIN) && (command <= SEQ_FLOW_ABORT);
}

static uint8_t avrComms_flowUsesCredit(uint8_t status, uint8_t data1)
{
   return !(status == SEQ_CC && avrCommsSending_isFlowCommand(data1));
}

static uint16_t avrComms_flowNow(void)
{
   uint16_t now;

   ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
   {
      now = time_sysTick;
   }

   return now;
}

void avrCommsSending_handleCallbackAck(void)
{
   avrComms_wait = 0;
}

void avrCommsSending_handleFlowMessage(uint8_t command, uint8_t data)
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

//#if 0
//void avrCommsSending_handlePrfCacheStatus(uint8_t command, uint8_t status)
//{
//   comm_prfCacheStatusCommand = command;
//   comm_prfCacheStatusValue = status;
//}
//#endif

static uint8_t avrComms_waitForFlowAck(uint8_t channel)
{
   uint16_t start = avrComms_flowNow();

   while(comm_flowAckPending)
   {
      uart_checkAndParse();
      if((uint16_t)(avrComms_flowNow() - start) > FLOW_WAIT_TICKS)
      {
         avrComms_sendData(SEQ_CC, SEQ_FLOW_ABORT, channel);
         comm_flowAckPending = 0;
         comm_flowActive = 0;
         comm_txCredits = 0;
         comm_flowFailed = 1;
         return 0;
      }
   }

   return !comm_flowFailed;
}

static uint8_t avrComms_flowSendAndWait(uint8_t command, uint8_t channel)
{
   comm_flowAckChannel = channel;
   comm_flowAckPending = 1;
   avrComms_sendData(SEQ_CC, command, channel);

   return avrComms_waitForFlowAck(channel);
}

static uint8_t avrComms_waitForCredit(void)
{
   uint16_t start = avrComms_flowNow();

   while(comm_flowActive && (comm_txCredits == 0))
   {
      uart_checkAndParse();
      if((uint16_t)(avrComms_flowNow() - start) > FLOW_WAIT_TICKS)
      {
         avrComms_sendData(SEQ_CC, SEQ_FLOW_ABORT, comm_flowChannel);
         comm_flowActive = 0;
         comm_txCredits = 0;
         comm_flowFailed = 1;
         return 0;
      }
   }

   return !comm_flowFailed;
}

//------------------------------------------------------------
void avrComms_sendMacro(uint8_t whichMacro,uint8_t value)
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
      avrComms_sendData(MACRO_CC, data1, value);
   }
   else if (whichMacro==2){
      data1=0x40;
      avrComms_sendData(MACRO_CC, data1, value);
   }
}
//------------------------------------------------------------
void avrComms_sendVoiceMorphValue(uint8_t voice, uint8_t amount)
{
   if(voice >= 6)
      return;

   avrComms_sendData(VOICE_MORPH, voice, (uint8_t)(amount & 0x7f));
   avrComms_sendData(VOICE_MORPH, (uint8_t)(voice + 6), (uint8_t)((amount >> 7) & 0x01));
}
//------------------------------------------------------------
void avrComms_updatePatternLeds(void)
{
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   avrComms_sendData(LED_CC, LED_QUERY_SEQ_TRACK, value);
}
//------------------------------------------------------------
void avrComms_updateActiveStepLeds(void)
{
   /*
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   avrComms_sendData(LED_CC, LED_QUERY_SEQ_TRACK, value);
   */
}
//------------------------------------------------------------
void avrComms_updateSubstepLeds(void)
{
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   avrComms_sendData(LED_CC, LED_ALL_SUBSTEP, value);
}
//------------------------------------------------------------
void avrComms_holdForBuffer(void)
{
  // start and end sysex to make sure mainboard is caught up on messages and can respond
// avrCommsParser_command.status = 0;
avrComms_wait=1;
avrComms_sendByte(CALLBACK_ACK);
while(avrComms_wait)
{
    uart_checkAndParse();
}
}
//------------------------------------------------------------
uint8_t avrComms_flowBeginSession(void)
{
   return avrComms_flowBegin(FLOW_CH_LOAD_SESSION);
}
//------------------------------------------------------------
uint8_t avrComms_flowEndSession(void)
{
   return avrComms_flowEnd(FLOW_CH_LOAD_SESSION);
}
//------------------------------------------------------------
uint8_t avrComms_flowBegin(uint8_t channel)
{
   comm_flowActive = (channel != FLOW_CH_LOAD_SESSION);
   comm_flowChannel = channel;
   comm_txCredits = 0;
   comm_flowFailed = 0;

   if(!avrComms_flowSendAndWait(SEQ_FLOW_BEGIN, channel))
      return 0;

   if(channel != FLOW_CH_LOAD_SESSION)
      comm_flowActive = 1;

   return 1;
}
//------------------------------------------------------------
uint8_t avrComms_flowEnd(uint8_t channel)
{
   uint8_t ret = avrComms_flowSendAndWait(SEQ_FLOW_END, channel);

   if(channel == comm_flowChannel)
   {
      comm_flowActive = 0;
      comm_txCredits = 0;
   }

   return ret && !comm_flowFailed;
}
//------------------------------------------------------------
uint8_t avrComms_flowFailed(void)
{
   return comm_flowFailed;
}
//------------------------------------------------------------
void avrComms_flowAbortSession(void)
{
   avrComms_sendData(SEQ_CC, SEQ_FLOW_ABORT, FLOW_CH_LOAD_SESSION);
   comm_flowAckPending = 0;
   comm_flowActive = 0;
   comm_txCredits = 0;
   comm_flowFailed = 1;
}
//------------------------------------------------------------
//#if 0
//uint8_t avrComms_prfCacheBegin(uint8_t fileType)
//{
//   comm_prfCacheStatusCommand = 0;
//   comm_prfCacheStatusValue = PRF_CACHE_REJECTED;
//
//   if(!avrComms_prfCacheControl(SEQ_PRF_CACHE_BEGIN, fileType))
//      return PRF_CACHE_REJECTED;
//
//   if(comm_prfCacheStatusCommand != SEQ_PRF_CACHE_BEGIN)
//      return PRF_CACHE_REJECTED;
//
//   return comm_prfCacheStatusValue;
//}
//------------------------------------------------------------
//uint8_t avrComms_prfCacheControl(uint8_t command, uint8_t fileType)
//{
//   comm_flowAckChannel = FLOW_CH_LOAD_SESSION;
//   comm_flowAckPending = 1;
//   avrComms_sendData(SEQ_CC, command, fileType);
//
//   return avrComms_waitForFlowAck(FLOW_CH_LOAD_SESSION);
//}
//#endif
//------------------------------------------------------------
void avrComms_sendMidiMsg(MidiMsg msg)
{
   while(uart_putc(msg.status) == 0);
	//data 1 - parameter number
   while(uart_putc(msg.data1) == 0);
	//data 2 - value
   while(uart_putc(msg.data2) == 0);
}

//------------------------------------------------------------
void avrComms_sendData(uint8_t status, uint8_t data1, uint8_t data2)
{
   /* RESTORE: Suppress outbound traffic while a parameter restore transaction is active
      to prevent feedback loops where pushed-up display values are sent back as authoritative edits.
      Allow READY and ACK messages to pass through. */
   if(avrCommsParser_isRestoreActive() && status != PARAM_RESTORE_READY && status != PARAM_RESTORE_ACK)
   {
      return;
   }

   uint8_t queued = 0;
   uint8_t usesCredit = (uint8_t)(comm_flowActive && avrComms_flowUsesCredit(status, data1));

   if(usesCredit && !avrComms_waitForCredit())
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
void avrComms_sendByte(uint8_t data)
{
   while(uart_putc(data) == 0);
}
//------------------------------------------------------------
