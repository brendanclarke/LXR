# SAMPLE_PLAN_PHASE_3: Dual-Folder Sample Import (`/SAMPLES` + `/LOOPS`)

## Scope

This phase implements the minimal changes required to read two SD card directories during sample import — `/samples` (one-shot, as today) and `/loops` (looped) — and tag the resulting `SampleInfo` metadata so the Phase 2 oscillator code can distinguish looped from one-shot samples at playback time.

Relevant current code (post Phase 1 + Phase 2):

- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`: SD directory scanner and per-sample reader. Currently hardcoded to scan only `/samples`.
- `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.h`: Public API for sample count, selection, and reading.
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`: Flash writer that iterates over `sd_getNumSamples()`, builds `SampleInfo[]`, and commits PCM + metadata to internal flash.
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`: `SAMPLE_INFO_LOOP_FLAG`, `SAMPLE_INFO_SIZE_MASK`, `SAMPLE_MAX_COUNT`, `sampleMemory_makeSampleInfo()`.
- `mainboard/LxrStm32/src/DSPAudio/Oscillator.c`: `calcUserSampleOscBlock()` already unpacks `looped` from `SampleInfo.size` and uses the loop/one-shot branch (Phase 2 work).
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`: `FRONT_SAMPLE_START_UPLOAD` handler calls `sampleMemory_loadSamples()`.
- `front/LxrAvr/Menu/menu.c`: AVR user-facing "Sample upload" action that sends `SAMPLE_CC, SAMPLE_START_UPLOAD, 0x00` to STM.

## Design Principles

1. **Minimal diff**: Change as few files as possible. The oscillator already consumes the loop flag — this phase only needs to produce it.
2. **No new AVR-to-STM opcodes**: The AVR sends `SAMPLE_START_UPLOAD` exactly as before; the STM decides what folders to scan.
3. **No protocol changes**: The post-upload sample count reply already works via `FRONT_SAMPLE_COUNT`. No new handshake needed.
4. **Preserve existing behavior**: If `/loops` is missing or empty, import behaves exactly as it does today.
5. **Flash layout unchanged**: Samples from both folders are written into one contiguous region (`SAMPLE_ROM_START_ADDRESS` to `SAMPLE_INFO_START_ADDRESS`). Metadata is one flat `SampleInfo[SAMPLE_MAX_COUNT]` table. The only difference is that entries originating from `/loops` have bit 31 set in `SampleInfo.size`.

---

## Architecture Overview

```
Current flow:
  AVR menu "Sample upload"
    → SAMPLE_CC / SAMPLE_START_UPLOAD → STM
    → sampleMemory_init()
    → sampleMemory_loadSamples()
        → sdManager_init()         [scans /samples, counts .WAV files]
        → sd_getNumSamples()       [returns count from /samples only]
        → for each sample:
            sd_setActiveSample(i)   [opens i-th .WAV in /samples]
            sd_readSampleData(...)  [reads PCM]
            → FLASH_If_WriteSamplePcm(...)
        → write SampleInfo[] metadata (all with looped=0)
        → write final sample count

Proposed flow:
  AVR menu "Sample upload"
    → SAMPLE_CC / SAMPLE_START_UPLOAD → STM
    → sampleMemory_init()
    → sampleMemory_loadSamples()
        → sdManager_init()                  [scans /samples, counts .WAV files]
        → sdManager_countLoopFolder()       [NEW: scans /loops, counts .WAV files]
        → sd_getNumSamples()                [returns COMBINED count]
        → sd_getNumOneShotSamples()         [NEW: returns /samples count only]
        → for each sample:
            sd_setActiveSample(i)           [opens correct file from correct folder]
            sd_readSampleData(...)          [reads PCM]
            → FLASH_If_WriteSamplePcm(...)
        → write SampleInfo[] metadata
            (with looped=1 for entries from /loops)
        → write final sample count
```

---

## Exact Code Changes

### 1. Add loop folder count state variables in `SD_Manager.c`

**Target**: `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`

**Add after the existing `sd_foundSampleFiles` declaration (line 50):**

