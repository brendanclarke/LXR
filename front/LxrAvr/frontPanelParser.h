/*
 * frontPanelParser.h
 *
 * Created: 27.04.2012 12:03:52
 *  Author: Julian
 */ 


#ifndef FRONTPANELPARSER_H_
#define FRONTPANELPARSER_H_
#include <avr/io.h>
#include "./Preset/SeqStep.h"



/**for pattern request from seq we need a flag to signal if new data arrived*/
extern volatile uint8_t frontParser_newSeqDataAvailable;
//a step instance to buffer the data received from the sequencer
extern volatile StepData frontParser_stepData;
extern uint8_t frontPanel_sysexMode;
extern uint8_t frontPanel_longOp;
extern uint8_t frontPanel_longData;



#define NOTE_ON 			0x90	// 2 data bytes
#define MIDI_CC				0xb0	// 2 data bytes
//#define MIDI_CC2			0xF4	// 2 data bytes an unused midi status is used to indicate another cc message for params above 127


#define END_PATTERN_NOTE_ON 0x

#define SYSEX_START			0xF0
#define SYSEX_END			0xF7
#define PATCH_RESET			0xFF
//control messages from cortex for leds
//status, param changes

#define VOICE_MORPH     0xab // message from seq automation node to morph voice
#define MACRO_CC        0xaa // message to send with macro changes
/* MACRO_CC message structure
byte1 - status byte 0xaa as above
byte2, data1 byte: xttaaa-b : tt= top level macro value sent (2 macros exist now, we can do 2 more if we want)
                              aaa= macro destination value sent (4 destinations exist now, can do 8)
                              b=macro mod target value top bit
                              I have left a blank bit above this to make it easier to make more than 255 kit parameters
                              if we ever want to take on that can of worms
                              
byte3, data2 byte: xbbbbbbb : b=macro mod target value lower 7 bits or top level value full
*/

#define MORPH_CC        0xac
#define BANK_CHANGE_CC  0xad
#define PARAM_CC        0xae
#define PARAM_CC2       0xaf
#define LED_CC				0xb1
#define SEQ_CC				0xb2
#define CODEC_CC			0xb3
#define VOICE_CC			0xb4
#define SET_BPM				0xb5
#define CC_2				0xb6		//for parameters above 127
#define CC_LFO_TARGET		0xb7		
#define CC_VELO_TARGET		0xb8	
#define STEP_CC				0xb9	//toggle sub step pattern step on/off
#define SET_P1_DEST			0xba	// SET_P1_DEST,  destinationNr (hi/lo byte) --> track(voice) via set active track cmd, step as well (SEQ_SELECT_ACTIVE_STEP)
#define SET_P2_DEST			0xbb
#define SET_P1_VAL			0xbc	// SET_P1_VAL, stepNr, value --> track(voice) via set active track cmd
#define SET_P2_VAL			0xbd
#define MAIN_STEP_CC		0xbe	//toggle main step
#define ARM_AUTOMATION_STEP	0xbf	// status - stepNr - track | OnOff

#define SAMPLE_CC			0xc0
#define SAMPLE_START_UPLOAD 0x01
#define SAMPLE_COUNT		0x02

//preset status bytes
#define PRESET_NAME				0xb4	/**< this message consists of 4 messages with status FRONT_PRESET_NAME and 2 data bytes each with 2 charactzers of the name*/
#define PRESET					0xb5


//preset
#define PRESET_LOAD				0x01
#define PRESET_SAVE				0x02
#define PATTERN_LOAD			0x03



// end of 1st byte 'STATUS' byte messages
//led_cc messages - if 1st byte is 'LED_CC'
#define LED_CURRENT_STEP_NR 0x01
#define LED_SEQ_BUTTON		0x02
#define LED_QUERY_SEQ_TRACK 0x03
#define LED_PULSE_BEAT		0x04	/**< pulse the beat indicator LED*/
#define LED_SEQ_SUB_STEP	0x05
#define LED_ALL_SUBSTEP        0x3f

