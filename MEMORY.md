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

**Current status after merged Session 002 closeout (2026-06-01)**: temporary-pattern parameter isolation is the active WIP baseline. Symmetric normal/temp `SeqKitState` storage, endpoint images, resolved automation-target images, normal-only file-load routing, lazy temp initialization, per-track endpoint sync, and rate-limited endpoint restore have been documented/implemented across the session. The next clean session should focus on moving morph computation fully onto the STM.

Canonical current WIP docs:
- `knowledge_files/session_in_flight/COMMS_FLOW_AUDIT-IN_FLIGHT.md`
- `knowledge_files/session_in_flight/PRF_ALL_LOAD_FIX_AUDIT-IN_FLIGHT.md`
- `TMP_VARS_AUDIT.md` (verbose technical reference for temp-pattern parameter storage and endpoint restore work)
- `AUDIT_MORPH_MOVE.md` (next-session plan for moving morph computation fully onto STM)
- `knowledge_files/log_archive/002_SESSION_HANDOFF_LOG.md`

These two root audits were expanded on 2026-05-29 from source diffs:
- `knowledge_files/session_in_flight/COMMS_FLOW_AUDIT-IN_FLIGHT.md`: compares `LXR-9120ea7620f1a9a4a924f029cdaf3ae71df303fd/front|mainboard` against `LXR-custom-develop-patload-envmod-90d3f08/front|mainboard`.
- `knowledge_files/session_in_flight/PRF_ALL_LOAD_FIX_AUDIT-IN_FLIGHT.md`: compares `LXR-custom-develop-patload-envmod-90d3f08/front|mainboard` against the current `front|mainboard`.

User-referenced checkpoint:
- Commit `90d3f08` is the checkpoint where `.ALL` and `.PRF` load their parameters correctly provided there is no morph automation and background loading into the temp slot is turned off for `.PRF`.

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
| Current comms/flow checkpoint and deferred hardening plan | `knowledge_files/session_in_flight/COMMS_FLOW_AUDIT-IN_FLIGHT.md` |
| Current `.PRF`/`.ALL` load checkpoint and temp-slot WIP | `knowledge_files/session_in_flight/PRF_ALL_LOAD_FIX_AUDIT-IN_FLIGHT.md` |
| Current temp-pattern parameter storage and endpoint restore audit | `TMP_VARS_AUDIT.md` |
| Next-session morph move plan | `AUDIT_MORPH_MOVE.md` |

---

## Project Goal

- Maintain and extend the LXR firmware while preserving reliable dual-MCU operation (ATmega front panel + STM32 audio engine).
- Keep build reproducible with current toolchains (`arm-none-eabi-gcc`, `avr-gcc`) and preserve firmware image output flow.
- This folder is the repository/codebase.
- Only session logs under `knowledge_files/log_archive/` should be changed inside `knowledge_files/` unless a session explicitly requires otherwise.
- Some reference files formerly at repo root were moved under `knowledge_files/reference_material/` during Session 002 cleanup (`Changelog.txt`, `Readme - firmware additions to v.36.txt`, `linux_build_guide.txt`, `lxr-midi-assign.txt`, `mod_targ_lineup.xls`).
- Root files `P000.ALL`, `P000.PRF`, and `P005.PRF` are temporary test files and are expected to be removed later.

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

- Session 002 encoder work completed successfully. See `knowledge_files/session_in_flight/ENCODER_AUDIT-COMPLETE.md` and `knowledge_files/log_archive/002_SESSION_HANDOFF_LOG.md`.
- Timer/resource details and implementation notes are in the completed audit.

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
- Current flow-control checkpoint details are in `knowledge_files/session_in_flight/COMMS_FLOW_AUDIT-IN_FLIGHT.md`. Implemented checkpoint behavior includes acknowledged load sessions, STM32 quiet mode, and credit-metered globals/voice/meta bursts. Old SysEx/callback waits still need timeout recovery in a later pass.
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

