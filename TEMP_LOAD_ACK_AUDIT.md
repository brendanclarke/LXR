# TEMP_LOAD_ACK_AUDIT.md — Background File Load STM Handshake

**Session**: 026 planning pass  
**Status**: Plan only — no code changes made yet

## Goal

When a user selects a file load whose type matches the `PAR_FILE_LOAD_BACKGROUND` global setting, the AVR must pause before sending the usual `SEQ_FILE_BEGIN`/kit/pattern/endpoint traffic. It sends a new "background swap begin" opcode to the STM, waits for the STM to reply "done" (after a 2-second async timer), and only then proceeds with the normal file load. While waiting, the AVR displays "Bckgrnd Swap..." on the LCD. If the file type does not match `PAR_FILE_LOAD_BACKGROUND`, file loads proceed exactly as they do today — no pause, no new handshake.

## Background-Load Type Matching

The AVR global `PAR_FILE_LOAD_BACKGROUND` (Session 023) is a 5-state selector:

| Value | Display | Type enum                | Matching file kind |
|-------|---------|--------------------------|--------------------|
| 0     | off     | `BACKGROUND_OFF`         | (none)             |
| 1     | pat     | `BACKGROUND_PAT`         | `.PAT` only        |
| 2     | prf     | `BACKGROUND_PRF`         | `.PRF` only        |
| 3     | all     | `BACKGROUND_ALL`         | `.ALL` only        |
| 4     | tot     | `BACKGROUND_TOT`         | `.ALL` + `.PRF`    |

The match function `preset_backgroundSwapNeeded(uint8_t fileType)` on AVR returns true when:

- `BACKGROUND_OFF` → always false
- `BACKGROUND_PAT` → fileType == `WTYPE_PATTERN`
- `BACKGROUND_PRF` → fileType == `WTYPE_PERFORMANCE`
- `BACKGROUND_ALL` → fileType == `WTYPE_ALL`
- `BACKGROUND_TOT` → fileType == `WTYPE_ALL` or `WTYPE_PERFORMANCE`

Note: `.SND` (kit/drumset) loads are never background-swapped in the current design (matching none of pat/prf/all/tot).

## New Protocol Messages

### AVR → STM: Background Swap Begin

```
Status:  SEQ_CC (0xb2)
Data1:   SEQ_BACKGROUND_SWAP_BEGIN (new, value 0x6d)
Data2:   file type (WTYPE_ALL=9, WTYPE_PERFORMANCE=8, WTYPE_PATTERN=4)
```

Sent by the AVR after the file is successfully opened and the name read, but **before** `SEQ_FILE_BEGIN`. The AVR then enters a blocking wait that polls `uart_checkAndParse()` until it receives the matching STM reply or a timeout.

### STM → AVR: Background Swap Done

```
Status:  SEQ_CC (0xb2)
Data1:   SEQ_BACKGROUND_SWAP_DONE (new, value 0x6e)
Data2:   echo of the file type the STM processed
```

Sent by the STM after its 2-second async timer fires. The echoed file type lets the AVR confirm this is the reply for the request it sent.

### STM → AVR: Background Swap Abort (optional, future)

```
Status:  SEQ_CC (0xb2)
Data1:   SEQ_BACKGROUND_SWAP_ABORT (new, value 0x6f)
Data2:   0
```

Reserved for a future timeout/error path. Not implemented in the first pass.

## Opcode Value Assignment

Free opcode slots near the existing block (0x5a–0x6c are flow/tmp-kit/morph). Looking at the current AVR opcodes (`avrCommsReceivingProtocol.h`) and STM opcodes (`frontPanelReceivingProtocol.h`):

| Constant                        | Value | Direction  |
|--------------------------------|-------|------------|
| `SEQ_BACKGROUND_SWAP_BEGIN`    | 0x6d  | AVR → STM  |
| `SEQ_BACKGROUND_SWAP_DONE`     | 0x6e  | STM → AVR  |
| `SEQ_BACKGROUND_SWAP_ABORT`    | 0x6f  | STM → AVR  |

