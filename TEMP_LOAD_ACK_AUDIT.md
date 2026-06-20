# TEMP_LOAD_ACK_AUDIT.md - Background File Load Acknowledge Loop

**Session**: 028 planning pass  
**Status**: Implemented in code; build verified  
**Scope**: First handshake step only. The STM will acknowledge after a dummy async delay. The later preset/pattern snapshot into temporary storage is intentionally out of scope for this pass.

## Implementation Notes

- Implemented the first-step acknowledge loop in the targeted AVR/STM files listed below.
- The STM done reply uses `frontPanelSending_sendPriorityTriplet()` rather than ordinary `frontPanelSending_sendTriplet()`. This is required because `.ALL` and `.PRF` handshakes happen inside the load-session quiet UI bracket, and ordinary front-panel sends are suppressed while `frontParser_isQuietUi()` is true.
- No Preset or Pattern temp snapshot logic was added in this pass.
- Verification passed with `make -C front/LxrAvr avr -j4`, `make -C mainboard/LxrStm32 -j4 stm32`, and `make firmware`.

## Goal

When the AVR starts a file load whose type matches the global `PAR_FILE_LOAD_BACKGROUND` mode, it must pause before sending the normal file-transfer envelope. The AVR sends a new background-swap begin command, shows `Bckgrnd Swap...`, polls the receive parser, and proceeds only after the STM sends a matching done reply or the AVR timeout expires.

For this first step, the STM does not yet swap/copy preset or pattern data. It records the requested file type, waits for a dummy 2-second async timer in the main loop, then replies done. This creates the protocol shape needed for the next dev step, where the dummy timer body can become "copy the current audible preset parameters and pattern data into temp storage."

## Current Code Facts This Plan Uses

- AVR file-load initiators live in `front/LxrAvr/Preset/presetManager.c`.
- The file type enum is local to `presetManager.c` near the top:
  - `WTYPE_PATTERN = 7`
  - `WTYPE_PERFORMANCE = 8`
  - `WTYPE_ALL = 9`
  - `WTYPE_KIT = 0`
- `.PAT`, `.ALL`, and `.PRF` loads all send `SEQ_FILE_BEGIN` only after the file opens and header/name data is read.
- `.ALL` and `.PRF` already call `avrComms_flowBeginSession()` before they open/read the file, then set `avrCommsParser_rxDisable = 1`. The new done reply therefore must be explicitly allowed through the AVR receive parser while `rxDisable` is active.
- `.PAT` does not use the load-session flow bracket, but it also sets `avrCommsParser_rxDisable = 1`.
- STM front-panel receive/session state is already owned by `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`.
- STM front-panel send helpers already include ordinary and priority triplet helpers. The done reply must use priority output so it can bypass quiet UI during `.ALL/.PRF` load-session flow.
- STM `systick_ticks` increments at 4 kHz because `main.c` configures `SysTick_Config(RCC_Clocks.HCLK_Frequency / 4000)`.
- AVR `time_sysTick` is a 16-bit tick with 16.384 ms units.

## Background-Load Type Matching

`PAR_FILE_LOAD_BACKGROUND` is the 5-state load-page selector from Session 023:

| Value | Display | Meaning |
|-------|---------|---------|
| 0 | `off` | no background handshakes |
| 1 | `pat` | `.PAT` only |
| 2 | `prf` | `.PRF` / performance only |
| 3 | `all` | `.ALL` only |
| 4 | `tot` | `.PAT`, `.PRF`, and `.ALL` |

The first implementation should add AVR-local aliases in `presetManager.c`:

```c
#define BACKGROUND_OFF 0
#define BACKGROUND_PAT 1
#define BACKGROUND_PRF 2
#define BACKGROUND_ALL 3
#define BACKGROUND_TOT 4
```

`preset_backgroundSwapNeeded(uint8_t fileType)` should return true for:

