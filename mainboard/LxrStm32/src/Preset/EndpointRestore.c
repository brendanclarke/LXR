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
#include "uARTFrontSYX/Uart.h"
#include "Sequencer/sequencer.h"

#define SEQ_ENDPOINT_RESTORE_NONE 0
#define SEQ_ENDPOINT_RESTORE_FULL 1
#define SEQ_ENDPOINT_RESTORE_MASK 2
#define SEQ_ENDPOINT_RESTORE_QUEUE_LENGTH 4
#define SEQ_ENDPOINT_RESTORE_PHASE_IDLE 0
#define SEQ_ENDPOINT_RESTORE_PHASE_WAIT_READY 1
#define SEQ_ENDPOINT_RESTORE_PHASE_SEND_FRONT 2
#define SEQ_ENDPOINT_RESTORE_PHASE_SEND_MORPH 3
#define SEQ_ENDPOINT_RESTORE_PHASE_WAIT_ACK 4
#define SEQ_ENDPOINT_RESTORE_WAIT_TIMEOUT 30000

typedef struct SeqEndpointRestoreRequestStruct
{
   const SeqKitState *kit;
   uint8_t mode;
   uint8_t voiceMask;
   uint8_t reportGlobalMorph;
} SeqEndpointRestoreRequest;

volatile uint8_t preset_tmpKitHandshakeReady = 0;
volatile uint8_t preset_tmpKitHandshakeAck = 0;

static uint8_t seq_tmpKitPushParamsToFrontEnabled = 1;
static SeqEndpointRestoreRequest seq_endpointRestoreQueue[SEQ_ENDPOINT_RESTORE_QUEUE_LENGTH];
static uint8_t seq_endpointRestoreQueueHead = 0;
static uint8_t seq_endpointRestoreQueueCount = 0;
static SeqEndpointRestoreRequest seq_endpointRestoreCurrent;
static uint8_t seq_endpointRestorePhase = SEQ_ENDPOINT_RESTORE_PHASE_IDLE;
static uint16_t seq_endpointRestoreParamCursor = 0;
static uint8_t seq_endpointRestoreVoiceCursor = 0;
static uint8_t seq_endpointRestoreVoiceParamCursor = 0;
static uint16_t seq_endpointRestoreWaitCounter = 0;

/* Writes a single endpoint byte to the AVR front panel, using the restore
   packet family that matches the raw parameter index. */
static void seq_pushSingleParameterToFront(uint16_t param, uint8_t value)
{
   if(param >= END_OF_SOUND_PARAMETERS)
      return;

   if(param < 128)
   {
      uart_sendFrontpanelPriorityByteWait(PRF_RESTORE_PARAM_CC);
      uart_sendFrontpanelPriorityByteWait((uint8_t)param);
   }
   else
   {
      uart_sendFrontpanelPriorityByteWait(PRF_RESTORE_PARAM_CC2);
      uart_sendFrontpanelPriorityByteWait((uint8_t)(param - 128));
   }

   uart_sendFrontpanelPriorityByteWait(value);
}

/* Writes a single morph endpoint byte to the AVR front panel. Morph endpoint
   pushes are separated from normal endpoint pushes so restore traffic can keep
   the menu state coherent while leaving the morph image intact. */
static void seq_pushSingleMorphParameterToFront(uint16_t param, uint8_t value)
{
   if(param >= END_OF_SOUND_PARAMETERS)
      return;

   if(param < 128)
   {
      uart_sendFrontpanelPriorityByteWait(PRF_RESTORE_MORPH_CC);
      uart_sendFrontpanelPriorityByteWait((uint8_t)param);
   }
   else
   {
      uart_sendFrontpanelPriorityByteWait(PRF_RESTORE_MORPH_CC2);
      uart_sendFrontpanelPriorityByteWait((uint8_t)(param - 128));
   }

   uart_sendFrontpanelPriorityByteWait(value);
}

/* Reports the global morph amount to the AVR so the display can reflect the
   current kit image when the temp/normal boundary changes. */
