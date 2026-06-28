# OSC_WAVE_INTERP_FINALIZE — Implementation Plan (Final)

DATE: 2026-06-28

**Goal**: Wire `PAR_OSC_WAVE_INTERPOLATION` from the AVR global menu to a runtime flag,
then implement per-oscillator waveform blend logic so that LFO or velocity modulation
targeting an OSC_WAVE parameter produces a real-time cross-fade between two adjacent
waveforms rather than a hard integer switch.

This is a single-pass change list. All changes are made once.

### Implementation Notes (2026-06-28)
- Updated `OSC_WAVE_INTERP_MAX_ACTIVE` to 1, ensuring only a single dynamic interpolation slot is allocated per block.
- Implemented `Oscillator.h/c` changes with scratch buffers and guards.
- Implemented `modulationNode.h/c` state variables, helpers, block epoch logic, and the `modNode_updateValue` intercept.
- Added communication opcodes to `avrCommsReceivingProtocol.h` and `frontPanelReceivingProtocol.h`.
- Updated `menu_parseGlobalParam` in `menu.c` to send the new opcode to STM.
- AVR firmware compiles successfully.
- STM firmware compilation fails due to `SampleImportReceiver.c` missing declarations in `SampleMemory.h`.

---

## Architecture — What The Reference Code Shows

### Where the flag and logic actually live

The reference `modulationNode.c` (not `MorphEngine.c`) is the home for:

- `modNode_waveInterpEnabled` — the on/off flag
- `modNode_waveInterpGeneration` — a per-block epoch counter
- `modNode_waveInterpActiveCount` — a per-block budget counter
- `modNode_getWaveTargetOsc(param)` — maps a waveform parameter ID to the oscillator pointer
- `modNode_getMaxWaveformIndex()` — returns the highest valid waveform ID
- `modNode_setWaveInterpEnabled()`, `modNode_getWaveInterpEnabled()`, `modNode_getWaveInterpGeneration()` — accessors

The blend state (`waveInterpFrac`, `waveInterpNext`, `waveInterpGeneration`) is written to
the oscillator struct inside `modNode_updateValue()`, in the `TYPE_UINT8` branch, at the
point where the modulated float value for a waveform parameter is normally truncated to an
integer waveform ID and written to `*dst`. When interp is enabled and the budget permits,
instead of just writing `(uint8_t)modulated` to `*dst` it also writes the fractional part
and the next waveform ID to the oscillator's blend fields.

`modNode_resetTargets()` bumps the generation counter and resets the active count at the
top of every DSP block render, so oscillators whose blend state was not refreshed this block
automatically fail the `osc_waveInterpActive()` guard via the stale generation check.

### Who calls modNode_setWaveInterpEnabled()

The UART receive path: when `FRONT_SEQ_OSC_WAVE_INTERPOLATION` arrives, it calls
`modNode_setWaveInterpEnabled(data2)`. That is the entire AVR→STM wire path.

### What osc_waveInterpActive() checks

1. `modNode_getWaveInterpEnabled()` returns non-zero
2. `osc->waveInterpGeneration == modNode_getWaveInterpGeneration()` (not stale)
3. `0 < osc->waveInterpFrac < 1` (non-trivial fraction)
4. `osc->waveInterpNext > osc->waveform` (forward direction only)
5. Both IDs <= `maxWave` (in-range)

### The live modulationNode.c differences from the reference

The live `modNode_updateValue()` has a more complex structure than the reference (it
handles `TYPE_UINT8_VMORPH`, a negative `vm->amount` branch, and an envelope-position
side-path). The reference simplifies this. We integrate only the waveform-interp addition
into the live code's existing structure, touching only the `TYPE_UINT8` positive-amount
branch where waveform parameters are written.

The live `modNode_resetTargets()` does not bump any generation counter (the generation
concept is new). We add the bump there.

The live `modulationNode.h` has no waveform-interp declarations. We add them.

---

## Complete Change List

### CHANGE 1 — `Oscillator.h`: Three new blend fields on `OscInfo`, two new prototypes

**File**: `mainboard/LxrStm32/src/DSPAudio/Oscillator.h`

**Location**: `OscStruct` typedef, after `uint8_t waveform;` (line 69).

**Exact insertion** (replace the line `uint8_t waveform;` and the blank line after it):
```c
	uint8_t		waveform;	// the selected waveform of the osc

	/* Waveform-blend state. Written by modNode_updateValue() when the interp flag
	   is on and a waveform parameter is being modulated to a fractional position.
	   waveInterpGeneration is compared against modNode_getWaveInterpGeneration()
	   each DSP block; a mismatch means the state is stale and interp is skipped.
	   waveInterpFrac == 0 or >= 1 means no blend is active. */
	float    waveInterpFrac;       // blend fraction: 0.0=waveform, 1.0=waveInterpNext
	uint8_t  waveInterpNext;       // target waveform ID to blend toward
	uint8_t  _wavePad;             // explicit alignment pad (float follows uint8_t)
	uint32_t waveInterpGeneration; // DSP block generation token from modNode
```

