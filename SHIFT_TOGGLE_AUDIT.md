# Global Shift Toggle Audit

## Goal

When `PAR_BUT_SHIFT_MODE` is enabled, the front-panel SHIFT button should become a true latch. While the latch is active, every AVR UI path should behave exactly as if the physical SHIFT switch is currently held down. Only the shift-button edge handler should know whether the physical button is pressed or released.

## Finding

The current code has two competing definitions of "shift is active":

- `shiftState` is the latched/effective state. It is toggled by `buttonHandler_handleShift()` and drives the SHIFT LED and many shifted page-paint calls.
- `buttonHandler_getShift()` reads the raw DIN mirror for `BUT_SHIFT`, so it returns true only while the physical switch is currently down.

The problematic code is in [buttonHandler.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/buttonHandler.c:363):

```c
uint8_t buttonHandler_getShift() {
   const uint8_t arrayPos = BUT_SHIFT / 8;
   const uint8_t bitPos = BUT_SHIFT & 7;

   if (din_inputData[arrayPos] & (1 << bitPos)) {
      return 0;
   }
   return 1;
}
```

This defeats global shift-toggle almost everywhere, because most action handlers correctly ask the public getter whether SHIFT is active. Once toggle mode is on, pressing SHIFT lights the LED and calls the shifted page helpers, but releasing the physical button makes `buttonHandler_getShift()` return false again.

That means the UI can remain painted as shifted while button, encoder, mode, copy/clear, transport, and comms refresh code run their unshifted branches. This is a credible cause for the blank or incoherent screens: page-paint state and action-routing state diverge.

## High-Risk Call Sites

These paths already use `buttonHandler_getShift()` and therefore currently see raw physical state instead of the latched state:

- Mode selection, including SHIFT+PERF / Euclid entry and temp-playback guard: [buttonHandler.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/buttonHandler.c:380)
- Sequencer button press/release shifted behavior: [buttonHandler.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/buttonHandler.c:854) and [buttonHandler.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/buttonHandler.c:981)
- SHIFT+PLAY kit reload: [buttonHandler.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/buttonHandler.c:1397)
- SHIFT+REC recording menu: [buttonHandler.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/buttonHandler.c:1411)
- SHIFT+COPY clear / erase behavior: [buttonHandler.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/buttonHandler.c:1428)
- Copy button release cleanup: [buttonHandler.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/buttonHandler.c:1608)
- Encoder edit dispatch: [menu.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/Menu/menu.c:2742)
- Morph-value edit routing: [menu.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/Menu/menu.c:2849)
- Substep LED parsing from STM: [avrCommsReceivingProtocol.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/avrComms/avrCommsReceivingProtocol.c:918)

There are also direct `shiftState` readers outside the shift handler:

- Voice/step mode entry re-applies shifted page state: [menu.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/Menu/menu.c:799) and [menu.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/Menu/menu.c:841)
- Single-parameter edit display chooses normal vs morph value: [menu.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/Menu/menu.c:1536)
- Rotation and substep LED refresh gates: [avrCommsReceivingProtocol.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/avrComms/avrCommsReceivingProtocol.c:554), [avrCommsReceivingProtocol.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/avrComms/avrCommsReceivingProtocol.c:956), and [avrCommsReceivingProtocol.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/avrComms/avrCommsReceivingProtocol.c:969)

Those direct `shiftState` readers happen to be closer to the desired effective state, but they should still be converted to the public getter so the codebase has exactly one shift abstraction.

## Proposed Fix

Make `buttonHandler_getShift()` return the effective state, not the physical DIN state.

Recommended implementation:

1. Treat `shiftState` as the sole effective shift state.
2. Replace the body of `buttonHandler_getShift()` with:

```c
uint8_t buttonHandler_getShift()
{
   return shiftState;
}
```

3. Refactor `buttonHandler_handleShift()` into a small state-transition helper so momentary and toggle mode both call the same enter/leave code:

```c
static void buttonHandler_setShiftState(uint8_t nextState)
{
   nextState = nextState ? 1 : 0;
   if(nextState == shiftState)
      return;

   shiftState = nextState;

   if(shiftState)
   {
      /* existing shift-on body from buttonHandler_handleShift() */
   }
   else
   {
      /* existing shift-off body from buttonHandler_handleShift() */
   }
}

void buttonHandler_handleShift(uint8_t isDown)
{
   if(shiftMode)
   {
      if(isDown)
         buttonHandler_setShiftState((uint8_t)!shiftState);
      return;
   }

   buttonHandler_setShiftState(isDown);
}
```

