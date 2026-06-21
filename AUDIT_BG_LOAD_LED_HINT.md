# Background Load Temp Playback LED Hint Audit

## Goal

Now that the AVR tracks whether background loading has moved playback to
temporary data, use that state to make the front-panel LEDs indicate that temp
playback is active.

When temp playback is active:

- if the current mode is anything other than `SELECT_MODE_PERF`, the PERF mode
  LED (`LED_MODE2`) should flash in addition to the current mode LED;
- `SHIFT+PERF` should be disabled;
- if the current mode is `SELECT_MODE_PERF`, all eight SELECT LEDs
  (`LED_PART_SELECT1` through `LED_PART_SELECT8`) should flash.

No STM change should be required for this. The AVR already has the authoritative
temp-playback flag added for the previous tweak.

## Current Relevant Code

### Temp Playback State

File:

- `front/LxrAvr/Preset/presetManager.c`

Current flag:

```c
static uint8_t preset_backgroundTempPlaybackActive = 0;
```

It is set in:

```c
void preset_backgroundSwapDoneFromStm(uint8_t fileType)
```

and cleared in:

```c
void preset_notePlayedPatternChanged(uint8_t playedPattern)
```

when `playedPattern != SEQ_TMP_PATTERN`.

There is currently no public getter, so UI code cannot query the state without
reaching into `presetManager.c`.

### Mode Handling

File:

- `front/LxrAvr/buttonHandler.c`

`buttonHandler_handleModeButtons(uint8_t mode)` computes the new mode:

```c
if (buttonHandler_getShift())
   buttonHandler_stateMemory.selectButtonMode = (mode + 4) & 0x07;
else
   buttonHandler_stateMemory.selectButtonMode = mode & 0x07;
```

That means pressing PERF while shift is held maps:

```c
SELECT_MODE_PERF -> SELECT_MODE_PAT_GEN
```

The same function clears blink state and mode LEDs:

```c
led_clearAllBlinkLeds();
led_clearModeLeds();
```

and then enters the requested mode.

Because mode changes clear blink state, the temp-playback hint must be applied
after this mode setup, not before it.

### Performance LED Setup

File:

- `front/LxrAvr/ledHandler.c`

Current `led_initPerformanceLeds()` lights the played pattern and blinks the
viewed pattern when viewed and played differ:

```c
led_setValue(1, playedPatternLed);
if(menu_playedPattern != menu_getViewedPattern())
   led_setBlinkLed(viewedPatternLed, 1);
```

When temp playback is active and the user is in PERF mode, this ordinary
played/viewed-pattern indication should be replaced by the requested “all
SELECT LEDs flash” hint.

### Pattern-Ack LED Refresh

File:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

`SEQ_CHANGE_PAT` currently updates:

```c
menu_playedPattern = patMsg;
preset_notePlayedPatternChanged(patMsg);
```

and later, when in PERF or PAT_GEN mode, clears and reinitializes performance
LEDs:

```c
led_clearSelectLeds();
led_clearAllBlinkLeds();
led_initPerformanceLeds();
```

Any temp-playback LED hint applied before that block would be cleared, so the
refresh hook must run after this mode-specific LED reset.

### Blink Slot Limit

File:

- `front/LxrAvr/ledHandler.c`

Current limit:

```c
#define NUM_OF_BLINKABLE_LEDS 6
```

Flashing all eight SELECT LEDs at once requires at least eight blink slots.

## Proposed Code Changes

### 1. Add A Public Temp Playback Getter

Files:

- `front/LxrAvr/Preset/presetManager.c`
- `front/LxrAvr/Preset/PresetManager.h`

Add:

```c
uint8_t preset_isBackgroundTempPlaybackActive(void)
{
   return preset_backgroundTempPlaybackActive;
}
```

and expose the prototype in `PresetManager.h`.

Purpose:

- keep `preset_backgroundTempPlaybackActive` private to `presetManager.c`;
- let button/LED code ask the model state directly;
- avoid duplicating temp-playback state in the UI layer.

### 2. Add A Button Handler LED-Hint Refresh Function

Files:

- `front/LxrAvr/buttonHandler.c`
- `front/LxrAvr/buttonHandler.h`

Add a public function:

```c
void buttonHandler_refreshTempPlaybackLedHint(void);
```

Implementation shape:

```c
static void buttonHandler_setAllSelectBlink(uint8_t onOff)
{
   uint8_t i;
   for(i = 0; i < 8; i++)
      led_setBlinkLed((uint8_t)(LED_PART_SELECT1 + i), onOff);
}

void buttonHandler_refreshTempPlaybackLedHint(void)
{
   uint8_t tempActive = preset_isBackgroundTempPlaybackActive();

   led_setBlinkLed(LED_MODE2, 0);

   if(buttonHandler_stateMemory.selectButtonMode == SELECT_MODE_PERF)
   {
      led_clearSelectBlinkLeds();

      if(tempActive)
         buttonHandler_setAllSelectBlink(1);
      else
         led_initPerformanceLeds();

      return;
   }

   if(tempActive)
      led_setBlinkLed(LED_MODE2, 1);
}
```

