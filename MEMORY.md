# LXR -bc- Enhanced Firmware — Project Context

This file is the working memory for you, the Codex/Claude Code/LLM agent performing a task on this project.
Read it fully at the start of every session before touching any code.

---

## Quick Start

```bash
# Repository root (confirmed)
cd /Users/bc/LXR01/LXR-current/LXR

# Build
make clean
make firmware

# Build outputs
# - mainboard/LxrStm32/LxrStm32.bin
# - front/LxrAvr/LxrAvr.bin
# - firmware image/FIRMWARE.BIN
```

**Current working source**: repository root, branch `master-avr-fp-clean`.

**Canonical comms naming**: STM-side front-panel comms live under `mainboard/LxrStm32/src/uARTFrontSYX/` and keep the `frontPanel*` naming. AVR-side comms live under `front/LxrAvr/avrComms/` and use the `avrComms*` and `avrCommsParser*` naming. On the AVR side, `frontPanel` and `frontParser` are historical terminology only and should not be used for new code or docs.

**Session 001 close status**: full top-level build succeeds in this repo with `make clean && make firmware` (warnings remain, no fatal errors).

**Current status after Session 009 closeout (2026-06-10)**: Phase 2 of the architectural refactor is complete. The morph engine, interpolation worker, and live-apply suppression cache have been moved into the new `mainboard/LxrStm32/src/Preset/MorphEngine` module. All relocated logic was renamed with the `preset_` prefix, and a new authoritative DSP bridge was established in `ParameterIngress.c`. `Sequencer` remains a compatibility façade. Build is verified, and the refactor continues according to the phased plan that followed this handoff. Session 010 is expanding the Phase 3 plan so the next implementation pass can move endpoint restore, temp switching, and background-load finalization into `Preset`.

**Current status after Session 013 closeout (2026-06-13)**: Phase 6 cleanup/naming is complete and `make -C mainboard/LxrStm32 -j4 stm32` is green. `mainboard/LxrStm32/src/uARTFrontSYX/` owns the front-panel UART transport and parser, `uARTFrontSYX/FrontPanelProtocol.h` owns the front-panel opcode namespace through a compatibility include from `MidiMessages.h`, and `PresetLoadCache` still exposes the shared `presetLoad_*` background-load/session API while `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c` still carries a parser-local duplicate helper block. `SeqKitState` uses endpoint-oriented field names, the AVR snapshot buffers were renamed to match their file-load/menu-snapshot role, and the session 013 handoff now points at `PRESET_CONSOLIDATION_AUDIT.md`, `COMMS_FLOW_SPEC.md`, and `TEMPORARY_PAT_PARAM_LOAD_SPEC.md` as the active next-step docs. Session 012 smoke-tested `.ALL` load, morph, parameter change, copy to temp, load new `.ALL`, and switch back; MIDI front-panel verification was already confirmed before the closeout pass.

**Current status after Session 014 implementation pass (2026-06-13)**: `Sequencer`, `MidiParser`, and `MidiVoiceControl` now read live voice/pattern/morph state directly from the owner modules instead of the shared cache header. `presetLoad_finalizeTempBackgroundLoad()` is exposed through the `TempPlaybackSwitch` interface so `sequencer.c` no longer needs `PresetLoadCache.h` for the finalizer call. `PresetLoadCache.c/.h` were deleted, and the remaining parser/session bridge now lives inside `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelParser.c` as the transitional holdover. `make -C mainboard/LxrStm32 -j4 stm32` is green after the cutover, and the user re-test confirmed the Phase 7 cutover behaves correctly.

**Current status after Session 015 implementation pass (2026-06-13)**: Phase 8 has started in code. `mainboard/LxrStm32/src/Preset/ParameterArray.c/.h` now owns the parameter table plus the classification helpers that used to live in `ParameterMap.c`, `ParameterMap.c` and `ParameterMap.h` have both been deleted, `mainboard/LxrStm32/src/MIDI/ParameterArray.h` has been deleted, and `TempPlaybackSwitch` now stores its transition flags inside one explicit state struct with legacy names preserved only as macros. Phase 9 now covers the prefix cleanup: the temp-switch state object is `preset_tempPlaybackSwitchState`, the AVR restore handshake mailboxes are `preset_tmpKitHandshakeReady` / `preset_tmpKitHandshakeAck`, and `make -C mainboard/LxrStm32 -j4 stm32` is green again after a clean rebuild. The remaining Phase 9 judgment call is whether `ParameterIngress` should stay as the final narrow router or be collapsed further in a later pass; Phase 10 will extract the live-apply helpers, Phase 11 will rename the remaining Preset exports to `preset_`, and Phase 12 will move the remaining Preset-state accessors out of `Sequencer`. The remaining stale `seq_` / `Seq` ownership names are being cataloged separately, starting with the restore queue/cursor block and the push-disable flag. Later in Session 015, the AVR front-panel encoder was reworked onto a Timer1 compare polling path, the dead 2-step API was removed, and the user re-tested it successfully even though reversals and fast spins still feel rough.

