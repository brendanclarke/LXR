# SAMPLE_PLAN_PHASE_4: Sample Name UI, Storage, and UART Opcodes

## Scope

This phase displays imported sample names on the AVR single-parameter view.

Relevant current code:

- `front/LxrAvr/Menu/MenuText.h`: built-in waveform names; `waveformNames[0][0] == 6`.
- `front/LxrAvr/Menu/menu.c`: `getMenuItemNameForValue()` currently renders user samples as `sN`.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`: AVR protocol constants.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`: AVR receive parser.
- `front/LxrAvr/avrComms/avrCommsSendingProtocol.c/.h`: AVR send helpers.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h/.c`: STM receive parser.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h`: STM send helpers.
- `mainboard/LxrStm32/src/SampleRom/SampleMemory.c/.h`: sample metadata source.

## Audit Result

The plan is right that AVR cannot directly read STM sample metadata. The opcode choice must be corrected:

- Use `SEQ_CC` subcommands `0x5e` and `0x5f` only if both AVR and STM define them in the sequencer command namespace.
- Do not define a top-level status byte `0x5e`; that would conflict with the MIDI-shaped parser because status bytes are expected to have bit 7 set.
- `OSC_SAMPLE_START` is not `100`. The current waveform user-sample boundary is `waveformNames[0][0]`, which is `6`, matching STM `OSC_SAMPLE_START 0x06`.

Also, current `SampleInfo.name` stores only three characters. Showing an 8-character sample name requires a metadata format extension in Phase 3, not just a UART request.

Post-Phase-1 hardware test callout: the AVR can show a stale `s1` waveform label when only `s0` is loaded. The menu range is correct for fresh edits, but `menu_setNumSamples()` only stores the new count and does not clamp existing waveform parameter values. `getMenuItemNameForValue()` then renders any value at or above the built-in waveform count as `sN`, even if `N >= menu_numSamples`. Phase 4 should make out-of-range sample labels impossible or visibly invalid.

## Exact Code Changes

### 1. Add shared sequencer subcommands

Targets:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`

Uncomment/reuse the free gap at `0x5e/0x5f`:

```c
#define SEQ_REQUEST_SAMPLE_NAME 0x5e
#define SEQ_REPORT_SAMPLE_NAME  0x5f
```

STM naming:

```c
#define FRONT_SEQ_REQUEST_SAMPLE_NAME 0x5e
#define FRONT_SEQ_REPORT_SAMPLE_NAME  0x5f
```

Why each line needs to happen:

- The project already reserves `SEQ_CC` / `FRONT_SEQ_CC` as the control status byte.
- `0x5e` and `0x5f` are currently commented legacy cache opcodes, so reusing them must be documented as intentional.
- Keeping prefix style local (`SEQ_` on AVR, `FRONT_SEQ_` on STM) matches current naming conventions.

Risk:

- Old firmware builds using the legacy PRF cache opcodes would not be protocol-compatible. Current memory says that cache path is intentionally disabled.

### 2. Extend sample metadata for display names

Target: `mainboard/LxrStm32/src/SampleRom/SampleMemory.h`

Do not expand `SampleInfo`; it is stored in the fixed metadata area. Instead add a parallel name table after the `SampleInfo` block, or steal from `SAMPLE_INFO_SIZE` only after calculating capacity.

Recommended exact addition:

```c
#define SAMPLE_DISPLAY_NAME_LEN 8u
#define SAMPLE_DISPLAY_NAME_START_ADDRESS \
   (SAMPLE_INFO_START_ADDRESS + (SAMPLE_MAX_COUNT * sizeof(SampleInfo)))
```

Add:

```c
void sampleMemory_getDisplayName(uint8_t index, char* out);
uint8_t sampleMemory_writeDisplayNames(const char names[][SAMPLE_DISPLAY_NAME_LEN],
                                       uint8_t count);
```

Why each line needs to happen:

- A parallel table preserves `SampleInfo`'s 12-byte layout.
- `SAMPLE_DISPLAY_NAME_LEN` matches the UI requirement and keeps wire payload fixed.
- `sampleMemory_getDisplayName()` gives STM protocol code one safe lookup function.

Risk:

- `SAMPLE_INFO_SIZE` is `0x190` bytes. `50 * 12 = 600`, which is larger than `0x190`. The existing comments are stale and already inconsistent with the actual struct size. Before implementing this phase, Phase 1/3 must recalculate metadata region capacity and either enlarge it downward or reduce `SAMPLE_MAX_COUNT`. This is a serious callout.

### 3. Add STM sample-name reply helper

Target: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h`

