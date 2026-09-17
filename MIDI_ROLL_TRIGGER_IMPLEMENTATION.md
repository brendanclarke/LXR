# MIDI Roll Trigger Implementation Schedule

## Scope

This schedule implements `MIDI_ROLL_TRIGGER.md` without changing existing Global CC assignments, without adding a new AVR menu datatype, and without treating note-on velocity `0` as note-off. It adds a saved global roll-note offset, routes that offset to STM, adds Global NRPN 93 for roll rate, and makes MIDI roll notes a last-consumer note path that sustains until literal note-off.

Offset encoding for implementation:

- Raw `0`: roll note trigger disabled and displayed as `off`.
- Raw `1..127`: active positive semitone offset above the assigned/normal trigger note. There is no negative offset and no centered conversion.
- If the shifted roll note does not exist in MIDI range, matching does nothing: `baseNote + offset > 127` is not a valid roll trigger, and `incomingNote - offset < 0` has no valid standard trigger note behind it.

## Session Progress (2026-09-17)

- [x] Appended the saved AVR global offset parameter without shifting existing
  global file bytes; added the MIDI-menu label, `off` display, default, and
  global-settings page slot.
- [x] Added matching AVR/STM front-panel opcode `0x6f` and STM-side release
  handling for offset, MIDI-channel, and note-override remaps.
- [x] Added STM parser-owned positive-offset matching, literal NOTE_ON/OFF
  handling, per-voice overlapping hold counts, and deduped voice masks.
- [x] Added separate manual/MIDI roll ownership in Sequencer so either source
  can sustain a roll independently.
- [x] Added Global NRPN 93 for the existing roll-rate control and updated the
  durable MIDI table.
- [x] `make -C mainboard/LxrStm32 -j4 stm32` and
  `make -C front/LxrAvr avr -j4` pass. The checked-in x86_64
  `tools/bin/FirmwareImageBuilder` cannot run on this arm64 host, so an
  arm64-native temporary build of that tool successfully regenerated
  `firmware image/FIRMWARE.BIN`; hardware verification remains to be run.

Implementation note: current chromatic normal MIDI routes accept every note on
their assigned channel. The parser therefore preserves the requested
last-consumer rule and only allows shifted roll fallback when normal routing
does not accept the note (for example, a shifted note-override route). This is
why `normalConsumed` is tracked from the actual override decision rather than
merely from whether `ChannelMidiParser` was called.

## Implementation Phases

1. Add AVR menu/config metadata for the appended global roll-offset parameter.
2. Add AVR-to-STM opcode for roll offset.
3. Add STM roll-offset storage, decode helpers, MIDI hold accounting, and last-consumer note matching.
4. Split manual and MIDI roll ownership in the sequencer.
5. Add Global NRPN 93 for roll rate.
6. Update durable MIDI documentation.
7. Build and verify both firmware sides.

## AVR Global Setting And Menu

### `front/LxrAvr/Parameters.h`

Modify around lines 428-441.

Change type: add.

Add `PAR_ROLL_NOTE_OFFSET` after `PAR_MIDI_NOTE7` and before `NUM_PARAMS`.

Why this must exist:

The roll offset is a saved global setting, so it must live in `parameter_values[]` and be included in global save/load. Appending after the existing MIDI note globals avoids shifting existing global bytes in `glo.cfg` and `.ALL` files.

Inputs:

- AVR menu edits store a raw `uint8_t` value.
- Old config/all files may not contain this byte.

Outputs/accessors:

- `parameter_values[PAR_ROLL_NOTE_OFFSET]`
- Included automatically by save/load loops that run from `PAR_BEGINNING_OF_GLOBALS` to `NUM_PARAMS`.

Adjacent comment-block text:

```c
/* Saved global MIDI roll-note offset.
   Raw 0 disables MIDI roll notes and displays as "off"; raw 1..127 is a
   positive semitone offset above the normal trigger note. This enum is
   appended to the global block so older glo.cfg/.ALL files keep their existing
   byte layout and simply default the missing value to 0/off. */
```

### `front/LxrAvr/Menu/menu.h`

Modify `NamesEnum` around lines 234-242.

Change type: add.

Add `TEXT_ROLL_NOTE_OFFSET` after `TEXT_ROLL_MODE` and before `TEXT_TRANSPOSE`.

Modify long-name enum around lines 497-504.

Change type: add.

Add `LONG_ROLL_NOTE_OFFSET` after `LONG_ROLL_MODE` and before `LONG_TRANSPOSE`.

No short-name enum change is required. Reuse existing `SHORT_ROLL` at line 296 so the display short text is exactly `rol`.

No datatype enum change is allowed or needed. Prefer an existing `0=off, 1..127=numeric` dtype if one is available; otherwise use existing `DTYPE_0B127` at line 590 plus a tiny `PAR_ROLL_NOTE_OFFSET` display exception for `0 -> off`. The datatype enum is capped at 16 entries at lines 606-607.

