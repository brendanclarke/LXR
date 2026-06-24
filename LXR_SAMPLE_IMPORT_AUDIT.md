# Sample Import and Handling Implementation Plan (Expanded)
This plan details the implementation of improved sample import and handling in the split `LXR` firmware by porting the reference features from `LXR02Open` and addressing the explicit architectural and hardware constraints identified in the subagent audit.
---
## Proposed Changes
### 1. 32-bit Sample Position Index and Loop Protection
**Target File**: `mainboard/LxrStm32/src/DSPAudio/Oscillator.c`

**Why it needs to change**: The existing 17-bit fractional shift logic (`oscPhase >> 17`) limits the maximum addressable phase before modulo wraps it back to 0. For samples larger than `32768u`, the phase overflows and causes an inadvertent loop. We must also extract the new loop and size data from the bit-packed `info.size` field since `SampleInfo` does not currently possess a native `looped` member.
**Line-by-line implementation strategy**:
Inside `calcUserSampleOscBlock`, we will add bit-unpacking and modify the phase increment logic.
```diff
 		//one shot
 void calcUserSampleOscBlock(OscInfo* osc, int16_t* buf, const uint8_t size ,const float gain)
 {
 	SampleInfo info = sampleMemory_getSampleInfo(osc->waveform - OSC_SAMPLE_START);
+	
+	/* Unpack the size and loop flag from the 32-bit info.size */
+	uint32_t sampleSize = info.size & SAMPLE_INFO_SIZE_MASK;
+	uint8_t looped = (info.size & SAMPLE_INFO_LOOP_FLAG) ? 1 : 0;
 
 	int16_t* sampleData = (int16_t*)((int8_t*)(info.offset));
     // ...
-		//one shot
-		if(itg < info.size)
-		{
-			osc->phase = oscPhase + osc->phaseInc;
-		}
+		if(looped)
+		{
+			uint32_t nextPhase = oscPhase + osc->phaseInc;
+			/* If sample size >= 32768, bypass modulo wrap to prevent overflow.
+			 * oscPhase will naturally wrap at 2^32, creating a one-shot effect 
+			 * which prevents inadvertent looping for long samples. */
+			if(sampleSize < 32768u) {
+				uint32_t loopPhase = sampleSize << 17;
+				if(nextPhase >= loopPhase)
+					nextPhase %= loopPhase;
+			}
+			osc->phase = nextPhase;
+		}
+		else if(itg < sampleSize)
+		{
+			osc->phase = oscPhase + osc->phaseInc;
+		}
     // ...
 }
```
---
### 2. Unified 'SAMPLES' and 'LOOPS' Import
**Target Files**: `mainboard/LxrStm32/src/Hardware/SD/filesystem.c` and `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`
**Why it needs to change**: The user expects a single modal UI action to sequentially sweep both the `/samples` and `/loops` directories, appending both into flash and tagging loops natively. Since `SampleInfo` is densely packed, we must implement a bit-packing strategy for the `info.size` property so we don't alter the 12-byte struct layout and disrupt flash storage mapping**Line-by-line implementation strategy**:
1. **In `SampleMemory.h`**: Add the bit-mask constants. Do not expand `SampleInfoStruct` directly to avoid alignment issues in flash.
```diff
+ #define SAMPLE_INFO_LOOP_FLAG       ((uint32_t)0x80000000)
+ #define SAMPLE_INFO_SIZE_MASK       ((uint32_t)0x7fffffff)
  typedef struct SampleInfoStruct {
      char     name[3];   /* 3-char sample name */
      uint32_t size;      /* Bit 31: loop flag. Bits 30-0: size in 16-bit samples */
      uint32_t offset;    /* absolute flash address of the sample data */
  } SampleInfo;
```
2. **In `filesystem.c`**: Add the unified install flow.
```c
/*
 * Reads a directory for .WAV files and sequentially installs them to flash.
 * Validates the file header and applies the loop flag to the metadata if requested.
 * 
 * @param folder The name of the directory to scan (e.g., "samples" or "loops").
 * @param append If 0, erases flash and starts at index 0. If 1, appends to existing samples.
 * @param looped If 1, applies the SAMPLE_INFO_LOOP_FLAG to the SampleInfo size field.
 * @return 1 on success, 0 on error.
 */
static uint8_t filesystem_installSampleFolderBlocking(const char *folder, uint8_t append, uint8_t looped) {
    // ... directory parsing, size checking, and sampleMemory_installOneSample loops
    // Port existing directory scanning logic, using 'looped' flag during metadata generation.
}
/*
 * Sweeps the SD card for both /SAMPLES and /LOOPS, installing them in one contiguous pass.
 * Erases the flash and places standard samples first, then appends looped samples.
 * 
 * @return 1 on success, 0 on error.
 */
uint8_t filesystem_installAllSamplesAndLoopsBlocking(void) {
    uint8_t ok = filesystem_installSampleFolderBlocking("samples", 0, 0);
    if (ok) {
        ok = filesystem_installSampleFolderBlocking("loops", 1, 1);
    }
    return ok;
}
```
---
### 3. SPI Flash Hardware Safety Guards
**Target File**: `mainboard/LxrStm32/src/Hardware/SPI_flash/spiFlash.c`
**Why it needs to change**: The STM32F4 implementation using the `SST25VF032B` SPI NOR flash is missing critical safety guards compared to the LXR02Open `STM32F7` equivalent. These guards must be added to prevent catastrophic flash corruption, hard faults during audio DMA, and bricked metadata tables.
**Line-by-line implementation strategy**:
- **Write Enable (WREN) Enforcement**: Explicitly inject a `spiFlash_writeEnable()` command before *every* erase, write, or AAI program cycle.
- **AAI (Auto Address Increment) Sequence Integrity**: In the mass-program loops, explicitly send `WRDI` (Write Disable) at the end of the AAI sequence to safely close the write cycle.
- **Block Protection (BP)**: Wrap erase/program functions with `spiFlash_writeStatusRegister()` calls to clear BP bits to `0x00` prior to writing, and restore them post-write to prevent accidental sector locks.
- **Status Validation and Bounds Checking**: 
```c
/* Draft check for write bounds */
if (address < SAMPLE_FLASH_START || address > SAMPLE_FLASH_END) return SPI_ERR_BOUNDS;
/* Draft post-write verification */
uint8_t status = spiFlash_readStatusRegister();
if ((status & STATUS_BPL_MASK) || !(status & STATUS_READY_MASK)) return SPI_ERR_WRITE_FAIL;
```
- **Read-back Verification**: After writing the final `SampleInfo` metadata table to flash, read it back into memory and `memcmp` against the buffer to ensure power wasn't lost.
- **Watchdog & IRQ**: Add `IWDG_ReloadCounter()` periodically within the block-erase loops to prevent watchdog resets. Wrap the actual SPI transaction payloads in `__disable_irq()` / `__enable_irq()` to prevent the audio codec from contending on the SPI bus.
---
### 4. Waveform Interpolation and Global Setting (Stretch Goal)
**Target Files**: `ParameterArray.h`, `menuPages.h`, `modulationNode.c`, `Oscillator.c`
**Why it needs to change**: The logic does not exist in the split-processor firmware. We must completely port the `modNode_waveInterpGeneration` fractional math, while safely inserting the UI toggle so it doesn't shift existing parameter array indices and break preset compatibility.
**Line-by-line implementation strategy**:
1. **`ParameterArray.h`**:
To maintain preset alignment, `PAR_OSC_WAVE_INTERP` must be appended strictly at the end of the global settings enumerations, just before `NUM_PARAMS`.
```diff
     PAR_FETCH,
     PAR_FOLLOW,
+    PAR_OSC_WAVE_INTERP,
```
2. **`menuPages.h`**:
Append it to the global page matrix, padding with `TEXT_EMPTY` / `PAR_NONE` to complete the row.
ASSISSTANT SHOULD NEVER EDIT **`menuPages.h`**. THIS MUST BE LEFT FOR USER MANUAL EDIT.
```diff
   TEXT_BPM,   TEXT_QUANTISATION, TEXT_MIDI_CHAN_GLOBAL, TEXT_MIDI_FILT_TX, TEXT_MIDI_FILT_RX, TEXT_MIDI_ROUTING, TEXT_FETCH, TEXT_FOLLOW,
   PAR_BPM,    PAR_QUANTISATION,  PAR_MIDI_CHAN_GLOBAL,  PAR_MIDI_FILT_TX,  PAR_MIDI_FILT_RX,  PAR_MIDI_ROUTING,  PAR_FETCH,  PAR_FOLLOW,
+  TEXT_OSC_INTERP, TEXT_EMPTY, TEXT_EMPTY, TEXT_EMPTY, TEXT_EMPTY, TEXT_EMPTY, TEXT_EMPTY, TEXT_EMPTY,
+  PAR_OSC_WAVE_INTERP, PAR_NONE, PAR_NONE, PAR_NONE, PAR_NONE, PAR_NONE, PAR_NONE, PAR_NONE,
```
3. **`modulationNode.c`**:
**Target File**: `mainboard/LxrStm32/src/DSPAudio/modulationNode.c`
Implement the state and fractional splitting logic exactly as ported from LXR02Open:
```c
static INCCMZ uint8_t modNode_waveInterpEnabled = 0;
static INCCMZ uint32_t modNode_waveInterpGeneration = 1u;
static INCCMZ uint8_t modNode_waveInterpActiveCount = 0u;
**Why it needs to change**: To extract the fractional component from an LFO modifying a waveform and pass it to the single permitted oscillator interpolation slot.
**Line-by-line changes**:
Add interpolation global state variables and update `modNode_updateValue`:
```c
/*
 * Called when a parameter's value is actively updated by a modulation node.
 * If wave interpolation is enabled globally, and the destination is an oscillator wave,
 * it splits the float into an integer wave and a fractional offset for the crossfade.
 */
