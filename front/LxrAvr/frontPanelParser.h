/*
 * frontPanelParser.h
 *
 * Front-panel parser and helper declarations.
 */

#ifndef FRONTPANELPARSER_H_
#define FRONTPANELPARSER_H_

#include "frontPanelReceivingProtocol.h"

void frontParser_parseNrpn(uint8_t value);
void frontPanel_ccHandler(void);
void frontPanel_parseData(uint8_t data);
void frontPanel_sendMidiMsg(MidiMsg msg);
void frontPanel_sendData(uint8_t status, uint8_t data1, uint8_t data2);
void frontPanel_sendByte(uint8_t data);

#endif /* FRONTPANELPARSER_H_ */
