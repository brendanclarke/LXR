/*
 * PresetManager.h
 *
 * Created: 09.05.2012 14:11:49
 *  Author: Julian
 */ 


#ifndef PRESETMANAGER_H_
#define PRESETMANAGER_H_

#include <avr/io.h>
#include "../Menu/menu.h"


extern char preset_currentName[8];
extern char preset_currentSaveMenuName[8];

extern uint8_t parameter_values_fileLoadSnapshot[END_OF_SOUND_PARAMETERS];
extern uint8_t parameters2_fileLoadSnapshot[END_OF_SOUND_PARAMETERS];
extern uint8_t preset_workingVoiceArray;

void preset_init();


/** save the parameters of all 6 voices + LFO settings to a file on the SD card 
if isMorph==1 the sound will be loaded into the morph buffer
*/
void preset_saveDrumset(uint8_t presetNr, uint8_t isMorph);
uint8_t preset_loadDrumset(uint8_t presetNr, uint8_t voiceArray, uint8_t isMorph);
void preset_loadVoice(uint8_t presetNr, uint8_t voiceArray, uint8_t isMorph);

void preset_saveGlobals();
void preset_loadGlobals();

void preset_saveAll(uint8_t presetNr, uint8_t isAll);

uint8_t preset_loadAll(uint8_t presetNr, uint8_t voiceArray);
uint8_t preset_loadPerf(uint8_t presetNr, uint8_t voiceArray);
void preset_backgroundSwapDoneFromStm(uint8_t fileType);
uint8_t preset_isBackgroundTempPlaybackActive(void);
void preset_notePlayedPatternChanged(uint8_t playedPattern);

char* preset_loadName(uint8_t presetNr, uint8_t what, uint8_t loadSave);

/** save a pattern set to the sd card */
void preset_savePattern(uint8_t presetNr);
uint8_t preset_loadPattern(uint8_t presetNr, uint8_t voiceArray);
/** morph pattern linear to �pattern buffer 2*/

void preset_readDrumVoice(uint8_t track, uint8_t isMorph);
void preset_readDrumVoice2(uint8_t track, uint8_t isMorph);
void preset_morph(uint8_t voiceArray, uint8_t morph);
uint8_t preset_getMorphValue(uint16_t index, uint8_t morph);

void preset_dumpNormalEndpointsToStm();

#endif /* PRESETMANAGER_H_ */
