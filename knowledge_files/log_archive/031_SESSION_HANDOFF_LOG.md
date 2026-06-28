# 031 Session Handoff Log - Sample Import Redo And Display Cleanup

DATE: 2026-06-28

## Session Goal

The session started as Phase 4 of the oscillator/sample work: implement a single oscillator waveform interpolation slot controlled later by `PAR_OSC_WAVE_INTERPOLATION`. During testing, the active problem shifted to sample import ordering, import speed, and sample-upload display behavior. After several experimental attempts, the repository was reverted and only the essential sample-import/display additions were reimplemented cleanly from `SAMPLE_LOAD_REDO_AUDIT.md`.

`SAMPLE_LOAD_REDO_AUDIT.md` was folded into this handoff. It can be deleted after this log and `COMMS_FLOW_SPEC.md` are kept.

## Completed

### Early Oscillator Interpolation Work Was Superseded

Early in the session, a plan for the oscillator interpolation slot was written and an initial always-on stage was attempted. Testing then showed the more urgent user-facing failures were in sample import ordering and upload display. The eventual user-directed cleanup reverted the repository and reimplemented only sample-import/display fixes. The durable code state from this closeout does not include the experimental oscillator interpolation work.

Important carry-forward for a future oscillator session:

- The global parameter name is `PAR_OSC_WAVE_INTERPOLATION`, not `PAR_OSC_WAVE_INTERP`.
- Interpolation is intended to be literal waveform-parameter interpolation. Do not invent eligibility filters that exclude samples, noise, or one-shots. If waveform modulation jumps from any waveform id to any other waveform id, the interpolation path should handle that transition.
- The interpolation table can be simple; avoid side quests around waveform categories unless the code actually requires them.

### Sample Import Redo

The final code changes are limited to sample import ordering, upload performance, upload progress/result transport, and the AVR final-write display.

Files changed:

- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`
- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.h`
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h`
- `front/LxrAvr/Menu/menu.c`
- `firmware image/FIRMWARE.BIN`

No `data2 == 0` final progress marker, no separate STM `Writing Flash` signal, no final ACK handshake, no delayed one-file progress queue, and no sample-upload timeout remain in the final redo.

## Detailed Code Changes

### 1. Sorted Cached SD Iterator

Files:

- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`
- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.h`

The STM SD manager now builds a sorted short-name cache per import folder. `sd_openSampleFolder(looped)` selects `/samples` or `/loops`, scans that folder once, inserts short 8.3 WAV names into `sd_sortedSampleNames`, and resets an index. `sd_openNextSampleInFolder()` advances through the cached names instead of continuing a raw FatFs directory walk.

New state:

```c
#define SD_SORTED_SAMPLE_CACHE_MAX ((uint16_t)248u)
static char sd_sortedSampleNames[SD_SORTED_SAMPLE_CACHE_MAX][13];
static uint16_t sd_sortedSampleNameCount = 0u;
static uint16_t sd_sortedSampleNameIndex = 0u;
```

New helpers:

- `sdManager_isDigit(char c)`
- `sdManager_isNameSeparator(char c)`
- `sdManager_copyShortName(char* dst, const char* src)`
- `sdManager_compareSampleNames(const char* a, const char* b)`
- `sdManager_resetSortedSampleCache(void)`
- `sdManager_insertSortedSampleName(const char* name)`
- `sdManager_buildSortedSampleCache(void)`
- `sdManager_openSortedEntryByName(const char* name)`

Sort behavior:

- ASCII-only and case-insensitive.
- Digit runs compare by numeric value, so `2.WAV` sorts before `10.WAV` and `AKWF_HVOICE_0009.WAV` sorts before `AKWF_HVOICE_0104.WAV`.
- When numeric values tie, shorter digit runs sort first as a tie-breaker.
- `_`, `-`, and space are treated as separators. If both names are at separator runs after an equal prefix, those separators are skipped before comparing the next meaningful characters. This keeps `01_cut.wav` before `01-unohit.wav` by comparing `cut` against `unohit`.
- No heap allocation and no locale-dependent `ctype`.

Why this exists:

- FatFs raw directory-entry order was not the requested numerical-alphabetical slot order.
- Earlier iteration approaches could leave the AVR display sitting on the previous progress number during end-of-folder proof.
- End-of-folder is now an index compare, not another scan.
- The cache is STM BSS only, about `248 * 13 = 3224` bytes, and is reused for `/samples` and `/loops`.

Header changes:

- `sd_openSampleFolder()` comments now say it opens and caches short WAV names in sorted order.
- `sd_openNextSampleInFolder()` comments now say it opens by cached numerical-alphabetical 8.3 filename order.

Risk:

- This is still short-name based because current FatFs long filename support is not part of the importer.
- If a folder has more than 248 WAV entries, the cache retains the first 248 sorted names and later sorted entries are ignored; `SampleMemory.c` still sets the count-limit status when total found exceeds `SAMPLE_MAX_COUNT`.

### 2. Immediate Progress And Faster PCM Blocks

File:

- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`

