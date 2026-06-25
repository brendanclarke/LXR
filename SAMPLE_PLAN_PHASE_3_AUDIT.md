# SAMPLE_PLAN_PHASE_3_AUDIT

## Updated Goal

Replace the failing "abort when combined count exceeds 50" behavior with a
partial-import policy:

- Import at most 120 combined entries.
- Scan/import `/samples` first.
- Then scan/import `/loops` into remaining count and flash space.
- If more than 120 `.WAV` files are available, import the first files that fit
  this policy and report `120 max exceeded`.
- If PCM data does not fit before the metadata table, skip files that do not
  fit, keep trying later files, and report `Flash exceeded`.
- If both limits are exceeded, report both warnings.
- AVR load screen displays each warning for one second:

```text
Sample Load
120 max exceeded
```

```text
Sample Load
Flash exceeded
```

The warning must not mean import failure. It means the import completed with
truncation.

## Design Summary

The fix needs both STM and AVR changes. The STM owns the actual import and must
return result flags. The AVR owns the LCD and must display those flags.

The current one-byte ACK is not enough because it cannot report "count cap hit"
or "flash cap hit". Replace the sample-upload ACK with a normal three-byte
sample-control result packet:

```text
SAMPLE_CC, SAMPLE_UPLOAD_RESULT, statusFlags
```

`statusFlags` is a bitfield:

```c
SAMPLE_UPLOAD_STATUS_OK          0x00
SAMPLE_UPLOAD_STATUS_COUNT_LIMIT 0x01
SAMPLE_UPLOAD_STATUS_FLASH_LIMIT 0x02
```

The AVR sample-load menu waits for this exact packet instead of calling the
legacy raw `uart_waitAck()`. This avoids the stale-byte ACK race documented in
the earlier audit.

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
// uint16_t is required so counts above 120 are detected instead of wrapping.
uint16_t sd_getNumSamples(void);

// Scans /loops after sdManager_init() has scanned /samples.
// Missing /loops remains non-fatal; the loop count stays at zero.
void sdManager_countLoopFolder(void);

// Returns only the number of files found in /samples.
// This is the boundary used to tag later entries as looped.
uint16_t sd_getNumOneShotSamples(void);

// Selects the active file by combined index.
// uint16_t lets the importer skip oversized files and keep scanning past 255.
void sd_setActiveSample(uint16_t sampleNr);
```

### Why Each Line Changes

- `uint16_t sd_getNumSamples(void);`: count-cap detection must work for totals
  above `255`, and certainly above `120`.
- `uint16_t sd_getNumOneShotSamples(void);`: the one-shot/loop boundary must
  use the same width as the total count.
- `void sd_setActiveSample(uint16_t sampleNr);`: partial import may need to scan
  beyond the 120 loaded entries when earlier files are skipped for size.

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
   // Return the real combined count so the importer can detect >120 cleanly.
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
   FRESULT res;              // FatFs result for directory/file operations.
   uint16_t currentSample = 0; // Counts matching .WAV files within the selected folder.
   const char* folder;       // Points to either "/samples" or "/loops".
   uint16_t localIndex;      // Index inside the selected folder, not combined index.
```

No other logic in `sd_setActiveSample()` needs to change. The existing folder
selection remains correct:

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

### Why Each Line Changes

- Counter globals become `uint16_t` because wrapping would hide the exact
  "more than 120" condition.
- `sd_getNumSamples()` stops clamping to `255` because the importer, not the SD
  manager, owns the `120` policy.
- `sd_setActiveSample()` accepts `uint16_t` so the importer can keep scanning
  after skipped entries.

---

## 3. `SampleMemory.h`

### Replace Stale Metadata Layout Comment

Current comments still describe the old 50-entry/0x190-byte table and the
incorrect "7 byte" struct assumption. Replace that comment block with:

```c
// Sample flash is split into:
//   [SAMPLE_ROM_START_ADDRESS]          32-bit committed sample count
//   [SAMPLE_ROM_START_ADDRESS + 4 ...]  packed PCM data
//   [SAMPLE_INFO_START_ADDRESS ...]     SampleInfo metadata table
//
// SampleInfo is compiler-padded to 12 bytes on STM32:
//   char name[3] + 1 byte padding + uint32_t size + uint32_t offset.
// 120 entries require 1440 bytes (0x5A0), so reserve 0x600 bytes.
```

