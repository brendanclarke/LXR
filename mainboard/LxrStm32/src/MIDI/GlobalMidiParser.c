/*
 * GlobalMidiParser.c
 *
 * MIDI-wide system handling that does not belong to the per-channel parser.
 */

#include "GlobalMidiParser.h"

#include "ChannelMidiParser.h"
#include "MidiParser.h"
#include "Preset/ParameterIngress.h"
#include "clockSync.h"
#include "globals.h"
#include "mixer.h"
#include "modulationNode.h"
#include "sequencer.h"
#include "uARTFrontSYX/frontPanelReceivingProtocol.h"
#include "uARTFrontSYX/frontPanelSendingProtocol.h"
#include "usb_manager.h"
#include "valueShaper.h"

static inline uint8_t midiParser_voiceMidiChannel(uint8_t voice)
{
   /* Local voice-to-MIDI-channel lookup used by the global CC ladder. */
   return (voice < 8) ? midi_MidiChannels[voice] : 0;
}

// this will be set to some value if we are ignoring all mtc messages until the next 0 message
static uint8_t midiParser_mtcIgnore = 1;
static volatile uint32_t midiParser_lastMtcReceived = 0x0;
static uint8_t midiParser_mtcIsRunning = 0;
static uint16_t globalMidiParser_activeNrpnNumber = 0;
static uint8_t globalMidiParser_nrpnSelected = 0;

#define GLOBAL_MIDI_UNMAPPED I_DUNNO

static const uint16_t globalMidiParser_ccToLxrParam[128] = {
   [2] = OSC_WAVE_DRUM1,
   [3] = OSC_WAVE_DRUM2,
   [4] = OSC_WAVE_DRUM3,
   [5] = OSC_WAVE_SNARE,
   [7] = CYM_WAVE1,
   [8] = WAVE1_HH,
   [9] = F_OSC1_COARSE,
   [10] = F_OSC1_FINE,
   [11] = F_OSC2_COARSE,
   [12] = F_OSC2_FINE,
   [13] = F_OSC3_COARSE,
   [14] = F_OSC3_FINE,
   [15] = F_OSC4_COARSE,
   [16] = F_OSC4_FINE,
   [17] = F_OSC5_COARSE,
   [18] = F_OSC5_FINE,
   [19] = F_OSC6_COARSE,
   [20] = F_OSC6_FINE,
   [21] = MOD_WAVE_DRUM1,
   [22] = MOD_WAVE_DRUM2,
   [23] = MOD_WAVE_DRUM3,
   [24] = CYM_WAVE2,
   [25] = CYM_WAVE3,
   [26] = WAVE2_HH,
   [27] = WAVE3_HH,
   [28] = SNARE_NOISE_F,
   [29] = SNARE_MIX,
   [30] = CYM_MOD_OSC_F1,
   [31] = CYM_MOD_OSC_F2,
   [32] = CYM_MOD_OSC_GAIN1,
   [33] = CYM_MOD_OSC_GAIN2,
   [34] = MOD_OSC_F1,
   [35] = MOD_OSC_F2,
   [36] = MOD_OSC_GAIN1,
   [37] = MOD_OSC_GAIN2,
   [38] = FILTER_FREQ_DRUM1,
   [39] = FILTER_FREQ_DRUM2,
   [40] = FILTER_FREQ_DRUM3,
   [41] = SNARE_FILTER_F,
   [42] = CYM_FIL_FREQ,
   [43] = HAT_FILTER_F,
   [44] = RESO_DRUM1,
   [45] = RESO_DRUM2,
   [46] = RESO_DRUM3,
   [47] = SNARE_RESO,
   [48] = CYM_RESO,
   [49] = HAT_RESO,
   [50] = VELOA1,
   [51] = VELOD1,
   [52] = VELOA2,
   [53] = VELOD2,
   [54] = VELOA3,
   [55] = VELOD3,
   [56] = VELOA4,
   [57] = VELOD4,
   [58] = VELOA5,
   [59] = VELOD5,
   [60] = VELOA6,
   [61] = VELOD6,
   [62] = VELOD6_OPEN,
   [63] = VOL_SLOPE1,
   [64] = VOL_SLOPE2,
   [65] = VOL_SLOPE3,
   [66] = EG_SNARE1_SLOPE,
   [67] = CYM_SLOPE,
   [68] = VOL_SLOPE6,
   [69] = REPEAT1,
   [70] = CYM_REPEAT,
   [71] = PITCHD1,
   [72] = PITCHD2,
   [73] = PITCHD3,
   [74] = PITCHD4,
   [75] = MODAMNT1,
   [76] = MODAMNT2,
   [77] = MODAMNT3,
   [78] = MODAMNT4,
   [79] = PITCH_SLOPE1,
   [80] = PITCH_SLOPE2,
   [81] = PITCH_SLOPE3,
   [82] = PITCH_SLOPE4,
   [83] = FMAMNT1,
   [84] = FMDTN1,
   [85] = FMAMNT2,
   [86] = FMDTN2,
   [87] = FMAMNT3,
   [88] = FMDTN3,
   [89] = VOL1,
   [90] = VOL2,
   [91] = VOL3,
   [92] = VOL4,
   [93] = VOL5,
   [94] = VOL6,
   [95] = PAN1,
   [96] = PAN2,
   [97] = PAN3,
   [100] = PAN4,
   [101] = PAN5,
   [102] = PAN6,
   [103] = OSC1_DIST,
   [104] = OSC2_DIST,
   [105] = OSC3_DIST,
   [106] = SNARE_DISTORTION,
   [107] = CYMBAL_DISTORTION,
   [108] = HAT_DISTORTION,
   [109] = VOICE_DECIMATION1,
   [110] = VOICE_DECIMATION2,
   [111] = VOICE_DECIMATION3,
   [112] = VOICE_DECIMATION4,
   [113] = VOICE_DECIMATION5,
   [114] = VOICE_DECIMATION6,
   [115] = VOICE_DECIMATION_ALL,
   [116] = FREQ_LFO1,
   [117] = FREQ_LFO2,
   [118] = FREQ_LFO3,
   [119] = FREQ_LFO4,
   [120] = FREQ_LFO5,
   [121] = FREQ_LFO6,
   [122] = AMOUNT_LFO1,
   [123] = AMOUNT_LFO2,
   [124] = AMOUNT_LFO3,
   [125] = AMOUNT_LFO4,
   [126] = AMOUNT_LFO5,
   [127] = AMOUNT_LFO6,
};

