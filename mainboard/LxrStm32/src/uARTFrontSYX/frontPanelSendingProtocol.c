/*
 * frontPanelSendingProtocol.c
 *
 * Packet composition helpers for outbound front-panel protocol traffic.
 * This file owns reusable packet families and the parser-facing reply helpers
 * so the receive code does not need to open-code packet triples.
 */

#include "frontPanelSendingProtocol.h"

#include "Uart.h"
#include "SampleRom/SampleMemory.h"
#include "Sequencer/sequencer.h"
#include "Sequencer/Pattern/EuklidGenerator.h"

void frontPanelSending_sendByte(uint8_t data)
{
   uart_sendFrontpanelByte(data);
}

void frontPanelSending_sendPriorityByte(uint8_t data)
{
   uart_sendFrontpanelPriorityByte(data);
}

void frontPanelSending_sendPriorityByteWait(uint8_t data)
{
   uart_sendFrontpanelPriorityByteWait(data);
}

void frontPanelSending_sendSysExByte(uint8_t data)
{
   uart_sendFrontpanelSysExByte(data);
}

void frontPanelSending_sendTriplet(uint8_t status, uint8_t data1, uint8_t data2)
{
   frontPanelSending_sendByte(status);
   frontPanelSending_sendByte(data1);
   frontPanelSending_sendByte(data2);
}

void frontPanelSending_sendPriorityTriplet(uint8_t status, uint8_t data1, uint8_t data2)
{
   frontPanelSending_sendPriorityByte(status);
   frontPanelSending_sendPriorityByte(data1);
   frontPanelSending_sendPriorityByte(data2);
}

void frontPanelSending_sendPriorityTripletWait(uint8_t status, uint8_t data1, uint8_t data2)
{
   frontPanelSending_sendPriorityByteWait(status);
   frontPanelSending_sendPriorityByteWait(data1);
   frontPanelSending_sendPriorityByteWait(data2);
}

void frontPanelSending_sendCallbackAck(void)
{
   frontPanelSending_sendPriorityByte(FRONT_CALLBACK_ACK);
}

void frontPanelSending_sendSampleUploadAck(void)
{
   frontPanelSending_sendByte(ACK);
}

void frontPanelSending_sendSampleCountReply(void)
{
   frontPanelSending_sendByte(SAMPLE_CC);
   frontPanelSending_sendByte(FRONT_SAMPLE_COUNT);
   frontPanelSending_sendByte(sampleMemory_getNumSamples());
}

void frontPanelSending_sendTrackLengthReply(uint8_t trackNr)
{
   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_TRACK_LENGTH);
   frontPanelSending_sendByte(seq_getTrackLength(trackNr));
}

void frontPanelSending_sendTrackRotationReply(uint8_t trackNr)
{
   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_TRACK_ROTATION);
   frontPanelSending_sendByte(seq_getTrackRotation(trackNr));
}

void frontPanelSending_sendPatternParamsReply(uint8_t patternNr)
{
   PatternSetting *patternSetting = seq_getPatternSettingPtr(patternNr);

   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_SET_PAT_BEAT);
   frontPanelSending_sendByte(patternSetting->changeBar);

   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_SET_PAT_NEXT);
   frontPanelSending_sendByte(patternSetting->nextPattern);
}

void frontPanelSending_sendPatternDataReply(uint8_t patternNr)
{
   frontPanelSending_sendSysExByte(seq_patternSet.seq_patternSettings[patternNr].nextPattern);
   frontPanelSending_sendSysExByte(seq_patternSet.seq_patternSettings[patternNr].changeBar);
}

void frontPanelSending_sendEuklidParamsReply(uint8_t trackNr)
{
   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_EUKLID_LENGTH);
   frontPanelSending_sendByte(euklid_getLength(trackNr));

   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_EUKLID_STEPS);
   frontPanelSending_sendByte(euklid_getSteps(trackNr));

   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_EUKLID_ROTATION);
   frontPanelSending_sendByte(euklid_getRotation(trackNr));

   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_EUKLID_SUBSTEP_ROTATION);
   frontPanelSending_sendByte(euklid_getSubStepRotation(trackNr));

   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_TRACK_SCALE);
   frontPanelSending_sendByte(seq_getTrackScale(trackNr));
}

void frontPanelSending_sendActiveTrackReply(uint8_t trackNr)
{
   frontPanelSending_sendTrackRotationReply(trackNr);

   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_TRANSPOSE);
   frontPanelSending_sendByte(seq_transpose_voiceAmount[trackNr]);
}

void frontPanelSending_sendMainStepLedReply(uint8_t trackNr, uint8_t stepNr, uint8_t patternNr)
{
   if(seq_isMainStepActive(trackNr, stepNr, patternNr))
   {
      frontPanelSending_sendTriplet(FRONT_STEP_LED_STATUS_BYTE,
                                    FRONT_LED_SEQ_BUTTON,
                                    (uint8_t)(stepNr * 8));
   }
}

