/*
 * EndpointRestore.c
 *
 * Preset owns the queued AVR endpoint restore protocol. The sequencer can
 * request restore work, but the queue, handshake state, and UART push-up
 * policy live here so normal runtime code does not have to own AVR traffic.
 */

#include "Preset/EndpointRestore.h"
#include "Preset/TempPlaybackSwitch.h"
#include "MidiMessages.h"
#include "uARTFrontSYX/frontPanelSendingProtocol.h"
#include "Sequencer/sequencer.h"

#define PRESET_ENDPOINT_RESTORE_NONE 0
#define PRESET_ENDPOINT_RESTORE_FULL 1
#define PRESET_ENDPOINT_RESTORE_MASK 2
#define PRESET_ENDPOINT_RESTORE_QUEUE_LENGTH 4
#define PRESET_ENDPOINT_RESTORE_PHASE_IDLE 0
#define PRESET_ENDPOINT_RESTORE_PHASE_WAIT_READY 1
#define PRESET_ENDPOINT_RESTORE_PHASE_SEND_FRONT 2
#define PRESET_ENDPOINT_RESTORE_PHASE_SEND_MORPH 3
#define PRESET_ENDPOINT_RESTORE_PHASE_WAIT_ACK 4
#define PRESET_ENDPOINT_RESTORE_WAIT_TIMEOUT 30000

typedef struct PresetEndpointRestoreRequestStruct
{
   const PresetKitState *kit;
   uint8_t mode;
   uint8_t voiceMask;
   uint8_t reportGlobalMorph;
} PresetEndpointRestoreRequest;

volatile uint8_t preset_tmpKitHandshakeReady = 0;
volatile uint8_t preset_tmpKitHandshakeAck = 0;

static uint8_t preset_tmpKitPushParamsToFrontEnabled = 1;
static PresetEndpointRestoreRequest preset_endpointRestoreQueue[PRESET_ENDPOINT_RESTORE_QUEUE_LENGTH];
static uint8_t preset_endpointRestoreQueueHead = 0;
static uint8_t preset_endpointRestoreQueueCount = 0;
static PresetEndpointRestoreRequest preset_endpointRestoreCurrent;
static uint8_t preset_endpointRestorePhase = PRESET_ENDPOINT_RESTORE_PHASE_IDLE;
static uint16_t preset_endpointRestoreParamCursor = 0;
static uint8_t preset_endpointRestoreVoiceCursor = 0;
static uint8_t preset_endpointRestoreVoiceParamCursor = 0;
static uint16_t preset_endpointRestoreWaitCounter = 0;

/* Enqueues a restore request. When a request for the same kit/mode is already
   at the tail of the queue, the voice masks are merged so repeated callers do
   not create redundant restore traffic. */
static void preset_pushKitEndpointVoiceMaskToFrontInternal(const PresetKitState *kit,
                                                        uint8_t voiceMask,
                                                        uint8_t reportGlobalMorph)
{
   PresetEndpointRestoreRequest request;
   uint8_t tail;
   uint8_t last;

   if(!kit || !voiceMask)
      return;

   request.kit = kit;
   request.mode = (voiceMask == 0xff) ? PRESET_ENDPOINT_RESTORE_FULL
                                      : PRESET_ENDPOINT_RESTORE_MASK;
   request.voiceMask = voiceMask;
   request.reportGlobalMorph = reportGlobalMorph;

   if(preset_endpointRestoreQueueCount)
   {
      last = (uint8_t)((preset_endpointRestoreQueueHead +
                        preset_endpointRestoreQueueCount - 1) %
                       PRESET_ENDPOINT_RESTORE_QUEUE_LENGTH);

      if(preset_endpointRestoreQueue[last].kit == request.kit &&
         preset_endpointRestoreQueue[last].mode == request.mode)
      {
         if(request.mode == PRESET_ENDPOINT_RESTORE_MASK)
            preset_endpointRestoreQueue[last].voiceMask |= request.voiceMask;
         if(request.reportGlobalMorph)
            preset_endpointRestoreQueue[last].reportGlobalMorph = 1;
         return;
      }
   }

   if(preset_endpointRestoreQueueCount >= PRESET_ENDPOINT_RESTORE_QUEUE_LENGTH)
   {
      last = (uint8_t)((preset_endpointRestoreQueueHead +
                        preset_endpointRestoreQueueCount - 1) %
                       PRESET_ENDPOINT_RESTORE_QUEUE_LENGTH);
      preset_endpointRestoreQueue[last] = request;
      return;
   }

   tail = (uint8_t)((preset_endpointRestoreQueueHead + preset_endpointRestoreQueueCount) %
                    PRESET_ENDPOINT_RESTORE_QUEUE_LENGTH);
   preset_endpointRestoreQueue[tail] = request;
   preset_endpointRestoreQueueCount++;
}

static void preset_pushKitEndpointsToFront(const PresetKitState *kit)
{
   if(!kit)
      return;

   preset_pushKitEndpointVoiceMaskToFrontInternal(kit, 0xff, 0);
}

