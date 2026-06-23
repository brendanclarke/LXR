# SAMPLE_PLAN_PHASE_1: Flash Memory Guards and Writing Improvements

## Scope

This phase audits and rewrites the Phase 1 recommendation from `LXR_SAMPLE_IMPORT_PHASED_PLAN.md` against the current tree. The correct storage target is the STM32F407VGT6 internal flash, not an external SST/W25 SPI NOR device. The external-SPI recommendations in `LXR_SAMPLE_IMPORT_AUDIT.md` and `SAMPLE_IMPORT_OPUS_SUB-AGENT.md` are invalid for this repository and this hardware.

Relevant current code:

- `knowledge_files/hardware_archive/main/STM32F4_HARDWARE.md`: STM32F407VGT6, 1024 KB internal flash, SD card owned by AVR, STM SPI pins floated.
- `mainboard/LxrStm32/src/SampleRom/flash_if.c`: raw internal flash erase/write helpers.
- `mainboard/LxrStm32/src/SampleRom/flash_if.h`: STM32F4 sector addresses.
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`: sample region constants.
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`: current sample flash writer.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`: `SAMPLE_START_UPLOAD` currently calls the blocking sample writer.

## Audit Result

The plan is directionally right that flash writes need guards, but several details must change:

- Do not add `spiFlash_*`, WREN, AAI, BP, or SPI status-register logic. Those files and devices are not used here.
- Do not call `IWDG_ReloadCounter()` unconditionally. This repo does not configure IWDG anywhere; adding watchdog calls without enabling/configuring IWDG is noise at best.
- Do not disable interrupts for an entire multi-sector erase unless the UI explicitly accepts a full audio blackout. Internal STM32F4 flash erase stalls instruction fetch from flash, so audio continuity cannot be preserved during internal flash programming anyway. The safe behavior is to stop sequencer/audio-facing activity, acknowledge that audio will stop during import, and keep flash routines bounded and verifiable.
- The current `FLASH_If_Write()` already verifies each programmed word, but it silently stops at `USER_FLASH_END_ADDRESS - 4` and returns success when the requested length overflows. That must be fixed.
- The current writer stores the sample count early. If power is lost after erase but before a valid table is written, `sampleMemory_getNumSamples()` can read erased flash as `0xff`. The count must be clamped, and the commit order must make invalid metadata fail closed.
- The current write-protection status helper checks from `APPLICATION_ADDRESS` and returns the inverse of its own documented meaning. Because `sampleMemory_init()` calls this helper before import, Phase 1 also needs to keep option-byte handling scoped to sample sectors and avoid launching option-byte programming when sectors are already writable.

## Implementation Notes

Implemented in this session:

- Added sample-region constants and explicit flash result codes in `mainboard/LxrStm32/src/SampleRom/flash_if.h`.
- Added `FLASH_If_IsWordAligned()` and `FLASH_If_RangeIsValid()` in `flash_if.c`. The implemented range helper includes the overflow precheck called out below, plus an `address > limit` guard before computing remaining bytes.
- Changed `FLASH_If_Init()` to clear stale flash flags and then lock flash again. `FLASH_If_Erase()` and `FLASH_If_Write()` now own their own unlock/clear/lock windows.
- Changed `FLASH_If_Erase()` to reject any start address other than `0x08080000`, so this helper cannot be reused accidentally to erase bootloader/application sectors below the sample ROM.
- Changed `FLASH_If_Write()` to validate the whole requested range before programming, return typed status codes, verify each word, and lock flash on every exit path.
- Added `FLASH_If_WriteSamplePcm()` so PCM writes must end before `SAMPLE_INFO_START_ADDRESS`. Metadata and the final count commit still use `FLASH_If_Write()`.
- Fixed `FLASH_If_GetWriteProtectionStatus()` to inspect sectors 8-11 only. On STM32F4 option bytes, a set WRP bit means writable; the old function returned `1` for the writable case, which could cause unnecessary option-byte programming.
- Changed `FLASH_If_DisableWriteProtection()` to disable write protection only for sectors 8-11 and lock option bytes on success or failure.
- Added `SAMPLE_MAX_COUNT` in `SampleMemory.h`, clamped erased/invalid sample counts to zero, rejected oversized SD sample counts, released SPI on the rejected-import path, and moved the visible sample-count commit after PCM and metadata writes.
- Changed `sampleMemory_setNumSamples()` to return the flash-write result. `sampleMemory_loadSamples()` now treats a failed final count write as an aborted import instead of pretending success.
- Added `memset(info, 0, sizeof(info))` before filling metadata so padded bytes in `SampleInfo` are deterministic when written to flash.

Build/test status:

- `make -C mainboard/LxrStm32 -j4 stm32` completed successfully.
- The build still prints unrelated pre-existing warnings in DSP, sequencer, MIDI, and linker output. No new SampleRom compile/link failure was introduced by Phase 1.

Residual risks for hardware test:

- Sample import remains a blocking offline operation. STM32F4 internal flash erase/program can stall instruction fetch from flash, so audio should not be expected to continue cleanly during import.
- `SampleInfo` is commented as 7 bytes in `SampleMemory.h`, but the current C layout is padded by the compiler. Phase 1 writes `sizeof(SampleInfo)` rounded to whole flash words, which matches the actual ABI, but the stale comment and `SAMPLE_INFO_SIZE` math should be revisited in a later phase.
- The linker still allows firmware to grow into sectors 8-11. The current binary links successfully, but a future build-time assertion should reserve the sample-flash range explicitly before relying on field imports.

## Exact Code Changes

### 1. Add sample-flash bounds and status constants in `flash_if.h`

Target: `mainboard/LxrStm32/src/SampleRom/flash_if.h`

Add after `USER_FLASH_END_ADDRESS`:

```c
#define FLASH_IF_SAMPLE_START_ADDRESS      ((uint32_t)0x08080000)
/* First byte of metadata table. PCM writes must stop before this address. */
#define FLASH_IF_SAMPLE_INFO_START_ADDRESS ((uint32_t)0x080F9E70)
#define FLASH_IF_SAMPLE_END_ADDRESS        USER_FLASH_END_ADDRESS

