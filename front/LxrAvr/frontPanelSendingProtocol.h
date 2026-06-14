/*
 * frontPanelSendingProtocol.h
 *
 * AVR front-panel outbound packet helpers. The receive/protocol header owns
 * opcode values and receive parser state; this header owns STM-bound packet
 * construction and flow-control send state.
 */

#ifndef FRONTPANELSENDINGPROTOCOL_H_
#define FRONTPANELSENDINGPROTOCOL_H_

#include "frontPanelReceivingProtocol.h"

void frontPanel_holdForBuffer(void);
void frontPanel_sendMidiMsg(MidiMsg msg);
void frontPanel_sendData(uint8_t status, uint8_t data1, uint8_t data2);
void frontPanel_sendByte(uint8_t data);
void frontPanel_updatePatternLeds(void);
void frontPanel_updateActiveStepLeds(void);
void frontPanel_updateSubstepLeds(void);
void frontPanel_sendMacro(uint8_t whichMacro,uint8_t value);
uint8_t frontPanel_flowBegin(uint8_t channel);
uint8_t frontPanel_flowEnd(uint8_t channel);
uint8_t frontPanel_flowFailed(void);
uint8_t frontPanel_flowBeginSession(void);
uint8_t frontPanel_flowEndSession(void);
void frontPanel_flowAbortSession(void);
uint8_t frontPanel_prfCacheBegin(uint8_t fileType);
uint8_t frontPanel_prfCacheControl(uint8_t command, uint8_t fileType);

uint8_t frontPanelSending_isFlowCommand(uint8_t command);
void frontPanelSending_handleFlowMessage(uint8_t command, uint8_t data);
void frontPanelSending_handlePrfCacheStatus(uint8_t command, uint8_t status);
void frontPanelSending_handleCallbackAck(void);

#endif /* FRONTPANELSENDINGPROTOCOL_H_ */
