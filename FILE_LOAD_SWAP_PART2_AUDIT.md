# FILE_LOAD_SWAP_PART2_AUDIT.md

## Status

Implemented in code; build verified.

Implementation notes:

- AVR `preset_backgroundSwapNeeded()` now returns false when
  `menu_sequencerRunning` is false, so stopped loads do not send
  `SEQ_BACKGROUND_SWAP_BEGIN` or wait for `SEQ_BACKGROUND_SWAP_DONE`.
- AVR `preset_backgroundSwapNeeded()` also returns false while
  `menu_playedPattern == SEQ_TMP_PATTERN`, following the review callout below
  so an active temp kit is not overwritten by a redundant normal-to-temp copy.
- AVR `.PAT`, `.ALL`, and `.PRF` loading-screen draws are now skipped while
  stopped. The file transfer itself still proceeds normally.
- STM `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` now calls
  `pat_copyToTmpPattern(seq_activePattern)` before recording
  `frontParser_backgroundSwapStartTick`, so the ACK timer starts after the
  existing normal-to-temp pattern/kit copy completes.

Verification run after implementation:

- `make -C front/LxrAvr avr -j4` passed.
- `make -C mainboard/LxrStm32 -j4 stm32` passed.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.

## Goal

This is the second background-load handshake step.

1. AVR: if the sequencer is not playing, do not send the background-swap opcodes and do not draw the file-load/loading screens, regardless of `PAR_FILE_LOAD_BACKGROUND`.
2. STM: when a background-swap begin opcode is received, copy the current normal preset/kit image and the currently playing pattern data into the existing temporary storage before starting the async delay that leads to `SEQ_BACKGROUND_SWAP_DONE`.

The STM copy must reuse the existing normal-to-temp copy behavior. It should not simulate button presses, change the copy/clear state machine, or introduce a second temp-copy implementation.

## Current Code Facts

### AVR Background Wait

The background-swap wait is owned by:

- `front/LxrAvr/Preset/presetManager.c:140-172`
  - `preset_backgroundSwapNeeded(uint8_t fileType)`
- `front/LxrAvr/Preset/presetManager.c:180-202`
  - `preset_performBackgroundSwapWait(uint8_t fileType)`

`preset_performBackgroundSwapWait()` currently always draws:

```c
Bckgrnd Swap...
```

before sending:

```c
avrComms_sendData(SEQ_CC, SEQ_BACKGROUND_SWAP_BEGIN, fileType);
```

The three supported file loaders call the wait before `SEQ_FILE_BEGIN`:

- `.PAT`: `front/LxrAvr/Preset/presetManager.c:2004-2005`
- `.ALL`: `front/LxrAvr/Preset/presetManager.c:2352-2353`
- `.PRF`: `front/LxrAvr/Preset/presetManager.c:2552-2553`

### AVR Loading Screens

There are still non-background loading screens that will draw even if the
background-swap wait is skipped:

- `.PAT`: `front/LxrAvr/Preset/presetManager.c:2007-2009`
  - `Loading Patrn`
- `.ALL`: `front/LxrAvr/Preset/presetManager.c:2388-2390`
  - `Loading All`
- `.PRF`: `front/LxrAvr/Preset/presetManager.c:204-209`
  - `preset_showLoadingPerf()`
  - called at `front/LxrAvr/Preset/presetManager.c:2555`
  - called again later during the same `.PRF` load

The AVR already has the front-panel play-state mirror:

- `front/LxrAvr/Menu/menu.c:599`
  - `uint8_t menu_sequencerRunning = 0;`
- `front/LxrAvr/Menu/menu.h:37`
  - `extern uint8_t menu_sequencerRunning;`
- `front/LxrAvr/buttonHandler.c:1584-1589`
  - `buttonHandler_setRunStopState()` keeps `menu_sequencerRunning` synced.

`presetManager.c` already includes `PresetManager.h`, and `PresetManager.h`
includes `../Menu/menu.h`, so `menu_sequencerRunning` is already visible.

### STM Background Ack