0x6d–0x6f are currently unused in both the AVR and STM opcode tables. The AVR defines go in `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`; the STM defines go in `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`.

## Detailed Code Changes

### 1. AVR — New opcode definitions

**File**: `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`

Add after the existing `SEQ_REPORT_GLOBAL_MORPH_MSB` block:

```c
#define SEQ_BACKGROUND_SWAP_BEGIN  0x6d  // AVR->STM: request background-swap pause before file load
#define SEQ_BACKGROUND_SWAP_DONE   0x6e  // STM->AVR: background-swap 2s timer completed, proceed with load
#define SEQ_BACKGROUND_SWAP_ABORT  0x6f  // STM->AVR: background-swap aborted (reserved)
```

### 2. STM — New opcode definitions

**File**: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`

Add after the existing `FRONT_SEQ_REPORT_GLOBAL_MORPH_MSB` block:

```c
#define FRONT_SEQ_BACKGROUND_SWAP_BEGIN  0x6d  // AVR->STM: request background-swap pause before file load
#define FRONT_SEQ_BACKGROUND_SWAP_DONE   0x6e  // STM->AVR: background-swap 2s timer completed, proceed with load
#define FRONT_SEQ_BACKGROUND_SWAP_ABORT  0x6f  // STM->AVR: background-swap aborted (reserved)
```

### 3. AVR — Background-swap type-match helper and handshake state

**File**: `front/LxrAvr/Preset/presetManager.c`

New defines and helper function near the top (before `preset_loadDrumset`), near the existing `VOICE_PARAM_LENGTH` / `NUM_TRACKS` defines (~line 64):

```c
// Value aliases for the PAR_FILE_LOAD_BACKGROUND menu state.
#define BACKGROUND_OFF  0
#define BACKGROUND_PAT  1
#define BACKGROUND_PRF  2
#define BACKGROUND_ALL  3
#define BACKGROUND_TOT  4
```

New static state variable near other static declarations (~line 89):

```c
// Handshake flag: set by the AVR receiving parser when the STM replies
// with SEQ_BACKGROUND_SWAP_DONE.  Polled by preset_performBackgroundSwapWait().
uint8_t preset_backgroundSwapDone = 0;
```

New match function:

```c
// Return 1 if the given file type requires a background-swap handshake
// before the normal file-load sequence begins.
static uint8_t preset_backgroundSwapNeeded(uint8_t fileType)
{
   uint8_t bgMode = parameter_values[PAR_FILE_LOAD_BACKGROUND];
   if(bgMode == BACKGROUND_OFF)
      return 0;
   if(bgMode == BACKGROUND_PAT && fileType == WTYPE_PATTERN)
      return 1;
   if(bgMode == BACKGROUND_PRF && fileType == WTYPE_PERFORMANCE)
      return 1;
   if(bgMode == BACKGROUND_ALL && fileType == WTYPE_ALL)
      return 1;
   if(bgMode == BACKGROUND_TOT &&
      (fileType == WTYPE_ALL || fileType == WTYPE_PERFORMANCE))
      return 1;
   return 0;
}
```

New handshake function:

```c
// Block until the STM responds with SEQ_BACKGROUND_SWAP_DONE or a timeout
// (~5 seconds).  Displays "Bckgrnd Swap..." on LCD while waiting.
// Returns 1 on success, 0 on timeout/abort.
static uint8_t preset_performBackgroundSwapWait(uint8_t fileType)
{
   uint16_t start = time_sysTick;
   preset_backgroundSwapDone = 0;

   // Show the waiting message on LCD
   lcd_clear();
   lcd_home();
   lcd_string_F(PSTR("Bckgrnd Swap..."));

   // Send the begin-request to STM
   avrComms_sendData(SEQ_CC, SEQ_BACKGROUND_SWAP_BEGIN, fileType);

   // Poll until done or timeout (~5 s)
   while(!preset_backgroundSwapDone)
   {
      uart_checkAndParse();
      if((uint16_t)(time_sysTick - start) > (uint16_t)(5 * 1000 / TIMER_TICK_MS))
      {
         // Timeout: proceed anyway rather than locking up
         return 0;
      }
   }
   return 1;
}
```

Note: `preset_backgroundSwapDone` is file-global (not static) so the receiving parser in `avrCommsReceivingProtocol.c` can set it via an extern declaration.

### 4. AVR — Handler for the STM reply in the receive parser

**File**: `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

