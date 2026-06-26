# PHASE_3_CORRECTIONS_PART_2

## Scope

This document covers the two remaining Phase 3 sample-import defects reported
after the Phase 3 correction attempt:

1. With only a `SAMPLES` folder, import completes, but silent slots are inserted
   between playable samples. The observed result is that `s0`, `s2`, `s4`, etc.
   play, while `s1`, `s3`, etc. are silent.
2. With a `LOOPS` folder present, the front panel remains on the sample-load
   screen and the operation never finishes.

This document was originally written as a correction plan. The implementation
notes at the end record the source changes made from that plan.

## Current Code State Audited

Current branch:

```text
12396c2 Phase 3 correction attempt: max 248 samples, warning screens for too many samples and too large sample size
```

Relevant current files:

- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`
- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`
- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`
- `front/LxrAvr/Menu/menu.c`
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`

Important audit findings:

- `sampleMemory_importInfo` is `0xBA0` bytes in the current STM build, which is
  exactly `248 * 12`. The current compiler is therefore using 12-byte
  `SampleInfo` entries.
- `sampleMemory_getSampleInfo()` also indexes the flash table with a 12-byte
  stride in the generated STM code.
- The alternating silent-slot symptom is therefore unlikely to be fixed by
  merely changing `sizeof(SampleInfo)` comments. The importer must be made to
  write a dense metadata table only for files that successfully open, expose a
  valid non-zero PCM data chunk, and read successfully.
- The AVR still waits for sample-load completion with `uart_waitAck()`, but the
  STM now sends `SAMPLE_CC, FRONT_SAMPLE_UPLOAD_RESULT, statusFlags`. That is a
  protocol mismatch and can desynchronize the AVR parser.
- `sampleMemory_loadSamples()` ignores the byte count returned by
  `sd_readSampleData()`. A loop file with a bad chunk length, read failure, or
  unexpected EOF can keep the STM busy writing stale buffer contents until the
  claimed length is exhausted.

## Correction 1: Remove Silent Interleaved Sample Slots

### Working Diagnosis

The current importer creates a metadata entry after `sd_setActiveSample(i)`
without proving that the selected candidate is a playable sample:

```c
sd_setActiveSample(i);
len = sd_getActiveSampleLength();
name = sd_getActiveSampleName();

info[loadedSamples] = sampleMemory_makeSampleInfo(name, len / 2u, ...);
...
loadedSamples++;
```

If `sd_setActiveSample()` fails to open the file, cannot find a valid `data`
chunk, or returns a zero-length chunk, the importer can still reserve a slot.
That slot has `size == 0` or otherwise invalid metadata, so playback correctly
returns silence. If the SD directory enumeration alternates between valid files
and invalid `.WAV` candidates, the visible result is exactly:

```text
s0 playable
s1 silent
s2 playable
s3 silent
...
```

Do not fix this by remapping even indexes. The metadata table must be dense:
only successfully imported samples should consume visible sample indices.

### Required Code Changes

#### 1. Change `sd_setActiveSample()` To Return Success

Target: `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.h`

Change:

```c
void sd_setActiveSample(uint16_t sampleNr);
```

to:

```c
uint8_t sd_setActiveSample(uint16_t sampleNr);
```

Why:

- The importer needs to know whether the selected candidate actually opened and
  produced a usable data chunk.
- A return value keeps file-selection failure from being smuggled into the
  metadata table as a silent sample slot.

Target: `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`

At the start of `sd_setActiveSample()`:

```c
sd_currentSampleLength = 0;
memset(sd_currentSampleName, 0, sizeof(sd_currentSampleName));
```

Why:

- A failed candidate must not reuse the previous file's length or name.
- This makes failure state explicit and prevents stale metadata.

Return `0` on every failure path:

```c
if(res != FR_OK)
{
   return 0;
}
```

Return `0` if `f_open()` fails.

Return `0` if `findDataChunk()` returns `0`.

