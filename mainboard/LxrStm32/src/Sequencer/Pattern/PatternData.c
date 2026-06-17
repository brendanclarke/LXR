/*
 * PatternData.c
 *
 * Owns the sequencer pattern payloads and the helpers that mutate them.
 */

#include "PatternData.h"
#include "MIDI/MidiMessages.h"
#include "Preset/KitState.h"

#include <string.h>

/* Sequencer state that PatternData reads when it needs live playback context
   or UI selection context. These values are owned by sequencer.c. */
extern uint8_t seq_activePattern;                /* Currently selected active pattern index. */
extern uint8_t seq_running;                      /* Non-zero while playback is running. */
extern uint8_t seq_perTrackActivePattern[NUM_TRACKS]; /* Live per-track source pattern table. */
extern int8_t seq_stepIndex[NUM_TRACKS+1];       /* Live step cursor for each track and master clock. */
extern uint8_t seq_loopLength;                   /* Current loop length used by the transport. */
extern uint8_t seq_pendingLoopLength;            /* Deferred loop length waiting for commit. */
extern int8_t seq_loopCurrentPosition;           /* Current loop playback position. */
extern int8_t seq_loopUpdateFlag;                /* Transport flag used to publish loop changes. */
extern uint8_t frontParser_shownPattern;         /* Pattern currently shown in the front-panel UI. */

/* Persistent storage for the eight normal patterns. */
PatternSet pat_patternSet;
/* Scratch storage for the temporary playback pattern image. */
TempPattern pat_tmpPattern;

/* Force the temp pattern to stay latched until a manual change replaces it. */
static void pat_setTmpPatternHoldSettings(void)
{
   pat_tmpPattern.pat_patternSettings.changeBar = 0;
   pat_tmpPattern.pat_patternSettings.nextPattern = SEQ_TMP_PATTERN;
}

/* Restore a step payload to the default inactive note/automation state. */
static void pat_resetNote(Step *step)
{
   step->note = SEQ_DEFAULT_NOTE;
   step->param1Nr = NO_AUTOMATION;
   step->param1Val = 0;
   step->param2Nr = NO_AUTOMATION;
   step->param2Val = 0;
   step->prob = 127;
   step->volume = 100;
}

/* Normalize a pattern index while preserving the temp sentinel. */
uint8_t pat_normalizePatternNumber(uint8_t pattern)
{
   if(pattern == SEQ_TMP_PATTERN)
      return SEQ_TMP_PATTERN;
   return pattern & 0x07;
}

/* Return the step slot for a normal or temp pattern.
   pattern: source pattern index or SEQ_TMP_PATTERN.
   track: track within the pattern.
   step: absolute step index. Returns the matching step storage cell. */
Step* pat_getStepPtr(uint8_t pattern, uint8_t track, uint8_t step)
{
   pattern = pat_normalizePatternNumber(pattern);
   if(pattern == SEQ_TMP_PATTERN)
      return &pat_tmpPattern.pat_subStepPattern[track][step & 0x7f];
   return &pat_patternSet.pat_subStepPattern[pattern][track][step & 0x7f];
}

/* Return the length/rotation cell for a normal or temp pattern.
   pattern: source pattern index or SEQ_TMP_PATTERN.
   track: track within the pattern. Returns the matching length/rotation cell. */
LengthRotate* pat_getLengthRotatePtr(uint8_t pattern, uint8_t track)
{
   pattern = pat_normalizePatternNumber(pattern);
   if(pattern == SEQ_TMP_PATTERN)
      return &pat_tmpPattern.pat_patternLengthRotate[track];
   return &pat_patternSet.pat_patternLengthRotate[pattern][track];
}

/* Return the pattern-level settings for a normal or temp pattern.
   pattern: source pattern index or SEQ_TMP_PATTERN. Returns the settings cell. */
PatternSetting* pat_getPatternSettingPtr(uint8_t pattern)
{
   pattern = pat_normalizePatternNumber(pattern);
   if(pattern == SEQ_TMP_PATTERN)
      return &pat_tmpPattern.pat_patternSettings;
   return &pat_patternSet.pat_patternSettings[pattern];
}

/* Return the 16-bit main-step mask for one track.
   pattern: source pattern index or SEQ_TMP_PATTERN.
   track: track within that pattern. Returns the stored main-step bitmap. */
uint16_t pat_getMainSteps(uint8_t pattern, uint8_t track)
{
   pattern = pat_normalizePatternNumber(pattern);
   if(pattern == SEQ_TMP_PATTERN)
      return pat_tmpPattern.pat_mainSteps[track];
   return pat_patternSet.pat_mainSteps[pattern][track];
}

/* Store the 16-bit main-step mask for one track.
   pattern: source pattern index or SEQ_TMP_PATTERN.
   track: track within that pattern.
   steps: new main-step bitmap to write. */
