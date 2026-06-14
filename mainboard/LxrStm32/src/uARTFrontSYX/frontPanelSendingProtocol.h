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

/* Packet-composition helpers for outbound front-panel traffic.
   - Priority packets are used for flow-control and restore traffic.
   - Wait variants block until the transport FIFO can accept the packet.
   - The caller decides when a packet is allowed to be emitted. */

void frontPanelSending_sendByte(uint8_t data);
void frontPanelSending_sendPriorityByte(uint8_t data);
void frontPanelSending_sendPriorityByteWait(uint8_t data);
void frontPanelSending_sendSysExByte(uint8_t data);
void frontPanelSending_sendTriplet(uint8_t status, uint8_t data1, uint8_t data2);
void frontPanelSending_sendPriorityTriplet(uint8_t status, uint8_t data1, uint8_t data2);
void frontPanelSending_sendPriorityTripletWait(uint8_t status, uint8_t data1, uint8_t data2);
void frontPanelSending_sendCallbackAck(void);
void frontPanelSending_sendSampleUploadAck(void);
void frontPanelSending_sendSampleCountReply(void);
void frontPanelSending_sendTrackLengthReply(uint8_t trackNr);
void frontPanelSending_sendTrackRotationReply(uint8_t trackNr);
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

void frontPanelSending_updateTrackLeds(uint8_t trackNr,
                                      uint8_t patternNr,
                                      uint8_t activeStep);
void frontPanelSending_updateSubStepLeds(uint8_t trackNr,
                                        uint8_t patternNr,
                                        uint8_t activeStep);

void frontPanelSending_sendFlowGrant(uint8_t channel, uint8_t credits);
void frontPanelSending_sendFlowGrantWait(uint8_t channel, uint8_t credits);
void frontPanelSending_sendFlowAbort(uint8_t channel);
void frontPanelSending_sendPrfCacheStatus(uint8_t command, uint8_t status);

#endif /* UARTFRONTSYX_FRONTPANELSENDINGPROTOCOL_H_ */
