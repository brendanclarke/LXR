/*
 * Parameters.h
 *
 * Created: 27.07.2012 16:48:46
 *  Author: Julian
 */ 


#ifndef PARAMETERS_H_
#define PARAMETERS_H_


enum ParamEnums
{
	
	PAR_NONE,	//TODO this is modwheel	- stupid offset +/- 1   /*0*/
	PAR_MOD_WHEEL ,												/*1*/
	
	//waveform parameters need to be grouped 
	//makes the special case to show their names instead of 0-127 values easier
	
	PAR_OSC_WAVE_DRUM1 = PAR_MOD_WHEEL,							/*1*/
	PAR_OSC_WAVE_DRUM2,
	PAR_OSC_WAVE_DRUM3,
	PAR_OSC_WAVE_SNARE,
	NRPN_DATA_ENTRY_COARSE,
	PAR_WAVE1_CYM,
	PAR_WAVE1_HH,
	
	PAR_COARSE1,
	PAR_FINE1,
	PAR_COARSE2,		/*10*/
	PAR_FINE2,
	PAR_COARSE3,
	PAR_FINE3,
	PAR_COARSE4,
	PAR_FINE4,	
	PAR_COARSE5,
	PAR_FINE5,		
	PAR_COARSE6,
	PAR_FINE6,	
	
	PAR_MOD_WAVE_DRUM1,	/*20*/
	PAR_MOD_WAVE_DRUM2,
	PAR_MOD_WAVE_DRUM3,
	PAR_WAVE2_CYM,
	PAR_WAVE3_CYM,	
	PAR_WAVE2_HH,
	PAR_WAVE3_HH,	

	PAR_NOISE_FREQ1,		
	PAR_MIX1,
	
	PAR_MOD_OSC_F1_CYM,
	PAR_MOD_OSC_F2_CYM,		//70		/*30*/
	PAR_MOD_OSC_GAIN1_CYM,	
	PAR_MOD_OSC_GAIN2_CYM,
	PAR_MOD_OSC_F1,
	PAR_MOD_OSC_F2,		
	PAR_MOD_OSC_GAIN1,	
	PAR_MOD_OSC_GAIN2,
	
	PAR_FILTER_FREQ_1,
	PAR_FILTER_FREQ_2,
	PAR_FILTER_FREQ_3,
	PAR_FILTER_FREQ_4,					/*40*/
	PAR_FILTER_FREQ_5,	
	PAR_FILTER_FREQ_6,	

	PAR_RESO_1,
	PAR_RESO_2,
	PAR_RESO_3,	
	PAR_RESO_4,
	PAR_RESO_5,
	PAR_RESO_6,	
	
	PAR_VELOA1,		
	PAR_VELOD1,							/*50*/
	PAR_VELOA2,
	PAR_VELOD2,	
	PAR_VELOA3,		
	PAR_VELOD3,	
	PAR_VELOA4,
	PAR_VELOD4,	
	PAR_VELOA5,
	PAR_VELOD5,	
	PAR_VELOA6,
	PAR_VELOD6_CLOSED,					/*60*/
	PAR_VELOD6_OPEN,
	
	PAR_VOL_SLOPE1,
	PAR_VOL_SLOPE2,
	PAR_VOL_SLOPE3,
	PAR_VOL_SLOPE4,	
	PAR_VOL_SLOPE5,		
	PAR_VOL_SLOPE6,	
	
	PAR_REPEAT4,	
	PAR_REPEAT5,
	
	PAR_MOD_EG1,						/*70*/
	PAR_MOD_EG2,
	PAR_MOD_EG3,
	PAR_MOD_EG4,
	
	PAR_MODAMNT1,
	PAR_MODAMNT2,
	PAR_MODAMNT3,
	PAR_MODAMNT4,	
	
	PAR_PITCH_SLOPE1,
	PAR_PITCH_SLOPE2,
	PAR_PITCH_SLOPE3,					/*80*/
	PAR_PITCH_SLOPE4,
	
	PAR_FMAMNT1,	
	PAR_FM_FREQ1,	
	PAR_FMAMNT2,
	PAR_FM_FREQ2,	
	PAR_FMAMNT3,
	PAR_FM_FREQ3,	
	
	PAR_VOL1,	
	PAR_VOL2,	
	PAR_VOL3,							/*90*/
	PAR_VOL4,		
	PAR_VOL5,
	PAR_VOL6,	
	
	PAR_PAN1,	
	PAR_PAN2,	
	PAR_PAN3,
		NRPN_FINE,
		NRPN_COARSE,	
	PAR_PAN4,
	PAR_PAN5,							/*100*/
	PAR_PAN6,	
	