Why this must exist:

The new setting needs a name tuple in the menu text system. Reusing `SHORT_ROLL` avoids another short string slot and keeps the requested short text.

Inputs:

- `TEXT_ROLL_NOTE_OFFSET` is referenced by `menuPages.h`.
- `LONG_ROLL_NOTE_OFFSET` is referenced by `menu.c` `valueNames[]`.

Outputs/accessors:

- Menu display resolves `TEXT_ROLL_NOTE_OFFSET` to short/category/long text through `valueNames[]`.

Adjacent comment-block text:

```c
/* Roll MIDI trigger offset menu label.
   The short text intentionally reuses SHORT_ROLL ("rol"). Raw 0 displays as
   "off"; raw 1..127 displays as the positive semitone offset. Keep this on an
   existing dtype path and avoid adding a new dtype. */
```

### `front/LxrAvr/Menu/MenuText.h`

Modify long names around lines 530-537.

Change type: add.

Add `{"RolOfset"}` after `{"RollMode"}` and before `{"NoteAmt"}` so it aligns with the new `LONG_ROLL_NOTE_OFFSET` enum.

No short text addition is required because `{"rol"}` already exists at line 328 for `SHORT_ROLL`.

Why this must exist:

The requested long name must be stored in the PROGMEM text table used by the menu renderer.

Inputs:

- `LONG_ROLL_NOTE_OFFSET` enum value.

Outputs/accessors:

- Menu long-name display reads `"RolOfset"`.

Adjacent comment-block text:

```c
/* Long label for the global MIDI roll-note offset.
   Spelling follows the requested eight-character display string "RolOfset". */
```

### `front/LxrAvr/Menu/menu.c`

Modify `valueNames[]` around lines 255-265.

Change type: add.

Add a `TEXT_ROLL_NOTE_OFFSET` entry after `TEXT_ROLL_MODE`:

```c
{SHORT_ROLL, CAT_MIDI, LONG_ROLL_NOTE_OFFSET},
```

Why this must exist:

The menu text enum needs a short/category/long tuple. `CAT_MIDI` places the setting in the MIDI category as requested.

Inputs:

- `TEXT_ROLL_NOTE_OFFSET`
- `SHORT_ROLL`
- `CAT_MIDI`
- `LONG_ROLL_NOTE_OFFSET`

Outputs/accessors:

- Menu renderer shows `rol`, `MIDI`, `RolOfset`.

Adjacent comment-block text:

```c
/* Global MIDI roll-note offset label.
   Uses the shared "rol" short text but categorizes the setting under MIDI,
   because this parameter changes external MIDI note interpretation rather
   than the existing performance roll button behavior. */
```

Modify `parameter_dtypes[]` around lines 583-591.

Change type: add.

Add:

```c
/*PAR_ROLL_NOTE_OFFSET*/ DTYPE_0B127,
```

after `PAR_MIDI_NOTE7`.

Why this must exist:

The menu needs an edit/display type for every `Parameters.h` enum. If no existing off-plus-127-values dtype exists, `DTYPE_0B127` gives the right edit bounds with no dtype churn; add only parameter-specific display handling so `0` shows `off`.

Inputs:

- `PAR_ROLL_NOTE_OFFSET`

Outputs/accessors:

- Numeric increment/decrement uses existing raw `0..127` handlers at lines 3054, 3287, and 4167.
- Display paths at lines 1641-1647 and 1747 need a `PAR_ROLL_NOTE_OFFSET && value == 0` exception if no existing dtype already displays `0` as `off` while allowing `1..127`.

Adjacent comment-block text:

```c
/* Roll MIDI trigger offset.
   Raw 0 displays as "off"; raw 1..127 displays and transmits as a positive
   semitone offset. DTYPE_0B127 is deliberate if no existing off-capable
   numeric dtype fits, so this feature does not spend a new dtype slot. */
```

Modify `menu_init()` around lines 676-681.

Change type: add.

Add:

```c
parameter_values[PAR_ROLL_NOTE_OFFSET] = 0;
```

Why this must exist:

`memset()` at line 660 already defaults it to `0`, but an explicit assignment documents the feature default beside other roll defaults and protects future initialization refactors.

Inputs:

- Startup/no-SD/no-config boot path.

Outputs/accessors:

- `parameter_values[PAR_ROLL_NOTE_OFFSET] == 0` until loaded or edited.

Adjacent comment-block text:

```c
/* MIDI roll notes default off.
   Old config files also normalize to this value when the appended byte is
   missing, so startup, old-file load, and fresh save all agree on 0/off. */
```

Modify `menu_parseGlobalParam()` around lines 4031-4042.

Change type: add.

