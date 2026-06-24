# LXR Sample Import & Handling Implementation Plan

This document acts as the definitive roadmap for implementing the improved sample import system on the STM32F407VGT6 (split-processor LXR design). All architectural pitfalls and constraints have been verified against the current firmware.

---

## Phase 1: Flash Memory Guards and Writing Improvements
**Objective**: Harden internal flash writes on the STM32F407VGT6 (1MB flash) to prevent hangs, watchdog resets, or corrupt tables during bulk sample import.

**Target File**: `mainboard/LxrStm32/src/SampleRom/flash_if.c` (and relevant sample memory controllers)

**Implementation Steps**:
1. **Flash Erase/Write Lifecycle Management**
   - **What**: Integrate safe flash unlocking, erasure, programming, and locking.
   - **Why**: Internal flash on the STM32F4 blocks the CPU bus during erase/program operations. If not safely managed, it will crash the system.
   - **Interactions**: Interfaces directly with `STM32F4xx_StdPeriph_Driver` flash routines.
   - **Risks**: Erasing 128KB sectors (Sectors 5-11) can take several seconds. If the independent watchdog (IWDG) is active, the CPU will reset mid-erase. If audio DMA is active, bus contention can cause hard faults.

```c
/*
 * Safely erases internal flash sectors for sample storage.
 * Inputs: startAddress (uint32_t) - The absolute flash address to begin erasing.
 * Outputs: uint32_t - 0 on success, 1 on failure.
 * Risks: Blocks CPU. Must pet watchdog and disable audio interrupts.
 */
uint32_t FLASH_If_Erase_Safe(uint32_t startAddress) {
    __disable_irq(); // Prevent audio DMA from firing during bus stall
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | 
                    FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
    
    uint32_t sector = GetSector(startAddress);
    for(uint32_t i = sector; i <= FLASH_Sector_11; i += 8) {
        IWDG_ReloadCounter(); // Prevent watchdog reset during slow erase
        if (FLASH_EraseSector(i, VoltageRange_3) != FLASH_COMPLETE) {
            FLASH_Lock();
            __enable_irq();
            return 1;
        }
    }
    FLASH_Lock();
    __enable_irq();
    return 0;
}
```

---

## Phase 2: Oscillator Fixes & Looped Playback Framework
**Objective**: Fix the 32k phase wrap bug and implement the bit-packing mechanism for loop flags.

**Target File**: `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`
**Implementation Steps**:
1. **Define Bit-masks for `SampleInfo.size`**
   - **What**: Define `SAMPLE_INFO_LOOP_FLAG` (`0x80000000`) and `SAMPLE_INFO_SIZE_MASK` (`0x7fffffff`).
   - **Why**: The `SampleInfo` struct is exactly 12 bytes. Adding a new `looped` boolean would pad the struct to 16 bytes, destroying flash alignment and breaking existing metadata storage tables. We must bit-pack the loop flag into the MSB of the `size` property.

**Target File**: `mainboard/LxrStm32/src/DSPAudio/Oscillator.c`
**Implementation Steps**:
1. **Modify `calcUserSampleOscBlock` Phase Logic**
   - **What**: Unpack the bit-flags and protect the modulo operation.
   - **Why**: The 17-bit fractional shift logic (`oscPhase >> 17`) limits the maximum addressable phase. Samples >32768 samples overflow the 32-bit modulo calculation, causing an inadvertent loop. By bypassing modulo for long samples, we allow the 32-bit `oscPhase` to naturally wrap at `0xFFFFFFFF`, creating a safe one-shot effect.
   - **Interactions**: Called per-audio-block by the DSP engine.

```diff
 void calcUserSampleOscBlock(OscInfo* osc, int16_t* buf, const uint8_t size ,const float gain)
 {
 	SampleInfo info = sampleMemory_getSampleInfo(osc->waveform - OSC_SAMPLE_START);
+	
+	/* Unpack the size and loop flag from the 32-bit info.size */
+	uint32_t sampleSize = info.size & SAMPLE_INFO_SIZE_MASK;
+	uint8_t looped = (info.size & SAMPLE_INFO_LOOP_FLAG) ? 1 : 0;
 
 	int16_t* sampleData = (int16_t*)((int8_t*)(info.offset));
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
 }
```

---

## Phase 3: Dual-Type Sample Write Implementation
**Objective**: Sequentially scan and import `/samples` then `/loops` from FAT32.



---

## Phase 4: Waveform Interpolation and Parameter
**Objective**: Low-DSP cost interpolation allowing smooth morphing between adjacent waveforms.

**Target Files**: `mainboard/LxrStm32/src/Preset/ParameterIngress.c`, `modulationNode.c`, `Oscillator.c`
**Implementation Steps**:
1. **AVR to STM32 Communication**: 
   - **What**: The AVR natively treats `PAR_OSC_WAVE_INTERP` as a standard global parameter. When edited, the AVR sends a standard `SYX_PRM` (Parameter Edit) message to the STM32. 
   - **STM32 Handling**: The STM32 intercepts this in `ParameterIngress.c` and calls `modNode_setWaveInterpEnabled(value)`. 
   - **Note**: The user will manually append `PAR_OSC_WAVE_INTERP` and `TEXT_OSC_INTERP` to `menuPages.h` on the AVR.

2. **Fractional Modulation Split (`modulationNode.c`)**:
   - **What**: Port the fractional interpolation logic from `LXR02Open`.
   - **Why**: Extracts the decimal fraction from an LFO's float modulation signal, updates the base index, and stores the fraction on the oscillator for crossfading.
   - **Risks**: Uses CPU cycles. Limited to one active oscillator slot via `modNode_waveInterpActiveCount` to prevent STM32F4 CPU exhaustion.

```c
/* Extracts fraction and assigns single interpolation slot to the target oscillator */
if (modNode_waveInterpEnabled) {
    OscInfo *osc = modNode_getWaveTargetOsc(vm->destination);
    if (osc && modNode_waveInterpActiveCount < OSC_WAVE_INTERP_MAX_ACTIVE) {
        const uint8_t maxWave = modNode_getMaxWaveformIndex();
        if (modulated > maxWave) { modulated = maxWave; }
        
        uint8_t base = (uint8_t)modulated;
        float frac = modulated - (float)base;
        uint8_t next = base;
        
        if (base < maxWave) {
            next = (uint8_t)(base + 1u);
        } else {
            frac = 0.f; // Max boundary, no crossfade target
        }

        *dst = base; // Apply integer to normal wave target
        osc->waveInterpNext = next;
        osc->waveInterpFrac = frac;
        osc->waveInterpGeneration = modNode_waveInterpGeneration;
        modNode_waveInterpActiveCount++;
        break; // Slot consumed
    }
}
```

3. **PCM Crossfade (`Oscillator.c`)**:
   - **What**: Perform linear interpolation between the base PCM sample and the next adjacent PCM sample.
   - **Why**: Allows seamless waveform sweeping.

```c
/* In calcSampleOscBlock: */
if (osc->waveInterpGeneration == modNode_getWaveInterpGeneration()) {
    /* Perform linear crossfade using pre-calculated fraction */
    int16_t nextOut = sampleData_nextWave[itg];
    oscOut = oscOut + osc->waveInterpFrac * (nextOut - oscOut);
}
```
