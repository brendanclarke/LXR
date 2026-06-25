# SAMPLE_PLAN_PHASE_3_AUDIT

## Updated Goal

Replace the failing "abort when combined count exceeds 50" behavior with a
partial-import policy based on a 248 imported-sample ceiling:

- Import at most 248 combined entries.
- Scan/import `/samples` first.
- Then scan/import `/loops` into remaining count and flash space.
- If more than 248 `.WAV` files are available, import the first files that fit
  this policy and report `248 max exceeded`.
- If PCM data does not fit before the metadata table, skip files that do not
  fit, keep trying later files, and report `Flash exceeded`.
- If both limits are exceeded, report both warnings.
- AVR load screen displays each warning for one second.

```text
Sample Load
248 max exceeded
```

```text
Sample Load
Flash exceeded
```

The warning must not mean import failure. It means the import completed with
truncation.

## Design Summary

The STM owns the actual import and must return result flags. The AVR owns the
LCD and must display those flags.

The existing raw one-byte ACK cannot report "count cap hit" or "flash cap hit",
and it is also vulnerable to stale queued bytes. Replace the sample-upload ACK
with a normal three-byte sample-control result packet:

```text
SAMPLE_CC, SAMPLE_UPLOAD_RESULT, statusFlags
```

`statusFlags` is a payload bitfield, not an opcode:

```c
SAMPLE_UPLOAD_STATUS_OK          0x00
SAMPLE_UPLOAD_STATUS_COUNT_LIMIT 0x01
SAMPLE_UPLOAD_STATUS_FLASH_LIMIT 0x02
```

These values do not conflict with `SAMPLE_START_UPLOAD == 0x01` or
`SAMPLE_COUNT == 0x02`, because those existing constants live in packet byte 2
and the new status flags live in packet byte 3.

The 248 cap is chosen because waveform values are `uint8_t`, built-in waveforms
occupy values `0..5`, and user samples start at `OSC_SAMPLE_START == 6`.
`6 + 248 - 1 == 253`, leaving the menu entry count at `6 + 248 == 254`, which
still fits in the current `uint8_t` menu entry count. This avoids the `255/256`
edge where existing menu logic would become fragile.

The AVR waveform label field is only three characters wide. Keep that field
working up to 248 samples by changing user sample labels:

```text
sample index 0..99    -> s0..s99
sample index 100..199 -> t0..t99
sample index 200..247 -> u0..u47
```

This keeps every displayed sample label at three characters or fewer.

## Files To Change

1. `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.h`
2. `mainboard/LxrStm32/src/Hardware/SD_FAT/SD_Manager.c`
3. `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`
4. `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`
5. `mainboard/LxrStm32/src/SampleRom/flash_if.h`
6. `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`
7. `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
8. `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h`
9. `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`
10. `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
11. `front/LxrAvr/Menu/menu.c`

---

## 1. `SD_Manager.h`

### Replace Existing Sample API Declarations

Current declarations:

```c
//returns the number of samples in the sample folder /samples
uint8_t sd_getNumSamples();
void sdManager_countLoopFolder(void);
uint8_t sd_getNumOneShotSamples(void);
//selects the active sample from the folder [0:NUM_SAMPLES]
void sd_setActiveSample(uint8_t sampleNr);
```

Replace them with:

```c
// Returns the total number of .WAV files found across /samples and /loops.
// uint16_t is required so counts above 248 are detected instead of wrapping.
uint16_t sd_getNumSamples(void);

// Scans /loops after sdManager_init() has scanned /samples.
// Missing /loops remains non-fatal; the loop count stays at zero.
void sdManager_countLoopFolder(void);

// Returns only the number of files found in /samples.
// This is the boundary used to tag later entries as looped.
uint16_t sd_getNumOneShotSamples(void);

// Selects the active file by combined index.
// uint16_t lets the importer scan past 248 candidates when some files are skipped.
void sd_setActiveSample(uint16_t sampleNr);
```

Why each line changes:

- `uint16_t sd_getNumSamples(void);` reports the real source-file count so the
  importer can detect `>248`.
- `uint16_t sd_getNumOneShotSamples(void);` keeps the `/samples` to `/loops`
  boundary in the same width as the total.
- `void sd_setActiveSample(uint16_t sampleNr);` lets the importer keep scanning
  source files after skipped files without wrapping at 255.

---

## 2. `SD_Manager.c`

### Change Count Storage Width

Current declarations:

