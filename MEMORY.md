# LXR -bc- Enhanced Firmware — Project Context

This file is the working memory for Codex/Claude Code/LLM Agent sessions on this project.
Read it fully at the start of every session before touching any code.
Update it whenever something is confirmed, fixed, or decided.

---

## Quick Start

```bash
# Repository root (confirmed)
cd /Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod

# Build
make clean
make firmware

# Build outputs
# - mainboard/LxrStm32/LxrStm32.bin
# - front/LxrAvr/LxrAvr.bin
# - firmware image/FIRMWARE.BIN
```

**Current working source**: repository root, branch `custom-develop-patload-envmod`.

**Session 001 close status**: full top-level build succeeds in this repo with `make clean && make firmware` (warnings remain, no fatal errors).

---

## Repository Layout

```text
/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod
├── .git/
├── BUILD_AUDIT.md
├── Changelog.txt
├── LICENSE.txt
├── MEMORY.md
├── MEMORY_example.md
├── Makefile
├── README.md
├── Readme - firmware additions to v.36.txt
├── firmware image/
│   └── FIRMWARE.BIN
├── front/
│   ├── LxrAvr/
│   │   ├── Hardware/
│   │   │   └── SD/
│   │   ├── IO/
│   │   ├── Menu/
│   │   ├── Preset/
│   │   ├── build/
│   │   ├── Makefile
│   │   ├── LxrAvr.bin
│   │   ├── LxrAvr.map
│   │   └── README.md
│   └── LxrAvr_bootloader/
│       ├── Bootloader/
│       ├── default/
│       ├── elmChan/
│       ├── lcd/
│       ├── README.md
│       ├── SD_main.c
│       ├── SD_routines.c
│       ├── SPI_routines.c
│       ├── UART_routines.c
│       └── userInterface.c
├── knowledge_files/
│   ├── SESSION_HANDOFF_TEMPLATE.md
│   ├── hardware_archive/
│   │   ├── front/
│   │   ├── main/
│   │   └── ATMEGA_STM32F4_COMMS_AUDIT.md
│   └── log_archive/
│       ├── 000_SESSION_INDEX.md
│       └── 001_SESSION_HANDOFF_LOG.md
├── linux_build_guide.txt
├── lxr-midi-assign.txt
├── mainboard/
│   ├── LxrStm32/
│   │   ├── Libraries/
│   │   │   ├── CMSIS/
│   │   │   │   └── Include/
│   │   │   ├── Device/
│   │   │   │   └── STM32F4xx/
│   │   │   │       └── Include/
│   │   │   ├── STM32F4xx_StdPeriph_Driver/
│   │   │   │   ├── inc/
│   │   │   │   └── src/
│   │   │   ├── STM32_USB_Device_Library/
│   │   │   │   └── Core/
│   │   │   │       ├── inc/
│   │   │   │       └── src/
│   │   │   └── STM32_USB_OTG_Driver/
│   │   │       ├── inc/
│   │   │       └── src/
│   │   ├── build/
│   │   ├── src/
│   │   │   ├── AudioCodecManager/
│   │   │   ├── DSPAudio/
│   │   │   ├── Hardware/
│   │   │   │   ├── SD_FAT/
│   │   │   │   └── USB/
│   │   │   ├── MIDI/
│   │   │   ├── SampleRom/
│   │   │   └── Sequencer/
│   │   ├── Makefile
│   │   ├── LxrStm32.bin
│   │   └── stm32_flash.ld
│   └── LxrStm32_bootloader/
│       ├── Libraries/
│       │   ├── CMSIS/
│       │   │   └── Include/
│       │   ├── Device/
│       │   │   └── STM32F4xx/
│       │   │       └── Include/
│       │   ├── STM32F4-Discovery/
│       │   └── STM32F4xx_StdPeriph_Driver/
│       │       ├── inc/
│       │       └── src/
│       ├── Release/
│       │   ├── Libraries/
│       │   │   ├── STM32F4-Discovery/
│       │   │   └── STM32F4xx_StdPeriph_Driver/
│       │   │       └── src/
│       │   └── src/
│       ├── src/
│       └── stm32_flash.ld
├── mod_targ_lineup.xls
├── requirements.txt
└── tools/
    ├── FirmwareImageBuilder/
    │   ├── FirmwareImageBuilder/
    │   ├── Release/
    │   ├── Makefile
    │   └── FirmwareImageBuilder.sln
    └── bin/
        ├── FirmwareImageBuilder
        ├── FirmwareImageBuilder.exe
        └── makeFirmware.bat
```

### Where to look for things

| Question | File |
|----------|------|
| Which session introduced a fix? | `knowledge_files/log_archive/000_SESSION_INDEX.md` |
| Full details of a fix or decision? | `knowledge_files/log_archive/00x_SESSION_HANDOFF_LOG.md` |
| Confirmed hardware/protocol notes? | `knowledge_files/hardware_archive/` |
| Current known issues and reminders? | `MEMORY.md` |
| Session 001 build triage details | `BUILD_AUDIT.md` |

---

## Project Goal

- Maintain and extend the LXR firmware while preserving reliable dual-MCU operation (ATmega front panel + STM32 audio engine).
- Keep build reproducible with current toolchains (`arm-none-eabi-gcc`, `avr-gcc`) and preserve firmware image output flow.
- This folder is the repository/codebase.
- Only session logs under `knowledge_files/log_archive/` should be changed inside `knowledge_files/` unless a session explicitly requires otherwise.

## General Process Reminders

- Always verify the local working repository directory before writing code.
- Validate error logs against the exact local source snapshot before patching.
- Prefer source-level compatibility fixes over compiler-flag masking.
- Blocking for 1ms anywhere in the main loop or any ISR at priority <= 4 is unacceptable.

