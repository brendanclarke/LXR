# Background Load Tweaks 2 Audit

## Goal

If playback is already running from temporary data and the user
starts another `.pat`, `.prf`, or `.all` load before switching playback back to a normal
pattern-set pattern, do not perform another background swap/copy-to-temp cycle.

Instead, the second `.pat`, `.prf`, or `.all` load should proceed like background loading is
off:

- no `SEQ_BACKGROUND_SWAP_BEGIN` should be sent for that second file
  load;
- STM should not copy normal parameters/pattern into temp again;
- STM should not instant-switch temp playback again;
- AVR should continue with the ordinary file-load flow so the newly loaded file lands in normal storage while temp playback remains audible.

## Current Relevant Behavior

### AVR Background-Swap Gate

`front/LxrAvr/Preset/presetManager.c`

Current `preset_backgroundSwapNeeded()`:

```c
static uint8_t preset_backgroundSwapNeeded(uint8_t fileType)
{
   uint8_t bgMode = parameter_values[PAR_FILE_LOAD_BACKGROUND];

   if(!menu_sequencerRunning)
      return 0;

   if(menu_playedPattern == SEQ_TMP_PATTERN)
      return 0;

   ...
}
```

There is already a broad guard at lines 161-162 that disables background swap
when AVR believes the played pattern is `SEQ_TMP_PATTERN`.

That helps, but it is pattern-state based. The requested rule is
temp-playback-state based: if any background load has already moved playback to temp,
do not copy/switch temp again until playback returns to a normal pattern.

### AVR Load Sites

`.pat`:

```c
preset_workingType=WTYPE_PATTERN;
...
if(preset_backgroundSwapNeeded(preset_workingType))
   preset_performBackgroundSwapWait(preset_workingType);
```

`.all`:

```c
preset_workingType=WTYPE_ALL;
...
if(preset_backgroundSwapNeeded(preset_workingType))
   preset_performBackgroundSwapWait(preset_workingType);
```

`.prf`:

```c
preset_workingType=WTYPE_PERFORMANCE;
...
if(preset_backgroundSwapNeeded(preset_workingType))
   preset_performBackgroundSwapWait(preset_workingType);
```

These are the only AVR decision points that should decide whether to send
`SEQ_BACKGROUND_SWAP_BEGIN`.

### STM Background-Swap Entry

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Current `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` handler:

```c
case FRONT_SEQ_BACKGROUND_SWAP_BEGIN:
   pat_copyToTmpPattern(seq_activePattern);
   seq_setNextPattern(SEQ_TMP_PATTERN, 0x0f);
   preset_tempPlaybackSwitchState.forceInstantSwitch = 1;
   preset_tempPlaybackSwitchState.patternOnlyTempPlayback =
      (frontParser_command.data2 == FRONT_FILE_DONE_TYPE_PATTERN);
   frontParser_backgroundSwapPending = 1;
   frontParser_backgroundSwapFileType = frontParser_command.data2;
   frontParser_backgroundSwapAckDelayActive = 0;
   break;
```

If AVR does not send `SEQ_BACKGROUND_SWAP_BEGIN`, this STM copy/switch path does
not run. Therefore the requested behavior can be implemented on the AVR side by
making `preset_backgroundSwapNeeded()` return false for the second file load while temp playback is already active.

## Proposed State

Add one AVR-side file-load state flag in
`front/LxrAvr/Preset/presetManager.c`:

```c
static uint8_t preset_backgroundTempPlaybackActive = 0;
```

Meaning:

- `0`: AVR should allow background swap according to the global setting and file
  type.
- `1`: playback has already been moved to temp by a background load, so another
  `.pat`, `.prf`, or `.all` load must skip the background swap phase and proceed as a normal
  load into normal storage.

This flag is deliberately broader than parameter ownership. A `.pat` background
load may keep parameters normal, but it still uses the temporary pattern slot;
while that temp playback is active, another file load must not copy normal data
into temp again.

## Proposed Code Changes

### 1. Add Helper To Recognize Background-Capable File Types

File:

- `front/LxrAvr/Preset/presetManager.c`

Add near `preset_backgroundSwapNeeded()`:

```c
static uint8_t preset_fileTypeCanBackgroundSwap(uint8_t fileType)
{
   return (fileType == WTYPE_PATTERN)
       || (fileType == WTYPE_PERFORMANCE)
       || (fileType == WTYPE_ALL);
}
```

Purpose:

- keep the new branch readable;
- make it explicit that `.pat`, `.prf`, and `.all` are all covered by this tweak.

### 2. Track Successful Background Swap

File:

- `front/LxrAvr/Preset/presetManager.c`

Current callback:

```c
void preset_backgroundSwapDoneFromStm(uint8_t fileType)
{
   if(fileType == preset_backgroundSwapExpectedType)
      preset_backgroundSwapDone = 1;
}
```

Change to:

```c
void preset_backgroundSwapDoneFromStm(uint8_t fileType)
{
   if(fileType == preset_backgroundSwapExpectedType)
   {
      preset_backgroundSwapDone = 1;
      if(preset_fileTypeCanBackgroundSwap(fileType))
         preset_backgroundTempPlaybackActive = 1;
   }
}
```

Rationale:

- the flag is set only after STM acknowledges the background swap;
- `.pat` background swaps also mark temp playback active because they use the temp
  pattern slot, even though parameter ownership remains normal.

### 3. Clear Flag When Playback Leaves Temp Pattern

File:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

Current `SEQ_CHANGE_PAT` handler already receives the active played pattern and
sets:

```c
menu_playedPattern = patMsg;
```

Add a small `PresetManager` API:

In `front/LxrAvr/Preset/PresetManager.h`:

```c
void preset_notePlayedPatternChanged(uint8_t playedPattern);
```

In `front/LxrAvr/Preset/presetManager.c`:

```c
void preset_notePlayedPatternChanged(uint8_t playedPattern)
{
   if(playedPattern != SEQ_TMP_PATTERN)
      preset_backgroundTempPlaybackActive = 0;
}
```

Then call it in `avrCommsReceivingProtocol.c` immediately after
`menu_playedPattern = patMsg;`:

```c
menu_playedPattern = patMsg;
preset_notePlayedPatternChanged(patMsg);
```

Rationale:

- the flag should remain true while the user continues playing from temp;
- it should clear when STM confirms playback has returned to a normal
  pattern-set pattern;
- do not clear it when the file load finishes, because the whole point is that
  the user may start another `.pat`, `.prf`, or `.all` load before leaving temp playback.

### 4. Gate `.pat` / `.prf` / `.all` Background Swap On The New Flag

File:

- `front/LxrAvr/Preset/presetManager.c`

Modify `preset_backgroundSwapNeeded()` after the sequencer-running check and
before checking the global background-load mode:

```c
if(preset_backgroundTempPlaybackActive
   && preset_fileTypeCanBackgroundSwap(fileType))
   return 0;
```

The existing `menu_playedPattern == SEQ_TMP_PATTERN` guard can remain:

```c
if(menu_playedPattern == SEQ_TMP_PATTERN)
   return 0;
```

Rationale:

- the new guard is the temp-playback rule requested here;
- the old pattern guard is still harmless and protects the broad “already on
  temp pattern” condition;
- `.pat` is now explicitly covered so it cannot trigger another copy-to-temp while temp playback is already active.

## Retest Result And Closure

Retest confirmed this behavior is working:

- while temp playback is already active, a subsequent `.pat`, `.prf`, or `.all`
  load skips the background swap/copy-to-temp phase;
- the ordinary file load still proceeds;
- returning playback to a normal pattern clears the temp-playback state so a
  later background load can use the swap path again.

Audit closed.

## Expected Flow After Change

### First `.pat`, `.prf`, or `.all` Load While Playing Normal

1. `preset_backgroundSwapNeeded(WTYPE_PATTERN/WTYPE_PERFORMANCE/WTYPE_ALL)` evaluates the
   global mode and returns true if configured.