```c
uint8_t sd_foundSampleFiles = 0;
static uint8_t sd_foundLoopFiles = 0;
```

Replace with:

```c
uint16_t sd_foundSampleFiles = 0;      // Count all /samples .WAV files without uint8_t wrap.
static uint16_t sd_foundLoopFiles = 0; // Count all /loops .WAV files without uint8_t wrap.
```

### Replace `sd_getNumSamples()`

Current:

```c
uint8_t sd_getNumSamples()
{
   uint16_t total = (uint16_t)sd_foundSampleFiles + (uint16_t)sd_foundLoopFiles;

   if(total > 255u) total = 255u;

   return (uint8_t)total;
}
```

Replace with:

```c
uint16_t sd_getNumSamples(void)
{
   // Return the real combined count so the importer can detect >248 cleanly.
   return (uint16_t)(sd_foundSampleFiles + sd_foundLoopFiles);
}
```

### Replace `sd_getNumOneShotSamples()`

Current:

```c
uint8_t sd_getNumOneShotSamples(void)
{
   return sd_foundSampleFiles;
}
```

Replace with:

```c
uint16_t sd_getNumOneShotSamples(void)
{
   // /samples entries are imported first and are never tagged as looped.
   return sd_foundSampleFiles;
}
```

### Change `sd_setActiveSample()` Signature And Locals

Current function header and count locals:

```c
void sd_setActiveSample(uint8_t sampleNr)
{
   FRESULT res;
   uint8_t currentSample = 0;
   const char* folder;
   uint8_t localIndex;
```

Replace with:

```c
void sd_setActiveSample(uint16_t sampleNr)
{
   FRESULT res;                 // FatFs result for directory/file operations.
   uint16_t currentSample = 0;  // Counts matching .WAV files within the selected folder.
   const char* folder;          // Points to either "/samples" or "/loops".
   uint16_t localIndex;         // Index inside the selected folder, not combined index.
```

Keep the existing folder-selection logic:

```c
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
```

Why each line changes:

- Counter globals become `uint16_t` because wrapping would hide the exact
  "more than 248" condition.
- `sd_getNumSamples()` stops clamping to `255` because the sample importer owns
  the count policy.
- `sd_setActiveSample()` accepts `uint16_t` so the importer can keep scanning
  after skipped entries.

---

## 3. `SampleMemory.h`

### Replace Stale Metadata Layout Comment

Current comments describe the old 50-entry/0x190-byte table and the incorrect
"7 byte" struct assumption. Replace that comment block with:

```c
// Sample flash is split into:
//   [SAMPLE_ROM_START_ADDRESS]          32-bit committed sample count
//   [SAMPLE_ROM_START_ADDRESS + 4 ...]  packed PCM data
//   [SAMPLE_INFO_START_ADDRESS ...]     SampleInfo metadata table
//
// SampleInfo is compiler-padded to 12 bytes on STM32:
//   char name[3] + 1 byte padding + uint32_t size + uint32_t offset.
// 248 entries require 2976 bytes (0xBA0), so reserve 0xC00 bytes.
```

### Replace Sample Layout Constants

Current block:

```c
#define SAMPLE_INFO_START_ADDRESS 	((uint32_t)0x080F9E70)
#define SAMPLE_INFO_SIZE 			0x190
#define SAMPLE_ROM_SIZE				((uint32_t)0x00078E70) //499.216 kByte
#define SAMPLE_MAX_COUNT			((uint8_t)50)
```

Replace with:

```c
// Reserve 0xC00 bytes for metadata: 248 padded SampleInfo entries need 0xBA0.
#define SAMPLE_INFO_SIZE                ((uint32_t)0x00000C00u)

// Keep metadata ending at the existing 0x080FA000 boundary while making room for 248 entries.
#define SAMPLE_INFO_START_ADDRESS       ((uint32_t)0x080F9400)

// PCM space runs from the count word at SAMPLE_ROM_START_ADDRESS up to metadata.
#define SAMPLE_ROM_SIZE                 ((uint32_t)(SAMPLE_INFO_START_ADDRESS - SAMPLE_ROM_START_ADDRESS))

// New combined /samples + /loops import cap.
#define SAMPLE_MAX_COUNT                ((uint8_t)248)
```

### Add Upload Status Flags After `SAMPLE_INFO_SIZE_MASK`

Add:

```c
// No truncation occurred during the latest sample upload.
#define SAMPLE_UPLOAD_STATUS_OK          ((uint8_t)0x00u)

// More than SAMPLE_MAX_COUNT files were available; only the first fitting 248 load.
#define SAMPLE_UPLOAD_STATUS_COUNT_LIMIT ((uint8_t)0x01u)

// One or more files did not fit in the available PCM flash region and were skipped.
#define SAMPLE_UPLOAD_STATUS_FLASH_LIMIT ((uint8_t)0x02u)
```

### Replace `sampleMemory_loadSamples()` Prototype

Current:

```c
void sampleMemory_loadSamples();
```

Replace with:

```c
// Loads as many samples/loops as fit and returns SAMPLE_UPLOAD_STATUS_* flags.
uint8_t sampleMemory_loadSamples(void);
```

Why each line changes:

- `SAMPLE_INFO_SIZE` grows because the current 0x190-byte metadata area only
  covered the 50-entry design.
- `SAMPLE_INFO_START_ADDRESS` moves down to reserve a 0xC00-byte metadata table.
- `SAMPLE_ROM_SIZE` becomes derived from addresses so it cannot drift again.
- `SAMPLE_MAX_COUNT` becomes 248, which fits the byte-oriented waveform model.
- Status flags let STM tell AVR which user-facing warnings to display.
- The loader returns flags instead of `void`.

---

## 4. `flash_if.h`

### Replace Metadata Boundary

Current:

```c
#define FLASH_IF_SAMPLE_INFO_START_ADDRESS ((uint32_t)0x080F9E70)
```

Replace with:

```c
// Must match SAMPLE_INFO_START_ADDRESS so PCM writes stop before the 248-entry table.
#define FLASH_IF_SAMPLE_INFO_START_ADDRESS ((uint32_t)0x080F9400)
```

Why: `FLASH_If_WriteSamplePcm()` uses this macro as the upper PCM write limit.
If it stays at the old address, PCM writes could overwrite the enlarged
metadata table.

---

## 5. `SampleMemory.c`

### Add A Static Import Metadata Buffer

After the existing flash pointer globals:

```c
static uint16_t *sampleMemory_data 			= (uint16_t*)	SAMPLE_ROM_START_ADDRESS;
static SampleInfo* sampleMemory_infoData	= (SampleInfo*) SAMPLE_INFO_START_ADDRESS;
```

Add:

```c
// Stages metadata for the accepted import subset without placing ~3 KB on the stack.
static SampleInfo sampleMemory_importInfo[SAMPLE_MAX_COUNT];
```

Why: `248 * sizeof(SampleInfo)` is about 2976 bytes. A static buffer avoids a
large stack allocation during the blocking import.

### Add Helper Functions Above `#define BLOCKSIZE 2`

Insert after `sampleMemory_setNumSamples()`:

```c
static uint32_t sampleMemory_getPcmCapacityWords(void)
{
   // Word zero at SAMPLE_ROM_START_ADDRESS stores the visible sample count.
   const uint32_t pcmStart = SAMPLE_ROM_START_ADDRESS + 4u;

   // Convert the byte span before metadata into the 32-bit word unit used by addr.
   return (SAMPLE_INFO_START_ADDRESS - pcmStart) / 4u;
}

static uint32_t sampleMemory_bytesToFlashWords(uint32_t byteCount)
{
   // The existing write loop programs one 32-bit word per BLOCKSIZE=2 int16_t read.
   // Round up so an odd final half-word still reserves the word the writer uses.
   return (byteCount + 3u) / 4u;
}
```

### Replace The Entire `sampleMemory_loadSamples()` Function

Replace the current function with this structure:

```c
uint8_t sampleMemory_loadSamples(void)
{
   // Use the file-scope staging table so 248 entries do not consume stack.
   SampleInfo *info = sampleMemory_importInfo;

   // Number of 32-bit PCM words already committed after the count word.
   uint32_t addr = 0;

   // Number of entries accepted into info[] and eventually committed as the sample count.
   uint8_t loadedSamples = 0;

   // Status bitfield returned to the front panel for one-second warning messages.
   uint8_t statusFlags = SAMPLE_UPLOAD_STATUS_OK;

   // Candidate index across /samples first, then /loops.
   uint16_t i;

   // Flash write cursor used by FLASH_If_Write* helpers.
   volatile uint32_t add;

   // Number of 32-bit words required to write the final metadata table.
   uint32_t infoWords;

   // Active WAV data-chunk length in bytes.
   uint32_t len;

   // Byte cursor through the active WAV data chunk.
   uint32_t j;

   // Current 8.3 filename from SD_Manager, used for the 3-byte SampleInfo name.
   char* name;

   // Scan /loops after sampleMemory_init()/sdManager_init() has counted /samples.
   sdManager_countLoopFolder();

   // Real combined candidate count, not clamped to 248.
   uint16_t totalFound = sd_getNumSamples();

   // Boundary between one-shot and looped candidates.
   uint16_t oneShotCount = sd_getNumOneShotSamples();

   // Capacity in the PCM region before metadata begins.
   const uint32_t pcmCapacityWords = sampleMemory_getPcmCapacityWords();

   // Preserve existing no-files behavior: do not erase flash if nothing is available.
   if(totalFound == 0u)
   {
      spi_deInit();
      return statusFlags;
   }

   // Count overflow is now a truncation warning, not an import abort.
   if(totalFound > SAMPLE_MAX_COUNT)
   {
      statusFlags |= SAMPLE_UPLOAD_STATUS_COUNT_LIMIT;
   }

   // Erase once before writing the accepted subset.
   if(FLASH_If_Erase(SAMPLE_ROM_START_ADDRESS) != FLASH_IF_OK)
   {
      spi_deInit();
      return statusFlags;
   }

   // Clear all staged metadata so skipped entries cannot leak stale bytes.
   memset(info, 0, sizeof(sampleMemory_importInfo));

   // Iterate candidates in existing combined order: all /samples first, then /loops.
   for(i = 0u; i < totalFound; i++)
   {
      // Once the 248-entry table is full, stop; remaining files cannot be imported.
      if(loadedSamples >= SAMPLE_MAX_COUNT)
      {
         break;
      }

      // Open the candidate file and position the FatFs read cursor at its data chunk.
      sd_setActiveSample(i);

      // Length is bytes of PCM payload reported by the WAV data chunk.
      len = sd_getActiveSampleLength();

      // Name pointer remains owned by SD_Manager until the next sd_setActiveSample().
      name = sd_getActiveSampleName();

      // Convert byte length into the 32-bit write units used by the existing writer.
      uint32_t sampleWords = sampleMemory_bytesToFlashWords(len);

      // If this whole file cannot fit, skip it and keep trying later candidates.
      if(sampleWords > pcmCapacityWords || addr > (pcmCapacityWords - sampleWords))
      {
         statusFlags |= SAMPLE_UPLOAD_STATUS_FLASH_LIMIT;
         continue;
      }

      // Entries before oneShotCount came from /samples; later entries came from /loops.
      uint8_t looped = (i >= oneShotCount) ? 1u : 0u;

      // Store metadata at the compact loaded index, not the source candidate index.
      info[loadedSamples] = sampleMemory_makeSampleInfo(name,
                                                        len / 2u,
                                                        SAMPLE_ROM_START_ADDRESS + 4u + addr * 4u,
                                                        looped);

      // Write the accepted sample's PCM data to internal flash.
      for(j = 0u; j < len; )
      {
         int16_t data[BLOCKSIZE];

         // Existing SD reader reads BLOCKSIZE int16_t values per step.
         sd_readSampleData(data, BLOCKSIZE);

         // Existing PCM layout writes immediately after the count word plus addr words.
         add = 4u + SAMPLE_ROM_START_ADDRESS + 4u * addr;

         // Bounds are pre-checked above; this still catches flash operation failures.
         if(FLASH_If_WriteSamplePcm(&add, (uint32_t*)data, BLOCKSIZE / 2u) != FLASH_IF_OK)
         {
            spi_deInit();
            return statusFlags;
         }

         // Advance by the bytes read/written by one BLOCKSIZE iteration.
         j += BLOCKSIZE * 2u;

         // Advance the compact PCM word cursor by one 32-bit word.
         addr += BLOCKSIZE / 2u;
      }

      // Only now is the candidate fully accepted and visible in the staged table.
      loadedSamples++;
   }

   // Write the compact metadata table for accepted entries only.
   add = SAMPLE_INFO_START_ADDRESS;
   infoWords = (loadedSamples * sizeof(SampleInfo) + 3u) / 4u;
   if(FLASH_If_Write(&add, (uint32_t*)(info), infoWords) != FLASH_IF_OK)
   {
      spi_deInit();
      return statusFlags;
   }

   // Commit the visible sample count last so partially failed imports remain invisible.
   if(sampleMemory_setNumSamples(loadedSamples) != FLASH_IF_OK)
   {
      spi_deInit();
      return statusFlags;
   }

   // Release the STM SD/SPI pins before returning to the front-panel protocol.
   spi_deInit();

   // Return truncation flags so the AVR can display the correct warning(s).
   return statusFlags;
}
```