**Current status after Session 016 implementation pass (2026-06-13)**: The AVR front-panel main encoder is now hardware-approved with the final Timer1 16 kHz rest-phase FSM. The only supported rotation read is `encode_stableRead4()`, `front/LxrAvr/encoder.h` exposes only `encode_init()`, `encode_stableRead4()`, and `encode_readButton()`, and `front/LxrAvr/config.h` no longer has `ENC_USE_STABLE_DRIVER`. The physical rest phase is fixed at `AB=11` (`ENCODER_REST_STATE = 0x03`), and the decoder emits detents only after a legal full sequence leaves and returns to that rest phase. Do not reintroduce legacy read modes, PCINT decoding, Timer0 encoder sampling, or the temporary LCD/debug hooks used during Session 016. `make -C front/LxrAvr avr -j4` and `make firmware` are green, with the usual AVR IO-register warnings.

**Current status after Session 017 closeout (2026-06-14)**: Phase 10/11 Preset cleanup is complete and the remaining Phase 12 review is retrospective only. `KitState`, `EndpointRestore`, `MorphEngine`, `ParameterIngress`, `TempPlaybackSwitch`, `sequencer.c`, and the parser/MIDI call sites were switched to the canonical `Preset*` / `preset_*` names, the last Preset-owned `seq_` aliases were removed, and the UART send-side work is now clearly tracked as protocol refactor work rather than more Preset ownership cleanup. The STM32 build was re-run successfully after the rename sweep, and the user re-tested the firmware and reported nothing obviously broken.

**Current status after Session 018 closeout (2026-06-14)**: The front-panel send helpers are now consolidated behind `uARTFrontSYX/frontPanelSendingProtocol`, the MIDI parser is split into `ChannelMidiParser` and `GlobalMidiParser`, and the transitional front-panel load/session bridge now lives in `Preset/PresetLoadCache` instead of `frontPanelParser.c`. The STM32 build was re-run successfully after the split work, and the user re-tested the key front-panel and MIDI paths: menu parameter change, file load, copy-to-temp, temp switch, MIDI clock sync, global CC1, voice CC1, and voice note trigger.

**Current status after Session 019 closeout (2026-06-14)**: The AVR front-panel main encoder was retuned after slow counter-clockwise missed decrements returned. The current hardware-approved implementation still uses only Timer1 compare A and raw `encode_stableRead4()` detents, but now samples at roughly 32.05 kHz with six stable phase samples, fixed `AB=11` rest anchoring, narrow rest-jump recovery for filtered `11 -> 00 -> adjacent -> 11` contact sequences, and 192-sample button debounce. Edit-mode parameter acceleration is config-driven from complete emitted detents only, with `ENC_ACCEL_MIN_REV_PER_SEC = 1`, `ENC_ACCEL_MAX_REV_PER_SEC = 2`, and final hardware-tested `ENC_ACCEL_MAX_MULT = 4`. Acceleration is applied only to menu edit-mode value changes; navigation/load/save/copy-clear selection remain unaccelerated. `make -C front/LxrAvr avr -j4` and `make firmware` are green with the usual AVR warnings, and the user reported the final encoder behavior is good.

