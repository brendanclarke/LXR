/*
 * ChannelMidiParser.c
 *
 * MIDI note routing and channel-level front-panel echo helpers.
 */

#include "ChannelMidiParser.h"

#include "DrumVoice.h"
#include "HiHat.h"
#include "MidiParser.h"
#include "MidiVoiceControl.h"
#include "GlobalMidiParser.h"
#include "Snare.h"
#include "TriggerOut.h"
#include "CymbalVoice.h"
#include "Preset/ParameterIngress.h"
#include "config.h"
#include "mixer.h"
#include "modulationNode.h"
#include "usb_manager.h"
#include "valueShaper.h"
#include "sequencer.h"
#include "uARTFrontSYX/frontPanelReceivingProtocol.h"
#include "uARTFrontSYX/frontPanelSendingProtocol.h"

#define BANK_1 0x01
#define BANK_2 0x02
#define BANK_3 0x04
#define BANK_4 0x08
#define BANK_5 0x10
#define BANK_6 0x20
#define BANK_7 0x40

static inline uint8_t midiParser_voiceMidiChannel(uint8_t voice)
{
   return (voice < 8) ? midi_MidiChannels[voice] : 0;
}

static inline uint8_t channelMidiParser_voiceNoteOverride(uint8_t voice)
{
   return (voice < 7) ? midi_NoteOverride[voice] : 0;
}

void channelMidiParser_noteOn(uint8_t voice, uint8_t note, uint8_t vel, uint8_t do_rec)
{
   /* Channel-owned note-on path with per-voice override and MIDI echo. */
   if(seq_isTrackMuted(voice))
      return;

   if(channelMidiParser_voiceNoteOverride(voice) != 0)
   {
      if(note == channelMidiParser_voiceNoteOverride(voice))
         note = SEQ_DEFAULT_NOTE;
      else
         return;
   }

   if(vel)
      voiceControl_noteOn(voice, note, vel);

   if(do_rec)
   {
      seq_addNote(voice, vel, note);

      if(midiParser_voiceMidiChannel(voice))
      {
         const uint8_t chan = midiParser_voiceMidiChannel(voice) - 1;
         seq_sendMidiNoteOn(chan, note, vel);
      }
   }
}

void channelMidiParser_noteOff(uint8_t voice, uint8_t note, uint8_t vel, uint8_t do_rec)
{
   /* Channel-owned note-off path reusing the note-on bookkeeping. */
   (void)note;
   vel = 0;
   channelMidiParser_noteOn(voice, note, vel, do_rec);
}

void channelMidiParser_sendBankChange(uint8_t bankCode, uint8_t value)
{
   /* Front-panel bank-change echo for the channel/global CC ladder. */
   frontPanelSending_sendBankChange(bankCode, value);
}

void channelMidiParser_sendParameterEcho(uint16_t paramNr, uint8_t value)
{
   /* Front-panel parameter echo that preserves the live CC index split. */
   frontPanelSending_sendParameterEcho(paramNr, value);
}

void channelMidiParser_sendPatchReset(void)
{
   /* Mirror the patch-reset SysEx to the front panel. */
   frontPanelSending_sendPatchReset();
}

void channelMidiParser_sendVoiceActivity(uint8_t voice)
{
   /* Pulse the front-panel voice LED for the active voice. */
   frontPanelSending_sendVoiceActivity(voice);
}
static ModulationNode* midiParser_getLfoModNode(uint8_t voice)
{
   switch(voice)
   {
      case 0:
      case 1:
      case 2:
         return &voiceArray[voice].lfo.modTarget;
      case 3:
         return &snareVoice.lfo.modTarget;
      case 4:
         return &cymbalVoice.lfo.modTarget;
      case 5:
      default:
         return &hatVoice.lfo.modTarget;
   }
}

static void midiParser_setLfoModAmount(uint8_t voice, uint8_t value)
{
   ModulationNode *node;

   if(voice >= 6)
      return;

   node = midiParser_getLfoModNode(voice);
   node->amount = value / 127.f;
   modNode_updateValue(node, node->lastVal);
}

static void midiParser_setVelocityModAmount(uint8_t voice, uint8_t value)
{
   if(voice >= 6)
      return;

   velocityModulators[voice].amount = value / 127.f;
   modNode_updateValue(&velocityModulators[voice],
                       velocityModulators[voice].lastVal);
}

static inline float calcPitchModAmount(uint8_t data2)
{
   const float val = data2 / 127.f;
   return val * val * PITCH_AMOUNT_FACTOR;
}