```c
/* ---- existing ---- */
uint8_t sd_foundSampleFiles = 0;       /* count from /samples */

/* ---- new ---- */
static uint8_t sd_foundLoopFiles = 0;  /* count from /loops   */
```

**Why**: We need a separate counter so we know how many files came from `/samples` vs `/loops`. `sd_foundSampleFiles` remains unchanged in meaning — it still counts only `/samples`. The new `sd_foundLoopFiles` is `static` because only `SD_Manager.c` needs the breakdown; external callers get the combined count.

**Risk**: None. Adding a static variable does not affect any existing interface. RAM cost is 1 byte.

---

### 2. Add the `/loops` scanning function in `SD_Manager.c`

**Target**: `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`

**Add after `sdManager_init()` (after line 105):**

```c
//---------------------------------------------------------------
/*
 * sdManager_countLoopFolder()
 *
 * Scans the /loops directory on the SD card and counts .WAV files,
 * storing the result in sd_foundLoopFiles. This function mirrors the
 * directory-scanning logic already used in sdManager_init() for /samples.
 *
 * Called by sampleMemory_loadSamples() after sdManager_init().
 * If /loops does not exist, sd_foundLoopFiles is left at 0 and
 * import proceeds with /samples only (backward compatible).
 *
 * Impacts: Only writes sd_foundLoopFiles. Does not affect sd_foundSampleFiles.
 * Risk: If FatFs returns FR_NO_PATH for a missing /loops directory, the
 *        function returns gracefully. Other FR_* errors are also non-fatal.
 */
void sdManager_countLoopFolder(void)
{
   FRESULT res;
   FILINFO fno;
   char *fn;

   sd_foundLoopFiles = 0;

   res = f_opendir(&sd_Directory, "/loops");
   if(res != FR_OK)
   {
      /* /loops directory missing or unreadable — not an error.
       * Import will proceed with /samples only. */
      return;
   }

   while(1)
   {
      res = f_readdir(&sd_Directory, &fno);
      if(res != FR_OK || fno.fname[0] == 0) break;
      if(fno.fname[0] == '.') continue;

      fn = fno.fname;

      if(fno.fattrib & AM_DIR)
      {
         continue;
      }
      else
      {
         int i;
         for(i = 0; i < 12 - 3; i++)
         {
            if(fn[i] == '.')
            {
               if((fn[i+1] == 'W') && (fn[i+2] == 'A') && (fn[i+3] == 'V'))
               {
                  sd_foundLoopFiles++;
               }
            }
         }
      }
   }
}
```

**Why**: This is a near-copy of the counting loop in `sdManager_init()` but targeting `/loops`. Keeping it as a separate function avoids complicating the existing init path, and the behavior is easy to audit: it either finds `.WAV` files in `/loops` and counts them, or silently returns 0 if the directory does not exist.

**Functions impacted**: None existing. This is a new function only called from `sampleMemory_loadSamples()`.

**Risk**: `f_opendir` reuses the global `sd_Directory`. This is safe because `sdManager_init()` has already finished using `sd_Directory` for `/samples` counting before this function is called. Both functions are called sequentially in the same blocking import, never concurrently.

---

### 3. Modify `sd_getNumSamples()` to return the combined count

**Target**: `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`

**Change the existing function (line 108–111):**

```c
/* ---- current ---- */
uint8_t sd_getNumSamples()
{
   return sd_foundSampleFiles;
}

/* ---- new ---- */
/*
 * sd_getNumSamples()
 *
 * Returns the total number of .WAV files found across both /samples and
 * /loops directories. This is the count used by sampleMemory_loadSamples()
 * to size the metadata table and iteration.
 *
 * Post-Phase-3, this returns (sd_foundSampleFiles + sd_foundLoopFiles).
 * If /loops was missing or empty, sd_foundLoopFiles is 0, so the return
 * value is identical to the pre-Phase-3 behavior.
 *
 * Risk: The sum could theoretically exceed SAMPLE_MAX_COUNT (50). The caller
 *        (sampleMemory_loadSamples) already clamps numSamples > SAMPLE_MAX_COUNT
 *        and aborts the import, so overflow is handled externally.
 */
uint8_t sd_getNumSamples()
{
   uint16_t total = (uint16_t)sd_foundSampleFiles + (uint16_t)sd_foundLoopFiles;

   /* Clamp to uint8_t range. SAMPLE_MAX_COUNT enforcement happens in the caller. */
   if(total > 255u) total = 255u;

   return (uint8_t)total;
}
```