static void seq_pushGlobalMorphToFront(const SeqKitState *kit)
{
   uint8_t amount;

   if(!kit)
      return;

   amount = kit->globalMorphAmount;

   uart_sendFrontpanelPriorityByteWait(FRONT_SEQ_CC);
   uart_sendFrontpanelPriorityByteWait(FRONT_SEQ_REPORT_GLOBAL_MORPH_LSB);
   uart_sendFrontpanelPriorityByteWait((uint8_t)(amount & 0x7f));

   uart_sendFrontpanelPriorityByteWait(FRONT_SEQ_CC);
   uart_sendFrontpanelPriorityByteWait(FRONT_SEQ_REPORT_GLOBAL_MORPH_MSB);
   uart_sendFrontpanelPriorityByteWait((uint8_t)((amount >> 7) & 0x01));
}

/* Enqueues a restore request. When a request for the same kit/mode is already
   at the tail of the queue, the voice masks are merged so repeated callers do
   not create redundant restore traffic. */
static void seq_pushKitEndpointVoiceMaskToFrontInternal(const SeqKitState *kit,
                                                        uint8_t voiceMask,
                                                        uint8_t reportGlobalMorph)
{
   SeqEndpointRestoreRequest request;
   uint8_t tail;
   uint8_t last;

   if(!kit || !voiceMask)
      return;

   request.kit = kit;
   request.mode = (voiceMask == 0xff) ? SEQ_ENDPOINT_RESTORE_FULL
                                      : SEQ_ENDPOINT_RESTORE_MASK;
   request.voiceMask = voiceMask;
   request.reportGlobalMorph = reportGlobalMorph;

   if(seq_endpointRestoreQueueCount)
   {
      last = (uint8_t)((seq_endpointRestoreQueueHead +
                        seq_endpointRestoreQueueCount - 1) %
                       SEQ_ENDPOINT_RESTORE_QUEUE_LENGTH);

      if(seq_endpointRestoreQueue[last].kit == request.kit &&
         seq_endpointRestoreQueue[last].mode == request.mode)
      {
         if(request.mode == SEQ_ENDPOINT_RESTORE_MASK)
            seq_endpointRestoreQueue[last].voiceMask |= request.voiceMask;
         if(request.reportGlobalMorph)
            seq_endpointRestoreQueue[last].reportGlobalMorph = 1;
         return;
      }
   }

   if(seq_endpointRestoreQueueCount >= SEQ_ENDPOINT_RESTORE_QUEUE_LENGTH)
   {
      last = (uint8_t)((seq_endpointRestoreQueueHead +
                        seq_endpointRestoreQueueCount - 1) %
                       SEQ_ENDPOINT_RESTORE_QUEUE_LENGTH);
      seq_endpointRestoreQueue[last] = request;
      return;
   }

   tail = (uint8_t)((seq_endpointRestoreQueueHead + seq_endpointRestoreQueueCount) %
                    SEQ_ENDPOINT_RESTORE_QUEUE_LENGTH);
   seq_endpointRestoreQueue[tail] = request;
   seq_endpointRestoreQueueCount++;
}

static void seq_pushKitEndpointsToFront(const SeqKitState *kit)
{
   if(!kit)
      return;

   seq_pushKitEndpointVoiceMaskToFrontInternal(kit, 0xff, 0);
}

static void seq_pushKitEndpointsToFrontWithGlobalMorphReport(const SeqKitState *kit)
{
   if(!kit)
      return;

   seq_pushKitEndpointVoiceMaskToFrontInternal(kit, 0xff, 1);
}

static void seq_pushKitEndpointVoiceMaskToFront(const SeqKitState *kit,
                                                uint8_t voiceMask)
{
   seq_pushKitEndpointVoiceMaskToFrontInternal(kit, voiceMask, 0);
}

static uint8_t seq_endpointRestorePopRequest(void)
{
   if(!seq_endpointRestoreQueueCount)
      return 0;

   seq_endpointRestoreCurrent = seq_endpointRestoreQueue[seq_endpointRestoreQueueHead];
   seq_endpointRestoreQueueHead =
      (uint8_t)((seq_endpointRestoreQueueHead + 1) % SEQ_ENDPOINT_RESTORE_QUEUE_LENGTH);
   seq_endpointRestoreQueueCount--;

   seq_endpointRestoreParamCursor = 0;
   seq_endpointRestoreVoiceCursor = 0;
   seq_endpointRestoreVoiceParamCursor = 0;
   seq_endpointRestoreWaitCounter = 0;
   return 1;
}