static const uint16_t globalMidiParser_nrpnToLxrParam[] = {
   128 + CC2_FILTER_DRIVE_1,
   128 + CC2_FILTER_DRIVE_2,
   128 + CC2_FILTER_DRIVE_3,
   128 + CC2_FILTER_DRIVE_4,
   128 + CC2_FILTER_DRIVE_5,
   128 + CC2_FILTER_DRIVE_6,
   128 + CC2_MIX_MOD_1,
   128 + CC2_MIX_MOD_2,
   128 + CC2_MIX_MOD_3,
   128 + CC2_VOLUME_MOD_ON_OFF1,
   128 + CC2_VOLUME_MOD_ON_OFF2,
   128 + CC2_VOLUME_MOD_ON_OFF3,
   128 + CC2_VOLUME_MOD_ON_OFF4,
   128 + CC2_VOLUME_MOD_ON_OFF5,
   128 + CC2_VOLUME_MOD_ON_OFF6,
   128 + CC2_VELO_MOD_AMT_1,
   128 + CC2_VELO_MOD_AMT_2,
   128 + CC2_VELO_MOD_AMT_3,
   128 + CC2_VELO_MOD_AMT_4,
   128 + CC2_VELO_MOD_AMT_5,
   128 + CC2_VELO_MOD_AMT_6,
   128 + CC2_VEL_DEST_1,
   128 + CC2_VEL_DEST_2,
   128 + CC2_VEL_DEST_3,
   128 + CC2_VEL_DEST_4,
   128 + CC2_VEL_DEST_5,
   128 + CC2_VEL_DEST_6,
   128 + CC2_WAVE_LFO1,
   128 + CC2_WAVE_LFO2,
   128 + CC2_WAVE_LFO3,
   128 + CC2_WAVE_LFO4,
   128 + CC2_WAVE_LFO5,
   128 + CC2_WAVE_LFO6,
   128 + CC2_VOICE_LFO1,
   128 + CC2_VOICE_LFO2,
   128 + CC2_VOICE_LFO3,
   128 + CC2_VOICE_LFO4,
   128 + CC2_VOICE_LFO5,
   128 + CC2_VOICE_LFO6,
   128 + CC2_TARGET_LFO1,
   128 + CC2_TARGET_LFO2,
   128 + CC2_TARGET_LFO3,
   128 + CC2_TARGET_LFO4,
   128 + CC2_TARGET_LFO5,
   128 + CC2_TARGET_LFO6,
   128 + CC2_RETRIGGER_LFO1,
   128 + CC2_RETRIGGER_LFO2,
   128 + CC2_RETRIGGER_LFO3,
   128 + CC2_RETRIGGER_LFO4,
   128 + CC2_RETRIGGER_LFO5,
   128 + CC2_RETRIGGER_LFO6,
   128 + CC2_SYNC_LFO1,
   128 + CC2_SYNC_LFO2,
   128 + CC2_SYNC_LFO3,
   128 + CC2_SYNC_LFO4,
   128 + CC2_SYNC_LFO5,
   128 + CC2_SYNC_LFO6,
   128 + CC2_OFFSET_LFO1,
   128 + CC2_OFFSET_LFO2,
   128 + CC2_OFFSET_LFO3,
   128 + CC2_OFFSET_LFO4,
   128 + CC2_OFFSET_LFO5,
   128 + CC2_OFFSET_LFO6,
   128 + CC2_FILTER_TYPE_1,
   128 + CC2_FILTER_TYPE_2,
   128 + CC2_FILTER_TYPE_3,
   128 + CC2_FILTER_TYPE_4,
   128 + CC2_FILTER_TYPE_5,
   128 + CC2_FILTER_TYPE_6,
   128 + CC2_TRANS1_VOL,
   128 + CC2_TRANS2_VOL,
   128 + CC2_TRANS3_VOL,
   128 + CC2_TRANS4_VOL,
   128 + CC2_TRANS5_VOL,
   128 + CC2_TRANS6_VOL,
   128 + CC2_TRANS1_WAVE,
   128 + CC2_TRANS2_WAVE,
   128 + CC2_TRANS3_WAVE,
   128 + CC2_TRANS4_WAVE,
   128 + CC2_TRANS5_WAVE,
   128 + CC2_TRANS6_WAVE,
   128 + CC2_TRANS1_FREQ,
   128 + CC2_TRANS2_FREQ,
   128 + CC2_TRANS3_FREQ,
   128 + CC2_TRANS4_FREQ,
   128 + CC2_TRANS5_FREQ,
   128 + CC2_TRANS6_FREQ,
   128 + CC2_AUDIO_OUT1,
   128 + CC2_AUDIO_OUT2,
   128 + CC2_AUDIO_OUT3,
   128 + CC2_AUDIO_OUT4,
   128 + CC2_AUDIO_OUT5,
   128 + CC2_AUDIO_OUT6,
};