Return `1` only after:

```c
sd_currentSampleLength = findDataChunk();
if(sd_currentSampleLength == 0u)
{
   return 0;
}
return 1;
```

Why:

- A zero-byte or unrecognized WAV data chunk is not an importable sample.
- Only importable samples should advance `loadedSamples`.

#### 2. Make WAV Matching A Helper

Target: `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`

Add a private helper:

```c
static uint8_t sdManager_isWavFile(const FILINFO* fno);
```

It should:

- Reject directories.
- Reject dot entries.
- Match `.WAV` case-insensitively.
- Return exactly one result per directory entry.

Why:

- The current extension scan is duplicated in `/samples`, `/loops`, and
  `sd_setActiveSample()`.
- Duplicated file filters can cause count/select mismatches. A file counted in
  one path must be selectable by the other path using the same rule.

Risk:

- FatFs is configured with `_USE_LFN == 0`, so only 8.3 `fname` should be used.
  Do not add long-filename stack buffers.

#### 3. Only Create Metadata After Validation

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`

Change the import loop from:

```c
sd_setActiveSample(i);
len = sd_getActiveSampleLength();
name = sd_getActiveSampleName();
...
info[loadedSamples] = sampleMemory_makeSampleInfo(...);
...
loadedSamples++;
```

to this structure:

```c
if(!sd_setActiveSample(i))
{
   continue;
}

len = sd_getActiveSampleLength();
if(len == 0u)
{
   continue;
}

name = sd_getActiveSampleName();
```

Then perform flash-capacity checks.

Only after those checks pass, write PCM and then commit the metadata entry for
that same `loadedSamples` index.

Why:

- Failed or zero-length candidates are skipped instead of assigned visible
  waveform slots.
- `loadedSamples` becomes the count of playable entries, not merely the count of
  directory candidates visited.

#### 4. Treat Zero-Length And Odd-Length PCM As Invalid Or Explicitly Padded

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`

At minimum:

```c
if(len < 2u)
{
   continue;
}
```

Preferred:

- Accept odd byte counts only if the final byte is explicitly padded to a
  16-bit frame.
- Otherwise skip odd-length chunks as malformed.

Why:

- Playback reads `int16_t` frames.
- A one-byte or odd-byte sample chunk cannot be represented safely without a
  padding policy.

#### 5. Make The Metadata Format Explicit

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`

Add:

```c
#define SAMPLE_INFO_FLASH_WORDS ((uint32_t)3u)
#define SAMPLE_INFO_FLASH_BYTES (SAMPLE_INFO_FLASH_WORDS * 4u)
```

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`

Add a compile-time size check:

```c
typedef char sample_info_size_must_be_12_bytes[(sizeof(SampleInfo) == SAMPLE_INFO_FLASH_BYTES) ? 1 : -1];
```

Use `SAMPLE_INFO_FLASH_BYTES` for metadata capacity math instead of repeating
`sizeof(SampleInfo)` assumptions in comments.

Why:

- Current compiler output already uses a 12-byte stride, but making that a hard
  invariant prevents future `-fpack-struct` or toolchain changes from silently
  reintroducing sparse/shifted metadata.

Risk:

- This is a build-time guard. It should fail loudly if the metadata ABI changes.

## Correction 2: Fix `LOOPS` Folder Freeze

### Working Diagnosis

There are two independent freeze risks.

First, the AVR waits with the wrong protocol primitive:

```c
uint8_t ret = uart_waitAck();
```

But the STM sends:

```c
SAMPLE_CC, FRONT_SAMPLE_UPLOAD_RESULT, statusFlags
```

`uart_waitAck()` returns the first raw byte it sees and does not preserve parser
alignment. It also has no timeout.

Second, the STM import loop ignores `sd_readSampleData()`'s return value:

```c
sd_readSampleData(data, BLOCKSIZE);
...
j += BLOCKSIZE * 2u;
```