**Why**: `sampleMemory_loadSamples()` calls `sd_getNumSamples()` to get the iteration limit and the final count to commit. It already rejects `numSamples > SAMPLE_MAX_COUNT`, so the combined count flows naturally. The `uint16_t` intermediate prevents `uint8_t` wrapping if someone puts 200+ files across both directories.

**Functions impacted**: `sampleMemory_loadSamples()` — receives the new combined count. Behavior is unchanged if `/loops` is empty.

**Risk**: If the user has exactly 50 files across both folders, all 50 are imported. If they have 51, the import is rejected by the existing `SAMPLE_MAX_COUNT` check. This is the same behavior as before, just counting from two folders.

---

### 4. Add `sd_getNumOneShotSamples()` accessor

**Target**: `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`

**Add after `sd_getNumSamples()` (after the function at line ~111):**

```c
/*
 * sd_getNumOneShotSamples()
 *
 * Returns only the count from /samples (the one-shot directory).
 * Used by sampleMemory_loadSamples() to determine the boundary
 * between one-shot and looped entries in the SampleInfo[] table.
 *
 * Indices [0 .. sd_getNumOneShotSamples()-1] are one-shot.
 * Indices [sd_getNumOneShotSamples() .. sd_getNumSamples()-1] are looped.
 */
uint8_t sd_getNumOneShotSamples(void)
{
   return sd_foundSampleFiles;
}
```

**Why**: `sampleMemory_loadSamples()` needs to know the split point so it can pass `looped=1` to `sampleMemory_makeSampleInfo()` for the second group of files.

**Functions impacted**: Only called from `sampleMemory_loadSamples()`.

**Risk**: None. This is a pure read of existing state.

---

### 5. Add the `/loops` directory file opener in `sd_setActiveSample()`

**Target**: `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`

**This is the most important change.** The existing `sd_setActiveSample()` only opens files from `/samples`. We need it to seamlessly open from `/loops` when the index is past the one-shot count.

**Replace the existing `sd_setActiveSample()` function (lines 146–211):**

