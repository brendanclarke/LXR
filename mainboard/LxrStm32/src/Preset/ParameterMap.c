/*
 * ParameterMap.c
 *
 * This file isolates the pure parameter lookup tables and selector mapping
 * logic that used to live inside sequencer.c.
 */

#include "Preset/ParameterMap.h"

/* Canonical voice ownership map for every synth voice. This table is the
   single source of truth for voice-parameter classification and is scanned by
   both the voice-mask helper and the simple predicate wrapper below. */
const uint16_t preset_voiceParamMask[PRESET_SYNTH_VOICES][PRESET_VOICE_PARAM_LENGTH] =
{
   {1,8,9,20,      37,43,49,50,   62,70,74,78,  82,83,88,94,   102,108,115,121,     128,134,137,143,    149,155,161,167,    173,179,185,191,    197,203,209,215,221},
   {2,10,11,21,    38,44,51,52,   63,71,75,79,  84,85,89,95,   103,109,116,122,     129,135,138,144,    150,156,162,168,    174,180,186,192,    198,204,210,216,222},
   {3,12,13,22,    39,45,53,54,   64,72,76,80,  86,87,90,96,   104,110,117,123,     130,136,139,145,    151,157,163,169,    175,181,187,193,    199,205,211,217,223},
   {4,14,15,27,28, 40,46,55,      56,65,68,73,  77,81,91,99,   105,111,118,124,     131,140,146,152,        158,164,170,    176,182,188,194,    200,206,212,218,224},
   {6,16,17,23,    24,29,30,31,   32,41,47,57,  58,66,69,92,   100,106,112,119,125, 132,141,147,153,        159,165,171,    177,183,189,195,    201,207,213,219,225},
   {7,18,19,25,    26,33,34,35,   36,42,48,59,  60,61,67,93,   101,107,113,120,126, 133,142,148,154,        160,166,172,    178,184,190,196,    202,208,214,220,226}
};

/* Reverse selector map for automation targets. The index in this table is the
   compact selector byte seen by the front panel and MIDI parsers, while the
   stored value is the canonical destination parameter that the preset layer
   needs to write into kit state. */