#define LED_SEQ_MAIN_ONE      0x40  // bc - send as 4-led sets to prevent message choke
#define LED_SEQ_MAIN_TWO      0x41
#define LED_SEQ_MAIN_THREE    0x42
#define LED_SEQ_MAIN_FOUR     0x43

#define LED_SEQ_SUB_STEP_LOWER 0x44
#define LED_SEQ_SUB_STEP_UPPER 0x45

//#define LED_TRIGGER_VOICE	0x05	/**< send by the sequencer whenever a voice is triggered*/


//VOICE_CC
//#define VOICE_AUDIO_OUT		0x01
// --AS appears unused
//#define VOICE_MIDI_CHAN		0x02
#define VOICE_DECIMATION	0x03

//TODO in hex werten... nicht dezimal als hex
//seq_cc messages
#define SEQ_RUN_STOP		0x01

#define SEQ_MUTE_TRACK		0x09
#define SEQ_UNMUTE_TRACK	0x0a
#define SEQ_CHANGE_PAT		0x0b	/**<requested a new pattern. the same message is send back to the front as ack that the new pattern is loaded (after the current patern finished)*/
#define SEQ_ROLL_ON			0x0c	/**< start roll for voice data2*/
#define SEQ_ROLL_OFF		0x0d	/**< stop roll for voice data2*/
//#define SEQ_GET_ACTIVE_PAT	0x0e	/**< get the active pattern number from the sequencer */
#define SEQ_REQUEST_STEP_PARAMS 0x0f
#define SEQ_ROLL_ON_OFF		0x10	/**< turn voice roll/flamm on/off. data 2 parameter is bit 0 to 3 = voice number, bit 4 is on/off flag*/
#define SEQ_ROLL_RATE		0x11
#define SEQ_VOLUME			0x12
#define SEQ_NOTE			0x13
#define SEQ_PROB			0x14
#define SEQ_SET_ACTIVE_TRACK 0x15
//#define SEQ_RESYNC_LFO		0x16	/**< LFO is no longer running on the front */
#define SEQ_EUKLID_LENGTH   0x17	/** sets the length of the current track from 0 to 15 steps*/
#define SEQ_EUKLID_STEPS	0x18
#define SEQ_REQUEST_EUKLID_PARAMS 0x19
#define SEQ_SET_SHOWN_PATTERN	0x1A

#define SEQ_REC_ON_OFF		0x1B		/**< start(data2=1) or stop(data2=0) recording mode */
#define SEQ_REQUEST_PATTERN_PARAMS 0x1C /**< the sequencer sends back the data of the active pattern */
#define SEQ_SET_PAT_BEAT	0x1D		/**< on every Nth bar the pattern will change to the next pattern*/
#define SEQ_SET_PAT_NEXT	0x1E		/**< the next pattern that will be played when the current finishes*/

#define SEQ_CLEAR_TRACK		0x1f
#define SEQ_COPY_TRACK		0x20
#define SEQ_COPY_PATTERN	0x21
#define SEQ_SET_QUANT		0x22
#define SEQ_SET_AUTOM_TRACK	0x23 // SEQ_CC, SEQ_SET_AUTOM_TRACK, autoTrkNr
#define SEQ_SELECT_ACTIVE_STEP 0x24 //used for automation params to select the step for which the destination should be set
#define SEQ_SHUFFLE			0x25
#define SEQ_TRACK_LENGTH	0x26
#define SEQ_TRACK_SCALE    0x3c
#define SEQ_CLEAR_PATTERN	0x27
#define SEQ_CLEAR_AUTOM		0x28 //voice nr (0xf0) + autom track nr (0x0f)