void frontPanelSending_sendStepParamsReply(uint8_t patternNr, uint8_t trackNr, uint8_t stepNr)
{
   Step *step = seq_getStepPtr(patternNr, trackNr, stepNr);
   uint8_t hi;
   uint8_t lo;
   uint8_t dest;

   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_VOLUME);
   frontPanelSending_sendByte(step->volume & STEP_VOLUME_MASK);

   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_NOTE);
   frontPanelSending_sendByte(step->note);

   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_PROB);
   frontPanelSending_sendByte(step->prob);

   dest = step->param1Nr;
   if(dest < 128 && dest)
      dest--;
   hi = (uint8_t)(dest >> 7);
   lo = (uint8_t)(dest & 0x7f);
   frontPanelSending_sendByte(FRONT_SET_P1_DEST);
   frontPanelSending_sendByte(hi);
   frontPanelSending_sendByte(lo);

   hi = (uint8_t)(step->param1Val >> 7);
   lo = (uint8_t)(step->param1Val & 0x7f);
   frontPanelSending_sendByte(FRONT_SET_P1_VAL);
   frontPanelSending_sendByte(hi);
   frontPanelSending_sendByte(lo);

   dest = step->param2Nr;
   if(dest < 128 && dest)
      dest--;
   hi = (uint8_t)(dest >> 7);
   lo = (uint8_t)(dest & 0x7f);
   frontPanelSending_sendByte(FRONT_SET_P2_DEST);
   frontPanelSending_sendByte(hi);
   frontPanelSending_sendByte(lo);

   hi = (uint8_t)(step->param2Val >> 7);
   lo = (uint8_t)(step->param2Val & 0x7f);
   frontPanelSending_sendByte(FRONT_SET_P2_VAL);
   frontPanelSending_sendByte(hi);
   frontPanelSending_sendByte(lo);
}

void frontPanelSending_sendSysexStartAck(void)
{
   frontPanelSending_sendSysExByte(SYSEX_START);
}

void frontPanelSending_sendSysexEndAck(void)
{
   frontPanelSending_sendSysExByte(SYSEX_END);
}

void frontPanelSending_sendSysexReceiveAck(uint8_t mode)
{
   frontPanelSending_sendSysExByte(mode);
}

void frontPanelSending_sendSysexStepAck(void)
{
   frontPanelSending_sendSysExByte(SYSEX_STEP_ACK);
}

void frontPanelSending_sendSysexBeginPatternTransmitAck(void)
{
   frontPanelSending_sendSysExByte(SYSEX_BEGIN_PATTERN_TRANSMIT);
}

void frontPanelSending_updateTrackLeds(uint8_t trackNr, uint8_t patternNr, uint8_t activeStep)
{
   uint8_t ledByte = 0x00;
   uint8_t i;
   uint8_t k;

   if(trackNr > 6)
      return;

   for(k=0;k<4;k++)
   {
      for(i=0;i<4;i++)
      {
         if(seq_isMainStepActive(trackNr,(uint8_t)((k<<2) + i),patternNr))
            ledByte |= (0x01<<i);
      }

      frontPanelSending_sendTriplet(FRONT_STEP_LED_STATUS_BYTE,
                                    (uint8_t)(FRONT_LED_SEQ_MAIN_ONE+k),
                                    ledByte);
      ledByte = 0x00;
   }

   frontPanelSending_updateSubStepLeds(trackNr, patternNr, activeStep);
}

void frontPanelSending_updateSubStepLeds(uint8_t trackNr, uint8_t patternNr, uint8_t activeStep)
{
   uint8_t start = activeStep & 0x78;
   uint8_t ledByte = 0x00;
   uint8_t i;

   for(i=0;i<4;i++)
   {
      if(seq_isStepActive(trackNr,(start+i),patternNr))
         ledByte |= (0x01<<i);
   }

   frontPanelSending_sendTriplet(FRONT_STEP_LED_STATUS_BYTE,
                                 FRONT_LED_SEQ_SUB_STEP_LOWER,
                                 ledByte);

   ledByte = 0x00;

   for(i=0;i<4;i++)
   {
      if(seq_isStepActive(trackNr,(start+4+i),patternNr))
         ledByte |= (0x01<<i);
   }

   frontPanelSending_sendTriplet(FRONT_STEP_LED_STATUS_BYTE,
                                 FRONT_LED_SEQ_SUB_STEP_UPPER,
                                 ledByte);
}

static uint8_t frontPanelSending_flowGrantByte(uint8_t channel, uint8_t credits)
{
   return (uint8_t)(((channel & 0x07) << 4) | (credits & 0x0f));
}

void frontPanelSending_sendFlowGrant(uint8_t channel, uint8_t credits)
{
   uart_sendFrontpanelPriorityByte(FRONT_SEQ_CC);
   uart_sendFrontpanelPriorityByte(FRONT_SEQ_FLOW_GRANT);
   uart_sendFrontpanelPriorityByte(frontPanelSending_flowGrantByte(channel, credits));
}

void frontPanelSending_sendFlowGrantWait(uint8_t channel, uint8_t credits)
{
   uart_sendFrontpanelPriorityByteWait(FRONT_SEQ_CC);
   uart_sendFrontpanelPriorityByteWait(FRONT_SEQ_FLOW_GRANT);
   uart_sendFrontpanelPriorityByteWait(frontPanelSending_flowGrantByte(channel, credits));
}

void frontPanelSending_sendFlowAbort(uint8_t channel)
{
   uart_sendFrontpanelPriorityByte(FRONT_SEQ_CC);
   uart_sendFrontpanelPriorityByte(FRONT_SEQ_FLOW_ABORT);
   uart_sendFrontpanelPriorityByte(channel);
}

void frontPanelSending_sendPrfCacheStatus(uint8_t command, uint8_t status)
{
   uart_sendFrontpanelPriorityByte(PRF_CACHE_STATUS);
   uart_sendFrontpanelPriorityByte(command);
   uart_sendFrontpanelPriorityByte(status);
}
