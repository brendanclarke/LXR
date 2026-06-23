# SAMPLE_PLAN_PHASE_2: Oscillator Fixes and Looped Playback Framework

## Scope

This phase covers sample metadata loop flags and playback behavior in:

- `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`
- `mainboard/LxrStm32/src/DSPAudio/Oscillator.h`
- `mainboard/LxrStm32/src/DSPAudio/Oscillator.c`
- Trigger reset sites in `DrumVoice.c`, `Snare.c`, `CymbalVoice.c`, and `HiHat.c`

## Audit Result

The planning documents correctly identify that loop metadata is missing, but the proposed `info.size & 0x80000000` code cannot be dropped into the current code without first changing `SampleInfo.size`.

Current reality:

- `SampleInfo.size` is `uint16_t`, so there is no bit 31.
- The struct is effectively 12 bytes today because of padding: `char[3]`, padding, `uint16_t`, padding, `uint32_t`.
- Replacing `uint16_t size` with `uint32_t size` still keeps the struct 12 bytes on ARM, so the bit-packed plan is viable.
- `oscPhase >> 17` has only 15 integer bits. That means the current oscillator cannot address beyond index `32767` no matter how large `SampleInfo.size` is.

Important callout: the draft's modulo guard does not by itself create a true 32-bit sample position index. It only prevents bad modulo math for looped samples. To actually play samples longer than 32768 16-bit frames, the oscillator needs a separate integer sample index and fractional accumulator.

Post-Phase-1 hardware test callout: an extra silent `s1` entry can appear when only `s0` is loaded. The STM audio dispatcher currently accepts one index past the committed count because it tests `sampleIndex > sampleMemory_getNumSamples()` instead of `sampleIndex >= sampleMemory_getNumSamples()`. Phase 2 must fix that off-by-one before reading `SampleInfo`, and rejected sample slots must zero-fill the output block rather than returning with stale buffer contents.

## Exact Code Changes

### 1. Change metadata packing in `SampleMemory.h`

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`

Change:

```c
typedef struct SampleInfoStruct
{
   char name[3];
   uint32_t size;     /* bit 31 loop flag, bits 30..0 length in int16 frames */
   uint32_t offset;   /* absolute STM32 internal flash address */
} SampleInfo;

#define SAMPLE_INFO_LOOP_FLAG ((uint32_t)0x80000000u)
#define SAMPLE_INFO_SIZE_MASK ((uint32_t)0x7fffffffu)
```

Why each line needs to happen:

- `uint32_t size` gives room for both a loop flag and a useful frame count.
- The comment documents the on-flash encoding so later code does not treat `size` as raw length.
- The two masks make all callers decode metadata consistently.

Risk:

- Existing sample tables written with the old `uint16_t size` layout may decode incorrectly after this change. Phase 1's clamped count helps fail closed, but a migration/version marker would be safer if old sample ROMs must be preserved.

### 2. Add metadata accessor helpers

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`

Add prototypes:

```c
uint32_t sampleMemory_getSampleSizeFrames(SampleInfo info);
uint8_t sampleMemory_isSampleLooped(SampleInfo info);
SampleInfo sampleMemory_makeSampleInfo(const char* name,
                                       uint32_t sizeFrames,
                                       uint32_t offset,
                                       uint8_t looped);
```

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`

Add:

```c
uint32_t sampleMemory_getSampleSizeFrames(SampleInfo info)
{
   return info.size & SAMPLE_INFO_SIZE_MASK;
}

uint8_t sampleMemory_isSampleLooped(SampleInfo info)
{
   return (uint8_t)((info.size & SAMPLE_INFO_LOOP_FLAG) != 0u);
}