#define FLASH_IF_OK              ((uint32_t)0)
#define FLASH_IF_ERR_OPERATION   ((uint32_t)1)
#define FLASH_IF_ERR_VERIFY      ((uint32_t)2)
#define FLASH_IF_ERR_BOUNDS      ((uint32_t)3)
#define FLASH_IF_ERR_ALIGNMENT   ((uint32_t)4)
```

Why each line needs to happen:

- `FLASH_IF_SAMPLE_START_ADDRESS` duplicates the current `SAMPLE_ROM_START_ADDRESS` without including `SampleMemory.h` from `flash_if.h`, avoiding a circular dependency.
- `FLASH_IF_SAMPLE_INFO_START_ADDRESS` gives the flash layer an explicit boundary between PCM data and metadata so a large import cannot overwrite the table.
- `FLASH_IF_SAMPLE_END_ADDRESS` keeps erase bounds tied to the STM32F4 internal flash end already defined in this header.
- The status constants replace the current ambiguous `0/1/2` convention with a bounds/alignment result that callers can distinguish.

Risk:

- These addresses must remain synchronized with `SampleMemory.h`. Phase 1 should add a comment in both files that these constants are mirrored intentionally.

### 2. Add internal validation helpers in `flash_if.c`

Target: `mainboard/LxrStm32/src/SampleRom/flash_if.c`

Add near the private prototype block:

```c
static uint8_t FLASH_If_IsWordAligned(uint32_t address)
{
   return (uint8_t)((address & 0x03u) == 0u);
}

static uint8_t FLASH_If_RangeIsValid(uint32_t address, uint32_t words, uint32_t limit)
{
   uint32_t bytes = words * 4u;

   if(words == 0u)
      return 1u;
   if(!FLASH_If_IsWordAligned(address))
      return 0u;
   if(address < FLASH_IF_SAMPLE_START_ADDRESS)
      return 0u;
   if(bytes > (limit - address + 1u))
      return 0u;

   return 1u;
}
```

Why each line needs to happen:

- `FLASH_If_IsWordAligned()` is required because STM32F4 `FLASH_ProgramWord()` expects word-aligned writes.
- `bytes = words * 4u` converts the existing word-count API into an address range check.
- `words == 0u` keeps zero-length writes harmless for callers that commit an empty folder.
- `address < FLASH_IF_SAMPLE_START_ADDRESS` prevents accidental writes into firmware sectors.
- `bytes > (limit - address + 1u)` catches overflow before a write begins instead of silently truncating.

Risk:

- The multiplication can overflow if an absurd `words` value is passed. If implementing, add a precheck `if(words > 0x3fffffffu) return 0u;` before `bytes = words * 4u`.

### 3. Harden `FLASH_If_Erase()`

Target: `mainboard/LxrStm32/src/SampleRom/flash_if.c`

Change the top of `FLASH_If_Erase()`:

```c
  if(startAddress != FLASH_IF_SAMPLE_START_ADDRESS)
     return FLASH_IF_ERR_BOUNDS;

  FLASH_Unlock();
  FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                  FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

  uint32_t UserStartSector = GetSector(startAddress), i = 0;
  for(i = UserStartSector; i <= FLASH_Sector_11; i += 8)
  {
     FLASH_Status error = FLASH_EraseSector(i, VoltageRange_3);
     if(error != FLASH_COMPLETE)
     {
        FLASH_Lock();
        return FLASH_IF_ERR_OPERATION;
     }
  }

  FLASH_Lock();
  return FLASH_IF_OK;