void pat_setMainSteps(uint8_t pattern, uint8_t track, uint16_t steps)
{
   pattern = pat_normalizePatternNumber(pattern);
   if(pattern == SEQ_TMP_PATTERN)
      pat_tmpPattern.pat_mainSteps[track] = steps;
   else
      pat_patternSet.pat_mainSteps[pattern][track] = steps;
}

/* Clear and initialize all persistent pattern storage.
   This seeds every normal pattern with self-chaining settings and resets the
   temp image into the dedicated hold state. */
void pat_initPatternData(void)
{
   uint8_t i;

   for(i=0;i<NUM_PATTERN;i++)
   {
      pat_patternSet.pat_patternSettings[i].changeBar = 0;
      pat_patternSet.pat_patternSettings[i].nextPattern = i;
      pat_clearPattern(i);
   }

   pat_setTmpPatternHoldSettings();
   pat_clearPattern(SEQ_TMP_PATTERN);
}

/* Copy the temp image into one normal pattern slot.
   pattern: destination normal pattern index. The temp slot itself is ignored. */
void pat_applyTmpPatternTo(uint8_t pattern)
{
   if(pattern == SEQ_TMP_PATTERN)
      return;

   pattern &= 0x07;
   memcpy(&pat_patternSet.pat_subStepPattern[pattern],
          &pat_tmpPattern.pat_subStepPattern,
          sizeof(Step) * NUM_TRACKS * NUM_STEPS);
   memcpy(&pat_patternSet.pat_mainSteps[pattern],
          &pat_tmpPattern.pat_mainSteps,
          sizeof(uint16_t) * NUM_TRACKS);
   memcpy(&pat_patternSet.pat_patternSettings[pattern],
          &pat_tmpPattern.pat_patternSettings,
          sizeof(PatternSetting));
   memcpy(&pat_patternSet.pat_patternLengthRotate[pattern],
          &pat_tmpPattern.pat_patternLengthRotate,
          sizeof(LengthRotate) * NUM_TRACKS);
}

/* Apply the temp image to the currently active pattern slot.
   This is the public temp-paste entry point used by the sequencer. */
void pat_activateTmpPattern(void)
{
   pat_applyTmpPatternTo(seq_activePattern);
}

/* Write a track length into the currently shown pattern.
   trackNr: track to edit. length: encoded length value, with 16 mapped to 0. */
void pat_setTrackLength(uint8_t trackNr, uint8_t length)
{
   if(length == 16)
      length = 0;
   pat_getLengthRotatePtr(frontParser_shownPattern, trackNr)->length = length;
}

/* Write a track scale into the currently shown pattern.
   trackNr: track to edit. scale: new scale value, clamped to the supported range. */
void pat_setTrackScale(uint8_t trackNr, uint8_t scale)
{
   if(scale > 7)
      scale = 7;

   pat_getLengthRotatePtr(frontParser_shownPattern, trackNr)->scale = scale;
}

/* Read the user-facing track length from the currently shown pattern.
   trackNr: track to inspect. Returns the decoded length, where 0 maps to 16. */
uint8_t pat_getTrackLength(uint8_t trackNr)
{
   uint8_t r = pat_getLengthRotatePtr(frontParser_shownPattern, trackNr)->length;
   if(r == 0)
      return 16;
   return r;
}

/* Read the user-facing track scale from the currently shown pattern.
   trackNr: track to inspect. Returns the stored scale value. */
uint8_t pat_getTrackScale(uint8_t trackNr)
{
   return pat_getLengthRotatePtr(frontParser_shownPattern, trackNr)->scale;
}

/* Write a track rotation and keep live playback aligned if the sequencer is running.
   trackNr: track to edit.
   newRot: new encoded rotation value. */
void pat_setTrackRotation(uint8_t trackNr, const uint8_t newRot)
{
   LengthRotate *lr = pat_getLengthRotatePtr(frontParser_shownPattern, trackNr);

   if(newRot == lr->rotate)
      return;

   if(seq_running)
   {
      int8_t len = 0x1f & (lr->length);
      if(len == 0)
         len = 16;

      int8_t offset = (((int8_t)newRot) % len) - (((int8_t)lr->rotate) % len);
      int16_t si = seq_stepIndex[trackNr] + (offset * 8);
      if(si < 0)
         si += (len * 8);
      else if(si >= (len * 8))
         si -= (len * 8);
      seq_stepIndex[trackNr] = (int8_t)si;
   }

   lr->rotate = newRot;
}

/* Read the current track rotation from the shown pattern.
   trackNr: track to inspect. Returns the stored rotation value. */
uint8_t pat_getTrackRotation(uint8_t trackNr)
{
   return pat_getLengthRotatePtr(frontParser_shownPattern, trackNr)->rotate;
}

