# LXR Sample Import Audit — Agent Session Log

**Trajectory ID**: `ac08cc8f-a31f-49d3-8ac3-6aa6e885ebae`  
**Session Date**: 2026-06-22  
**Repository**: `file:///c:/Users/brendan.clarke/proj/LXR`  
**Branch**: `master`

---

## User Task

> Check `LXR_SAMPLE_IMPORT_AUDIT.md` against the repository. The plan in general seems ok — check the code implementation against what exists for the LXR (split-processor design), using the LXR02Open directory as **reference only**.
>
> In addition, check the flash storage implementation for the STM32F4 against what is used for the STM32F7 for the LXR02Open. What additional guards for flash writing do we need to put in place for the original LXR split-processor design for the STM32F4?

---

## Previous Conversation Context

**Conversation** `0bae6a19-6cf0-4f1c-a9dd-b976d47e699e`: *Implementing Sample Management System*  
- Created: 2026-06-22T12:49:42Z  
- Last modified: 2026-06-22T13:41:08Z

---

## Audit Plan — `LXR_SAMPLE_IMPORT_AUDIT.md`

*(Content read from repository at session start)*

This plan details the implementation of improved sample import and handling in the split `LXR` firmware by porting reference features from `LXR02Open`.

---

### 1. 32-bit Sample Position Index and Loop Protection

**Target File**: `mainboard/LxrStm32/src/DSPAudio/Oscillator.c`

**Why it needs to change**: The 17-bit fractional shift logic (`oscPhase >> 17`) limits the maximum addressable phase before modulo wraps it back to 0. For samples larger than `32768u`, the phase overflows and causes an inadvertent loop. By protecting the modulo operation with a length check, we can allow the 32-bit `oscPhase` to run up to its absolute maximum for long one-shot samples, matching the `LXR02Open` behaviour.

**Proposed change** inside `calcUserSampleOscBlock`:

```diff
 		//one shot
-		if(itg < info.size)
+		if(looped)
+		{
+			uint32_t nextPhase = oscPhase + osc->phaseInc;
+			if(sampleSize < 32768u) {
+				uint32_t loopPhase = sampleSize << 17;
+				if(nextPhase >= loopPhase)
+					nextPhase %= loopPhase;
+			}
+			osc->phase = nextPhase;
+		}
+		else if(itg < info.size)
 		{
 			osc->phase = oscPhase + osc->phaseInc;
 		}
```

---

### 2. Unified `/SAMPLES` and `/LOOPS` Import

**Target Files**: `mainboard/LxrStm32/src/Hardware/SD/filesystem.c` and `SampleMemory.c`

**Why it needs to change**: The user expects a single modal UI action to sequentially sweep both the `/samples` and `/loops` directories on the SD card, appending both into flash and tagging loops with a bit-flag.

**Proposed additions**:

```c
/*
 * Reads a directory for .WAV files and sequentially installs them to flash.
 * @param folder: The name of the directory to scan (e.g., "samples" or "loops").
 * @param append: If 0, erases flash and starts at index 0. If 1, appends to existing samples.
 * @param looped: If 1, applies the SAMPLE_INFO_LOOP_FLAG to the SampleInfo metadata cache.
 * @return 1 on success, 0 on error.
 */
static uint8_t filesystem_installSampleFolderBlocking(const char *folder, uint8_t append, uint8_t looped) {
    // ... directory parsing, size checking, and sampleMemory_installOneSample loops
}

/*
 * Sweeps the SD card for both /SAMPLES and /LOOPS, installing them in one pass.
 * Samples are placed first, followed by loops appended to the end.
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

### 3. Sample Name in Single-Parameter View

**Target Files**: `front/LxrAvr/Menu/menu.c` and `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

**Why it needs to change**: When a user highlights a user sample with the encoder and presses it, they should see an 8-character display name parsed from the FAT filename. The AVR handles the display, but only the STM32 has access to the SD card and `SampleMemory` metadata.

