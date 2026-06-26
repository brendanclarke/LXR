# 030 Session Handoff Log - Sample Import Stabilization And Closeout

DATE: 2026-06-26

## Session Goal

Improve sample handling across the planned Phase 1 through Phase 3 work, then preserve the details from the temporary root planning/debug documents before those files are deleted.

Temporary source documents folded into this handoff:

- `SAMPLE_PLAN_PHASE_1.md`
- `SAMPLE_PLAN_PHASE_1_POST_BUGS.md`
- `SAMPLE_PLAN_PHASE_2.md`
- `SAMPLE_PLAN_PHASE_3.md`
- `SAMPLE_PLAN_PHASE_3_AUDIT_INVESTIGATION_1.md`
- `SAMPLE_PLAN_PHASE_3_AUDIT_INVESTIGATION_2.md`
- `PHASE_3_CORRECTIONS_PART_2.md`

## Completed

### Phase 1 - Flash Safety And Metadata Commit Guards

Phase 1 hardened the STM32 internal flash sample-import path.

Files:

- `mainboard/LxrStm32/src/SampleRom/flash_if.h`
- `mainboard/LxrStm32/src/SampleRom/flash_if.c`
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`

Current behavior:

- Sample storage is bounded to STM32F407 internal flash sectors 8..11:
  `0x08080000..0x080FFFFF`.
- PCM storage starts after the committed count word at
  `SAMPLE_ROM_START_ADDRESS + 4`.
- The `SampleInfo` metadata table starts at `0x080F9400`.
- PCM writes use `FLASH_If_WriteSamplePcm()` so they cannot cross into the
  metadata table.
- Generic sample flash writes check null pointers, word alignment, start/end
  bounds, and post-write verify.
- `FLASH_If_Erase()` accepts only `FLASH_IF_SAMPLE_START_ADDRESS`; it no longer
  acts as a broad user-flash erase helper for arbitrary application sectors.
- Write protection logic is scoped to sectors 8..11 through
  `FLASH_IF_SAMPLE_WRP_MASK`.
- The committed sample count is written last, after PCM and metadata have been
  accepted.
- Imported `SampleInfo` structs are zero-filled before use so the padding byte
  in the 12-byte on-flash ABI is deterministic.

Why this exists:

- The hardware archive confirms the STM32F407VGT6 has 1 MB internal flash.
  The bootloader/application live below the sample area; sample import must not
  erase or program those sectors.
- Flash programming is word-sized, so unaligned and out-of-range writes must be
  rejected before the HAL programming call.
- Writing the committed count last prevents partially imported or failed samples
  from appearing as valid waveform slots after reboot.

### Phase 1 Post-Bug Assessment

Two pre-existing glitches were investigated:

- First `Load: Samples` attempt flashed/returned to the load menu early.
- A silent `s1` waveform appeared when only the loaded `s0` should exist.

Assessment:

- The first-load glitch came from waiting for a raw ACK-style byte while STM was
  sending ordinary protocol traffic. This is now fixed by the Session 030
  `SAMPLE_UPLOAD_RESULT` latch and parsed wait loop.
- The silent extra slot was a combination of stale sample-count/menu state and
  an off-by-one playback guard. Phase 2 added the real `sampleIndex >= count`
  guard in the user-sample renderer, and Phase 3 made import metadata dense so
  invalid candidates no longer consume slots.

### Phase 2 - User Sample Playback And Loop Metadata

Phase 2 made imported samples safe to play as oscillator waveforms, including
long samples and looped samples.

Files:

- `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`
- `mainboard/LxrStm32/src/DSPAudio/Oscillator.h`
- `mainboard/LxrStm32/src/DSPAudio/Oscillator.c`
- `mainboard/LxrStm32/src/DSPAudio/DrumVoice.c`
- `mainboard/LxrStm32/src/DSPAudio/Snare.c`
- `mainboard/LxrStm32/src/DSPAudio/CymbalVoice.c`
- `mainboard/LxrStm32/src/DSPAudio/HiHat.c`

Current behavior:

- `SampleInfo.size` keeps the 12-byte on-flash ABI:
  - bit 31 is `SAMPLE_INFO_LOOP_FLAG`;
  - bits 30..0 are int16 frame count.
- `sampleMemory_getSampleSizeFrames()`,
  `sampleMemory_isSampleLooped()`, and `sampleMemory_makeSampleInfo()` are the
  canonical metadata helpers.
- `OscInfo` now tracks imported-sample playback with:
  - `samplePosition` as a 32-bit frame index;
  - `sampleFraction` as the 17-bit fractional accumulator;
  - `sampleActive` as the one-shot gate.
- `osc_resetSamplePlayback()` resets the imported-sample read head and one-shot
  gate on voice trigger.
- Drum, snare, cymbal, hihat, and mod oscillators call
  `osc_resetSamplePlayback()` when their waveform can be an imported sample.
- `calcUserSampleOscBlock()`:
  - zero-fills when `sampleIndex >= sampleMemory_getNumSamples()`;
  - reads `SampleInfo` once per block;
  - rejects zero-size/impossible-offset/inactive samples;
  - wraps looped samples;
  - clears `sampleActive` for one-shots after the final frame.
- `calcUserSampleOscFmBlock()` deliberately delegates to the same long-index
  non-FM reader. This preserves bounds safety without keeping a second,
  historically fragile, user-sample path.

Risk note:

- `calcNextOscSampleBlock()` still has an older outer guard using `>` before it
  calls `calcUserSampleOscBlock()`. The callee has the real `>=` guard and
  zero-fills invalid slots, so this is safe but redundant.

### Phase 3 - Dual Folder Import, Bad Attempt, And Rescue

The original Phase 3 objective was to support one-shot samples in `/samples` and
looped samples in `/loops`.

One attempted Phase 3 implementation caused a serious boot regression:

- Boot showed `Kit Read Error`.
- Buttons did not respond.
- Only encoder and potentiometers responded.

Investigation documents:

- `SAMPLE_PLAN_PHASE_3_AUDIT_INVESTIGATION_1.md`
- `SAMPLE_PLAN_PHASE_3_AUDIT_INVESTIGATION_2.md`

Important investigation findings:

- The broken approach added too much new AVR/STM sample-import state and mixed
  streaming/import semantics into the protocol path.
- It created a fragile relationship between sample import, front-panel UART
  status handling, boot-time kit load, and button responsiveness.
- The final implementation did not preserve that architecture. The salvageable
  idea was only dual-folder import and explicit status reporting; the final code
  keeps import in the existing STM SD/flash path.

Final Phase 3 code lives in:

- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.h`
- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`
- `front/LxrAvr/Menu/menu.h`
- `front/LxrAvr/Menu/menu.c`

Current import behavior:

- AVR `Load: Samples` releases AVR SD SPI with `spi_deInit()`.
- AVR displays `Sample upload / Started`, waits 250 ms so the LCD draw is
  visible, clears the result latch, and sends
  `SAMPLE_CC / SAMPLE_START_UPLOAD / 0`.
- AVR waits by repeatedly calling `uart_checkAndParse()` until the parser sees
  `SAMPLE_UPLOAD_RESULT` or a two-minute timeout expires.
- STM receives `FRONT_SAMPLE_START_UPLOAD`, calls `seq_setRunning(0)`,
  initializes sample/SD handling, imports samples, locks flash, and sends
  `SAMPLE_UPLOAD_RESULT`.
- STM imports `/samples` first as one-shots, then `/loops` as looped samples.
- `/loops` is optional. Missing `/loops` means one-shot-only import, not a
  failure.
- The importer uses `sd_openSampleFolder()` and
  `sd_openNextSampleInFolder()` once per folder instead of repeatedly calling
  indexed `sd_setActiveSample()`.
- `sd_setActiveSample()` remains as a compatibility helper but is not the main
  importer path.

Final fixes from `PHASE_3_CORRECTIONS_PART_2.md`:

- Alternating silent slots were fixed by opening/validating a WAV before
  metadata is created. Invalid, empty, odd-length, too-large, or unreadable
  candidates do not increment `loadedSamples`.
- `/loops` freeze was fixed by replacing raw ACK waiting with parsed
  `SAMPLE_UPLOAD_RESULT` status packets and by checking actual FatFs read
  results.
- Progress packets were added:
  - `SAMPLE_UPLOAD_SAMPLE_PROGRESS` for `/samples`;
  - `SAMPLE_UPLOAD_LOOP_PROGRESS` for `/loops`.
- The LCD now shows:
  - `Sample upload / Started N`;
  - `Loop upload / Started N`.
- The increasing per-sample import time was fixed by replacing the indexed
  rescan path with one-pass folder traversal.

### Protocol Details

Current `SAMPLE_CC` subcommands:

| Direction | Subcommand | Value | Payload |
|---|---|---:|---|
| AVR -> STM | `SAMPLE_START_UPLOAD` / `FRONT_SAMPLE_START_UPLOAD` | `0x01` | ignored |
| AVR -> STM | `SAMPLE_COUNT` / `FRONT_SAMPLE_COUNT` | `0x02` | ignored |
| STM -> AVR | `SAMPLE_COUNT` / `FRONT_SAMPLE_COUNT` | `0x02` | dense sample count |
| STM -> AVR | `SAMPLE_UPLOAD_RESULT` / `FRONT_SAMPLE_UPLOAD_RESULT` | `0x03` | status flags |
| STM -> AVR | `SAMPLE_UPLOAD_SAMPLE_PROGRESS` / `FRONT_SAMPLE_UPLOAD_SAMPLE_PROGRESS` | `0x04` | one-based sample count |
| STM -> AVR | `SAMPLE_UPLOAD_LOOP_PROGRESS` / `FRONT_SAMPLE_UPLOAD_LOOP_PROGRESS` | `0x05` | one-based loop count |

Status flags:

- `SAMPLE_UPLOAD_STATUS_OK = 0x00`
- `SAMPLE_UPLOAD_STATUS_COUNT_LIMIT = 0x01`
- `SAMPLE_UPLOAD_STATUS_FLASH_LIMIT = 0x02`
- `SAMPLE_UPLOAD_STATUS_READ_ERROR = 0x04`

Durable protocol reference updated:

- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`

