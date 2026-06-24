# Sonic Potions LXR Drumsynth Firmware
## -bc- edition, based on v0.37

The LXR is a digital drum synthesizer based on a 32-bit Cortex-M4 (STM32F407) audio/control processor and an ATmega644 8-bit front-panel CPU. Originally developed by Julian Schmidt / Sonic Potions.

The `front` folder contains the AVR code. The `mainboard` folder contains the STM32F4 code. The `tools` folder contains the firmware image builder, which combines AVR and Cortex code into a single file usable by the bootloader.

### Libraries

The mainboard code uses several third-party libraries, all located in the `Libraries` subfolder with their own licenses:

- ARM CMSIS library
- ST STM32_USB_Device_Library
- ST STM32_USB_OTG_Driver
- ST STM32F4xx_StdPeriph_Driver

### Thanks

Many thanks to Rudeog for bug fixes and features in versions 0.26 and 0.33, to Patrick Dowling and Andrew Shakinovsky for Makefile and code contributions, and to Julian Schmidt for the original LXR design and open-source release.

---

## Building the Firmware

Build requirements are documented in `requirements.txt`. In brief, you need `arm-none-eabi-gcc`, `avr-gcc` (with avr-libc), `make`, and a host C++ compiler for the FirmwareImageBuilder tool. All are available from your platform's standard package manager or from the Arm and Microchip toolchain download pages.

```bash
make clean
make firmware
```

The output is `firmware image/FIRMWARE.BIN`. Copy this to the root of the SD card and power on while holding the main encoder button.

### Linux (quick reference)

```bash
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi gcc-avr binutils-avr avr-libc make build-essential
# On x64 systems you may also need:
sudo apt-get install libc6-dev-i386
```

### macOS (quick reference)

```bash
brew install arm-none-eabi-gcc
brew tap osx-cross/avr && brew install avr-gcc
xcode-select --install   # for make and host C++
```

### Windows (quick reference)

WSL Ubuntu is recommended. Inside WSL:

```bash
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi gcc-avr binutils-avr avr-libc make g++
```

See `requirements.txt` for MSYS2 paths and toolchain override options if your compilers are not on `PATH`.

---

## Additions to Sonic Potions Firmware v0.37

The following features have been added in the -bc- edition on top of the upstream v0.37 release. For full implementation history see `knowledge_files/log_archive/000_SESSION_INDEX.md`.

### General UI

**Morph kit parameter quick access.** While viewing a single parameter in the encoder click-in view, hold SHIFT to toggle between editing the normal parameter value and the morph target value for that parameter. This lets you set morph kit values without save/reload faff.

**Re-align patterns shortcut.** in PERF mode, press the current pattern button again to realign all tracks to the master clock and clear any pattern rotation offsets or misalignment from setting different pattern timescales. 

**Pot (knob) assignments in load/save reworked.** The shortcut knob functions have been revised to speed up character entry when naming files. In the load menu: knob 1 changes load type, knob 2 changes preset number with auto-load for kit and voice types, knob 3 disables auto-load if turned, knob 4 disables auto-load and moves the cursor. In the save menu: knob 1 changes save type, knob 2 moves the cursor between fields, knobs 3 and 4 cycles through characters. Capital letters are at the left of knob 3, numbers are in the middle, lower case is at the left of knob 4.

**SHIFT as toggle.** A global menu option makes the SHIFT button a toggle rather than momentary. This is useful for step editing workflows where you need SHIFT to remain active across multiple button presses.

---

### Sound / Voice Mode

**Individual voice morph.** Each drum voice has an independent morph value that blends between the kit parameters and the morph-target parameters for that voice only. Per-voice morph can be set from the PERF menu, by step automation, or by velocity modulation and LFO (see below). Global morph still operates across all voices and overwrites individual per-voice morph values when received.

**One-shot LFO waveforms.** Additional LFO waveforms are available: `si1`, `tr1`, `sq1`, `rmp1`, `rnd1` etc. are one-shot versions of sine, triangle, square, ramp, and noise. There is also a new `xtr`/`xt1` exponential-triangle shape (exponential rise followed immediately by exponential fall). In one-shot mode, the LFO offset control sets a pre-trigger delay; the delay is scaled with rate. The noise one-shot holds a single random value on each retrigger. The rect one-shot is phase-inverted so it can be immediately on and then off, with the offset delay setting an off portion at the start if desired.

