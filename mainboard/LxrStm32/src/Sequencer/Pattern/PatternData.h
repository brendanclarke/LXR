/*
 * PatternData.h
 *
 * Pattern storage owns the persistent pattern bank, the temp snapshot, and
 * the public helpers that read or mutate pattern payloads.
 */

#ifndef PATTERNDATA_H_
#define PATTERNDATA_H_

#include "stm32f4xx.h"

/* Number of sequencer tracks stored in every pattern. */
#define NUM_TRACKS 7
/* Number of persistent pattern slots. */
#define NUM_PATTERN 8
/* Temp slot sentinel used by the temp playback path. */
#define SEQ_TMP_PATTERN 8
/* Number of step positions per track. */
#define NUM_STEPS 128

/* Default note used when a step is cleared. */
#define SEQ_DEFAULT_NOTE 63

/* Bit mask for the active-step flag stored in volume. */
#define STEP_ACTIVE_MASK 0x80
/* Bit mask for the editable volume value stored in volume. */
#define STEP_VOLUME_MASK 0x7f

/* One editable step payload.
   volume carries the active bit and the audible volume.
   prob stores note probability.
   note stores the step note.
   param1Nr/param1Val and param2Nr/param2Val store the two automation lanes. */
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

/* Pattern-level chain settings.
   changeBar selects the repeat interval.
   nextPattern selects the next pattern in the chain. */
typedef struct PatternSettingsStruct
{
   uint8_t changeBar;
   uint8_t nextPattern;
} PatternSetting;

/* Encoded track length, scale, and rotation state. */
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

/* Persistent storage for all normal patterns.
   pat_subStepPattern holds the step payloads.
   pat_mainSteps holds the 16-step masks.
   pat_patternSettings holds the next/repeat chain.
   pat_patternLengthRotate holds the per-track length/scale/rotation state. */
typedef struct PatternSetStruct
{
   Step pat_subStepPattern[NUM_PATTERN][NUM_TRACKS][NUM_STEPS];
   uint16_t pat_mainSteps[NUM_PATTERN][NUM_TRACKS];
   PatternSetting pat_patternSettings[NUM_PATTERN];
   LengthRotate pat_patternLengthRotate[NUM_PATTERN][NUM_TRACKS];
} PatternSet;

/* Temp snapshot storage used by temp playback and temp copy/paste. */
typedef struct TempPatternStruct
{
   Step pat_subStepPattern[NUM_TRACKS][NUM_STEPS];
   uint16_t pat_mainSteps[NUM_TRACKS];
   PatternSetting pat_patternSettings;
   LengthRotate pat_patternLengthRotate[NUM_TRACKS];
} TempPattern;

/* Persistent pattern bank owned by PatternData.c. */
extern PatternSet pat_patternSet;
/* Temp snapshot image owned by PatternData.c. */
extern TempPattern pat_tmpPattern;

/* Normalize a raw pattern number while preserving the temp sentinel.
   pattern: raw pattern index or SEQ_TMP_PATTERN.
   Returns the temp sentinel unchanged or a normal slot masked to 0-7. */
uint8_t pat_normalizePatternNumber(uint8_t pattern);

/* Get a step payload pointer from a normal or temp pattern.
   pattern: source pattern index or SEQ_TMP_PATTERN.
   track: track index inside the pattern.
   step: absolute step index. Returns the selected step storage cell. */
Step* pat_getStepPtr(uint8_t pattern, uint8_t track, uint8_t step);

/* Get a length/rotation pointer from a normal or temp pattern.
   pattern: source pattern index or SEQ_TMP_PATTERN.
   track: track index inside the pattern. Returns the selected length/rotation cell. */
LengthRotate* pat_getLengthRotatePtr(uint8_t pattern, uint8_t track);

/* Get the pattern settings for a normal or temp pattern.
   pattern: source pattern index or SEQ_TMP_PATTERN. Returns the settings cell. */
PatternSetting* pat_getPatternSettingPtr(uint8_t pattern);

/* Read the 16-bit main-step mask for one track.
   pattern: source pattern index or SEQ_TMP_PATTERN.
   track: track index inside the pattern. Returns the stored main-step mask. */
uint16_t pat_getMainSteps(uint8_t pattern, uint8_t track);

/* Write the 16-bit main-step mask for one track.
   pattern: source pattern index or SEQ_TMP_PATTERN.
   track: track index inside the pattern.
   steps: new main-step mask to store. */
void pat_setMainSteps(uint8_t pattern, uint8_t track, uint16_t steps);

/* Initialize all pattern storage.
   Seeds the normal patterns with self-chaining defaults and resets the temp
   slot into its dedicated hold state. */
void pat_initPatternData(void);