If a loop file has a bad data length, an unexpected EOF, or a read error, the
loader does not know. It keeps advancing by the requested block size and writes
whatever remains in `data`.

### Required Code Changes

#### 1. Add AVR-Side Upload Result Constants

Target: `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`

Add:

```c
#define SAMPLE_UPLOAD_RESULT       0x03
#define SAMPLE_UPLOAD_STATUS_OK    0x00
#define SAMPLE_UPLOAD_STATUS_COUNT_LIMIT 0x01
#define SAMPLE_UPLOAD_STATUS_FLASH_LIMIT 0x02
```

Why:

- The AVR must understand the result packet the STM already sends.
- `0x03` is a packet `data1` value under `SAMPLE_CC`, not a new status byte.

#### 2. Parse And Store Sample Upload Result On AVR

Target: `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

Add module state:

```c
volatile uint8_t avrComms_sampleUploadDone;
volatile uint8_t avrComms_sampleUploadStatus;
```

In the `SAMPLE_CC` switch, add:

```c
case SAMPLE_UPLOAD_RESULT:
   avrComms_sampleUploadStatus = avrCommsParser_command.data2;
   avrComms_sampleUploadDone = 1u;
   break;
```

Why:

- The menu should wait for a parsed semantic result, not consume raw transport
  bytes.
- This preserves UART parser alignment for the following `SAMPLE_COUNT` query.

#### 3. Replace `uart_waitAck()` In Sample Load Menu

Target: `front/LxrAvr/Menu/menu.c`

Replace the sample-load branch's ACK wait with a helper shaped like:

```c
static uint8_t menu_waitSampleUploadResult(uint8_t* statusFlags);
```

The helper should:

- Clear `avrComms_sampleUploadDone` before sending `SAMPLE_START_UPLOAD`.
- Repeatedly call `uart_checkAndParse()`.
- Return success when `avrComms_sampleUploadDone != 0`.
- Include a long timeout based on `time_sysTick`.
- Return failure on timeout.

Why:

- It lets the normal parser receive `SAMPLE_UPLOAD_RESULT`.
- It prevents an endless front-panel wait if the STM never sends completion.

Timeout guidance:

- Do not use a short UI timeout. Sample flashing can legitimately take many
  seconds.
- Start with a conservative timeout such as 120 seconds, then tune after
  hardware timing measurements.

After receiving the result:

```c
if(statusFlags & SAMPLE_UPLOAD_STATUS_COUNT_LIMIT)
{
   display "248 max exceeded" for about one second.
}

if(statusFlags & SAMPLE_UPLOAD_STATUS_FLASH_LIMIT)
{
   display "Flash exceeded" for about one second.
}
```

Then send:

```c
avrComms_sendData(SAMPLE_CC, SAMPLE_COUNT, 0x00);
```

Why:

- Warnings are not fatal import failures.
- `SAMPLE_COUNT` refreshes `menu_numSamples` after the import result has been
  parsed cleanly.

#### 4. Use `sd_readSampleData()` Return Value

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`

Change the read/write loop to track actual bytes:

```c
uint16_t bytesRead = sd_readSampleData(data, BLOCKSIZE);
if(bytesRead == 0u)
{
   statusFlags |= SAMPLE_UPLOAD_STATUS_READ_ERROR;
   break_or_abort;
}
```

Because no `SAMPLE_UPLOAD_STATUS_READ_ERROR` exists yet, choose one:

```c
#define SAMPLE_UPLOAD_STATUS_READ_ERROR ((uint8_t)0x04u)
```

Add the same flag to the AVR header and warning handling.

Why:

- A read failure should not look like a successful import.
- This is the most likely STM-side reason a `LOOPS` import can appear to hang.

Implementation policy:

- If a read error occurs after flash has already been erased, abort the import
  and report read error.
- Do not commit a partial metadata table after a mid-file read error unless the
  UI explicitly reports a partial import.