Why: the old comment was already called out by Phase 1 as stale, and it becomes
actively misleading when the table grows to 120 entries.

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
// Reserve 0x600 bytes for metadata: 120 padded SampleInfo entries need 0x5A0.
#define SAMPLE_INFO_SIZE                ((uint32_t)0x00000600u)

// Move metadata down from 0x080F9E70 so 120 entries cannot overlap PCM data.
#define SAMPLE_INFO_START_ADDRESS       ((uint32_t)0x080F9A00)

// PCM space runs from the count word at SAMPLE_ROM_START_ADDRESS up to metadata.
#define SAMPLE_ROM_SIZE                 ((uint32_t)(SAMPLE_INFO_START_ADDRESS - SAMPLE_ROM_START_ADDRESS))

// New combined /samples + /loops import cap requested by the user.
#define SAMPLE_MAX_COUNT                ((uint8_t)120)
```

### Add Upload Status Flags After `SAMPLE_INFO_SIZE_MASK`

Add:

```c
// No truncation occurred during the latest sample upload.
#define SAMPLE_UPLOAD_STATUS_OK          ((uint8_t)0x00u)

// More than SAMPLE_MAX_COUNT files were available; only the first fitting 120 load.
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

### Why Each Line Changes

- `SAMPLE_INFO_SIZE` grows because the current 0x190-byte metadata area only
  covered the 50-entry design.
- `SAMPLE_INFO_START_ADDRESS` moves down by enough space for 120 entries.
- `SAMPLE_ROM_SIZE` becomes derived from addresses so it cannot drift again.
- `SAMPLE_MAX_COUNT` becomes 120.
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
// Must match SAMPLE_INFO_START_ADDRESS so PCM writes stop before the 120-entry table.
#define FLASH_IF_SAMPLE_INFO_START_ADDRESS ((uint32_t)0x080F9A00)
```

### Why This Changes

`FLASH_If_WriteSamplePcm()` uses this macro as the upper PCM write limit. If it
stays at the old address, PCM writes could overwrite the enlarged metadata
table.

---

## 5. `SampleMemory.c`

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

Replace the current function with:

```c
#define BLOCKSIZE 2
uint8_t sampleMemory_loadSamples(void)
{
   // Metadata staging table sized to the new combined import cap.
   SampleInfo info[SAMPLE_MAX_COUNT];

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

   // Real combined candidate count, not clamped to 120.
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

   // Clear all staged metadata so skipped entries cannot leak stale stack bytes.
   memset(info, 0, sizeof(info));

   // Iterate candidates in existing combined order: all /samples first, then /loops.
   for(i = 0u; i < totalFound; i++)
   {
      // Once the 120-entry table is full, stop; remaining files cannot be imported.
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
      if(sampleWords > (pcmCapacityWords - addr))
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

### Removed Behavior

Delete this old abort block:

```c
if(numSamples==0 || numSamples > SAMPLE_MAX_COUNT)
{
   spi_deInit();
   return;
}
```

Reason: `numSamples > SAMPLE_MAX_COUNT` is now a truncation condition, not a
hard abort.

### Why The New Function Works

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

### Why This Changes

The AVR needs a structured result packet. A normal `SAMPLE_CC` packet avoids
the old raw ACK ambiguity.

---

## 7. `frontPanelReceivingProtocol.c`

### Replace Sample Upload Handler Body

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

### Why Each Line Changes

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

### Why This Changes

The old helper could only say "done". The new helper reports "done, possibly
with truncation warnings".

---

## 9. `frontPanelSendingProtocol.c`

### Replace Upload ACK Helper

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

   // data2 is a 7-bit-safe bitfield of SAMPLE_UPLOAD_STATUS_* warning flags.
   frontPanelSending_sendByte((uint8_t)(statusFlags & (SAMPLE_UPLOAD_STATUS_COUNT_LIMIT |
                                                       SAMPLE_UPLOAD_STATUS_FLASH_LIMIT)));
}
```

### Why This Changes

`ACK == 1` can collide with ordinary data bytes. A three-byte packet is
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
#define SAMPLE_UPLOAD_STATUS_COUNT_LIMIT ((uint8_t)0x01u) // More than 120 candidates existed.
#define SAMPLE_UPLOAD_STATUS_FLASH_LIMIT ((uint8_t)0x02u) // One or more candidates did not fit flash.
```

### Why This Changes

The AVR menu code needs symbolic names for the result packet and warning bits.

---

## 11. `menu.c` On AVR

### Add Static Helper Prototypes Near Existing Static Prototypes

After:

```c
static void menu_normalizeDtypeMenuEncoderValue(uint16_t paramNr, uint8_t *paramValue);
```

Add:

```c
// Waits for the STM sample-upload result packet and returns SAMPLE_UPLOAD_STATUS_* flags.
static uint8_t menu_waitSampleUploadResult(void);

// Displays one bottom-line sample-load warning for exactly one second.
static void menu_showSampleLoadWarning(const char *line2);

// Displays all warning messages requested by the STM result flags.
static void menu_showSampleLoadWarnings(uint8_t statusFlags);
```

### Add Helper Implementations Before `menu_handleLoadMenu()`

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

   while(1)
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
         {
            return data;
         }

         // If a stale count reply is encountered, consume it usefully.
         if(sampleCommand == SAMPLE_COUNT)
         {
            menu_setNumSamples(data);
         }

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
   lcd_home();
   lcd_string_F(PSTR("Sample Load"));

   // Bottom line is one of the exact requested warning strings.
   lcd_setcursor(0,2);
   lcd_string_F(line2);

   // Hold the warning long enough to read, then let the normal repaint resume.
   _delay_ms(1000);
}

