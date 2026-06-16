/*
 * frontPanelReceivingProtocol.h
 *
 * Front-panel receive/protocol boundary owned by the uARTFrontSYX layer.
 *
 * This header owns the STM front-panel opcode namespace and the receive-side
 * parser entry points. Uart.c/.h remains raw transport. Outbound packet
 * construction is owned by frontPanelSendingProtocol.c/.h.
 */

#ifndef UARTFRONTSYX_FRONTPANELRECEIVINGPROTOCOL_H_
#define UARTFRONTSYX_FRONTPANELRECEIVINGPROTOCOL_H_

#include <stdint.h>

// Cortex <-> Front messages

// control messages from cortex for leds
// status
#define FRONT_SEQ_VOICE_MORPH           0xab // automation morph drum voice
#define FRONT_CC_MACRO_TARGET          0xaa // performance macro changes to destination or main macro control, not amount
/* MACRO_CC message structure
byte1 - status byte 0xaa as above
byte2, data1 byte: xttaaa-b : tt= top level macro value sent (2 macros exist now, we can do 2 more if we want)
                              aaa= macro destination value sent (4 destinations exist now, can do 8)
                              b=macro mod target value top bit
                              I have left a blank bit above this to make it easier to make more than 255 kit parameters
                              if we ever want to take on that can of worms
                              
byte3, data2 byte: xbbbbbbb : b=macro mod target value lower 7 bits or top level value full
*/

#define FRONT_STEP_LED_STATUS_BYTE 		0xb1
#define FRONT_SEQ_CC					      0xb2
#define FRONT_CODEC_CONTROL				0xb3
#define VOICE_CC						      0xb4
#define BANK_CHANGE_CC                  0xad
#define PARAM_CC                        0xae
#define PARAM_CC2                       0xaf
#define FRONT_SET_BPM				   	0xb5
#define FRONT_CC_2					   	0xb6	//for parameters above 127
#define FRONT_CC_LFO_TARGET				0xb7
#define FRONT_CC_VELO_TARGET		   	0xb8
#define FRONT_STEP_CC				   	0xb9	// toggle a step in the subStepPattern array
#define FRONT_SET_P1_DEST		   		0xba	// SET_P1_DEST, stepNr, destinationNr --> track(voice) via set active track cmd
#define FRONT_SET_P2_DEST				   0xbb
#define FRONT_SET_P1_VAL				   0xbc	// SET_P1_VAL, stepNr, value --> track(voice) via set active track cmd
#define FRONT_SET_P2_VAL				   0xbd
#define FRONT_MAIN_STEP_CC				   0xbe	// toggle main step
#define FRONT_ARM_AUTOMATION_STEP		0xbf	// status - stepNr - track | OnOff

#define SAMPLE_CC						      0xc0
#define PRF_RESTORE_PARAM_CC          0xc1
#define PRF_RESTORE_PARAM_CC2         0xc2
#define PRF_RESTORE_MORPH_CC          0xc3
#define PRF_RESTORE_MORPH_CC2         0xc4
#define PRF_CACHE_STATUS              0xc5
#define PARAM_RESTORE_DONE            0xc6
#define PARAM_RESTORE_BEGIN           0xc7
#define PARAM_RESTORE_READY           0xc8
#define PARAM_RESTORE_ACK             0xc9
#define PRF_CACHE_REJECTED            0x00
#define PRF_CACHE_ACCEPTED            0x01
#define FRONT_SAMPLE_START_UPLOAD 		0x01
#define FRONT_SAMPLE_COUNT		 		   0x02

// message
#define FRONT_CURRENT_STEP_NUMBER_CC	0x01	/**< send the current active chase light step number to the frontplate*/
#define FRONT_LED_SEQ_BUTTON			   0x02	/**< turn on a step seq. led*/
#define FRONT_LED_QUERY_SEQ_TRACK		0x03	/**< the frontpanel wants to know whick seq. leds should be lit*/
#define FRONT_LED_PULSE_BEAT			   0x04	/**< pulse the beat indicator LED*/
#define FRONT_LED_SEQ_SUB_STEP	      0x05
#define FRONT_LED_ALL_SUBSTEP          0x3f  // send back all the substeps

#define FRONT_LED_SEQ_MAIN_ONE         0x40  // bc - send as 4-led sets to prevent message choke
#define FRONT_LED_SEQ_MAIN_TWO         0x41
#define FRONT_LED_SEQ_MAIN_THREE       0x42
#define FRONT_LED_SEQ_MAIN_FOUR        0x43

#define FRONT_LED_SEQ_SUB_STEP_LOWER	0x44  // sending single substeps was slow - send as two messages of 4 leds each
#define FRONT_LED_SEQ_SUB_STEP_UPPER	0x45  // nb - can't fit all 8 b/c midi data2 is only 7 bits
#define FRONT_LCD_PRINT_SCREEN	0x46

// Sequencer commands
#define FRONT_SEQ_RUN_STOP				   0x01

