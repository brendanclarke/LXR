/*
 * PatternData.c
 *
 * Owns the sequencer pattern payloads and the helpers that mutate them.
 */

#include "PatternData.h"
#include "MIDI/MidiMessages.h"
#include "Preset/KitState.h"

#include <string.h>

extern uint8_t seq_activePattern;
extern uint8_t seq_running;
extern uint8_t seq_perTrackActivePattern[NUM_TRACKS];
extern int8_t seq_stepIndex[NUM_TRACKS+1];
extern uint8_t seq_loopLength;
extern uint8_t seq_pendingLoopLength;
extern int8_t seq_loopCurrentPosition;
extern int8_t seq_loopUpdateFlag;
extern uint8_t frontParser_shownPattern;

PatternSet seq_patternSet;
TempPattern seq_tmpPattern;

/* Temp playback uses a dedicated hold state so the temp pattern never falls
   back into the normal next-pattern chain. Keep that normalization in one
   place so init and copy-to-temp stay aligned. */
static void seq_setTmpPatternHoldSettings(void)
{
   seq_tmpPattern.seq_patternSettings.changeBar = 0;
   seq_tmpPattern.seq_patternSettings.nextPattern = SEQ_TMP_PATTERN;
}

static void seq_resetNote(Step *step)
{
   step->note = SEQ_DEFAULT_NOTE;
   step->param1Nr = NO_AUTOMATION;
   step->param1Val = 0;
   step->param2Nr = NO_AUTOMATION;
   step->param2Val = 0;
   step->prob = 127;
   step->volume = 100;
}

uint8_t seq_normalizePatternNumber(uint8_t pattern)
{
   if(pattern == SEQ_TMP_PATTERN)
      return SEQ_TMP_PATTERN;
   return pattern & 0x07;
}

Step* seq_getStepPtr(uint8_t pattern, uint8_t track, uint8_t step)
{
   pattern = seq_normalizePatternNumber(pattern);
   if(pattern == SEQ_TMP_PATTERN)
      return &seq_tmpPattern.seq_subStepPattern[track][step & 0x7f];
   return &seq_patternSet.seq_subStepPattern[pattern][track][step & 0x7f];
}

LengthRotate* seq_getLengthRotatePtr(uint8_t pattern, uint8_t track)
{
   pattern = seq_normalizePatternNumber(pattern);
   if(pattern == SEQ_TMP_PATTERN)
      return &seq_tmpPattern.seq_patternLengthRotate[track];
   return &seq_patternSet.seq_patternLengthRotate[pattern][track];
}

PatternSetting* seq_getPatternSettingPtr(uint8_t pattern)
{
   pattern = seq_normalizePatternNumber(pattern);
   if(pattern == SEQ_TMP_PATTERN)
      return &seq_tmpPattern.seq_patternSettings;
   return &seq_patternSet.seq_patternSettings[pattern];
}

uint16_t seq_getMainSteps(uint8_t pattern, uint8_t track)
{
   pattern = seq_normalizePatternNumber(pattern);
   if(pattern == SEQ_TMP_PATTERN)
      return seq_tmpPattern.seq_mainSteps[track];
   return seq_patternSet.seq_mainSteps[pattern][track];
}

void seq_setMainSteps(uint8_t pattern, uint8_t track, uint16_t steps)
{
   pattern = seq_normalizePatternNumber(pattern);
   if(pattern == SEQ_TMP_PATTERN)
      seq_tmpPattern.seq_mainSteps[track] = steps;
   else
      seq_patternSet.seq_mainSteps[pattern][track] = steps;
}

void seq_initPatternData(void)
{
   uint8_t i;

   for(i=0;i<NUM_PATTERN;i++)
   {
      seq_patternSet.seq_patternSettings[i].changeBar = 0;
      seq_patternSet.seq_patternSettings[i].nextPattern = i;
      seq_clearPattern(i);
   }

   seq_setTmpPatternHoldSettings();
   seq_clearPattern(SEQ_TMP_PATTERN);
}

