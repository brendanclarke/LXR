/*
 * avrCommsSendingProtocol.h
 *
 * AVR comms outbound packet helpers. The receive/protocol header owns
 * opcode values and receive parser state; this header owns STM-bound packet
 * construction and flow-control send state.
 */

#ifndef AVRCOMMSSENDINGPROTOCOL_H_
#define AVRCOMMSSENDINGPROTOCOL_H_

#include "avrCommsReceivingProtocol.h"

void avrComms_holdForBuffer(void);
void avrComms_sendMidiMsg(MidiMsg msg);
void avrComms_sendData(uint8_t status, uint8_t data1, uint8_t data2);
void avrComms_sendByte(uint8_t data);
void avrComms_updatePatternLeds(void);
void avrComms_updateActiveStepLeds(void);
void avrComms_updateSubstepLeds(void);
void avrComms_sendMacro(uint8_t whichMacro,uint8_t value);
uint8_t avrComms_flowBegin(uint8_t channel);
uint8_t avrComms_flowEnd(uint8_t channel);
uint8_t avrComms_flowFailed(void);
uint8_t avrComms_flowBeginSession(void);
uint8_t avrComms_flowEndSession(void);
void avrComms_flowAbortSession(void);
//#if 0
//uint8_t avrComms_prfCacheBegin(uint8_t fileType);
//uint8_t avrComms_prfCacheControl(uint8_t command, uint8_t fileType);
//#endif

uint8_t avrCommsSending_isFlowCommand(uint8_t command);
void avrCommsSending_handleFlowMessage(uint8_t command, uint8_t data);
//#if 0
//void avrCommsSending_handlePrfCacheStatus(uint8_t command, uint8_t status);
//#endif
void avrCommsSending_handleCallbackAck(void);

#endif /* AVRCOMMSSENDINGPROTOCOL_H_ */
