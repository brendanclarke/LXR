# LXR Project Memory (Session 001)
hey fucko
Last updated: 2026-05-21

## Project Identity
- Project: Sonic Potions LXR drum synth firmware fork (`LXR -bc- Enhanced Firmware`).
- Hardware architecture: dual MCU.
- Front panel MCU: ATmega644 (`front/LxrAvr`) handles UI, LCD, buttons, encoders, SD.
- Main audio MCU: STM32F407 (`mainboard/LxrStm32`) handles DSP, sequencer, MIDI, USB.

## Repository Map
- `README.md`: original build and toolchain notes.
- `mainboard/LxrStm32/`: STM32 firmware (audio/sequencer/MIDI/USB).
- `front/LxrAvr/`: AVR firmware (UI + SD + front protocol).
- `tools/FirmwareImageBuilder/`: combines MCU binaries into final firmware image.
- `knowledge_files/hardware_archive/`: hardware and setup audits for both MCUs.
- `knowledge_files/log_archive/000_SESSION_INDEX.md`: session index template/log.

## Build Workflow
- Clean: `make clean`
- Build full firmware image: `make firmware`
- Outputs:
- `mainboard/LxrStm32/LxrStm32.bin`
- `front/LxrAvr/LxrAvr.bin`
- `firmware image/FIRMWARE.BIN`

## Toolchain Expectations
- Original docs target older GCC toolchains (ARM GCC 4.8 era + avr-gcc).
- Current observed build log uses ARM GCC 14.2.1 (`ArmGNUToolchain/14.2.rel1`).
- Main compatibility risk: legacy C patterns in headers and `inline` usage that were tolerated by old compilers but fail on newer defaults.

## Session 001 Build Findings
- Original fatal linker failures were in STM32 build (see `err.txt`):
- Multiple definition symbols (`voiceStatus`, `ParamEnums`, `Param2Enums`, SD globals).
- Undefined references to functions declared/defined as `inline` (`bufferTool_*`, `GetRngValue`).
- Session 001 applied source fixes and STM32 now links successfully (`LxrStm32.bin` produced).
- Current remaining blocker for full `make firmware`: AVR toolchain is missing (`avr-gcc: command not found`).
- Detailed analysis and patch strategy captured in `BUILD_AUDIT.md`.

## Hardware/Protocol Notes To Keep In Mind
- STM32<->AVR UART protocol runs at 500000 baud and is central to UI + preset transfer.
- Knowledge audit flags protocol robustness risks (silent FIFO overflow and blocking ACK waits) in `knowledge_files/hardware_archive/ATMEGA_STM32F4_COMMS_AUDIT.md`.
- These comms concerns are separate from the current compile breakage, but matter for future stability work.

## Recommended Next Session Starting Point
1. Apply the concrete source fixes listed in `BUILD_AUDIT.md`.
2. Install/configure AVR toolchain so `avr-gcc` is available in PATH (or set `AVR_TOOLKIT_ROOT`).
3. Rebuild with `make clean && make firmware`.
4. Capture remaining warnings and decide what to treat as technical debt vs required fixes.
5. Append session outcome to `knowledge_files/log_archive/000_SESSION_INDEX.md`.