**Current status after Session 020 closeout (2026-06-14)**: The refactor finalization pass corrected the Session 018 planning regression and completed the cache/protocol split objectives. `mainboard/LxrStm32/src/Preset/PresetLoadCache.c/.h` are gone again, no active `presetLoad_*` cache API remains, file loads route directly to normal Preset/Pattern storage, and the existing normal/temp Preset and Pattern switching remains the only supported staging model. STM front-panel receive is now `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c/.h`, STM front-panel send is `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h`, and the old STM `FrontPanelProtocol.h` / `frontPanelParser.h` shim headers were removed after include redirection. At that point the AVR side was still using the older `frontPanelReceivingProtocol.c/.h` and `frontPanelSendingProtocol.c/.h` names; Session 021 later renamed that layer to `front/LxrAvr/avrComms/` with `avrComms*` / `avrCommsParser*` prefixes and removed the AVR `frontPanelParser.h` shim. The final cleanup also renamed front-panel receive state from `frontParser_midiMsg` to `frontParser_command`, moved the internal CC/CC2 parameter-apply ladder from `MIDI/MidiParser.c` to `uARTFrontSYX/frontPanelReceivingProtocol.c` as `frontParser_applyParameterCommand()`, renamed `MidiVoiceControl.c/.h` to `MidiOutputControl.c/.h`, and moved Sequencer MIDI fan-out behind `outputControl_*` helpers. `make -C mainboard/LxrStm32 -j4 stm32`, `make -C front/LxrAvr avr -j4`, and `make firmware` were verified, and the user hardware-tested Steps 1, 2, and 3 successfully.

**Current status after Session 021 closeout (2026-06-15)**: The AVR comms layer was renamed and consolidated under `front/LxrAvr/avrComms/` with `avrCommsReceivingProtocol.c/.h`, `avrCommsSendingProtocol.c/.h`, and `avrCommsPanelParser.h`. AVR-side identifiers now use the `avrComms*` / `avrCommsParser*` prefixes, while STM-side front-panel ownership remains under `mainboard/LxrStm32/src/uARTFrontSYX/` with `frontPanel*` names. This session was a naming and documentation consolidation pass, so no firmware behavior changed; the most recent compiled verification remains the prior AVR build.

**Current status after Session 022 closeout (2026-06-16)**: Copy-to-temp now snapshots the actually playing per-track pattern sources instead of a generic pending-pattern view, and the temp-pattern repeat path is keyed off `SEQ_TMP_PATTERN` so the temp slot keeps holding until the user makes a manual change. The `Sequencer` API surface was documented in-place in both `sequencer.h` and `sequencer.c`, the stale Sequencer temp-boundary declaration was removed, Pattern/PatternData naming and comments were cleaned up, and `make -C mainboard/LxrStm32 -j4 stm32` was verified again. This pass was implementation plus documentation cleanup only; no comms spec updates were needed.

**Current status after Session 023 closeout (2026-06-16)**: The AVR load-page control was renamed to the 5-state background-load selector `PAR_FILE_LOAD_BACKGROUND`, with `TEXT_FILE_LOAD_BACKGROUND`, `backgroundLoadNames`, and `SEQ_LOAD_BACKGROUND` updated on the AVR side while STM runtime behavior stayed untouched. The packed menu-ID table used the free `MENU_FILE_LOAD_BACKGROUND = 0` slot because the existing dtype encoding only has four bits for the menu ID, and the globals file continues to round-trip the same raw byte without a serializer change. `make -C front/LxrAvr avr -j4`, `make -C mainboard/LxrStm32 -j4 stm32`, and `make firmware` all passed again after the rename. `TEMP_LOAD_MENU_AUDIT.md`, `knowledge_files/log_archive/000_SESSION_INDEX.md`, `knowledge_files/log_archive/023_SESSION_HANDOFF_LOG.md`, `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`, and `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md` were updated to keep the new terminology canonical.

**Current consolidation / protocol planning artifacts**: the durable finalization notes now live in `knowledge_files/log_archive/023_SESSION_HANDOFF_LOG.md`, `knowledge_files/log_archive/021_SESSION_HANDOFF_LOG.md`, `knowledge_files/log_archive/020_SESSION_HANDOFF_LOG.md`, `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`, and `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`. Current encoder details live in `knowledge_files/log_archive/019_SESSION_HANDOFF_LOG.md` and supersede the temporary `ENCODER_AUDIT.md`. `TEMP_LOAD_MENU_AUDIT.md` was the temporary working doc for Session 023 and can be deleted after the closeout pass. `REFACTOR_FINALIZATION.md`, `REFACTOR_MY_MESS_CLEANUP.md`, `PRESET_CONSOLIDATION_AUDIT.md`, and `MIDI_UART_SPLIT_AUDIT_EDIT.md` were temporary working docs and may be deleted after Session 020.

Canonical current WIP docs:
- `knowledge_files/log_archive/023_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/022_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/021_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/020_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/019_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/018_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/017_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/016_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/015_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/014_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/013_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/012_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/011_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/010_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/009_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/008_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/007_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/006_SESSION_HANDOFF_LOG.md`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
- `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`