After the existing `osc_resetSamplePlayback()` declaration at the bottom of the header,
before `#endif`, add:
```c
/* Resets all three waveform-blend fields to inactive state. Called on recursive
   calcNextOscSampleBlock() calls inside blend helpers to prevent re-entry.
   Input/output: osc -- waveInterpFrac=0, waveInterpNext=osc->waveform, generation=0. */
void osc_clearWaveInterp(OscInfo* osc);

/* Writes waveform-blend target state to an oscillator. Called from
   modNode_updateValue() when a TYPE_UINT8 waveform parameter is modulated
   to a fractional position and the interp budget permits.
   Inputs: osc, fracValue (0.0..1.0), nextWaveform, generation (from modNode). */
void osc_setWaveInterp(OscInfo* osc, float fracValue, uint8_t nextWaveform, uint32_t generation);
```

**Why `uint32_t waveInterpGeneration`**: `modNode_resetTargets()` bumps a `uint32_t` counter
each DSP block. Each oscillator stores the generation at which its blend state was last
written. `osc_waveInterpActive()` rejects states from prior blocks by comparing these. This
makes stale interp state auto-expire without needing an explicit clear call.

**Why `_wavePad`**: Makes the layout of the two `uint8_t` fields explicit and prevents
surprising implicit padding before the `uint32_t` that follows.

---

### CHANGE 2 — `Oscillator.c`: New helpers and four dispatcher guard insertions

**File**: `mainboard/LxrStm32/src/DSPAudio/Oscillator.c`

#### 2a. Add include for modulationNode.h

At the top of `Oscillator.c`, after the existing includes (currently ends at `#include "sequencer.h"` line 43):
```c
#include "modulationNode.h"
```

**Why**: `osc_waveInterpActive()` calls `modNode_getWaveInterpEnabled()` and
`modNode_getWaveInterpGeneration()`. These are declared in `modulationNode.h`.

#### 2b. Add two scratch buffers and five new static/non-static functions

Insert as a block after `freq2PhaseIncr32767()` (after line 86) and before
`osc_resetSamplePlayback()` (line 73 in live). The insertion point is at line 87
in the live file.