static const uint16_t preset_modTargetParams[] =
{
   PAR_NONE,
   PAR_VOICE_DECIMATION_ALL,
   PAR_COARSE1,
   PAR_FINE1,
   PAR_OSC_WAVE_DRUM1,
   PAR_VELOA1,
   PAR_VELOD1,
   PAR_VOL_SLOPE1,
   PAR_MOD_EG1,
   PAR_PITCH_SLOPE1,
   PAR_MODAMNT1,
   PAR_VEL_DEST_1,
   PAR_VELO_MOD_AMT_1,
   PAR_VOLUME_MOD_ON_OFF1,
   PAR_FMAMNT1,
   PAR_FM_FREQ1,
   PAR_MOD_WAVE_DRUM1,
   PAR_MIX_MOD_1,
   PAR_TRANS1_WAVE,
   PAR_TRANS1_VOL,
   PAR_TRANS1_FREQ,
   PAR_FILTER_FREQ_1,
   PAR_RESO_1,
   PAR_FILTER_TYPE_1,
   PAR_FILTER_DRIVE_1,
   PAR_FREQ_LFO1,
   PAR_SYNC_LFO1,
   PAR_AMOUNT_LFO1,
   PAR_WAVE_LFO1,
   PAR_RETRIGGER_LFO1,
   PAR_OFFSET_LFO1,
   PAR_VOL1,
   PAR_PAN1,
   PAR_VOICE_DECIMATION1,
   PAR_DRIVE1,
   PAR_ENVELOPE_POSITION_1,
   PAR_MORPH_DRUM1,
   PAR_COARSE2,
   PAR_FINE2,
   PAR_OSC_WAVE_DRUM2,
   PAR_VELOA2,
   PAR_VELOD2,
   PAR_VOL_SLOPE2,
   PAR_MOD_EG2,
   PAR_PITCH_SLOPE2,
   PAR_MODAMNT2,
   PAR_VEL_DEST_2,
   PAR_VELO_MOD_AMT_2,
   PAR_VOLUME_MOD_ON_OFF2,
   PAR_FMAMNT2,
   PAR_FM_FREQ2,
   PAR_MOD_WAVE_DRUM2,
   PAR_MIX_MOD_2,
   PAR_TRANS2_WAVE,
   PAR_TRANS2_VOL,
   PAR_TRANS2_FREQ,
   PAR_FILTER_FREQ_2,
   PAR_RESO_2,
   PAR_FILTER_TYPE_2,
   PAR_FILTER_DRIVE_2,
   PAR_FREQ_LFO2,
   PAR_SYNC_LFO2,
   PAR_AMOUNT_LFO2,
   PAR_WAVE_LFO2,
   PAR_RETRIGGER_LFO2,
   PAR_OFFSET_LFO2,
   PAR_VOL2,
   PAR_PAN2,
   PAR_VOICE_DECIMATION2,
   PAR_DRIVE2,
   PAR_ENVELOPE_POSITION_2,
   PAR_MORPH_DRUM2,
   PAR_COARSE3,
   PAR_FINE3,
   PAR_OSC_WAVE_DRUM3,
   PAR_VELOA3,
   PAR_VELOD3,
   PAR_VOL_SLOPE3,
   PAR_MOD_EG3,
   PAR_PITCH_SLOPE3,
   PAR_MODAMNT3,
   PAR_VEL_DEST_3,
   PAR_VELO_MOD_AMT_3,
   PAR_VOLUME_MOD_ON_OFF3,
   PAR_FMAMNT3,
   PAR_FM_FREQ3,
   PAR_MOD_WAVE_DRUM3,
   PAR_MIX_MOD_3,
   PAR_TRANS3_WAVE,
   PAR_TRANS3_VOL,
   PAR_TRANS3_FREQ,
   PAR_FILTER_FREQ_3,
   PAR_RESO_3,
   PAR_FILTER_TYPE_3,
   PAR_FILTER_DRIVE_3,
   PAR_FREQ_LFO3,
   PAR_SYNC_LFO3,
   PAR_AMOUNT_LFO3,
   PAR_WAVE_LFO3,
   PAR_RETRIGGER_LFO3,
   PAR_OFFSET_LFO3,
   PAR_VOL3,
   PAR_PAN3,
   PAR_VOICE_DECIMATION3,
   PAR_DRIVE3,
   PAR_ENVELOPE_POSITION_3,
   PAR_MORPH_DRUM3,
   PAR_COARSE4,
   PAR_FINE4,
   PAR_NOISE_FREQ1,
   PAR_MIX1,
   PAR_OSC_WAVE_SNARE,
   PAR_VELOA4,
   PAR_VELOD4,
   PAR_REPEAT4,
   PAR_VOL_SLOPE4,
   PAR_MOD_EG4,
   PAR_PITCH_SLOPE4,
   PAR_MODAMNT4,
   PAR_VEL_DEST_4,
   PAR_VELO_MOD_AMT_4,
   PAR_VOLUME_MOD_ON_OFF4,
   PAR_TRANS4_WAVE,
   PAR_TRANS4_VOL,
   PAR_TRANS4_FREQ,
   PAR_FILTER_FREQ_4,
   PAR_RESO_4,
   PAR_FILTER_TYPE_4,
   PAR_FILTER_DRIVE_4,
   PAR_FREQ_LFO4,
   PAR_SYNC_LFO4,
   PAR_AMOUNT_LFO4,
   PAR_WAVE_LFO4,
   PAR_RETRIGGER_LFO4,
   PAR_OFFSET_LFO4,
   PAR_VOL4,
   PAR_PAN4,
   PAR_VOICE_DECIMATION4,
   PAR_SNARE_DISTORTION,
   PAR_ENVELOPE_POSITION_4,
   PAR_MORPH_SNARE,
   PAR_COARSE5,
   PAR_FINE5,
   PAR_WAVE1_CYM,
   PAR_VELOA5,
   PAR_VELOD5,
   PAR_REPEAT5,
   PAR_VOL_SLOPE5,
   PAR_VEL_DEST_5,
   PAR_VELO_MOD_AMT_5,
   PAR_VOLUME_MOD_ON_OFF5,
   PAR_MOD_OSC_F1_CYM,
   PAR_MOD_OSC_F2_CYM,
   PAR_MOD_OSC_GAIN1_CYM,
   PAR_MOD_OSC_GAIN2_CYM,
   PAR_WAVE2_CYM,
   PAR_WAVE3_CYM,
   PAR_TRANS5_WAVE,
   PAR_TRANS5_VOL,
   PAR_TRANS5_FREQ,
   PAR_FILTER_FREQ_5,
   PAR_RESO_5,
   PAR_FILTER_TYPE_5,
   PAR_FILTER_DRIVE_5,
   PAR_FREQ_LFO5,
   PAR_SYNC_LFO5,
   PAR_AMOUNT_LFO5,
   PAR_WAVE_LFO5,
   PAR_RETRIGGER_LFO5,
   PAR_OFFSET_LFO5,
   PAR_VOL5,
   PAR_PAN5,
   PAR_VOICE_DECIMATION5,
   PAR_CYMBAL_DISTORTION,
   PAR_ENVELOPE_POSITION_5,
   PAR_MORPH_CYM,
   PAR_COARSE6,
   PAR_FINE6,
   PAR_WAVE1_HH,
   PAR_VELOA6,
   PAR_VELOD6_CLOSED,
   PAR_VELOD6_OPEN,
   PAR_VOL_SLOPE6,
   PAR_VEL_DEST_6,
   PAR_VELO_MOD_AMT_6,
   PAR_VOLUME_MOD_ON_OFF6,
   PAR_MOD_OSC_F1,
   PAR_MOD_OSC_F2,
   PAR_MOD_OSC_GAIN1,
   PAR_MOD_OSC_GAIN2,
   PAR_WAVE2_HH,
   PAR_WAVE3_HH,
   PAR_TRANS6_WAVE,
   PAR_TRANS6_VOL,
   PAR_TRANS6_FREQ,
   PAR_FILTER_FREQ_6,
   PAR_RESO_6,
   PAR_FILTER_TYPE_6,
   PAR_FILTER_DRIVE_6,
   PAR_FREQ_LFO6,
   PAR_SYNC_LFO6,
   PAR_AMOUNT_LFO6,
   PAR_WAVE_LFO6,
   PAR_RETRIGGER_LFO6,
   PAR_OFFSET_LFO6,
   PAR_VOL6,
   PAR_PAN6,
   PAR_VOICE_DECIMATION6,
   PAR_HAT_DISTORTION,
   PAR_ENVELOPE_POSITION_6,
   PAR_MORPH_HIHAT,
};