```

Why each line needs to happen:

- The exact-start check prevents callers from erasing application sectors by passing an earlier address.
- Unlock/clear inside the function makes erase self-contained; currently it relies on `FLASH_If_Init()` having been called earlier.
- `FLASH_Status` should be used rather than `uint8_t` so the ST driver status is not truncated.
- `FLASH_Lock()` on every exit reduces the window where accidental flash writes can happen.

Risk:

- Erasing sectors 8-11 takes noticeable time and will interrupt audio. This is unavoidable with internal flash if executing from flash. The UI must present sample import as a blocking offline operation.

### 4. Harden `FLASH_If_Write()`

Target: `mainboard/LxrStm32/src/SampleRom/flash_if.c`

Change the function body to validate the entire range before programming:

```c
uint32_t FLASH_If_Write(__IO uint32_t* FlashAddress, uint32_t* Data, uint32_t DataLength)
{
  uint32_t i = 0;

  if(FlashAddress == 0 || Data == 0)
     return FLASH_IF_ERR_OPERATION;
  if(!FLASH_If_RangeIsValid(*FlashAddress, DataLength, FLASH_IF_SAMPLE_END_ADDRESS))
     return FLASH_IF_ERR_BOUNDS;

  FLASH_Unlock();
  FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                  FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

  for(i = 0; i < DataLength; i++)
  {
     FLASH_Status error = FLASH_ProgramWord(*FlashAddress, Data[i]);
     if(error != FLASH_COMPLETE)
     {
        FLASH_Lock();
        return FLASH_IF_ERR_OPERATION;
     }

     if(*(uint32_t*)*FlashAddress != Data[i])
     {
        FLASH_Lock();
        return FLASH_IF_ERR_VERIFY;
     }

     *FlashAddress += 4u;
  }

  FLASH_Lock();
  return FLASH_IF_OK;
}
```

Why each line needs to happen:

- Null checks avoid hard faults from bad callers.
- Whole-range validation prevents the current silent partial write.
- Unlock/clear/lock makes the write lifecycle local and auditable.
- The read-back verification preserves the useful existing safety behavior.

Risk:

- Locking after every small `BLOCKSIZE` write will increase total import time. A later optimization can add a begin/end write session, but correctness should come first.

### 5. Add PCM-specific write helper

Target: `mainboard/LxrStm32/src/SampleRom/flash_if.h` and `.c`

Header:

```c
uint32_t FLASH_If_WriteSamplePcm(__IO uint32_t* FlashAddress,
                                 uint32_t* Data,
                                 uint32_t DataLength);
```

Implementation:

```c
uint32_t FLASH_If_WriteSamplePcm(__IO uint32_t* FlashAddress,
                                 uint32_t* Data,
                                 uint32_t DataLength)
{
   if(FlashAddress == 0)
      return FLASH_IF_ERR_OPERATION;
   if(!FLASH_If_RangeIsValid(*FlashAddress,
                             DataLength,
                             FLASH_IF_SAMPLE_INFO_START_ADDRESS - 1u))
      return FLASH_IF_ERR_BOUNDS;

   return FLASH_If_Write(FlashAddress, Data, DataLength);
}
```

Why each line needs to happen:

- PCM writes need a stricter upper bound than metadata writes.
- The implemented helper also checks word alignment explicitly so callers can distinguish bad address alignment from a valid address that simply exceeds the PCM region.
- Reusing `FLASH_If_Write()` keeps verification and locking behavior in one place.

Risk:

- Existing callers must be updated carefully: PCM data uses `FLASH_If_WriteSamplePcm()`, metadata/count writes use `FLASH_If_Write()`.

### 5a. Fix sample-sector write-protection handling

Target: `mainboard/LxrStm32/src/SampleRom/flash_if.c`

Add a private mask:

```c
#define FLASH_IF_SAMPLE_WRP_MASK (OB_WRP_Sector_8 | OB_WRP_Sector_9 | \
                                  OB_WRP_Sector_10 | OB_WRP_Sector_11)
