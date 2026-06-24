# SAMPLE_PLAN_PHASE_5: Waveform Interpolation and Global Setting

## Scope

This phase covers the stretch goal: a global toggle for low-cost waveform interpolation while modulating waveform parameters.

Relevant current code:

- `front/LxrAvr/Parameters.h`: AVR parameter enum and globals.
- `front/LxrAvr/Menu/menu.c`: dtype table, global send path.
- `front/LxrAvr/Menu/menuPages.h`: user-owned menu page layout.
- `front/LxrAvr/Menu/MenuText.h`: text IDs and display names.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`: global/control opcodes.
- `mainboard/LxrStm32/src/Preset/ParameterArray.h`: STM active sound parameter enum, not full global enum.
- `mainboard/LxrStm32/src/DSPAudio/modulationNode.c/.h`: modulation application.
- `mainboard/LxrStm32/src/DSPAudio/Oscillator.c/.h`: oscillator rendering.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c/.h`: STM control receive.

## Audit Result

The draft plan's insertion point is wrong for this repository:

- AVR `PAR_FOLLOW` is not the final global. Many global parameters follow it.
- STM `ParameterArray.h` currently comments out the old global enum block. STM does not have live `PAR_OSC_WAVE_INTERP` global handling via `ParameterArray`.
- Adding an AVR parameter shifts `NUM_PARAMS`, which affects global file storage loops.
- The user explicitly said text/menu additions should be left for manual edit. This document therefore marks `menuPages.h` and display text as user-owned, but still lists exact places where the user must add them.

## Recommended Architecture

Use the existing AVR global-control pattern:

1. AVR stores `PAR_OSC_WAVE_INTERP` in `parameter_values[]` as a global.
2. AVR sends `SEQ_CC, SEQ_OSC_WAVE_INTERP, value` from `menu_parseGlobalParam()`.
3. STM receives `FRONT_SEQ_OSC_WAVE_INTERP` and calls `modNode_setWaveInterpEnabled(value)`.
4. `modulationNode.c` writes per-oscillator interpolation state only when waveform modulation targets an oscillator wave parameter.
5. `Oscillator.c` consumes that state in the normal wavetable oscillator path.

Do not route this through STM `ParameterIngress.c` unless STM globals are restored first.

## Exact Code Changes

### 1. Add AVR global parameter safely

Target: `front/LxrAvr/Parameters.h`

Add before `PAR_GLOBAL_SETTINGS_VERSION`:

```c
   PAR_OSC_WAVE_INTERP,      // bool, enables oscillator waveform crossfade

   PAR_GLOBAL_SETTINGS_VERSION,
```

Why this line needs to happen:

- Inserting just before `PAR_GLOBAL_SETTINGS_VERSION` keeps the version marker at the end of the active global settings block and avoids inserting near `PAR_FOLLOW`, where many existing globals would shift unexpectedly.

Risk:

- Any added enum shifts `PAR_GLOBAL_SETTINGS_VERSION` and MIDI note globals after it. Bump `FILE_VERSION` and add default handling for older `glo.cfg`, `.all`, and `.prf` global blocks.

### 2. Add AVR dtype

Target: `front/LxrAvr/Menu/menu.c`

Add in `parameter_dtypes[]` at the matching enum position:

```c
      /*PAR_OSC_WAVE_INTERP*/ DTYPE_ON_OFF,
      /*PAR_GLOBAL_SETTINGS_VERSION*/ DTYPE_0B127,
```

Why this line needs to happen:

- The encoder/menu code needs to clamp the new value to `0/1`.
- The comment must match the enum so future parameter-table audits can catch misalignment.

Risk:

- `parameter_dtypes[]` is positional. A missing line will corrupt every dtype after the insertion point.

### 3. Add AVR default

Target: `front/LxrAvr/Menu/menu.c`, `menu_init()`

Add near other global defaults:

```c
   parameter_values[PAR_OSC_WAVE_INTERP] = 0;
```

Why this line needs to happen:

- Default-off protects STM32F4 CPU budget until the user enables the feature.

Risk:

- Default-on could increase DSP load immediately after firmware update.

### 4. Add AVR send opcode