**Proposed change** in `menu.c` near `DTYPE_MENU` (~line 1220):

```diff
             case DTYPE_MENU: {
                 uint8_t menuId = (uint8_t)(parameter_dtypes[parNr] >> 4);
+                if (menuId == MENU_WAVEFORM && curParmVal >= OSC_SAMPLE_START) {
+                    // AVR requests name over UART. Uses cached string once received.
+                    avrComms_requestSampleName(curParmVal - OSC_SAMPLE_START);
+                    memcpy(&editDisplayBuffer[1][0], avrComms_getSampleNameCache(), 8);
+                }
                 getMenuItemNameForValue(menuId, curParmVal, &editDisplayBuffer[1][13]);
                 break; }
```

---

### 4. Waveform Interpolation and Global Setting *(Stretch Goal)*

**Target Files**: `front/LxrAvr/Menu/menuPages.h`, `mainboard/LxrStm32/src/Preset/ParameterArray.h`, `modulationNode.c`, `Oscillator.c`

**Why it needs to change**: As in `LXR02Open`, there needs to be a global toggle for waveform interpolation to save DSP resources when not needed.

**`ParameterArray.h`**:

```diff
     PAR_FETCH,
     PAR_FOLLOW,
+    PAR_OSC_WAVE_INTERP,
```

**`menuPages.h`** — add second row to global settings page:

```diff
   TEXT_BPM,   TEXT_QUANTISATION, TEXT_MIDI_CHAN_GLOBAL, TEXT_MIDI_FILT_TX, TEXT_MIDI_FILT_RX, TEXT_MIDI_ROUTING, TEXT_FETCH, TEXT_FOLLOW,
   PAR_BPM,    PAR_QUANTISATION,  PAR_MIDI_CHAN_GLOBAL,  PAR_MIDI_FILT_TX,  PAR_MIDI_FILT_RX,  PAR_MIDI_ROUTING,  PAR_FETCH,  PAR_FOLLOW,
+  TEXT_OSC_INTERP, TEXT_EMPTY, TEXT_EMPTY, TEXT_EMPTY, TEXT_EMPTY, TEXT_EMPTY, TEXT_EMPTY, TEXT_EMPTY,
+  PAR_OSC_WAVE_INTERP, PAR_NONE, PAR_NONE, PAR_NONE, PAR_NONE, PAR_NONE, PAR_NONE, PAR_NONE,
```

**`modulationNode.c`** — new interpolation state and `modNode_updateValue`:

```c
void modNode_updateValue(ModulationNode* vm, float val) {
    case TYPE_UINT8:
        if (modNode_waveInterpEnabled) {
            OscInfo *osc = modNode_getWaveTargetOsc(vm->destination);
            if (osc && modNode_waveInterpActiveCount < OSC_WAVE_INTERP_MAX_ACTIVE) {
                // Split modulated float into base index and fraction,
                // set osc->waveInterpNext and osc->waveInterpFrac.
                // Increment modNode_waveInterpActiveCount.
            }
        }
}
```

**`Oscillator.c`** — consume interpolation slot in `calcSampleOscBlock`:

```c
static inline uint8_t osc_waveInterpActive(const OscInfo* osc) {
    return (osc->waveInterpGeneration == modNode_getWaveInterpGeneration());
}

void calcSampleOscBlock(...) {
    if (osc_waveInterpActive(osc)) {
        int16_t nextOut = sampleData_nextWave[itg];
        oscOut = oscOut + osc->waveInterpFrac * (nextOut - oscOut);
    }
}
```

---

## Agent Planning

The agent launched 4 parallel research subagents to verify the plan against the codebase:

1. **Oscillator code researcher** — phase handling, loop protection, interpolation
2. **Filesystem & SampleMemory researcher** — sample import flow, loops support
3. **Flash storage implementation researcher** — STM32F4 vs STM32F7 and safety guards
4. **Front panel menu researcher** — menu structure, UART protocol, parameter arrays