/* Update the transport loop length.
   length: new loop length. Zero clears the loop; non-zero values are stored
   immediately or staged as pending when a loop is already active. */
void pat_setLoop(uint8_t length)
{
   if(!length)
   {
      seq_loopLength = 0;
      seq_pendingLoopLength = 0;
      seq_loopCurrentPosition = 0;
   }
   else if(seq_loopLength)
   {
      seq_pendingLoopLength = length;
   }
   else
   {
      seq_loopLength = length;
      seq_pendingLoopLength = length;
      seq_loopUpdateFlag = 1;
   }
}

/* Toggle the active-step bit for one step.
   voice: track index.
   stepNr: absolute step index.
   patternNr: pattern to edit. */
void pat_toggleStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
   Step *step = pat_getStepPtr(patternNr, voice, stepNr);
   if((step->volume & STEP_ACTIVE_MASK) == 0)
   {
      step->volume |= STEP_ACTIVE_MASK;
   }
   else
   {
      step->volume &= ~STEP_ACTIVE_MASK;
   }
}

/* Toggle one main-step bit for a track.
   voice: track index.
   stepNr: main-step index.
   patternNr: pattern to edit. */
void pat_toggleMainStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
   pat_setMainSteps(patternNr, voice, (uint16_t)(pat_getMainSteps(patternNr, voice) ^ (1 << stepNr)));
}

/* Set or clear one main-step bit for a track.
   patternNr: pattern to edit.
   voice: track index.
   stepNr: main-step index.
   onOff: non-zero writes the bit, zero clears it. */
void pat_setMainStep(uint8_t patternNr, uint8_t voice, uint8_t stepNr, uint8_t onOff)
{
   uint16_t mainSteps = pat_getMainSteps(patternNr, voice);
   if(onOff)
   {
      mainSteps |= (1 << stepNr);
   }
   else
   {
      mainSteps &= ~(1 << stepNr);
   }
   pat_setMainSteps(patternNr, voice, mainSteps);
}

/* Test whether a step is active.
   voice: track index.
   stepNr: absolute step index.
   patternNr: pattern to inspect. Returns non-zero if the active bit is set. */
uint8_t pat_isStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
   return ((pat_getStepPtr(patternNr, voice, stepNr)->volume & STEP_ACTIVE_MASK) > 0);
}

/* Test whether a main-step bit is active.
   voice: track index.
   mainStepNr: main-step index.
   pattern: pattern to inspect. Returns non-zero if the bit is set. */
uint8_t pat_isMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern)
{
   return (pat_getMainSteps(pattern, voice) & (1 << mainStepNr)) > 0;
}

/* Clear one track in one pattern.
   trackNr: track to reset.
   pattern: pattern to reset. Step payloads, main steps, and rotation are
   restored to defaults. */
void pat_clearTrack(uint8_t trackNr, uint8_t pattern)
{
   int k;
   for(k=0;k<128;k++)
   {
      Step *step = pat_getStepPtr(pattern, trackNr, k);
      pat_resetNote(step);

      if((k % 8) == 0)
         step->volume |= STEP_ACTIVE_MASK;
   }

   pat_setMainSteps(pattern, trackNr, 0);
   pat_getLengthRotatePtr(pattern, trackNr)->rotate = 0;
}

/* Clear only one automation lane for one track.
   trackNr: track to reset.
   pattern: pattern to edit.
   automTrack: automation lane selector, 0 for param1 and non-zero for param2. */
void pat_clearAutomation(uint8_t trackNr, uint8_t pattern, uint8_t automTrack)
{
   int k;

   if(automTrack == 0)
   {
      for(k=0;k<128;k++)
      {
         Step *step = pat_getStepPtr(pattern, trackNr, k);
         step->param1Nr = NO_AUTOMATION;
         step->param1Val = 0;
      }
   }
   else
   {
      for(k=0;k<128;k++)
      {
         Step *step = pat_getStepPtr(pattern, trackNr, k);
         step->param2Nr = NO_AUTOMATION;
         step->param2Val = 0;
      }
   }
}

/* Clear every track in one pattern.
   pattern: pattern to reset. This is the bulk pattern-reset helper. */
void pat_clearPattern(uint8_t pattern)
{
   int i;
   for(i=0;i<NUM_TRACKS;i++)
      pat_clearTrack(i, pattern);
}

/* Copy one track between tracks inside the same pattern.
   srcNr: source track.
   dstNr: destination track.
   pattern: pattern that owns both tracks. */