#define FRONT_SEQ_MUTE_TRACK			   0x09
#define FRONT_SEQ_UNMUTE_TRACK			0x0a
#define FRONT_SEQ_CHANGE_PAT			   0x0b	/**< the user requested a new pattern. send the same message back to the front as ack that the new pattern is loaded*/
#define FRONT_SEQ_CHANGE_TMP_PAT		   0x0e	/**< request pat_tmpPattern as a temporary ninth performance pattern */
#define FRONT_SEQ_ROLL_ON			   	0x0c	/**< start roll for voice data2*/
#define FRONT_SEQ_ROLL_OFF			   	0x0d	/**< stop roll for voice data2*/
#define FRONT_SEQ_REQUEST_STEP_PARAMS 	0x0f
#define FRONT_SEQ_ROLL_ON_OFF		   	0x10	/**< turn voice roll/flamm on/off. data 2 parameter is bit 0 to 3 = voice number, bit 4 is on/off flag*/
#define FRONT_SEQ_ROLL_RATE				0x11
#define FRONT_SEQ_VOLUME			   	0x12
#define FRONT_SEQ_NOTE				   	0x13
#define FRONT_SEQ_PROB				   	0x14
#define FRONT_SEQ_SET_ACTIVE_TRACK 		0x15	/**< select the active track. all track specific messages (request step params etc) received will refer to the track selected with this command*/
//#define FRONT_SEQ_RESYNC_LFO			0x16	/**< LFO is no longer running on the front */
#define FRONT_SEQ_EUKLID_LENGTH 		   0x17	/** sets the length of the current track from 0 to 16 steps*/
#define FRONT_SEQ_EUKLID_STEPS			0x18
#define FRONT_SEQ_REQUEST_EUKLID_PARAMS 0x19
#define FRONT_SEQ_SET_SHOWN_PATTERN		0x1A

#define FRONT_SEQ_REC_ON_OFF			   0x1B	/**< start(data2=1) or stop(data2=0) recording mode */
#define FRONT_SEQ_REQUEST_PATTERN_PARAMS 0x1C 	/**< the sequencer sends back the data of the active pattern */
#define FRONT_SEQ_SET_PAT_BEAT			0x1D	/**< on every Nth bar the pattern will change to the next pattern*/
#define FRONT_SEQ_SET_PAT_NEXT			0x1E	/**< the next pattern that will be played when the current finishes*/
#define FRONT_SEQ_CLEAR_TRACK			   0x1f
#define FRONT_SEQ_COPY_TRACK			   0x20
#define FRONT_SEQ_COPY_PATTERN			0x21
#define FRONT_SEQ_SET_QUANT				0x22
#define FRONT_SEQ_SET_AUTOM_TRACK		0x23 	// SEQ_CC, SEQ_SET_AUTOM_TRACK, autoTrkNr
#define FRONT_SEQ_SELECT_ACTIVE_STEP 	0x24
#define FRONT_SEQ_SHUFFLE				   0x25
#define FRONT_SEQ_TRACK_LENGTH			0x26
#define FRONT_SEQ_TRACK_SCALE          0x3c
#define FRONT_SEQ_CLEAR_PATTERN			0x27
#define FRONT_SEQ_CLEAR_AUTOM			   0x28 	//voice nr (0xf0) + autom track nr (0x0f)

#define FRONT_SEQ_POSX					   0x29
#define FRONT_SEQ_POSY					   0x2a
#define FRONT_SEQ_FLUX					   0x2b
#define FRONT_SEQ_SOM_FREQ				   0x2c
#define FRONT_SEQ_MIDI_CHAN				0x2d	//voiceNr (0xf0) + channel (0x0f). --AS voice 7=global channel
#define FRONT_SEQ_MIDI_MODE				0x2e //--AS not used anymore
#define FRONT_SEQ_MIDI_ROUTING			0x2f	// midi routing
#define FRONT_SEQ_MIDI_TX_FILTER		   0x30    // tx filtering
#define FRONT_SEQ_MIDI_RX_FILTER		   0x31    // rx filtering
#define FRONT_SEQ_BAR_RESET_MODE		   0x32	// --AS reset bar on manual pattern change (0 is default - to not reset)
#define FRONT_SEQ_ERASE_ON_OFF			0x33    // --AS turn erase mode on/off
#define FRONT_SEQ_TRACK_ROTATION		   0x34	// --AS rotate a track's start position 0 to 15
#define FRONT_SEQ_EUKLID_ROTATION		0x35
#define FRONT_SEQ_EUKLID_SUBSTEP_ROTATION 0x46
#define FRONT_SEQ_EUKLID_RESET         0x47

#define FRONT_SEQ_TRIGGER_IN_PPQ		   0x36
#define FRONT_SEQ_TRIGGER_OUT1_PPQ 		0x37
#define FRONT_SEQ_TRIGGER_OUT2_PPQ 		0x38
#define FRONT_SEQ_TRIGGER_GATE_MODE 	0x39

#define FRONT_SEQ_COPY_TRACK_PATTERN   0x3a // added message for single track pattern copy
#define FRONT_SEQ_PC_TIME_MODE         0x3b // setting for change pattern on bar or step
#define FRONT_SEQ_COPY_SRC            0x3d // added message for copy step
#define FRONT_SEQ_COPY_DST            0x3e