- `BACKGROUND_PAT`: `fileType == WTYPE_PATTERN`
- `BACKGROUND_PRF`: `fileType == WTYPE_PERFORMANCE`
- `BACKGROUND_ALL`: `fileType == WTYPE_ALL`
- `BACKGROUND_TOT`: `fileType == WTYPE_PATTERN || fileType == WTYPE_PERFORMANCE || fileType == WTYPE_ALL`

`.SND` / kit loads (`WTYPE_KIT`) do not match any mode in this first pass.

## New Protocol Messages

### AVR to STM: Background Swap Begin

```text
status = SEQ_CC / FRONT_SEQ_CC = 0xb2
data1  = SEQ_BACKGROUND_SWAP_BEGIN / FRONT_SEQ_BACKGROUND_SWAP_BEGIN = 0x6d
data2  = file type: WTYPE_PATTERN=7, WTYPE_PERFORMANCE=8, WTYPE_ALL=9
```

Sent after the load file has been opened and validated, but before `SEQ_FILE_BEGIN`.

### STM to AVR: Background Swap Done

```text
status = SEQ_CC / FRONT_SEQ_CC = 0xb2
data1  = SEQ_BACKGROUND_SWAP_DONE / FRONT_SEQ_BACKGROUND_SWAP_DONE = 0x6e
data2  = echoed file type
```

The AVR should only treat the reply as complete if the echoed type matches the type it is waiting for.

## Opcode Definitions

`0x6d` and `0x6e` are free immediately after the current global morph report opcodes.

### AVR opcode header

File: `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`

Current nearby block:

```c
#define SEQ_REPORT_GLOBAL_MORPH_LSB 0x6b
#define SEQ_REPORT_GLOBAL_MORPH_MSB 0x6c
```

Add directly after it:

```c
#define SEQ_BACKGROUND_SWAP_BEGIN 0x6d
#define SEQ_BACKGROUND_SWAP_DONE  0x6e
```

### STM opcode header

File: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`

Current nearby block:

```c
#define FRONT_SEQ_REPORT_GLOBAL_MORPH_LSB 0x6b
#define FRONT_SEQ_REPORT_GLOBAL_MORPH_MSB 0x6c
```

Add directly after it:

```c
#define FRONT_SEQ_BACKGROUND_SWAP_BEGIN 0x6d
#define FRONT_SEQ_BACKGROUND_SWAP_DONE  0x6e
```

Do not change `SEQ_LOAD_BACKGROUND` / `FRONT_SEQ_LOAD_FAST` at `0x50` in this first pass. That is the existing background-load setting byte path, not the new wait/done handshake.

## AVR Changes

### 1. Add helper state and functions

File: `front/LxrAvr/Preset/presetManager.c`

Add near the current file-local defines after `FEXT_PERF`:

```c
#define BACKGROUND_OFF 0
#define BACKGROUND_PAT 1
#define BACKGROUND_PRF 2
#define BACKGROUND_ALL 3
#define BACKGROUND_TOT 4

