# SAMPLE_PLAN_PHASE_3: Dual-Type Sample Write Implementation

## Scope

This phase covers importing `/samples` followed by `/loops`, appending both into the STM32 internal flash sample region and tagging loops in metadata.

Relevant current code:

- `front/LxrAvr/Menu/menu.c`: sample upload menu action.
- `front/LxrAvr/Hardware/SD/ff.*`: AVR-owned FatFs implementation.
- `front/LxrAvr/avrComms/*`: AVR-to-STM UART protocol.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`: `SAMPLE_CC` command receive.
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`: current STM-side writer.
- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`: current STM-side SD scanner, not safe for the hardware archive.

## Audit Result

The phase plan points at `mainboard/LxrStm32/src/Hardware/SD/filesystem.c`, but that file does not exist in this repo. The actual current sample import path is:

1. AVR load menu sends `SAMPLE_CC, SAMPLE_START_UPLOAD, 0`.
2. STM stops sequencer.
3. STM calls `sampleMemory_init()`.
4. STM calls `sampleMemory_loadSamples()`.
5. `sampleMemory_loadSamples()` calls STM `SD_Manager` to scan `/samples`.

This conflicts with `STM32F4_HARDWARE.md`, which says the SD card is owned by the AVR and the STM SPI pins are explicitly floated to avoid bus contention. Therefore, the safe implementation must move directory scanning and WAV reading to the AVR, then stream validated sample data to STM over the existing front-panel UART.

Post-Phase-1 hardware test callout: the first `Load: Samples` selection can flash and return early without loading. The current AVR path waits for upload completion with `uart_waitAck()`, but that helper returns the first queued UART byte of any kind. A delayed startup `SAMPLE_COUNT` reply can be consumed as the upload ACK, after which the AVR calls `preset_init()` and retakes the SD card while the STM is still trying to scan it. Phase 3 must replace this raw ACK wait with explicit import status messages and keep SD ownership on the AVR side for the whole transaction.

## Required Protocol Design

Use the existing `SAMPLE_CC` status byte (`0xc0`) and add subcommands. Do not use `SEQ_CC 0x5e/0x5f` here; those are reserved in Phase 4 for sample-name display and are in the sequencer opcode space.

Add to both:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`

```c
#define SAMPLE_IMPORT_BEGIN      0x03
#define SAMPLE_IMPORT_HEADER     0x04
#define SAMPLE_IMPORT_DATA7      0x05
#define SAMPLE_IMPORT_COMMIT     0x06
#define SAMPLE_IMPORT_ABORT      0x07
#define SAMPLE_IMPORT_STATUS     0x08

#define SAMPLE_IMPORT_OK         0x00
#define SAMPLE_IMPORT_ERR_FULL   0x01
#define SAMPLE_IMPORT_ERR_WAV    0x02
#define SAMPLE_IMPORT_ERR_FLASH  0x03
#define SAMPLE_IMPORT_ERR_FLOW   0x04
```

Why each line needs to happen:

- `BEGIN` tells STM to erase/prepare sample flash once.
- `HEADER` sends one sample's name, size, and loop flag before PCM data.
- `DATA7` is a 7-bit-safe PCM payload packet; raw PCM cannot be sent as arbitrary bytes because the parser treats high-bit bytes as status.
- `COMMIT` writes the metadata table and visible sample count.
- `ABORT` lets either side fail closed.
- `STATUS` gives the AVR a displayable result instead of waiting forever.

Risk:

- More subcommands increase parser complexity. Keep all sample import state isolated in a new `SampleImportReceiver` module on STM and a matching AVR sender helper.

## Exact Code Changes

### 1. Add an STM receiver module

Create:

- `mainboard/LxrStm32/src/SampleRom/SampleImportReceiver.h`
- `mainboard/LxrStm32/src/SampleRom/SampleImportReceiver.c`

Header:

```c
#ifndef SAMPLEIMPORTRECEIVER_H_
#define SAMPLEIMPORTRECEIVER_H_

#include "stm32f4xx.h"

void sampleImportReceiver_begin(void);
uint8_t sampleImportReceiver_beginSample(const char* name,
                                         uint32_t frames,
                                         uint8_t looped);
uint8_t sampleImportReceiver_writeFrames(const int16_t* frames,
                                         uint8_t frameCount);
uint8_t sampleImportReceiver_commit(void);
void sampleImportReceiver_abort(void);

#endif
```

Why each line needs to happen:

- The module keeps multi-packet import state out of the already-large front-panel parser.
- `frames` is a 32-bit count because Phase 2 supports long sample positions.
- `looped` feeds directly into `sampleMemory_makeSampleInfo()`.
- `writeFrames()` works in frame counts, not byte counts, avoiding repeated byte/frame conversion mistakes.

Implementation state:

```c
#define SAMPLE_IMPORT_MAX_INFOS SAMPLE_MAX_COUNT

static SampleInfo sampleImport_infos[SAMPLE_IMPORT_MAX_INFOS];
static uint8_t sampleImport_count;
static uint32_t sampleImport_writeAddress;
static uint32_t sampleImport_currentFramesRemaining;
static uint8_t sampleImport_active;
```

Why each line needs to happen:

- `sampleImport_infos` replaces the stack-local `SampleInfo info[50]` currently used by `sampleMemory_loadSamples()`.
- `sampleImport_count` becomes the committed count if all writes succeed.
- `sampleImport_writeAddress` tracks the next PCM flash word address.
- `sampleImport_currentFramesRemaining` validates that AVR sends exactly the announced number of frames.
- `sampleImport_active` rejects stray packets outside an import session.

### 2. Add append-capable SampleMemory writer helpers

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`

Add:

```c
uint8_t sampleMemory_beginInstall(void);
uint8_t sampleMemory_installOneSampleBegin(SampleInfo* dst,
                                           const char* name,
                                           uint32_t frames,
                                           uint8_t looped,
                                           uint32_t offset);
uint8_t sampleMemory_writePcmFrames(uint32_t* writeAddress,
                                    const int16_t* frames,
                                    uint8_t frameCount);
uint8_t sampleMemory_commitInstall(const SampleInfo* infos, uint8_t count);
void sampleMemory_abortInstall(void);
```

Why each line needs to happen:

- `beginInstall()` erases once, instead of erasing between `/samples` and `/loops`.
- `installOneSampleBegin()` creates one packed metadata entry.
- `writePcmFrames()` is the only PCM flash write path and uses Phase 1 bounds.
- `commitInstall()` writes metadata and then count last.
- `abortInstall()` locks flash and leaves `sampleMemory_getNumSamples()` clamped to zero on partial imports.

Risk:

- This replaces the monolithic `sampleMemory_loadSamples()`. Keep the old function as a wrapper only during transition, or remove it once the AVR streaming path is confirmed.

### 3. Update STM `SAMPLE_CC` parser

Target: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Change the current `SAMPLE_START_UPLOAD` case:

```c
case FRONT_SAMPLE_START_UPLOAD:
   seq_setRunning(0);
   sampleMemory_init();
   sampleImportReceiver_begin();
   frontPanelSending_sendSampleUploadAck();
   break;