```c
/* Scratch buffers for waveform-blend rendering.
   Block-sized static globals: two waveforms are rendered into these before the
   per-sample blend loop runs. Static so they are not allocated on the audio stack.
   Size = OUTPUT_DMA_SIZE (16 frames). */
static int16_t osc_interp_a[OUTPUT_DMA_SIZE];
static int16_t osc_interp_b[OUTPUT_DMA_SIZE];

/* -----------------------------------------------------------------------
   osc_clearWaveInterp
   Resets blend fields to inactive. Call on the temporary OscInfo copies used
   inside the blend helpers so recursive calcNextOscSampleBlock() calls do not
   re-enter the blend path.
   Input/output: osc -- waveInterpFrac=0.f, waveInterpNext=osc->waveform,
   waveInterpGeneration=0.
   ----------------------------------------------------------------------- */
void osc_clearWaveInterp(OscInfo* osc)
{
   osc->waveInterpFrac = 0.f;
   osc->waveInterpNext = osc->waveform;
   osc->waveInterpGeneration = 0u;
}

/* -----------------------------------------------------------------------
   osc_setWaveInterp
   Writes waveform-blend target state. Called by modNode_updateValue() inside
   the TYPE_UINT8 branch for waveform parameters.
   Inputs:
     osc          - the oscillator to update.
     fracValue    - blend fraction in (0.0, 1.0).
     nextWaveform - the integer waveform ID above the current integer floor.
     generation   - current block generation from modNode_getWaveInterpGeneration().
   Output: osc->waveInterpFrac, waveInterpNext, waveInterpGeneration written.
   ----------------------------------------------------------------------- */
void osc_setWaveInterp(OscInfo* osc, float fracValue, uint8_t nextWaveform, uint32_t generation)
{
   osc->waveInterpFrac = fracValue;
   osc->waveInterpNext = nextWaveform;
   osc->waveInterpGeneration = generation;
}

/* -----------------------------------------------------------------------
   osc_waveInterpActive  (static, called only inside Oscillator.c)
   Guard function called at the top of each calcNextOscSample[Fm][Block]
   dispatcher. Returns 1 only when a valid blend is active for this oscillator.
   Returning 0 causes the normal waveform-switch path to run unchanged.

   Conditions that return 0 (in order of cheapest check first):
     1. modNode_getWaveInterpEnabled() is 0  (global flag off)
     2. osc->waveInterpGeneration != modNode_getWaveInterpGeneration()
           (blend state from a prior DSP block -- auto-expired)
     3. osc->waveInterpFrac <= 0.f or >= 1.f  (no fractional blend needed)
     4. osc->waveInterpNext <= osc->waveform   (illegal or reverse direction)
     5. osc->waveform or waveInterpNext exceeds the highest valid waveform ID

   Input: osc (read-only). Output: 0 or 1.
   ----------------------------------------------------------------------- */
static uint8_t osc_waveInterpActive(const OscInfo* osc)
{
   if (!modNode_getWaveInterpEnabled()) {
      return 0u;
   }
   if (osc->waveInterpGeneration != modNode_getWaveInterpGeneration()) {
      return 0u;
   }
   if (osc->waveInterpFrac <= 0.f || osc->waveInterpFrac >= 1.f) {
      return 0u;
   }
   if (osc->waveInterpNext <= osc->waveform) {
      return 0u;
   }
   const uint8_t numSamples = sampleMemory_getNumSamples();
   const uint8_t maxWave = (numSamples > 0u)
                         ? (uint8_t)(OSC_SAMPLE_START + numSamples - 1u)
                         : (uint8_t)(OSC_SAMPLE_START - 1u);
   if (osc->waveform > maxWave || osc->waveInterpNext > maxWave) {
      return 0u;
   }
   return 1u;
}

/* -----------------------------------------------------------------------
   calcPeriodicInterpBlock  (static)
   Block-mode waveform blend for non-FM oscillators.
   Renders waveform and waveInterpNext into osc_interp_a and osc_interp_b
   by calling calcNextOscSampleBlock() on temporary copies with interp cleared
   (to prevent re-entry). Then blends per sample by waveInterpFrac.
   Inputs: osc, buf (OUTPUT_DMA_SIZE int16 output), size, gain.
   Output: buf filled. osc->phase advanced by phaseInc * size.
   ----------------------------------------------------------------------- */
static void calcPeriodicInterpBlock(OscInfo* osc, int16_t* buf, const uint8_t size, const float gain)
{
   const uint32_t startPhase = osc->phase;
   OscInfo oscA = *osc;
   OscInfo oscB = *osc;
   const float frac = osc->waveInterpFrac;
   uint8_t i;

   oscA.waveform = osc->waveform;
   osc_clearWaveInterp(&oscA);
   oscB.waveform = osc->waveInterpNext;
   osc_clearWaveInterp(&oscB);

   calcNextOscSampleBlock(&oscA, osc_interp_a, size, 1.f);
   calcNextOscSampleBlock(&oscB, osc_interp_b, size, 1.f);

   for (i = 0u; i < size; i++) {
      const int16_t a = osc_interp_a[i];
      const int16_t b = osc_interp_b[i];
      buf[i] = (int16_t)((a + frac * (float)(b - a)) * gain);
   }
   osc->phase = startPhase + (osc->phaseInc * (uint32_t)size);
}

/* -----------------------------------------------------------------------
   calcPeriodicInterp  (static)
   Scalar waveform blend for non-FM oscillators. Same logic as the block
   version but produces one sample. Used by calcNextOscSample().
   Output: blended int16. osc->phase advanced by one phaseInc. osc->output set.
   ----------------------------------------------------------------------- */
static int16_t calcPeriodicInterp(OscInfo* osc)
{
   const uint32_t phase = osc->phase;
   const float frac = osc->waveInterpFrac;
   OscInfo oscA = *osc;
   OscInfo oscB = *osc;
   int16_t a, b, out;

   oscA.waveform = osc->waveform;
   osc_clearWaveInterp(&oscA);
   oscB.waveform = osc->waveInterpNext;
   osc_clearWaveInterp(&oscB);

   /* Render one sample by routing through the block path with size=1.
      This reuses calcNextOscSampleBlock()'s waveform dispatch without
      duplicating the switch logic. */
   calcNextOscSampleBlock(&oscA, &a, 1u, 1.f);
   calcNextOscSampleBlock(&oscB, &b, 1u, 1.f);

   out = (int16_t)(a + frac * (float)(b - a));
   osc->phase = phase + osc->phaseInc;
   osc->output = out;
   return out;
}

/* -----------------------------------------------------------------------
   calcPeriodicInterpFmBlock  (static)
   Block-mode waveform blend for FM oscillators.
   Identical to calcPeriodicInterpBlock but routes through
   calcNextOscSampleFmBlock(). Needed because the FM dispatch path applies
   the FM modulator index before the wavetable lookup.
   Inputs: osc, modBuffer (FM modulator samples), buf, size, gain.
   Output: buf filled. osc->phase advanced. osc->output = last blended sample.
   ----------------------------------------------------------------------- */
static void calcPeriodicInterpFmBlock(OscInfo* osc, int16_t* modBuffer, int16_t* buf, const uint8_t size, const float gain)
{
   const uint32_t startPhase = osc->phase;
   OscInfo oscA = *osc;
   OscInfo oscB = *osc;
   const float frac = osc->waveInterpFrac;
   int16_t lastOut = 0;
   uint8_t i;

   oscA.waveform = osc->waveform;
   osc_clearWaveInterp(&oscA);
   oscB.waveform = osc->waveInterpNext;
   osc_clearWaveInterp(&oscB);

   calcNextOscSampleFmBlock(&oscA, modBuffer, osc_interp_a, size, 1.f);
   calcNextOscSampleFmBlock(&oscB, modBuffer, osc_interp_b, size, 1.f);

   for (i = 0u; i < size; i++) {
      const int16_t a = osc_interp_a[i];
      const int16_t b = osc_interp_b[i];
      lastOut = (int16_t)(a + frac * (float)(b - a));
      buf[i] = (int16_t)(lastOut * gain);
   }
   osc->phase = startPhase + (osc->phaseInc * (uint32_t)size);
   osc->output = lastOut;
}

/* -----------------------------------------------------------------------
   calcPeriodicInterpFm  (static)
   Scalar waveform blend for FM oscillators.
   Inputs: osc, modOsc (provides modOsc->output as modulator sample).
   Output: blended int16. osc->phase advanced. osc->output set.
   ----------------------------------------------------------------------- */
static int16_t calcPeriodicInterpFm(OscInfo* osc, OscInfo* modOsc)
{
   const uint32_t phase = osc->phase;
   const float frac = osc->waveInterpFrac;
   OscInfo oscA = *osc;
   OscInfo oscB = *osc;
   int16_t a, b, out;
   int16_t modBuf[1] = { modOsc->output };

   oscA.waveform = osc->waveform;
   osc_clearWaveInterp(&oscA);
   oscB.waveform = osc->waveInterpNext;
   osc_clearWaveInterp(&oscB);

   calcNextOscSampleFmBlock(&oscA, modBuf, &a, 1u, 1.f);
   calcNextOscSampleFmBlock(&oscB, modBuf, &b, 1u, 1.f);

   out = (int16_t)(a + frac * (float)(b - a));
   osc->phase = phase + osc->phaseInc;
   osc->output = out;
   return out;
}
```