#define BACKGROUND_SWAP_TIMEOUT_TICKS 305
```

`305` ticks is approximately 5 seconds at the AVR `time_sysTick` rate (`5 / 0.016384 = 305.17`). Keep this as a named constant rather than introducing a floating expression.

Add file-local wait state near the existing globals/static arrays:

```c
static volatile uint8_t preset_backgroundSwapDone = 0;
static uint8_t preset_backgroundSwapExpectedType = 0;
```

Add a small tick reader because `time_sysTick` is 16-bit on AVR:

```c
static uint16_t preset_backgroundSwapNow(void)
{
   uint16_t now;
   ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
   {
      now = time_sysTick;
   }
   return now;
}
```

Add the match helper:

```c
static uint8_t preset_backgroundSwapNeeded(uint8_t fileType)
{
   uint8_t bgMode = parameter_values[PAR_FILE_LOAD_BACKGROUND];

   if(bgMode == BACKGROUND_PAT)
      return fileType == WTYPE_PATTERN;
   if(bgMode == BACKGROUND_PRF)
      return fileType == WTYPE_PERFORMANCE;
   if(bgMode == BACKGROUND_ALL)
      return fileType == WTYPE_ALL;
   if(bgMode == BACKGROUND_TOT)
   {
      return (fileType == WTYPE_PATTERN)
          || (fileType == WTYPE_PERFORMANCE)
          || (fileType == WTYPE_ALL);
   }

   return 0;
}
```

Add the parser callback function:

```c
void preset_backgroundSwapDoneFromStm(uint8_t fileType)
{
   if(fileType == preset_backgroundSwapExpectedType)
      preset_backgroundSwapDone = 1;
}
```

Add the wait/send helper:

```c
static uint8_t preset_performBackgroundSwapWait(uint8_t fileType)
{
   uint16_t start;

   preset_backgroundSwapDone = 0;
   preset_backgroundSwapExpectedType = fileType;

   lcd_clear();
   lcd_home();
   lcd_string_F(PSTR("Bckgrnd Swap..."));

   avrComms_sendData(SEQ_CC, SEQ_BACKGROUND_SWAP_BEGIN, fileType);
   start = preset_backgroundSwapNow();

   while(!preset_backgroundSwapDone)
   {
      uart_checkAndParse();
      if((uint16_t)(preset_backgroundSwapNow() - start) > BACKGROUND_SWAP_TIMEOUT_TICKS)
         return 0;
   }

   return 1;
}
```

The return value is diagnostic only for this first pass. Callers should proceed with the file load even on timeout so the UI cannot lock permanently.

### 2. Expose only the parser callback

File: `front/LxrAvr/Preset/PresetManager.h`

Add near the other preset load declarations:

```c
void preset_backgroundSwapDoneFromStm(uint8_t fileType);
```

Do not expose `preset_backgroundSwapDone` as a mutable extern. The AVR receive parser already includes `Preset/PresetManager.h`, so a function callback keeps the handshake state owned by `presetManager.c`.

### 3. Accept done while receive-disable is active

File: `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

The parser currently allows all `SEQ_CC` messages to collect `data1` while `avrCommsParser_rxDisable` is set, but after `data2` it only permits flow messages or restore messages through. In the block beginning:

```c
if(avrCommsParser_rxDisable)
{
   if((avrCommsParser_command.status == SEQ_CC) && avrCommsSending_isFlowCommand(...))
      ...
   else if(...)
   {
      /* RESTORE ... */
   }
   else
   {
      return;
   }
}
```

add a pass-through branch for the new done opcode:

```c
else if((avrCommsParser_command.status == SEQ_CC)
        && (avrCommsParser_command.data1 == SEQ_BACKGROUND_SWAP_DONE))
{
   /* Background-load acknowledge: allow processing while file-load rxDisable is active. */
}
```

Then in the existing `SEQ_CC` switch, near the flow and morph-report cases, add:

```c
case SEQ_BACKGROUND_SWAP_DONE:
   preset_backgroundSwapDoneFromStm(avrCommsParser_command.data2);
   break;
```

### 4. Insert the wait before `SEQ_FILE_BEGIN`

File: `front/LxrAvr/Preset/presetManager.c`

#### `preset_loadPattern()`

Current location: after the name read and `preset_workingVersion = 0`, the code closes the file, prints `Loading Patrn`, and sends:

```c
avrComms_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_PATTERN);
```

Insert the wait after `f_close((FIL*)&preset_File);` and before the `Loading Patrn` LCD message:

```c
if(preset_backgroundSwapNeeded(preset_workingType))
   (void)preset_performBackgroundSwapWait(preset_workingType);
```

This is the correct location because `preset_loadPattern()` closes the header read early and its sub-readers reopen the file later.

#### `preset_loadAll()`

Current location: after version validation:

```c
preset_workingVersion = version;
avrComms_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_ALL);
fileBeginSent=1;
```

Insert the wait between `preset_workingVersion = version;` and `SEQ_FILE_BEGIN`.

#### `preset_loadPerf()`

Current location: after version validation:

```c
preset_workingVersion = version;
preset_showLoadingPerf();

avrComms_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_PERFORMANCE);
fileBeginSent=1;
```

Insert the wait between `preset_workingVersion = version;` and `preset_showLoadingPerf();`. The final order should be:

```c
preset_workingVersion = version;
if(preset_backgroundSwapNeeded(preset_workingType))
   (void)preset_performBackgroundSwapWait(preset_workingType);
preset_showLoadingPerf();

avrComms_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_PERFORMANCE);
```

#### `preset_loadDrumset()`

No change. `.SND` / kit load is not a background-swap match in this first pass.

## STM Changes

### 1. Add protocol-owned dummy delay state

File: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Add `globals.h` to the includes if it is not already visible from this file:

```c
#include "globals.h"
```

Add near the existing receive/session state:

```c
#define FRONT_BACKGROUND_SWAP_DELAY_TICKS 8000U

static uint8_t frontParser_backgroundSwapPending = 0;
static uint8_t frontParser_backgroundSwapFileType = 0;
static uint32_t frontParser_backgroundSwapStartTick = 0;
```

`8000` ticks is a 2-second dummy wait at the STM's 4 kHz SysTick rate.

### 2. Add service entry point

File: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Add:

```c
void frontParser_serviceBackgroundSwapAck(void)
{
   if(!frontParser_backgroundSwapPending)
      return;

   if((uint32_t)(systick_ticks - frontParser_backgroundSwapStartTick) < FRONT_BACKGROUND_SWAP_DELAY_TICKS)
      return;

   frontParser_backgroundSwapPending = 0;
   frontPanelSending_sendPriorityTriplet(FRONT_SEQ_CC,
                                         FRONT_SEQ_BACKGROUND_SWAP_DONE,
                                         frontParser_backgroundSwapFileType);
}
```

File: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`

Add the declaration next to the parser entry points:

```c
void frontParser_serviceBackgroundSwapAck(void);
```

### 3. Start the dummy delay from the receive handler

File: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

In `frontParser_handleSeqCC()`, add a case near the existing load/session cases, before `FRONT_SEQ_FILE_BEGIN` handling:

```c
case FRONT_SEQ_BACKGROUND_SWAP_BEGIN:
   frontParser_backgroundSwapPending = 1;
   frontParser_backgroundSwapFileType = frontParser_command.data2;
   frontParser_backgroundSwapStartTick = systick_ticks;
   break;