static void seq_endpointRestoreClearCurrent(void)
{
   seq_endpointRestoreCurrent.kit = 0;
   seq_endpointRestoreCurrent.mode = SEQ_ENDPOINT_RESTORE_NONE;
   seq_endpointRestoreCurrent.voiceMask = 0;
   seq_endpointRestoreCurrent.reportGlobalMorph = 0;
   seq_endpointRestoreParamCursor = 0;
   seq_endpointRestoreVoiceCursor = 0;
   seq_endpointRestoreVoiceParamCursor = 0;
   seq_endpointRestoreWaitCounter = 0;
   seq_endpointRestorePhase = SEQ_ENDPOINT_RESTORE_PHASE_IDLE;
}

static uint8_t seq_endpointRestoreWaitTimedOut(void)
{
   if(seq_endpointRestoreWaitCounter < SEQ_ENDPOINT_RESTORE_WAIT_TIMEOUT)
   {
      seq_endpointRestoreWaitCounter++;
      return 0;
   }

   return 1;
}

static uint8_t seq_endpointRestoreSendNextFull(uint8_t morphEndpoint)
{
   uint16_t param;
   const SeqKitState *kit = seq_endpointRestoreCurrent.kit;
   const uint8_t *values = morphEndpoint ? kit->morphEndpointParams
                                         : kit->kitEndpointParams;

   param = seq_endpointRestoreParamCursor;
   if(param < END_OF_SOUND_PARAMETERS)
   {
      if(morphEndpoint)
         seq_pushSingleMorphParameterToFront(param, values[param]);
      else
         seq_pushSingleParameterToFront(param, values[param]);

      seq_endpointRestoreParamCursor = (uint16_t)(param + 1);
      return 1;
   }

   return 0;
}

static uint8_t seq_endpointRestoreSendNextMasked(uint8_t morphEndpoint)
{
   const SeqKitState *kit = seq_endpointRestoreCurrent.kit;
   const uint8_t *values = morphEndpoint ? kit->morphEndpointParams
                                         : kit->kitEndpointParams;

   while(seq_endpointRestoreVoiceCursor < SEQ_SYNTH_VOICES)
   {
      uint8_t synthVoice = seq_endpointRestoreVoiceCursor;

      if(!(seq_endpointRestoreCurrent.voiceMask & (uint8_t)(1 << synthVoice)))
      {
         seq_endpointRestoreVoiceCursor++;
         seq_endpointRestoreVoiceParamCursor = 0;
         continue;
      }

      while(seq_endpointRestoreVoiceParamCursor < SEQ_VOICE_PARAM_LENGTH)
      {
         uint16_t param =
            seq_canonicalParamFromVoiceMask(
               seq_voiceParamMask[synthVoice][seq_endpointRestoreVoiceParamCursor]);

         seq_endpointRestoreVoiceParamCursor++;

         if(param < END_OF_SOUND_PARAMETERS)
         {
            if(morphEndpoint)
               seq_pushSingleMorphParameterToFront(param, values[param]);
            else
               seq_pushSingleParameterToFront(param, values[param]);

            return 1;
         }
      }

      seq_endpointRestoreVoiceCursor++;
      seq_endpointRestoreVoiceParamCursor = 0;
   }

   return 0;
}

static uint8_t seq_endpointRestoreSendNext(uint8_t morphEndpoint)
{
   if(!seq_endpointRestoreCurrent.kit)
      return 0;

   if(seq_endpointRestoreCurrent.mode == SEQ_ENDPOINT_RESTORE_FULL)
      return seq_endpointRestoreSendNextFull(morphEndpoint);

   return seq_endpointRestoreSendNextMasked(morphEndpoint);
}

uint8_t seq_endpointRestoreBusy(void)
{
   return (uint8_t)((seq_endpointRestorePhase != SEQ_ENDPOINT_RESTORE_PHASE_IDLE) ||
                    (seq_endpointRestoreQueueCount != 0));
}