#define SEQ_POSX			0x29
#define SEQ_POSY			0x2a
#define SEQ_FLUX			0x2b
#define SEQ_SOM_FREQ		0x2c
#define SEQ_MIDI_CHAN		0x2d	//voiceNr (0xf0) + channel (0x0f). --AS voice 7=global channel
#define SEQ_MIDI_MODE		0x2e //--AS unused now
#define SEQ_MIDI_ROUTING    0x2f
#define SEQ_MIDI_FILT_TX	0x30
#define SEQ_MIDI_FILT_RX	0x31

#define SEQ_BAR_RESET_MODE  0x32 //--AS
#define SEQ_ERASE_ON_OFF    0x33 //--AS **RECORD data2=1: erase on data2=0: erase off
#define SEQ_TRACK_ROTATION	0x34 // --AS **PATROT rotate a track's start position 0 to 15
#define SEQ_EUKLID_ROTATION	0x35
#define SEQ_EUKLID_SUBSTEP_ROTATION 0x46
#define SEQ_EUKLID_RESET   0x47

#define SEQ_TRIGGER_IN_PPQ	  0x36
#define SEQ_TRIGGER_OUT1_PPQ  0x37
#define SEQ_TRIGGER_OUT2_PPQ  0x38
#define SEQ_TRIGGER_GATE_MODE 0x39
#define SEQ_ROLL_NOTE       0x40
#define SEQ_ROLL_VELOCITY       0x41
#define SEQ_ROLL_MODE          0x42

#define SEQ_TRANSPOSE            0x43
#define SEQ_TRANSPOSE_ON_OFF     0x44
#define SEQ_SET_LOOP             0x45

//bc adds

#define SEQ_COPY_TRACK_PATTERN         0x3a
#define SEQ_PC_TIME                    0x3b
#define SEQ_COPY_STEP_SET_SRC          0x3d // added message for copy step
#define SEQ_COPY_STEP_SET_DST          0x3e 
#define SEQ_PATTERN_TRACKARRAY	       0x48 // let the mainboard know which
#define SEQ_PATTERN_PATARRAY	       0x49 // voices to care about for patterns
#define SEQ_LOCK_VOICES			0x4a // lock voice params until next trigger
//SysEx
#define SYSEX_REQUEST_STEP_DATA			0x01
#define SYSEX_SEND_STEP_DATA			0x02
#define SYSEX_REQUEST_MAIN_STEP_DATA		0x03
#define SYSEX_SEND_MAIN_STEP_DATA		0x04
#define SYSEX_REQUEST_PATTERN_DATA		0x05
#define SYSEX_SEND_PAT_LEN_DATA			0x06
#define SYSEX_SEND_PAT_SCALE_DATA		0x07
// bc - added to deal with changing only some patterns. let the mainboard
// know the pattern transmit is done and which voices should change
#define SYSEX_BEGIN_PATTERN_TRANSMIT		0x08
#define SYSEX_END_PATTERN_TRANSMIT		0x09
#define SYSEX_SEND_PATTERN_AND_VOICE		0x10
#define SYSEX_SEND_PATTERN_NEXT			0x11
#define SYSEX_SEND_PATTERN_REPEAT		0x12

#define SYSEX_ACTIVE_MODE_NONE			0x7f	/**< a placeholder message indicating that sysex is active but no mode is selected yet*/

/** a struct defining a standard midi message*/
typedef struct MidiStruct {
	uint8_t status;
	uint8_t data1;
	uint8_t data2;
} MidiMsg;

/** parse incoming data from the cortex*/
void frontPanel_parseData(uint8_t data);

/** send messages to the cortex chip*/
void frontPanel_sendMidiMsg(MidiMsg msg);
void frontPanel_sendData(uint8_t status, uint8_t data1, uint8_t data2);
void frontPanel_sendByte(uint8_t data);
void frontPanel_updatePatternLeds();
void frontPanel_updateActiveStepLeds();
void frontPanel_updateSubstepLeds();
void frontPanel_sendMacro(uint8_t whichMacro,uint8_t value);

extern volatile MidiMsg frontParser_midiMsg;

extern void midiMsg_checkLongOps();

#endif /* FRONTPANELPARSER_H_ */
