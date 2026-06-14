/*
 * frontPanelSendingProtocol.h
 *
 * Front-panel outbound packet helpers owned by the uARTFrontSYX layer.
 * These helpers assemble reusable packet families so parser and sequencer
 * code do not need to rebuild the same byte triples inline.
 */

#ifndef UARTFRONTSYX_FRONTPANELSENDINGPROTOCOL_H_
#define UARTFRONTSYX_FRONTPANELSENDINGPROTOCOL_H_

#include <stdint.h>

#include "FrontPanelProtocol.h"

/* Low-level transport wrappers. These are the only helpers that should talk to
   the raw front-panel UART primitives. */
void frontPanelSending_sendByte(uint8_t data);
void frontPanelSending_sendPriorityByte(uint8_t data);
void frontPanelSending_sendPriorityByteWait(uint8_t data);
void frontPanelSending_sendSysExByte(uint8_t data);

/* Small packet builders used by the parser, sequencer, and restore code. */
void frontPanelSending_sendTriplet(uint8_t status, uint8_t data1, uint8_t data2);
void frontPanelSending_sendPriorityTriplet(uint8_t status, uint8_t data1, uint8_t data2);
void frontPanelSending_sendPriorityTripletWait(uint8_t status, uint8_t data1, uint8_t data2);
void frontPanelSending_sendCallbackAck(void);
void frontPanelSending_sendSampleUploadAck(void);
void frontPanelSending_sendSampleCountReply(void);
void frontPanelSending_sendTrackLengthReply(uint8_t trackNr);
void frontPanelSending_sendTrackRotationReply(uint8_t trackNr);
void frontPanelSending_sendTrackRotationValue(uint8_t rotation);
void frontPanelSending_sendBankChange(uint8_t bankCode, uint8_t value);
void frontPanelSending_sendParameterEcho(uint16_t paramNr, uint8_t value);
void frontPanelSending_sendPatchReset(void);
void frontPanelSending_sendVoiceActivity(uint8_t voice);
void frontPanelSending_sendPatternParamsReply(uint8_t patternNr);
void frontPanelSending_sendPatternDataReply(uint8_t patternNr);
void frontPanelSending_sendEuklidParamsReply(uint8_t trackNr);
void frontPanelSending_sendActiveTrackReply(uint8_t trackNr);
void frontPanelSending_sendMainStepLedReply(uint8_t trackNr,
                                           uint8_t stepNr,
                                           uint8_t patternNr);
void frontPanelSending_sendStepParamsReply(uint8_t patternNr,
                                          uint8_t trackNr,
                                          uint8_t stepNr);
void frontPanelSending_sendSysexStartAck(void);
void frontPanelSending_sendSysexEndAck(void);
void frontPanelSending_sendSysexReceiveAck(uint8_t mode);
void frontPanelSending_sendSysexStepAck(void);
void frontPanelSending_sendSysexBeginPatternTransmitAck(void);
void frontPanelSending_sendRestoreBegin(void);
void frontPanelSending_sendRestoreDone(void);
void frontPanelSending_sendRestoreParam(uint16_t param, uint8_t value);
void frontPanelSending_sendRestoreMorphParam(uint16_t param, uint8_t value);
void frontPanelSending_sendGlobalMorphReport(uint8_t amount);
void frontPanelSending_sendPatternChange(uint8_t pattern);
void frontPanelSending_sendRunStop(uint8_t isRunning);
void frontPanelSending_sendBeatLed(uint8_t onOff);
void frontPanelSending_sendCurrentStepLed(uint8_t stepNr);
void frontPanelSending_sendMainStepInfo(uint16_t stepNr);
void frontPanelSending_sendStepInfo(uint16_t stepNr);

/* Display helpers for the step LEDs. */
void frontPanelSending_updateTrackLeds(uint8_t trackNr,
                                      uint8_t patternNr,
                                      uint8_t activeStep);
void frontPanelSending_updateSubStepLeds(uint8_t trackNr,
                                        uint8_t patternNr,
                                        uint8_t activeStep);

/* Flow-control and parser cache status helpers. */
void frontPanelSending_sendFlowGrant(uint8_t channel, uint8_t credits);
void frontPanelSending_sendFlowGrantWait(uint8_t channel, uint8_t credits);
void frontPanelSending_sendFlowAbort(uint8_t channel);
void frontPanelSending_sendPrfCacheStatus(uint8_t command, uint8_t status);

#endif /* UARTFRONTSYX_FRONTPANELSENDINGPROTOCOL_H_ */