void channelMidiParser_MIDIccHandler(MidiMsg msg, uint8_t updateOriginalValue)
/* patch for interpreting external midi cc values by voice channel assignments 
  updateOriginalValue with 1 to record value to either automation or kit param, 0 for DSP only*/
{
   /* Voice-scoped CC ladder and bookkeeping live here after the Phase 5 split. */

// const uint8_t msgonly = msg.status & 0xF0;
   const uint8_t chanonly = (msg.status & 0x0F)+1;
   const uint16_t MIDIparamNr = msg.data1;
// const uint16_t msgVal = msg.data2;
   uint8_t midiChannelCode=0;
   
   uint16_t LXRparamNr = I_DUNNO; // zero is undefined in LXR param numbers
 
 // bc - bank change and morph are potentially time-consuming operations,
 // accumulate all the voices that need this and send as one
   if (MIDIparamNr==BANK){
      
      midiChannelCode=0;
   // deal with this separately, because we can't have the mainboard overwhelming
   // the front with bank change messages
   // this stripes the channels across a byte
      if (chanonly==midiParser_voiceMidiChannel(7)) // global channel - send global bank change to front
         midiChannelCode=0x3F;
      else{
         if (chanonly==midiParser_voiceMidiChannel(0))
            midiChannelCode|=BANK_1;
         if (chanonly==midiParser_voiceMidiChannel(1))
            midiChannelCode|=BANK_2;
         if (chanonly==midiParser_voiceMidiChannel(2))
            midiChannelCode|=BANK_3;
         if (chanonly==midiParser_voiceMidiChannel(3))
            midiChannelCode|=BANK_4;
         if (chanonly==midiParser_voiceMidiChannel(4))
            midiChannelCode|=BANK_5;
         if ( (chanonly==midiParser_voiceMidiChannel(5))||(chanonly==midiParser_voiceMidiChannel(6)) )
         {
            midiChannelCode|=BANK_6;
            midiChannelCode|=BANK_7;
         }
      }
           
         
      if (midiChannelCode!=0)
      {
         channelMidiParser_sendBankChange(midiChannelCode, msg.data2);
      }
      
   }
   else if (MIDIparamNr==MOD_WHEEL)
   {  
      if(preset_morphLoadDisabled)
         return;

      midiChannelCode=0;
   // deal with this separately, because we can't have the mainboard overwhelming
   // the front with bank change messages
   // this stripes the channels across a byte
      if (chanonly==midiParser_voiceMidiChannel(7)) // global channel - send global bank change to front
         midiChannelCode=0x3F;
      else{
         if (chanonly==midiParser_voiceMidiChannel(0))
            midiChannelCode|=BANK_1;
         if (chanonly==midiParser_voiceMidiChannel(1))
            midiChannelCode|=BANK_2;
         if (chanonly==midiParser_voiceMidiChannel(2))
            midiChannelCode|=BANK_3;
         if (chanonly==midiParser_voiceMidiChannel(3))
            midiChannelCode|=BANK_4;
         if (chanonly==midiParser_voiceMidiChannel(4))
            midiChannelCode|=BANK_5;
         if ( (chanonly==midiParser_voiceMidiChannel(5))||(chanonly==midiParser_voiceMidiChannel(6)) )
         {
            midiChannelCode|=BANK_6;
            midiChannelCode|=BANK_7;
         }
      }
           
         
      if (midiChannelCode!=0)
      {
         if(midiChannelCode == 0x3f)
            seq_setGlobalMorphAutomationValue(msg.data2);
         else
            seq_setVoiceMorphMaskAutomationValue(midiChannelCode, msg.data2);
      }
      
   }
   else {
      if (chanonly == midiParser_voiceMidiChannel(0)) // DRUM1 voice is a target
      {
         switch(MIDIparamNr){
            case MOD_WHEEL:
               break;
            case CHANNEL_VOL: // voice 1-6
               voiceArray[0].vol = msg.data2/127.f;
               LXRparamNr=VOL1;
               break;
            case UNDEF_9: // voice 1-6
               voiceArray[0].osc.waveform = msg.data2;
               LXRparamNr=OSC_WAVE_DRUM1;
               break;
            case PAN: // pan 1-6
               setPan(0,msg.data2);
               LXRparamNr=PAN1;
               break;            
            case EFFECT_1: // decimation 1-6, VOICE_DECIMATION_ALL
               mixer_decimation_rate[0] = valueShaperI2F(msg.data2,-0.7f);
               LXRparamNr=VOICE_DECIMATION1;
               break;
            case EFFECT_2: // distortion, 1-3, SNARE_DISTORTION CYMBAL_DISTORTION HAT_DISTORTION
            #if USE_FILTER_DRIVE
            voiceArray[0].filter.drive = 0.5f + (msg.data2/127.f) *6;
            #else
               setDistortionShape(&voiceArray[0].distortion,msg.data2);
            #endif
               LXRparamNr=OSC1_DIST;
               break;
            case UNDEF_14: // osc coarse tune, voice 1-6
            //clear upper nibble
               voiceArray[0].osc.midiFreq &= 0x00ff;
            //set upper nibble
               voiceArray[0].osc.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&voiceArray[0].osc);
               LXRparamNr=F_OSC1_COARSE;
               break;
            case UNDEF_14_LSB: // osc fine tune, voice 1-6
            //clear lower nibble
               voiceArray[0].osc.midiFreq &= 0xff00;
            //set lower nibble
               voiceArray[0].osc.midiFreq |= msg.data2;
               osc_recalcFreq(&voiceArray[0].osc);
               LXRparamNr=F_OSC1_FINE;
               break;
            case GEN_CONTROLLER_16:
               {
                  const float f = msg.data2/127.f;
               //exponential full range freq
                  SVF_directSetFilterValue(&voiceArray[0].filter,valueShaperF2F(f,FILTER_SHAPER) );
               }
               LXRparamNr=FILTER_FREQ_DRUM1; // SNARE_FILTER_F, CYM_FIL_FREQ, HAT_FILTER_F
               break;
            case GEN_CONTROLLER_17:
               SVF_setReso(&voiceArray[0].filter, msg.data2/127.f);
               LXRparamNr=RESO_DRUM1; // SNARE_RESO, CYM_RESO, HAT_RESO
               break;
            case GEN_CONTROLLER_18: // filter drive, 1-6
            #if UNIT_GAIN_DRIVE
            voiceArray[0].filter.drive = (msg.data2/127.f);
            #else
               SVF_setDrive(&voiceArray[0].filter,msg.data2);
            #endif
               LXRparamNr=128+CC2_FILTER_DRIVE_1;
               break;
            case GEN_CONTROLLER_19: // filter type 1-6
               voiceArray[0].filterType = msg.data2+1;
               LXRparamNr=128+CC2_FILTER_TYPE_1;
               break;
            case UNDEF_20: // snare mix - snare only
               break;
            case UNDEF_21: // snare noise freq - snare only
               break;
            case UNDEF_22: // mix mod - DRUM voice 1-3 only
               voiceArray[0].mixOscs = msg.data2;
               LXRparamNr=128+CC2_MIX_MOD_1;
               break;
            case UNDEF_23: // volume mod 
               voiceArray[0].volumeMod = msg.data2;
               LXRparamNr=128+CC2_VOLUME_MOD_ON_OFF1;
               break;
            case UNDEF_24: // velo mod amount, voice 1-6
               midiParser_setVelocityModAmount(0, msg.data2);
               LXRparamNr=128+CC2_VELO_MOD_AMT_1;
               break;
            case UNDEF_25: // velocity mod destination 1-6
            // TODO - this isn't in the params, gets done elsewhere?
               LXRparamNr=128+CC2_VEL_DEST_1;
               break;
            case UNDEF_26: // transient wave volume, voice 1-6
               voiceArray[0].transGen.volume = msg.data2/127.f;
               LXRparamNr=128+CC2_TRANS1_VOL;
               break;
            case UNDEF_27: // transient wave select, voice 1-6
               voiceArray[0].transGen.waveform = msg.data2;
               LXRparamNr=128+CC2_TRANS1_WAVE;
               break;
            case UNDEF_28: // transient wave frequency
               voiceArray[0].transGen.pitch = 1.f + ((msg.data2/33.9f)-0.75f) ;// range about  0.25 to 4 => 1/4 to 1*4
               LXRparamNr=128+CC2_TRANS1_FREQ;
               break;
            case SOUND_VAR: // snare and cym repeat only
               break;
            case ENV_RELEASE: // open hat decay - voice 7 (param 6) channel only
               break;
            case ENV_ATTACK: // attack, voices 1-6
               slopeEg2_setAttack(&voiceArray[0].oscVolEg,msg.data2,AMP_EG_SYNC);
               LXRparamNr=VELOA1;
               break;
            case SOUND_BRIGHT: // volume slope (1-3), EG_SNARE1_SLOPE, CYM_SLOPE, VOL_SLOPE6
               slopeEg2_setSlope(&voiceArray[0].oscVolEg,msg.data2);
               LXRparamNr=VOL_SLOPE1;
               break;
            case ENV_DECAY: // decay, voice 1-6
               slopeEg2_setDecay(&voiceArray[0].oscVolEg,msg.data2,AMP_EG_SYNC);
               LXRparamNr=VELOD1;
               break;
            case SOUND_VIB_RATE: // lfo rate 1-3 - lfo params are different for 4-6
               lfo_setFreq(&voiceArray[0].lfo,msg.data2);
               LXRparamNr=FREQ_LFO1;
               break;
            case SOUND_VIB_DEPTH: // AMOUNT_LFO* (1-6)
               midiParser_setLfoModAmount(0, msg.data2);
               LXRparamNr=AMOUNT_LFO1;
               break;
            case SOUND_VIB_DELAY: // 128+CC2_OFFSET_LFO* (1-6)
               voiceArray[0].lfo.phaseOffset = msg.data2/127.f * 0xffffffff;
               LXRparamNr=128+CC2_OFFSET_LFO1;
               break;
            case SOUND_UNDEF: // 128+CC2_WAVE_LFO* (1-6)
               voiceArray[0].lfo.waveform = msg.data2;
               LXRparamNr=128+CC2_WAVE_LFO1;
               break;
            case GEN_CONTROLLER_80: /*80*/	// 128+CC2_VOICE_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_VOICE_LFO1;
               break;
            case GEN_CONTROLLER_81: // 128+CC2_TARGET_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_TARGET_LFO1;
               break;
            case GEN_CONTROLLER_82: // 128+CC2_RETRIGGER_LFO* (1-6)
               voiceArray[0].lfo.retrigger = msg.data2;
               LXRparamNr=128+CC2_RETRIGGER_LFO1;
               break;
            case GEN_CONTROLLER_83: // 128+CC2_SYNC_LFO* (1-6) (different param for snare etc)
               lfo_setSync(&voiceArray[0].lfo, msg.data2);
               LXRparamNr=128+CC2_SYNC_LFO1;
               break;
            case PORT_CONTROL: // PITCHD* (1-4 for DRUM and SNARE)
               DecayEg_setDecay(&voiceArray[0].oscPitchEg,msg.data2);
               LXRparamNr=PITCHD1;
               break;
            case UNDEF_85: // MODAMNT* (1-4 for DRUM and SNARE)
               voiceArray[0].egPitchModAmount = calcPitchModAmount(msg.data2);
               LXRparamNr=MODAMNT1;
               break;
            case UNDEF_86: // pitch slope for DRUM and SNARE only
               DecayEg_setSlope(&voiceArray[0].oscPitchEg,msg.data2);
               LXRparamNr=PITCH_SLOPE1;
               break;
            case UNDEF_89: // 128+CC2_AUDIO_OUT* (1-6)
               mixer_audioRouting[0] = msg.data2;
               LXRparamNr=128+CC2_AUDIO_OUT1;
               break;
            case UNDEF_90: // 128+CC2_ENVELOPE_POSITION_* (1-6)
            // reset envelope for voice
               midi_envPosition[0] = msg.data2;
               LXRparamNr=128+CC2_ENVELOPE_POSITION_1;
               drumVoice_setEnvelope(0,midi_envPosition[0]);
               break;
            case UNDEF_102: // MOD_WAVE_DRUM* (1-3 only)
               voiceArray[0].modOsc.waveform = msg.data2;
               LXRparamNr=MOD_WAVE_DRUM1;
               break;
            case UNDEF_103: // FMDTN* (1-3 only)
            //clear upper nibble
               voiceArray[0].modOsc.midiFreq &= 0x00ff;
            //set upper nibble
               voiceArray[0].modOsc.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&voiceArray[0].modOsc);
               LXRparamNr=FMDTN1;
               break;
            case UNDEF_104: /*104*/	// FMAMNT* (1-3 only)
               voiceArray[0].fmModAmount = msg.data2/127.f;
               LXRparamNr=FMAMNT1;
               break;
            case UNDEF_105: // CYM_WAVE2, WAVE2_HH (voice 5,6)
               break;
            case UNDEF_106: // CYM_MOD_OSC_GAIN1, MOD_OSC_GAIN1 (voice 5,6)
               break;
            case UNDEF_107: // CYM_MOD_OSC_F1, MOD_OSC_F1 (voice 5,6)
               break;
            case UNDEF_108: // CYM_WAVE3, WAVE3_HH (voice 5,6)
               break;
            case UNDEF_109: // CYM_MOD_OSC_GAIN1, MOD_OSC_GAIN2 (voice 5,6)
               break;
            case UNDEF_110: // CYM_MOD_OSC_F2, MOD_OSC_F2 (voice 5,6)
               break;
            case ALL_SOUND_OFF: /*120*/	//128+CC2_MUTE_* (1-6)
               {
               
                  if(msg.data2 == 0)
                  {
                     seq_setMute(0,0);
                  }
                  else
                  {
                     seq_setMute(0,1);
                  }
               
               }
               LXRparamNr=128+CC2_MUTE_1;
               break;
            default:
               break;
         }
      
         if (LXRparamNr) // we got a valid MIDI CC destination, so LXRparamNr is non-zero
         {
            midiParser_originalCcValues[LXRparamNr] = msg.data2;
            preset_storeParameterIngress(LXRparamNr, msg.data2);
            modNode_originalValueChanged(LXRparamNr-1); // BS offset goes here NB: also for above 127 params?
         
            if((midiParser_txRxFilter & 0x04)&&updateOriginalValue&&updateOriginalValue) 
            {
               if(seq_recordActive)
               {
               //record automation if record is turned on
                  seq_recordAutomation(frontParser_activeTrack, LXRparamNr, msg.data2);
               }
               
               else{
               //midi data goes to kit params
               //midiParser_ccHandler(msg,1);
               //we received a midi cc message forward it to the front panel
                  channelMidiParser_sendParameterEcho(LXRparamNr, msg.data2);
               }
            }
         
         }
      
      
      
      }
   
    
      if (chanonly == midiParser_voiceMidiChannel(1)) // DRUM2 voice is a target
      {
         switch(MIDIparamNr){
            case MOD_WHEEL:
               break; 
            case CHANNEL_VOL: // voice 1-6
               voiceArray[1].vol = msg.data2/127.f;
               LXRparamNr=VOL2;
               break;
            case UNDEF_9: // voice 1-6
               voiceArray[1].osc.waveform = msg.data2;
               LXRparamNr=OSC_WAVE_DRUM2;
               break;
            case PAN: // pan 1-6
               setPan(1,msg.data2);
               LXRparamNr=PAN2;
               break;            
            case EFFECT_1: // decimation 1-6, VOICE_DECIMATION_ALL
               mixer_decimation_rate[1] = valueShaperI2F(msg.data2,-0.7f);
               LXRparamNr=VOICE_DECIMATION2;
               break;
            case EFFECT_2: // distortion, 1-3, SNARE_DISTORTION CYMBAL_DISTORTION HAT_DISTORTION
            #if USE_FILTER_DRIVE
            voiceArray[1].filter.drive = 0.5f + (msg.data2/127.f) *6;
            #else
               setDistortionShape(&voiceArray[1].distortion,msg.data2);
            #endif
               LXRparamNr=OSC2_DIST;
               break;
            case UNDEF_14: // osc coarse tune, voice 1-6
            //clear upper nibble
               voiceArray[1].osc.midiFreq &= 0x00ff;
            //set upper nibble
               voiceArray[1].osc.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&voiceArray[1].osc);
               LXRparamNr=F_OSC2_COARSE;
               break;
            case UNDEF_14_LSB: // osc fine tune, voice 1-6
            //clear lower nibble
               voiceArray[1].osc.midiFreq &= 0xff00;
            //set lower nibble
               voiceArray[1].osc.midiFreq |= msg.data2;
               osc_recalcFreq(&voiceArray[1].osc);
               LXRparamNr=F_OSC2_FINE;
               break;
            case GEN_CONTROLLER_16:
               {
                  const float f = msg.data2/127.f;
               //exponential full range freq
                  SVF_directSetFilterValue(&voiceArray[1].filter,valueShaperF2F(f,FILTER_SHAPER) );
               }
               LXRparamNr=FILTER_FREQ_DRUM2; // SNARE_FILTER_F, CYM_FIL_FREQ, HAT_FILTER_F
               break;
            case GEN_CONTROLLER_17:
               SVF_setReso(&voiceArray[1].filter, msg.data2/127.f);
               LXRparamNr=RESO_DRUM2; // SNARE_RESO, CYM_RESO, HAT_RESO
               break;
            case GEN_CONTROLLER_18: // filter drive, 1-6
            #if UNIT_GAIN_DRIVE
            voiceArray[1].filter.drive = (msg.data2/127.f);
            #else
               SVF_setDrive(&voiceArray[1].filter,msg.data2);
            #endif
               LXRparamNr=128+CC2_FILTER_DRIVE_2;
               break;
            case GEN_CONTROLLER_19: // filter type 1-6
               voiceArray[1].filterType = msg.data2+1;
               LXRparamNr=128+CC2_FILTER_TYPE_2;
               break;
            case UNDEF_20: // snare mix - snare only
               break;
            case UNDEF_21: // snare noise freq - snare only
               break;
            case UNDEF_22: // mix mod - DRUM voice 1-3 only
               voiceArray[1].mixOscs = msg.data2;
               LXRparamNr=128+CC2_MIX_MOD_2;
               break;
            case UNDEF_23: // volume mod 
               voiceArray[1].volumeMod = msg.data2;
               LXRparamNr=128+CC2_VOLUME_MOD_ON_OFF2;
               break;
            case UNDEF_24: // velo mod amount, voice 1-6
               midiParser_setVelocityModAmount(1, msg.data2);
               LXRparamNr=128+CC2_VELO_MOD_AMT_2;
               break;
            case UNDEF_25: // velocity mod destination 1-6
            // TODO - this isn't in the params, gets done elsewhere?
               LXRparamNr=128+CC2_VEL_DEST_2;
               break;
            case UNDEF_26: // transient wave volume, voice 1-6
               voiceArray[1].transGen.volume = msg.data2/127.f;
               LXRparamNr=128+CC2_TRANS2_VOL;
               break;
            case UNDEF_27: // transient wave select, voice 1-6
               voiceArray[1].transGen.waveform = msg.data2;
               LXRparamNr=128+CC2_TRANS2_WAVE;
               break;
            case UNDEF_28: // transient wave frequency
               voiceArray[1].transGen.pitch = 1.f + ((msg.data2/33.9f)-0.75f) ;// range about  0.25 to 4 => 1/4 to 1*4
               LXRparamNr=128+CC2_TRANS2_FREQ;
               break;
            case SOUND_VAR: // snare and cym repeat only
               break;
            case ENV_RELEASE: // open hat decay - voice 7 (param 6) channel only
               break;
            case ENV_ATTACK: // attack, voices 1-6
               slopeEg2_setAttack(&voiceArray[1].oscVolEg,msg.data2,AMP_EG_SYNC);
               LXRparamNr=VELOA2;
               break;
            case SOUND_BRIGHT: // volume slope (1-3), EG_SNARE1_SLOPE, CYM_SLOPE, VOL_SLOPE6
               slopeEg2_setSlope(&voiceArray[1].oscVolEg,msg.data2);
               LXRparamNr=VOL_SLOPE2;
               break;
            case ENV_DECAY: // decay, voice 1-6
               slopeEg2_setDecay(&voiceArray[1].oscVolEg,msg.data2,AMP_EG_SYNC);
               LXRparamNr=VELOD2;
               break;
            case SOUND_VIB_RATE: // lfo rate 1-3 - lfo params are different for 4-6
               lfo_setFreq(&voiceArray[1].lfo,msg.data2);
               LXRparamNr=FREQ_LFO2;
               break;
            case SOUND_VIB_DEPTH: // AMOUNT_LFO* (1-6)
               midiParser_setLfoModAmount(1, msg.data2);
               LXRparamNr=AMOUNT_LFO2;
               break;
            case SOUND_VIB_DELAY: // 128+CC2_OFFSET_LFO* (1-6)
               voiceArray[1].lfo.phaseOffset = msg.data2/127.f * 0xffffffff;
               LXRparamNr=128+CC2_OFFSET_LFO2;
               break;
            case SOUND_UNDEF: // 128+CC2_WAVE_LFO* (1-6)
               voiceArray[1].lfo.waveform = msg.data2;
               LXRparamNr=128+CC2_WAVE_LFO2;
               break;
            case GEN_CONTROLLER_80: /*80*/	// 128+CC2_VOICE_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_VOICE_LFO2;
               break;
            case GEN_CONTROLLER_81: // 128+CC2_TARGET_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_TARGET_LFO2;
               break;
            case GEN_CONTROLLER_82: // 128+CC2_RETRIGGER_LFO* (1-6)
               voiceArray[1].lfo.retrigger = msg.data2;
               LXRparamNr=128+CC2_RETRIGGER_LFO2;
               break;
            case GEN_CONTROLLER_83: // 128+CC2_SYNC_LFO* (1-6) (different param for snare etc)
               lfo_setSync(&voiceArray[1].lfo, msg.data2);
               LXRparamNr=128+CC2_SYNC_LFO2;
               break;
            case PORT_CONTROL: // PITCHD* (1-4 for DRUM and SNARE)
               DecayEg_setDecay(&voiceArray[1].oscPitchEg,msg.data2);
               LXRparamNr=PITCHD2;
               break;
            case UNDEF_85: // MODAMNT* (1-4 for DRUM and SNARE)
               voiceArray[1].egPitchModAmount = calcPitchModAmount(msg.data2);
               LXRparamNr=MODAMNT2;
               break;
            case UNDEF_86: // pitch slope for DRUM and SNARE only
               DecayEg_setSlope(&voiceArray[1].oscPitchEg,msg.data2);
               LXRparamNr=PITCH_SLOPE2;
               break;
            case UNDEF_89: // 128+CC2_AUDIO_OUT* (1-6)
               mixer_audioRouting[1] = msg.data2;
               LXRparamNr=128+CC2_AUDIO_OUT2;
               break;
            case UNDEF_90: // 128+CC2_ENVELOPE_POSITION_* (1-6)
            // reset envelope for voice
               midi_envPosition[1] = msg.data2;
               LXRparamNr=128+CC2_ENVELOPE_POSITION_2;
               drumVoice_setEnvelope(1,midi_envPosition[1]);
               break;
            case UNDEF_102: // MOD_WAVE_DRUM* (1-3 only)
               voiceArray[1].modOsc.waveform = msg.data2;
               LXRparamNr=MOD_WAVE_DRUM2;
               break;
            case UNDEF_103: // FMDTN* (1-3 only)
            //clear upper nibble
               voiceArray[1].modOsc.midiFreq &= 0x00ff;
            //set upper nibble
               voiceArray[1].modOsc.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&voiceArray[1].modOsc);
               LXRparamNr=FMDTN2;
               break;
            case UNDEF_104: /*104*/	// FMAMNT* (1-3 only)
               voiceArray[1].fmModAmount = msg.data2/127.f;
               LXRparamNr=FMAMNT2;
               break;
            case UNDEF_105: // CYM_WAVE2, WAVE2_HH (voice 5,6)
               break;
            case UNDEF_106: // CYM_MOD_OSC_GAIN1, MOD_OSC_GAIN1 (voice 5,6)
               break;
            case UNDEF_107: // CYM_MOD_OSC_F1, MOD_OSC_F1 (voice 5,6)
               break;
            case UNDEF_108: // CYM_WAVE3, WAVE3_HH (voice 5,6)
               break;
            case UNDEF_109: // CYM_MOD_OSC_GAIN1, MOD_OSC_GAIN2 (voice 5,6)
               break;
            case UNDEF_110: // CYM_MOD_OSC_F2, MOD_OSC_F2 (voice 5,6)
               break;
            case ALL_SOUND_OFF: /*120*/	//128+CC2_MUTE_* (1-6)
               {
               
                  if(msg.data2 == 0)
                  {
                     seq_setMute(1,0);
                  }
                  else
                  {
                     seq_setMute(1,1);
                  }
               
               }
               LXRparamNr=128+CC2_MUTE_2;
               break;
            default:
               break;
         }
      
         if (LXRparamNr) // we got a valid MIDI CC destination, so LXRparamNr is non-zero
         {
            midiParser_originalCcValues[LXRparamNr] = msg.data2;
            preset_storeParameterIngress(LXRparamNr, msg.data2);
            modNode_originalValueChanged(LXRparamNr-1); // BS offset goes here NB: also for above 127 params?
         
            if((midiParser_txRxFilter & 0x04)&&updateOriginalValue) 
            {
               if(seq_recordActive)
               {
               //record automation if record is turned on
                  seq_recordAutomation(frontParser_activeTrack, LXRparamNr, msg.data2);
               }
               
               else{
               //midi data goes to kit params
               //midiParser_ccHandler(msg,1);
               //we received a midi cc message forward it to the front panel
                  channelMidiParser_sendParameterEcho(LXRparamNr, msg.data2);
               }
            }
         
         }
      
      
      
      }
   
     
      if (chanonly == midiParser_voiceMidiChannel(2)) // DRUM3 voice is a target
      {
         switch(MIDIparamNr){
            case MOD_WHEEL:
               break;
            case CHANNEL_VOL: // voice 1-6
               voiceArray[2].vol = msg.data2/127.f;
               LXRparamNr=VOL3;
               break;
            case UNDEF_9: // voice 1-6
               voiceArray[2].osc.waveform = msg.data2;
               LXRparamNr=OSC_WAVE_DRUM3;
               break;
            case PAN: // pan 1-6
               setPan(2,msg.data2);
               LXRparamNr=PAN3;
               break;            
            case EFFECT_1: // decimation 1-6, VOICE_DECIMATION_ALL
               mixer_decimation_rate[2] = valueShaperI2F(msg.data2,-0.7f);
               LXRparamNr=VOICE_DECIMATION3;
               break;
            case EFFECT_2: // distortion, 1-3, SNARE_DISTORTION CYMBAL_DISTORTION HAT_DISTORTION
            #if USE_FILTER_DRIVE
            voiceArray[2].filter.drive = 0.5f + (msg.data2/127.f) *6;
            #else
               setDistortionShape(&voiceArray[2].distortion,msg.data2);
            #endif
               LXRparamNr=OSC3_DIST;
               break;
            case UNDEF_14: // osc coarse tune, voice 1-6
            //clear upper nibble
               voiceArray[2].osc.midiFreq &= 0x00ff;
            //set upper nibble
               voiceArray[2].osc.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&voiceArray[2].osc);
               LXRparamNr=F_OSC3_COARSE;
               break;
            case UNDEF_14_LSB: // osc fine tune, voice 1-6
            //clear lower nibble
               voiceArray[2].osc.midiFreq &= 0xff00;
            //set lower nibble
               voiceArray[2].osc.midiFreq |= msg.data2;
               osc_recalcFreq(&voiceArray[2].osc);
               LXRparamNr=F_OSC3_FINE;
               break;
            case GEN_CONTROLLER_16:
               {
                  const float f = msg.data2/127.f;
               //exponential full range freq
                  SVF_directSetFilterValue(&voiceArray[2].filter,valueShaperF2F(f,FILTER_SHAPER) );
               }
               LXRparamNr=FILTER_FREQ_DRUM3; // SNARE_FILTER_F, CYM_FIL_FREQ, HAT_FILTER_F
               break;
            case GEN_CONTROLLER_17:
               SVF_setReso(&voiceArray[2].filter, msg.data2/127.f);
               LXRparamNr=RESO_DRUM3; // SNARE_RESO, CYM_RESO, HAT_RESO
               break;
            case GEN_CONTROLLER_18: // filter drive, 1-6
            #if UNIT_GAIN_DRIVE
            voiceArray[2].filter.drive = (msg.data2/127.f);
            #else
               SVF_setDrive(&voiceArray[2].filter,msg.data2);
            #endif
               LXRparamNr=128+CC2_FILTER_DRIVE_3;
               break;
            case GEN_CONTROLLER_19: // filter type 1-6
               voiceArray[2].filterType = msg.data2+1;
               LXRparamNr=128+CC2_FILTER_TYPE_3;
               break;
            case UNDEF_20: // snare mix - snare only
               break;
            case UNDEF_21: // snare noise freq - snare only
               break;
            case UNDEF_22: // mix mod - DRUM voice 1-3 only
               voiceArray[2].mixOscs = msg.data2;
               LXRparamNr=128+CC2_MIX_MOD_3;
               break;
            case UNDEF_23: // volume mod 
               voiceArray[2].volumeMod = msg.data2;
               LXRparamNr=128+CC2_VOLUME_MOD_ON_OFF3;
               break;
            case UNDEF_24: // velo mod amount, voice 1-6
               midiParser_setVelocityModAmount(2, msg.data2);
               LXRparamNr=128+CC2_VELO_MOD_AMT_3;
               break;
            case UNDEF_25: // velocity mod destination 1-6
            // TODO - this isn't in the params, gets done elsewhere?
               LXRparamNr=128+CC2_VEL_DEST_3;
               break;
            case UNDEF_26: // transient wave volume, voice 1-6
               voiceArray[2].transGen.volume = msg.data2/127.f;
               LXRparamNr=128+CC2_TRANS3_VOL;
               break;
            case UNDEF_27: // transient wave select, voice 1-6
               voiceArray[2].transGen.waveform = msg.data2;
               LXRparamNr=128+CC2_TRANS3_WAVE;
               break;
            case UNDEF_28: // transient wave frequency
               voiceArray[2].transGen.pitch = 1.f + ((msg.data2/33.9f)-0.75f) ;// range about  0.25 to 4 => 1/4 to 1*4
               LXRparamNr=128+CC2_TRANS3_FREQ;
               break;
            case SOUND_VAR: // snare and cym repeat only
               break;
            case ENV_RELEASE: // open hat decay - voice 7 (param 6) channel only
               break;
            case ENV_ATTACK: // attack, voices 1-6
               slopeEg2_setAttack(&voiceArray[2].oscVolEg,msg.data2,AMP_EG_SYNC);
               LXRparamNr=VELOA3;
               break;
            case SOUND_BRIGHT: // volume slope (1-3), EG_SNARE1_SLOPE, CYM_SLOPE, VOL_SLOPE6
               slopeEg2_setSlope(&voiceArray[2].oscVolEg,msg.data2);
               LXRparamNr=VOL_SLOPE3;
               break;
            case ENV_DECAY: // decay, voice 1-6
               slopeEg2_setDecay(&voiceArray[2].oscVolEg,msg.data2,AMP_EG_SYNC);
               LXRparamNr=VELOD3;
               break;
            case SOUND_VIB_RATE: // lfo rate 1-3 - lfo params are different for 4-6
               lfo_setFreq(&voiceArray[2].lfo,msg.data2);
               LXRparamNr=FREQ_LFO3;
               break;
            case SOUND_VIB_DEPTH: // AMOUNT_LFO* (1-6)
               midiParser_setLfoModAmount(2, msg.data2);
               LXRparamNr=AMOUNT_LFO3;
               break;
            case SOUND_VIB_DELAY: // 128+CC2_OFFSET_LFO* (1-6)
               voiceArray[2].lfo.phaseOffset = msg.data2/127.f * 0xffffffff;
               LXRparamNr=128+CC2_OFFSET_LFO3;
               break;
            case SOUND_UNDEF: // 128+CC2_WAVE_LFO* (1-6)
               voiceArray[2].lfo.waveform = msg.data2;
               LXRparamNr=128+CC2_WAVE_LFO3;
               break;
            case GEN_CONTROLLER_80: /*80*/	// 128+CC2_VOICE_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_VOICE_LFO3;
               break;
            case GEN_CONTROLLER_81: // 128+CC2_TARGET_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_TARGET_LFO3;
               break;
            case GEN_CONTROLLER_82: // 128+CC2_RETRIGGER_LFO* (1-6)
               voiceArray[2].lfo.retrigger = msg.data2;
               LXRparamNr=128+CC2_RETRIGGER_LFO3;
               break;
            case GEN_CONTROLLER_83: // 128+CC2_SYNC_LFO* (1-6) (different param for snare etc)
               lfo_setSync(&voiceArray[2].lfo, msg.data2);
               LXRparamNr=128+CC2_SYNC_LFO3;
               break;
            case PORT_CONTROL: // PITCHD* (1-4 for DRUM and SNARE)
               DecayEg_setDecay(&voiceArray[2].oscPitchEg,msg.data2);
               LXRparamNr=PITCHD3;
               break;
            case UNDEF_85: // MODAMNT* (1-4 for DRUM and SNARE)
               voiceArray[2].egPitchModAmount = calcPitchModAmount(msg.data2);
               LXRparamNr=MODAMNT3;
               break;
            case UNDEF_86: // pitch slope for DRUM and SNARE only
               DecayEg_setSlope(&voiceArray[2].oscPitchEg,msg.data2);
               LXRparamNr=PITCH_SLOPE3;
               break;
            case UNDEF_89: // 128+CC2_AUDIO_OUT* (1-6)
               mixer_audioRouting[2] = msg.data2;
               LXRparamNr=128+CC2_AUDIO_OUT3;
               break;
            case UNDEF_90: // 128+CC2_ENVELOPE_POSITION_* (1-6)
            // reset envelope for voice
               midi_envPosition[2] = msg.data2;
               LXRparamNr=128+CC2_ENVELOPE_POSITION_3;
               drumVoice_setEnvelope(2,midi_envPosition[2]);
               break;
            case UNDEF_102: // MOD_WAVE_DRUM* (1-3 only)
               voiceArray[2].modOsc.waveform = msg.data2;
               LXRparamNr=MOD_WAVE_DRUM3;
               break;
            case UNDEF_103: // FMDTN* (1-3 only)
            //clear upper nibble
               voiceArray[2].modOsc.midiFreq &= 0x00ff;
            //set upper nibble
               voiceArray[2].modOsc.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&voiceArray[2].modOsc);
               LXRparamNr=FMDTN3;
               break;
            case UNDEF_104: /*104*/	// FMAMNT* (1-3 only)
               voiceArray[2].fmModAmount = msg.data2/127.f;
               LXRparamNr=FMAMNT3;
               break;
            case UNDEF_105: // CYM_WAVE2, WAVE2_HH (voice 5,6)
               break;
            case UNDEF_106: // CYM_MOD_OSC_GAIN1, MOD_OSC_GAIN1 (voice 5,6)
               break;
            case UNDEF_107: // CYM_MOD_OSC_F1, MOD_OSC_F1 (voice 5,6)
               break;
            case UNDEF_108: // CYM_WAVE3, WAVE3_HH (voice 5,6)
               break;
            case UNDEF_109: // CYM_MOD_OSC_GAIN1, MOD_OSC_GAIN2 (voice 5,6)
               break;
            case UNDEF_110: // CYM_MOD_OSC_F2, MOD_OSC_F2 (voice 5,6)
               break;
            case ALL_SOUND_OFF: /*120*/	//128+CC2_MUTE_* (1-6)
               {
               
                  if(msg.data2 == 0)
                  {
                     seq_setMute(2,0);
                  }
                  else
                  {
                     seq_setMute(2,1);
                  }
               
               }
               LXRparamNr=128+CC2_MUTE_3;
               break;
         
            default:
               break;
         }
      
         if (LXRparamNr) // we got a valid MIDI CC destination, so LXRparamNr is non-zero
         {
            midiParser_originalCcValues[LXRparamNr] = msg.data2;
            preset_storeParameterIngress(LXRparamNr, msg.data2);
            modNode_originalValueChanged(LXRparamNr-1); // BS offset goes here NB: also for above 127 params?
         
            if((midiParser_txRxFilter & 0x04)&&updateOriginalValue) 
            {
               if(seq_recordActive)
               {
               //record automation if record is turned on
                  seq_recordAutomation(frontParser_activeTrack, LXRparamNr, msg.data2);
               }
               
               else{
               //midi data goes to kit params
               //midiParser_ccHandler(msg,1);
               //we received a midi cc message forward it to the front panel
                  channelMidiParser_sendParameterEcho(LXRparamNr, msg.data2);
               }
            }
         
         }
      
      
      
      }
   
   
      if (chanonly == midiParser_voiceMidiChannel(3)) // SNARE voice is a target
      {
         switch(MIDIparamNr){
            case MOD_WHEEL:
               break;
            case CHANNEL_VOL: // voice 1-6
               snareVoice.vol = msg.data2/127.f;
               LXRparamNr=VOL4;
               break;
            case UNDEF_9: // voice 1-6
               snareVoice.osc.waveform = msg.data2;
               LXRparamNr=OSC_WAVE_SNARE;
               break;
            case PAN: // pan 1-6
               Snare_setPan(msg.data2);
               LXRparamNr=PAN4;
               break;            
            case EFFECT_1: // decimation 1-6, VOICE_DECIMATION_ALL
               mixer_decimation_rate[3] = valueShaperI2F(msg.data2,-0.7f);
               LXRparamNr=VOICE_DECIMATION4;
               break;
            case EFFECT_2: // distortion, 1-3, SNARE_DISTORTION CYMBAL_DISTORTION HAT_DISTORTION
               setDistortionShape(&snareVoice.distortion,msg.data2);
               LXRparamNr=SNARE_DISTORTION;
               break;
            case UNDEF_14: // osc coarse tune, voice 1-6
               {
               //clear upper nibble
                  snareVoice.osc.midiFreq &= 0x00ff;
               //set upper nibble
                  snareVoice.osc.midiFreq |= msg.data2 << 8;
                  osc_recalcFreq(&snareVoice.osc);
               }
               LXRparamNr=F_OSC4_COARSE;
               break;
            case UNDEF_14_LSB: // osc fine tune, voice 1-6
            // -bc- TODO - no midi function for snare fine tune set, need to look this up.
               LXRparamNr=F_OSC4_FINE;
               break;
            case GEN_CONTROLLER_16:
               {
               #if USE_PEAK
               peak_setFreq(&snareVoice.filter, msg.data2/127.f*20000.f);
               #else
                  const float f = msg.data2/127.f;
               //exponential full range freq
                  SVF_directSetFilterValue(&snareVoice.filter,valueShaperF2F(f,FILTER_SHAPER) );
               #endif
               }
               LXRparamNr=SNARE_FILTER_F; // SNARE_FILTER_F, CYM_FIL_FREQ, HAT_FILTER_F
               break;
            case GEN_CONTROLLER_17:
            #if USE_PEAK
            peak_setGain(&snareVoice.filter, msg.data2/127.f);
            #else
               SVF_setReso(&snareVoice.filter, msg.data2/127.f);
            #endif
               LXRparamNr=SNARE_RESO; // SNARE_RESO, CYM_RESO, HAT_RESO
               break;
            case GEN_CONTROLLER_18: // filter drive, 1-6
            #if UNIT_GAIN_DRIVE
            snareVoice.filter.drive = (msg.data2/127.f);
            #else
               SVF_setDrive(&snareVoice.filter, msg.data2);
            #endif
               LXRparamNr=128+CC2_FILTER_DRIVE_4;
               break;
            case GEN_CONTROLLER_19: // filter type 1-6
               snareVoice.filterType = msg.data2 + 1; // +1 because 0 is filter off which results in silence
               LXRparamNr=128+CC2_FILTER_TYPE_4;
               break;
            case UNDEF_20: // snare mix - snare only
               snareVoice.mix = msg.data2/127.f;
               LXRparamNr=SNARE_MIX;
               break;
            case UNDEF_21: // snare noise freq - snare only
               snareVoice.noiseOsc.freq = msg.data2/127.f*22000;
               LXRparamNr=SNARE_NOISE_F;
               break;
            case UNDEF_22: // mix mod - DRUM voice 1-3 only
               break;
            case UNDEF_23: // volume mod 
               snareVoice.volumeMod = msg.data2;
               LXRparamNr=128+CC2_VOLUME_MOD_ON_OFF4;
               break;
            case UNDEF_24: // velo mod amount, voice 1-6
               midiParser_setVelocityModAmount(3, msg.data2);
               LXRparamNr=128+CC2_VELO_MOD_AMT_4;
               break;
            case UNDEF_25: // velocity mod destination 1-6
            // TODO - this isn't in the params, gets done elsewhere?
               LXRparamNr=128+CC2_VEL_DEST_4;
               break;
            case UNDEF_26: // transient wave volume, voice 1-6
               snareVoice.transGen.volume = msg.data2/127.f;
               LXRparamNr=128+CC2_TRANS4_VOL;
               break;
            case UNDEF_27: // transient wave select, voice 1-6
               snareVoice.transGen.waveform = msg.data2;
               LXRparamNr=128+CC2_TRANS4_WAVE;
               break;
            case UNDEF_28: // transient wave frequency
               snareVoice.transGen.pitch = 1.f + ((msg.data2/33.9f)-0.75f) ;
               LXRparamNr=128+CC2_TRANS4_FREQ;
               break;
            case SOUND_VAR: // snare and cym repeat only
               snareVoice.oscVolEg.repeat = msg.data2;
               LXRparamNr=REPEAT1;
               break;
            case ENV_RELEASE: // open hat decay - voice 7 (param 6) channel only
               break;
            case ENV_ATTACK: // attack, voices 1-6
               {
                  slopeEg2_setAttack(&snareVoice.oscVolEg,msg.data2,false);
               }
               LXRparamNr=VELOA4;
               break;
            case SOUND_BRIGHT: // volume slope (1-3), EG_SNARE1_SLOPE, CYM_SLOPE, VOL_SLOPE6
               slopeEg2_setSlope(&snareVoice.oscVolEg,msg.data2);
               LXRparamNr=EG_SNARE1_SLOPE;
               break;
            case ENV_DECAY: // decay, voice 1-6
               {
                  slopeEg2_setDecay(&snareVoice.oscVolEg,msg.data2,false);
               }
               LXRparamNr=VELOD4;
               break;
            case SOUND_VIB_RATE: // lfo rate 1-3 - lfo params are different for 4-6
               lfo_setFreq(&snareVoice.lfo,msg.data2);
               LXRparamNr=FREQ_LFO4;
               break;
            case SOUND_VIB_DEPTH: // AMOUNT_LFO* (1-6)
               midiParser_setLfoModAmount(3, msg.data2);
               LXRparamNr=AMOUNT_LFO4;
               break;
            case SOUND_VIB_DELAY: // 128+CC2_OFFSET_LFO* (1-6)
               snareVoice.lfo.phaseOffset = msg.data2/127.f * 0xffffffff;
               LXRparamNr=128+CC2_OFFSET_LFO4;
               break;
            case SOUND_UNDEF: // 128+CC2_WAVE_LFO* (1-6)
               snareVoice.lfo.waveform = msg.data2;
               LXRparamNr=128+CC2_WAVE_LFO4;
               break;
            case GEN_CONTROLLER_80: /*80*/	// 128+CC2_VOICE_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_VOICE_LFO4;
               break;
            case GEN_CONTROLLER_81: // 128+CC2_TARGET_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_TARGET_LFO4;
               break;
            case GEN_CONTROLLER_82: // 128+CC2_RETRIGGER_LFO* (1-6)
               snareVoice.lfo.retrigger = msg.data2;
               LXRparamNr=128+CC2_RETRIGGER_LFO4;
               break;
            case GEN_CONTROLLER_83: // 128+CC2_SYNC_LFO* (1-6) (different param for snare etc)
               lfo_setSync(&snareVoice.lfo, msg.data2);
               LXRparamNr=128+CC2_SYNC_LFO4;
               break;
            case PORT_CONTROL: // PITCHD* (1-4 for DRUM and SNARE)
               {
                  DecayEg_setDecay(&snareVoice.oscPitchEg,msg.data2);
               }
               LXRparamNr=PITCHD4;
               break;
            case UNDEF_85: // MODAMNT* (1-4 for DRUM and SNARE)
               snareVoice.egPitchModAmount = calcPitchModAmount(msg.data2);
               LXRparamNr=MODAMNT4;
               break;
            case UNDEF_86: // pitch slope for DRUM and SNARE only
               DecayEg_setSlope(&snareVoice.oscPitchEg,msg.data2);
               LXRparamNr=PITCH_SLOPE4;
               break;
            case UNDEF_89: // 128+CC2_AUDIO_OUT* (1-6)
               mixer_audioRouting[3] = msg.data2;
               LXRparamNr=128+CC2_AUDIO_OUT4;
               break;
            case UNDEF_90: // 128+CC2_ENVELOPE_POSITION_* (1-6)
            // reset envelope for voice
               midi_envPosition[3] = msg.data2;
               LXRparamNr=128+CC2_ENVELOPE_POSITION_4;
               snare_setEnvelope(midi_envPosition[3]);
               break;
            case UNDEF_102: // MOD_WAVE_DRUM* (1-3 only)
               break;
            case UNDEF_103: // FMDTN* (1-3 only)
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
            case UNDEF_109: // CYM_MOD_OSC_GAIN1, MOD_OSC_GAIN2 (voice 5,6)
               break;
            case UNDEF_110: // CYM_MOD_OSC_F2, MOD_OSC_F2 (voice 5,6)
               break;
            case ALL_SOUND_OFF: /*120*/	//128+CC2_MUTE_* (1-6)
               {
               
                  if(msg.data2 == 0)
                  {
                     seq_setMute(3,0);
                  }
                  else
                  {
                     seq_setMute(3,1);
                  }
               
               }
               LXRparamNr=128+CC2_MUTE_4;
               break;
            default:
               break;
         }
      
         if (LXRparamNr) // we got a valid MIDI CC destination, so LXRparamNr is non-zero
         {
            midiParser_originalCcValues[LXRparamNr] = msg.data2;
            preset_storeParameterIngress(LXRparamNr, msg.data2);
            modNode_originalValueChanged(LXRparamNr-1); // BS offset goes here NB: also for above 127 params?
         
            if((midiParser_txRxFilter & 0x04)&&updateOriginalValue) 
            {
               if(seq_recordActive)
               {
               //record automation if record is turned on
                  seq_recordAutomation(frontParser_activeTrack, LXRparamNr, msg.data2);
               }
               
               else{
               //midi data goes to kit params
               //midiParser_ccHandler(msg,1);
               //we received a midi cc message forward it to the front panel
                  channelMidiParser_sendParameterEcho(LXRparamNr, msg.data2);
               }
            }
         
         }
      
      
      
      }
   
   
      if (chanonly == midiParser_voiceMidiChannel(4)) // CYMBAL voice is a target
      {
         switch(MIDIparamNr){ 
            case MOD_WHEEL:
               break;
            case CHANNEL_VOL: // voice 1-6
               cymbalVoice.vol = msg.data2/127.f;
               LXRparamNr=VOL5;
               break;
            case UNDEF_9: // voice 1-6
               cymbalVoice.osc.waveform = msg.data2;
               LXRparamNr=CYM_WAVE1;
               break;
            case PAN: // pan 1-6
               Cymbal_setPan(msg.data2);
               LXRparamNr=PAN5;
               break;            
            case EFFECT_1: // decimation 1-6, VOICE_DECIMATION_ALL
               mixer_decimation_rate[4] = valueShaperI2F(msg.data2,-0.7f);
               LXRparamNr=VOICE_DECIMATION5;
               break;
            case EFFECT_2: // distortion, 1-3, SNARE_DISTORTION CYMBAL_DISTORTION HAT_DISTORTION
               setDistortionShape(&cymbalVoice.distortion,msg.data2);
               LXRparamNr=CYMBAL_DISTORTION;
               break;
            case UNDEF_14: // osc coarse tune, voice 1-6
               {
               //clear upper nibble
                  cymbalVoice.osc.midiFreq &= 0x00ff;
               //set upper nibble
                  cymbalVoice.osc.midiFreq |= msg.data2 << 8;
                  osc_recalcFreq(&cymbalVoice.osc);
               }
               LXRparamNr=F_OSC5_COARSE;
               break;
            case UNDEF_14_LSB: // osc fine tune, voice 1-6
            // -bc- TODO - no midi function for cymbal fine tune set, need to look this up?
               LXRparamNr=F_OSC5_FINE;
               break;
            case GEN_CONTROLLER_16:
               {
                  const float f = msg.data2/127.f;
               //exponential full range freq
                  SVF_directSetFilterValue(&cymbalVoice.filter,valueShaperF2F(f,FILTER_SHAPER) );
               }
               LXRparamNr=CYM_FIL_FREQ; // SNARE_FILTER_F, CYM_FIL_FREQ, HAT_FILTER_F
               break;
            case GEN_CONTROLLER_17:
               SVF_setReso(&cymbalVoice.filter, msg.data2/127.f);
               LXRparamNr=CYM_RESO; // SNARE_RESO, CYM_RESO, HAT_RESO
               break;
            case GEN_CONTROLLER_18: // filter drive, 1-6
            #if UNIT_GAIN_DRIVE
            cymbalVoice.filter.drive = (msg.data2/127.f);
            #else
               SVF_setDrive(&cymbalVoice.filter, msg.data2);
            #endif
               LXRparamNr=128+CC2_FILTER_DRIVE_5;
               break;
            case GEN_CONTROLLER_19: // filter type 1-6
               cymbalVoice.filterType = msg.data2+1; // +1 because 0 is filter off which results in silence
               LXRparamNr=128+CC2_FILTER_TYPE_5;
               break;
            case UNDEF_20: // snare mix - snare only
               break;
            case UNDEF_21: // snare noise freq - snare only
               break;
            case UNDEF_22: // mix mod - DRUM voice 1-3 only
               break;
            case UNDEF_23: // volume mod 
               cymbalVoice.volumeMod = msg.data2;
               LXRparamNr=128+CC2_VOLUME_MOD_ON_OFF5;
               break;
            case UNDEF_24: // velo mod amount, voice 1-6
               midiParser_setVelocityModAmount(4, msg.data2);
               LXRparamNr=128+CC2_VELO_MOD_AMT_5;
               break;
            case UNDEF_25: // velocity mod destination 1-6
            // TODO - this isn't in the params, gets done elsewhere?
               LXRparamNr=128+CC2_VEL_DEST_5;
               break;
            case UNDEF_26: // transient wave volume, voice 1-6
               cymbalVoice.transGen.volume = msg.data2/127.f;
               LXRparamNr=128+CC2_TRANS5_VOL;
               break;
            case UNDEF_27: // transient wave select, voice 1-6
               cymbalVoice.transGen.waveform = msg.data2;
               LXRparamNr=128+CC2_TRANS5_WAVE;
               break;
            case UNDEF_28: // transient wave frequency
               cymbalVoice.transGen.pitch = 1.f + ((msg.data2/33.9f)-0.75f) ;
               LXRparamNr=128+CC2_TRANS5_FREQ;
               break;
            case SOUND_VAR: // snare and cym repeat only
               cymbalVoice.oscVolEg.repeat = msg.data2;
               LXRparamNr=CYM_REPEAT;
               break;
            case ENV_RELEASE: // open hat decay - voice 7 (param 6) channel only
               break;
            case ENV_ATTACK: // attack, voices 1-6
               {
                  slopeEg2_setAttack(&cymbalVoice.oscVolEg,msg.data2,false);
               }
               LXRparamNr=VELOA5;
               break;
            case SOUND_BRIGHT: // volume slope (1-3), EG_SNARE1_SLOPE, CYM_SLOPE, VOL_SLOPE6
               slopeEg2_setSlope(&cymbalVoice.oscVolEg,msg.data2);
               LXRparamNr=CYM_SLOPE;
               break;
            case ENV_DECAY: // decay, voice 1-6
               {
                  slopeEg2_setDecay(&cymbalVoice.oscVolEg,msg.data2,false);
               }
               LXRparamNr=VELOD5;
               break;
            case SOUND_VIB_RATE: // lfo rate 1-3 - lfo params are different for 4-6
               lfo_setFreq(&cymbalVoice.lfo,msg.data2);
               LXRparamNr=FREQ_LFO5;
               break;
            case SOUND_VIB_DEPTH: // AMOUNT_LFO* (1-6)
               midiParser_setLfoModAmount(4, msg.data2);
               LXRparamNr=AMOUNT_LFO5;
               break;
            case SOUND_VIB_DELAY: // 128+CC2_OFFSET_LFO* (1-6)
               cymbalVoice.lfo.phaseOffset = msg.data2/127.f * 0xffffffff;
               LXRparamNr=128+CC2_OFFSET_LFO5;
               break;
            case SOUND_UNDEF: // 128+CC2_WAVE_LFO* (1-6)
               cymbalVoice.lfo.waveform = msg.data2;
               LXRparamNr=128+CC2_WAVE_LFO5;
               break;
            case GEN_CONTROLLER_80: /*80*/	// 128+CC2_VOICE_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_VOICE_LFO5;
               break;
            case GEN_CONTROLLER_81: // 128+CC2_TARGET_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_TARGET_LFO5;
               break;
            case GEN_CONTROLLER_82: // 128+CC2_RETRIGGER_LFO* (1-6)
               cymbalVoice.lfo.retrigger = msg.data2;
               LXRparamNr=128+CC2_RETRIGGER_LFO5;
               break;
            case GEN_CONTROLLER_83: // 128+CC2_SYNC_LFO* (1-6) (different param for snare etc)
               lfo_setSync(&cymbalVoice.lfo, msg.data2);
               LXRparamNr=128+CC2_SYNC_LFO5;
               break;
            case PORT_CONTROL: // PITCHD* (1-4 for DRUM and SNARE)
               break;
            case UNDEF_85: // MODAMNT* (1-4 for DRUM and SNARE)
               break;
            case UNDEF_86: // pitch slope for DRUM and SNARE only
               break;
            case UNDEF_89: // 128+CC2_AUDIO_OUT* (1-6)
               mixer_audioRouting[4] = msg.data2;
               LXRparamNr=128+CC2_AUDIO_OUT5;
               break;
            case UNDEF_90: // 128+CC2_ENVELOPE_POSITION_* (1-6)
            // reset envelope for voice
               midi_envPosition[4] = msg.data2;
               LXRparamNr=128+CC2_ENVELOPE_POSITION_5;
               cymbal_setEnvelope(midi_envPosition[4]);
               break;
            case UNDEF_102: // MOD_WAVE_DRUM* (1-3 only)
               break;
            case UNDEF_103: // FMDTN* (1-3 only)
               break;
            case UNDEF_104: /*104*/	// FMAMNT* (1-3 only)
               break;
            case UNDEF_105: // CYM_WAVE2, WAVE2_HH (voice 5,6)
               cymbalVoice.modOsc.waveform = msg.data2;
               LXRparamNr=CYM_WAVE2;
               break;
            case UNDEF_106: // CYM_MOD_OSC_GAIN1, MOD_OSC_GAIN1 (voice 5,6)
               cymbalVoice.fmModAmount1 = msg.data2/127.f;
               LXRparamNr=CYM_MOD_OSC_GAIN1;
               break;
            case UNDEF_107: // CYM_MOD_OSC_F1, MOD_OSC_F1 (voice 5,6)
            //cymbalVoice.modOsc.freq = MidiNoteFrequencies[msg.data2];
            //clear upper nibble
               cymbalVoice.modOsc.midiFreq &= 0x00ff;
            //set upper nibble
               cymbalVoice.modOsc.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&cymbalVoice.modOsc);
               LXRparamNr=CYM_MOD_OSC_F1;
               break;
            case UNDEF_108: // CYM_WAVE3, WAVE3_HH (voice 5,6)
               cymbalVoice.modOsc2.waveform = msg.data2;
               LXRparamNr=CYM_WAVE3;
               break;
            case UNDEF_109: // CYM_MOD_OSC_GAIN2, MOD_OSC_GAIN2 (voice 5,6)
               cymbalVoice.fmModAmount2 = msg.data2/127.f;
               LXRparamNr=CYM_MOD_OSC_GAIN2;
               break;
            case UNDEF_110: // CYM_MOD_OSC_F2, MOD_OSC_F2 (voice 5,6)
            //clear upper nibble
               cymbalVoice.modOsc2.midiFreq &= 0x00ff;
            //set upper nibble
               cymbalVoice.modOsc2.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&cymbalVoice.modOsc2);
               LXRparamNr=CYM_MOD_OSC_F2;
               break;
            case ALL_SOUND_OFF: /*120*/	//128+CC2_MUTE_* (1-6)
               {
               
                  if(msg.data2 == 0)
                  {
                     seq_setMute(4,0);
                  }
                  else
                  {
                     seq_setMute(4,1);
                  }
               
               }
               LXRparamNr=128+CC2_MUTE_5;
               break;
            default:
               break;
         }
      
         if (LXRparamNr) // we got a valid MIDI CC destination, so LXRparamNr is non-zero
         {
            midiParser_originalCcValues[LXRparamNr] = msg.data2;
            preset_storeParameterIngress(LXRparamNr, msg.data2);
            modNode_originalValueChanged(LXRparamNr-1); // BS offset goes here NB: also for above 127 params?
         
            if((midiParser_txRxFilter & 0x04)&&updateOriginalValue) 
            {
               if(seq_recordActive)
               {
               //record automation if record is turned on
                  seq_recordAutomation(frontParser_activeTrack, LXRparamNr, msg.data2);
               }
               
               else{
               //midi data goes to kit params
               //midiParser_ccHandler(msg,1);
               //we received a midi cc message forward it to the front panel
                  channelMidiParser_sendParameterEcho(LXRparamNr, msg.data2);
               }
            }
         
         }
      
      
      
      }
   
   
      if (chanonly == midiParser_voiceMidiChannel(5)) // HAT CLOSED voice is a target
      {
         switch(MIDIparamNr){
            case MOD_WHEEL:
               break;
            case CHANNEL_VOL: // voice 1-6
               hatVoice.vol = msg.data2/127.f;
               LXRparamNr=VOL6;
               break;
            case UNDEF_9: // voice 1-6
               hatVoice.osc.waveform = msg.data2;
               LXRparamNr=WAVE1_HH;
               break;
            case PAN: // pan 1-6
               HiHat_setPan(msg.data2);
               LXRparamNr=PAN6;
               break;            
            case EFFECT_1: // decimation 1-6, VOICE_DECIMATION_ALL
               mixer_decimation_rate[5] = valueShaperI2F(msg.data2,-0.7f);
               LXRparamNr=VOICE_DECIMATION6;
               break;
            case EFFECT_2: // distortion, 1-3, SNARE_DISTORTION CYMBAL_DISTORTION HAT_DISTORTION
               setDistortionShape(&hatVoice.distortion,msg.data2);
               LXRparamNr=HAT_DISTORTION;
               break;
            case UNDEF_14: // osc coarse tune, voice 1-6
               {
               //clear upper nibble
                  hatVoice.osc.midiFreq &= 0x00ff;
               //set upper nibble
                  hatVoice.osc.midiFreq |= msg.data2 << 8;
                  osc_recalcFreq(&hatVoice.osc);
               }
               LXRparamNr=F_OSC6_COARSE;
               break;
            case UNDEF_14_LSB: // osc fine tune, voice 1-6
            // -bc- TODO - no midi function for hat fine tune set, need to look this up?
               LXRparamNr=F_OSC6_FINE;
               break;
            case GEN_CONTROLLER_16:
               {
                  const float f = msg.data2/127.f;
               //exponential full range freq
                  SVF_directSetFilterValue(&hatVoice.filter,valueShaperF2F(f,FILTER_SHAPER) );
               }
               LXRparamNr=HAT_FILTER_F; // SNARE_FILTER_F, CYM_FIL_FREQ, HAT_FILTER_F
               break;
            case GEN_CONTROLLER_17:
               SVF_setReso(&hatVoice.filter, msg.data2/127.f);
               LXRparamNr=HAT_RESO; // SNARE_RESO, CYM_RESO, HAT_RESO
               break;
            case GEN_CONTROLLER_18: // filter drive, 1-6
            #if UNIT_GAIN_DRIVE
            hatVoice.filter.drive = (msg.data2/127.f);
            #else
               SVF_setDrive(&hatVoice.filter, msg.data2);
            #endif
               LXRparamNr=128+CC2_FILTER_DRIVE_6;
               break;
            case GEN_CONTROLLER_19: // filter type 1-6
               hatVoice.filterType = msg.data2+1; // +1 because 0 is filter off which results in silence
               LXRparamNr=128+CC2_FILTER_TYPE_6;
               break;
            case UNDEF_20: // snare mix - snare only
               break;
            case UNDEF_21: // snare noise freq - snare only
               break;
            case UNDEF_22: // mix mod - DRUM voice 1-3 only
               break;
            case UNDEF_23: // volume mod 
               hatVoice.volumeMod = msg.data2;
               LXRparamNr=128+CC2_VOLUME_MOD_ON_OFF6;
               break;
            case UNDEF_24: // velo mod amount, voice 1-6
               midiParser_setVelocityModAmount(5, msg.data2);
               LXRparamNr=128+CC2_VELO_MOD_AMT_6;
               break;
            case UNDEF_25: // velocity mod destination 1-6
            // TODO - this isn't in the params, gets done elsewhere?
               LXRparamNr=128+CC2_VEL_DEST_6;
               break;
            case UNDEF_26: // transient wave volume, voice 1-6
               hatVoice.transGen.volume = msg.data2/127.f;
               LXRparamNr=128+CC2_TRANS6_VOL;
               break;
            case UNDEF_27: // transient wave select, voice 1-6
               hatVoice.transGen.waveform = msg.data2;
               LXRparamNr=128+CC2_TRANS6_WAVE;
               break;
            case UNDEF_28: // transient wave frequency
               hatVoice.transGen.pitch = 1.f + ((msg.data2/33.9f)-0.75f) ;
               LXRparamNr=128+CC2_TRANS6_FREQ;
               break;
            case SOUND_VAR: // snare and cym repeat only
               break;
            case ENV_RELEASE: // open hat decay - voice 7 (param 6) channel only
               hatVoice.decayOpen = slopeEg2_calcDecay(msg.data2);
               LXRparamNr=VELOD6_OPEN;
               break;
            case ENV_ATTACK: // attack, voices 1-6
               slopeEg2_setAttack(&hatVoice.oscVolEg,msg.data2,false);
               LXRparamNr=VELOA6;
               break;
            case SOUND_BRIGHT: // volume slope (1-3), EG_SNARE1_SLOPE, CYM_SLOPE, VOL_SLOPE6
               slopeEg2_setSlope(&hatVoice.oscVolEg,msg.data2);
               LXRparamNr=VOL_SLOPE6;
               break;
            case ENV_DECAY: // decay, voice 1-6
               hatVoice.decayClosed = slopeEg2_calcDecay(msg.data2);
               LXRparamNr=VELOD6;
               break;
            case SOUND_VIB_RATE: // lfo rate 1-3 - lfo params are different for 4-6
               lfo_setFreq(&hatVoice.lfo,msg.data2);
               LXRparamNr=FREQ_LFO6;
               break;
            case SOUND_VIB_DEPTH: // AMOUNT_LFO* (1-6)
               midiParser_setLfoModAmount(5, msg.data2);
               LXRparamNr=AMOUNT_LFO6;
               break;
            case SOUND_VIB_DELAY: // 128+CC2_OFFSET_LFO* (1-6)
               hatVoice.lfo.phaseOffset = msg.data2/127.f * 0xffffffff;
               LXRparamNr=128+CC2_OFFSET_LFO6;
               break;
            case SOUND_UNDEF: // 128+CC2_WAVE_LFO* (1-6)
               hatVoice.lfo.waveform = msg.data2;
               LXRparamNr=128+CC2_WAVE_LFO6;
               break;
            case GEN_CONTROLLER_80: /*80*/	// 128+CC2_VOICE_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_VOICE_LFO6;
               break;
            case GEN_CONTROLLER_81: // 128+CC2_TARGET_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_TARGET_LFO6;
               break;
            case GEN_CONTROLLER_82: // 128+CC2_RETRIGGER_LFO* (1-6)
               hatVoice.lfo.retrigger = msg.data2;
               LXRparamNr=128+CC2_RETRIGGER_LFO6;
               break;
            case GEN_CONTROLLER_83: // 128+CC2_SYNC_LFO* (1-6) (different param for snare etc)
               lfo_setSync(&hatVoice.lfo, msg.data2);
               LXRparamNr=128+CC2_SYNC_LFO5;
               break;
            case PORT_CONTROL: // PITCHD* (1-4 for DRUM and SNARE)
               break;
            case UNDEF_85: // MODAMNT* (1-4 for DRUM and SNARE)
               break;
            case UNDEF_86: // pitch slope for DRUM and SNARE only
               break;
            case UNDEF_89: // 128+CC2_AUDIO_OUT* (1-6)
               mixer_audioRouting[5] = msg.data2;
               LXRparamNr=128+CC2_AUDIO_OUT6;
               break;
            case UNDEF_90: // 128+CC2_ENVELOPE_POSITION_* (1-6)
            // reset envelope for voice
               midi_envPosition[5] = msg.data2;
               LXRparamNr=128+CC2_ENVELOPE_POSITION_6;
               hihat_setEnvelope(midi_envPosition[5]);
               break;
            case UNDEF_102: // MOD_WAVE_DRUM* (1-3 only)
               break;
            case UNDEF_103: // FMDTN* (1-3 only)
               break;
            case UNDEF_104: /*104*/	// FMAMNT* (1-3 only)
               break;
            case UNDEF_105: // CYM_WAVE2, WAVE2_HH (voice 5,6)
               hatVoice.modOsc.waveform = msg.data2;
               LXRparamNr=WAVE2_HH;
               break;
            case UNDEF_106: // CYM_MOD_OSC_GAIN1, MOD_OSC_GAIN1 (voice 5,6)
               hatVoice.fmModAmount1 = msg.data2/127.f;
               LXRparamNr=MOD_OSC_GAIN1;
               break;
            case UNDEF_107: // CYM_MOD_OSC_F1, MOD_OSC_F1 (voice 5,6)
            //clear upper nibble
               hatVoice.modOsc.midiFreq &= 0x00ff;
            //set upper nibble
               hatVoice.modOsc.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&hatVoice.modOsc);
               LXRparamNr=MOD_OSC_F1;
               break;
            case UNDEF_108: // CYM_WAVE3, WAVE3_HH (voice 5,6)
               hatVoice.modOsc2.waveform = msg.data2;
               LXRparamNr=WAVE3_HH;
               break;
            case UNDEF_109: // CYM_MOD_OSC_GAIN2, MOD_OSC_GAIN2 (voice 5,6)
               hatVoice.fmModAmount2 = msg.data2/127.f;
               LXRparamNr=MOD_OSC_GAIN2;
               break;
            case UNDEF_110: // CYM_MOD_OSC_F2, MOD_OSC_F2 (voice 5,6)
            //clear upper nibble
               hatVoice.modOsc2.midiFreq &= 0x00ff;
            //set upper nibble
               hatVoice.modOsc2.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&hatVoice.modOsc2);
               LXRparamNr=MOD_OSC_F2;
               break;
            case ALL_SOUND_OFF: /*120*/	//128+CC2_MUTE_* (1-6)
               {
               
                  if(msg.data2 == 0)
                  {
                     seq_setMute(5,0);
                  }
                  else
                  {
                     seq_setMute(5,1);
                  }
               
               }
               LXRparamNr=128+CC2_MUTE_6;
               break;
            default:
               break;
         }
      
         if (LXRparamNr) // we got a valid MIDI CC destination, so LXRparamNr is non-zero
         {
            midiParser_originalCcValues[LXRparamNr] = msg.data2;
            preset_storeParameterIngress(LXRparamNr, msg.data2);
            modNode_originalValueChanged(LXRparamNr-1); // BS offset goes here NB: also for above 127 params?
         
            if((midiParser_txRxFilter & 0x04)&&updateOriginalValue) 
            {
               if(seq_recordActive)
               {
               //record automation if record is turned on
                  seq_recordAutomation(frontParser_activeTrack, LXRparamNr, msg.data2);
               }
               
               else{
               //midi data goes to kit params
               //midiParser_ccHandler(msg,1);
               //we received a midi cc message forward it to the front panel
                  channelMidiParser_sendParameterEcho(LXRparamNr, msg.data2);
               }
            }
         
         }
      
      
      
      }
   
     
      if (chanonly == midiParser_voiceMidiChannel(6)) // HAT OPEN voice is a target - same as before
      {
         switch(MIDIparamNr){
            case MOD_WHEEL:
               break;
            case CHANNEL_VOL: // voice 1-6
               hatVoice.vol = msg.data2/127.f;
               LXRparamNr=VOL6;
               break;
            case UNDEF_9: // voice 1-6
               hatVoice.osc.waveform = msg.data2;
               LXRparamNr=WAVE1_HH;
               break;
            case PAN: // pan 1-6
               HiHat_setPan(msg.data2);
               LXRparamNr=PAN6;
               break;            
            case EFFECT_1: // decimation 1-6, VOICE_DECIMATION_ALL
               mixer_decimation_rate[5] = valueShaperI2F(msg.data2,-0.7f);
               LXRparamNr=VOICE_DECIMATION6;
               break;
            case EFFECT_2: // distortion, 1-3, SNARE_DISTORTION CYMBAL_DISTORTION HAT_DISTORTION
               setDistortionShape(&hatVoice.distortion,msg.data2);
               LXRparamNr=HAT_DISTORTION;
               break;
            case UNDEF_14: // osc coarse tune, voice 1-6
               {
               //clear upper nibble
                  hatVoice.osc.midiFreq &= 0x00ff;
               //set upper nibble
                  hatVoice.osc.midiFreq |= msg.data2 << 8;
                  osc_recalcFreq(&hatVoice.osc);
               }
               LXRparamNr=F_OSC6_COARSE;
               break;
            case UNDEF_14_LSB: // osc fine tune, voice 1-6
            // -bc- TODO - no midi function for hat fine tune set, need to look this up?
               LXRparamNr=F_OSC6_FINE;
               break;
            case GEN_CONTROLLER_16:
               {
                  const float f = msg.data2/127.f;
               //exponential full range freq
                  SVF_directSetFilterValue(&hatVoice.filter,valueShaperF2F(f,FILTER_SHAPER) );
               }
               LXRparamNr=HAT_FILTER_F; // SNARE_FILTER_F, CYM_FIL_FREQ, HAT_FILTER_F
               break;
            case GEN_CONTROLLER_17:
               SVF_setReso(&hatVoice.filter, msg.data2/127.f);
               LXRparamNr=HAT_RESO; // SNARE_RESO, CYM_RESO, HAT_RESO
               break;
            case GEN_CONTROLLER_18: // filter drive, 1-6
            #if UNIT_GAIN_DRIVE
            hatVoice.filter.drive = (msg.data2/127.f);
            #else
               SVF_setDrive(&hatVoice.filter, msg.data2);
            #endif
               LXRparamNr=128+CC2_FILTER_DRIVE_6;
               break;
            case GEN_CONTROLLER_19: // filter type 1-6
               hatVoice.filterType = msg.data2+1; // +1 because 0 is filter off which results in silence
               LXRparamNr=128+CC2_FILTER_TYPE_6;
               break;
            case UNDEF_20: // snare mix - snare only
               break;
            case UNDEF_21: // snare noise freq - snare only
               break;
            case UNDEF_22: // mix mod - DRUM voice 1-3 only
               break;
            case UNDEF_23: // volume mod 
               hatVoice.volumeMod = msg.data2;
               LXRparamNr=128+CC2_VOLUME_MOD_ON_OFF6;
               break;
            case UNDEF_24: // velo mod amount, voice 1-6
               midiParser_setVelocityModAmount(5, msg.data2);
               LXRparamNr=128+CC2_VELO_MOD_AMT_6;
               break;
            case UNDEF_25: // velocity mod destination 1-6
            // TODO - this isn't in the params, gets done elsewhere?
               LXRparamNr=128+CC2_VEL_DEST_6;
               break;
            case UNDEF_26: // transient wave volume, voice 1-6
               hatVoice.transGen.volume = msg.data2/127.f;
               LXRparamNr=128+CC2_TRANS6_VOL;
               break;
            case UNDEF_27: // transient wave select, voice 1-6
               hatVoice.transGen.waveform = msg.data2;
               LXRparamNr=128+CC2_TRANS6_WAVE;
               break;
            case UNDEF_28: // transient wave frequency
               hatVoice.transGen.pitch = 1.f + ((msg.data2/33.9f)-0.75f) ;
               LXRparamNr=128+CC2_TRANS6_FREQ;
               break;
            case SOUND_VAR: // snare and cym repeat only
               break;
            case ENV_RELEASE: // open hat decay - voice 7 (param 6) channel only
               hatVoice.decayOpen = slopeEg2_calcDecay(msg.data2);
               LXRparamNr=VELOD6_OPEN;
               break;
            case ENV_ATTACK: // attack, voices 1-6
               slopeEg2_setAttack(&hatVoice.oscVolEg,msg.data2,false);
               LXRparamNr=VELOA6;
               break;
            case SOUND_BRIGHT: // volume slope (1-3), EG_SNARE1_SLOPE, CYM_SLOPE, VOL_SLOPE6
               slopeEg2_setSlope(&hatVoice.oscVolEg,msg.data2);
               LXRparamNr=VOL_SLOPE6;
               break;
            case ENV_DECAY: // decay, voice 1-6
               hatVoice.decayClosed = slopeEg2_calcDecay(msg.data2);
               LXRparamNr=VELOD6;
               break;
            case SOUND_VIB_RATE: // lfo rate 1-3 - lfo params are different for 4-6
               lfo_setFreq(&hatVoice.lfo,msg.data2);
               LXRparamNr=FREQ_LFO6;
               break;
            case SOUND_VIB_DEPTH: // AMOUNT_LFO* (1-6)
               midiParser_setLfoModAmount(5, msg.data2);
               LXRparamNr=AMOUNT_LFO6;
               break;
            case SOUND_VIB_DELAY: // 128+CC2_OFFSET_LFO* (1-6)
               hatVoice.lfo.phaseOffset = msg.data2/127.f * 0xffffffff;
               LXRparamNr=128+CC2_OFFSET_LFO6;
               break;
            case SOUND_UNDEF: // 128+CC2_WAVE_LFO* (1-6)
               hatVoice.lfo.waveform = msg.data2;
               LXRparamNr=128+CC2_WAVE_LFO6;
               break;
            case GEN_CONTROLLER_80: /*80*/	// 128+CC2_VOICE_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_VOICE_LFO6;
               break;
            case GEN_CONTROLLER_81: // 128+CC2_TARGET_LFO* (1-6)
            // this is just a break in the cc params?
               LXRparamNr=128+CC2_TARGET_LFO6;
               break;
            case GEN_CONTROLLER_82: // 128+CC2_RETRIGGER_LFO* (1-6)
               hatVoice.lfo.retrigger = msg.data2;
               LXRparamNr=128+CC2_RETRIGGER_LFO6;
               break;
            case GEN_CONTROLLER_83: // 128+CC2_SYNC_LFO* (1-6) (different param for snare etc)
               lfo_setSync(&hatVoice.lfo, msg.data2);
               LXRparamNr=128+CC2_SYNC_LFO5;
               break;
            case PORT_CONTROL: // PITCHD* (1-4 for DRUM and SNARE)
               break;
            case UNDEF_85: // MODAMNT* (1-4 for DRUM and SNARE)
               break;
            case UNDEF_86: // pitch slope for DRUM and SNARE only
               break;
            case UNDEF_89: // 128+CC2_AUDIO_OUT* (1-6)
               mixer_audioRouting[5] = msg.data2;
               LXRparamNr=128+CC2_AUDIO_OUT6;
               break;
            case UNDEF_90: // 128+CC2_ENVELOPE_POSITION_* (1-6)
            // reset envelope for voice
               midi_envPosition[5] = msg.data2;
               LXRparamNr=128+CC2_ENVELOPE_POSITION_6;
               drumVoice_setEnvelope(5,midi_envPosition[5]);
               break;
            case UNDEF_102: // MOD_WAVE_DRUM* (1-3 only)
               break;
            case UNDEF_103: // FMDTN* (1-3 only)
               break;
            case UNDEF_104: /*104*/	// FMAMNT* (1-3 only)
               break;
            case UNDEF_105: // CYM_WAVE2, WAVE2_HH (voice 5,6)
               hatVoice.modOsc.waveform = msg.data2;
               LXRparamNr=WAVE2_HH;
               break;
            case UNDEF_106: // CYM_MOD_OSC_GAIN1, MOD_OSC_GAIN1 (voice 5,6)
               hatVoice.fmModAmount1 = msg.data2/127.f;
               LXRparamNr=MOD_OSC_GAIN1;
               break;
            case UNDEF_107: // CYM_MOD_OSC_F1, MOD_OSC_F1 (voice 5,6)
            //clear upper nibble
               hatVoice.modOsc.midiFreq &= 0x00ff;
            //set upper nibble
               hatVoice.modOsc.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&hatVoice.modOsc);
               LXRparamNr=MOD_OSC_F1;
               break;
            case UNDEF_108: // CYM_WAVE3, WAVE3_HH (voice 5,6)
               hatVoice.modOsc2.waveform = msg.data2;
               LXRparamNr=WAVE3_HH;
               break;
            case UNDEF_109: // CYM_MOD_OSC_GAIN2, MOD_OSC_GAIN2 (voice 5,6)
               hatVoice.fmModAmount2 = msg.data2/127.f;
               LXRparamNr=MOD_OSC_GAIN2;
               break;
            case UNDEF_110: // CYM_MOD_OSC_F2, MOD_OSC_F2 (voice 5,6)
            //clear upper nibble
               hatVoice.modOsc2.midiFreq &= 0x00ff;
            //set upper nibble
               hatVoice.modOsc2.midiFreq |= msg.data2 << 8;
               osc_recalcFreq(&hatVoice.modOsc2);
               LXRparamNr=MOD_OSC_F2;
               break;
            case ALL_SOUND_OFF: /*120*/	//128+CC2_MUTE_* (1-6)
               {
               
                  if(msg.data2 == 0)
                  {
                     seq_setMute(5,0);
                  }
                  else
                  {
                     seq_setMute(5,1);
                  }
               
               }
               LXRparamNr=128+CC2_MUTE_6;
               break;
            default:
               break;
         }
      
         if (LXRparamNr) // we got a valid MIDI CC destination, so LXRparamNr is non-zero
         {
            midiParser_originalCcValues[LXRparamNr] = msg.data2;
            preset_storeParameterIngress(LXRparamNr, msg.data2);
            modNode_originalValueChanged(LXRparamNr-1); // BS offset goes here NB: also for above 127 params?
         
            if((midiParser_txRxFilter & 0x04)&&updateOriginalValue) 
            {
               if(seq_recordActive)
               {
               //record automation if record is turned on
                  seq_recordAutomation(frontParser_activeTrack, LXRparamNr, msg.data2);
               }
               
               else{
               //midi data goes to kit params
               //midiParser_ccHandler(msg,1);
               //we received a midi cc message forward it to the front panel
                  channelMidiParser_sendParameterEcho(LXRparamNr, msg.data2);
               }
            }
         
         }
      
      
      
      }
   
         
      globalMidiParser_MIDIccHandler(msg, updateOriginalValue);
   }
}
 

//-----------------------------------------------------------