```c
/*
 * sd_setActiveSample()
 *
 * Selects the i-th sample across the combined /samples and /loops directories.
 *
 * Indices [0 .. sd_foundSampleFiles-1] open from /samples.
 * Indices [sd_foundSampleFiles .. sd_foundSampleFiles+sd_foundLoopFiles-1]
 *   open from /loops.
 *
 * The function opens the directory, iterates to the target .WAV file,
 * opens it, sets sd_currentSampleLength from the 'data' chunk, and
 * copies the 8.3 filename into sd_currentSampleName.
 *
 * Impacts: This replaces the prior single-folder version. The behavior
 *   for indices < sd_foundSampleFiles is identical to before.
 *
 * Risk: The function re-opens the directory from scratch each time.
 *   Performance is O(n) per call, which is acceptable for ≤50 total files
 *   during a blocking offline import. Not suitable for real-time use.
 */
void sd_setActiveSample(uint8_t sampleNr)
{
   FRESULT res;
   uint8_t currentSample = 0;
   const char* folder;
   uint8_t localIndex;

   /*
    * Determine which folder this index belongs to.
    *
    * If sampleNr < sd_foundSampleFiles → /samples, local index = sampleNr.
    * Otherwise → /loops, local index = sampleNr - sd_foundSampleFiles.
    *
    * This is the key routing logic. The rest of the function is identical
    * for both folders — open the directory, count .WAV files until we
    * reach localIndex, then open that file.
    */
   if(sampleNr < sd_foundSampleFiles)
   {
      folder = "/samples";
      localIndex = sampleNr;
   }
   else
   {
      folder = "/loops";
      localIndex = sampleNr - sd_foundSampleFiles;
   }

   res = f_opendir(&sd_Directory, folder);
   if(res != FR_OK)
   {
      /* Directory vanished between init count and load — should not happen
       * during a blocking import, but fail gracefully. */
      sd_currentSampleLength = 0;
      return;
   }

   /* Iterate to the localIndex-th .WAV file in this folder. */
   FILINFO fno;

   while(1)
   {
      res = f_readdir(&sd_Directory, &fno);
      if(res != FR_OK || fno.fname[0] == 0) break;
      if(fno.fname[0] == '.') continue;

      char *fn = fno.fname;

      if(fno.fattrib & AM_DIR)
      {
         continue;
      }
      else
      {
         int i;
         for(i = 0; i < 12 - 3; i++)
         {
            if(fn[i] == '.')
            {
               if((fn[i+1] == 'W') && (fn[i+2] == 'A') && (fn[i+3] == 'V'))
               {
                  if(localIndex == currentSample)
                  {
                     /*
                      * Build the full path.
                      *
                      * "/samples/" is 9 chars, "/loops/" is 7 chars.
                      * 8.3 filename is max 12 chars + null.
                      * Buffer is 22 bytes, enough for either path.
                      *
                      * IMPORTANT: we use strlen(folder)+1 to position the
                      * filename copy correctly after the folder prefix + '/'.
                      */
                     char filename[22];
                     uint8_t prefixLen;

                     /* Build "/<folder>/<filename>" */
                     memset(filename, 0, sizeof(filename));
                     memcpy(filename, folder, strlen(folder));
                     prefixLen = strlen(folder);
                     filename[prefixLen] = '/';
                     memcpy(&filename[prefixLen + 1], fn, 13);
                     filename[21] = 0;

                     res = f_open((FIL*)&sd_File, filename, FA_OPEN_EXISTING | FA_READ);

                     if(res != FR_OK)
                     {
                        sd_currentSampleLength = 0;
                        return;
                     }

                     memcpy(sd_currentSampleName, fn, 11);
                     sd_currentSampleName[11] = 0;
                     sd_currentSampleLength = findDataChunk();

                     return;
                  }
                  currentSample++;
               }
            }
         }
      }
   }

   /* Fell through without finding the target — shouldn't happen if counts
    * are consistent, but set length to 0 to produce a silent entry. */
   sd_currentSampleLength = 0;
}
```

**Why**: The old version hardcoded `"/samples/"` as the directory and constructed the path using `char filename[22] = "/samples/";` with a `memcpy` at offset 9. The new version determines the folder dynamically based on the sample index and the known split point.

**Design note**: The folder string is `"/samples"` or `"/loops"` (without trailing slash). We build the path as `folder + "/" + fn` so both folders work with the same code. The `"/samples"` prefix is 8 chars + `'/'` = 9, same as before. The `"/loops"` prefix is 6 chars + `'/'` = 7, so filenames from `/loops` sit earlier in the 22-byte buffer with plenty of room.

**Structural impact**: This replaces the entire body of `sd_setActiveSample()`. All existing callers (`sampleMemory_loadSamples()`) call it the same way. The only behavioral change is that indices ≥ `sd_foundSampleFiles` now reach into `/loops` instead of failing silently.

**Risk**:
- If the SD card is removed mid-import, `f_opendir` or `f_open` will fail. This is pre-existing behavior; Phase 3 does not make it worse.
- `strlen()` is used on compile-time constant strings. The compiler will likely inline these, but even at runtime, the cost is trivial for 7-9 char strings during a blocking import.
- The `filename` buffer is 22 bytes. The longest possible path is `"/samples/" + 12-char 8.3 name = 21 chars + null = 22`. Safe.

---

### 6. Add `sdManager_countLoopFolder()` prototype to `SD_Manager.h`

**Target**: `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.h`

**Add after the existing `sd_getNumSamples()` declaration (after line 54):**

```c
/* Scans /loops for .WAV files and stores the count internally.
 * Call after sdManager_init(). If /loops is missing, count stays 0. */
void sdManager_countLoopFolder(void);

/* Returns the number of one-shot samples from /samples only.
 * Indices [0..N-1] are one-shot, [N..total-1] are looped. */
uint8_t sd_getNumOneShotSamples(void);
```

**Why**: `sampleMemory_loadSamples()` in `SampleMemory.c` needs to call both new functions. They must be declared in the header.