### Code Comment Preservation

Session 030 closeout added adjacent comments to the current code so the
temporary phase docs can be deleted without losing rationale.

Comment coverage added or expanded around:

- STM internal flash sample-sector bounds and `FLASH_If_WriteSamplePcm()`.
- `SampleInfo` loop-bit packing and 12-byte ABI guard.
- Metadata staging and final committed count write.
- One-pass SD folder iteration and optional `/loops`.
- Minimal WAV `data` chunk scanner risks.
- Dense slot creation after full PCM success.
- Actual bytes-read handling and read-error reporting.
- AVR/STM result/progress packet contract.
- AVR result latch and parsed wait loop.
- LCD progress rendering and 250 ms initial display delay.
- Imported sample playback state in oscillators and trigger reset sites.

## Verified

Builds run during the implementation arc:

```bash
make -C mainboard/LxrStm32 stm32 -j4
make -C front/LxrAvr avr -j4
make firmware
```

Known final build characteristics from the working Phase 3 corrections:

- AVR text/data/bss after the final build were approximately:
  `text 55586`, `data 490`, `bss 3142`.
- STM text/data/bss after the final build were approximately:
  `text 198040`, `data 35812`, `bss 74932`.
- `sampleMemory_importInfo` is `0xBA0` bytes (`248 * 12`) and remains within
  the reserved `0xC00` metadata table.