**Disable MIDI input per voice or globally.** MIDI channel select menus now include a `0` option that disables MIDI input for that voice or globally, making it straightforward to isolate voices from external MIDI.

**Multi-voice note trigger on global MIDI channel.** When the active track has a note assignment other than 'any', note-ons received on the global MIDI channel are matched and recorded to all tracks that have a note assignment other than 'any', not just the currently active one. This makes it easier to record multi-voice patterns from an external keyboard on the global channel.

**Per-voice MIDI CC assignments.** Individual voice MIDI channels now have direct CC mappings without needing NRPN, covering all voice parameters. The global channel takes precedence: global CC1 is morph (Mod Wheel), and global CC2-127 route through the global CC table. See `knowledge_files/comms_spec_reference/MIDI_TABLE.md` for the full assignment table.

**Per-voice morph as automation target.** Individual voice morph is available as a velocity modulation target and as an LFO modulation target. LFO-to-voice-morph modulates between the current per-voice morph value (at zero depth) and the morph kit value (at full depth).

---

### PERF Mode

**Roll independent of pattern length.** Roll rate is no longer tied to the current pattern length, allowing consistent roll behavior when switching between patterns of different lengths.

**Rolls recordable in three modes.** Rolls can be recorded as full (pitch and velocity), note (pitch) only, or velocity only, giving more control over how roll content lands in the sequencer.

**Individual voice morph in PERF menu.** Each voice has a morph control directly accessible from the PERF page, displayed and settable in full 0–255 range.

**Track transpose.** A track transpose function is available as a SHIFT function on the PERF menu.

**Instant pattern switching.** A global menu option makes pattern switching (via buttons or program change MIDI) happen at the next sub-step rather than at the end of the bar, keeping the sequencer position. The default behavior (end-of-bar switching) is preserved when the option is off.

**Per-track pattern assignment.** Individual tracks can be set to follow different patterns: hold a VOICE button and press a pattern button to assign that track to a different pattern source independently of the other tracks.

**Looper.** SEQ buttons 8–16 provide looper functionality in PERF mode.

---

### Sequencer / Step Mode

**Copy step, copy sub-step, copy single-voice track between patterns.** Steps and sub-steps can be copied individually. To copy a single voice track between patterns: view the source pattern, hold COPY, press the source track (voice) button, then press the destination pattern button.

**Voice load from kit via step automation.** Individual drum voices can be changed by step automation targeting a Bank MSB (CC0) value, letting you swap a single drum sound mid-pattern from within the sequencer without a full kit change.

**Automation-only steps (velocity 0).** When a step's velocity is set to 0, it does not retrigger the voice envelope but still plays back any automation recorded on that step. This works like a 'trigless lock' — useful for parameter movement without a new hit.

**Track pattern step timing scale.** Pattern scaling per track is accessible via a second page under the 'click' (transient voicing) sub-page. This lets you run a track at a different rhythmic subdivision from the rest of the pattern.

**Per-voice morph settable by step automation.** Individual voice morph amounts can be written directly into step automation slots, allowing per-step morph movements on individual voices.

---

### File / Load-Save Mode

**Version number for kit files.** Kit files now carry a version number. The .prf and .all file format has been incremented to version 3 to include morph target parameters. Previous versions load with an empty morph target and are re-saved as the new type.

**Reload kit from snapshot.** Pressing SHIFT+PLAY reverts all voice parameters to their state at the last file load, discarding any live edits made since. This is a quick way to recover the saved kit without going through the load menu.

**Load a kit by MIDI bank change.** Sending a Bank MSB (CC0) message on the global MIDI channel loads the corresponding .kit file. Individual drum voices can also be changed by sending Bank MSB on their individual MIDI channels.

**Load a performance file by MIDI bank change.** A global menu option switches Bank MSB (CC0) messages on the global channel from loading .kit files to loading .prf (performance) files, which include the full drum kit, pattern set, BPM, and morph target.