Targets:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`

Add the next free sequencer command after `0x6e`:

```c
#define SEQ_OSC_WAVE_INTERP 0x6f
```

STM:

```c
#define FRONT_SEQ_OSC_WAVE_INTERP 0x6f
```

Why each line needs to happen:

- This follows the existing global-control opcode style.
- `0x6f` is adjacent to the recent background-load opcodes and not used in current headers.

Risk:

- Verify no external docs reserve `0x6f` before implementation.

### 5. Send the AVR global value

Target: `front/LxrAvr/Menu/menu.c`, `menu_parseGlobalParam()`

Add:

```c
   case PAR_OSC_WAVE_INTERP:
      parameter_values[PAR_OSC_WAVE_INTERP] = value;
      avrComms_sendData(SEQ_CC, SEQ_OSC_WAVE_INTERP, value ? 1u : 0u);
      break;
```

Why each line needs to happen:

- The assignment keeps the front-panel cache authoritative for global save/load.
- The ternary clamps the value to one bit before it crosses the protocol.
- `SEQ_CC` keeps the message in the existing global/control path.

Risk:

- `menu_sendAllGlobals()` will send this on boot and globals load. That is desired, but only after STM implements the receive side.

### 6. Receive on STM

Target: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Inside `frontParser_handleSeqCC()` add:

```c
      case FRONT_SEQ_OSC_WAVE_INTERP:
         modNode_setWaveInterpEnabled(frontParser_command.data2 ? 1u : 0u);
         break;
```

Add include if needed:

```c
#include "modulationNode.h"
```

Why each line needs to happen:

- The receive case is the STM-side bridge from AVR global settings to DSP modulation behavior.
- Clamping on STM protects against malformed input.
- Including `modulationNode.h` exposes the setter without reaching into static state.

Risk:

- `frontPanelReceivingProtocol.c` already includes many DSP headers. Adding another is acceptable but reinforces the need to keep the setter narrow.

### 7. Add modulation-node interpolation API

Target: `mainboard/LxrStm32/src/DSPAudio/modulationNode.h`

Add:

```c
#define OSC_WAVE_INTERP_MAX_ACTIVE 1u

void modNode_setWaveInterpEnabled(uint8_t enabled);
uint8_t modNode_getWaveInterpEnabled(void);
uint32_t modNode_getWaveInterpGeneration(void);
void modNode_beginWaveInterpBlock(void);
```

Why each line needs to happen:

- `OSC_WAVE_INTERP_MAX_ACTIVE` caps CPU cost on STM32F4.
- The setter is called from STM front-panel receive.
- The getter/generation APIs let `Oscillator.c` decide whether its interpolation state is current.
- `beginWaveInterpBlock()` resets per-block active count and advances generation.

Risk:

- The active-slot limit means only one oscillator can crossfade per block. This is intentional for CPU safety but should be documented to the user.

Target: `modulationNode.c`

Add static state:

```c
static INCCMZ uint8_t modNode_waveInterpEnabled;
static INCCMZ uint32_t modNode_waveInterpGeneration;
static INCCMZ uint8_t modNode_waveInterpActiveCount;
```

Why each line needs to happen:

- CCMZ keeps fast mutable DSP-control state in zero-initialized CCM RAM.
- Generation avoids stale interpolation values being reused after modulation stops.
- Active count enforces the CPU cap.

### 8. Map waveform destinations to oscillators

Target: `modulationNode.c`

Add:

```c
static OscInfo* modNode_getWaveTargetOsc(uint16_t destination)
{
   switch(destination)
   {
      case PAR_OSC_WAVE_DRUM1: return &voiceArray[0].osc;
      case PAR_OSC_WAVE_DRUM2: return &voiceArray[1].osc;
      case PAR_OSC_WAVE_DRUM3: return &voiceArray[2].osc;
      case PAR_OSC_WAVE_SNARE: return &snareVoice.osc;
      case PAR_WAVE1_CYM:      return &cymbalVoice.osc;
      case PAR_WAVE1_HH:       return &hatVoice.osc;
      default:                 return 0;
   }
}
```

Why each line needs to happen:

- Only primary waveform parameters have obvious audible crossfade targets.
- Mod oscillators can be added later, but starting with primary oscillators reduces CPU and test surface.

Risk:

- If LFO modulation targets mod oscillator waveforms, those will still step until explicitly added.

### 9. Extend `OscInfo` for interpolation

Target: `mainboard/LxrStm32/src/DSPAudio/Oscillator.h`

Add:

```c
   uint8_t waveInterpNext;
   float waveInterpFrac;
   uint32_t waveInterpGeneration;