void modNode_updateValue(ModulationNode* vm, float val) {
    // ... existing switch statement ...
    // ...
    case TYPE_UINT8:
        if (modNode_waveInterpEnabled) {
            OscInfo *osc = modNode_getWaveTargetOsc(vm->destination);
            if (osc && modNode_waveInterpActiveCount < OSC_WAVE_INTERP_MAX_ACTIVE) {
                // Draft logic: split `modulated` float into base index and fraction,
                // set osc->waveInterpNext and osc->waveInterpFrac.
                // Increment modNode_waveInterpActiveCount.
                /*
                 * Extracts fractional offset from the float modulation signal.
                 * The base integer updates the primary wave array index, while
                 * the fraction is stored on the oscillator struct for the DSP crossfade.
                 */
                uint8_t base = (uint8_t)modulated;
                float frac = modulated - (float)base;
                osc->waveInterpNext = base + 1;
                osc->waveInterpFrac = frac;
                osc->waveInterpGeneration = modNode_waveInterpGeneration;
                modNode_waveInterpActiveCount++;
            }
        }
        // ...
    // ...
}
```
4. **`Oscillator.c`**:
**Target File**: `mainboard/LxrStm32/src/DSPAudio/Oscillator.c`
**Why it needs to change**: The oscillator must consume `waveInterpNext` and `waveInterpFrac` to crossfade the two PCM samples during the audio interrupt.
**Line-by-line changes**:
Add the active interpolator check and apply it inside `calcSampleOscBlock`:
Consume the slot inside `calcSampleOscBlock`.
```c
/* 
 * Returns true if the oscillator holds the active slot for this DSP block execution.
 */