**Risk**: None. New declarations only.

---

### 7. Modify `sampleMemory_loadSamples()` to call the loop scanner and tag metadata

**Target**: `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`

This is the final change that ties everything together. The existing function already iterates over `sd_getNumSamples()` entries and builds `SampleInfo[]`. We add:
1. A call to `sdManager_countLoopFolder()` after `sdManager_init()`.
2. Recording the split point from `sd_getNumOneShotSamples()`.
3. Passing `looped = (i >= oneShotCount) ? 1u : 0u` to `sampleMemory_makeSampleInfo()`.

**Replace the existing `sampleMemory_loadSamples()` function (lines 88–165):**

```c
//---------------------------------------------------------------
#define BLOCKSIZE 2
/*
 * sampleMemory_loadSamples()
 *
 * Main sample import entry point. Called from the FRONT_SAMPLE_START_UPLOAD
 * handler in frontPanelReceivingProtocol.c.
 *
 * Post-Phase-3 changes:
 *   1. Calls sdManager_countLoopFolder() to also count /loops .WAV files.
 *   2. sd_getNumSamples() now returns the combined count from both folders.
 *   3. Records oneShotCount = sd_getNumOneShotSamples() (files from /samples).
 *   4. For each sample, determines looped = (i >= oneShotCount).
 *   5. sd_setActiveSample(i) opens from /samples or /loops as appropriate.
 *   6. sampleMemory_makeSampleInfo() receives the looped flag.
 *
 * Everything else — flash erase, PCM write, metadata write, count commit —
 * is unchanged from Phase 1.
 *
 * Impacts: This is the only function that calls into SD_Manager during import.
 *   The flash write path (FLASH_If_WriteSamplePcm, FLASH_If_Write) is not
 *   changed. The metadata format (SampleInfo with bit-packed size) is not
 *   changed — we just set bit 31 for loop entries.
 *
 * Risk:
 *   - If /loops has files but /samples is empty, oneShotCount is 0 and
 *     all files are tagged as looped. This is correct behavior.
 *   - If /loops is missing, oneShotCount == numSamples and all files are
 *     tagged as one-shot. This is identical to pre-Phase-3 behavior.
 *   - The SAMPLE_MAX_COUNT check applies to the combined total. If the
 *     user has 40 samples + 20 loops = 60, the import is rejected.
 */
void sampleMemory_loadSamples()
{
   SampleInfo info[SAMPLE_MAX_COUNT];
   uint32_t addr = 0;
   uint8_t i;
   volatile uint32_t add;
   uint32_t infoWords;
   uint32_t len;
   uint32_t j;
   char* name;

   /*
    * Phase 3 addition: scan /loops after /samples.
    * sdManager_init() was already called by sampleMemory_init() and counted
    * the /samples .WAV files. Now count /loops too.
    */
   sdManager_countLoopFolder();

   uint8_t numSamples = sd_getNumSamples();      /* combined total */
   uint8_t oneShotCount = sd_getNumOneShotSamples(); /* from /samples only */

   if(numSamples == 0 || numSamples > SAMPLE_MAX_COUNT)
   {
      spi_deInit();
      return;
   }

   /* Erase user flash (sectors 8-11). Blocking, no audio. */
   if(FLASH_If_Erase(SAMPLE_ROM_START_ADDRESS) != FLASH_IF_OK)
   {
      spi_deInit();
      return;
   }

   /* Reserve space for sample info header. */
   memset(info, 0, sizeof(info));

   for(i = 0; i < numSamples; i++)
   {
      sd_setActiveSample(i);
      len  = sd_getActiveSampleLength();
      name = sd_getActiveSampleName();

      /*
       * Phase 3 key change: determine looped flag from the index.
       *
       * Indices [0 .. oneShotCount-1] came from /samples → one-shot (looped=0).
       * Indices [oneShotCount .. numSamples-1] came from /loops → looped (looped=1).
       *
       * sampleMemory_makeSampleInfo() sets bit 31 of SampleInfo.size when
       * looped is non-zero. The oscillator's calcUserSampleOscBlock() already
       * unpacks this flag via sampleMemory_isSampleLooped() (Phase 2 work).
       */
      uint8_t looped = (i >= oneShotCount) ? 1u : 0u;

      info[i] = sampleMemory_makeSampleInfo(name,
                                            len / 2u,
                                            SAMPLE_ROM_START_ADDRESS + 4u + addr * 4u,
                                            looped);

      for(j = 0; j < len; )
      {
         int16_t data[BLOCKSIZE];
         sd_readSampleData(data, BLOCKSIZE);
         add = 4 + SAMPLE_ROM_START_ADDRESS + 4 * addr;

         if(FLASH_If_WriteSamplePcm(&add, (uint32_t*)data, BLOCKSIZE / 2) != FLASH_IF_OK)
         {
            spi_deInit();
            return;
         }

         j += BLOCKSIZE * 2;
         addr += BLOCKSIZE / 2;
      }
   }

   /* Write info header to flash. */
   add = SAMPLE_INFO_START_ADDRESS;
   infoWords = (numSamples * sizeof(SampleInfo) + 3u) / 4u;
   if(FLASH_If_Write(&add, (uint32_t*)(info), infoWords) != FLASH_IF_OK)
   {
      spi_deInit();
      return;
   }

   /* Commit the sample count — this is the visibility marker. */
   if(sampleMemory_setNumSamples(numSamples) != FLASH_IF_OK)
   {
      spi_deInit();
      return;
   }

   spi_deInit();
}
```