---

## Hardware

### MCU: <add details>

- Main audio/control MCU: STM32F407 (`mainboard/LxrStm32`).
- Front-panel/UI MCU: ATmega644 (`front/LxrAvr`).
- Inter-MCU link: UART-based protocol at 500000 baud.
- Front panel handles LCD, encoders, buttons, LEDs, SD card preset I/O; STM32 handles DSP/sequencer/MIDI/USB.

### Flash Sector Layout (single-bank, confirmed via memtest)

| Sector | Range | Size | Use |
|--------|-------|------|-----|
| <to add later> | <to add later> | <to add later> | <to add later> |

### Confirmed GPIO

| Peripheral | Pins | Notes |
|-----------|------|-------|
| STM32 <-> AVR UART | <to add from hardware audit/code grep> | Known to run 500000 baud in current firmware |
| OUT jack detect mapping | PD6/PD7/PB4/PB6 | Carried forward from prior notes; re-validate in this branch if touched |

### IRQ Assignments

| IRQ | Priority | Handler | Function |
|-----|----------|---------|----------|
| <to add later> | <to add later> | <to add later> | <to add later> |

---

## Audio Pipeline <add later>

**Main loop pattern:**
```c
/* TODO: capture from mainboard/LxrStm32/src/main.c in a dedicated audit session */
```

**OUTPUT_DMA_SIZE = xx** <to be audited later>

**DSP render must remain in the main loop.**

---

## SD Card Architecture

Current high-level architecture (from source tree):
- STM32 side: `mainboard/LxrStm32/src/Hardware/SD_FAT/`
- AVR side: `front/LxrAvr/Hardware/SD/`
- Shared FAT-style flow with board-local SPI/SD routines.

**Architecture:**
```text
ATmega644 UI/Preset manager <-> SD (front/LxrAvr/Hardware/SD)
STM32 sample/preset-related storage services <-> SD_FAT (mainboard/LxrStm32/src/Hardware/SD_FAT)
```

**Key design points:**
- `startBlock`, `totalBlocks`, `SDHC_flag`, `cardType` are global SD state symbols and must remain single-defined (`extern` in headers, one C definition).
- SD layers are build-critical on both MCUs; header ownership mistakes cause cross-TU link failures.

**ISR migration path** <to be audited>

**Approaches confirmed wrong** (do not retry):
- Defining SD state globals directly in headers included by multiple C files.

---

## Sample Flash Loading

<to audit another session>

Sample flash map:

| Region | Range |
|--------|-------|
| <to be completed> | <to be completed> |

---

## Encoder

- **Algorithm**: <to audit>
- **Seed**: <to audit>
- **Divide**: <to audit>
- **Acceleration**: <to audit>
- **Rebound suppression**: <to audit>

---

## Display / Menu

- **LCD queue**: <to audit>
- **Knob repaint**: <to audit>

---

## DSP / RNG Rules

- **Compiler**: modern GCC toolchains expose legacy `inline` linkage pitfalls.
- **FPU**: STM32F4 hardware FPU present; exact compile flag set should be captured in a later dedicated toolchain audit.
- **VLAs**: <to audit usage policy>
- `bufferTool_*` and `GetRngValue` must keep consistent declaration/definition linkage across headers and C files.
- `freq2PhaseIncr` currently uses `static inline` in header in this branch.

---

## Boot / Init Order (do not reorder)

```c
/* TODO: capture explicit init order from mainboard/LxrStm32/src/main.c */
```

### Sequencer / PATGEN Reminders

- <to add>

### MIDI / Clock Reminders

- Inter-MCU UART protocol uses MIDI-like status/data framing and sysex-like bulk transfer behavior.
- Communications audit warns about silent FIFO overflow and blocking ACK wait patterns; review `knowledge_files/hardware_archive/ATMEGA_STM32F4_COMMS_AUDIT.md` before touching parser/transport logic.
- If build errors reference functions/line numbers missing in local files, treat as snapshot mismatch and confirm active repository first.

---

### Morph / Endless-Pot Reminders

- <to add>

---

## Known Issues / Technical Debt

### Resolved in Session xxx

- **Session 001 resolved**:
- Header-defined globals causing multiple definitions (`MidiMessages.h`, `MidiVoiceControl.h`, SD headers).
- Legacy inline linkage breakage causing undefined references (`BufferTools`, `GetRngValue`).
- Full build now completes to `firmware image/FIRMWARE.BIN` in this repo.

### High Priority

- Hardware smoke test pending for current successful build outputs.
- Warning triage needed (compiler diagnostics are currently noisy and can hide regressions).

### Medium Priority

- Capture definitive boot/init order and IRQ priority table in this file.
- Expand directory and subsystem references with owner/entrypoint notes.

### Lower Priority

- Normalize older warning patterns where safe (fallthrough annotations, const duplicates, legacy macro style).

---

## Failed Approaches — Do Not Retry

- Applying patches from an `err.txt` log without confirming the log matches current local source snapshot.
- Working in a similarly named sibling repository without verifying `pwd` and branch first.
- Using compiler workarounds alone (`-fcommon`, `-fgnu89-inline`) as a substitute for real source ownership/linkage fixes.

---

## Toolchain

```text
Required:
- arm-none-eabi-gcc
- avr-gcc (+ avr-libc)
- make
- host C++ compiler (for tools/bin/FirmwareImageBuilder)
```

Notes:
- Reference install guidance: `requirements.txt`.
- Top-level build command: `make firmware`.
- Bootloader flash flow: copy `FIRMWARE.BIN` to SD root, then hold main encoder while powering on.