---

## Research Findings

### Subagent 1 — Oscillator Code

**File**: `mainboard/LxrStm32/src/DSPAudio/Oscillator.c` (1163 lines)

#### `calcUserSampleOscBlock` (~line 630)

Current code (split-processor):

```c
void calcUserSampleOscBlock(...)
{
    const uint32_t oscPhase = osc->phase;
    const uint32_t itg = oscPhase >> 17;

    // one shot
    if(itg < info.size)
    {
        osc->phase = oscPhase + osc->phaseInc;
    }
    else
    {
        osc->phase = 0;
        // ... mute logic
    }
}
```

**Key findings**:
- **No `looped` variable exists** — the plan's `if(looped)` branch references something not yet defined; it needs to come from a `looped` field on `SampleInfo` (mirroring `LXR02Open`'s `info.looped`)
- For looped waveforms, the modulo wrap currently uses `osc->phase %= (info.size << 17)` unconditionally; for large samples (`>32768`), `info.size << 17` overflows a 32-bit value, causing the wrap bug the plan describes
- `sampleSize` is also not a currently-scoped local variable inside the function — the plan needs to clarify where it comes from

#### `calcSampleOscBlock` (~line 470–600)

- Handles built-in waveform samples
- No interpolation support
- No `waveInterpFrac`, `waveInterpNext`, or `waveInterpGeneration` fields exist anywhere in the split-processor codebase

#### LXR02Open comparison (`LXR02Open/mainboard/firmware/DrumSynth_FPU/src/DSPAudio/Oscillator.c`, 1475 lines)

- Has `info.looped` field on `SampleInfo` — gives the plan's `looped` its source
- Phase wrap logic matches the plan exactly: `if(sampleSize < 32768u) { loopPhase = sampleSize << 17; if(nextPhase >= loopPhase) nextPhase %= loopPhase; }`
- Has `osc->waveInterpFrac`, `osc->waveInterpNext`, `osc->waveInterpGeneration`
- Has `osc_waveInterpActive()` and `modNode_getWaveInterpGeneration()` — both absent in split-processor

---

### Subagent 2 — Filesystem & SampleMemory

**File**: `mainboard/LxrStm32/src/Hardware/SD/filesystem.c` (~850 lines)

#### Current sample installation flow (split-processor)

- `filesystem_installSamplesBlocking()` scans `/samples` via FatFs (`f_opendir` / `f_readdir`)
- Validates 16-bit mono PCM WAV files; copies PCM via `sampleMemory_installOneSample()`
- Calls `sampleMemory_eraseAll()` at the start — **no append mode**
- **No `/loops` directory scanning exists**
- **No `filesystem_installSampleFolderBlocking` helper exists**
- **No `filesystem_installAllSamplesAndLoopsBlocking` exists**

#### `SampleInfo` struct (split-processor)

```c
typedef struct {
    uint32_t startAddr;   // Flash address where sample PCM starts
    uint32_t size;        // Number of 16-bit samples
    // ... possibly other fields
} SampleInfo;
```

- **No `looped` field** in `SampleInfo`
- **`SAMPLE_INFO_LOOP_FLAG` is not defined anywhere**

#### LXR02Open equivalents

- `filesystem_installSampleFolderBlocking(const char *folder, uint8_t append, uint8_t looped)` exists exactly as the plan proposes
- `filesystem_installAllSamplesAndLoopsBlocking()` calls it twice
- `SAMPLE_INFO_LOOP_FLAG` is defined and used in `SampleMemory.h`

> **Gap**: The plan does not specify that `SAMPLE_INFO_LOOP_FLAG` needs to be **defined** in `SampleMemory.h`, nor the bit-packing strategy for the `size` field.

**Sample storage**: Both designs use **external SPI NOR flash** — not internal STM32 flash.

---