The current dummy STM implementation is:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:1213-1225`
  - `frontParser_serviceBackgroundSwapAck()`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:2614-2618`
  - `FRONT_SEQ_BACKGROUND_SWAP_BEGIN`

The receive case currently only records the request and starts the delay:

```c
case FRONT_SEQ_BACKGROUND_SWAP_BEGIN:
   frontParser_backgroundSwapPending = 1;
   frontParser_backgroundSwapFileType = frontParser_command.data2;
   frontParser_backgroundSwapStartTick = systick_ticks;
   break;
```

### Existing Copy-To-Temp Behavior

The STM pattern copy path already does the desired manual-copy behavior:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:2349-2358`
  - `FRONT_SEQ_COPY_PATTERN`
  - if destination is `SEQ_TMP_PATTERN`, it calls `pat_copyToTmpPattern(src)`.
- `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.c:443-460`
  - `pat_copyToTmpPattern(uint8_t srcPattern)`

When the sequencer is stopped, `pat_copyToTmpPattern(srcPattern)` copies the
selected normal source pattern into `SEQ_TMP_PATTERN`.

When the sequencer is running, `pat_copyToTmpPattern(srcPattern)` ignores the
single source pattern for track payload and copies each track from:

```c
seq_perTrackActivePattern[track]
```

into `SEQ_TMP_PATTERN`. This is exactly the "currently used individual track
patterns" behavior requested.

The same helper also captures the current normal preset/kit image through:

- `mainboard/LxrStm32/src/Preset/KitState.c:106-132`
  - `preset_captureTmpKitState()`

That copies normal kit endpoints, morph endpoints, interpolated params,
automation targets, global morph, and individual voice morph state into
`preset_tmpKitState` and marks the temp image valid.

## Proposed Code Changes

### 1. AVR: Gate Background Swap On Playback

File:

- `front/LxrAvr/Preset/presetManager.c`

Change `preset_backgroundSwapNeeded(uint8_t fileType)` so stopped transport is
an immediate no:

```c
static uint8_t preset_backgroundSwapNeeded(uint8_t fileType)
{
   if(!menu_sequencerRunning)
      return 0;

   ...
}
```

This ensures stopped `.PAT`, `.ALL`, and `.PRF` loads never send:

```c
SEQ_BACKGROUND_SWAP_BEGIN
```

and therefore never wait for:

```c
SEQ_BACKGROUND_SWAP_DONE
```

regardless of the global background-load mode.

### 2. AVR: Gate Loading Screens On Playback

File:

- `front/LxrAvr/Preset/presetManager.c`

Make the visible file-load messages conditional on `menu_sequencerRunning`.

For `.PAT`, wrap:

```c
lcd_clear();
lcd_home();
lcd_string_F(PSTR("Loading Patrn"));
```

at `front/LxrAvr/Preset/presetManager.c:2007-2009`.

For `.ALL`, wrap:

```c
lcd_clear();
lcd_home();
lcd_string_F(PSTR("Loading All"));
```

at `front/LxrAvr/Preset/presetManager.c:2388-2390`.

For `.PRF`, make `preset_showLoadingPerf()` return without drawing when the
sequencer is stopped:

```c
static void preset_showLoadingPerf()
{
   if(!menu_sequencerRunning)
      return;

   lcd_clear();
   lcd_home();
   lcd_string_F(PSTR("Loading Perf"));
}
```

This covers both current `.PRF` call sites without duplicating the condition.

### 3. STM: Replace Dummy Begin Body With Real Temp Snapshot Prep

File:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

In the `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` case, call the existing temp-copy
helper before setting `frontParser_backgroundSwapStartTick`:

```c
case FRONT_SEQ_BACKGROUND_SWAP_BEGIN:
   pat_copyToTmpPattern(seq_activePattern);
   frontParser_backgroundSwapPending = 1;
   frontParser_backgroundSwapFileType = frontParser_command.data2;
   frontParser_backgroundSwapStartTick = systick_ticks;
   break;