```

Why each line needs to happen:

- `seq_setRunning(0)` preserves current behavior: sample import is offline.
- `sampleMemory_init()` prepares flash state.
- `sampleImportReceiver_begin()` replaces unsafe STM SD scanning.
- ACK now means "STM is ready to receive streamed samples", not "STM has finished import".

Add cases for the new subcommands and route their decoded payloads into `SampleImportReceiver`.

Risk:

- The current parser only assembles three-byte messages. `HEADER` and `DATA7` need a bounded multi-byte receive mode, similar to existing SysEx/flow-control code. Do not cram a sample header into one triplet.

### 4. Add AVR folder scanning for `/samples` and `/loops`

Target: new AVR helper:

- `front/LxrAvr/SampleImport/sampleImport.h`
- `front/LxrAvr/SampleImport/sampleImport.c`

The helper should expose:

```c
uint8_t sampleImport_installAll(void);
```

Implementation outline:

```c
uint8_t sampleImport_installAll(void)
{
   if(!sampleImport_beginSession())
      return 0;

   if(!sampleImport_sendFolder("/samples", 0u))
      return sampleImport_abort();

   if(!sampleImport_sendFolder("/loops", 1u))
      return sampleImport_abort();

   return sampleImport_commit();
}
```

Why each line needs to happen:

- `beginSession()` sends `SAMPLE_IMPORT_BEGIN` and waits for STM readiness.
- `/samples` is sent first with `looped = 0`.
- `/loops` is sent second with `looped = 1`, appending to the same STM metadata list.
- Commit happens once after both folders succeed.

Risk:

- AVR RAM is limited. Do not buffer whole samples; stream in small blocks from FatFs.

### 5. Replace load-menu action

Target: `front/LxrAvr/Menu/menu.c`

Replace the current sample load branch:

```c
avrComms_sendData(SAMPLE_CC,SAMPLE_START_UPLOAD,0x00);
uint8_t ret = uart_waitAck();
...
avrComms_sendData(SAMPLE_CC,SAMPLE_COUNT,0x00);
```

with:

```c
if(sampleImport_installAll())
{
   avrComms_sendData(SAMPLE_CC, SAMPLE_COUNT, 0x00);
}
else
{
   lcd_clear();
   lcd_home();
   lcd_string_F(PSTR("Sample import"));
   lcd_setcursor(0, 2);
   lcd_string_F(PSTR("Failed"));
}
```

Why each line needs to happen:

- The AVR now owns the full import transaction.
- The sample count query remains the existing way to refresh `menu_numSamples`.
- Failure is displayed locally without leaving the UI waiting for an ACK that may never arrive.
- Removing `uart_waitAck()` from the sample-load path fixes the observed first-load race where a stale `SAMPLE_COUNT` reply can be mistaken for upload completion.

Risk:

- `sampleImport_installAll()` can take seconds. Keep it modal and blocking, matching the current sample upload behavior.
- Until Phase 3 lands, any interim use of the old path should filter specifically for `ACK`/`NACK` with a timeout and preserve parser alignment for other queued bytes.

## Data Encoding Requirement

Raw PCM frames must be 7-bit safe over the MIDI-shaped front-panel parser.

Recommended packet:

```text
SAMPLE_CC SAMPLE_IMPORT_DATA7 count
payload follows in SysEx-like mode:
  each int16 frame => low7, mid7, high2
