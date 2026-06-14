/*
 * PatternData.h
 *
 * Owns the sequencer pattern storage model and the public helpers that mutate
 * or query pattern payloads.
 */

#ifndef SEQUENCER_PATTERNDATA_H_
#define SEQUENCER_PATTERNDATA_H_

#include "stm32f4xx.h"

#define NUM_TRACKS 7
#define NUM_PATTERN 8
#define SEQ_TMP_PATTERN 8
#define NUM_STEPS 128

#define SEQ_DEFAULT_NOTE 63

#define STEP_ACTIVE_MASK 0x80
#define STEP_VOLUME_MASK 0x7f

typedef struct StepStruct
{
   uint8_t volume;
   uint8_t prob;
   uint8_t note;

   uint8_t param1Nr;
   uint8_t param1Val;

   uint8_t param2Nr;
   uint8_t param2Val;
} Step;

typedef struct PatternSettingsStruct
{
   uint8_t changeBar;
   uint8_t nextPattern;
} PatternSetting;

typedef union
{
   uint8_t value;
   struct
   {
      unsigned length:4;
      unsigned scale:3;
      unsigned rotate:4;
   };
} LengthRotate;

typedef struct PatternSetStruct
{
   Step seq_subStepPattern[NUM_PATTERN][NUM_TRACKS][NUM_STEPS];
   uint16_t seq_mainSteps[NUM_PATTERN][NUM_TRACKS];
   PatternSetting seq_patternSettings[NUM_PATTERN];
   LengthRotate seq_patternLengthRotate[NUM_PATTERN][NUM_TRACKS];
} PatternSet;

typedef struct TempPatternStruct
{
   Step seq_subStepPattern[NUM_TRACKS][NUM_STEPS];
   uint16_t seq_mainSteps[NUM_TRACKS];
   PatternSetting seq_patternSettings;
   LengthRotate seq_patternLengthRotate[NUM_TRACKS];
} TempPattern;

extern PatternSet seq_patternSet;
extern TempPattern seq_tmpPattern;

uint8_t seq_normalizePatternNumber(uint8_t pattern);
Step* seq_getStepPtr(uint8_t pattern, uint8_t track, uint8_t step);
LengthRotate* seq_getLengthRotatePtr(uint8_t pattern, uint8_t track);
PatternSetting* seq_getPatternSettingPtr(uint8_t pattern);
uint16_t seq_getMainSteps(uint8_t pattern, uint8_t track);
void seq_setMainSteps(uint8_t pattern, uint8_t track, uint16_t steps);

void seq_initPatternData(void);
void seq_applyTmpPatternTo(uint8_t pattern);
void seq_activateTmpPattern(void);

void seq_setTrackLength(uint8_t trackNr, uint8_t length);
uint8_t seq_getTrackLength(uint8_t trackNr);
void seq_setTrackScale(uint8_t trackNr, uint8_t scale);
uint8_t seq_getTrackScale(uint8_t trackNr);
void seq_setTrackRotation(uint8_t trackNr, const uint8_t rot);
void seq_setLoop(uint8_t length);
uint8_t seq_getTrackRotation(uint8_t trackNr);

void seq_toggleStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr);
void seq_toggleMainStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr);
void seq_setMainStep(uint8_t patternNr, uint8_t voice, uint8_t stepNr, uint8_t onOff);
uint8_t seq_isStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr);
uint8_t seq_isMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern);
void seq_clearTrack(uint8_t trackNr, uint8_t pattern);
void seq_clearAutomation(uint8_t trackNr, uint8_t pattern, uint8_t automTrack);
void seq_clearPattern(uint8_t pattern);
void seq_copyTrack(uint8_t srcNr, uint8_t dstNr, uint8_t pattern);
void seq_copyPattern(uint8_t src, uint8_t dst);
void seq_copyTrackPattern(uint8_t srcNr, uint8_t dstPat, uint8_t srcPat);
void seq_copySubStep(uint8_t srcStep, uint8_t dstStep, uint8_t activeTrack);

#endif /* SEQUENCER_PATTERNDATA_H_ */