#### 5. Advance PCM Loop By Actual Bytes Read

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`

Replace:

```c
j += BLOCKSIZE * 2u;
addr += BLOCKSIZE / 2u;
```

with logic based on `bytesRead`.

For the current `BLOCKSIZE == 2`:

- `bytesRead == 4`: write one 32-bit word and advance one word.
- `bytesRead == 2`: write one padded 32-bit word containing one final int16
  frame and advance one word.
- Any other odd byte count: treat as malformed/read error.

Why:

- The importer writes flash in 32-bit words, but WAV PCM is counted in bytes.
- The loop must not pretend it read four bytes when FatFs returned fewer.

#### 6. Add A File Close Or Reset Hook

Target: `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`

Add:

```c
void sd_closeActiveSample(void)
{
   f_close(&sd_File);
}
```

Call it after each imported or skipped file where `f_open()` succeeded.

Why:

- The loader reuses a single global `FIL`.
- Closing between `/samples` and `/loops` files reduces FatFs state risk,
  especially with `_FS_TINY == 1`.

Risk:

- `f_close()` should be available even in read-only FatFs configurations. If it
  is unavailable in this minimized build, explicitly reset `sd_File` only after
  confirming FatFs permits it.

#### 7. Harden Loop Folder Path Building

Target: `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`

Current path construction uses a fixed `char filename[22]` and manual copies.
Replace it with bounds-checked construction:

```c
char filename[24];
uint8_t prefixLen = strlen(folder);