- The final firmware image and local `tools/bin/FirmwareImageBuilder` binary
  were rebuilt during the work.

Hardware/user verification:

- Phase 1 flash-guard pass tested OK.
- Phase 2 oscillator/sample playback pass tested OK.
- The first Phase 3 attempt was rejected because it caused `Kit Read Error` and
  unresponsive buttons on boot.
- After the correction pass, `/samples` import no longer inserted silent
  between-sample slots.
- `/loops` no longer froze on the `Sample Load` screen.
- Progress display with numbered samples/loops was tested and accepted.
- The one-pass traversal change was left in place after the user asked to leave
  the behavior as-is.

Closeout verification after the comment/docs pass:

- `git diff --check` passed.
- `make -C mainboard/LxrStm32 stm32 -j4` passed with the existing warning set.
- `make -C front/LxrAvr avr -j4` passed with the existing warning set.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.

## Hardware Safety Notes

- The hardware archive says STM SPI pins overlapping the SD bus are normally
  floating because the SD card is owned by the AVR. Current sample import is
  safe only because the AVR releases SPI before issuing `SAMPLE_START_UPLOAD`,
  and the STM deinitializes SPI on every import exit.
- Sample import programs STM32 internal flash. It is intentionally blocking,
  stops the sequencer, and is not appropriate for background audio operation.
- Flash wear is still a real constraint. Repeated sample-import testing erases
  sectors 8..11 each time.
- `findDataChunk()` remains a minimal legacy scanner. It does not fully validate
  RIFF/fmt structure; it finds the first `data` token and returns that chunk
  length. Read failures and zero lengths now fail safely, but stronger WAV
  validation is still future work.

## Known Issues

- No active user-reported sample-import blocker remains after the final
  correction pass.
- The STM SD loader is still a special ownership exception to the normal
  AVR-owned SD-card model. Keep the AVR `spi_deInit()` and STM cleanup calls
  intact.
- The current import UI reports warnings after completion. It does not show a
  final "done" count screen when there are no warnings.
- The older outer user-sample guard in `calcNextOscSampleBlock()` is redundant
  because `calcUserSampleOscBlock()` owns the real bounds check.

## Next Session

Recommended only if sample work continues:

- Add fuller RIFF/WAV validation before `findDataChunk()` accepts a file.
- Consider batching flash programming more efficiently if import time becomes a
  user problem.
- Add a final no-warning "Sample upload done" display if the user wants clearer
  completion feedback.
- Consider making the sample-import SD ownership exception explicit in the
  hardware archive if future hardware documentation work is requested.

## Blockers

None for the current accepted sample-import behavior.

## Critical Reminders

- Do not restore the raw `uart_waitAck()` path for sample import.
- Do not create `SampleInfo` metadata before PCM for that sample has fully
  written.
- Do not use indexed `sd_setActiveSample()` as the main importer loop.
- Do not grow `SampleInfo`; the on-flash ABI is 12 bytes.
- Keep `SAMPLE_MAX_COUNT` at 248 unless the AVR waveform menu/range is audited.
- Keep `/samples` before `/loops` so one-shot sample indices remain stable.
- Keep the STM/AVR opcode definitions synchronized in both receive headers.
- Keep STM code named `frontPanel*` under `uARTFrontSYX/` and AVR code named
  `avrComms*` under `front/LxrAvr/avrComms/`.