**.prf and .all save morph kit.** The performance and 'all' save types now include morph target parameters, preserving the full morph state across saves and loads.

**Load individual drum voices from kit files.** The Load menu now includes entries to load individual drum voices from .kit files without replacing the full drumkit. The name and number shown for a loaded voice reflect the kit it was derived from, even if the voice was subsequently changed by a MIDI bank change.

**Background file loading.** When loading a file, the currently playing pattern and parameters are held in a temporary slot while the load executes in the background. The loaded sound becomes active when the next new pattern is selected. A global menu option controls which file types use background loading.

---

## Developer Notes

### Structure

A few key changes have been made to the way the code and ownership in the LXR work from the 0.37 release:

**The Canonical Preset Parameters are stored on the STM.** Previously, the STM stored only the interpolated parameters calculated by and sourced from the AVR. This created an exceedingly large amount of parameter and morph traffic in some cases and could crash the AVR. The amount of storage needed on the STM for this is negligible. The routines to manage preset storage and morph are in a new 'preset' folder in 'mainboard'. 

**The Morph Interpolation is performed on the STM.** The morph interpolation is now calculated asynchronously on the STM as one parameter per cycle of the main() loop. Per-voice morph by LFO (if it is assigned) is done with a separate calculation pass on just that voice, so morph gets a little slower with more assignments, but keeps the overhead stable.

**'FrontPanelParser' Naming convention is changed.** The communication exchange files in '/mainboard/' are within folder '/uARTFrontSYX/' to indicate that the communcations spec should ideally migrate in the direction of a full sysEx implementation. In the AVR code, the communications files are in '/avrComms/' and use 'avrComms' as the naming convention to avoid semantic confusion with identically named files and functions between the STM and AVR. 

### Knowledge Files

This repository is designed to be LLM-friendly for feature development. MEMORY.md exists as a context quick-start for a development session, referencing the spec descriptions and log index in `knowledge_files/`. Start a session with something like:

> "The goal of this session is to implement \<some feature\>. Read @README.md and @MEMORY.md for project context and any further files as necessary, then write a plan of implementation with possible conflicts and risk factors to the root directory as \<some feature\>\_AUDIT.md."

The LLM will pull the context it needs, you review the plan, work through it, and write back the session log and updated MEMORY.md when done. There are verbose session logs, a handoff template, and a lightweight log index to keep context manageable.

For a running session index and keyword lookup: `knowledge_files/log_archive/000_SESSION_INDEX.md`.

---

## Repository

**Structure**
- **Session logs:** `knowledge_files/log_archive/` — see `000_SESSION_INDEX.md` for a keyword index
- **Comms and protocol specs:** `knowledge_files/comms_spec_reference/`
- **Hardware notes:** `knowledge_files/hardware_archive/`

## Directory Structure