static void preset_pushKitEndpointsToFrontWithGlobalMorphReport(const PresetKitState *kit)
{
   if(!kit)
      return;

   preset_pushKitEndpointVoiceMaskToFrontInternal(kit, 0xff, 1);
}

static void preset_pushKitEndpointVoiceMaskToFront(const PresetKitState *kit,
                                                uint8_t voiceMask)
{
   preset_pushKitEndpointVoiceMaskToFrontInternal(kit, voiceMask, 0);
}

static uint8_t preset_endpointRestorePopRequest(void)
{
   if(!preset_endpointRestoreQueueCount)
      return 0;

   preset_endpointRestoreCurrent = preset_endpointRestoreQueue[preset_endpointRestoreQueueHead];
   preset_endpointRestoreQueueHead =
      (uint8_t)((preset_endpointRestoreQueueHead + 1) % PRESET_ENDPOINT_RESTORE_QUEUE_LENGTH);
   preset_endpointRestoreQueueCount--;

   preset_endpointRestoreParamCursor = 0;
   preset_endpointRestoreVoiceCursor = 0;
   preset_endpointRestoreVoiceParamCursor = 0;
   preset_endpointRestoreWaitCounter = 0;
   return 1;
}

static void preset_endpointRestoreClearCurrent(void)
{
   preset_endpointRestoreCurrent.kit = 0;
   preset_endpointRestoreCurrent.mode = PRESET_ENDPOINT_RESTORE_NONE;
   preset_endpointRestoreCurrent.voiceMask = 0;
   preset_endpointRestoreCurrent.reportGlobalMorph = 0;
   preset_endpointRestoreParamCursor = 0;
   preset_endpointRestoreVoiceCursor = 0;
   preset_endpointRestoreVoiceParamCursor = 0;
   preset_endpointRestoreWaitCounter = 0;
   preset_endpointRestorePhase = PRESET_ENDPOINT_RESTORE_PHASE_IDLE;
}

static uint8_t preset_endpointRestoreWaitTimedOut(void)
{
   if(preset_endpointRestoreWaitCounter < PRESET_ENDPOINT_RESTORE_WAIT_TIMEOUT)
   {
      preset_endpointRestoreWaitCounter++;
      return 0;
   }

   return 1;
}

static uint8_t preset_endpointRestoreSendNextFull(uint8_t morphEndpoint)
{
   uint16_t param;
   const PresetKitState *kit = preset_endpointRestoreCurrent.kit;
   const uint8_t *values = morphEndpoint ? kit->morphEndpointParams
                                         : kit->kitEndpointParams;

      param = preset_endpointRestoreParamCursor;
      if(param < END_OF_SOUND_PARAMETERS)
      {
         if(morphEndpoint)
            frontPanelSending_sendRestoreMorphParam(param, values[param]);
         else
            frontPanelSending_sendRestoreParam(param, values[param]);

      preset_endpointRestoreParamCursor = (uint16_t)(param + 1);
      return 1;
   }

   return 0;
}

static uint8_t preset_endpointRestoreSendNextMasked(uint8_t morphEndpoint)
{
   const PresetKitState *kit = preset_endpointRestoreCurrent.kit;
   const uint8_t *values = morphEndpoint ? kit->morphEndpointParams
                                         : kit->kitEndpointParams;

   while(preset_endpointRestoreVoiceCursor < SEQ_SYNTH_VOICES)
   {
      uint8_t synthVoice = preset_endpointRestoreVoiceCursor;

      if(!(preset_endpointRestoreCurrent.voiceMask & (uint8_t)(1 << synthVoice)))
      {
         preset_endpointRestoreVoiceCursor++;
         preset_endpointRestoreVoiceParamCursor = 0;
         continue;
      }

      while(preset_endpointRestoreVoiceParamCursor < SEQ_VOICE_PARAM_LENGTH)
      {
         uint16_t param =
            preset_canonicalParamFromVoiceMask(
               preset_voiceParamMask[synthVoice][preset_endpointRestoreVoiceParamCursor]);

         preset_endpointRestoreVoiceParamCursor++;

         if(param < END_OF_SOUND_PARAMETERS)
         {
            if(morphEndpoint)
               frontPanelSending_sendRestoreMorphParam(param, values[param]);
            else
               frontPanelSending_sendRestoreParam(param, values[param]);

            return 1;
         }
      }

      preset_endpointRestoreVoiceCursor++;
      preset_endpointRestoreVoiceParamCursor = 0;
   }

   return 0;
}

static uint8_t preset_endpointRestoreSendNext(uint8_t morphEndpoint)
{
   if(!preset_endpointRestoreCurrent.kit)
      return 0;

   if(preset_endpointRestoreCurrent.mode == PRESET_ENDPOINT_RESTORE_FULL)
      return preset_endpointRestoreSendNextFull(morphEndpoint);

   return preset_endpointRestoreSendNextMasked(morphEndpoint);
}