void seq_applyTmpPatternTo(uint8_t pattern)
{
   if(pattern == SEQ_TMP_PATTERN)
      return;

   pattern &= 0x07;
   memcpy(&seq_patternSet.seq_subStepPattern[pattern],
          &seq_tmpPattern.seq_subStepPattern,
          sizeof(Step) * NUM_TRACKS * NUM_STEPS);
   memcpy(&seq_patternSet.seq_mainSteps[pattern],
          &seq_tmpPattern.seq_mainSteps,
          sizeof(uint16_t) * NUM_TRACKS);
   memcpy(&seq_patternSet.seq_patternSettings[pattern],
          &seq_tmpPattern.seq_patternSettings,
          sizeof(PatternSetting));
   memcpy(&seq_patternSet.seq_patternLengthRotate[pattern],
          &seq_tmpPattern.seq_patternLengthRotate,
          sizeof(LengthRotate) * NUM_TRACKS);
}

void seq_activateTmpPattern(void)
{
   seq_applyTmpPatternTo(seq_activePattern);
}

void seq_setTrackLength(uint8_t trackNr, uint8_t length)
{
   if(length == 16)
      length = 0;
   seq_getLengthRotatePtr(frontParser_shownPattern, trackNr)->length = length;
}

void seq_setTrackScale(uint8_t trackNr, uint8_t scale)
{
   if(scale > 7)
      scale = 7;

   seq_getLengthRotatePtr(frontParser_shownPattern, trackNr)->scale = scale;
}

uint8_t seq_getTrackLength(uint8_t trackNr)
{
   uint8_t r = seq_getLengthRotatePtr(frontParser_shownPattern, trackNr)->length;
   if(r == 0)
      return 16;
   return r;
}

uint8_t seq_getTrackScale(uint8_t trackNr)
{
   return seq_getLengthRotatePtr(frontParser_shownPattern, trackNr)->scale;
}

void seq_setTrackRotation(uint8_t trackNr, const uint8_t newRot)
{
   LengthRotate *lr = seq_getLengthRotatePtr(frontParser_shownPattern, trackNr);

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

uint8_t seq_getTrackRotation(uint8_t trackNr)
{
   return seq_getLengthRotatePtr(frontParser_shownPattern, trackNr)->rotate;
}

void seq_setLoop(uint8_t length)
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

void seq_toggleStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
   Step *step = seq_getStepPtr(patternNr, voice, stepNr);
   if((step->volume & STEP_ACTIVE_MASK) == 0)
   {
      step->volume |= STEP_ACTIVE_MASK;
   }
   else
   {
      step->volume &= ~STEP_ACTIVE_MASK;
   }
}

void seq_toggleMainStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
   seq_setMainSteps(patternNr, voice, (uint16_t)(seq_getMainSteps(patternNr, voice) ^ (1 << stepNr)));
}

void seq_setMainStep(uint8_t patternNr, uint8_t voice, uint8_t stepNr, uint8_t onOff)
{
   uint16_t mainSteps = seq_getMainSteps(patternNr, voice);
   if(onOff)
   {
      mainSteps |= (1 << stepNr);
   }
   else
   {
      mainSteps &= ~(1 << stepNr);
   }
   seq_setMainSteps(patternNr, voice, mainSteps);
}

uint8_t seq_isStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
   return ((seq_getStepPtr(patternNr, voice, stepNr)->volume & STEP_ACTIVE_MASK) > 0);
}

uint8_t seq_isMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern)
{
   return (seq_getMainSteps(pattern, voice) & (1 << mainStepNr)) > 0;
}

void seq_clearTrack(uint8_t trackNr, uint8_t pattern)
{
   int k;
   for(k=0;k<128;k++)
   {
      Step *step = seq_getStepPtr(pattern, trackNr, k);
      seq_resetNote(step);

      if((k % 8) == 0)
         step->volume |= STEP_ACTIVE_MASK;
   }

   seq_setMainSteps(pattern, trackNr, 0);
   seq_getLengthRotatePtr(pattern, trackNr)->rotate = 0;
}

void seq_clearAutomation(uint8_t trackNr, uint8_t pattern, uint8_t automTrack)
{
   int k;

   if(automTrack == 0)
   {
      for(k=0;k<128;k++)
      {
         Step *step = seq_getStepPtr(pattern, trackNr, k);
         step->param1Nr = NO_AUTOMATION;
         step->param1Val = 0;
      }
   }
   else
   {
      for(k=0;k<128;k++)
      {
         Step *step = seq_getStepPtr(pattern, trackNr, k);
         step->param2Nr = NO_AUTOMATION;
         step->param2Val = 0;
      }
   }
}

void seq_clearPattern(uint8_t pattern)
{
   int i;
   for(i=0;i<NUM_TRACKS;i++)
      seq_clearTrack(i, pattern);
}