	PAR_DRIVE1,	
	PAR_DRIVE2,	
	PAR_DRIVE3,	
	PAR_SNARE_DISTORTION,
	PAR_CYMBAL_DISTORTION,
	PAR_HAT_DISTORTION,	
	
	PAR_VOICE_DECIMATION1,
	PAR_VOICE_DECIMATION2,
	PAR_VOICE_DECIMATION3,				/*110*/
	PAR_VOICE_DECIMATION4,
	PAR_VOICE_DECIMATION5,
	PAR_VOICE_DECIMATION6,
	PAR_VOICE_DECIMATION_ALL, 	
	
	PAR_FREQ_LFO1 ,
	PAR_FREQ_LFO2,
	PAR_FREQ_LFO3,	
	PAR_FREQ_LFO4,
	PAR_FREQ_LFO5,
	PAR_FREQ_LFO6,						/*120*/
	
	PAR_AMOUNT_LFO1,
	PAR_AMOUNT_LFO2,
	PAR_AMOUNT_LFO3,
	PAR_AMOUNT_LFO4,
	PAR_AMOUNT_LFO5,		
	PAR_AMOUNT_LFO6,	
	
			PAR_RESERVED4, //todo stupid offset -> param 0 /*127*/
	//######################################
	//######## END OF MIDI DATASIZE ########
	//######## PARAM NR 127 REACHED ########
	//######################################

	PAR_FILTER_DRIVE_1,					/*128*/
	PAR_FILTER_DRIVE_2,
	PAR_FILTER_DRIVE_3,					/*130*/
	PAR_FILTER_DRIVE_4,
	PAR_FILTER_DRIVE_5,
	PAR_FILTER_DRIVE_6,

	PAR_MIX_MOD_1,
	PAR_MIX_MOD_2,
	PAR_MIX_MOD_3,
	
	PAR_VOLUME_MOD_ON_OFF1,
	PAR_VOLUME_MOD_ON_OFF2,
	PAR_VOLUME_MOD_ON_OFF3,
	PAR_VOLUME_MOD_ON_OFF4,				/*140*/
	PAR_VOLUME_MOD_ON_OFF5,
	PAR_VOLUME_MOD_ON_OFF6,
	
	PAR_VELO_MOD_AMT_1,
	PAR_VELO_MOD_AMT_2,
	PAR_VELO_MOD_AMT_3,
	PAR_VELO_MOD_AMT_4,
	PAR_VELO_MOD_AMT_5,
	PAR_VELO_MOD_AMT_6,	
	
	// modulation destination for the velocity value in the sequencer
	// --AS these are now indices into modTargets
	PAR_VEL_DEST_1,
	PAR_VEL_DEST_2,						/*150*/
	PAR_VEL_DEST_3,
	PAR_VEL_DEST_4,
	PAR_VEL_DEST_5,
	PAR_VEL_DEST_6,	
	
	PAR_WAVE_LFO1,
	PAR_WAVE_LFO2,
	PAR_WAVE_LFO3,
	PAR_WAVE_LFO4, 
	PAR_WAVE_LFO5,
	PAR_WAVE_LFO6,						/*160*/
	
	//the target and voice parameters must be after one another!
	// these represent the voice number that the lfo is targeting
	PAR_VOICE_LFO1,
	PAR_VOICE_LFO2,
	PAR_VOICE_LFO3,
	PAR_VOICE_LFO4,
	PAR_VOICE_LFO5,
	PAR_VOICE_LFO6,
	
	// these represent the parameter being targeted
	// --AS these are now indices into modTargets
	PAR_TARGET_LFO1,	
	PAR_TARGET_LFO2,
	PAR_TARGET_LFO3,
	PAR_TARGET_LFO4,					/*170*/
	PAR_TARGET_LFO5,
	PAR_TARGET_LFO6,
	
	PAR_RETRIGGER_LFO1,
	PAR_RETRIGGER_LFO2,
	PAR_RETRIGGER_LFO3,
	PAR_RETRIGGER_LFO4,
	PAR_RETRIGGER_LFO5,
	PAR_RETRIGGER_LFO6,
	
	PAR_SYNC_LFO1,
	PAR_SYNC_LFO2,						/*180*/
	PAR_SYNC_LFO3,		
	PAR_SYNC_LFO4,
	PAR_SYNC_LFO5,
	PAR_SYNC_LFO6,
				
	PAR_OFFSET_LFO1,	
	PAR_OFFSET_LFO2,
	PAR_OFFSET_LFO3,
	PAR_OFFSET_LFO4,
	PAR_OFFSET_LFO5,
	PAR_OFFSET_LFO6,					/*190*/

	PAR_FILTER_TYPE_1,
	PAR_FILTER_TYPE_2,
	PAR_FILTER_TYPE_3,
	PAR_FILTER_TYPE_4,
	PAR_FILTER_TYPE_5,
	PAR_FILTER_TYPE_6,
	