```

Why `seq_activePattern`:

- It is the STM-side currently playing pattern selector.
- When running, `pat_copyToTmpPattern()` already copies each track from
  `seq_perTrackActivePattern[track]`, so the argument is only the stopped-mode
  fallback.
- The AVR should not send this opcode when stopped after change 1, but this
  argument still gives sensible behavior if a stale/manual opcode arrives.

The current include set already has what this needs:

- `frontPanelReceivingProtocol.c` includes `sequencer.h`, which exposes
  `seq_activePattern` and `seq_perTrackActivePattern`.
- It includes `PatternData.h`, which declares `pat_copyToTmpPattern()`.

No new STM opcode is needed.

### 4. Leave The Ack Service Shape Intact

File:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Leave `frontParser_serviceBackgroundSwapAck()` structurally unchanged:

```c
if(!frontParser_backgroundSwapPending)
   return;

if((uint32_t)(systick_ticks - frontParser_backgroundSwapStartTick) < FRONT_BACKGROUND_SWAP_DELAY_TICKS)
   return;

frontParser_backgroundSwapPending = 0;
frontPanelSending_sendPriorityTriplet(... FRONT_SEQ_BACKGROUND_SWAP_DONE ...);
```

The only behavior change is that the timer starts after the synchronous copy
has completed.

## Review Callout: Already Playing Temp

One edge case deserves review before implementation.

If playback is already using `SEQ_TMP_PATTERN`, normal storage is already
isolated from audible playback, so a background-swap copy may not be needed.
Calling `pat_copyToTmpPattern()` in that state reuses the manual copy behavior,
which includes `preset_captureTmpKitState()`. That copies normal kit state into
the temp kit image and could overwrite the currently audible temp kit image.

Recommended guard:

```c
if(menu_playedPattern == SEQ_TMP_PATTERN)
   return 0;
```

near the top of AVR `preset_backgroundSwapNeeded()`, after the
`menu_sequencerRunning` check.

This would mean:

- stopped loads: no background-swap opcode, no loading screen;
- already-temp playback: no background-swap opcode because playback is already protected from normal-storage file writes;
- normal playback with a matching background-load mode: send the opcode and let STM seed temp storage before the file bytes arrive.

This guard is not strictly part of the user's two requested changes, but it
prevents the new STM copy from overwriting an active temp kit in the one case
where the copy is unnecessary.

## Guardrails

- Do not change `pat_copyToTmpPattern()`.
- Do not change `preset_captureTmpKitState()`.
- Do not synthesize or parse a fake `FRONT_SEQ_COPY_PATTERN` command.
- Do not touch `copyClear_Mode` or any AVR button-handler copy state.
- Do not route `.PAT` parameter ingress to temp storage.
- Do not reintroduce any cache/session object beside the existing normal/temp
  Preset and Pattern storage model.
- Keep `FRONT_SEQ_BACKGROUND_SWAP_DONE` sent through
  `frontPanelSending_sendPriorityTriplet()` so `.ALL/.PRF` quiet UI does not
  suppress the ACK.

## Verification Plan

Build checks after implementation:

```bash
make -C front/LxrAvr avr -j4
make -C mainboard/LxrStm32 -j4 stm32
make firmware
```

Manual retest checklist:

1. Sequencer stopped, background mode `tot`: load `.PAT`, `.PRF`, and `.ALL`.
   Expect no `Bckgrnd Swap...`, no `Loading ...` screen, and no background-swap
   wait.
2. Sequencer running, background mode matching file type: load `.PAT`, `.PRF`,
   or `.ALL`. Expect STM to copy current normal kit/pattern into temp, delay,
   send done, then AVR proceeds with normal file load.
3. Sequencer running with per-track patterns active: background `.PAT`/`.ALL`
   should snapshot the per-track active pattern sources because
   `pat_copyToTmpPattern()` uses `seq_perTrackActivePattern[track]`.
4. Sequencer running with background mode `off`: no background-swap opcode or
   wait.
5. If the recommended temp-active guard is accepted: while already playing
   `SEQ_TMP_PATTERN`, background-enabled file loads should skip the swap
   opcode because playback is already insulated by temp storage.