#### 2c. Guard insertion into `calcNextOscSampleBlock()` (live line 352)

Current first line of function body:
```c
	switch(osc->waveform)
```

Replace with:
```c
	/* Waveform blend guard: if modulation has set a fractional waveform target
	   for this oscillator this DSP block, render both neighbors and blend. */
	if (osc_waveInterpActive(osc)) {
		calcPeriodicInterpBlock(osc, buf, size, gain);
		return;
	}

	switch(osc->waveform)
```

#### 2d. Guard insertion into `calcNextOscSample()` (live line 390)

Current first line of function body:
```c
	switch(osc->waveform)
```

Replace with:
```c
	if (osc_waveInterpActive(osc)) {
		return calcPeriodicInterp(osc);
	}

	switch(osc->waveform)
```

#### 2e. Guard insertion into `calcNextOscSampleFmBlock()` (live line 231)

Current first line of function body:
```c
	switch(osc->waveform)
```

Replace with:
```c
	if (osc_waveInterpActive(osc)) {
		calcPeriodicInterpFmBlock(osc, modBuffer, buf, size, gain);
		return;
	}

	switch(osc->waveform)
```

#### 2f. Guard insertion into `calcNextOscSampleFm()` (live line 267)

Current first line of function body:
```c
	switch(osc->waveform)
```

Replace with:
```c
	if (osc_waveInterpActive(osc)) {
		return calcPeriodicInterpFm(osc, modOsc);
	}

	switch(osc->waveform)
```

**Why all four dispatch functions**: The four entry points cover the full cross product of
block/scalar × FM/non-FM. DrumVoice voices call the block paths; the cymbal/snare/hihat
scalar paths call `calcNextOscSample()`. Missing any one of these would leave that class of
voice without waveform blend.

**Why `calcPeriodicInterp` uses `calcNextOscSampleBlock` with size=1**: This avoids
duplicating the waveform-dispatch switch a fifth time. Size=1 is valid for all block
renderers. The scratch buffers `osc_interp_a/b` are written and immediately read.

---

### CHANGE 3 — `modulationNode.h`: New declarations

**File**: `mainboard/LxrStm32/src/DSPAudio/modulationNode.h`

