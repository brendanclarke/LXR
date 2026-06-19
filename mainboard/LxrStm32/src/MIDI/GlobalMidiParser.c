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
   /* Global-channel CC ladder and sequencer-side effects. */
   const uint8_t chanonly = (msg.status & 0x0F)+1;
   const uint16_t MIDIparamNr = msg.data1;
   uint8_t i;
   uint16_t LXRparamNr = I_DUNNO; // zero is undefined in LXR param numbers

      if (chanonly == midiParser_voiceMidiChannel(7)) // GLOBAL is a target
      {
         switch(MIDIparamNr){
            case MOD_WHEEL: // global morph is handled elsewhere
               break;
            case CHANNEL_VOL: // voice 1-6
               break;
            case UNDEF_9: // voice 1-6
               break;
            case PAN: // pan 1-6
               break;            
            case EFFECT_1: // decimation 1-6, VOICE_DECIMATION_ALL, this is MIDI CC 12
               mixer_decimation_rate[6] = valueShaperI2F(msg.data2,-0.7f);
               LXRparamNr=VOICE_DECIMATION_ALL;
               break;
            case EFFECT_2: // distortion, 1-3, SNARE_DISTORTION CYMBAL_DISTORTION HAT_DISTORTION
               break;
            case UNDEF_14: // osc coarse tune, voice 1-6
               break;
            case UNDEF_14_LSB: // osc fine tune, voice 1-6
               break;
            case GEN_CONTROLLER_16: // performance roll rate
	       seq_setRollRate(msg.data2);
               break;
            case GEN_CONTROLLER_17: // performance roll note
	       seq_setRollNote(msg.data2);
               break;
            case GEN_CONTROLLER_18: // filter drive, 1-6
	       seq_setRollVelocity(msg.data2); // performance roll velocity
               break;
            case GEN_CONTROLLER_19: // filter type 1-6 // global set roll on/off by code 0=none, 127=all
	    {
		for(i=0;i<7;i++)
		{seq_rollChange(i, ((msg.data2>>i)&0x01));}
	    }
               break;
            case UNDEF_20: // snare mix - snare only
               break;
            case UNDEF_21: // snare noise freq - snare only
               break;
            case UNDEF_22: // mix mod - DRUM voice 1-3 only
               break;
            case UNDEF_23: // volume mod 
               break;
            case UNDEF_24: // velo mod amount, voice 1-6
               break;
            case UNDEF_25: // velocity mod destination 1-6
               break;
            case UNDEF_26: // transient wave volume, voice 1-6
               break;
            case UNDEF_27: // transient wave select, voice 1-6
               break;
            case UNDEF_28: // transient wave frequency
               break;
            case SOUND_VAR: // snare and cym repeat only
               break;
            case ENV_RELEASE: // open hat decay - voice 7 (param 6) channel only
               break;
            case ENV_ATTACK: // attack, voices 1-6
               break;
            case SOUND_BRIGHT: // volume slope (1-3), EG_SNARE1_SLOPE, CYM_SLOPE, VOL_SLOPE6
               break;
            case ENV_DECAY: // decay, voice 1-6
               break;
            case SOUND_VIB_RATE: // lfo rate 1-3 - lfo params are different for 4-6
               break;
            case SOUND_VIB_DEPTH: // AMOUNT_LFO* (1-6)
               break;
            case SOUND_VIB_DELAY: // 128+CC2_OFFSET_LFO* (1-6)
               break;
            case SOUND_UNDEF: // 128+CC2_WAVE_LFO* (1-6)
               break;
            case GEN_CONTROLLER_80: /*80*/	// 128+CC2_VOICE_LFO* (1-6)
               break;
            case GEN_CONTROLLER_81: // 128+CC2_TARGET_LFO* (1-6)
               break;
            case GEN_CONTROLLER_82: // 128+CC2_RETRIGGER_LFO* (1-6)
               break;
            case GEN_CONTROLLER_83: // 128+CC2_SYNC_LFO* (1-6) (different param for snare etc)
               break;
            case PORT_CONTROL: // PITCHD* (1-4 for DRUM and SNARE)
               break;
            case UNDEF_85: // MODAMNT* (1-4 for DRUM and SNARE)
               break;
            case UNDEF_86: // pitch slope for DRUM and SNARE only
               break;
            case UNDEF_89: // 128+CC2_AUDIO_OUT* (1-6)
               break;
            case UNDEF_90: 
               break;
            case UNDEF_102: // MOD_WAVE_DRUM* (1-3 only)
	    	if(msg.data2<16)
                {
                  seq_setNextPattern(msg.data2&0x07,0x7f); // all voices, hold off
                  if(msg.data2>7)
                  {
                     seq_newVoiceAvailable=0x7f; // also re-load kit voice
                  }
               }  
               break;
            case UNDEF_103: // FMDTN* (1-3 only)
	       pat_setLoop(msg.data2); // loop the sequence, just as from the frontpanel keys in perf mode. on yer own to send valid CC data. 0 is off.
               break;
            case UNDEF_104: /*104*/	// FMAMNT* (1-3 only)
               break;
            case UNDEF_105: // CYM_WAVE2, WAVE2_HH (voice 5,6)
               break;
            case UNDEF_106: // CYM_MOD_OSC_GAIN1, MOD_OSC_GAIN1 (voice 5,6)
               break;
            case UNDEF_107: // CYM_MOD_OSC_F1, MOD_OSC_F1 (voice 5,6)
               break;
            case UNDEF_108: // CYM_WAVE3, WAVE3_HH (voice 5,6)
               break;
            case UNDEF_109: // CYM_MOD_OSC_GAIN2, MOD_OSC_GAIN2 (voice 5,6)
               break;
            case UNDEF_110: // CYM_MOD_OSC_F2, MOD_OSC_F2 (voice 5,6)
               break;
            case TRACK1_SOUND_OFF: /*113*/	//128+CC2_MUTE_* (1-6)
            case TRACK2_SOUND_OFF:
            case TRACK3_SOUND_OFF:
            case TRACK4_SOUND_OFF:
            case TRACK5_SOUND_OFF:
            case TRACK6_SOUND_OFF:
            case TRACK7_SOUND_OFF:
               {
               
                  if(msg.data2 == 0)
                  {
                     seq_setMute(MIDIparamNr-TRACK1_SOUND_OFF,0);
                  }
                  else
                  {
                     seq_setMute(MIDIparamNr-TRACK1_SOUND_OFF,1);
                  }
               }
               LXRparamNr=128+CC2_MUTE_1+MIDIparamNr-TRACK1_SOUND_OFF;
               break;
	    case ALL_SOUND_OFF:     /*120*/ // bytecode - 0 is mute none, 127 is mute all
		for(i=0;i<7;i++)
		{seq_setMute(i, ((msg.data2>>i)&0x01));}
		break;
            case RESET_ALL_CONTROLLERS:
               {// this should be the only circumstance in which VOICE_CC is sent back to front
                  seq_newVoiceAvailable=0x7f;
                  channelMidiParser_sendPatchReset();
               }
               break;
            default:
               break;
         }
      
         if (LXRparamNr) // we got a valid MIDI CC destination, so LXRparamNr is non-zero
         {
            frontParser_originalCcValues[LXRparamNr] = msg.data2;
            preset_storeParameterIngress(LXRparamNr, msg.data2);
            modNode_originalValueChanged(LXRparamNr-1); // BS offset goes here NB: also for above 127 params?
         
            if((midiParser_txRxFilter & 0x04)&&updateOriginalValue) 
            {
               if(seq_recordActive)
               {
               //record automation if record is turned on
                  seq_recordAutomationMidiDestination(frontParser_activeTrack, LXRparamNr, msg.data2);
               }
               
               else{
               //midi data goes to kit params
               //frontParser_applyParameterCommand(msg,1);
               //we received a midi cc message forward it to the front panel
                  channelMidiParser_sendParameterEcho(LXRparamNr, msg.data2);
               }
            }
         }
      }
   }