`BLOCKSIZE` changed from `2` to `128`, so the importer reads up to 128 int16 frames per SD read/write chunk instead of two frames. The read loop now computes:

```c
framesRemaining = (len - j) / 2u;
framesToRead = (framesRemaining > BLOCKSIZE)
                  ? BLOCKSIZE
                  : (uint16_t)framesRemaining;
```

The data buffer is zero-filled before each read so any final odd 32-bit flash word has deterministic padding.

Progress is now sent immediately after a file candidate is accepted and assigned its dense one-based display count:

```c
frontPanelSending_sendSampleUploadProgress(looped, progressIndex);
```

That call happens before reading/writing the file payload and before the next `sd_openNextSampleInFolder()` call. This is the key fix for the “last visible number is second-last” bug. The LCD status now describes the file currently being imported, not the previous file that was proven complete after another candidate was found.

The final code explicitly does not use:

- a pending-progress tuple;
- a final pending-progress flush;
- a final synthetic marker;
- a separate STM flash-start packet.

Error/cleanup changes:

- `sampleMemory_loadSamples()` now uses one `sampleMemory_loadSamples_done:` cleanup label after SPI is initialized.
- `FLASH_If_Erase()` failure sets `SAMPLE_UPLOAD_STATUS_FLASH_LIMIT`.
- PCM write failure sets `SAMPLE_UPLOAD_STATUS_FLASH_LIMIT`.
- metadata `FLASH_If_Write()` failure sets `SAMPLE_UPLOAD_STATUS_FLASH_LIMIT`.
- `sampleMemory_setNumSamples()` failure sets `SAMPLE_UPLOAD_STATUS_FLASH_LIMIT`.
- read/short-read/odd-byte failures set `SAMPLE_UPLOAD_STATUS_READ_ERROR`.
- no files found is not treated as hardware failure; it exits through cleanup with OK status.

Metadata/count behavior:

- PCM payload still writes before metadata.
- `SampleInfo` entries are still created only after the PCM payload for that file is fully written.
- The committed sample count is still written last.
- Dense user slots remain `/samples` first, then `/loops`.

Risk:

- The displayed progress now means “accepted and currently importing” rather than “previous file completed.”
- If a payload read/write later fails, the AVR may briefly show that file’s number before the final error screen. That is intentional; the file was the active import candidate.

### 3. Reliable STM Progress/Result Packets

Files:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h`

The sample progress helper remains simple:

```c
void frontPanelSending_sendSampleUploadProgress(uint8_t looped, uint8_t index);
```

No `finalProgress` argument was added.

Progress packets now use `frontPanelSending_sendPriorityTripletWait()`:

```c
uint8_t progressOpcode = looped ? FRONT_SAMPLE_UPLOAD_LOOP_PROGRESS :
                                  FRONT_SAMPLE_UPLOAD_SAMPLE_PROGRESS;
frontPanelSending_sendPriorityTripletWait(SAMPLE_CC, progressOpcode, index);
```

The result packet also uses `frontPanelSending_sendPriorityTripletWait()`:

```c
frontPanelSending_sendPriorityTripletWait(
   SAMPLE_CC,
   FRONT_SAMPLE_UPLOAD_RESULT,
   (uint8_t)(statusFlags & (SAMPLE_UPLOAD_STATUS_COUNT_LIMIT |
                            SAMPLE_UPLOAD_STATUS_FLASH_LIMIT |
                            SAMPLE_UPLOAD_STATUS_READ_ERROR)));