/* Identity hook for future canonicalization. The table-driven helpers all call
   through this function so any later normalization can happen in one place
   without changing the classifier call sites. */
uint16_t preset_canonicalParamFromVoiceMask(uint16_t param)
{
   return param;
}

/* Returns the first voice bit present in a mask, or 0xff when no voice owns
   the supplied mask. The caller uses this to collapse a set of voice owners
   into a single synth-voice index. */
uint8_t preset_firstVoiceForMask(uint8_t voiceMask)
{
   uint8_t synthVoice;

   for(synthVoice=0;synthVoice<PRESET_SYNTH_VOICES;synthVoice++)
   {
      if(voiceMask & (uint8_t)(1 << synthVoice))
         return synthVoice;
   }

   return 0xff;
}

/* Builds the voice bitmask for a parameter by scanning the canonical table and
   marking every voice whose ownership list contains the parameter. This is the
   helper that lets later code decide whether a write should be routed through a
   temp-voice path or a normal-voice path. */
uint8_t preset_voiceMaskForParameter(uint16_t param)
{
   uint8_t voice;
   uint8_t i;
   uint8_t voiceMask = 0;

   for(voice=0;voice<PRESET_SYNTH_VOICES;voice++)
   {
      for(i=0;i<PRESET_VOICE_PARAM_LENGTH;i++)
      {
         if(preset_canonicalParamFromVoiceMask(preset_voiceParamMask[voice][i]) == param)
            voiceMask |= (uint8_t)(1 << voice);
      }
   }

   return voiceMask;
}