**Why each changed section matters**:

- `sdManager_countLoopFolder()` call: must happen before `sd_getNumSamples()` so the combined count includes `/loops`.
- `oneShotCount` capture: this is the boundary between one-shot and looped entries. Without it, we cannot determine which entries to tag.
- `looped` determination in the loop body: this is the single line that connects Phase 3 (import) to Phase 2 (playback). The oscillator code in `calcUserSampleOscBlock()` already calls `sampleMemory_isSampleLooped(info)` and branches to loop/one-shot behavior. Phase 3 just ensures that the flag is correctly written during import.
- Everything else (erase, PCM write, metadata write, count commit, error paths) is identical to the Phase 1 implementation.

**Structural impact**: Only `sampleMemory_loadSamples()` changes. No other function in `SampleMemory.c` is affected. The function signature and caller (`frontPanelReceivingProtocol.c`) are unchanged.

**Risk**:
- The `sdManager_countLoopFolder()` call adds one extra directory scan during import. This is a blocking offline operation, so the time is acceptable.
- If the SD card has files that disappear between counting and opening (e.g., card removed), the import aborts at the `FLASH_If_WriteSamplePcm` error or the `sd_currentSampleLength == 0` producing a zero-length entry. This is pre-existing failure behavior.

---

## Files Changed Summary

| File | Type | Description |
|------|------|-------------|
| `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c` | MODIFY | Add `sd_foundLoopFiles`, `sdManager_countLoopFolder()`, `sd_getNumOneShotSamples()`, modify `sd_getNumSamples()`, replace `sd_setActiveSample()` |
| `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.h` | MODIFY | Add `sdManager_countLoopFolder()` and `sd_getNumOneShotSamples()` prototypes |
| `mainboard/LxrStm32/src/SampleRom/SampleMemory.c` | MODIFY | Replace `sampleMemory_loadSamples()` body to call loop scanner and pass looped flag |

**Total: 3 files. No new files. No AVR changes. No protocol changes. No oscillator changes.**

---

## What Does NOT Change

- **`Oscillator.c`**: Already has loop/one-shot branching from Phase 2. No changes needed.
- **`SampleMemory.h`**: Already has `SAMPLE_INFO_LOOP_FLAG`, `SAMPLE_INFO_SIZE_MASK`, and `sampleMemory_makeSampleInfo()` from Phase 2. No changes needed.
- **`flash_if.c` / `flash_if.h`**: No changes. The flash write path is agnostic to sample content.
- **AVR code**: No changes. The AVR sends the same `SAMPLE_START_UPLOAD` command. The AVR does not know or care about `/loops`.
- **Front-panel protocol**: No new opcodes, no new handshakes.
- **Sample naming**: The 3-char name in `SampleInfo.name` comes from the filename. Both `/samples` and `/loops` files get their names the same way. The AVR sample name display (if any) works identically.