In the `avrCommsParser_handleSeqCC()` function (or equivalent central `SEQ_CC` dispatch), add a case for the new opcode:

```c
case SEQ_BACKGROUND_SWAP_DONE:
   preset_backgroundSwapDone = 1;
   break;
```

**File**: `front/LxrAvr/Preset/PresetManager.h`

Add extern declaration so the receiving protocol can see the flag:

```c
extern uint8_t preset_backgroundSwapDone;
```

### 5. AVR — Wire the handshake into each file-load function

Each load function needs the check and handshake inserted **after** the file is opened/name is read and **before** `SEQ_FILE_BEGIN` is sent. The insertion pattern is:

```c
if(preset_backgroundSwapNeeded(preset_workingType))
{
   preset_performBackgroundSwapWait(preset_workingType);
}
```

**Specific insertion points:**

#### `preset_loadPerf()` (currently ~line 2473–2476)

Current:
```c
   preset_workingVersion = version;
   preset_showLoadingPerf();                            // shows "Loading Perf"

   avrComms_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_PERFORMANCE);
```
Change to:
```c
   preset_workingVersion = version;

   if(preset_backgroundSwapNeeded(preset_workingType))
   {
      preset_performBackgroundSwapWait(preset_workingType);
   }

   preset_showLoadingPerf();                            // still shows "Loading Perf" after handshake

   avrComms_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_PERFORMANCE);
```

#### `preset_loadAll()` (currently ~line 2277–2278)

Current:
```c
   preset_workingVersion = version;
   avrComms_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_ALL);
```
Change to:
```c
   preset_workingVersion = version;
   if(preset_backgroundSwapNeeded(preset_workingType))
   {
      preset_performBackgroundSwapWait(preset_workingType);
   }
   avrComms_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_ALL);
```

#### `preset_loadPattern()` (currently ~line 1928–1936)

Current:
```c
   preset_workingVersion = 0;

   f_close((FIL*)&preset_File);

   lcd_clear();
   lcd_home();
   lcd_string_F(PSTR("Loading Patrn"));

   avrComms_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_PATTERN);
```
Change to:
```c
   preset_workingVersion = 0;

   f_close((FIL*)&preset_File);

   if(preset_backgroundSwapNeeded(preset_workingType))
   {
      preset_performBackgroundSwapWait(preset_workingType);
   }

   lcd_clear();
   lcd_home();
   lcd_string_F(PSTR("Loading Patrn"));

   avrComms_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_PATTERN);
```

Note: `preset_loadPattern()` calls `f_close()` early (before `SEQ_FILE_BEGIN`) and then individual sub-readers re-open the file. The handshake must slot between the name-read/close and the `SEQ_FILE_BEGIN` send.

#### `preset_loadDrumset()` (kit load) — NO CHANGE

Kit loads (`.SND`) are never matched by `preset_backgroundSwapNeeded()`, so no handshake insertion is needed. The existing flow is preserved.

### 6. STM — Receive handler for Background Swap Begin

**File**: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

In the `frontParser_handleSeqCC()` (or equivalent `SEQ_CC` dispatch), add a case:

```c
case FRONT_SEQ_BACKGROUND_SWAP_BEGIN:
   {
      // Start a 2-second async timer that will fire the done reply.
      // fileType is in frontParser_command.data2.
      seq_startBackgroundSwapTimer(frontParser_command.data2);
   }
   break;
```