```

This case should not call any Preset or Pattern copy function yet. The later dev step will replace or extend this body with the actual temp snapshot operation.

### 4. Service the dummy delay from the main loop

File: `mainboard/LxrStm32/src/main.c`

Add the receive-protocol header:

```c
#include "uARTFrontSYX/frontPanelReceivingProtocol.h"
```

In the main loop, after `uart_processFront();` is the most direct location:

```c
uart_processFront();
frontParser_serviceBackgroundSwapAck();
```

This keeps the 2-second wait non-blocking on STM. Audio, MIDI, front-panel receive, morph interpolation, endpoint restore, and sequencer tick continue to run normally while the AVR is waiting.

## Files Changed Summary For The Implementation Pass

| File | Planned change |
|------|----------------|
| `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h` | Add `SEQ_BACKGROUND_SWAP_BEGIN` and `SEQ_BACKGROUND_SWAP_DONE` at `0x6d/0x6e` |
| `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h` | Add `FRONT_SEQ_BACKGROUND_SWAP_BEGIN/DONE` plus `frontParser_serviceBackgroundSwapAck()` declaration |
| `front/LxrAvr/Preset/presetManager.c` | Add background mode aliases, wait state, type-match helper, parser callback, wait loop, and insert wait before `.PAT/.ALL/.PRF` `SEQ_FILE_BEGIN` |
| `front/LxrAvr/Preset/PresetManager.h` | Declare `preset_backgroundSwapDoneFromStm(uint8_t fileType)` |
| `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c` | Let `SEQ_BACKGROUND_SWAP_DONE` pass while `rxDisable` is set and dispatch it to the preset callback |
| `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c` | Add dummy async pending state, begin handler, and service function that sends done via priority triplet |
| `mainboard/LxrStm32/src/main.c` | Include receive protocol header and call `frontParser_serviceBackgroundSwapAck()` each main-loop pass |

## Files Intentionally Not Changed In This First Step

- `mainboard/LxrStm32/src/Preset/*`: no temp parameter snapshot yet.
- `mainboard/LxrStm32/src/Sequencer/Pattern/*`: no temp pattern staging yet.
- `mainboard/LxrStm32/src/Sequencer/sequencer.c/.h`: no new handshake state; this first step belongs to front-panel protocol/load-session plumbing.
- `front/LxrAvr/IO/uart.c/.h` and `mainboard/LxrStm32/src/uARTFrontSYX/Uart.c/.h`: transport stays byte-only.
- `front/LxrAvr/Menu/*`: the background selector already exists.
- `SEQ_LOAD_BACKGROUND` / `FRONT_SEQ_LOAD_FAST` at `0x50`: existing setting-byte path remains untouched.

## Execution Flow

### Non-matching file type

```text
User selects file
-> preset_loadPattern/loadAll/loadPerf()
-> preset_backgroundSwapNeeded() returns 0
-> existing load path sends SEQ_FILE_BEGIN immediately
-> file data transfers as today
```

### Matching file type

```text
User selects file
-> AVR opens/validates file header
-> AVR sends SEQ_BACKGROUND_SWAP_BEGIN + fileType
-> AVR displays "Bckgrnd Swap..." and polls uart_checkAndParse()
-> STM receives FRONT_SEQ_BACKGROUND_SWAP_BEGIN
-> STM records fileType and start tick
-> STM main loop continues running
-> after 2 seconds, STM sends FRONT_SEQ_BACKGROUND_SWAP_DONE + fileType
-> AVR parser accepts done despite rxDisable
-> AVR callback marks the matching wait complete
-> AVR sends the normal SEQ_FILE_BEGIN and file payload
```

### Timeout path

If no matching done reply arrives within roughly 5 seconds, the AVR wait helper returns false and the load continues anyway. This prevents a permanent front-panel lockup while still making the failure visible during testing because the 2-second pause will be absent/extended to timeout.

## Verification After Implementation

Build checks:

```bash
make -C front/LxrAvr avr -j4
make -C mainboard/LxrStm32 -j4 stm32
make firmware
```

Functional checks:

1. Set background load to `off`; load `.ALL`, `.PRF`, and `.PAT`; no `Bckgrnd Swap...` pause should occur.
2. Set to `all`; load `.ALL`; the message should show for about 2 seconds, then normal loading should begin.
3. Set to `all`; load `.PRF` and `.PAT`; no pause.
4. Set to `prf`; load `.PRF`; pause, then normal load.
5. Set to `pat`; load `.PAT`; pause, then normal load.
6. Set to `tot`; load `.ALL`, `.PRF`, and `.PAT`; all three pause, then load.
7. Load `.SND`; no pause in any mode.

## Follow-Up Step

After this acknowledge loop is proven on hardware, replace the STM dummy delay body with the actual pre-load snapshot:

- copy the currently audible preset parameter image into `preset_tmpKitState`;
- copy the currently audible/playing pattern data into pattern-owned temp storage;
- keep `.PAT` pattern-only behavior separate from parameter temp switching;
- send `FRONT_SEQ_BACKGROUND_SWAP_DONE` only after that copy/swap prep is complete.