Add:

```c
void frontPanelSending_sendSampleName(uint8_t index, const char* name);
```

Target: `frontPanelSendingProtocol.c`

Add:

```c
void frontPanelSending_sendSampleName(uint8_t index, const char* name)
{
   uint8_t i;

   frontPanelSending_sendByte(FRONT_SEQ_CC);
   frontPanelSending_sendByte(FRONT_SEQ_REPORT_SAMPLE_NAME);
   frontPanelSending_sendByte(index & 0x7f);

   for(i = 0; i < SAMPLE_DISPLAY_NAME_LEN; i++)
      frontPanelSending_sendByte((uint8_t)(name[i] & 0x7f));
}
```

Why each line needs to happen:

- The first three bytes identify the reply and the sample index it belongs to.
- `index & 0x7f` keeps the payload data-byte safe.
- Masking name bytes to 7 bits avoids accidental parser status bytes.

Risk:

- This is a variable-length reply in a parser mostly built around triplets. AVR receive code must enter a small "sample name payload" mode after seeing `SEQ_REPORT_SAMPLE_NAME`.

### 4. Handle request on STM

Target: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Inside `frontParser_handleSeqCC()` add:

```c
case FRONT_SEQ_REQUEST_SAMPLE_NAME:
{
   char name[SAMPLE_DISPLAY_NAME_LEN];
   uint8_t index = frontParser_command.data2;

   sampleMemory_getDisplayName(index, name);
   frontPanelSending_sendSampleName(index, name);
   break;
}
```

Why each line needs to happen:

- Requests are control commands, so they belong in the `FRONT_SEQ_CC` switch, not the top-level parser switch.
- `data2` carries the sample index because `data1` is the subcommand.
- The send helper keeps packet formatting out of receive logic.

Risk:

- If a request arrives during flash import, metadata may be changing. Return blanks or reject while `sampleImportReceiver` is active.

### 5. Add AVR cache and request helper

Target: `front/LxrAvr/avrComms/avrCommsSendingProtocol.h`

Add:

```c
void avrComms_requestSampleName(uint8_t index);
const char* avrComms_getSampleNameCache(uint8_t index);
void avrComms_storeSampleName(uint8_t index, const char* name);
```

Target: `avrCommsSendingProtocol.c`

Add:

```c
static char avrComms_sampleNameCache[SAMPLE_DISPLAY_NAME_LEN];
static uint8_t avrComms_sampleNameCacheIndex = 0xffu;

void avrComms_requestSampleName(uint8_t index)
{
   if(index == avrComms_sampleNameCacheIndex)
      return;

   avrComms_sendData(SEQ_CC, SEQ_REQUEST_SAMPLE_NAME, index & 0x7f);
}

const char* avrComms_getSampleNameCache(uint8_t index)
{
   if(index != avrComms_sampleNameCacheIndex)
      return 0;

   return avrComms_sampleNameCache;
}

void avrComms_storeSampleName(uint8_t index, const char* name)
{
   memcpy(avrComms_sampleNameCache, name, SAMPLE_DISPLAY_NAME_LEN);
   avrComms_sampleNameCacheIndex = index;
}
```

Why each line needs to happen:

- A one-entry cache is enough for single-parameter edit display and avoids repeated UART spam.
- `0xff` is an invalid index sentinel.
- Returning `0` lets menu display fall back to `sN` until the reply arrives.

Risk:

- Add `#include <string.h>` and define `SAMPLE_DISPLAY_NAME_LEN` on AVR too, preferably in the protocol header.

### 6. Parse AVR sample-name replies