### Subagent 3 — Flash Storage Implementation

#### Architecture overview

| | Split-processor (STM32F4) | LXR02Open (STM32F7) |
|---|---|---|
| Sample storage | External SPI NOR — **SST25VF032B** (4 MB) or W25Q64 (8 MB) | External SPI NOR — **W25Q128JV** (16 MB) |
| Internal flash | STM32F407, 1 MB | STM32F746, 1 MB |
| SPI driver | `src/Hardware/SPI_flash/spiFlash.c` | Same path under LXR02Open |

#### SPI flash driver comparison

**Split-processor (STM32F4) — SST25VF032B**:
- Uses **AAI (Auto Address Increment)** word programming — 2 bytes at a time
- WREN command not always sent before erase/program — **potential bug**
- No error checking of status register after write operations
- Block Protection (BP) mechanism not managed — could inadvertently lock sectors
- No write verification (read-back check)
- No page/sector boundary checking
- DMA for reads only

**LXR02Open (STM32F7) — W25Q128JV**:
- Supports **page programming** (up to 256 bytes at once)
- **Explicitly sends WREN** before every erase and program operation
- Handles page boundary splits (256-byte pages)
- Has `spiFlash_writeEnable()` called explicitly
- Still no error flag checking or read-back verification
- DMA for both reads and writes

#### Internal flash comparison

| | STM32F407 | STM32F746 |
|---|---|---|
| Total | 1 MB (some variants 2 MB) | 1 MB |
| Sectors | 4×16 KB, 1×64 KB, 7×128 KB | 4×32 KB, 1×128 KB, 3×256 KB |
| Architecture | Single-bank (CPU stalls during flash ops) | Dual-bank option (read-while-write possible) |
| Write granularity at 3.3 V | 32-bit word | Byte minimum (PSIZE configurable) |
| Error bits (FLASH_SR) | PGSERR, PGPERR, PGAERR, WRPERR | Similar + additional |
| Cache | — | ART Accelerator — **must invalidate after programming** |

#### Additional guards required for STM32F4 SPI flash writing

Guards present in LXR02Open but **missing** in the split-processor:

1. **WREN enforcement** — Send Write Enable before every erase and program command. The SST25VF032B requires it; the F4 driver doesn't always send it.

2. **AAI sequence integrity** — Properly enter and exit AAI mode; send WRDI (Write Disable) after AAI completion. The F4 driver doesn't manage this in all code paths.

3. **Block Protection clearing** — Send WRSR (Write Status Register) to clear BP bits before bulk erase/program; restore them after.

4. **Status register validation** — After each operation, check for write-protect errors and programming failures.

5. **Address range validation** — Bounds-check all write addresses against `SAMPLE_FLASH_START` and `SAMPLE_FLASH_END` constants.

6. **Sector erase verification** — Read back erased sector to verify all `0xFF` before programming.

7. **Write verification** — Read-back after critical writes (sample info table at minimum).

8. **Watchdog feeding** — Long erase/program sequences must pet the watchdog to prevent reset.

9. **Interrupt safety** — Disable audio interrupts during flash operations to prevent SPI bus contention (audio codec and flash may share SPI peripheral or GPIO pins).

10. **Power-loss resilience** — Write the sample info table **last**, after all PCM data is committed, so a power loss mid-import doesn't leave corrupt metadata pointing to incomplete PCM data.

---

### Subagent 4 — Front Panel Menu

#### `menu.c` — DTYPE_MENU (~line 1220)

Current code:

```c
case DTYPE_MENU: {
    uint8_t menuId = (uint8_t)(parameter_dtypes[parNr] >> 4);
    getMenuItemNameForValue(menuId, curParmVal, &editDisplayBuffer[1][13]);
    break;
}
```