Delete this old abort block:

```c
if(numSamples==0 || numSamples > SAMPLE_MAX_COUNT)
{
   spi_deInit();
   return;
}
```

Why the new function works:

- `/samples` priority is preserved because the SD manager's combined order is
  already `/samples` first, then `/loops`.
- Count limiting is applied by accepting no more than `SAMPLE_MAX_COUNT`.
- Flash limiting is applied before writing a file, so no partial file becomes
  visible in metadata.
- Oversized files are skipped, not fatal; later smaller files can still fit.
- Metadata is compacted to `loadedSamples`, so skipped files leave no holes.
- The final committed sample count is the number actually loaded.

---

## 6. `frontPanelReceivingProtocol.h` On STM

### Add Sample Upload Result Opcode

Current sample opcodes:

```c
#define FRONT_SAMPLE_START_UPLOAD 		0x01	// begin sample upload
#define FRONT_SAMPLE_COUNT		 		   0x02	// sample count request/response
```

Replace with:

```c
#define FRONT_SAMPLE_START_UPLOAD       0x01 // begin sample upload
#define FRONT_SAMPLE_COUNT              0x02 // sample count request/response
#define FRONT_SAMPLE_UPLOAD_RESULT      0x03 // upload completed; data2 is SAMPLE_UPLOAD_STATUS_* flags
```

Why: the AVR needs a structured result packet. A normal `SAMPLE_CC` packet
avoids the old raw ACK ambiguity.

---

## 7. `frontPanelReceivingProtocol.c`

### Replace Sample Upload Handling

Current:

```c
case FRONT_SAMPLE_START_UPLOAD:
   seq_setRunning(0);
   sampleMemory_init();
   sampleMemory_loadSamples();
   FLASH_Lock();

   frontPanelSending_sendSampleUploadAck();
   break;
```

Replace with:

```c
case FRONT_SAMPLE_START_UPLOAD:
{
   // Stop playback before blocking flash erase/write work.
   seq_setRunning(0);

   // Initialize flash access and STM SD/FatFs state.
   sampleMemory_init();

   // Load the accepted subset and capture truncation flags for the AVR.
   uint8_t sampleStatus = sampleMemory_loadSamples();

   // Keep the existing defensive lock after loader completion.
   FLASH_Lock();

   // Send a structured result packet instead of an ambiguous bare ACK.
   frontPanelSending_sendSampleUploadResult(sampleStatus);
   break;
}
```

Why each line changes:

- `sampleStatus` captures whether count or flash truncation occurred.
- The send helper changes from ACK to `SAMPLE_CC / FRONT_SAMPLE_UPLOAD_RESULT`.
- Braces are added so the local `sampleStatus` declaration is scoped inside the
  `case`.

---

## 8. `frontPanelSendingProtocol.h`

### Replace Upload ACK Declaration

Current:

```c
/* Acknowledge SAMPLE_START_UPLOAD after STM starts sample reload handling. */
void frontPanelSending_sendSampleUploadAck(void);
```

Replace with:

```c
/* Report SAMPLE_START_UPLOAD completion. data2 carries SAMPLE_UPLOAD_STATUS_* flags. */
void frontPanelSending_sendSampleUploadResult(uint8_t statusFlags);
```

Why: the old helper could only say "done". The new helper reports "done,
possibly with truncation warnings".

---

## 9. `frontPanelSendingProtocol.c`

### Replace Upload ACK Sender

Current:

```c
void frontPanelSending_sendSampleUploadAck(void)
{
   /* Acknowledge a sample upload request. */
   frontPanelSending_sendByte(ACK);
}
```

Replace with:

```c
void frontPanelSending_sendSampleUploadResult(uint8_t statusFlags)
{
   // Use a normal three-byte packet so the AVR can wait for this exact reply.
   frontPanelSending_sendByte(SAMPLE_CC);

   // data1 identifies this as the post-upload result.
   frontPanelSending_sendByte(FRONT_SAMPLE_UPLOAD_RESULT);

   // data2 is a bitfield of SAMPLE_UPLOAD_STATUS_* warning flags.
   frontPanelSending_sendByte((uint8_t)(statusFlags & (SAMPLE_UPLOAD_STATUS_COUNT_LIMIT |
                                                       SAMPLE_UPLOAD_STATUS_FLASH_LIMIT)));
}
```