```

Why:

- Any byte with bit 7 set can be mistaken for a new status byte by the existing parser.
- Three 7-bit bytes carry 16 bits safely.

Risk:

- 499 KB of PCM becomes about 748 KB over UART. At 500000 baud this is still acceptable for an offline import, but the UI should show progress.

## Interdependencies

- Phase 1 provides safe internal flash writes.
- Phase 2 provides packed loop metadata.
- Phase 4 can display imported names because Phase 3 preserves the first three current metadata chars; if 8-char display names are desired, Phase 4 must add extra name storage.

## Plan Callouts

- `mainboard/LxrStm32/src/Hardware/SD/filesystem.c` does not exist.
- The current `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c` path is not hardware-safe according to `STM32F4_HARDWARE.md`.
- If hardware testing proves STM SD access is actually wired and safe, this phase can be simplified, but that would contradict the current hardware archive and `main.c` pin setup.

## Notes
- Implemented `SampleImportReceiver.c/h` on STM to manage stateful flash writes across multiple UART packets.
- Implemented append-capable `SampleMemory` helpers (`sampleMemory_beginInstall`, `sampleMemory_installOneSampleBegin`, `sampleMemory_writePcmFrames`, `sampleMemory_commitInstall`).
- Updated STM `SAMPLE_CC` parser in `frontPanelReceivingProtocol.c` to handle multi-byte payload packets (`SAMPLE_IMPORT_HEADER` and `SAMPLE_IMPORT_DATA7`), reading raw frames directly into memory via custom sys-ex style state machine to avoid 3-byte truncation.
- Added AVR `sampleImport.c/h` to handle FAT filesystem directory traversal of `/samples` and `/loops`.
- Wired AVR `menu.c` `SAVE_TYPE_SAMPLES` to call `sampleImport_installAll()`, sending both directories sequentially and cleanly recovering SD control.
- Fixed the `uart_waitAck()` timeout issue by creating `sampleImport_waitStatus` which polls until a `SAMPLE_IMPORT_STATUS` packet is fully decoded from STM32.
- Both `LxrAvr.bin` and `LxrStm32.bin` compile successfully with the modifications. All changes include necessary comments detailing "how" and "why" they exist.

## Rescue Pass After Hardware Boot Regression

Observed hardware symptom after the first Phase 3 landing:

- Boot displayed `Kit Read Error`.
- Front-panel buttons did not respond.
- Encoder and potentiometers still responded.

Assessment:

- The Phase 3 import path is not supposed to execute at boot, so a boot-time `Kit Read Error` most likely comes from a stale/mismatched `FIRMWARE.BIN`, front/STM protocol desynchronization, or side effects from the new sample-import command path touching resources it should not touch.
- The implementation was salvageable, but it had several safety defects that could wedge the front-panel UART or leave flash/import state inconsistent once `Load: Samples` was selected.
- The old Phase 3 code still called `sampleMemory_init()` from import setup. That function initializes the STM SD manager, which contradicts this phase's hardware assumption that AVR owns sample-file SD access. Import setup now calls `sampleMemory_initFlash()` instead, which only initializes/unprotects internal flash.
- `sampleImportReceiver_begin()` now returns success/failure, and the STM sends `SAMPLE_IMPORT_ERR_FLASH` instead of reporting OK if flash erase/setup fails.
- AVR sample transmission now waits for TX FIFO space before every UART byte, because ignoring `uart_putc()` failure could silently drop bytes when the 256-byte TX buffer filled.
- AVR status waits now have a timeout based on `time_sysTick`, so a lost status packet cannot leave the front panel stuck forever in the load menu.
- AVR folder/file streaming now aborts the STM import session when a file read or directory send fails.
- STM packet handling now rejects oversized `SAMPLE_IMPORT_DATA7` packets before filling the parser buffer.
- STM packet handling now aborts and reports a status error if `sampleImportReceiver_beginSample()` or `sampleImportReceiver_writeFrames()` fails.
- `sampleMemory_writePcmFrames()` now pads an odd final int16 sample frame instead of silently dropping it when flash writes require 32-bit words.
- The local `tools/bin/FirmwareImageBuilder` binary could not run on this Mac due to a libc++ ABI mismatch, so it was rebuilt from the checked-in `tools/FirmwareImageBuilder` source before regenerating `firmware image/FIRMWARE.BIN`.

Post-rescue verification:

- `make -C front/LxrAvr avr -j4` succeeds.
- `make -C mainboard/LxrStm32 stm32 -j4` succeeds.
- `make firmware` succeeds and regenerates `firmware image/FIRMWARE.BIN`.
- `git diff --check` succeeds.

Remaining hardware risk:

- If `Kit Read Error` still appears immediately at boot after flashing the regenerated image, Phase 3 should be isolated by temporarily removing `sampleImport_installAll()` from the AVR load menu and excluding `SampleImport/sampleImport.c` from the AVR build. A boot failure before selecting `Load: Samples` would indicate a startup/protocol/resource regression rather than a streaming/import-data bug.
- If boot is clean but sample import fails during `Load: Samples`, continue from this salvaged implementation rather than ripping it out; the next likely fixes would be stricter WAV validation, progress/status UI, and smaller packet pacing.

## Status
Complete