Expected behavior:

- non-PERF mode while temp playback is active: current mode LED remains set by
  normal mode handling, and `LED_MODE2` blinks as the temp-playback hint;
- PERF mode while temp playback is active: all eight SELECT LEDs blink;
- when temp playback clears while in PERF mode, ordinary
  `led_initPerformanceLeds()` restores the normal played/viewed pattern display;
- when temp playback clears outside PERF mode, the PERF LED blink is removed.

This helper should not call `led_clearModeLeds()` or `led_clearAllBlinkLeds()`.
Those broader clears belong to mode transitions and would erase unrelated LED
state if run from a communication ack.

### 3. Reapply The Hint After Mode Changes

File:

- `front/LxrAvr/buttonHandler.c`

At the end of `buttonHandler_handleModeButtons(uint8_t mode)`, after the mode
switch has entered the new mode and set the ordinary mode LEDs, call:

```c
buttonHandler_refreshTempPlaybackLedHint();
```

Rationale:

- `buttonHandler_handleModeButtons()` clears all blink LEDs during mode entry;
- the hint must be applied after the normal LED state is rebuilt.

### 4. Disable `SHIFT+PERF` While Temp Playback Is Active

File:

- `front/LxrAvr/buttonHandler.c`

At the start of `buttonHandler_handleModeButtons(uint8_t mode)`, after clearing
`buttonHandler_heldVoiceButtons` and `buttonHandler_muteTag`, add an early guard:

```c
if(preset_isBackgroundTempPlaybackActive()
   && buttonHandler_getShift()
   && ((mode & 0x07) == SELECT_MODE_PERF))
{
   buttonHandler_refreshTempPlaybackLedHint();
   return;
}
```

This blocks only the shifted PERF function. Unshifted PERF still enters
`SELECT_MODE_PERF`, where all SELECT LEDs will flash while temp playback is
active.

The current shifted PERF target is `SELECT_MODE_PAT_GEN`, reached by the
`(mode + 4) & 0x07` mapping. This guard prevents that mode change from
happening while temp playback is active.

### 5. Refresh The Hint When STM Changes Temp Playback State

File:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

After:

```c
preset_backgroundSwapDoneFromStm(avrCommsParser_command.data2);
```

in the `SEQ_BACKGROUND_SWAP_DONE` case, call:

```c
buttonHandler_refreshTempPlaybackLedHint();
```

Purpose:

- temp playback becomes active as soon as the background swap is acknowledged;
- the LED hint should appear without requiring the user to press another mode
  button.

In the `SEQ_CHANGE_PAT` case, call the same helper after the existing PERF /
PAT_GEN LED refresh block:

```c
if( (buttonHandler_getMode() == SELECT_MODE_PERF) || (buttonHandler_getMode() == SELECT_MODE_PAT_GEN) )
{
   led_clearSelectLeds();
   led_clearAllBlinkLeds();
   led_initPerformanceLeds();
}

buttonHandler_refreshTempPlaybackLedHint();
```

Purpose:

- `preset_notePlayedPatternChanged(patMsg)` may clear the temp-playback flag;
- the existing PERF/PAT_GEN block clears blink LEDs, so the temp hint must be
  restored or removed after that block.

### 6. Increase Blink Capacity For Eight SELECT LEDs

File:

- `front/LxrAvr/ledHandler.c`

Change:

```c
#define NUM_OF_BLINKABLE_LEDS 6
```

to:

```c
#define NUM_OF_BLINKABLE_LEDS 8
```

Rationale:

- the requested PERF-mode hint needs all eight SELECT LEDs blinking at once;
- the existing six-slot limit cannot represent that state.

## Expected User-Visible Behavior

Temp playback active, user is in VOICE / STEP / LOAD / MENU / PAT_GEN:

- current mode LED remains active according to existing mode logic;
- PERF LED flashes as the temp-playback reminder.

Temp playback active, user is in PERF:

- PERF mode LED remains lit;
- all eight SELECT LEDs flash.

Temp playback active, user presses `SHIFT+PERF`:

- no mode change to PAT_GEN;
- the temp-playback LED hint remains active.

Temp playback clears by switching to a normal played pattern:

- PERF LED stops flashing outside PERF mode;
- PERF mode SELECT LEDs return to the normal played/viewed pattern display.

## Verification Plan

Build checks:

```sh
make -C front/LxrAvr -j4 avr
make firmware
```

Hardware retest:

1. Start sequencer and perform a background `.all` or `.prf` load so playback
   switches to temporary data.
2. Enter VOICE, STEP, LOAD, and MENU modes. Confirm the current mode LED remains
   active and PERF flashes.
3. Enter PERF mode. Confirm all eight SELECT LEDs flash.
4. While temp playback is active, press `SHIFT+PERF`. Confirm PAT_GEN does not
   activate.
5. Switch playback back to a normal pattern. Confirm the temp-playback hint
   stops and normal PERF pattern LEDs return.