If this file does not already see the `SAMPLE_UPLOAD_STATUS_*` macros through
its includes, add `#include "SampleMemory.h"` at the top with the other local
protocol includes.

Why: `ACK == 1` can collide with ordinary data bytes. A three-byte packet is
parseable and can carry both requested warning bits.

---

## 10. `avrCommsReceivingProtocol.h` On AVR

### Add Matching Sample Result Opcode And Flags

Current sample opcodes:

```c
#define SAMPLE_START_UPLOAD 0x01	// begin sample upload
#define SAMPLE_COUNT		0x02	// sample count request/response
```

Replace with:

```c
#define SAMPLE_START_UPLOAD       0x01 // begin sample upload
#define SAMPLE_COUNT              0x02 // sample count request/response
#define SAMPLE_UPLOAD_RESULT      0x03 // STM upload finished; data2 is SAMPLE_UPLOAD_STATUS_* flags

#define SAMPLE_UPLOAD_STATUS_OK          ((uint8_t)0x00u) // No warning.
#define SAMPLE_UPLOAD_STATUS_COUNT_LIMIT ((uint8_t)0x01u) // More than 248 candidates existed.
#define SAMPLE_UPLOAD_STATUS_FLASH_LIMIT ((uint8_t)0x02u) // One or more candidates did not fit flash.
```

Why: the AVR menu code needs symbolic names for the result packet and warning
bits.

---

## 11. `menu.c` On AVR

### Add A Local Sample Count Cap

Near the other local `#define` values, add:

```c
// AVR waveform menu supports 6 built-ins plus 248 imported entries: 254 total.
#define MENU_SAMPLE_MAX_COUNT ((uint8_t)248)
```

Why: clamping the received count protects the `uint8_t` menu-entry count from
bad or stale STM replies.

### Add Static Helper Prototypes Near Existing Static Prototypes

After:

```c
static void menu_normalizeDtypeMenuEncoderValue(uint16_t paramNr, uint8_t *paramValue);
```

Add:

```c
// Formats sample index 0..247 into a three-character waveform label.
static void menu_formatSampleWaveformLabel(uint8_t sampleIndex, char *buf);

// Waits for the STM sample-upload result packet and returns SAMPLE_UPLOAD_STATUS_* flags.
static uint8_t menu_waitSampleUploadResult(void);

// Displays one bottom-line sample-load warning for exactly one second.
static void menu_showSampleLoadWarning(const char *line2);

// Displays all warning messages requested by the STM result flags.
static void menu_showSampleLoadWarnings(uint8_t statusFlags);
```

### Replace `menu_setNumSamples()`

Current:

```c
void menu_setNumSamples(uint8_t num)
{
	menu_numSamples = num;
}
```

Replace with:

```c
void menu_setNumSamples(uint8_t num)
{
   // Keep the waveform menu below the uint8_t entry-count edge.
   if(num > MENU_SAMPLE_MAX_COUNT)
      num = MENU_SAMPLE_MAX_COUNT;

   // This value is the committed imported sample count reported by STM.
   menu_numSamples = num;
}
```

### Add The Sample Label Helper Before `getMenuItemNameForValue()`

Insert before `getMenuItemNameForValue()`:

```c
static void menu_formatSampleWaveformLabel(uint8_t sampleIndex, char *buf)
{
   // Clear the three-character value field so one-digit labels do not leak old text.
   buf[0] = ' ';
   buf[1] = ' ';
   buf[2] = ' ';

   // Samples 200..247 render as u0..u47.
   if(sampleIndex >= 200u)
   {
      buf[0] = 'u';
      numtostru(&buf[1], (uint8_t)(sampleIndex - 200u));
   }
   // Samples 100..199 render as t0..t99.
   else if(sampleIndex >= 100u)
   {
      buf[0] = 't';
      numtostru(&buf[1], (uint8_t)(sampleIndex - 100u));
   }
   // Samples 0..99 render as s0..s99.
   else
   {
      buf[0] = 's';
      numtostru(&buf[1], sampleIndex);
   }
}
```

### Replace The `MENU_WAVEFORM` Sample-Label Branch

Current branch inside `getMenuItemNameForValue()`:

```c
case MENU_WAVEFORM:
   if(curParmVal < waveformNames[0][0])
      p=waveformNames[curParmVal + 1];
   else
   {
      buf[0]='s';
      buf[2]=' ';
      numtostru(&buf[1],(uint8_t)(curParmVal - waveformNames[0][0]));
      return;
   }
   break;
```