Add a case before the `PAR_MIDI_NOTE1..7` block:

```c
case PAR_ROLL_NOTE_OFFSET:
   parameter_values[PAR_ROLL_NOTE_OFFSET] = value;
   avrComms_sendData(SEQ_CC, SEQ_ROLL_NOTE_OFFSET, (uint8_t)value);
   break;
```

Why this must exist:

The AVR owns the saved/menu value, but STM owns external MIDI parsing. Every menu edit and global restore must send the new raw offset to STM.

Inputs:

- `paramNr == PAR_ROLL_NOTE_OFFSET`
- `value` raw `0..127`

Outputs/accessors:

- AVR-to-STM command `SEQ_CC / SEQ_ROLL_NOTE_OFFSET / value`
- STM will release any MIDI-owned rolls if the value changes.

Adjacent comment-block text:

```c
/* Push the saved MIDI roll-note offset to STM.
   STM owns note routing and clears MIDI-owned roll holds when this value
   changes, which prevents a held roll note from sustaining under the old
   offset map. */
```

### `front/LxrAvr/Menu/menuPages.h`

Modify `MENU_MIDI_PAGE` third subpage around lines 381-383.

Change type: modify.

Replace the first empty slot after `TEXT_OSC_WAVE_INTERPOLATION`:

```c
TEXT_BUT_SHIFT_MODE, TEXT_FILE_LOAD_BACKGROUND, TEXT_OSC_WAVE_INTERPOLATION, TEXT_ROLL_NOTE_OFFSET, TEXT_EMPTY, ...
PAR_BUT_SHIFT_MODE,  PAR_FILE_LOAD_BACKGROUND,  PAR_OSC_WAVE_INTERPOLATION,  PAR_ROLL_NOTE_OFFSET,  PAR_NONE, ...
```

Why this must exist:

The setting is global and MIDI-facing, so it belongs in the global MIDI/settings page, not the performance roll page.

Inputs:

- Menu page navigation.

Outputs/accessors:

- User can edit the saved global roll offset from the settings menu.

Adjacent comment-block text:

```c
/* MIDI roll-note offset lives with global MIDI settings.
   It is intentionally separate from performance roll rate/note/velocity/mode,
   because it controls which external MIDI notes hold the roll engine. */
```

### `front/LxrAvr/Preset/presetManager.c`

No direct code change expected, but verify after adding the enum.

Relevant lines:

- `preset_writeGlobalData()` lines 381-384 writes `PAR_BEGINNING_OF_GLOBALS..NUM_PARAMS`.
- `preset_readGlobalData()` lines 408-412 defaults missing bytes to `0`.
- `.ALL` save lines 2288-2293 computes remaining global padding from `NUM_PARAMS - PAR_BEGINNING_OF_GLOBALS`.
- `.ALL` read lines 2454-2463 normalizes `0xff` filler to `0`.

Why no change:

Appending `PAR_ROLL_NOTE_OFFSET` before `NUM_PARAMS` is enough for current global serialization. The new byte consumes one existing padding byte in `.ALL`.

Verification:

- Confirm `64 - (NUM_PARAMS - PAR_BEGINNING_OF_GLOBALS)` remains non-negative after the enum append.
- Confirm old files missing the byte leave `PAR_ROLL_NOTE_OFFSET == 0`.

Adjacent comment-block text if this file is touched later:

```c
/* Appended global parameters are serialized by the existing NUM_PARAMS-bounded
   loops. Missing bytes and 0xff padding normalize to 0, which is the MIDI roll
   offset off state. */
```

## AVR/STM Protocol

### `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`

Modify around lines 256-258.

Change type: add.

Add:

```c
#define SEQ_ROLL_NOTE_OFFSET 0x6f
```

between `SEQ_BACKGROUND_SWAP_DONE 0x6e` and `SEQ_OSC_WAVE_INTERPOLATION 0x70`.

Why this must exist:

The front panel needs a one-byte command to push the global raw offset to STM.

Inputs:

- `SEQ_CC` command from `menu_parseGlobalParam()`
- `data2` raw offset `0..127`

Outputs/accessors:

- STM receives matching `FRONT_SEQ_ROLL_NOTE_OFFSET`.

Adjacent comment-block text:

```c
/* Raw global MIDI roll-note offset.
   data2 is 0/off or a positive 1..127 semitone offset above the normal
   trigger note; STM owns matching and release of any MIDI-held rolls when this
   value changes. */
```

### `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`

Modify around lines 202-204.

Change type: add.

Add:

```c
#define FRONT_SEQ_ROLL_NOTE_OFFSET 0x6f
```

between `FRONT_SEQ_BACKGROUND_SWAP_DONE 0x6e` and `FRONT_SEQ_OSC_WAVE_INTERPOLATION 0x70`.

Why this must exist:

The STM parser needs the same opcode value as AVR for the new `SEQ_CC` command.

Inputs:

- AVR `SEQ_ROLL_NOTE_OFFSET`

Outputs/accessors:

- `frontPanelReceivingProtocol.c` switch case.

Adjacent comment-block text:

```c
/* Front-panel command for the global MIDI roll-note offset.
   The value changes external MIDI note routing only; it does not alter
   performance roll rate, roll note, velocity, or mode. */
```

### `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Modify includes around lines 42-59.

Change type: add.

Add:

```c
#include "MidiParser.h"
```

Why this must exist:

This file directly mutates `midi_MidiChannels[]` and `midi_NoteOverride[]`, and it will call parser APIs to set roll offset and clear MIDI roll holds.

Inputs:

- Public declarations from `MidiParser.h`.

Outputs/accessors:

- `midiParser_setRollNoteOffset()`
- `midiParser_clearMidiRollHolds()`

Adjacent comment-block text:

```c
/* MidiParser owns external MIDI note routing and MIDI-held roll state.
   Front-panel mapping changes call into it so held roll notes cannot survive
   an offset, channel, or note-override remap. */
```

Modify MIDI channel assignment around lines 2582-2593.

Change type: modify.

After detecting a changed channel and before/after assignment, call `midiParser_clearMidiRollHolds()`. Keep the existing `voiceControl_noteOff(voice)` behavior.

Why this must exist:

Changing a global or voice MIDI channel while a roll note is held invalidates the note-off path that would release it.

Inputs:

- `FRONT_SEQ_MIDI_CHAN`
- `voice = data2 >> 4`
- `channel = (data2 & 0x0f) + 1`

Outputs/accessors:

- Existing `midi_MidiChannels[voice] = channel`
- New release of all MIDI-owned roll holds.

Adjacent comment-block text:

```c
/* MIDI channel changes invalidate the held-note map used by MIDI roll
   triggers. Clear parser-owned MIDI roll holds before the old channel can lose
   the note-off that would otherwise release them. */
```

Modify MIDI channel off around lines 2596-2599.

Change type: modify.

Call `midiParser_clearMidiRollHolds()` before or after `midi_MidiChannels[frontParser_command.data2] = 0`.

Why this must exist:

Disabling a channel while a MIDI roll note is held can otherwise leave the sequencer roll aggregate on.

Inputs:

- `FRONT_SEQ_MIDI_CHAN_OFF`
- `data2` voice/global channel slot index.

Outputs/accessors:

- `midi_MidiChannels[]`
- MIDI-owned roll releases.

Adjacent comment-block text:

```c
/* Turning a MIDI channel off removes the route that would receive release
   messages. Drop MIDI-owned roll holds so the roll engine cannot hang. */
```

Modify roll command block around lines 2727-2736.

Change type: add.

Add:

```c
case FRONT_SEQ_ROLL_NOTE_OFFSET:
   midiParser_setRollNoteOffset(frontParser_command.data2);
   break;
```

near the existing roll cases.

Why this must exist:

The new AVR global parameter must update STM parser state immediately on menu edit and on global restore.

Inputs:

- Raw offset `frontParser_command.data2`

Outputs/accessors:

- Parser-owned roll offset state.
- MIDI-owned roll holds cleared if the raw value changed.

Adjacent comment-block text:

```c
/* Update the external MIDI roll-note offset.
   The parser releases existing MIDI-held rolls on changes because old held
   notes may no longer map to the same voice after the offset update. */
```

Modify track note override block around lines 2947-2954.

Change type: modify.

Call `midiParser_clearMidiRollHolds()` when any `midi_NoteOverride[]` value changes.

Why this must exist:

The roll trigger relation is based on the same note override map as normal MIDI triggering. Changing a note override while a roll note is held invalidates the note-off match.

Inputs:

- `FRONT_SEQ_TRACK_NOTE1..7`
- `data1 - FRONT_SEQ_TRACK_NOTE1`
- `data2` new note override.

Outputs/accessors:

- `midi_NoteOverride[]`
- MIDI-owned roll releases.

Adjacent comment-block text:

```c
/* Note overrides define both normal MIDI triggers and shifted roll triggers.
   Clear MIDI-held rolls when a note override changes so the old shifted note
   cannot remain latched without a matching release. */
```

## STM Sequencer Roll Ownership

### `mainboard/LxrStm32/src/Sequencer/sequencer.h`

Modify roll-state comment around lines 133-138.

Change type: modify.

Mention that roll requested state is now an aggregate of manual/front-panel and MIDI ownership.

Why this must exist:

Callers need to understand that `seq_rollTriggered` is no longer written directly by every source.

Adjacent comment-block text:

```c
/* Roll requested state is the aggregate of front-panel/manual ownership and
   parser-owned MIDI note holds. The aggregate still drives the existing
   quantized seq_rollTriggered/seq_rollState engine. */
```