Target: `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

Add receive state:

```c
static uint8_t avrComms_sampleNameRxActive;
static uint8_t avrComms_sampleNameRxIndex;
static uint8_t avrComms_sampleNameRxPos;
static char avrComms_sampleNameRxBuf[SAMPLE_DISPLAY_NAME_LEN];
```

When `SEQ_CC, SEQ_REPORT_SAMPLE_NAME, index` arrives:

```c
avrComms_sampleNameRxActive = 1u;
avrComms_sampleNameRxIndex = avrCommsParser_command.data2;
avrComms_sampleNameRxPos = 0u;
```

In the raw byte parser, before normal triplet assembly:

```c
if(avrComms_sampleNameRxActive)
{
   avrComms_sampleNameRxBuf[avrComms_sampleNameRxPos++] = (char)(data & 0x7f);
   if(avrComms_sampleNameRxPos >= SAMPLE_DISPLAY_NAME_LEN)
   {
      avrComms_storeSampleName(avrComms_sampleNameRxIndex,
                               avrComms_sampleNameRxBuf);
      avrComms_sampleNameRxActive = 0u;
      menu_repaintAll();
   }
   return;
}
```

Why each line needs to happen:

- The name payload is longer than one command triplet.
- The parser must consume those bytes as payload, not as new status/data bytes.
- Repainting after the cache update refreshes the single-parameter view.

Risk:

- If a malformed payload is interrupted by a real status byte, this simple parser will consume it as name text. Add a timeout or abort on bytes with bit 7 set if that becomes an issue.

### 7. Update menu display

Target: `front/LxrAvr/Menu/menu.c`

Change `getMenuItemNameForValue()` user-sample branch:

```c
const uint8_t firstUserSample = waveformNames[0][0];

if(curParmVal < firstUserSample)
   p = waveformNames[curParmVal + 1];
else
{
   const uint8_t sampleIndex = (uint8_t)(curParmVal - firstUserSample);
   const char* cachedName = avrComms_getSampleNameCache(sampleIndex);

   if(cachedName)
   {
      memcpy(buf, cachedName, 3);
      return;
   }

   avrComms_requestSampleName(sampleIndex);
   buf[0] = 's';
   buf[2] = ' ';
   numtostru(&buf[1], sampleIndex);
   return;
}
```

For single-parameter edit mode at `menu_repaintGeneric()`, when `DTYPE_MENU` and `MENU_WAVEFORM` and user sample, copy up to 8 chars into `editDisplayBuffer[1][0]` instead of the 3-char right edge.

Why each line needs to happen:

- `waveformNames[0][0]` is the real AVR sample boundary.
- `sampleIndex >= menu_numSamples` should display `---` or clamp to the highest valid sample instead of showing a nonexistent `sN`.
- The fallback preserves current behavior while the UART reply is pending.
- The request is asynchronous, so the UI never blocks in repaint.

Risk:

- Do not request names from the four-column non-edit view for every repaint; that can flood UART. Restrict full 8-char requests to edit mode, or rate-limit requests.

### 8. Clamp waveform values when sample count changes

Target: `front/LxrAvr/Menu/menu.c`

Update `menu_setNumSamples(uint8_t num)` to call a helper that clamps waveform menu values against the new sample count:

```c
static void menu_clampWaveformValue(uint8_t* value)
{
   const uint8_t firstUserSample = waveformNames[0][0];
   const uint8_t maxEntries = (uint8_t)(firstUserSample + menu_numSamples);

   if(maxEntries == 0u)
      *value = 0u;
   else if(*value >= maxEntries)
      *value = (uint8_t)(maxEntries - 1u);
}
```

Apply it to every live waveform parameter and matching morph endpoint that uses `MENU_WAVEFORM`.

Why each line needs to happen:

- Updating the count changes the valid value range for oscillator waveform parameters immediately.
- Clamping prevents stale preset/UI values from continuing to display as nonexistent samples such as `s1`.
- Applying the same clamp to morph endpoint values prevents shift/morph editing from reintroducing an out-of-range sample value.

Risk:

- Do not clamp LFO waveform menus; they use `MENU_LFO_WAVES`, not `MENU_WAVEFORM`.

## Interdependencies

- Phase 3 must decide where 8-char display names are stored.
- Phase 1 must make metadata reads fail closed after interrupted imports.
- Phase 2 should fix STM audio bounds first so out-of-range UI values cannot drive invalid metadata reads.
- Phase 4 should not be implemented before the sample import format is stable.

## Plan Callouts

- The older `OSC_SAMPLE_START 100` suggestion is wrong for this codebase.
- The proposed `CMD_REQUEST_SAMPLE_NAME` as a top-level opcode is not compatible with the existing MIDI-shaped parser.