This keeps the physical button edge as the only raw input. In toggle mode, the latch changes on the press edge and the release edge is ignored. In momentary mode, press sets effective shift on and release sets it off.

4. Replace remaining direct external reads of `shiftState` with `buttonHandler_getShift()`.
5. Remove `extern uint8_t shiftState;` from [buttonHandler.h](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/buttonHandler.h:19). If no external file needs it after step 4, make `shiftState` `static` in `buttonHandler.c`.
6. Keep `shiftMode` wired to the global setting for now, because [menu.c](/Users/bc/LXR01/LXR-current/LXR/front/LxrAvr/Menu/menu.c:3944) writes it when `PAR_BUT_SHIFT_MODE` changes. A later cleanup could hide that behind `buttonHandler_setShiftMode(value)`, but it is not necessary for the bug fix.

## Why This Is Safe

The DIN scanner should keep reporting press/release edges exactly as it does today. The only behavioral change is what non-shift code sees after a toggle-on press has been released.

Normal momentary SHIFT behavior should remain equivalent because `shiftState` is already set on physical press and cleared on physical release when `shiftMode == 0`.

The recent special cases should also line up with the requested semantics:

- `SHIFT+PERF` / Euclid page entry will work with either held SHIFT or latched SHIFT because mode-button routing asks `buttonHandler_getShift()`.
- `SHIFT+PLAY` / kit reload will work with latched SHIFT because transport routing asks `buttonHandler_getShift()`.
- Encoder morph editing will stop splitting display and edit destination because both repaint and encoder routing will use the same effective state.

## Verification Plan

Build:

```bash
make -C front/LxrAvr avr -j4
make firmware
```

Hardware smoke test:

1. Set global `stg` / `PAR_BUT_SHIFT_MODE` off. Confirm normal hold-to-shift behavior still works for SHIFT+PLAY, SHIFT+REC, SHIFT+COPY, SHIFT+PERF, step select/edit, and encoder morph edit.
2. Set global `stg` on. Press and release SHIFT once. Confirm the SHIFT LED stays lit.
3. With SHIFT latched on, test the same actions without holding the physical button: mode buttons, seq buttons, copy/clear, encoder morph edit, SHIFT+PLAY, SHIFT+REC, and SHIFT+PERF.
4. Press and release SHIFT again. Confirm the LED turns off and unshifted behavior returns.
5. Specifically test the blank-screen repro path. The display should no longer enter a shifted page while subsequent controls behave unshifted.

## Implementation Notes

Code changes made in this pass:

- `buttonHandler_getShift()` now returns the effective latched state instead of reading `din_inputData` for `BUT_SHIFT`.
- `shiftState` is private to `buttonHandler.c`; `buttonHandler.h` no longer exports it.
- `buttonHandler_handleShift()` now delegates to `buttonHandler_setShiftState()`, so momentary and toggle mode share one enter/leave implementation.
- In toggle mode, the effective state changes only on the physical press edge. The release edge is ignored, which makes the latch behave like "press once to hold SHIFT, press again to release SHIFT."
- Direct external `shiftState` readers in `menu.c` and `avrCommsReceivingProtocol.c` now call `buttonHandler_getShift()`, keeping page paint, encoder routing, and LED packet parsing on the same effective state.
- Added adjacent code comments at the new getter, shift-state transition helper, mode-entry paint gates, edit-display morph gate, and comms LED gates to document that global shift-toggle is intended to be a button-level override.

One deliberate behavior cleanup: when toggle mode is active and SHIFT is toggled off, the shifted UI is left immediately on the press edge. The old code left that cleanup until the release edge because the release handler contained the shift-off body. Immediate cleanup better matches a latch and removes a short state-disagreement window.

Build verification from this implementation pass:

- `make -C front/LxrAvr avr -j4` passes. The build still reports the existing fallthrough, array-bounds, conversion, and unused-function warnings.
- `make firmware` passes and rebuilds `firmware image/FIRMWARE.BIN` from the current AVR and STM binaries.
