/*
 * avrCommsPanelParser.h
 *
 * AVR comms panel parser helpers.
 */

#ifndef AVRCOMMSPANELPARSER_H_
#define AVRCOMMSPANELPARSER_H_

#include "avrCommsReceivingProtocol.h"
#include "avrCommsSendingProtocol.h"

void avrCommsParser_parseNrpn(uint8_t value);
void avrCommsPanelParser_ccHandler(void);

#endif /* AVRCOMMSPANELPARSER_H_ */