Add after the existing `void modNode_setDestination(...)` declaration (line 71):

```c
/* Maximum number of oscillators that receive waveform blend state per DSP block.
   Limits the double-render cost: the first OSC_WAVE_INTERP_MAX_ACTIVE eligible
   waveform-parameter modulation targets get fractional blend state; later ones
   fall through to integer waveform IDs as normal. */
#define OSC_WAVE_INTERP_MAX_ACTIVE 4u

/* Enable / disable waveform-parameter interpolation.
   When enabled, modNode_updateValue() writes fractional blend state to oscillators
   for TYPE_UINT8 waveform parameters instead of truncating to an integer ID.
   Called from frontParser_handleSeqCC() on FRONT_SEQ_OSC_WAVE_INTERPOLATION. */
void modNode_setWaveInterpEnabled(uint8_t enabled);

/* Returns the current waveform interp enable flag (0 or 1). Read by
   osc_waveInterpActive() in the audio render path. */
uint8_t modNode_getWaveInterpEnabled(void);

/* Returns the current DSP block generation counter. Read by osc_waveInterpActive()
   to detect and reject stale per-oscillator blend state. */
uint32_t modNode_getWaveInterpGeneration(void);
```

Also add `#include "Oscillator.h"` at the top of `modulationNode.h` if it is not already
present — `modNode_getWaveTargetOsc()` uses `OscInfo*`. Check the current include list;
`modulationNode.h` currently includes only `stm32f4xx.h` and `Preset/ParameterArray.h`.
Add `#include "Oscillator.h"` between those.

---

### CHANGE 4 — `modulationNode.c`: New state variables, resetTargets bump, updateValue interp branch

**File**: `mainboard/LxrStm32/src/DSPAudio/modulationNode.c`

#### 4a. Add new includes

The live file currently includes `DrumVoice.h`, `CymbalVoice.h`, `HiHat.h`, `Snare.h`,
`sequencer.h`, `MorphEngine.h`. Add after the existing includes:

```c
#include "Oscillator.h"
#include "../SampleRom/SampleMemory.h"
```

`Oscillator.h` is needed for `OscInfo*` return type of `modNode_getWaveTargetOsc()` and
for the `osc_setWaveInterp()` / `osc_clearWaveInterp()` calls.
`SampleMemory.h` is needed for `sampleMemory_getNumSamples()` in `modNode_getMaxWaveformIndex()`.

#### 4b. Add three new static state variables

After the existing `INCCMZ ModulationNode macroModulators[4];` (line 49), add:

```c
/* Waveform-parameter interpolation state.
   modNode_waveInterpEnabled: set by modNode_setWaveInterpEnabled() from the
     front-panel receive path; read by osc_waveInterpActive() each render call.
   modNode_waveInterpGeneration: bumped by modNode_resetTargets() at the start of
     every DSP block. Each oscillator stores the generation at which its blend
     state was written; a mismatch means the state is stale and interp is skipped.
     Starts at 1 so that zero-initialised oscillators (generation=0) are always
     stale on the first block, preventing unintended blend on startup.
   modNode_waveInterpActiveCount: reset to 0 each block in modNode_resetTargets().
     Incremented per oscillator in modNode_updateValue(). Capped at
     OSC_WAVE_INTERP_MAX_ACTIVE so the double-render cost is bounded. */
static uint8_t  modNode_waveInterpEnabled      = 0u;
static uint32_t modNode_waveInterpGeneration   = 1u;
static uint8_t  modNode_waveInterpActiveCount  = 0u;
```

#### 4c. Add four new static/non-static helper functions

Insert after the new state variables and before `modNode_init()` (currently line 80):