Modify declarations around lines 498-501.

Change type: add/modify.

Keep `seq_rollChange(uint8_t voice, uint8_t onOff)` as the manual/front-panel API and add:

```c
void seq_rollMidiChange(uint8_t voice, uint8_t onOff);
```

Why this must exist:

MIDI note-off must not clear a manually held roll, and manual release must not clear a MIDI-held roll.

Inputs:

- `voice`: sequencer track `0..6`
- `onOff`: nonzero held/on, zero released/off

Outputs/accessors:

- Updates MIDI ownership mask.
- Recomputes aggregate roll requested bit.

Adjacent comment-block text:

```c
/* Record a MIDI-owned roll hold for one voice.
   MIDI ownership is separate from front-panel roll buttons; either source can
   keep the aggregate roll request active until both sources release. */
```

### `mainboard/LxrStm32/src/Sequencer/sequencer.c`

Modify global roll state around lines 67-75.

Change type: add.

Add static masks:

```c
static uint8_t seq_rollManualHeld = 0;
static uint8_t seq_rollMidiHeld = 0;
```

near existing `seq_rollTriggered`.

Why this must exist:

The existing single `seq_rollTriggered` bitset cannot distinguish manual and MIDI roll ownership.

Inputs:

- Manual/front-panel calls through `seq_rollChange()`
- MIDI calls through `seq_rollMidiChange()`

Outputs/accessors:

- Aggregate writes to `seq_rollTriggered`

Adjacent comment-block text:

```c
/* Roll ownership by source.
   seq_rollTriggered remains the aggregate requested state consumed by the
   existing quantized roll engine; these masks remember which source is still
   holding each voice so one source cannot release the other's roll. */
```

Add an internal helper before line 1605.

Change type: add.

Add a static helper such as:

```c
static void seq_rollApplyAggregate(uint8_t voice)
```

Behavior:

- Return if `voice >= 7`.
- If `seq_rollManualHeld | seq_rollMidiHeld` has the voice bit, set `seq_rollTriggered`.
- Else clear `seq_rollTriggered` and, if `seq_rollRate != 0xff`, reset `seq_rollCounter[voice] = seq_rollRate`.

Why this must exist:

Both manual and MIDI wrappers need identical aggregate transition behavior.

Inputs:

- `voice`
- `seq_rollManualHeld`
- `seq_rollMidiHeld`
- `seq_rollRate`

Outputs/accessors:

- `seq_rollTriggered`
- `seq_rollCounter[voice]` on aggregate release.

Adjacent comment-block text:

```c
/* Rebuild the roll request bit for one voice from all held sources.
   The existing processing loop still observes seq_rollTriggered; this helper
   only decides whether the requested bit should remain set after a manual or
   MIDI source changes state. */
```

Modify `seq_rollChange()` around lines 1605-1623.

Change type: modify.

Replace direct `seq_rollTriggered` writes with:

- Set/clear `seq_rollManualHeld` bit for `voice`.
- Call `seq_rollApplyAggregate(voice)`.

Why this must exist:

The existing API becomes the manual/front-panel ownership wrapper without changing front-panel callers.

Inputs:

- `voice`
- `onOff`

Outputs/accessors:

- `seq_rollManualHeld`
- aggregate `seq_rollTriggered`

Adjacent comment-block text:

```c
/* Front-panel/manual roll source.
   This updates only the manual-held mask, then lets the shared aggregate logic
   decide whether the roll engine should stay requested because MIDI may still
   be holding the same voice. */
```

Add `seq_rollMidiChange()` after `seq_rollChange()`.

Change type: add.

Behavior:

- Return if `voice >= 7`.
- Set/clear `seq_rollMidiHeld` bit for `voice`.
- Call `seq_rollApplyAggregate(voice)`.

Why this must exist:

External MIDI note-on/off needs an independent roll source that can be sustained separately from the front-panel roll button.

Inputs:

- `voice`
- `onOff`

Outputs/accessors:

- `seq_rollMidiHeld`
- aggregate `seq_rollTriggered`

Adjacent comment-block text:

```c
/* MIDI roll-note source.
   MIDI note-on/off updates the MIDI-held mask only. The aggregate roll request
   remains active while either MIDI or manual ownership is still held. */
```

No changes expected to `seq_setRoll()` lines 1626-1688 or `seq_checkRollStep()` lines 1691-1707.

Why no change:

The existing roll engine should keep its quantization, early-roll, one-shot, and repeat behavior. Only the ownership source feeding `seq_rollTriggered` changes.

## STM MIDI Parser Roll Note Matching

### `mainboard/LxrStm32/src/MIDI/MidiParser.h`

Modify around lines 65-66 and 101-102.

Change type: add.