if(prefixLen + 1u + 12u >= sizeof(filename))
{
   return 0;
}
```

Then copy:

```c
memset(filename, 0, sizeof(filename));
memcpy(filename, folder, prefixLen);
filename[prefixLen] = '/';
memcpy(&filename[prefixLen + 1u], fn, 12u);
```

Why:

- `/samples/XXXXXXXX.WAV` and `/loops/XXXXXXXX.WAV` fit, but the code should
  prove that before copying.
- This avoids path buffer fragility when changing folder names or FatFs name
  formatting.

## Hardware Safety Notes

- Keep all flash writes bounded below `FLASH_IF_SAMPLE_INFO_START_ADDRESS`.
- Keep metadata writes bounded between `SAMPLE_INFO_START_ADDRESS` and the end
  of the sample flash region.
- Continue erasing only STM32F4 sectors 8 through 11 for sample import.
- Do not add writes to SD from the STM. The current loader reads SD and writes
  internal flash only.
- Do not touch codec, GPIO, encoder, button, or DAC code for these two fixes.
- Do not disable interrupts during long flash or SD loops beyond what the
  existing flash driver already does internally.

## Validation Plan

### Build Checks

Run:

```sh
make -C mainboard/LxrStm32 clean
make -C front/LxrAvr clean
make -C mainboard/LxrStm32 stm32 -j4
make -C front/LxrAvr avr -j4
make firmware
git diff --check
```

Confirm:

- No new compiler warnings in touched files.
- `SAMPLE_INFO_FLASH_BYTES == 12`.
- `sampleMemory_importInfo` remains `248 * 12 == 0xBA0` bytes.

### Hardware Test Matrix

1. Empty SD sample folders:
   - No freeze.
   - Sample count becomes zero.

2. `SAMPLES` only, three valid WAV files:
   - Visible entries are `s0`, `s1`, `s2`.
   - All three play.
   - There are no silent entries between them.

3. `SAMPLES` with invalid WAV-like files interleaved:
   - Invalid candidates are skipped.
   - Valid samples are dense: `s0`, `s1`, `s2`, not `s0`, `s2`, `s4`.

4. `LOOPS` only:
   - Load completes.
   - Entries are tagged looped.
   - Playback loops instead of one-shot stopping.

5. `SAMPLES` plus `LOOPS`:
   - Samples appear first.
   - Loops appear after samples.
   - No freeze.

6. Oversized combined sample set:
   - Import completes with `Flash exceeded`.
   - Playable imported entries are dense.

7. More than 248 candidates:
   - Import completes with `248 max exceeded`.
   - Count reported to AVR is at most 248.

8. Malformed loop file with bogus data length:
   - Import times out or reports read error.
   - Front panel leaves the load screen instead of hanging forever.

## Do Not Do

- Do not solve the alternating slots by dividing the selected sample index by
  two in the oscillator. That would hide bad metadata and break real sample
  ordering.
- Do not keep `uart_waitAck()` in the sample-load path. It is incompatible with
  the current STM result packet.
- Do not commit metadata for zero-length files.
- Do not ignore `sd_readSampleData()` return values.
- Do not increase `SAMPLE_MAX_COUNT` above 248 without a separate audit of
  AVR menu value ranges and waveform labels.

## Implementation Notes

Implemented in this pass:

- `sd_setActiveSample()` now returns success only after it opens a candidate and
  finds a non-empty WAV data chunk.
- `/samples`, `/loops`, and active-file selection now share one `.WAV` matcher.
- STM sample import skips failed or invalid candidates before allocating a
  visible sample slot.
- STM sample import reads and advances by actual bytes read, pads final single
  int16 frames into one flash word, and reports `SAMPLE_UPLOAD_STATUS_READ_ERROR`
  on malformed or short reads.
- AVR sample load now waits for parsed `SAMPLE_UPLOAD_RESULT` instead of raw
  `uart_waitAck()`.
- AVR displays `248 max exceeded`, `Flash exceeded`, `Read error`, or
  `Load timeout` from the structured result path.
- `firmware image/FIRMWARE.BIN` was regenerated after successful AVR and STM
  builds.

QOL progress update pass:

- Added STM-to-AVR progress packets for accepted files:
  `SAMPLE_UPLOAD_SAMPLE_PROGRESS` reports a 1-based `/samples` import number,
  and `SAMPLE_UPLOAD_LOOP_PROGRESS` reports a 1-based `/loops` import number.
- The STM sends the progress packet after the file is validated and capacity
  checked, immediately before PCM flash writes begin. This avoids showing
  skipped malformed files as visible sample slots.
- The AVR parser repaints the load screen on those packets:
  `Sample upload` / `Started N` for ordinary samples, and
  `Loop upload` / `Started N` for loops.
- The initial sample-load screen now uses the same display helper and waits
  briefly before sending `SAMPLE_START_UPLOAD`, giving the LCD time to settle so
  the full `Sample upload` text is visible before flash work begins.

Single-pass traversal follow-up:

- Hardware testing showed each later sample took longer to start importing than
  the one before it.
- The cause is the indexed selector path: `sd_setActiveSample(i)` reopens the
  folder and scans from the beginning every time. This makes selection cost grow
  as `1 + 2 + 3 ... N` directory entries.
- The correction is to walk `/samples` once, importing each valid WAV as it is
  encountered, then walk `/loops` once. This preserves the existing samples-first
  ordering while making directory traversal linear.
- The iterator must keep the same validation rule as the indexed selector:
  skipped or malformed files must not consume a visible sample slot, and loop
  progress numbers should count only accepted loop files.
- Implemented by adding `sd_openSampleFolder()` and
  `sd_openNextSampleInFolder()` in `SD_Manager`, then changing
  `sampleMemory_loadSamples()` to import all accepted `/samples` entries in one
  directory pass before opening `/loops` and importing accepted loop entries in
  one second pass.
- The old `sd_setActiveSample()` indexed selector remains for compatibility,
  but the sample-import path no longer uses it.
- `firmware image/FIRMWARE.BIN` was regenerated after the single-pass traversal
  build.

Verification:

- `make -C mainboard/LxrStm32 stm32 -j4`
- `make -C front/LxrAvr avr -j4`
- `make firmware`
- `git diff --check`
- `sampleMemory_importInfo` remains `0xBA0` bytes, matching `248 * 12`.
