/*
 * EndpointRestore.h
 *
 * Preset owns the queued AVR endpoint restore protocol so the sequencer can
 * ask for menu synchronization without embedding the UART push-up state
 * machine in the real-time loop.
 */

#ifndef PRESET_ENDPOINTRESTORE_H_
#define PRESET_ENDPOINTRESTORE_H_

#include "Preset/KitState.h"

/* The AVR restore handshake uses these two flags as a small shared mailbox.
   The parser raises them when the AVR is ready/acknowledged and the restore
   service consumes them while sending the staged endpoint bytes. */
extern volatile uint8_t preset_tmpKitHandshakeReady;
extern volatile uint8_t preset_tmpKitHandshakeAck;

/* Requests a restore-service pass. The restore queue lives in the Preset
   module so the sequencer only schedules work and does not own the queue
   policy. */
void preset_serviceEndpointRestore(void);

/* Reports whether the restore queue or the active restore phase still has work
   in flight. */
uint8_t preset_endpointRestoreBusy(void);

/* Schedules a restore of the affected endpoint subset after a voice-source
   change. The restore service uses this to keep the AVR menu in sync when the
   temp/normal boundary changes. */
void preset_pushEndpointUpdateForVoiceSourceChange(uint8_t changedVoiceMask);

/* Schedules a full-kit restore push for the supplied image, with the global
   morph report preserved when the current temp boundary needs to be mirrored
   on the AVR menu. */
void preset_maybePushKitEndpointsToFrontWithGlobalMorphReport(const PresetKitState *kit);

#endif /* PRESET_ENDPOINTRESTORE_H_ */