#define FRONT_SEQ_ROLL_NOTE            0x40
#define FRONT_SEQ_ROLL_VELOCITY        0x41
#define FRONT_SEQ_ROLL_MODE            0x42
#define FRONT_SEQ_TRANSPOSE            0x43
#define FRONT_SEQ_TRANSPOSE_ON_OFF     0x44
#define FRONT_SEQ_SET_LOOP             0x45
#define FRONT_SEQ_LOAD_VOICE             0x48
#define FRONT_SEQ_UNHOLD_VOICE             0x49
#define FRONT_SEQ_FILE_BEGIN             0x4f
#define FRONT_SEQ_LOAD_FAST             0x50
#define FRONT_SEQ_FILE_DONE             0x51
#define FRONT_SEQ_TRACK_NOTE1		0x52
#define FRONT_SEQ_TRACK_NOTE2		0x53
#define FRONT_SEQ_TRACK_NOTE3		0x54
#define FRONT_SEQ_TRACK_NOTE4		0x55
#define FRONT_SEQ_TRACK_NOTE5		0x56
#define FRONT_SEQ_TRACK_NOTE6		0x57
#define FRONT_SEQ_TRACK_NOTE7		0x58
#define FRONT_SEQ_MIDI_CHAN_OFF         0x59
#define FRONT_SEQ_FLOW_BEGIN            0x5a
#define FRONT_SEQ_FLOW_GRANT            0x5b
#define FRONT_SEQ_FLOW_END              0x5c
#define FRONT_SEQ_FLOW_ABORT            0x5d
#define FRONT_SEQ_PRF_CACHE_BEGIN       0x5e
#define FRONT_SEQ_PRF_PENDING_BEGIN     0x5f
#define FRONT_SEQ_PRF_PENDING_DONE      0x60
#define FRONT_SEQ_PRF_CACHE_ABORT       0x61
#define FRONT_SEQ_PRF_AVR_SNAPSHOT_BEGIN 0x62
#define FRONT_SEQ_PRF_AVR_SNAPSHOT_END   0x63
#define FRONT_SEQ_PRF_RESTORE_AVR_LIVE   0x64
#define FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN 0x65
#define FRONT_SEQ_TMP_KIT_ENDPOINT_END   0x66
#define FRONT_SEQ_TMP_KIT_AUTOMATION_PHASE 0x67
#define FRONT_SEQ_SET_GLOBAL_MORPH       0x68
#define FRONT_SEQ_SET_GLOBAL_MORPH_LSB   0x69
#define FRONT_SEQ_SET_GLOBAL_MORPH_MSB   0x6a
#define FRONT_SEQ_REPORT_GLOBAL_MORPH_LSB 0x6b
#define FRONT_SEQ_REPORT_GLOBAL_MORPH_MSB 0x6c

#define FRONT_SEQ_TMP_KIT_ENDPOINT_BOTH 0x00
#define FRONT_SEQ_TMP_KIT_ENDPOINT_FRONT_ONLY 0x01
#define FRONT_SEQ_TMP_KIT_ENDPOINT_MORPH_ONLY 0x02

#define FRONT_SEQ_TMP_KIT_AUTOMATION_NONE 0x00
#define FRONT_SEQ_TMP_KIT_AUTOMATION_FRONT_ENDPOINT 0x01
#define FRONT_SEQ_TMP_KIT_AUTOMATION_MORPH_ENDPOINT 0x02

#define FLOW_CH_LOAD_SESSION            0x00
#define FLOW_CH_GLOBALS                 0x01
#define FLOW_CH_VOICE_PARAM             0x02
#define FLOW_CH_DRUM_META               0x03

#include "MidiMessages.h"

/* Shared receive-side front-panel state. These globals are owned by the
   receive protocol while legacy frontParser_* call sites remain. */
extern uint8_t frontParser_rxCnt;
/* Three-byte front-panel command assembly. The storage is MIDI-shaped because
   the legacy wire format is MIDI-shaped, but this is AVR/STM protocol state. */
extern MidiMsg frontParser_command;
extern uint8_t frontParser_activeTrack;
extern uint8_t frontParser_shownPattern;
extern uint8_t frontParser_sysexActive;
extern uint8_t frontParser_activeFrontTrack;
extern uint16_t frontParser_twoByteData;
extern uint8_t frontParser_sysexBuffer[16];
extern uint16_t frontParser_sysexSeqStepNr;
extern uint8_t frontParser_activeStep;
extern uint8_t frontParser_stepCopySource;
extern uint8_t frontParser_originalCcValues[0xff];
extern uint8_t midi_envPosition[6];

/* Receive-side entry points and parser state queries. */
void frontParser_parseUartData(unsigned char data);
void frontParser_handleMidiMessage(void);
void frontParser_applyParameterCommand(MidiMsg msg, uint8_t updateOriginalValue);
uint8_t frontParser_isQuietUi();

#endif /* UARTFRONTSYX_FRONTPANELRECEIVINGPROTOCOL_H_ */