2. AVR sends `SEQ_BACKGROUND_SWAP_BEGIN`.
3. STM copies current normal data to temp and switches playback to temp.
4. STM sends `SEQ_BACKGROUND_SWAP_DONE`.
5. AVR sets `preset_backgroundTempPlaybackActive = 1`.
6. AVR performs the ordinary file load into normal storage.

### Second `.pat`, `.prf`, or `.all` Load Before Leaving Temp Playback

1. `preset_backgroundSwapNeeded(WTYPE_PATTERN/WTYPE_PERFORMANCE/WTYPE_ALL)` sees
   `preset_backgroundTempPlaybackActive == 1`.
2. AVR does not send `SEQ_BACKGROUND_SWAP_BEGIN`.
3. STM does not copy normal to temp again.
4. AVR continues the ordinary file load into normal storage.
5. Temp playback continues uninterrupted from the already-existing temp data.

### After Switching Back To A Normal Pattern-Set Pattern

1. STM reports `SEQ_CHANGE_PAT` with a non-`SEQ_TMP_PATTERN` pattern.
2. AVR sets `menu_playedPattern` to that normal pattern.
3. AVR clears `preset_backgroundTempPlaybackActive`.
4. A later `.pat`, `.prf`, or `.all` load can use background swap again, depending on the
   global setting.

## Files Expected To Change

- `front/LxrAvr/Preset/presetManager.c`
- `front/LxrAvr/Preset/PresetManager.h`
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

No STM code should be required for this tweak because the STM background
copy/switch path is only entered when AVR sends `SEQ_BACKGROUND_SWAP_BEGIN`.

## Verification Plan

1. Build AVR and full firmware:

   ```sh
   make -C front/LxrAvr -j4 avr
   make firmware
   ```

2. Runtime test:

   - enable background load for `.pat`, `.prf`, or `.all`;
   - start sequencer on a normal pattern;
   - load a `.pat`, `.prf`, or `.all`;
   - verify the background swap occurs and playback moves to temp;
   - without switching back to a normal pattern-set pattern, load another
     `.pat`, `.prf`, or `.all`;
   - verify there is no background-swap copy/switch phase for the second load;
   - verify the second load proceeds into normal storage;
   - specifically repeat with `.pat` as both the first and second load to verify
     pattern-only background loading also skips the second copy-to-temp;
   - switch back to a normal pattern-set pattern;
   - load another `.pat`, `.prf`, or `.all` and verify background swap can occur again.

## Implementation Notes

Implemented the AVR-side temp-playback guard.

Files changed:

- `front/LxrAvr/Preset/presetManager.c`
- `front/LxrAvr/Preset/PresetManager.h`
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

Changes:

- Added `preset_backgroundTempPlaybackActive` in `presetManager.c`.
- Added `preset_fileTypeCanBackgroundSwap()` covering `.pat`, `.prf`, and
  `.all` file types.
- `preset_backgroundSwapNeeded()` now returns false when
  `preset_backgroundTempPlaybackActive` is true for those file types, before
  checking the global background-load mode.
- `preset_backgroundSwapDoneFromStm()` now sets
  `preset_backgroundTempPlaybackActive` after STM acknowledges a successful
  `.pat`/`.prf`/`.all` background swap.
- Added `preset_notePlayedPatternChanged()` and exported it in
  `PresetManager.h`.
- `SEQ_CHANGE_PAT` handling now calls `preset_notePlayedPatternChanged(patMsg)`
  immediately after updating `menu_playedPattern`, clearing the guard once STM
  reports playback has returned to a normal pattern.

No STM code was changed for this tweak. The STM copy/switch path is avoided by
not sending `SEQ_BACKGROUND_SWAP_BEGIN` for the second load while temp playback
is already active.

## Build Verification

- `make -C front/LxrAvr -j4 avr` passed and rebuilt `front/LxrAvr/LxrAvr.bin`.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.

Warnings were the existing AVR warning set, including LED switch fallthrough,
port/register array-bounds noise, and the existing unused `value` warning in
`preset_readDrumsetMeta()` from the commented legacy macro block.