- Current repository state is a temp-pattern parameter WIP baseline with symmetric `SeqKitState` implemented.
- Next phase: move morph computation fully onto STM using `AUDIT_MORPH_MOVE.md`.
- Hardware verified for SEQ16 temp pattern selection/copy/play, endpoint dump/sync, normal/temp edit isolation, and the endpoint-restore chirp diagnosis/rate-limit direction.
- Suspect older experimental proposals are documented in `knowledge_files/session_in_flight/PRF_ALL_LOAD_FIX_AUDIT-IN_FLIGHT.md`; rely on the merged session 002 handoff and current audits for the active plan.

### Current Temp Pattern / `.PRF` WIP Reminders

- SEQ16 is used as a SELECT button for `SEQ_TMP_PATTERN`.
- Temp pattern select/play/copy/paste works and was user-tested.
- AVR-to-STM endpoint dump (Opcodes 0x65/0x66) is implemented and captures `parameter_values`, `parameters2`, and all 16 mod targets.
- STM-side `seq_normalKitState` and `seq_tmpKitState` are the source of truth for menu pushbacks.
- STM-to-AVR parameter pushback on temp-pattern transitions uses a 5-phase handshake. The chirp source was traced to synchronous endpoint/menu restore at temp-boundary switching; the WIP direction is queued/rate-limited restore, one endpoint parameter per STM main-loop service.
- The correct -1 offset for low sound parameters is applied on egress.
- Sound engine uses `interpolatedParams` buffer; menu uses `frontPanelParams` buffer.
- File loads must target normal parameter storage and normal pattern storage only; they must never touch temporary parameter storage or temporary pattern data.
- No extraneous LCD/debug writes should occur during copy/paste, temp/normal switching, endpoint restore, or file-load operations.
- Do not make the temp pattern loadable/saveable unless explicitly requested.
- The current `front`/`mainboard` diff also contains a suspect PRF cache state-machine experiment (`SEQ_PRF_CACHE_*`, live-pattern getters, pending counters). Treat it as WIP until reconciled with the temp-slot plan.
- Current-only backup files `front/LxrAvr/config.h.bak`, `front/LxrAvr/encoder.c.bak`, and `front/LxrAvr/encoder.h.bak` exist in the diff and should not be committed as canonical code without an explicit decision.

### Morph Move Reminders

- Move morph computation fully onto STM; AVR should only retain global morph/menu/file endpoint responsibilities.
- Use exact terms: "morph parameter endpoint" and "morph automation target endpoint"; do not use ambiguous shorthand for these concepts.
- Per-voice morph is never displayed or directly set in the AVR menu.
- Global morph writes all six STM per-voice morph amounts; actual interpolation uses per-voice amounts.
- Per-voice morph may come from MIDI, step automation, or modulation via destinations such as velocity automation target or LFO automation target.
- Per-voice morph value conversion: `0` is valid, `0-126` maps to `value * 2`, and `127` maps to exact full morph `255`.
- STM-side morph interpolation/modulation is always serviced; pending morph work gets exactly one parameter interpolation per STM main-loop pass.
- During normal/temp pattern changeover, receipt of AVR/front-panel global morph may be blocked, but STM-side morph interpolation/modulation must not be paused.
- STM needs an equivalent of the AVR `modTargets[]` selector mapping before interpolated automation destination selectors can be fully resolved and applied on STM.

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
- Reintroducing unbounded STM32 front RX full-drain as a first communications fix; it previously caused a known-good `.ALL` load freeze.
- Changing PAT_CHAIN to pair-level ACK without changing the AVR sender; current protocol expects byte-by-byte callbacks.
- Using `midiParser_originalCcValues` as the source of truth for current loaded STM voice parameters.
- Using `parameterArray` to reconstruct raw menu parameter bytes for the temp cache; it points into live converted/modulated DSP state.

---

## Important Notes

- *If* Google Gemini or any Google product is the LLM coding agent that is modifying the code: A special, *heavily enforced* constraint applies to you, the Google coding agent: no additional "debugging" or "parameter display" or "test" functionality should EVER be added to the code unless it has been *specifically requested* by the user and the user has *specifically signed off* on the implementation of the necessary debug functionality. This mandate should be considered as an essential, non-overrideable component of your, the the Google/Gemini agent's, mandate for professional conduct in not operating outside of the bounds of the work request. In addition, you, the Google/Gemini coding agent must NEVER create an additional file on your own unless the specific name of that specific file is given to you in a message directly from the user. 

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