---

## SD Card Directory Structure Expected

```
SD Card Root
├── samples/
│   ├── KICK.WAV      ← imported as one-shot (looped=0)
│   ├── SNARE.WAV     ← imported as one-shot (looped=0)
│   └── HAT.WAV       ← imported as one-shot (looped=0)
└── loops/
    ├── PAD01.WAV      ← imported as looped (looped=1)
    └── BASS.WAV       ← imported as looped (looped=1)
```

- Directory names are case-insensitive on FAT32 (FatFs handles this).
- If `/loops` does not exist, import proceeds with `/samples` only.
- If `/samples` does not exist, `sdManager_init()` sets `sd_foundSampleFiles = 0` and does not set `sd_initOkFlag`. Depending on subsequent behavior this may cause `sd_getNumSamples()` to return only the `/loops` count — **this is an edge case that should be tested**.

---

## Verification Plan

### Build Verification

```bash
make -C mainboard/LxrStm32 -j4 stm32
```

Must complete with no new errors or warnings in `SD_Manager.c` or `SampleMemory.c`.

### Hardware Test Cases

1. **Baseline: /samples only, no /loops folder**
   - Place 3-5 `.WAV` files in `/samples/`, no `/loops/` directory.
   - Trigger "Sample upload" from AVR menu.
   - Verify all samples load and play as one-shot (same as before Phase 3).
   - Verify sample count via `SAMPLE_COUNT` reply matches file count.

2. **Both folders: /samples + /loops**
   - Place 2-3 `.WAV` files in `/samples/` and 1-2 `.WAV` files in `/loops/`.
   - Trigger "Sample upload".
   - Verify one-shot samples play and stop at end.
   - Verify loop samples play and wrap continuously.
   - Verify sample count is the combined total.

3. **Only /loops, no /samples**
   - Create `/loops/` with 1-2 `.WAV` files, no `/samples/` directory.
   - Trigger "Sample upload".
   - Expected: either all files import as looped, or import is rejected if `sdManager_init()` fails. Document the actual behavior.

4. **Empty /loops folder**
   - `/samples/` has files, `/loops/` exists but is empty.
   - Verify behavior is identical to test case 1.

5. **Combined count exceeds SAMPLE_MAX_COUNT**
   - Place 30 files in `/samples/` and 25 in `/loops/` (total 55 > 50).
   - Verify import is rejected gracefully (no crash, no partial write).

6. **Large looped sample**
   - Place a `.WAV` file > 32768 frames in `/loops/`.
   - Verify it imports and loops correctly (exercises Phase 2 long-sample code path).

---

## Interdependencies

- **Phase 1 (complete)**: `FLASH_If_WriteSamplePcm()`, `SAMPLE_MAX_COUNT` clamp, count-last commit. Required.
- **Phase 2 (complete)**: `sampleMemory_makeSampleInfo()`, `sampleMemory_isSampleLooped()`, loop/one-shot oscillator branching. Required.
- **Phase 4 (independent)**: Waveform interpolation. Not affected by or dependent on Phase 3.

---

## Edge Case: `sdManager_init()` Failure When `/samples` Missing

The current `sdManager_init()` only sets `sd_initOkFlag = 1` if `f_opendir("/samples")` succeeds. If `/samples` does not exist:

- `sd_foundSampleFiles = 0`
- `sd_initOkFlag = 0`

The current code does not check `sd_initOkFlag` before proceeding with the import in `sampleMemory_loadSamples()`. It relies on `sd_getNumSamples() == 0` to abort. Post-Phase-3, if `/loops` has files but `/samples` doesn't, `sd_getNumSamples()` will return a non-zero count and the import will proceed.

This is actually the desired behavior — loops-only import should work. However, `sdManager_init()` is called from `sampleMemory_init()` and handles the FatFs mount (`f_mount`). If `f_opendir("/samples")` fails, the mount is still valid and `sdManager_countLoopFolder()` can still open `/loops` successfully.

**Action**: No code change needed. The FatFs mount in `sdManager_init()` happens before the directory open. Even if `/samples` is missing, the filesystem is mounted and `/loops` can be scanned.