static uint8_t globalMidiParser_isGlobalChannel(MidiMsg msg)
{
   const uint8_t chanonly = (msg.status & 0x0F) + 1;
   return (chanonly == midiParser_voiceMidiChannel(7));
}

static void globalMidiParser_applyInternalParameter(uint16_t lxrParamNr,
                                                    uint8_t value,
                                                    uint8_t updateOriginalValue,
                                                    enum MidiSource source)
{
   MidiMsg internalMsg = {0};

   if(lxrParamNr == GLOBAL_MIDI_UNMAPPED || lxrParamNr >= END_OF_SOUND_PARAMETERS)
      return;

   if(lxrParamNr < 128)
   {
      internalMsg.status = MIDI_CC;
      internalMsg.data1 = (uint8_t)lxrParamNr;
   }
   else
   {
      internalMsg.status = MIDI_CC2;
      internalMsg.data1 = (uint8_t)(lxrParamNr - 128);
   }

   internalMsg.data2 = value;
   internalMsg.bits.source = source;
   internalMsg.bits.sysxbyte = 0;
   internalMsg.bits.length = 2;

   frontParser_applyParameterCommand(internalMsg, updateOriginalValue);

   if((midiParser_txRxFilter & 0x04) && updateOriginalValue)
   {
      if(seq_recordActive)
         seq_recordAutomationMidiDestination(frontParser_activeTrack, lxrParamNr, value);
      else
         channelMidiParser_sendParameterEcho(lxrParamNr, value);
   }
}

static uint8_t globalMidiParser_handleNrpnControl(MidiMsg msg,
                                                  uint8_t updateOriginalValue)
{
   switch(msg.data1)
   {
      case NRPN_FINE:
         globalMidiParser_activeNrpnNumber &= (uint16_t)~0x7f;
         globalMidiParser_activeNrpnNumber |= (uint16_t)(msg.data2 & 0x7f);
         globalMidiParser_nrpnSelected = 1;
         return 1;

      case NRPN_COARSE:
         globalMidiParser_activeNrpnNumber &= 0x7f;
         globalMidiParser_activeNrpnNumber |= (uint16_t)(msg.data2 << 7);
         globalMidiParser_nrpnSelected = 1;
         return 1;

      case NRPN_DATA_ENTRY_COARSE:
         if(globalMidiParser_nrpnSelected
            && globalMidiParser_activeNrpnNumber
                  < (sizeof(globalMidiParser_nrpnToLxrParam)
                     / sizeof(globalMidiParser_nrpnToLxrParam[0])))
         {
            globalMidiParser_applyInternalParameter(
               globalMidiParser_nrpnToLxrParam[globalMidiParser_activeNrpnNumber],
               msg.data2,
               updateOriginalValue,
               msg.bits.source);
         }
         return 1;

      default:
         return 0;
   }
}