```

Why this exists:

- Sample progress and final result are sparse, user-facing packet triples.
- Dropping one byte can leave the AVR on an older progress number.
- The AVR wait loop no longer has a timeout; the result packet is the only completion signal.

Protocol payloads:

- `SAMPLE_UPLOAD_SAMPLE_PROGRESS` / `FRONT_SAMPLE_UPLOAD_SAMPLE_PROGRESS` (`0x04`) sends a real one-based `/samples` progress number.
- `SAMPLE_UPLOAD_LOOP_PROGRESS` / `FRONT_SAMPLE_UPLOAD_LOOP_PROGRESS` (`0x05`) sends a real one-based `/loops` progress number.
- `SAMPLE_UPLOAD_RESULT` / `FRONT_SAMPLE_UPLOAD_RESULT` (`0x03`) sends status flags.
- There is no `data2 == 0` final marker in the current protocol.

### 4. AVR Wait Loop And Local `Writing Flash` Spinner

File:

- `front/LxrAvr/Menu/menu.c`

The old fixed upload timeout and `Load timeout` screen were removed.

New constants:

```c
#define MENU_SAMPLE_FLASH_ANIM_TICKS ((uint16_t)12u)
#define MENU_SAMPLE_FLASH_IDLE_TICKS ((uint16_t)153u)
```

Timer2 ticks at roughly 13.1 ms, so `153` ticks is about two seconds.

New AVR-local state:

```c
static uint8_t menu_sampleFlashProgressScreenActive = 0u;
static uint8_t menu_sampleFlashProgressAnimate = 0u;
static uint8_t menu_sampleFlashProgressPhase = 0u;
static uint16_t menu_sampleFlashProgressLastTick = 0u;
static uint8_t menu_sampleUploadProgressSeen = 0u;
static uint16_t menu_sampleUploadLastProgressTick = 0u;
```

New private helpers:

- `menu_drawSampleUploadFlashDots(uint8_t phase)`
- `menu_showSampleUploadFlashProgress(uint8_t phase)`
- `menu_tickSampleUploadFlashProgress(void)`
- `menu_maybeShowSampleUploadFlashIdle(void)`

`menu_showSampleUploadProgress(looped, index)` now:

- resets any final-flash spinner;
- records `menu_sampleUploadLastProgressTick = time_sysTick`;
- arms the idle fallback only when `index != 0`;
- draws the existing `Sample upload` / `Loop upload` and `Started <index>` screen.

`menu_waitSampleUploadResult()` now loops until `avrComms_sampleUploadDone`:

```c
while(1)
{
   uart_checkAndParse();
   menu_maybeShowSampleUploadFlashIdle();
   menu_tickSampleUploadFlashProgress();

   if(avrComms_sampleUploadDone)
   {
      menu_sampleFlashProgressAnimate = 0u;
      menu_sampleUploadProgressSeen = 0u;
      *statusFlags = avrComms_sampleUploadStatus;
      return;
   }
}
```

If numbered progress has started and no further numbered progress arrives for about two seconds while waiting for `SAMPLE_UPLOAD_RESULT`, the AVR switches display-only state to:

```text
Writing Flash
...
```

Dots animate locally until the result arrives.

Risk:

- A very large single file could show `Writing Flash` while the STM is still writing that file’s PCM blocks, because the fallback is progress-idle based. This is acceptable: the STM is writing flash, the UI is alive, and any later numbered progress packet redraws the normal upload status.
- Completion still depends only on `SAMPLE_UPLOAD_RESULT`.

### 5. AVR Receive Path

File checked:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

No final-marker handling was added.

Current behavior remains direct:

```c
case SAMPLE_UPLOAD_SAMPLE_PROGRESS:
   menu_showSampleUploadProgress(0u, avrCommsParser_command.data2);
   break;

case SAMPLE_UPLOAD_LOOP_PROGRESS:
   menu_showSampleUploadProgress(1u, avrCommsParser_command.data2);
   break;
