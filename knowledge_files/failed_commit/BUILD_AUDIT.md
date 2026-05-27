# BUILD_AUDIT (Session 001)

Date: 2026-05-21

## Scope
- Inputs reviewed:
- `README.md`
- `knowledge_files/` exploratory documents
- `err.txt` build transcript

## Executive Summary
- The build failure is primarily a toolchain-compatibility issue exposed by modern ARM GCC (14.2.1), not a new DSP logic regression.
- Two fatal classes block linking:
- Header-defined globals causing multiple definitions.
- `inline` function linkage behavior causing missing external symbols.
- The most reliable path is to fix source declarations/definitions (recommended), not just relax compiler flags.

## Implementation Status (Session 001)
- Applied in this session:
- Header global-definition fixes (Group A).
- `inline` linkage fixes for `BufferTools` and `GetRngValue` (Group B), plus oscillator consistency cleanup.
- Verification result:
- `mainboard/LxrStm32/LxrStm32.bin` now compiles and links successfully.
- Remaining top-level `make firmware` blocker is environment/tooling:
- `avr-gcc: command not found` while building `front/LxrAvr`.

## Fatal Error Group A: Multiple Definitions

### A1) `ParamEnums` and `Param2Enums` are accidentally global variables
- Evidence:
- `mainboard/LxrStm32/src/MIDI/MidiMessages.h:253` has `}ParamEnums;`
- `mainboard/LxrStm32/src/MIDI/MidiMessages.h:394` has `}Param2Enums;`
- Effect:
- Each translation unit including this header defines real global objects named `ParamEnums` and `Param2Enums`, producing linker collisions.
- Required fix:
- Change both enum endings to plain enum terminators (`};`) or convert to `typedef enum { ... } ParamEnums;` style intentionally.
- Do not leave trailing identifiers that instantiate storage in headers.

### A2) `voiceStatus` is defined in a header
- Evidence:
- `mainboard/LxrStm32/src/MIDI/MidiVoiceControl.h:58` defines `uint8_t voiceStatus[NUM_VOICES];`
- Effect:
- Every C file including this header emits its own `voiceStatus`, causing widespread duplicate symbol errors.
- Required fix:
- If needed globally: make it `extern` in header and define once in `MidiVoiceControl.c`.
- If unused (currently appears unused): remove declaration entirely.

### A3) SD card globals are defined in a header
- Evidence:
- `mainboard/LxrStm32/src/Hardware/SD_FAT/sd_routines.h:65-66` defines:
- `volatile unsigned long startBlock, totalBlocks;`
- `volatile unsigned char SDHC_flag, cardType;`
- Effect:
- Duplicate symbol errors (`SDHC_flag`, `cardType`, `totalBlocks`, `startBlock`) when multiple source files include the header.
- Required fix:
- Change header declarations to `extern`.
- Add a single definition in `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_routines.c`.

## Fatal Error Group B: Undefined References from `inline` Usage

### B1) `bufferTool_*` symbols missing at link time
- Evidence:
- Header declares many `inline` prototypes in `mainboard/LxrStm32/src/DSPAudio/BufferTools.h:43-67`.
- Definitions in `mainboard/LxrStm32/src/DSPAudio/BufferTools.c:40+` are also `inline`.
- Linker reports undefined references (for example `bufferTool_clearBuffer`, `bufferTool_addBuffersSaturating`, `bufferTool_addGain`).
- Root cause:
- Current compiler defaults + C inline rules do not guarantee external symbol emission for this pattern.
- Required fix:
- Use normal external functions:
- Remove `inline` from declarations in `BufferTools.h`.
- Remove `inline` from definitions in `BufferTools.c`.
- Alternative (not recommended here): move full `static inline` definitions into header and remove C-file definitions.

### B2) `GetRngValue` missing at link time
- Evidence:
- Declaration is `__inline uint32_t GetRngValue();` in `mainboard/LxrStm32/src/DSPAudio/random.h:44`.
- Definition is `__inline uint32_t GetRngValue()` in `mainboard/LxrStm32/src/DSPAudio/random.c:49`.
- Linker reports undefined references from `lfo.c`, `Oscillator.c`, `sequencer.c`.
- Required fix:
- Make `GetRngValue` a normal externally linked function:
- Header declaration: `uint32_t GetRngValue(void);`
- Source definition: `uint32_t GetRngValue(void) { ... }`

### B3) Preventive consistency fix for oscillator helper
- Evidence:
- `mainboard/LxrStm32/src/DSPAudio/Oscillator.h:88` declares `__inline uint32_t freq2PhaseIncr(float f);`
- `mainboard/LxrStm32/src/DSPAudio/Oscillator.c:62` defines it as `__inline`.
- Current log shows warning-only for this symbol, but pattern matches failing inline style above.
- Recommended fix:
- Convert to normal function prototype/definition or to `static inline` in header only.

## Non-Blocking Warnings (Post-Link Cleanup)
- `usb_core.c` warns about ignored `packed` attribute on pointer casts (legacy CMSIS/ST code).
- Linker warns ELF has an RWX load segment.
- Many “inline function declared but never defined” warnings should disappear after Group B fixes.
- These are not the immediate blockers for producing `LxrStm32.bin`.

## Recommended Fix Order
1. Fix header global definitions (Group A).
2. Fix inline linkage issues (Group B).
3. Rebuild: `make clean && make firmware`.
4. Confirm STM32 binary links; then check whether AVR and firmware image builder complete.
5. Triage residual warnings separately.

## Temporary Workaround (Not Preferred)
- In `mainboard/LxrStm32/Makefile`, adding flags like `-fcommon` and `-fgnu89-inline` may mask the issues.
- This is useful only for quick bring-up; source-level fixes are the maintainable solution.

## Verification Criteria
- No multiple-definition linker errors.
- No undefined references to `bufferTool_*` or `GetRngValue`.
- `mainboard/LxrStm32/LxrStm32.bin` produced successfully.
- Top-level `make firmware` completes and emits `firmware image/FIRMWARE.BIN`.