uint8_t preset_endpointRestoreBusy(void)
{
   return (uint8_t)((preset_endpointRestorePhase != PRESET_ENDPOINT_RESTORE_PHASE_IDLE) ||
                    (preset_endpointRestoreQueueCount != 0));
}

void preset_serviceEndpointRestore(void)
{
   switch(preset_endpointRestorePhase)
   {
      case PRESET_ENDPOINT_RESTORE_PHASE_IDLE:
         if(!preset_endpointRestorePopRequest())
            return;

         preset_tmpKitHandshakeReady = 0;
         preset_tmpKitHandshakeAck = 0;
         frontPanelSending_sendRestoreBegin();
         preset_endpointRestoreWaitCounter = 0;
         preset_endpointRestorePhase = PRESET_ENDPOINT_RESTORE_PHASE_WAIT_READY;
         return;

      case PRESET_ENDPOINT_RESTORE_PHASE_WAIT_READY:
         if(preset_tmpKitHandshakeReady)
         {
            preset_endpointRestoreParamCursor = 0;
            preset_endpointRestoreVoiceCursor = 0;
            preset_endpointRestoreVoiceParamCursor = 0;
            preset_endpointRestoreWaitCounter = 0;
            preset_endpointRestorePhase = PRESET_ENDPOINT_RESTORE_PHASE_SEND_FRONT;
         }
         else if(preset_endpointRestoreWaitTimedOut())
         {
            frontPanelSending_sendRestoreDone();
            preset_endpointRestoreClearCurrent();
         }
         return;

      case PRESET_ENDPOINT_RESTORE_PHASE_SEND_FRONT:
         if(preset_endpointRestoreSendNext(0))
            return;

         preset_endpointRestoreParamCursor = 0;
         preset_endpointRestoreVoiceCursor = 0;
         preset_endpointRestoreVoiceParamCursor = 0;
         preset_endpointRestorePhase = PRESET_ENDPOINT_RESTORE_PHASE_SEND_MORPH;
         return;

      case PRESET_ENDPOINT_RESTORE_PHASE_SEND_MORPH:
         if(preset_endpointRestoreSendNext(1))
            return;

         preset_tmpKitHandshakeAck = 0;
         if(preset_endpointRestoreCurrent.reportGlobalMorph)
            frontPanelSending_sendGlobalMorphReport(preset_endpointRestoreCurrent.kit->globalMorphAmount);
         frontPanelSending_sendRestoreDone();
         preset_endpointRestoreWaitCounter = 0;
         preset_endpointRestorePhase = PRESET_ENDPOINT_RESTORE_PHASE_WAIT_ACK;
         return;

      case PRESET_ENDPOINT_RESTORE_PHASE_WAIT_ACK:
         if(preset_tmpKitHandshakeAck)
         {
            preset_endpointRestoreClearCurrent();
         }
         else if(preset_endpointRestoreWaitTimedOut())
         {
            preset_endpointRestoreClearCurrent();
         }
         return;

      default:
         preset_endpointRestoreClearCurrent();
         return;
   }
}

void preset_pushEndpointUpdateForVoiceSourceChange(uint8_t changedVoiceMask)
{
   uint8_t synthVoice;
   uint8_t normalVoiceMask = 0;
   uint8_t tmpVoiceMask = 0;

   if(!preset_tmpKitPushParamsToFrontEnabled || !changedVoiceMask)
      return;

   if(preset_allVoiceSourcesUseTmp())
   {
      if(!preset_tmpKitState.valid)
         preset_captureTmpKitState();

      preset_pushKitEndpointsToFront(&preset_tmpKitState);
      return;
   }

   if(preset_allVoiceSourcesUseNormal())
   {
      preset_pushKitEndpointsToFront(&preset_normalKitState);
      return;
   }

   for(synthVoice=0;synthVoice<SEQ_SYNTH_VOICES;synthVoice++)
   {
      uint8_t bit = (uint8_t)(1 << synthVoice);

      if(!(changedVoiceMask & bit))
         continue;

      if(preset_voiceSourceState[synthVoice] == PRESET_VOICE_SOURCE_TMP)
         tmpVoiceMask |= bit;
      else
         normalVoiceMask |= bit;
   }

   if(tmpVoiceMask)
   {
      if(!preset_tmpKitState.valid)
         preset_captureTmpKitState();

      preset_pushKitEndpointVoiceMaskToFront(&preset_tmpKitState, tmpVoiceMask);
   }

   if(normalVoiceMask)
      preset_pushKitEndpointVoiceMaskToFront(&preset_normalKitState, normalVoiceMask);
}

void preset_maybePushKitEndpointsToFrontWithGlobalMorphReport(const PresetKitState *kit)
{
   if(!preset_tmpKitPushParamsToFrontEnabled)
      return;

   preset_pushKitEndpointsToFrontWithGlobalMorphReport(kit);
}