SampleInfo sampleMemory_makeSampleInfo(const char* name,
                                       uint32_t sizeFrames,
                                       uint32_t offset,
                                       uint8_t looped)
{
   SampleInfo info;

   memset(&info, 0, sizeof(info));
   memcpy(info.name, name, 3);
   info.size = sizeFrames & SAMPLE_INFO_SIZE_MASK;
   if(looped)
      info.size |= SAMPLE_INFO_LOOP_FLAG;
   info.offset = offset;

   return info;
}
```

Why each line needs to happen:

- The getter helpers prevent repeated mask logic in the audio loop and UI code.
- `memset()` prevents padding bytes in the metadata table from containing stack garbage.
- Masking `sizeFrames` ensures no accidental high bit turns a one-shot into a loop.

Risk:

- Calling these helpers from the inner audio loop adds function-call overhead unless optimized inline. If profiling shows a cost, move the two decode helpers to `static inline` definitions in the header.

### 3. Update sample metadata creation

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`

Replace the current direct assignments:

```c
memcpy(info[i].name, name, 3);
info[i].offset = SAMPLE_ROM_START_ADDRESS + 4 + addr * 4;
info[i].size = len / 2;
```

with:

```c
info[i] = sampleMemory_makeSampleInfo(name,
                                      len / 2u,
                                      SAMPLE_ROM_START_ADDRESS + 4u + addr * 4u,
                                      0u);
```

Why each line needs to happen:

- The helper centralizes the new packed format.
- Passing `0u` keeps `/samples` as one-shot material until Phase 3 adds `/loops`.
- Keeping the same offset expression preserves the existing internal flash layout.

Risk:

- `len / 2u` assumes valid 16-bit PCM. Current `findDataChunk()` does not validate WAV format; Phase 3 must add validation before trusting this length.

### 4. Add true long-sample playback state to `OscInfo`

Target: `mainboard/LxrStm32/src/DSPAudio/Oscillator.h`

Add fields after `phase`:

```c
   uint32_t samplePosition;  /* integer frame index for user samples */
   uint32_t sampleFraction;  /* 17-bit fractional accumulator for user samples */
   uint8_t sampleActive;     /* one-shot playback gate; cleared when sample ends */
```

Why each line needs to happen:

- `samplePosition` is the actual 32-bit sample index; this is what the draft plan calls for but does not implement.
- `sampleFraction` keeps the existing pitch resolution without stealing integer bits from the index.
- `sampleActive` prevents a long one-shot from wrapping to frame zero when the accumulator overflows.

Risk:

- `OscInfo` grows. There are several oscillator instances, but the RAM increase is small. Because these structs are not stored in preset files, preset compatibility is not affected.

### 5. Reset sample playback state on trigger

Targets:

- `mainboard/LxrStm32/src/DSPAudio/DrumVoice.c`
- `mainboard/LxrStm32/src/DSPAudio/Snare.c`
- `mainboard/LxrStm32/src/DSPAudio/CymbalVoice.c`
- `mainboard/LxrStm32/src/DSPAudio/HiHat.c`

Add a helper in `Oscillator.h`:

```c
void osc_resetSamplePlayback(OscInfo* osc);
```

Add in `Oscillator.c`:

```c
void osc_resetSamplePlayback(OscInfo* osc)
{
   osc->samplePosition = 0u;
   osc->sampleFraction = 0u;
   osc->sampleActive = 1u;
}
```

Call this helper beside each existing `osc.phase = 0` retrigger assignment for user-sample-capable oscillators.

Why each line needs to happen:

- User sample playback now has state outside `phase`; retrigger paths must reset it or samples resume in the middle.
- Keeping the reset in one helper avoids missing one of the many voice trigger paths.

Risk:

- Only reset when the oscillator is triggered, not when a parameter is edited. Resetting on waveform menu edits would create audible glitches.

### 6. Replace `calcUserSampleOscBlock()` indexing

Target: `mainboard/LxrStm32/src/DSPAudio/Oscillator.c`

Replace the body logic with this shape:

```c
const uint8_t sampleIndex = (uint8_t)(osc->waveform - OSC_SAMPLE_START);
const uint8_t sampleCount = sampleMemory_getNumSamples();

if(sampleIndex >= sampleCount)
{
   memset(buf, 0, size * sizeof(int16_t));
   return;
}

SampleInfo info = sampleMemory_getSampleInfo(sampleIndex);
const uint32_t sampleSize = sampleMemory_getSampleSizeFrames(info);
const uint8_t looped = sampleMemory_isSampleLooped(info);
int16_t* sampleData = (int16_t*)((int8_t*)(info.offset));

if(sampleSize == 0u || info.offset < SAMPLE_ROM_START_ADDRESS || !osc->sampleActive)
{
   memset(buf, 0, size * sizeof(int16_t));
   return;
}

for(i = 0; i < size; i++)
{
   uint32_t pos = osc->samplePosition;
   uint32_t nextPos = pos + 1u;
   int16_t oscOut = 0;

   if(pos >= sampleSize)
   {
      if(looped)
         pos %= sampleSize;
      else
      {
         osc->sampleActive = 0u;
         buf[i] = 0;
         continue;
      }
   }

#if INTERPOLATE_OSC
   oscOut = sampleData[pos];
   if(nextPos < sampleSize)
   {
      const float frac = (osc->sampleFraction & 0x1ffffu) * 0.00000762939453125f;
      oscOut += frac * (sampleData[nextPos] - oscOut);
   }
#else
   oscOut = sampleData[pos];
#endif

   osc->sampleFraction += osc->phaseInc;
   osc->samplePosition += (osc->sampleFraction >> 17);
   osc->sampleFraction &= 0x1ffffu;

   if(looped && sampleSize > 0u && osc->samplePosition >= sampleSize)
      osc->samplePosition %= sampleSize;

   buf[i] = oscOut * gain;
}
```

Why each line needs to happen:

- `sampleIndex >= sampleCount` fixes the observed silent `s1` bug and prevents out-of-range metadata reads.
- The early zero-fill for rejected sample slots guarantees invalid sample selections produce silence deterministically.
- Decoding `sampleSize` and `looped` once per block keeps the inner loop simpler.
- The `info.offset` check avoids treating blank or corrupt metadata as a valid flash pointer.
- `samplePosition` is now independent of the 17-bit fractional accumulator, so samples longer than 32768 frames can be addressed.
- The interpolation guard avoids reading `sampleData[sampleSize]`.
- The loop wrap only happens after a valid looped sample advances beyond the end.

Risk:

- This is a behavior change for all user samples. It needs hardware testing for pitch, retrigger, one-shot end, and loop wrap.

### 7. Apply the same safety to `calcUserSampleOscFmBlock()`

Target: `mainboard/LxrStm32/src/DSPAudio/Oscillator.c`

The FM version currently uses `oscPhase + modBuffer` and `itg = index >> 17`. It should either:

- receive the same `samplePosition/sampleFraction` treatment with FM contributing to the fractional read offset, or
- explicitly disable FM for user samples until a safe indexed implementation exists.

Exact conservative change:

```c
const uint8_t sampleIndex = (uint8_t)(osc->waveform - OSC_SAMPLE_START);
const uint8_t sampleCount = sampleMemory_getNumSamples();

if(sampleIndex >= sampleCount)
{
   memset(buf, 0, size * sizeof(int16_t));
   return;
}

/* For Phase 2, route FM user samples through the non-FM long-index reader.
   This preserves safe bounds while avoiding a second buggy long-sample path. */
calcUserSampleOscBlock(osc, buf, size, gain);
```

Why these lines need to happen:

- The same `sampleIndex >= sampleCount` guard must apply to FM user-sample playback; otherwise `s1` is still unsafe in FM mode.
- It prevents the old 15-bit integer index from surviving in the FM path.

Risk:

- User-sample FM modulation is disabled by this conservative step. If that behavior matters, implement a bounded FM read after the base long-index path is confirmed.

## Interdependencies

- Phase 1 must land first so invalid sample counts and corrupt metadata fail closed.
- Phase 3 must use `sampleMemory_makeSampleInfo(..., looped)` to tag `/loops`.
- Phase 4's name display can reuse `SampleInfo.name`.

## Plan Callouts

- The draft's `if(sampleSize < 32768u) nextPhase %= loopPhase` is not enough to fulfill "32-bit sample position index".
- `OSC_SAMPLE_START` is `0x06` on STM and corresponds to `waveformNames[0][0]` on AVR. Do not use the unrelated value `100` from the older audit.