Session 005 closeout / remaining follow-up:
- Encoder follow-up was most recently completed in Session 019. The hardware-approved implementation is the Timer1 ~32.05 kHz fixed-`AB=11` rest-phase FSM with six stable phase samples, narrow rest-jump recovery, 192-sample button debounce, raw `encode_stableRead4()` rotation reads, and edit-mode-only acceleration. Keep it unchanged unless new hardware testing reveals a regression.
- Automate the temp-pattern switch/background-load process if it remains desired.
- Add global parameter switches for background loading if that work is revived.
- Keep the temporary SEQ16 pattern keyhole in place until after the future preset/morph refactor.

The `knowledge_files/session_in_flight/` directory was cleaned up (post-Session 020). The following audits no longer exist on disk and should not be referenced or recreated:
- `knowledge_files/session_in_flight/AUDIT_MORPH_MOVE.md`
- `knowledge_files/session_in_flight/TMP_VARS_AUDIT.md`
- `knowledge_files/session_in_flight/PRF_ALL_LOAD_FIX_AUDIT-IN_FLIGHT.md`
- `knowledge_files/session_in_flight/COMMS_FLOW_AUDIT-IN_FLIGHT.md`
- `knowledge_files/session_in_flight/TEMP_CACHE_LOAD-IN_FLIGHT-POST_MORPH_MOVE.md`
- `knowledge_files/session_in_flight/COMMS_FLOW_AUDIT-IN_FLIGHT-POST_MORPH_MOVE.md`

User-referenced checkpoints:
- Commit `90d3f08` is the checkpoint where `.ALL` and `.PRF` load their parameters correctly provided there is no morph automation and background loading into the temp slot is turned off for `.PRF`.
- Session 003 final code is the checkpoint where standard STM-owned morph sounds correct, but normal/temp parameter exchange is broken.

---

## Repository Layout