static void menu_showSampleLoadWarnings(uint8_t statusFlags)
{
   // If the 120-entry cap was exceeded, show the exact requested message.
   if(statusFlags & SAMPLE_UPLOAD_STATUS_COUNT_LIMIT)
   {
      menu_showSampleLoadWarning(PSTR("120 max exceeded"));
   }

   // If flash capacity was exceeded, show the exact requested message.
   if(statusFlags & SAMPLE_UPLOAD_STATUS_FLASH_LIMIT)
   {
      menu_showSampleLoadWarning(PSTR("Flash exceeded"));
   }
}
```

### Replace The `SAVE_TYPE_SAMPLES` Branch

Current branch:

```c
case SAVE_TYPE_SAMPLES:
   spi_deInit();

   //Display load message
   lcd_clear();
   lcd_home();
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
      else
      {

      }
   }
   //re-initialize SD-Card
   preset_init();
   //redraw screen
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
   lcd_home();
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

### Why Each Line Changes

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
- 130 files in `/samples`, 20 in `/loops`: imports first 120 `/samples`,
  imports no `/loops`, shows `120 max exceeded`.
- 80 files in `/samples`, 80 in `/loops`: imports 80 `/samples` and first
  40 `/loops`, shows `120 max exceeded`.

### Flash Limit Examples

- 20 `/samples` fit and 10 `/loops` fit: imports all, no warning.
- Some `/samples` are too large: skips the oversized files, keeps trying later
  `/samples`, shows `Flash exceeded`.
- `/samples` consume nearly all flash: imports only `/loops` that fit in the
  remaining space, shows `Flash exceeded` if any loop is skipped.
- Both file count and flash capacity are exceeded: imports according to the
  sample-first policy, then shows both warning messages sequentially.

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

- `/samples` only, fewer than 120, fits flash: no warning, all imported.
- `/samples + /loops` total 121+, fits flash: `120 max exceeded`.
- Total fewer than 120, too much PCM data: `Flash exceeded`.
- Total 121+ and too much PCM data: both warnings, each one second.
- Confirm sample count after upload equals actual imported count, not source
  file count.

## Residual Risk

This plan keeps the existing STM-side SD import model. It fixes the Phase 3
count abort and adds warning feedback, but it does not redesign SD bus ownership
or add a timeout to the blocking AVR wait. A future hardening pass should still
replace indefinite waits with timeouts.