Replace with:

```c
case MENU_WAVEFORM:
   if(curParmVal < waveformNames[0][0])
      p = waveformNames[curParmVal + 1];
   else
   {
      // Convert waveform value 6..253 into imported sample index 0..247.
      const uint8_t sampleIndex = (uint8_t)(curParmVal - waveformNames[0][0]);

      // Use s/t/u prefixes so every sample label fits the three-character cell.
      menu_formatSampleWaveformLabel(sampleIndex, buf);
      return;
   }
   break;
```

### Add Upload Result Helper Implementations Before `menu_handleLoadMenu()`

Insert before:

```c
void menu_handleLoadMenu(int8_t inc, uint8_t btnClicked)
```

Add:

```c
static uint8_t menu_waitSampleUploadResult(void)
{
   // Small local parser states for SAMPLE_CC, data1, data2.
   uint8_t rxState = 0;

   // Holds the data1 byte while waiting for data2.
   uint8_t sampleCommand = 0;

   // Raw byte read from the UART FIFO.
   uint8_t data;

   for(;;)
   {
      // Busy-wait matches the existing sample-load blocking model.
      if(!uart_getc(&data))
      {
         continue;
      }

      // Status bytes start packets; only SAMPLE_CC matters for this wait.
      if(data & 0x80)
      {
         rxState = (data == SAMPLE_CC) ? 1u : 0u;
         continue;
      }

      // First data byte after SAMPLE_CC is the sample command id.
      if(rxState == 1u)
      {
         sampleCommand = data;
         rxState = 2u;
         continue;
      }

      // Second data byte completes the sample packet.
      if(rxState == 2u)
      {
         // This is the exact upload result packet requested by the menu action.
         if(sampleCommand == SAMPLE_UPLOAD_RESULT)
            return data;

         // If a stale count reply is encountered, consume it usefully.
         if(sampleCommand == SAMPLE_COUNT)
            menu_setNumSamples(data);

         // Reset for the next packet.
         rxState = 0u;
      }
   }
}

static void menu_showSampleLoadWarning(const char *line2)
{
   // Clear the normal load menu so the warning owns the screen for one second.
   lcd_clear();

   // Top line requested by the user.
   lcd_string_F(PSTR("Sample Load"));

   // Bottom line is one of the exact requested warning strings.
   lcd_setcursor(0,2);
   lcd_string_F(line2);

   // Hold the warning long enough to read, then let the normal repaint resume.
   _delay_ms(1000);
}

static void menu_showSampleLoadWarnings(uint8_t statusFlags)
{
   // If the 248-entry cap was exceeded, show the exact requested message.
   if(statusFlags & SAMPLE_UPLOAD_STATUS_COUNT_LIMIT)
   {
      menu_showSampleLoadWarning(PSTR("248 max exceeded"));
   }

   // If flash capacity was exceeded, show the exact requested message.
   if(statusFlags & SAMPLE_UPLOAD_STATUS_FLASH_LIMIT)
   {
      menu_showSampleLoadWarning(PSTR("Flash exceeded"));
   }
}
```

### Replace The `SAVE_TYPE_SAMPLES` Load Branch

Current branch:

```c
case SAVE_TYPE_SAMPLES:
   spi_deInit();

   lcd_clear();
   lcd_string_F(PSTR("Sample upload"));
   lcd_setcursor(0,2);
   lcd_string_F(PSTR("Started"));
   //send load sample command to mainboard
   avrComms_sendData(SAMPLE_CC,SAMPLE_START_UPLOAD,0x00);
   //wait for ack
   {
      uint8_t ret = uart_waitAck();
      if(ret == ACK)
      {

      }
   }

   //re-initialize SD-Card
   preset_init();

   // menu_repaintAll(); --AS screen will be repainted later, relax!

   avrComms_sendData(SAMPLE_CC,SAMPLE_COUNT,0x00);
   break;
```

Replace with:

```c
case SAVE_TYPE_SAMPLES:
{
   // Release AVR SD/SPI before asking STM to read the card.
   spi_deInit();

   // Drop stale sample-count replies so this transaction starts from a clean FIFO.
   uart_clearRxFifo();

   // Display the existing start message while STM performs the blocking import.
   lcd_clear();
   lcd_string_F(PSTR("Sample upload"));
   lcd_setcursor(0,2);
   lcd_string_F(PSTR("Started"));

   // Send the existing upload command; no AVR-to-STM opcode change is needed.
   avrComms_sendData(SAMPLE_CC, SAMPLE_START_UPLOAD, 0x00);

   // Wait for the structured STM result packet and collect truncation flags.
   uint8_t sampleStatus = menu_waitSampleUploadResult();

   // Show each requested warning for one second. If both bits are set, both display.
   menu_showSampleLoadWarnings(sampleStatus);

   // Re-initialize the AVR SD/FatFs side after STM has finished using the card.
   preset_init();

   // Refresh AVR's sample count display/ranges from the newly committed STM count.
   avrComms_sendData(SAMPLE_CC, SAMPLE_COUNT, 0x00);
   break;
}
```

Why each line changes:

- `uart_clearRxFifo()` removes stale `SAMPLE_COUNT` replies before starting the
  transaction.
- `uart_waitAck()` is removed because it cannot carry warning flags and can
  misread ordinary bytes.
- `menu_waitSampleUploadResult()` waits for the exact result packet.
- `menu_showSampleLoadWarnings()` displays the requested one-second warnings.
- Braces are added so `sampleStatus` is scoped inside the `case`.

---

## Expected Import Behavior After These Changes

### Count Limit Examples

- 90 files in `/samples`, 20 in `/loops`: imports all 110, no warning.
- 260 files in `/samples`, 20 in `/loops`: imports first 248 fitting
  `/samples`, imports no `/loops`, shows `248 max exceeded`.
- 180 files in `/samples`, 100 in `/loops`: imports 180 `/samples` and first
  68 fitting `/loops`, shows `248 max exceeded`.
- 80 files in `/samples`, 80 in `/loops`: imports all 160, no count warning.

### Flash Limit Examples

- 20 `/samples` fit and 10 `/loops` fit: imports all, no warning.
- Some `/samples` are too large: skips the oversized files, keeps trying later
  `/samples`, shows `Flash exceeded`.
- `/samples` consume nearly all flash: imports only `/loops` that fit in the
  remaining space, shows `Flash exceeded` if any loop is skipped.
- Both file count and flash capacity are exceeded: imports according to the
  sample-first policy, then shows both warning messages sequentially.

### Display Label Examples

- Sample index `0` displays as `s0`.
- Sample index `99` displays as `s99`.
- Sample index `100` displays as `t0`.
- Sample index `199` displays as `t99`.
- Sample index `200` displays as `u0`.
- Sample index `247` displays as `u47`.

## Verification Plan

1. Build STM:

```bash
make -C mainboard/LxrStm32 -j4 stm32
```

2. Build AVR:

```bash
make -C front/LxrAvr avr -j4
```

3. Build full firmware:

```bash
make firmware
```

4. Hardware tests:

- `/samples` only, fewer than 248, fits flash: no warning, all imported.
- `/samples + /loops` total 249+, fits flash: `248 max exceeded`.
- Total fewer than 248, too much PCM data: `Flash exceeded`.
- Total 249+ and too much PCM data: both warnings, each one second.
- Confirm sample count after upload equals actual imported count, not source
  file count.
- Confirm waveform display labels at sample indexes 99, 100, 199, 200, and 247.
- Confirm selecting `t0`, `t99`, `u0`, and `u47` plays the intended imported
  sample.

## Residual Risk

This plan keeps the existing STM-side SD import model. It fixes the Phase 3
count abort and adds warning feedback, but it does not redesign SD bus ownership
or add a timeout to the blocking AVR wait.

Standard external MIDI data values are still 7-bit. The front panel, preset
storage, STM live parameter path, automation storage, and user sample playback
use byte values, but external MIDI CC/NRPN control above waveform value 127
should be treated as out of scope for this plan unless a separate MIDI encoding
extension is designed.

## Implementation Notes

- STM SD/sample import/protocol changes applied: SD source counts now use
  `uint16_t`, sample metadata reserves the 248-entry table at `0x080F9400`,
  `sampleMemory_loadSamples()` returns truncation flags, and STM sends
  `SAMPLE_CC / FRONT_SAMPLE_UPLOAD_RESULT` instead of a raw ACK.
- Blocker found before AVR implementation: the normal AVR receive parser treats
  any byte with bit 7 set as a new status byte, so the existing
  `SAMPLE_CC / SAMPLE_COUNT / data2` reply cannot carry committed sample counts
  from 128 through 248. The 248 import cap therefore needs an explicit
  architecture decision for how the committed count is encoded back to AVR.