### 7. STM — Background-swap timer logic in Sequencer

**File**: `mainboard/LxrStm32/src/Sequencer/sequencer.c`

Add near the timer/tick area (near existing timer-oriented functions):

```c
// --- Background-swap handshake timer ---
static uint8_t  seq_bgSwapActive = 0;
static uint8_t  seq_bgSwapFileType = 0;
static uint32_t seq_bgSwapTimeout = 0;
#define BG_SWAP_WAIT_MS 2000
```

New function to start the timer:

```c
/* Start a 2-second background-swap timer.  When the timer fires in
   seq_serviceBackgroundSwap(), a FRONT_SEQ_BACKGROUND_SWAP_DONE reply
   is sent back to the AVR so the file load can proceed. */
void seq_startBackgroundSwapTimer(uint8_t fileType)
{
   seq_bgSwapActive   = 1;
   seq_bgSwapFileType = fileType;
   seq_bgSwapTimeout  = HAL_GetTick() + BG_SWAP_WAIT_MS;
}
```

Service function called from the main loop:

```c
/* Check whether the background-swap timer has elapsed and send the
   completion reply.  Call once per main loop iteration. */
void seq_serviceBackgroundSwap(void)
{
   if(!seq_bgSwapActive)
      return;
   if(HAL_GetTick() >= seq_bgSwapTimeout)
   {
      seq_bgSwapActive = 0;
      // Send the "done" reply: SEQ_CC / SEQ_BACKGROUND_SWAP_DONE / fileType
      frontPanelSending_sendTriplet(FRONT_SEQ_CC,
                                    FRONT_SEQ_BACKGROUND_SWAP_DONE,
                                    seq_bgSwapFileType);
   }
}
```

**File**: `mainboard/LxrStm32/src/Sequencer/sequencer.h`

Declare the new public functions:

```c
/* Background-swap handshake — 2-second async timer before file load */
void seq_startBackgroundSwapTimer(uint8_t fileType);
void seq_serviceBackgroundSwap(void);
```

### 8. STM — Wire the service call into the main loop

**File**: `mainboard/LxrStm32/src/main.c`

In the main loop, after `seq_service()` or at an appropriate non-blocking tick point, add:

```c
seq_serviceBackgroundSwap();
```

Since the timer is non-blocking (just checks `HAL_GetTick()`), this must be called regularly — once per main loop iteration is sufficient. The 2-second wait is not precise; it is a guard to let the STM finish any in-flight parameter applications before the file data starts arriving.

### 9. AVR — Declare `preset_backgroundSwapDone` extern

**File**: `front/LxrAvr/Preset/PresetManager.h`

Add after the existing extern declarations:

```c
extern uint8_t preset_backgroundSwapDone;
```

So the receiving protocol parser in `avrCommsReceivingProtocol.c` can set it.

### 10. Files NOT changed

- `front/LxrAvr/IO/uart.c` / `uart.h` — no UART changes; the handshake uses the existing packet path
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c` — the `frontPanelSending_sendTriplet()` helper already handles `FRONT_SEQ_CC` triples
- `mainboard/LxrStm32/src/MIDI/` — no MIDI changes
- `mainboard/LxrStm32/src/Preset/` — no Preset module changes; the timer lives in Sequencer as a simple sleep gate
- AVR menu/lcd files — the "Bckgrnd Swap..." message is inline in `preset_performBackgroundSwapWait()`, no menu system changes

## Execution Flow Summary

### Path A: File type does NOT match background setting
```
User selects file → button handler calls preset_loadXxx()
  → preset_backgroundSwapNeeded() returns 0
  → proceeds directly to SEQ_FILE_BEGIN → kit load → pattern load → SEQ_FILE_DONE
  → (no LCD pause, no new handshake)