/* Paste the temp image into a normal pattern slot.
   pattern: destination normal pattern index. The temp slot itself is ignored. */
void pat_applyTmpPatternTo(uint8_t pattern);

/* Paste the temp image into the currently active pattern slot.
   This is the public temp-paste entry point used by the sequencer. */
void pat_activateTmpPattern(void);

/* Set the track length for the currently shown pattern.
   trackNr: track index to edit.
   length: encoded length value, with 16 mapped to 0 before storage. */
void pat_setTrackLength(uint8_t trackNr, uint8_t length);

/* Read the decoded track length for the currently shown pattern.
   trackNr: track index to inspect. Returns the stored length, with 0 mapped
   back to 16. */
uint8_t pat_getTrackLength(uint8_t trackNr);

/* Set the track scale for the currently shown pattern.
   trackNr: track index to edit.
   scale: encoded scale value, clamped to the supported range. */
void pat_setTrackScale(uint8_t trackNr, uint8_t scale);

/* Read the track scale for the currently shown pattern.
   trackNr: track index to inspect. Returns the stored scale value. */
uint8_t pat_getTrackScale(uint8_t trackNr);

/* Set the track rotation for the currently shown pattern.
   trackNr: track index to edit.
   rot: new encoded rotation value. */
void pat_setTrackRotation(uint8_t trackNr, const uint8_t rot);

/* Set the transport loop length.
   length: zero clears the loop; non-zero values are applied immediately or
   staged as pending when a loop is already active. */
void pat_setLoop(uint8_t length);

/* Read the track rotation for the currently shown pattern.
   trackNr: track index to inspect. Returns the stored rotation value. */
uint8_t pat_getTrackRotation(uint8_t trackNr);

/* Toggle one step's active flag.
   voice: track index.
   stepNr: absolute step index.
   patternNr: pattern to edit. */
void pat_toggleStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr);

/* Toggle one main-step bit.
   voice: track index.
   stepNr: main-step index.
   patternNr: pattern to edit. */
void pat_toggleMainStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr);

/* Set or clear one main-step bit.
   patternNr: pattern to edit.
   voice: track index.
   stepNr: main-step index.
   onOff: non-zero sets the bit, zero clears it. */
void pat_setMainStep(uint8_t patternNr, uint8_t voice, uint8_t stepNr, uint8_t onOff);

/* Test whether a step is active.
   voice: track index.
   stepNr: absolute step index.
   patternNr: pattern to inspect. Returns non-zero if the active bit is set. */
uint8_t pat_isStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr);

/* Test whether a main-step bit is active.
   voice: track index.
   mainStepNr: main-step index.
   pattern: pattern to inspect. Returns non-zero if the bit is set. */
uint8_t pat_isMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern);

/* Clear one track in one pattern.
   trackNr: track to reset.
   pattern: pattern to reset. */
void pat_clearTrack(uint8_t trackNr, uint8_t pattern);

/* Clear one automation lane for one track.
   trackNr: track to reset.
   pattern: pattern to edit.
   automTrack: automation lane selector, 0 for param1 and non-zero for param2. */
void pat_clearAutomation(uint8_t trackNr, uint8_t pattern, uint8_t automTrack);

/* Clear every track in one pattern.
   pattern: pattern to reset. */
void pat_clearPattern(uint8_t pattern);

/* Copy one track between tracks inside the same pattern.
   srcNr: source track.
   dstNr: destination track.
   pattern: pattern that owns both tracks. */
void pat_copyTrack(uint8_t srcNr, uint8_t dstNr, uint8_t pattern);

/* Copy one whole pattern into another.
   src: source pattern index.
   dst: destination pattern index. Copies step data, main steps, and
   per-track length/rotation state. Temp copies also trigger kit-state capture. */
void pat_copyPattern(uint8_t src, uint8_t dst);

/* Copy the live or selected image into the temp slot.
   srcPattern: fallback source pattern when playback is stopped.
   When playback is running, the currently audible per-track sources are copied
   instead. The temp snapshot is forced into the hold state after the copy. */
void pat_copyToTmpPattern(uint8_t srcPattern);

/* Copy one track from one pattern into another pattern.
   srcNr: track to copy.
   dstPat: destination pattern.
   srcPat: source pattern that owns the track data. */
void pat_copyTrackPattern(uint8_t srcNr, uint8_t dstPat, uint8_t srcPat);

/* Copy one step between absolute step positions in the active source pattern.
   srcStep: source step index.
   dstStep: destination step index.
   activeTrack: track whose live source pattern should be used. */
void pat_copySubStep(uint8_t srcStep, uint8_t dstStep, uint8_t activeTrack);

#endif /* PATTERNDATA_H_ */