/* Pure ownership predicate that mirrors `preset_voiceMaskForParameter()` when
   callers only need a yes/no answer instead of the full bitmask. */
uint8_t preset_isVoiceParameter(uint16_t param)
{
   uint8_t voice;
   uint8_t i;

   for(voice=0;voice<PRESET_SYNTH_VOICES;voice++)
   {
      for(i=0;i<PRESET_VOICE_PARAM_LENGTH;i++)
      {
         if(preset_canonicalParamFromVoiceMask(preset_voiceParamMask[voice][i]) == param)
            return 1;
      }
   }

   return 0;
}

/* Resolves a compact selector byte into the canonical destination parameter.
   Invalid selector bytes return `PAR_NONE`, which keeps the caller's error path
   obvious and avoids silently inventing a destination. */
uint16_t preset_resolveAutomationTargetSelector(uint8_t selector)
{
   if(selector >= (sizeof(preset_modTargetParams) / sizeof(preset_modTargetParams[0])))
      return PAR_NONE;

   return preset_modTargetParams[selector];
}

/* Performs the reverse lookup from canonical destination parameter back to the
   compact selector byte used by the front-panel protocol. This keeps stored
   destinations and user-facing selector values synchronized. */
uint8_t preset_selectorForAutomationTargetDestination(uint16_t destination)
{
   uint16_t selector;

   for(selector=0;
       selector<(sizeof(preset_modTargetParams) / sizeof(preset_modTargetParams[0]));
       selector++)
   {
      if(preset_modTargetParams[selector] == destination)
         return (uint8_t)selector;
   }

   return 0;
}

/* Converts a destination parameter into the voice selector that should be
   stored alongside it. If the destination is not known, the supplied fallback
   voice index is used so the caller still has a sane selector to preserve. */
uint8_t preset_voiceSelectorForAutomationTargetDestination(uint16_t destination,
                                                           uint8_t fallback)
{
   uint8_t voiceMask = preset_voiceMaskForParameter(destination);
   uint8_t synthVoice = preset_firstVoiceForMask(voiceMask);

   if(synthVoice < PRESET_SYNTH_VOICES)
      return (uint8_t)(synthVoice + 1);

   if(fallback >= 1 && fallback <= PRESET_SYNTH_VOICES)
      return fallback;

   return 1;
}

/* Recognizes the parameter range that stores compact automation selectors for
   LFO and velocity destinations. The ingress layer uses this to decide when it
   must update the destination sideband tables as well as the raw parameter. */
uint8_t preset_isAutomationTargetSelectorParam(uint16_t param)
{
   return (param >= PAR_VEL_DEST_1 && param <= PAR_VEL_DEST_6)
       || (param >= PAR_VOICE_LFO1 && param <= PAR_VOICE_LFO6)
   || (param >= PAR_TARGET_LFO1 && param <= PAR_TARGET_LFO6);
}

/* Identifies the global morph-amount parameter range. Keeping the check in
   ParameterMap means the morph engine can ask the question without knowing the
   raw parameter numbering scheme. */
uint8_t preset_isMorphAmountParam(uint16_t param)
{
   return param >= PAR_MORPH_DRUM1 && param <= PAR_MORPH_HIHAT;
}

/* Converts a morph-amount parameter back into a voice index. The helper only
   succeeds for parameters inside the morph range and returns 0xff otherwise so
   callers can reject non-morph writes without guessing at the source voice. */
uint8_t preset_morphVoiceForParam(uint16_t param)
{
   if(param >= PAR_MORPH_DRUM1 && param <= PAR_MORPH_HIHAT)
      return (uint8_t)(param - PAR_MORPH_DRUM1);

   return 0xff;
}