Add public APIs:

```c
void midiParser_setRollNoteOffset(uint8_t rawOffset);
void midiParser_clearMidiRollHolds(void);
```

Why this must exist:

Front-panel command handling needs to set the offset and release parser-owned holds on mapping changes.

Inputs:

- `rawOffset`: `0` off, `1..127` active.

Outputs/accessors:

- Parser state in `MidiParser.c`.
- Calls to `seq_rollMidiChange()`.

Adjacent comment-block text:

```c
/* MIDI roll-note offset and hold ownership.
   The parser owns these because it is the only layer that knows which incoming
   note/channel pairs map to shifted roll triggers. */
```

### `mainboard/LxrStm32/src/MIDI/MidiParser.c`

Add parser state near lines 158-164.

Change type: add.

Add:

```c
static uint8_t midiParser_rollNoteOffsetRaw = 0;
static uint8_t midiParser_rollHoldCount[7] = {0};
```

Why this must exist:

STM must remember the current global offset and count overlapping MIDI roll holds per voice.

Inputs:

- Front-panel `FRONT_SEQ_ROLL_NOTE_OFFSET`
- Incoming MIDI note-on/off messages.

Outputs/accessors:

- `seq_rollMidiChange(voice, 1)` on count `0 -> 1`
- `seq_rollMidiChange(voice, 0)` on count `1 -> 0`

Adjacent comment-block text:

```c
/* Parser-owned MIDI roll state.
   Raw 0 disables shifted roll notes; raw 1..127 is the positive semitone
   offset above the normal trigger note. Hold counters allow overlapping
   roll-note presses for the same voice without an early note-off releasing the
   roll. */
```

Add helpers after `midiParser_voiceNoteOverride()` around lines 86-90.

Change type: add.

Add static helpers:

- `midiParser_rollOffsetEnabled()`
- `midiParser_rollOffsetValue()`
- `midiParser_shiftedNoteInRange(uint8_t baseNote, uint8_t offset, uint8_t *shiftedNote)`
- `midiParser_shiftedBaseInRange(uint8_t incomingNote, uint8_t offset, uint8_t *baseNote)`
- `midiParser_rollNoteMatches(uint8_t incomingNote, uint8_t baseNote)`
- `midiParser_rollVoiceOn(uint8_t voice)`
- `midiParser_rollVoiceOff(uint8_t voice)`
- `midiParser_applyRollVoiceMask(uint8_t voiceMask, uint8_t isNoteOff)`

Why this must exist:

The note router needs small, testable logic for offset decode, MIDI-note bounds, hold counting, and calling the sequencer source API.

Inputs:

- Incoming note number.
- Normal trigger base note.
- Current raw offset.
- Per-voice hold count.

Outputs/accessors:

- Dedupe voice mask.
- Hold counter transitions.
- `seq_rollMidiChange()`.

Adjacent comment-block text:

```c
/* Roll-note helpers.
   Matching is defined against the normal note that would trigger the voice,
   then shifted upward by the current positive offset. If the shifted note would
   exceed 127, or an incoming note minus the offset would be below 0, no valid
   trigger exists and the roll path does nothing. Note-on and note-off status
   are interpreted literally by the caller. */
```

Add exported functions after `midi_clearCache()` around lines 91-106, or near other parser configuration setters.

Change type: add.

`midiParser_clearMidiRollHolds()` behavior:

- For voices `0..6`, if count is nonzero, set count to `0` and call `seq_rollMidiChange(voice, 0)`.

`midiParser_setRollNoteOffset(uint8_t rawOffset)` behavior:

- Mask/clamp to `0x7f`.
- If raw value differs from `midiParser_rollNoteOffsetRaw`, call `midiParser_clearMidiRollHolds()` first.
- Store the new raw value.

Why this must exist:

Changing offset or mappings must not leave MIDI-owned rolls held under stale note relations.

Inputs:

- New raw offset.
- Current hold counters.

Outputs/accessors:

- Updated `midiParser_rollNoteOffsetRaw`.
- MIDI roll release calls as needed.

Adjacent comment-block text:

```c
/* Set the global MIDI roll-note offset.
   A changed offset invalidates every outstanding shifted-note hold, so MIDI
   roll ownership is released before the new raw value is installed. */
```

Modify `midiParser_parseMidiMessage()` note branch around lines 264-323.

Change type: modify.

Replace the current note-routing block with a structured version that preserves existing calls but records consumption and builds a last-consumer roll mask:

1. Initialize:
   - `uint8_t normalConsumed = 0;`
   - `uint8_t rollVoiceMask = 0;`
   - `uint8_t isNoteOff = (msgonly == NOTE_OFF);`