uint8_t globalMidiParser_handleSystemMessage(MidiMsg msg)
{
   /* Global MIDI clock/start/stop/MTC handling lives here. */
   switch(msg.status)
   {
      case MIDI_CLOCK:
         if((midiParser_txRxFilter & 0x02) && seq_getExtSync())
            seq_sync();
         return 1;

      case MIDI_START:
      case MIDI_CONTINUE:
         if((midiParser_txRxFilter & 0x02) && seq_getExtSync())
            sync_midiStartStop(1);
         return 1;

      case MIDI_STOP:
         if((midiParser_txRxFilter & 0x02) && seq_getExtSync())
            sync_midiStartStop(0);
         return 1;

      case MIDI_MTC_QFRAME:
         /* --AS Strategy:
          * If we get through all 8 of the mtc messages with a value of 0, it means that
          * the song has just started playing. That is the only time we will start the
          * sequencer. so we will not start it if they start the tape recorder playing half way
          * through the song. Also, if the sequencer is running, or we are in external sync mode
          * we will ignore mtc messages as well
          */
         if((midiParser_txRxFilter & 0x02) == 0)
            return 1;

         // keep track of when we got the last mtc. only do this for the 0 msg to save time
         if((msg.data1 & 0x70) == 0)
         {
            midiParser_lastMtcReceived = systick_ticks;
         }

         if(seq_isRunning())
            return 1; // already running, so we don't care to figure out where we are

         if(seq_getExtSync())
            return 1; // bypass the lot. we are using external midi clock sync

         // figure out whether we are at 0:0:0:0
         if((msg.data1 & 0x7F) == 0)
         {
            // this is the first mtc message of the set AND it's value is 0
            midiParser_mtcIgnore = 0; // reset our level of ignorance
         }
         else if(midiParser_mtcIgnore)
         {
            return 1; // not the first msg and we are ignoring
         }
         else if((msg.data1 & 0x70) != 0x70)
         {
            if((msg.data1 & 0x0F) != 0)
            {
               midiParser_mtcIgnore = 1;
               return 1;
            }
         }
         else
         {
            // message 7 and we are not ignoring yet
            if((msg.data1 & 0x01) == 0)
            {
               // well, we got all the way thru all 8 messages with 0, so the song has just begun
               // tell the front that we've started running on our own
               frontPanelSending_sendRunStop(1);
               midiParser_mtcIgnore = 1; // in case we happen to miss a 0 message. probably wouldn't happen, but...
               midiParser_mtcIsRunning = 1;
               midiParser_lastMtcReceived = systick_ticks; // also might not be needed, but...
               // start the sequencer
               seq_setRunning(1);
            }
         }

         return 1;

      default:
         return 0;
   }
}

// this will check whether mtc is running, and if so will
// check to see whether we need to stop the sequencer due to
// lack of recent mtc activity. It will also reset our running indicator
// if the sequencer has stopped for some other reason
void midiParser_checkMtc(void)
{
   /* Watch the MTC running state and stop the sequencer if it goes stale. */
   if(!midiParser_mtcIsRunning)
      return;

   if(!seq_isRunning())
   {
      // inform mtc that his services are no longer needed
      midiParser_mtcIsRunning = 0;
      return;
   }

   // at a 24fps framerate (the lowest) we should receive a completed message (we receive one every 2 frames)
   // every 83 ms. our tick counter is .25 ms resolution
   if(systick_ticks - midiParser_lastMtcReceived > 100 * 4)
   {
      // overestimate, just in case something untoward should happen
      // too much time has elapsed since our last message. mtc has gone away.
      midiParser_mtcIsRunning = 0;

      frontPanelSending_sendRunStop(0);
      // stop the sequencer
      seq_setRunning(0);
   }
}
void globalMidiParser_MIDIccHandler(MidiMsg msg, uint8_t updateOriginalValue)
{
   uint16_t lxrParamNr;

   if(!globalMidiParser_isGlobalChannel(msg))
      return;

   if(msg.data1 == BANK || msg.data1 == MOD_WHEEL)
      return;

   if(globalMidiParser_handleNrpnControl(msg, updateOriginalValue))
      return;

   lxrParamNr = globalMidiParser_ccToLxrParam[msg.data1 & 0x7f];
   globalMidiParser_applyInternalParameter(lxrParamNr,
                                           msg.data2,
                                           updateOriginalValue,
                                           msg.bits.source);
}