```

Why each line needs to happen:

- `waveInterpNext` stores the adjacent waveform index.
- `waveInterpFrac` stores the crossfade amount.
- `waveInterpGeneration` lets the oscillator reject stale crossfade state.

Risk:

- Adds RAM to every oscillator. Small, but verify stack/RAM map if other phases grow buffers.

### 10. Split waveform modulation values

Target: `modulationNode.c`, inside `modNode_updateValue()` `TYPE_UINT8` paths.

Add before writing the ordinary integer value:

```c
if(modNode_waveInterpEnabled && modNode_waveInterpActiveCount < OSC_WAVE_INTERP_MAX_ACTIVE)
{
   OscInfo* osc = modNode_getWaveTargetOsc(vm->destination);
   if(osc)
   {
      float modulated = (*((uint8_t*)p->ptr)) * vm->amount * val
                      + (1.f - vm->amount) * (*((uint8_t*)p->ptr));
      uint8_t base = (uint8_t)modulated;
      float frac = modulated - (float)base;

      *((uint8_t*)p->ptr) = base;
      osc->waveInterpNext = (uint8_t)(base + 1u);
      osc->waveInterpFrac = frac;
      osc->waveInterpGeneration = modNode_waveInterpGeneration;
      modNode_waveInterpActiveCount++;
      return;
   }
}
```

Why each line needs to happen:

- The feature only applies when enabled and capacity remains.
- `modNode_getWaveTargetOsc()` prevents non-waveform parameters from being treated as oscillators.
- The fractional part is preserved before the ordinary `uint8_t` write truncates it.
- Returning avoids the old integer write doing a second, inconsistent update.

Risk:

- The shown formula mirrors the positive-amount path only. The negative-amount path needs equivalent handling or an explicit decision that interpolation applies only to non-negative modulation amounts.
- Bounds must clamp `base + 1` to the highest valid built-in waveform. Never crossfade from a built-in waveform to a user sample without a separate implementation.

### 11. Consume crossfade in `Oscillator.c`

Target: `mainboard/LxrStm32/src/DSPAudio/Oscillator.c`

Add include:

```c
#include "modulationNode.h"
```

Add helper:

```c
static uint8_t osc_waveInterpActive(const OscInfo* osc)
{
   return (uint8_t)(osc->waveInterpGeneration == modNode_getWaveInterpGeneration());
}
```

In `calcWavetableOscBlock()` after `oscOut` is calculated:

```c
if(osc_waveInterpActive(osc))
{
   int16_t nextOut = table[osc->waveInterpNext][itg];
   oscOut += osc->waveInterpFrac * (nextOut - oscOut);
}
```

Why each line needs to happen:

- Oscillator consumes only current-generation interpolation state.
- Crossfade happens after normal intra-table interpolation, minimizing extra math.
- Reading `table[waveInterpNext]` crossfades adjacent waveform tables.

Risk:

- Current built-in wavetable arrays are separated by waveform family (`sawTable`, `triTable`, `recTable`) rather than a single `table[waveform]` bank. The draft `sampleData_nextWave` idea does not match this code. Implementation must either build a resolver for SINE/TRI/SAW/REC/NOISE/CRASH or limit interpolation to compatible wavetable families. This is a major unresolved design item.

### 12. User-owned menu text/page edits

Per user instruction, automated implementation should not edit `front/LxrAvr/Menu/menuPages.h`.

The user must manually add `PAR_OSC_WAVE_INTERP` to a global page row and add text IDs/names in:

- `front/LxrAvr/Menu/menu.h`
- `front/LxrAvr/Menu/menu.c` value-name table
- `front/LxrAvr/Menu/MenuText.h` if using a PROGMEM string table entry
- `front/LxrAvr/Menu/menuPages.h`

Risk:

- If the parameter exists but is not on a menu page, it can still load/save and send defaults, but the user cannot edit it from the panel.

## Interdependencies

- Phase 2 changes `OscInfo`; coordinate struct edits so sample-playback fields and interpolation fields are added once.
- Phase 4 uses `0x5e/0x5f`; Phase 5 uses `0x6f`.
- Global file compatibility must be tested with existing `glo.cfg`, `.all`, and `.prf` files.

## Plan Callouts

- The draft says `ParameterArray.h` should append `PAR_OSC_WAVE_INTERP` near `PAR_FOLLOW`; that is not correct for this repo.
- The draft `Oscillator.c` crossfade code assumes data structures that do not exist here.
- This phase is significantly larger than a simple toggle. Treat it as experimental and default it off.