void pat_copyTrack(uint8_t srcNr, uint8_t dstNr, uint8_t pattern)
{
   int k;
   Step *src, *dst;
   for(k=0;k<128;k++)
   {
      dst = pat_getStepPtr(pattern, dstNr, k);
      src = pat_getStepPtr(pattern, srcNr, k);
      dst->note = src->note;
      dst->param1Nr = src->param1Nr;
      dst->param1Val = src->param1Val;
      dst->param2Nr = src->param2Nr;
      dst->param2Val = src->param2Val;
      dst->prob = src->prob;
      dst->volume = src->volume;
   }

   pat_setMainSteps(pattern, dstNr, pat_getMainSteps(pattern, srcNr));
   pat_getLengthRotatePtr(pattern, dstNr)->value =
      pat_getLengthRotatePtr(pattern, srcNr)->value;
}

/* Copy one whole pattern into another.
   src: source pattern index.
   dst: destination pattern index. Copies step data, main steps, and per-track
   length/rotation state. Temp copies also trigger kit-state capture. */
void pat_copyPattern(uint8_t src, uint8_t dst)
{
   int k, j;
   Step *psrc, *pdst;
   uint8_t normalizedSrc = pat_normalizePatternNumber(src);
   uint8_t normalizedDst = pat_normalizePatternNumber(dst);

   for(j=0;j<NUM_TRACKS;j++)
   {
      for(k=0;k<128;k++)
      {
         pdst = pat_getStepPtr(dst, j, k);
         psrc = pat_getStepPtr(src, j, k);
         pdst->note = psrc->note;
         pdst->param1Nr = psrc->param1Nr;
         pdst->param1Val = psrc->param1Val;
         pdst->param2Nr = psrc->param2Nr;
         pdst->param2Val = psrc->param2Val;
         pdst->prob = psrc->prob;
         pdst->volume = psrc->volume;
      }

      pat_setMainSteps(dst, j, pat_getMainSteps(src, j));
      pat_getLengthRotatePtr(dst, j)->value =
         pat_getLengthRotatePtr(src, j)->value;
   }

   if((normalizedDst == SEQ_TMP_PATTERN) && (normalizedSrc != SEQ_TMP_PATTERN))
   {
      preset_captureTmpKitState();
   }
}

/* Copy the live or selected pattern image into the temp slot.
   srcPattern: selected source pattern used when playback is stopped.
   When playback is running, the currently audible per-track sources are copied
   instead. The temp snapshot is forced into the hold state after the payload
   copy finishes. */
void pat_copyToTmpPattern(uint8_t srcPattern)
{
   uint8_t track;

   if(!seq_running)
   {
      pat_copyPattern(srcPattern, SEQ_TMP_PATTERN);
      pat_setTmpPatternHoldSettings();
      return;
   }

   for(track=0; track<NUM_TRACKS; track++)
   {
      pat_copyTrackPattern(track, SEQ_TMP_PATTERN, seq_perTrackActivePattern[track]);
   }

   pat_tmpPattern.pat_patternSettings = *pat_getPatternSettingPtr(seq_activePattern);
   pat_setTmpPatternHoldSettings();
   preset_captureTmpKitState();
}

/* Copy one track from one pattern into one destination pattern.
   srcNr: track to copy.
   dstPat: destination pattern.
   srcPat: source pattern that owns the track data. */
void pat_copyTrackPattern(uint8_t srcNr, uint8_t dstPat, uint8_t srcPat)
{
   int k;
   Step *psrc, *pdst;
   for(k=0;k<128;k++)
   {
      pdst = pat_getStepPtr(dstPat, srcNr, k);
      psrc = pat_getStepPtr(srcPat, srcNr, k);
      pdst->note = psrc->note;
      pdst->param1Nr = psrc->param1Nr;
      pdst->param1Val = psrc->param1Val;
      pdst->param2Nr = psrc->param2Nr;
      pdst->param2Val = psrc->param2Val;
      pdst->prob = psrc->prob;
      pdst->volume = psrc->volume;
   }

   pat_setMainSteps(dstPat, srcNr, pat_getMainSteps(srcPat, srcNr));
   pat_getLengthRotatePtr(dstPat, srcNr)->value =
      pat_getLengthRotatePtr(srcPat, srcNr)->value;
}

/* Copy one sub-step inside the currently active per-track pattern.
   src: source step index.
   dst: destination step index.
   track: track whose active source pattern is used. */
void pat_copySubStep(uint8_t src, uint8_t dst, uint8_t track)
{
   Step *psrc, *pdst;

   pdst = pat_getStepPtr(seq_perTrackActivePattern[track], track, dst);
   psrc = pat_getStepPtr(seq_perTrackActivePattern[track], track, src);
   pdst->note = psrc->note;
   pdst->param1Nr = psrc->param1Nr;
   pdst->param1Val = psrc->param1Val;
   pdst->param2Nr = psrc->param2Nr;
   pdst->param2Val = psrc->param2Val;
   pdst->prob = psrc->prob;
   pdst->volume = psrc->volume;
}