2. Run existing global-channel path first when `midiParser_voiceMidiChannel(7) == chanonly`.
3. Whenever an existing normal route would call `channelMidiParser_noteOn()` or `channelMidiParser_noteOff()`, set `normalConsumed = 1`.
4. Run existing voice-channel loop second, preserving the current `do_rec` rules for active vs inactive tracks.
5. Whenever an existing voice-channel route would call a channel note function, set `normalConsumed = 1`.
6. Only after both existing paths have run, and only if `!normalConsumed`, roll offset is enabled, and the message is literal `NOTE_ON` or `NOTE_OFF`, build/apply `rollVoiceMask`.
7. Roll mask rules:
   - Global channel, active track note override off: subtract the positive offset from `incomingNote`; if that base note is in `0..127`, add `frontParser_activeTrack`.
   - Global channel, overrides on: scan `0..6`, compare `midi_NoteOverride[v] + offset`.
   - Voice channel: for every `v` whose assigned voice channel equals `chanonly`, compare either shifted-back chromatic range if override is `0`, or shifted override note if override is nonzero.
   - Any overflow/underflow means the roll trigger does not exist and no voice bit is added.
   - Dedupe with bitmask before applying.
8. Apply with `midiParser_applyRollVoiceMask(rollVoiceMask, isNoteOff)`.

Why this must exist:

The requested behavior is "roll as last consumer." Normal note triggering and all existing global/voice note routes must win before roll sees the note.

Inputs:

- `msg.status`
- `msg.data1` incoming note
- `msg.data2` velocity, used only by existing normal path for roll matching ignored except literal status
- `midi_MidiChannels[]`
- `midi_NoteOverride[]`
- `frontParser_activeTrack`
- `midiParser_rollNoteOffsetRaw`

Outputs/accessors:

- Existing `channelMidiParser_noteOn/off()` behavior unchanged for normal notes.
- New `seq_rollMidiChange()` calls through parser hold counters for shifted roll notes.

Adjacent comment-block text:

```c
/* Last-consumer MIDI roll-note path.
   The normal global and voice note routes run first and mark the message
   consumed when they would hand it to ChannelMidiParser. Only unconsumed
   literal NOTE_ON/NOTE_OFF messages are tested against the positive-offset
   roll map, and out-of-range shifted notes are ignored. */
```

Important preservation notes:

- Do not reinterpret `NOTE_ON` with velocity `0` as note-off.
- Do not change `ChannelMidiParser.c` prototypes just to return "accepted"; consumption is defined at the existing `MidiParser.c` routing decision points.
- Do not add voice-channel CC handling for roll rate.
- Do not add a separate pre-pass parser before normal note routing.

## Global NRPN 93 Roll Rate

### `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c`

Modify defines around lines 33-37.

Change type: add.

Add:

```c
#define GLOBAL_MIDI_NRPN_ROLL_RATE 93
```

and optionally:

```c
#define GLOBAL_MIDI_ROLL_RATE_MAX 15
```

Why this must exist:

NRPN 93 is a special performance command, not a raw preset parameter. A named constant prevents accidental table indexing.

Inputs:

- Selected NRPN number.

Outputs/accessors:

- Branch in `globalMidiParser_handleNrpnControl()`.

Adjacent comment-block text:

```c
/* Global NRPN 93 is a performance roll-rate command.
   It intentionally bypasses the raw parameter table because roll rate is not
   a kit sound parameter and existing Global CC assignments must not move. */
```

Do not modify `globalMidiParser_nrpnToRawParam[]` lines 169-263.

Why no table entry:

Indexes `0..92` map to high raw sound parameters. Adding roll rate to this table would misrepresent a performance control as a preset raw parameter and could route into `frontParser_applyParameterCommand()`.

Modify `globalMidiParser_handleNrpnControl()` around lines 361-373.

Change type: modify.

Inside `case NRPN_DATA_ENTRY_COARSE`, before the raw-table bounds check, add:

```c
if(globalMidiParser_nrpnSelected
   && globalMidiParser_activeNrpnNumber == GLOBAL_MIDI_NRPN_ROLL_RATE)
{
   uint8_t rate = msg.data2;
   if(rate > GLOBAL_MIDI_ROLL_RATE_MAX)
      rate = GLOBAL_MIDI_ROLL_RATE_MAX;
   seq_setRollRate(rate);
   return 1;
}
```

Why this must exist:

External Global NRPN 93 must set the same roll rate used by front-panel performance roll and MIDI-held roll notes.

Inputs:

- CC99/CC98 selected NRPN number.
- CC6 `msg.data2` value.

Outputs/accessors:

- `seq_setRollRate(rate)`
- No preset storage.
- No automation recording.
- No Global CC map changes.

Adjacent comment-block text:

```c
/* Apply Global NRPN 93 as roll rate.
   This is a live performance/sequencer setting, so CC6 clamps to the existing
   0..15 UI range and calls seq_setRollRate() directly instead of entering the
   raw sound-parameter apply/storage path. */
```