```text
./LXR/
├── LICENSE.txt
├── MEMORY.md
├── Makefile
├── README.md
├── requirements.txt
├── conflicts.txt
├── firmware image/
│   └── FIRMWARE.BIN                    ← combined build output
├── front/
│   ├── LxrAvr/                         ← ATmega644 front-panel firmware
│   │   ├── Hardware/
│   │   ├── IO/
│   │   ├── Menu/
│   │   ├── Preset/
│   │   ├── avrComms/                   ← AVR-side inter-MCU comms (avrComms* / avrCommsParser* naming)
│   │   └── Makefile
│   └── LxrAvr_bootloader/
│       ├── Bootloader/
│       ├── elmChan/
│       └── lcd/
├── knowledge_files/
│   ├── SESSION_HANDOFF_TEMPLATE.md     ← template for writing new session handoff logs
│   ├── comms_spec_reference/
│   │   ├── COMMS_FLOW_SPEC.md          ← inter-MCU protocol spec
│   │   ├── BACKGROUND_LOAD_TEMPORARY.md
│   │   └── MIDI_TABLE.md               ← full external MIDI CC/NRPN assignment table
│   ├── hardware_archive/
│   │   ├── front/
│   │   │   ├── AVR_HARDWARE.md
│   │   │   └── AVR_SETUP_ALLOCATION.md
│   │   ├── main/
│   │   │   ├── STM32F4_HARDWARE.md
│   │   │   └── STM32F4_SETUP_ALLOCATION.md
│   │   └── ATMEGA_STM32F4_COMMS_AUDIT.md
│   ├── log_archive/
│   │   ├── 000_SESSION_INDEX.md        ← index of all sessions with keyword lookup
│   │   ├── 001_SESSION_HANDOFF_LOG.md
│   │   └── ...                         ← see 000_SESSION_INDEX.md for current list
│   └── reference_material/
├── mainboard/
│   ├── LxrStm32/                       ← STM32F407 audio/sequencer/MIDI firmware
│   │   ├── Libraries/                  ← ARM CMSIS + ST peripheral libraries
│   │   ├── Makefile
│   │   ├── stm32_flash.ld
│   │   └── src/
│   │       ├── AudioCodecManager/      ← DMA ISRs, I2S init, SPSC audio queue
│   │       ├── DSPAudio/               ← voice DSP, mixer
│   │       ├── Hardware/               ← clocks, SD_FAT, USB
│   │       ├── MIDI/                   ← UART MIDI, USB MIDI, parser, output control
│   │       ├── Preset/                 ← kit/morph/pattern state, background load, morph engine
│   │       ├── SampleRom/              ← sample flash metadata and load helpers
│   │       ├── Sequencer/              ← sequencer, Euclidean/SOM generators, clock sync
│   │       └── uARTFrontSYX/           ← STM-side front-panel UART transport and parser
│   └── LxrStm32_bootloader/
└── tools/
    ├── FirmwareImageBuilder/           ← host tool source (C++)
    └── bin/
        ├── FirmwareImageBuilder        ← pre-built Linux binary
        ├── FirmwareImageBuilder.exe    ← pre-built Windows binary
        └── makeFirmware.bat
```

### Where to look for things

| Question | File |
|----------|------|
| Which session introduced a fix? | `knowledge_files/log_archive/000_SESSION_INDEX.md` |
| Full details of a fix or decision? | `knowledge_files/log_archive/0xx_SESSION_HANDOFF_LOG.md` |
| Handoff log template | `knowledge_files/SESSION_HANDOFF_TEMPLATE.md` |
| Confirmed hardware pin / IRQ details | `knowledge_files/hardware_archive/front/` and `knowledge_files/hardware_archive/main/` |
| Inter-MCU comms protocol | `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md` |
| Background file load spec | `knowledge_files/comms_spec_reference/BACKGROUND_LOAD_TEMPORARY.md` |
| External MIDI CC/NRPN table | `knowledge_files/comms_spec_reference/MIDI_TABLE.md` |
| Current known issues and reminders | `MEMORY.md` |

---

## Hardware Overview

- **Audio/control MCU:** STM32F407 (`mainboard/LxrStm32`) — DSP, sequencer, MIDI, USB
- **Front-panel MCU:** ATmega644 (`front/LxrAvr`) — LCD, encoders, buttons, LEDs, SD card preset I/O
- **Inter-MCU link:** UART-based protocol at 500,000 baud (`mainboard/LxrStm32/src/uARTFrontSYX/` on the STM side, `front/LxrAvr/avrComms/` on the AVR side)

### MCU Naming Conventions

STM-side front-panel comms live under `mainboard/LxrStm32/src/uARTFrontSYX/` and use the `frontPanel*` naming. AVR-side comms live under `front/LxrAvr/avrComms/` and use the `avrComms*` / `avrCommsParser*` naming. The terms `frontPanel` and `frontParser` on the AVR side are historical only and should not be used for new code or documentation.

### Encoder

AVR main encoder: Timer1 CTC at ~32.05 kHz, fixed `AB=11` rest phase, six stable-sample phase filter, 192-sample button debounce, narrow rest-jump recovery, edit-mode-only acceleration (1–2 rev/s → 1–4× multiplier). See `knowledge_files/log_archive/019_SESSION_HANDOFF_LOG.md` for the full hardware-approved implementation record. Do not reintroduce legacy read modes, PCINT decoding, or Timer0 encoder sampling.

---