static inline uint8_t osc_waveInterpActive(const OscInfo* osc) {
    return (osc->waveInterpGeneration == modNode_getWaveInterpGeneration());
}
void calcSampleOscBlock(...) {
    // ...
    if (osc_waveInterpActive(osc)) {
        /* Perform linear crossfade between base wave PCM and next wave PCM */
        int16_t nextOut = sampleData_nextWave[itg];
        oscOut = oscOut + osc->waveInterpFrac * (nextOut - oscOut);
    }
    // ...
}
```
## User Review Required
Please review the specific line-by-line plan and the added global configuration changes for the waveform interpolation. Is the approach for injecting `PAR_OSC_WAVE_INTERP` onto the global menu page alongside `PAR_FOLLOW` correct?
--> Answer: PAR_OSC_WAVE_INTERP should be added as a parameter. Text additions to the global menu MUST be left to the user.
Please review the complete end-to-end plan, especially:
1. The new UART opcodes (`CMD_REQUEST_SAMPLE_NAME` ~~`0x7A`~~ and ~~`0x7B`~~) to ensure they do not collide with any undocumented internal protocols.
--> Answer: DO NOT USE THOSE OPCODES. use `0x5E` and `0x5F` instead, they are free.
2. The SPI flash safeguards arrayed against the `SST25VF032B` to ensure they meet your stability expectations for STM32F4 IO.
--> Answer: This is not the hardware. The hardware is a STM32F407VGT6 with 1MB internal flash. this is the flash we will use. It should be a similar setup to the LXR02Open, but with half the flash storage in total.