## Durable MIDI Documentation

### `knowledge_files/comms_spec_reference/MIDI_TABLE.md`

Modify around lines 274-284.

Change type: modify.

Change:

```md
Valid Global NRPN numbers are `0..92`.
```

to:

```md
Valid Global NRPN numbers are `0..93`.
```

Modify after line 378.

Change type: add.

Add table row:

```md
| 93 | Roll rate | Global performance | `seq_setRollRate(value clamped 0..15)` |
```

Add a short note near the note-routing guidance around line 64, or near the Global MIDI overview:

```md
MIDI roll notes use the saved global roll-offset setting. Raw 0 disables the
feature and displays as off; raw 1..127 is a positive semitone offset above the
normal trigger note. Shifted roll notes are evaluated only after the normal
global/voice note routes have had a chance to consume the message. If the
shifted note is outside MIDI note range, the roll path does nothing. Note-on
and note-off statuses are interpreted literally.
```

Why this must exist:

`MIDI_TABLE.md` is the durable external MIDI contract. NRPN 93 and the last-consumer roll-note behavior must be documented with the rest of the Global MIDI behavior.

## Files Expected Not To Change

### `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c`

No change expected.

Reason:

Existing note-on/off behavior remains the normal note consumer. The last-consumer roll decision belongs in `MidiParser.c`, where both global and voice paths are visible together. Avoid changing `channelMidiParser_noteOn()`/`noteOff()` return types because that would expand the blast radius.

### `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.h`

No change expected.

Reason:

No prototype changes are required if consumption is tracked at `MidiParser.c` routing points.

## Verification Schedule

1. Static/code review:
   - Confirm `PAR_ROLL_NOTE_OFFSET` is appended, not inserted.
   - Confirm `parameter_dtypes[]` count matches `NUM_PARAMS`.
   - Confirm `valueNames[]`, `MenuText.h`, and `NamesEnum` stay aligned.
   - Confirm opcode `0x6f` matches on AVR and STM.
   - Confirm `frontPanelReceivingProtocol.c` includes `MidiParser.h`.

2. Build:
   - `make -C mainboard/LxrStm32 -j4 stm32`
   - `make -C front/LxrAvr avr -j4`
   - `make firmware`

3. MIDI behavior tests:
   - Offset `0`: displays `off`; all existing normal MIDI notes unchanged; shifted roll notes ignored.
   - Raw `24`: Drum 1 chromatic C1-C2 on channel 7 rolls from C3-C4 on channel 7.
   - Positive-only lower bound: incoming roll notes lower than the offset have no valid shifted-back base note and do nothing.
   - Positive-only upper bound: assigned notes whose `baseNote + offset` exceeds `127` do not create roll triggers.
   - Global channel assigned, active track override off: shifted chromatic notes roll active track only.
   - Global channel assigned, overrides on: shifted override notes roll matching voices.
   - Voice channel assigned, override off: shifted chromatic notes roll that voice.
   - Voice channel assigned, override on: only shifted override note rolls that voice.
   - Global and voice channels both assigned: either assigned path can roll the voice, but one incoming message dedupes per voice.
   - Standard consumed note does not fall through to roll.
   - Neither global nor voice channel assigned: no roll trigger.
   - Literal note-on velocity `0` on a shifted roll note starts roll; literal note-off releases it.
   - Multiple note-ons for the same voice require matching note-offs/countdown to zero before release.
   - Manual roll held plus MIDI note-off: manual roll remains active.
   - MIDI roll held plus manual release: MIDI roll remains active.
   - Offset change releases all MIDI-owned rolls.
   - MIDI channel change/off releases all MIDI-owned rolls.
   - MIDI note override change releases all MIDI-owned rolls.

4. NRPN tests:
   - Global NRPN 93 + CC6 values `0..15` set roll rate.
   - Global NRPN 93 + CC6 value above `15` clamps to `15`.
   - Existing Global CC16 still controls Snare Osc 1 fine tune.
   - Voice-channel CCs do not control roll rate.
   - Existing NRPN `0..92` raw parameter mappings still work.

5. Save/load tests:
   - Fresh boot default displays `off`.
   - Save `glo.cfg`, reboot/load, value persists.
   - Load older `glo.cfg` without the appended byte: value becomes `0`.
   - Save/load `.ALL`: global padding remains valid and value persists.

## Follow-Up Checks Before Coding

- Confirm there is enough `.ALL` global padding after incrementing `NUM_PARAMS`; current line 2293 formula should remain positive.
- Confirm whether an existing dtype already supports `0=off` display plus `1..127` numeric values. Current research suggests no generic dtype does; if that holds, add only the scoped `PAR_ROLL_NOTE_OFFSET == 0` display exception and keep `DTYPE_0B127`.