```

### Path B: File type matches background setting
```
User selects file → button handler calls preset_loadXxx()
  → preset_backgroundSwapNeeded() returns 1
  → preset_performBackgroundSwapWait(fileType):
      LCD shows "Bckgrnd Swap..."
      AVR sends SEQ_BACKGROUND_SWAP_BEGIN + fileType to STM
      AVR polls uart_checkAndParse() in a loop
  → STM receives SEQ_BACKGROUND_SWAP_BEGIN
      → seq_startBackgroundSwapTimer(fileType): sets 2s timer
  → STM main loop: seq_serviceBackgroundSwap() checks timer
      → when elapsed: sends SEQ_BACKGROUND_SWAP_DONE + fileType back
  → AVR uart parser receives SEQ_BACKGROUND_SWAP_DONE
      → sets preset_backgroundSwapDone = 1
  → AVR exits poll loop
  → proceeds with normal file load: SEQ_FILE_BEGIN → kit → pattern → SEQ_FILE_DONE
```

### Timeout path
```
If STM never replies within 5 seconds:
  → preset_performBackgroundSwapWait() returns 0
  → File load still proceeds (no lock-up)
```

## Files Changed Summary

| File | Change |
|------|--------|
| `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h` | 3 new opcode `#define`s (0x6d–0x6f) |
| `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h` | 3 new opcode `#define`s (`FRONT_SEQ_` prefix) |
| `front/LxrAvr/Preset/presetManager.c` | `BACKGROUND_*` defines + `preset_backgroundSwapDone` variable + `preset_backgroundSwapNeeded()` + `preset_performBackgroundSwapWait()` + handshake insertion in `preset_loadPerf()`, `preset_loadAll()`, `preset_loadPattern()` |
| `front/LxrAvr/Preset/PresetManager.h` | `extern uint8_t preset_backgroundSwapDone` |
| `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c` | `case SEQ_BACKGROUND_SWAP_DONE` handler |
| `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c` | `case FRONT_SEQ_BACKGROUND_SWAP_BEGIN` handler |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | `seq_bgSwap*` state + `seq_startBackgroundSwapTimer()` + `seq_serviceBackgroundSwap()` |
| `mainboard/LxrStm32/src/Sequencer/sequencer.h` | Declarations for the two new functions |
| `mainboard/LxrStm32/src/main.c` | `seq_serviceBackgroundSwap()` call in main loop |

## Verification

After implementation:

```bash
make -C front/LxrAvr clean && make -C front/LxrAvr avr -j4
make -C mainboard/LxrStm32 -j4 stm32
make firmware
```

Functional test:
1. Set `PAR_FILE_LOAD_BACKGROUND` to `off` (0) → load any `.ALL` → no pause, immediate load
2. Set to `all` (3) → load an `.ALL` → "Bckgrnd Swap..." shows for ~2s, then load proceeds
3. Set to `all` (3) → load a `.PRF` → no pause, immediate load
4. Set to `tot` (4) → load a `.ALL` → pause
5. Set to `tot` (4) → load a `.PRF` → pause
6. Set to `tot` (4) → load a `.PAT` → no pause

## Open Questions / Future Work

1. **What meaningful work should the STM do during the 2-second wait?** Currently the timer is a dummy pause. In a future pass, the STM could snapshot the playing kit into temp storage during this window so the background-loaded file does not interrupt playback. The 2-second guard ensures any in-flight parameter applies settle before file data arrives.

2. **Should `.PAT` loads get background treatment too?** Currently `BACKGROUND_PAT = 1` exists as a menu option but `.PAT` background behavior would need its own pattern-swap staging logic — out of scope for this pass.

3. **Should the 2-second timer be replaced with an "all clear" signal from the STM after it finishes any snapshot?** This would be more precise but requires more STM-side state tracking. The 2-second fixed delay is simple and safe for the first implementation.

4. **Timeout recovery**: The 5-second AVR timeout means the file load proceeds even if the STM is unresponsive. This prevents permanent lock-up but skips the intended guard delay. A future pass could add retry logic.