```c
/* Maps a voice parameter index to the oscillator pointer that should receive
   waveform blend state. Returns NULL for non-waveform parameters.
   Covers all six primary oscillators: drum1-3, snare, cymbal, hihat. */
static OscInfo* modNode_getWaveTargetOsc(uint16_t param)
{
   switch (param) {
   case PAR_OSC_WAVE_DRUM1: return &voiceArray[0].osc;
   case PAR_OSC_WAVE_DRUM2: return &voiceArray[1].osc;
   case PAR_OSC_WAVE_DRUM3: return &voiceArray[2].osc;
   case PAR_OSC_WAVE_SNARE: return &snareVoice.osc;
   case PAR_WAVE1_CYM:      return &cymbalVoice.osc;
   case PAR_WAVE1_HH:       return &hatVoice.osc;
   default:                 return NULL;
   }
}

/* Returns the highest valid waveform ID at the current sample count.
   OSC_SAMPLE_START is the first user-sample waveform ID.
   If no samples are loaded, the highest waveform is CRASH (0x05). */
static uint8_t modNode_getMaxWaveformIndex(void)
{
   const uint8_t sampleCount = sampleMemory_getNumSamples();
   if (sampleCount == 0u) {
      return CRASH;
   }
   /* OSC_SAMPLE_START + sampleCount - 1 is the last valid waveform ID.
      Cast to uint16_t before subtraction to avoid uint8_t underflow if the
      sum exceeded 255 (impossible given SAMPLE_MAX_COUNT=248, but explicit). */
   const uint16_t maxWave = (uint16_t)OSC_SAMPLE_START + (uint16_t)sampleCount - 1u;
   return (maxWave > 255u) ? 255u : (uint8_t)maxWave;
}

void modNode_setWaveInterpEnabled(uint8_t enabled)
{
   modNode_waveInterpEnabled = enabled ? 1u : 0u;
}

uint8_t modNode_getWaveInterpEnabled(void)
{
   return modNode_waveInterpEnabled;
}

uint32_t modNode_getWaveInterpGeneration(void)
{
   return modNode_waveInterpGeneration;
}
```

#### 4d. Update `modNode_resetTargets()` to bump generation and reset active count

**Current `modNode_resetTargets()` body** (lines 143–159):
```c
void modNode_resetTargets()
{
	uint8_t i;
	for(i=0;i<6;i++)
	{
		paramArray_setParameter(velocityModulators[i].destination,velocityModulators[i].originalValue);
	}
    
	paramArray_setParameter(voiceArray[0].lfo.modTarget.destination,voiceArray[0].lfo.modTarget.originalValue);
	...


}
```

**Change**: Insert two lines at the very top of the function body, before the `uint8_t i;`:
```c
void modNode_resetTargets()
{
	/* Bump the waveform-interp generation epoch and reset the per-block active
	   count. Oscillators that were not refreshed this block have a stale
	   generation and will fail the osc_waveInterpActive() guard automatically.
	   Wrap 0 to 1 so zero-initialised oscillators are always stale. */
	modNode_waveInterpGeneration++;
	if (modNode_waveInterpGeneration == 0u) {
		modNode_waveInterpGeneration = 1u;
	}
	modNode_waveInterpActiveCount = 0u;

	uint8_t i;
	for(i=0;i<6;i++)
	{
		paramArray_setParameter(velocityModulators[i].destination,velocityModulators[i].originalValue);
	}
	... (rest unchanged)
```

#### 4e. Update `modNode_updateValue()` TYPE_UINT8 positive-amount branch

The live `modNode_updateValue()` has the following structure in the positive `vm->amount`
branch (lines 281–303):

```c
   switch(p->type)
   {
   case TYPE_UINT8:
       (*((uint8_t*)p->ptr)) = (*((uint8_t*)p->ptr)) * vm->amount * val + (1.f-vm->amount) * (*((uint8_t*)p->ptr));
       break;
   ...
   }
```

The `TYPE_UINT8` case computes a float product and writes a truncated byte directly to
`*((uint8_t*)p->ptr)`. We need to intercept this path when the destination is a waveform
parameter and the interp flag is on. The reference code refactors this into a local variable
first, then tests the waveform-interp path. We do the same:

**Replace the `case TYPE_UINT8:` block in the positive-amount branch** (lines 283–285 of
the live file) with:

```c
   case TYPE_UINT8:
   {
      uint8_t *dst = (uint8_t*)p->ptr;
      const float current = (float)*dst;
      float modulated = current * vm->amount * val + (1.f - vm->amount) * current;
      if (modulated < 0.f) {
         modulated = 0.f;
      }

      /* Waveform blend path: if interp is enabled and this is a waveform parameter
         with budget remaining, write fractional blend state to the oscillator instead
         of truncating modulated to an integer ID immediately.
         The integer floor is still written to *dst so that osc->waveform reflects
         the base waveform; osc_waveInterpActive() uses waveInterpFrac on top of that. */
      if (modNode_waveInterpEnabled) {
         OscInfo *osc = modNode_getWaveTargetOsc(vm->destination);
         if (osc != NULL && modNode_waveInterpActiveCount < OSC_WAVE_INTERP_MAX_ACTIVE) {
            const uint8_t maxWave = modNode_getMaxWaveformIndex();
            if (modulated > (float)maxWave) {
               modulated = (float)maxWave;
            }

            uint8_t base = (uint8_t)modulated;
            if (base > maxWave) {
               base = maxWave;
            }
            float frac = modulated - (float)base;
            uint8_t next = base;
            if (base < maxWave) {
               next = (uint8_t)(base + 1u);
            } else {
               /* At the ceiling waveform: clamp fraction to 0 so interp guard
                  rejects it and the hard ceiling waveform is rendered. */
               frac = 0.f;
            }

            *dst = base;
            osc_setWaveInterp(osc, frac, next, modNode_waveInterpGeneration);
            modNode_waveInterpActiveCount++;
            break;
         }
      }

      *dst = (uint8_t)modulated;
      break;
   }
```