void seq_serviceEndpointRestore(void)
{
   switch(seq_endpointRestorePhase)
   {
      case SEQ_ENDPOINT_RESTORE_PHASE_IDLE:
         if(!seq_endpointRestorePopRequest())
            return;

         preset_tmpKitHandshakeReady = 0;
         preset_tmpKitHandshakeAck = 0;
         uart_sendFrontpanelPriorityByteWait(PARAM_RESTORE_BEGIN);
         uart_sendFrontpanelPriorityByteWait(0);
         uart_sendFrontpanelPriorityByteWait(0);
         seq_endpointRestoreWaitCounter = 0;
         seq_endpointRestorePhase = SEQ_ENDPOINT_RESTORE_PHASE_WAIT_READY;
         return;

      case SEQ_ENDPOINT_RESTORE_PHASE_WAIT_READY:
         if(preset_tmpKitHandshakeReady)
         {
            seq_endpointRestoreParamCursor = 0;
            seq_endpointRestoreVoiceCursor = 0;
            seq_endpointRestoreVoiceParamCursor = 0;
            seq_endpointRestoreWaitCounter = 0;
            seq_endpointRestorePhase = SEQ_ENDPOINT_RESTORE_PHASE_SEND_FRONT;
         }
         else if(seq_endpointRestoreWaitTimedOut())
         {
            uart_sendFrontpanelPriorityByteWait(PARAM_RESTORE_DONE);
            uart_sendFrontpanelPriorityByteWait(0);
            uart_sendFrontpanelPriorityByteWait(0);
            seq_endpointRestoreClearCurrent();
         }
         return;

      case SEQ_ENDPOINT_RESTORE_PHASE_SEND_FRONT:
         if(seq_endpointRestoreSendNext(0))
            return;

         seq_endpointRestoreParamCursor = 0;
         seq_endpointRestoreVoiceCursor = 0;
         seq_endpointRestoreVoiceParamCursor = 0;
         seq_endpointRestorePhase = SEQ_ENDPOINT_RESTORE_PHASE_SEND_MORPH;
         return;

      case SEQ_ENDPOINT_RESTORE_PHASE_SEND_MORPH:
         if(seq_endpointRestoreSendNext(1))
            return;

         preset_tmpKitHandshakeAck = 0;
         if(seq_endpointRestoreCurrent.reportGlobalMorph)
            seq_pushGlobalMorphToFront(seq_endpointRestoreCurrent.kit);
         uart_sendFrontpanelPriorityByteWait(PARAM_RESTORE_DONE);
         uart_sendFrontpanelPriorityByteWait(0);
         uart_sendFrontpanelPriorityByteWait(0);
         seq_endpointRestoreWaitCounter = 0;
         seq_endpointRestorePhase = SEQ_ENDPOINT_RESTORE_PHASE_WAIT_ACK;
         return;

      case SEQ_ENDPOINT_RESTORE_PHASE_WAIT_ACK:
         if(preset_tmpKitHandshakeAck)
         {
            seq_endpointRestoreClearCurrent();
         }
         else if(seq_endpointRestoreWaitTimedOut())
         {
            seq_endpointRestoreClearCurrent();
         }
         return;

      default:
         seq_endpointRestoreClearCurrent();
         return;
   }
}

void seq_pushEndpointUpdateForVoiceSourceChange(uint8_t changedVoiceMask)
{
   uint8_t synthVoice;
   uint8_t normalVoiceMask = 0;
   uint8_t tmpVoiceMask = 0;

   if(!seq_tmpKitPushParamsToFrontEnabled || !changedVoiceMask)
      return;

   if(seq_allVoiceSourcesUseTmp())
   {
      if(!seq_tmpKitState.valid)
         preset_captureTmpKitState();

      seq_pushKitEndpointsToFront(&seq_tmpKitState);
      return;
   }

   if(seq_allVoiceSourcesUseNormal())
   {
      seq_pushKitEndpointsToFront(&seq_normalKitState);
      return;
   }

   for(synthVoice=0;synthVoice<SEQ_SYNTH_VOICES;synthVoice++)
   {
      uint8_t bit = (uint8_t)(1 << synthVoice);

      if(!(changedVoiceMask & bit))
         continue;

      if(seq_voiceSourceState[synthVoice] == SEQ_VOICE_SOURCE_TMP)
         tmpVoiceMask |= bit;
      else
         normalVoiceMask |= bit;
   }

   if(tmpVoiceMask)
   {
      if(!seq_tmpKitState.valid)
         preset_captureTmpKitState();

      seq_pushKitEndpointVoiceMaskToFront(&seq_tmpKitState, tmpVoiceMask);
   }

   if(normalVoiceMask)
      seq_pushKitEndpointVoiceMaskToFront(&seq_normalKitState, normalVoiceMask);
}

void seq_maybePushKitEndpointsToFrontWithGlobalMorphReport(const SeqKitState *kit)
{
   if(!seq_tmpKitPushParamsToFrontEnabled)
      return;

   seq_pushKitEndpointsToFrontWithGlobalMorphReport(kit);
}