	PAR_TRANS1_VOL,
	PAR_TRANS2_VOL,
	PAR_TRANS3_VOL,
	PAR_TRANS4_VOL,						/*200*/
	PAR_TRANS5_VOL,
	PAR_TRANS6_VOL,

	PAR_TRANS1_WAVE,
	PAR_TRANS2_WAVE,
	PAR_TRANS3_WAVE,	
	PAR_TRANS4_WAVE,	
	PAR_TRANS5_WAVE,	
	PAR_TRANS6_WAVE,	
	
	PAR_TRANS1_FREQ,
	PAR_TRANS2_FREQ,					/*210*/
	PAR_TRANS3_FREQ,
	PAR_TRANS4_FREQ,
	PAR_TRANS5_FREQ,
	PAR_TRANS6_FREQ,
	
	PAR_AUDIO_OUT1,
	PAR_AUDIO_OUT2,
	PAR_AUDIO_OUT3,
	PAR_AUDIO_OUT4,
	PAR_AUDIO_OUT5,
	PAR_AUDIO_OUT6,						/*220*/
	
	//--AS midi note - This is a note value that is sent when the track triggers, if it's 0 it means that
	// it will trigger the note that is specified on the step (PAR_STEP_NOTE). If it is non-zero then
	// PAR_STEP_NOTE is ignored and this note is sent instead
	PAR_MIDI_NOTE1,
	PAR_MIDI_NOTE2,
	PAR_MIDI_NOTE3,
	PAR_MIDI_NOTE4,
	PAR_MIDI_NOTE5,
	PAR_MIDI_NOTE6,
	PAR_MIDI_NOTE7,

	//#########################################
	//######## End of sound Parameters ########
	//#########################################
	
	//all parameters in this section are only there to be referenced from the menu
	//they are not saved anywhere
	END_OF_SOUND_PARAMETERS,
	PAR_ROLL= END_OF_SOUND_PARAMETERS, //228
	PAR_MORPH,

	PAR_ACTIVE_STEP, 					/*230*/
	PAR_STEP_VOLUME,
	PAR_STEP_PROB,
	PAR_STEP_NOTE,
	
	PAR_EUKLID_LENGTH,
	PAR_EUKLID_STEPS,
	PAR_EUKLID_ROTATION,
	
	PAR_AUTOM_TRACK,
	
	// --AS these are now indices into modTargets
	PAR_P1_DEST,
	PAR_P2_DEST,
	
	PAR_P1_VAL,							/*240*/
	PAR_P2_VAL,
	
	PAR_SHUFFLE,

	PAR_PATTERN_BEAT,
	PAR_PATTERN_NEXT,
	
	PAR_TRACK_LENGTH,
	
	PAR_POS_X,
	PAR_POS_Y,
	PAR_FLUX,
	PAR_SOM_FREQ,
	PAR_TRACK_ROTATION,				// --AS **PATROT

	
	//#########################################
	//######## Global Parameters ##############
	//#########################################
	PAR_BEGINNING_OF_GLOBALS, //a placeholder to mark the beginning of the global var space not present in morph and not needed in the seq
	//global params
	PAR_BPM = PAR_BEGINNING_OF_GLOBALS,	/*250*/
	

	
	PAR_MIDI_CHAN_1,
	PAR_MIDI_CHAN_2,
	PAR_MIDI_CHAN_3,
	PAR_MIDI_CHAN_4,
	PAR_MIDI_CHAN_5,
	PAR_MIDI_CHAN_6,

	PAR_FETCH,							// bool
	PAR_FOLLOW,							// bool
	
	
	PAR_QUANTISATION,
	
	PAR_SCREENSAVER_ON_OFF,				// bool /*260*/
	PAR_MIDI_MODE,						//--AS unused now
	PAR_MIDI_CHAN_7,
	PAR_MIDI_ROUTING,
	PAR_MIDI_FILT_TX,
	PAR_MIDI_FILT_RX,
	//prescaler control for trigger IO extension
	PAR_PRESCALER_CLOCK_IN,
	PAR_PRESCALER_CLOCK_OUT1,
	PAR_PRESCALER_CLOCK_OUT2,
	PAR_TRIG_GATE_MODE,

	PAR_BAR_RESET_MODE,					// bool --AS 0 or 1   /*270*/
	PAR_MIDI_CHAN_GLOBAL,				// --AS global midi channel
   	PAR_SEQ_PC_TIME,                 // bool, 0 for bar sequence change, 1 for step change
	PAR_BUT_SHIFT_MODE,
	NUM_PARAMS
};


#endif /* PARAMETERS_H_ */