```

`SAMPLE_UPLOAD_RESULT` still stores status before setting `avrComms_sampleUploadDone`.

## Verified

Build commands run:

```sh
make -C mainboard/LxrStm32 stm32
make -C front/LxrAvr avr
make firmware
```

Result:

- STM build passed.
- AVR build passed.
- `firmware image/FIRMWARE.BIN` rebuilt.
- `git diff --check` passed for touched files.
- Existing project warnings remain, including STM mixer/oscillator inline warnings, sequencer array-size warnings, AVR IO-register warnings, fallthrough warnings, and the existing `menu_resetActiveParameter` unsigned mask warning.

Hardware/user testing during the session:

- The user confirmed the final clean redo seemed OK before session close.
- The final expected visible behavior is that a card with 104 loop files reaches `Loop upload Started 103`, then after about two seconds of no further progress displays `Writing Flash` with local dots until `SAMPLE_UPLOAD_RESULT`.

## Comms Spec Updates

`knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md` was updated for Session 031:

- status/date now reference sorted cached sample import;
- sample progress/result packets are documented as reliable priority-wait triples;
- progress payloads are documented as real one-based numbers only;
- no `data2 == 0` final marker;
- no fixed AVR timeout;
- AVR-local `Writing Flash` idle display is documented;
- sorted cached short-name iteration is documented;
- guardrails now say not to restore raw ACK waits, timeout screens, delayed progress, or synthetic final markers.

Checked but did not update:

- `knowledge_files/comms_spec_reference/BACKGROUND_LOAD_TEMPORARY.md` - no sample-import content affected.
- `knowledge_files/comms_spec_reference/MIDI_TABLE.md` - no MIDI behavior changed.

## Working Repository Status

Working repository at close:

```text
/Users/bc/LXR01/LXR-current/LXR
branch: dev-samples
```

Relevant modified files:

- `firmware image/FIRMWARE.BIN`
- `front/LxrAvr/Menu/menu.c`
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`
- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.h`
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h`

Other working tree items present at close, not part of the final sample redo:

- `.DS_Store` modified.
- `SAMPLE_PLAN_PHASE_4.md` deleted in the working tree.
- `SAMPLE_LOAD_REDO_AUDIT.md` untracked and now superseded by this handoff.
- `SD_CARD/` untracked test-card snapshot.
- `interp_ref_files/` untracked reference directory.

## Known Issues Introduced

None known from the final sample redo.

The AVR-local `Writing Flash` fallback is intentionally idle-based, not an STM semantic phase signal. It is a user feedback mechanism while waiting for `SAMPLE_UPLOAD_RESULT`.

## Known Issues Resolved

- Sample import order no longer follows raw FAT directory-entry order.
- `01_cut.wav` should sort before `01-unohit.wav`.
- Numbered sets such as `AKWF_hvoice_0001.wav` through `AKWF_hvoice_0104.wav` sort numerically.
- The final loop/sample number is sent at the start of the current file import, not held until after the next file is discovered.
- The AVR no longer shows a `Load timeout` while STM is still working.
- The old long silent final wait now transitions to AVR-local `Writing Flash` dots.
- PCM import uses larger bounded blocks instead of tiny two-frame transfers.

## Next Session Recommended Goal

Return to the oscillator interpolation work as a clean session, using the clarified rule: waveform-parameter interpolation applies across the full waveform domain, including samples, one-shots, loops, noise, and base oscillator waveforms. Use `PAR_OSC_WAVE_INTERPOLATION` as the parameter name.

## Blockers

- Hardware retest is still the real confirmation for final SD-card ordering and upload display on the user’s actual card contents.
- The oscillator interpolation work should start from the current post-redo code state, not from the discarded experimental attempts.

## Critical Reminders For Next Session

- Do not reintroduce delayed sample progress, final progress markers, or sample-upload timeouts.
- Do not use `uart_waitAck()` for sample import completion; AVR must parse until `SAMPLE_UPLOAD_RESULT`.
- Keep sample progress payloads as real one-based numbers only.
- Keep `frontPanelSending_sendSampleUploadProgress(uint8_t looped, uint8_t index)` two-argument and simple.
- Keep AVR `Writing Flash` as display-only idle fallback unless a later design explicitly changes the protocol.
- `SAMPLE_LOAD_REDO_AUDIT.md` can be deleted after this handoff is preserved.
- `PAR_OSC_WAVE_INTERPOLATION` is the correct interpolation toggle parameter name.