**Key observations**:
- No sample name display logic
- No `avrComms_requestSampleName()` exists in the AVR codebase
- No `avrComms_getSampleNameCache()` exists
- `MENU_WAVEFORM` constant exists
- `OSC_SAMPLE_START` does **not** exist in AVR code — user sample waveform IDs start at a hardcoded offset
- User sample slots currently display as numbers only

#### `menuPages.h` — global settings page

Current:

```c
TEXT_BPM, TEXT_QUANTISATION, TEXT_MIDI_CHAN_GLOBAL, TEXT_MIDI_FILT_TX, TEXT_MIDI_FILT_RX, TEXT_MIDI_ROUTING, TEXT_FETCH, TEXT_FOLLOW,
PAR_BPM,  PAR_QUANTISATION,  PAR_MIDI_CHAN_GLOBAL,  PAR_MIDI_FILT_TX,  PAR_MIDI_FILT_RX,  PAR_MIDI_ROUTING,  PAR_FETCH,  PAR_FOLLOW,
```

The plan's diff is structurally correct — it adds a second 8-parameter sub-page row.

#### `ParameterArray.h`

Current:

```c
enum {
    // ...
    PAR_FETCH,
    PAR_FOLLOW,
    NUM_PARAMS
};
```

**`PAR_OSC_WAVE_INTERP` does not exist**.

> ⚠️ **Warning**: Inserting `PAR_OSC_WAVE_INTERP` after `PAR_FOLLOW` changes `NUM_PARAMS` and shifts the enum value of any parameter that follows. This **breaks preset compatibility** if any parameter is stored by index. Verify that `PAR_FOLLOW` is the last parameter before `NUM_PARAMS`.

#### `frontPanelReceivingProtocol.c`

- No sample name query/response protocol exists
- The plan provides zero protocol detail for the new UART message type — no command byte, no response format, no encoding, no async vs. blocking specification
- This is a **significant implementation gap**

#### LXR02Open comparison

- LXR02Open does not need a UART protocol for sample names — it's single-processor; names come directly from `sampleMemory_getSampleName(index)`
- The split-processor design must **invent** a new UART message type — the plan acknowledges this but provides no specification

#### `modulationNode.c`

All of the following are absent from the split-processor codebase:
- `modNode_waveInterpEnabled`
- `modNode_waveInterpActiveCount`
- `modNode_getWaveTargetOsc()`
- `modNode_getWaveInterpGeneration()`

Section 4 of the plan is **entirely new functionality** with no existing hooks to port from.

---

## Session Outcome

All four research subagents were terminated mid-execution by **HTTP 429 RESOURCE_EXHAUSTED** errors (individual quota reached; resets in ~165 hours). The partial reports captured above were returned before the quota was hit. No final synthesised audit report was produced by the agent before the session ended.

---

## Open Items / Implementation Gaps

| # | Area | Gap |
|---|---|---|
| 1 | `Oscillator.c` | `looped` variable source unspecified — must come from a new `looped` field on `SampleInfo` |
| 2 | `Oscillator.c` | `sampleSize` local variable not defined in current `calcUserSampleOscBlock` scope |
| 3 | `SampleMemory.h` | `SAMPLE_INFO_LOOP_FLAG` not defined; bit-packing strategy for `size` field unspecified |
| 4 | `SampleMemory.h` | `looped` field missing from `SampleInfo` struct — must be added |
| 5 | `SPI flash / STM32F4` | 10 safety guards missing (WREN, AAI, BP, error check, bounds, verify, watchdog, IRQ, power-loss resilience) |
| 6 | `frontPanelReceivingProtocol.c` | New UART message type for sample name request/response completely unspecified |
| 7 | `menu.c` | `OSC_SAMPLE_START` constant missing from AVR codebase — needs defining |
| 8 | `ParameterArray.h` | Inserting `PAR_OSC_WAVE_INTERP` risks breaking preset compatibility — verify it is safe |
| 9 | `modulationNode.c` | All interpolation infrastructure is new — no LXR02Open UART equivalent to port from directly |