**Why this location**: The live code's positive-amount `TYPE_UINT8` case (lines 283–285)
is where waveform parameter values are computed and written during velocity/LFO modulation.
This is exactly the point at which the reference code intercepts the value. The negative-amount
branch (lines 256–258) uses a different formula that already writes directly and does not
commonly apply to waveform parameters; leave it unchanged.

**Why `frac = 0.f` at ceiling rather than `frac = modulated - base`**: When `base == maxWave`,
`next` cannot go above `maxWave` without leaving the valid waveform range. Setting `frac = 0.f`
causes `osc_waveInterpActive()` to reject the state (`waveInterpFrac <= 0.f`), so the oscillator
renders just `base` (= `maxWave`) with no blend. The ceiling waveform plays cleanly.

---

### CHANGE 5 — `avrCommsReceivingProtocol.h`: New opcode

**File**: `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`

After line 248 (`#define SEQ_BACKGROUND_SWAP_DONE 0x6e`), add:
```c
#define SEQ_OSC_WAVE_INTERPOLATION 0x6f  /* enable(1)/disable(0) waveform blend in modulation */
```

---

### CHANGE 6 — `menu.c`: Send case in `menu_parseGlobalParam()`

**File**: `front/LxrAvr/Menu/menu.c`

Inside `menu_parseGlobalParam()`, add after the final active case (before the closing brace
of the switch):
```c
   case PAR_OSC_WAVE_INTERPOLATION:
      /* Transmit waveform-blend enable flag to STM. value: 0=off, 1=on (DTYPE_ON_OFF).
         Received by STM as FRONT_SEQ_OSC_WAVE_INTERPOLATION, stored via
         modNode_setWaveInterpEnabled(). Sent on boot and every file load by
         menu_sendAllGlobals(). */
      avrComms_sendData(SEQ_CC, SEQ_OSC_WAVE_INTERPOLATION, value);
      break;
```

---

### CHANGE 7 — `frontPanelReceivingProtocol.h`: New opcode

**File**: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`

After line 195 (`#define FRONT_SEQ_BACKGROUND_SWAP_DONE 0x6e`), add:
```c
#define FRONT_SEQ_OSC_WAVE_INTERPOLATION 0x6f  /* enable(1)/disable(0) waveform blend in modulation */
```

Must equal `SEQ_OSC_WAVE_INTERPOLATION = 0x6f` from Change 5.

---

### CHANGE 8 — `frontPanelReceivingProtocol.c`: Receive case

**File**: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Inside `frontParser_handleSeqCC()`, add after the `FRONT_SEQ_TRACK_NOTE7` block:
```c
      case FRONT_SEQ_OSC_WAVE_INTERPOLATION:
         /* data2: 0 = disable waveform-blend modulation, 1 = enable.
            Sent by AVR on every globals push (boot, file load, live menu edit).
            Stored in modNode_waveInterpEnabled via modNode_setWaveInterpEnabled(). */
         modNode_setWaveInterpEnabled(frontParser_command.data2);
         break;
```

Verify that `frontPanelReceivingProtocol.c` already includes `DSPAudio/modulationNode.h`.
If not, add it alongside the other `DSPAudio/` includes at the top of the file.

---

## Summary Table