```

Change `FLASH_If_GetWriteProtectionStatus()`:

```c
uint16_t FLASH_If_GetWriteProtectionStatus(void)
{
   uint16_t sampleWrp = FLASH_OB_GetWRP() & FLASH_IF_SAMPLE_WRP_MASK;

   if(sampleWrp == FLASH_IF_SAMPLE_WRP_MASK)
      return 0;

   return 1;
}
```

Change `FLASH_If_DisableWriteProtection()` to pass `FLASH_IF_SAMPLE_WRP_MASK` to `FLASH_OB_WRPConfig()` and call `FLASH_OB_Lock()` on every exit.

Why each line needs to happen:

- `FLASH_IF_SAMPLE_WRP_MASK` scopes option-byte decisions to sectors 8-11, the only sectors erased by sample import.
- `FLASH_OB_GetWRP() & FLASH_IF_SAMPLE_WRP_MASK` ignores application and bootloader sectors.
- Returning `0` when all sample WRP bits are set matches the documented API meaning: no sample sector is write protected.
- Returning `1` otherwise makes `sampleMemory_init()` call `FLASH_If_DisableWriteProtection()` only when a sample sector is actually protected.
- Locking option bytes after launch reduces the chance of later accidental option-byte writes.

Risk:

- If a unit genuinely ships with sample sectors write-protected, `FLASH_OB_Launch()` may still have hardware-visible side effects while option bytes reload. This is pre-existing behavior, but Phase 1 narrows when it can happen.

### 6. Clamp sample count and commit last in `SampleMemory.c`

Targets:

- `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c`

Add in `SampleMemory.h` near the sample constants:

```c
#define SAMPLE_MAX_COUNT ((uint8_t)50)
```

Change `sampleMemory_getNumSamples()`:

```c
uint8_t sampleMemory_getNumSamples()
{
   uint16_t stored = sampleMemory_data[0];

   if(stored == 0xffffu || stored > SAMPLE_MAX_COUNT)
      return 0;

   return (uint8_t)stored;
}
```

Change `sampleMemory_loadSamples()`:

```c
uint8_t numSamples = sd_getNumSamples();
if(numSamples == 0 || numSamples > SAMPLE_MAX_COUNT)
{
   spi_deInit();
   return;
}

if(FLASH_If_Erase(SAMPLE_ROM_START_ADDRESS) != FLASH_IF_OK)
   return;

/* Do not write the sample count yet. A non-zero count is the commit marker. */
memset(info, 0, sizeof(info));
...
if(FLASH_If_WriteSamplePcm(&add, (uint32_t*)data, BLOCKSIZE / 2) != FLASH_IF_OK)
   return;
...
infoWords = (numSamples * sizeof(SampleInfo) + 3u) / 4u;
if(FLASH_If_Write(&add, (uint32_t*)(info), infoWords) != FLASH_IF_OK)
   return;

if(sampleMemory_setNumSamples(numSamples) != FLASH_IF_OK)
   return;
```

Change `sampleMemory_setNumSamples()` and its declaration:

```c
uint32_t sampleMemory_setNumSamples(uint8_t num)
{
   uint32_t data = num;
   volatile uint32_t add = SAMPLE_ROM_START_ADDRESS;
   return FLASH_If_Write(&add, (uint32_t*)&data, 1);
}
```

Why each line needs to happen:

- `SAMPLE_MAX_COUNT` names the existing table capacity instead of spreading magic `50`.
- The erased-flash clamp prevents `0xff` sample counts after interrupted imports.
- Rejecting `numSamples > SAMPLE_MAX_COUNT` prevents the current stack buffer and metadata overflow.
- Calling `spi_deInit()` before returning from the rejected-import path releases the STM SD/SPI side even when no erase/write happens.
- `memset(info, 0, sizeof(info))` makes any compiler padding bytes deterministic before metadata is programmed.
- Rounding `sizeof(SampleInfo)` up to whole 32-bit words avoids losing the tail bytes of the metadata table.
- Writing count last makes the count act as the visible commit marker for playback.
- Returning the count-write status lets `sampleMemory_loadSamples()` fail closed if the final commit marker cannot be programmed.

Risk:

- If metadata write succeeds and count write fails, no samples are visible. That is safer than exposing partial metadata.

## Hardware Safety Notes

- Internal flash erase/program stalls code fetch on STM32F407. Since audio ISR code lives in flash, sample import cannot be transparent during playback.
- Audio DMA uses priority 0 interrupts (`DMA1_Stream7_IRQn`, `DMA1_Stream4_IRQn`), but those interrupts still cannot fetch handler code during flash erase. Stopping sequencer is not enough to guarantee clean audio; the UI should state this as an offline operation.
- The STM SPI SD path should not be used as-is because `main.c` floats PA6/PA7/PB3/PB0 explicitly to avoid SD bus contention with AVR-owned SD.

## Interdependencies

- Phase 2 depends on `sampleMemory_getNumSamples()` clamping invalid counts.
- Phase 3 depends on `FLASH_If_WriteSamplePcm()` and metadata-last/count-last commit behavior.
- Phase 4 sample-name lookup depends on committed metadata being trustworthy.

## Plan Callouts

- `LXR_SAMPLE_IMPORT_AUDIT.md` is wrong about SST25VF032B/SPI flash for this repo.
- `LXR_SAMPLE_IMPORT_PHASED_PLAN.md` is right to target `flash_if.c`, but it should not use `__disable_irq()` as a magic safety fix. The operation is inherently blocking on internal flash.