void seq_copyTrack(uint8_t srcNr, uint8_t dstNr, uint8_t pattern)
{
   int k;
   Step *src, *dst;
   for(k=0;k<128;k++)
   {
      dst = seq_getStepPtr(pattern, dstNr, k);
      src = seq_getStepPtr(pattern, srcNr, k);
      dst->note = src->note;
      dst->param1Nr = src->param1Nr;
      dst->param1Val = src->param1Val;
      dst->param2Nr = src->param2Nr;
      dst->param2Val = src->param2Val;
      dst->prob = src->prob;
      dst->volume = src->volume;
   }

   seq_setMainSteps(pattern, dstNr, seq_getMainSteps(pattern, srcNr));
   seq_getLengthRotatePtr(pattern, dstNr)->value =
      seq_getLengthRotatePtr(pattern, srcNr)->value;
}

void seq_copyPattern(uint8_t src, uint8_t dst)
{
   int k, j;
   Step *psrc, *pdst;
   uint8_t normalizedSrc = seq_normalizePatternNumber(src);
   uint8_t normalizedDst = seq_normalizePatternNumber(dst);

   for(j=0;j<NUM_TRACKS;j++)
   {
      for(k=0;k<128;k++)
      {
         pdst = seq_getStepPtr(dst, j, k);
         psrc = seq_getStepPtr(src, j, k);
         pdst->note = psrc->note;
         pdst->param1Nr = psrc->param1Nr;
         pdst->param1Val = psrc->param1Val;
         pdst->param2Nr = psrc->param2Nr;
         pdst->param2Val = psrc->param2Val;
         pdst->prob = psrc->prob;
         pdst->volume = psrc->volume;
      }

      seq_setMainSteps(dst, j, seq_getMainSteps(src, j));
      seq_getLengthRotatePtr(dst, j)->value =
         seq_getLengthRotatePtr(src, j)->value;
   }

   if((normalizedDst == SEQ_TMP_PATTERN) && (normalizedSrc != SEQ_TMP_PATTERN))
   {
      preset_captureTmpKitState();
   }
}

/* Copies either the selected pattern or the currently playing per-track
   arrangement into the temp pattern image. When playback is running, the live
   track source table wins; when playback is stopped, the caller's selected
   source pattern remains the fallback so the old copy-to-temp semantics stay
   intact. */
void seq_copyToTmpPattern(uint8_t srcPattern)
{
   uint8_t track;

   if(!seq_running)
   {
      seq_copyPattern(srcPattern, SEQ_TMP_PATTERN);
      return;
   }

   for(track=0; track<NUM_TRACKS; track++)
   {
      seq_copyTrackPattern(track, SEQ_TMP_PATTERN, seq_perTrackActivePattern[track]);
   }

   seq_tmpPattern.seq_patternSettings = *seq_getPatternSettingPtr(seq_activePattern);
   seq_setTmpPatternHoldSettings();
   preset_captureTmpKitState();
}

void seq_copyTrackPattern(uint8_t srcNr, uint8_t dstPat, uint8_t srcPat)
{
   int k;
   Step *psrc, *pdst;
   for(k=0;k<128;k++)
   {
      pdst = seq_getStepPtr(dstPat, srcNr, k);
      psrc = seq_getStepPtr(srcPat, srcNr, k);
      pdst->note = psrc->note;
      pdst->param1Nr = psrc->param1Nr;
      pdst->param1Val = psrc->param1Val;
      pdst->param2Nr = psrc->param2Nr;
      pdst->param2Val = psrc->param2Val;
      pdst->prob = psrc->prob;
      pdst->volume = psrc->volume;
   }

   seq_setMainSteps(dstPat, srcNr, seq_getMainSteps(srcPat, srcNr));
   seq_getLengthRotatePtr(dstPat, srcNr)->value =
      seq_getLengthRotatePtr(srcPat, srcNr)->value;
}

void seq_copySubStep(uint8_t src, uint8_t dst, uint8_t track)
{
   Step *psrc, *pdst;

   pdst = seq_getStepPtr(seq_perTrackActivePattern[track], track, dst);
   psrc = seq_getStepPtr(seq_perTrackActivePattern[track], track, src);
   pdst->note = psrc->note;
   pdst->param1Nr = psrc->param1Nr;
   pdst->param1Val = psrc->param1Val;
   pdst->param2Nr = psrc->param2Nr;
   pdst->param2Val = psrc->param2Val;
   pdst->prob = psrc->prob;
   pdst->volume = psrc->volume;
}