| # | File | Change |
|---|------|--------|
| 1 | `DSPAudio/Oscillator.h` | Add `waveInterpFrac`, `waveInterpNext`, `_wavePad`, `waveInterpGeneration` to `OscInfo`; add `osc_clearWaveInterp()` and `osc_setWaveInterp()` prototypes |
| 2a | `DSPAudio/Oscillator.c` | Add `#include "modulationNode.h"` |
| 2b | `DSPAudio/Oscillator.c` | Add `osc_interp_a/b` scratch buffers; add `osc_clearWaveInterp`, `osc_setWaveInterp`, `osc_waveInterpActive`, `calcPeriodicInterpBlock`, `calcPeriodicInterp`, `calcPeriodicInterpFmBlock`, `calcPeriodicInterpFm` |
| 2c–2f | `DSPAudio/Oscillator.c` | Insert interp guard at top of `calcNextOscSampleBlock`, `calcNextOscSample`, `calcNextOscSampleFmBlock`, `calcNextOscSampleFm` |
| 3 | `DSPAudio/modulationNode.h` | Add `OSC_WAVE_INTERP_MAX_ACTIVE`; add `modNode_setWaveInterpEnabled`, `modNode_getWaveInterpEnabled`, `modNode_getWaveInterpGeneration` prototypes; add `#include "Oscillator.h"` |
| 4a | `DSPAudio/modulationNode.c` | Add `#include "Oscillator.h"` and `#include "../SampleRom/SampleMemory.h"` |
| 4b | `DSPAudio/modulationNode.c` | Add `modNode_waveInterpEnabled`, `modNode_waveInterpGeneration`, `modNode_waveInterpActiveCount` statics |
| 4c | `DSPAudio/modulationNode.c` | Add `modNode_getWaveTargetOsc`, `modNode_getMaxWaveformIndex`, `modNode_setWaveInterpEnabled`, `modNode_getWaveInterpEnabled`, `modNode_getWaveInterpGeneration` |
| 4d | `DSPAudio/modulationNode.c` | `modNode_resetTargets()`: add generation bump + active-count reset at top |
| 4e | `DSPAudio/modulationNode.c` | `modNode_updateValue()` positive-amount `TYPE_UINT8` case: intercept waveform parameters for fractional blend state |
| 5 | `avrComms/avrCommsReceivingProtocol.h` | `#define SEQ_OSC_WAVE_INTERPOLATION 0x6f` |
| 6 | `Menu/menu.c` | `case PAR_OSC_WAVE_INTERPOLATION:` in `menu_parseGlobalParam()` |
| 7 | `uARTFrontSYX/frontPanelReceivingProtocol.h` | `#define FRONT_SEQ_OSC_WAVE_INTERPOLATION 0x6f` |
| 8 | `uARTFrontSYX/frontPanelReceivingProtocol.c` | `case FRONT_SEQ_OSC_WAVE_INTERPOLATION:` calling `modNode_setWaveInterpEnabled()` |

No new files. No existing function signatures changed except `modNode_resetTargets()`
(no callers change — the new state management is internal).

---

## Risks and Mitigations

### R1 — Circular include: `modulationNode.h` includes `Oscillator.h`
`Oscillator.h` includes `SampleMemory.h` and `config.h`. Neither of those includes
`modulationNode.h`. The include chain is `modulationNode.h` → `Oscillator.h` → `SampleMemory.h`
with no cycle. Verify with a full build.

### R2 — Oscillator.c includes modulationNode.h, which includes Oscillator.h
This creates `Oscillator.c` → `modulationNode.h` → `Oscillator.h`. A `.c` file including
a header that includes its own header is fine as long as include guards are present on both.
`Oscillator.h` already has `#ifndef OSCILLATOR_H_` guards. No issue.

### R3 — `osc_interp_a/b` scratch buffers are shared, not ISR-safe
Both buffers are static globals. If the audio render callback were reentrant they would be
corrupted. The STM DMA audio callback is not reentrant (single ISR priority, main-loop
render). Safe under current architecture.

### R4 — Generation counter wraps to 0
Handled: `modNode_resetTargets()` skips 0 (`if == 0, set to 1`). OscInfo fields are
zero-initialised in BSS. A zero `waveInterpGeneration` on an oscillator will never match
the live generation (always >= 1), so unrefreshed oscillators are always stale.

### R5 — Budget cap `OSC_WAVE_INTERP_MAX_ACTIVE`
Set to 4. There are 6 primary oscillators; only the first 4 to call `modNode_updateValue()`
with a waveform parameter will get blend state this block. The other 2 will get hard integer
IDs as before. This is the reference design's approach. Adjust the constant if CPU budget allows more.

### R6 — The negative `vm->amount` branch is not modified
The negative-amount `TYPE_UINT8` case (live lines 256–258) uses a subtraction formula rather
than the blend formula. Waveform parameters are not typically modulated with negative amounts,
but if they are they will receive a hard integer write as before. This is intentional scope
limitation consistent with the reference.

---

## Verification

1. `make -C mainboard/LxrStm32 -j4 stm32` — no errors. Check for circular include warnings.
2. `make -C front/LxrAvr -j4 avr` — no errors.
3. On hardware: assign an LFO to an OSC_WAVE parameter on Drum 1 (e.g., SINE at resting,
   SAW as LFO target with positive amount and slow rate). With WavIntrp OFF: confirm hard
   waveform switch. With WavIntrp ON: confirm audible cross-fade between waveforms as the
   LFO sweeps through fractional positions.
4. Velocity modulation targeting an OSC_WAVE parameter: confirm blend state is set per
   trigger based on velocity.
5. Power-cycle with WavIntrp ON in globals: confirm STM receives flag via `menu_sendAllGlobals()`
   on boot.
6. Morph between two presets with different waveforms with WavIntrp ON: confirm morph also
   uses blend (morph calls the same modulation path when a waveform parameter is in the
   morph endpoint delta).