```text
./LXR/
├── LICENSE.txt
├── MEMORY.md
├── Makefile
├── README.md
├── conflicts.txt
├── firmware image/
│   └── FIRMWARE.BIN
├── front/
│   ├── LxrAvr/
│   │   ├── Hardware/
│   │   ├── IO/
│   │   ├── Menu/
│   │   ├── Preset/
│   │   ├── avrComms/
│   │   ├── Makefile
│   │   └── README.md
│   └── LxrAvr_bootloader/
│       ├── Bootloader/
│       ├── default/
│       ├── elmChan/
│       ├── lcd/
│       └── README.md
├── knowledge_files/
│   ├── SESSION_HANDOFF_TEMPLATE.md
│   ├── comms_spec_reference/
│   ├── hardware_archive/
│   │   ├── front/
│   │   │   ├── AVR_SETUP_ALLOCATION.md
│   │   │   └── AVR_HARDWARE.md
│   │   ├── main/
│   │   │   ├── STM32F4_SETUP_ALLOCATION.md
│   │   │   └── STM32F4_HARDWARE.md
│   │   └── ATMEGA_STM32F4_COMMS_AUDIT.md
│   ├── log_archive/
│   │   ├── 000_SESSION_INDEX.md
│   │   ├── 001_SESSION_HANDOFF_LOG.md
│   │   └── ...
│   └── reference_material/
├── mainboard/
│   ├── LxrStm32/
│   │   ├── Libraries/
│   │   ├── Makefile
│   │   ├── src/
│   │   │   ├── AudioCodecManager/
│   │   │   ├── DSPAudio/
│   │   │   ├── Hardware/
│   │   │   ├── MIDI/
│   │   │   ├── Preset/
│   │   │   ├── SampleRom/
│   │   │   ├── Sequencer/
│   │   │   └── uARTFrontSYX/
│   │   └── stm32_flash.ld
│   └── LxrStm32_bootloader/
│       ├── Libraries/
│       ├── Release/
│       ├── src/
│       └── stm32_flash.ld
├── requirements.txt
└── tools/
    ├── FirmwareImageBuilder/
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
| Session handoff template / formatting | `knowledge_files/SESSION_HANDOFF_TEMPLATE.md` |
| Confirmed hardware/protocol notes? | `knowledge_files/hardware_archive/` |
| AVR hardware and setup allocation | `knowledge_files/hardware_archive/front/AVR_HARDWARE.md`, `knowledge_files/hardware_archive/front/AVR_SETUP_ALLOCATION.md` |
| STM32 hardware and setup allocation | `knowledge_files/hardware_archive/main/STM32F4_HARDWARE.md`, `knowledge_files/hardware_archive/main/STM32F4_SETUP_ALLOCATION.md` |
| Current comms specs | `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`, `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md` |
| Reference material snapshots | `knowledge_files/reference_material/` |
| Current known issues and reminders? | `MEMORY.md` |
| Session 003 STM morph move details | `knowledge_files/log_archive/003_SESSION_HANDOFF_LOG.md` |
| Session 004 temp/background loading closeout | `knowledge_files/log_archive/004_SESSION_HANDOFF_LOG.md` |
| Session 006 refactor planning details | `knowledge_files/log_archive/006_SESSION_HANDOFF_LOG.md` |
| Session 007 refactor Phase 1 details | `knowledge_files/log_archive/007_SESSION_HANDOFF_LOG.md` |
| Current preset/morph refactor knowledge | `knowledge_files/log_archive/020_SESSION_HANDOFF_LOG.md`, `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`, `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md` |
| Current comms/protocol knowledge | `knowledge_files/log_archive/021_SESSION_HANDOFF_LOG.md`, `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`, `knowledge_files/hardware_archive/ATMEGA_STM32F4_COMMS_AUDIT.md` |

---

## Project Goal

- Maintain and extend the LXR firmware while preserving reliable dual-MCU operation (ATmega front panel + STM32 audio engine).
- Keep build reproducible with current toolchains (`arm-none-eabi-gcc`, `avr-gcc`) and preserve firmware image output flow.
- This folder is the repository/codebase.
- In normal maintenance, only session logs under `knowledge_files/log_archive/` should be changed inside `knowledge_files/`; broader documentation passes may update `comms_spec_reference/`, `hardware_archive/`, or `reference_material/` when a session explicitly requires alignment.
- Some reference files formerly at repo root were moved under `knowledge_files/reference_material/` during Session 002 cleanup (`Changelog.txt`, `Readme - firmware additions to v.36.txt`, `linux_build_guide.txt`, `lxr-midi-assign.txt`, `mod_targ_lineup.xls`).
- Root files `P000.ALL`, `P000.PRF`, `P005.PRF`, and `P000.SND` are temporary test files and are expected to be removed later.

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

- Session 019 supersedes older encoder timing/tuning notes. See `knowledge_files/log_archive/019_SESSION_HANDOFF_LOG.md`.
- Final AVR main encoder implementation:
  - Wiring is immutable: phase A = PC0, phase B = PC1, button = PC2.
  - Timer1 CTC samples at roughly 32.05 kHz from `TIMER1_COMPA_vect` using prescaler 8 and `OCR1A = 77` at 20 MHz.
  - Phase filtering requires six stable samples per phase.
  - Button debounce uses a 192-sample integrator in the same Timer1 ISR.
  - Physical detent rest is fixed as `AB=11` (`ENCODER_REST_STATE = 0x03`).
  - The rest-phase FSM emits one detent only after a same-direction sequence leaves rest and returns to rest.
  - A narrow rest-jump recovery rescues filtered `11 -> 00 -> adjacent -> 11` contact sequences while all other illegal transitions still reset.
  - `encode_stableRead4()` is the raw supported rotation read and drains complete detents atomically.
  - `encode_readButton()` is the only button read.
  - `encode_getAccelerationMultiplier()` exposes a complete-detent speed multiplier for menu edit acceleration.
  - Acceleration config lives in `front/LxrAvr/config.h`: min 1 rev/s, max 2 rev/s, max multiplier 4.
  - Acceleration is applied only in menu edit-mode parameter changes, not navigation/load/save/copy-clear selection.
- Do not reintroduce `encode_stableRead1()`, `encode_read1()`, `encode_read4()`, `encode_stableRead2()`, `ENC_USE_STABLE_DRIVER`, PCINT decoding, Timer0 encoder sampling, or temporary LCD/debug hooks.

---

## Digital Input (DIN)

- **Initialization Requirements**: `din_inputData[]` software mirror must be synchronized with the physical shift registers during `din_init()`.
- **Logic**: 0 = Pressed, 1 = Released.
- **Initialization Bug (Fixed Session 008)**: Initializing mirror to `0` with `memset` caused ghost release events on the first scan. Sequential polling (10 buttons/loop) allowed SELECT buttons to be processed while SHIFT state was still `0` (interpreted as 'Pressed'), triggering phantom 'shifted release' actions.

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
- Session 004 comms knowledge is consolidated in `knowledge_files/hardware_archive/ATMEGA_STM32F4_COMMS_AUDIT.md`. Implemented checkpoint behavior includes acknowledged load sessions, STM32 quiet mode, credit-metered globals/voice/meta bursts, endpoint restore on every normal/temp switch, and removal/bypass of obsolete first-switch voice hold/unhold delays. Old SysEx/callback waits still need timeout recovery in a later pass.
- If build errors reference functions/line numbers missing in local files, treat as snapshot mismatch and confirm active repository first.

---

### Morph / Endless-Pot Reminders

- Standard morph operation is STM-owned after Session 003.
- Normal/temp parameter switching is STM-owned after Session 004. AVR/front panel owns menu display, file I/O, and endpoint-byte transport/display, but must not compute or stream standard live morph or own canonical temp staging.
- Raw endpoint parameter storage uses AVR/menu indices on both MCUs. Low `+1` is only for applying an ordinary low live CC through `frontParser_applyParameterCommand(...)`.
- Global morph writes all six STM per-voice morph amounts; actual interpolation uses per-voice morph amounts.
- Per-voice morph may come from global morph, MIDI, step automation, or LFO/velocity modulation destinations.
- Velocity-to-voice-morph is a one-shot trigger-time write to the active normal/temp per-voice morph value. Do not route velocity-to-voice-morph through the generic modulation matrix.
- LFO-to-voice-morph is serviced by the sequencer morph drain using `modNode_lastVal` style LFO state. Do not route it through generic high-frequency modulation paths; that caused audio breakup.
- `seq_serviceMorphInterpolation()` runs one parameter per STM main-loop pass and is the only intended writer of `interpolatedParams[]`.
- `seq_serviceMorphInterpolation()` now has phase 0 for standard morph and optional later phases for source voices whose LFO targets voice morph. Each main-loop pass must still perform at most one interpolation/application unit.
- Automation selector parameters, morph amount parameters, and LFO target voice selector parameters are carved out of ordinary interpolation and return kit/front endpoint values.
- Automation target endpoint bytes and resolved sideband structures must stay coherent. For LFO target ingress, raw selector bytes must be written to `kitEndpointParams[]` for kit endpoints or `morphEndpointParams[]` for morph endpoints, and the matching `PresetAutomationTargets` sideband must be refreshed. If only the sideband is changed, normal/temp switching can later restore stale/off raw bytes and drop the assignment.

---

## Known Issues / Technical Debt

### Resolved in Session 008

- **AVR Startup Substep Toggle Bug**: Root cause identified as a mismatch between the `din_inputData` software mirror (initialized to 'all-pressed' `0`) and the actual hardware state (all-released `1`), combined with a sequential polling order that processed SELECT buttons before the SHIFT button. This triggered a phantom 'shifted release' event on boot, sending 8 `STEP_CC` toggle messages to the STM32. Fixed by synchronizing `din_inputData` with hardware in `din_init()` before the main loop starts.

### Resolved in Session 001
- Header-defined globals causing multiple definitions (`MidiMessages.h`, `MidiVoiceControl.h`, SD headers).
- Legacy inline linkage breakage causing undefined references (`BufferTools`, `GetRngValue`).
- Full build now completes to `firmware image/FIRMWARE.BIN` in this repo.

### High Priority

- Current repository state is a functional post-morph temp/background-load baseline after Session 004.
- Standard morph and normal/temp switching are STM-owned and hardware-verified for ordinary operation, including LFO target to voice morph.
- Session 005 closed the remaining global morph menu-sync issue; the remaining small workflow items are tracked below and before the larger preset/morph refactor.
- SEQ16 temp pattern observation bodge remains in place until after the future refactor.
- Preserve STM morph invariants while making Session 005 fixes: no AVR live morph fallback, no per-parameter valid arrays, no direct file/AVR writes into interpolation arrays.

### Current Temp Pattern / `.PRF` WIP Reminders

- SEQ16 is used as a SELECT button for `SEQ_TMP_PATTERN`.
- Session 004 made normal/temporary pattern and parameter exchange functional again after the Session 003 morph move.
- AVR-to-STM endpoint dump (Opcodes 0x65/0x66) is implemented and captures `parameter_values_fileLoadSnapshot`, `parameters2_fileLoadSnapshot`, and all 16 mod targets.
- STM-side `preset_normalKitState` and `preset_tmpKitState` are the source of truth for menu pushbacks.
- STM-to-AVR parameter pushback on temp-pattern transitions must happen on every normal/temp switch so the menu stays in sync. Global morph push-up on those boundaries is now handled by display-only report traffic.
- Endpoint storage uses raw AVR/menu parameter indices. Do not apply low `+1/-1` offsets to endpoint arrays or PRF restore opcodes.
- Sound engine live morph path uses `interpolatedParams` after the STM worker writes it; menu pushback uses `kitEndpointParams`.
- File loads must target normal parameter storage and normal pattern storage only; they must never touch temporary parameter storage or temporary pattern data.
- File loads must only write endpoint arrays, not interpolation arrays. Automation target sideband caches may be refreshed on file load, but parameter bytes still flow into endpoint storage only.
- Sound parameter arrays are always valid from zero init. Do not reintroduce `kitEndpointParamsValid[]`, `morphEndpointParamsValid[]`, or `interpolatedParamsValid[]`.
- No extraneous LCD/debug writes should occur during copy/paste, temp/normal switching, endpoint restore, or file-load operations.
- Do not make the temp pattern loadable/saveable unless explicitly requested.
- Future background-load automation should build on the Session 004 STM-owned normal/temp image model and should not revive AVR-owned temp staging.
- The old PRF cache/state-machine work was retired in Session 014. Treat any remaining `SEQ_PRF_CACHE_*` mentions, live-pattern getter references, or pending-counter notes as historical context only.

### Morph Move Completed In Session 003

- Use exact terms: "morph parameter endpoint" and "morph automation target endpoint"; do not use ambiguous shorthand for these concepts.
- Per-voice morph is never displayed or directly set in the AVR menu.
- Per-voice morph value conversion: `0` is valid, `0-126` maps to `value * 2`, and `127` maps to exact full morph `255`.
- STM-side morph interpolation/modulation is always serviced; pending morph work gets exactly one parameter interpolation per STM main-loop pass.
- During normal/temp pattern changeover, receipt of AVR/front-panel global morph may be blocked, but STM-side morph interpolation/modulation must not be paused.
- STM now has the AVR `modTargets[]` selector mapping needed to resolve interpolated automation destinations and apply voice morph modulation.
- The live-apply cache is not a validity system. It only prevents repeated DSP setter calls for unchanged values while still allowing first-time zero values from file load to land in DSP.

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
- Reintroducing per-parameter valid arrays in `PresetKitState`; sound parameter arrays are always-defined from zero init.
- Letting file load or AVR endpoint restore write directly into `interpolatedParams[]`; the STM morph worker owns interpolation arrays.
- Using `interpolatedParams[]` equality as a proxy for live DSP state; this skipped zero-valued file parameters when DSP init defaults were nonzero.
- Applying every parameter to DSP on every morph scan pass; this caused a loud continuous low-Hz overlay.
- Restoring AVR live morph computation to paper over temp-cache/load bugs; standard morph is STM-owned after Session 003.

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

---

## Governance Rules

### 1. Identity And Communication

- Be technical, concise, and direct.
- No greetings, apologies, filler, or meta-commentary.
- State facts, risks, and next actions. Keep code-adjacent documentation comments brief: why, ownership, timing.

### 2. Security And Boundaries

- Stay inside the workspace root, read permissions are always granted in root. 
- No network, sudo, system config, or destructive commands unless explicitly requested.
- No mutating git unless explicitly requested. Read-only git inspection is allowed when needed.
- Never expose credentials. Ask before any command that can alter external state.

### 3. Coding Standards

- Make the smallest correct change to functional code possible for the request, preferring existing helpers and protocols over new abstractions.
- All exported functions and variables should be accompanied by a commented-out note adjacent the code describing its function. All frequently-accessed variables in code files that are not function-local should have similar comments describing function.

### 4. Design Philosophy

- Preserve STM ownership of sound state and AVR ownership of UI, SD file I/O, and menu display state.
- Protocol direction matters. Do not reuse a command name in the opposite direction unless the intent is explicit.

### 5. Reasoning And Self-Review

- Before a complex change, give a short reasoning summary: challenge, affected modules, edge cases, timing risks, verification.
- After drafting code, do a red-team pass for scope creep, ownership errors, timing spikes, null or bounds risk, stale docs, and missing verification.
