# MIDI Roll Trigger Plan

## Goal

Add a separate MIDI note trigger for the existing per-instrument roll engine. A normal MIDI note should keep triggering voices exactly as it does today. A roll MIDI note should start that voice's roll on note-on and keep it sustained until the matching note-off.

Also add:

- A saved global setting shown in the settings menu:
  - short text: `rol`
  - category: `MIDI`
  - long name: `RolOfset`
  - default/display: `off`
  - value: raw `0` is off; raw `1..127` is a positive semitone offset above the assigned note. No negative offsets.
- Global NRPN 93 assignment for the existing performance roll-rate parameter (`PAR_ROLL`).

No code should be changed until the implementation details below are followed.

## Product Knowledge Summary

- External MIDI input is STM-owned. Byte parsing starts in `mainboard/LxrStm32/src/MIDI/MidiParser.c`.
- Per-channel notes currently route through `MidiParser.c` into `channelMidiParser_noteOn()` / `channelMidiParser_noteOff()`.
- Global-channel notes currently trigger the active front-panel track in chromatic mode when that track has no note override, or scan all seven configured note overrides when overrides are assigned.
- Voice-channel notes currently trigger each matching voice channel, with the per-voice note override checked in `ChannelMidiParser.c`.
- Roll playback already exists in `mainboard/LxrStm32/src/Sequencer/sequencer.c`:
  - `seq_rollChange(voice, onOff)` records roll button state changes.
  - `seq_setRoll()` starts/stops or quantizes the roll.
  - `seq_checkRollStep()` repeats while roll is active.
  - `seq_setRollRate()`, `seq_setRollNote()`, `seq_setRollVelocity()`, and `seq_rollMode` already apply the performance roll controls.
- AVR performance menu already exposes:
  - `PAR_ROLL` as roll rate.
  - `PAR_ROLL_NOTE`, `PAR_ROLL_VELOCITY`, and `PAR_ROLL_MODE`.
  - `PAR_ROLL` sends `SEQ_ROLL_RATE` to STM.
- Global settings are AVR-owned for UI and SD save/load. `glo.cfg` and `.ALL` write bytes from `PAR_BEGINNING_OF_GLOBALS` to `NUM_PARAMS`.
- The `.ALL` global block has 64 reserved bytes, so appending one global byte should fit without a file format resize.
- Session 029 intentionally replaced old Global CC2-127 side effects with the current global CC/NRPN table. Old Global CC16 roll-rate is no longer safe: it is now Global CC16 = Snare Osc 1 fine tune.

## Recommended Design

### 1. Add The Global Roll Offset Setting

Append a new AVR global parameter near the end of the global enum, after the existing MIDI note globals and before `NUM_PARAMS`, rather than inserting it before `PAR_GLOBAL_SETTINGS_VERSION` or `PAR_MIDI_NOTE1`.

Reason: inserting in the middle would shift every later global byte and corrupt reads of existing `glo.cfg` / `.ALL` files. Appending means older files simply leave the new byte missing or `0xff` padding, both of which can be normalized to off.

Suggested representation:

- Use an existing datatype if one supports `0=off, 1..127=numeric`; otherwise keep `DTYPE_0B127` and add only minimal `PAR_ROLL_NOTE_OFFSET`-specific display handling for `0 -> off`.
- Stored byte `0`: roll trigger disabled; display `off`.
- Stored byte `1..127`: active positive semitone offset above the standard trigger note.
- Do not add a new dtype and do not add signed/centered display behavior.
- When this setting changes, immediately switch off all active MIDI-roll holds so no sustained roll can hang under the old offset mapping.

Implementation touch points later:

- `front/LxrAvr/Parameters.h`: add the appended `PAR_ROLL_NOTE_OFFSET` or similar.
- `front/LxrAvr/Menu/menu.h`: add text enum ids for the new label.
- `front/LxrAvr/Menu/MenuText.h`: add `rol`, `MIDI`, and `RolOfset` text entries.
- `front/LxrAvr/Menu/menu.c`: add dtype and default. Prefer an existing `0=off, 1..127=numeric` dtype if available; otherwise use `DTYPE_0B127` plus a tiny `PAR_ROLL_NOTE_OFFSET == 0` display exception. Do not add a new dtype because the dtype enum is already capped at 16.
- `front/LxrAvr/Menu/menuPages.h`: place the setting in an empty global settings slot.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h` and `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`: add a `SEQ_ROLL_NOTE_OFFSET` / `FRONT_SEQ_ROLL_NOTE_OFFSET` opcode.
- `front/LxrAvr/Menu/menu.c`: send that opcode from `menu_parseGlobalParam()`.
- STM MIDI parser: store decoded offset in parser-owned state, default off; changing this value must clear/release every MIDI-owned roll.

### 2. Match Roll Notes As The Last Consumer

Add roll-note detection to the actual global and voice note paths in `MidiParser.c`, but only as the last consumer. This should not be patched on as an independent first-pass parser; it needs to share the real "would this note trigger this voice?" decision points.

For every incoming note-on/note-off:

1. If roll offset is off, skip roll-note logic.
2. Decode the incoming MIDI channel as today.
3. First run the normal note-consumer path.
4. If a normal note trigger or any other earlier consumer accepts the incoming note, do not let the note fall through to roll.
5. If no earlier consumer accepts it, build a voice bitmask of voices whose normal trigger note would equal the incoming note after applying the configured roll offset.
6. Only include a voice if the channel path that would normally trigger it is assigned:
   - Global channel assigned and matching the incoming channel.
   - Voice channel assigned and matching the incoming channel.
7. Deduplicate voice matches so a global+voice overlap cannot double-count the same voice from one incoming message.
8. On note-on, start MIDI roll for those voices.
9. On note-off, release MIDI roll for those voices.

Recommended matching rules:

- Any note on any assigned channel that would otherwise trigger the voice should be eligible for roll when received with the configured offset.
- Voice channel with note override off: any incoming note within the shifted chromatic range can roll that voice.
- Voice channel with note override on: `incomingNote == noteOverride + offset`.
- Global channel with active track note override off: shifted chromatic roll for the active track.
- Global channel with overrides on: scan all seven overrides and roll matching voices, exactly like standard global note routing.
- Global and voice-channel assignments can both exist simultaneously; a matching roll note on either assigned channel should work.
- Because the offset is positive-only, shifted notes above `127` should not match.
- If subtracting the offset from an incoming roll note would produce a base note below `0`, or if an assigned note plus offset would be above `127`, that trigger does not exist and the roll path does nothing.

Note-on and note-off status should be read literally: note-on is note-on, note-off is note-off. Do not reinterpret note-on with velocity `0` as note-off for this feature.

### 3. Sustain Roll Until Note-Off

Do not call `seq_rollChange()` directly from note-on/off without source tracking. The current roll state is shared with manual front-panel roll buttons, so a MIDI note-off could cancel a manually held roll.

Recommended implementation:

- Add source-aware roll ownership in `sequencer.c`, for example:
  - manual source bit
  - MIDI source bit
  - aggregate state drives the existing `seq_rollTriggered` / `seq_rollState` transition
- Keep `seq_rollChange(voice, onOff)` as the manual/front-panel wrapper.
- Add a new internal/exported `seq_rollMidiChange(voice, onOff)` for MIDI.
- Roll stays active while either manual or MIDI source remains held.

For MIDI holds, maintain at least a per-voice held count or bit state in `MidiParser.c`.

Preferred robust-enough option:

- Build a deduped voice bitmask per incoming roll note message.
- Increment one per-voice MIDI hold counter on note-on.
- Decrement on note-off.
- Call `seq_rollMidiChange(voice, 1)` only on transition `0 -> 1`.
- Call `seq_rollMidiChange(voice, 0)` only on transition `1 -> 0`.
- Clear MIDI roll holds on roll-offset changes, all-notes-off, channel reassignment, and full parser reset if those paths are touched.

Riskier but simpler option: no hold count, just note-on = on and note-off = off. This is vulnerable to overlapping held roll notes for the same voice.

### 4. Roll Rate MIDI Assignment

Current status:

- Existing performance roll-rate is `PAR_ROLL`.
- AVR sends it to STM through `SEQ_ROLL_RATE`.
- Old external Global CC16 roll-rate existed in `knowledge_files/reference_material/lxr-midi-assign.txt`, but Session 029 removed old Global CC2-127 side effects.
- Current Global CC table already uses CC16 for Snare Osc 1 fine tune.
- The current Global CC table has no safe non-NRPN CC gap: CC0/1 are special, CC6/98/99 are NRPN mechanics, and the rest of CC2-127 are assigned.

Decision:

- Do not re-use Global CC16.
- Do not alter any currently assigned Global MIDI CC mapping.
- Do not use voice-channel CCs for roll rate.
- Add Global NRPN 93 = Roll Rate, value `0..15`.

## Implementation Outline

1. Add AVR menu/storage metadata for the appended global roll-offset parameter.
2. Add the AVR-to-STM opcode and parse it on STM into an off/active offset state.
3. On roll-offset changes, release all MIDI-owned rolls before storing the new value.
4. Identify the actual global-note and voice-note trigger paths in `MidiParser.c`, then add roll matching as the last consumer on those paths:
   - normal note trigger and earlier consumers win;
   - roll sees only notes not consumed earlier;
   - decode positive offset;
   - test global-channel eligibility;
   - test voice-channel eligibility;
   - produce a deduped voice mask.
5. Add source-aware roll state in `sequencer.c` so manual and MIDI holds do not cancel each other.
6. Add MIDI roll hold accounting in `MidiParser.c`.
7. Add Global NRPN 93 for roll rate.
8. Update `knowledge_files/comms_spec_reference/MIDI_TABLE.md` with:
   - roll note offset behavior;
   - Global NRPN 93 roll-rate assignment;
   - last-consumer note routing behavior.
9. Build:
   - `make -C mainboard/LxrStm32 -j4 stm32`
   - `make -C front/LxrAvr avr -j4`
   - `make firmware`

## Verification Matrix

- Default/off offset: existing MIDI note triggering unchanged and no MIDI roll note is accepted.
- Positive offset example: Drum 1 on channel 7, chromatic C1-C2 standard notes; offset `24`; C3-C4 starts/stops roll on channel 7.
- Upper-bound example: assigned notes whose positive offset would exceed MIDI note `127` do not create roll triggers.
- Lower-bound example: incoming roll notes below the offset have no valid shifted-back base note and do nothing.
- Global channel assigned, active track chromatic: shifted notes roll only active track.
- Global channel assigned, note overrides active: shifted override notes roll matching tracks.
- Voice channel assigned, note override off: shifted chromatic roll works.
- Voice channel assigned, note override on: only shifted override note rolls.
- Global channel and voice channel both assigned: a valid shifted note on either channel can trigger roll for the voice.
- A standard note that is consumed by normal triggering must not also start roll.
- Neither global nor voice channel assigned: no roll trigger.
- Note-on starts roll; matching note-off stops it.
- Note-on with velocity 0 is still treated as note-on for the roll trigger path.
- Manual roll button held while MIDI roll note-off arrives: manual roll stays active.
- MIDI roll note held while manual roll button released: MIDI roll stays active.
- Multiple roll note-ons for same voice do not stop until all corresponding note-offs are received.
- Changing roll offset in the settings menu releases all MIDI-owned rolls.
- Roll rate changed by front-panel `PAR_ROLL` still works.
- Global NRPN 93 changes the same rate used by MIDI-held rolls.
- Existing Global CC16 still controls Snare fine tune.
- No existing Global CC mapping changes.
- No voice-channel CC controls roll rate.
- Save/load `glo.cfg` and `.ALL`: old files default roll offset to off; newly saved files preserve it.

## Risks

- Global parameter insertion can corrupt old config/all-file reads if inserted in the middle. Append only.
- The AVR menu dtype system is full; avoid new dtype work. If no existing `0=off, 1..127=numeric` dtype fits, use `DTYPE_0B127` and a parameter-specific `off` display exception.
- Existing Global CC table has no safe non-NRPN CC gap. Reusing old Global CC16 would be a regression against Session 029 and is explicitly out of scope.
- Some controllers send note-on velocity `0` as note-off, but this feature should read statuses literally. Those controllers may need to send real note-off to release MIDI rolls.
- Source ownership matters. A naive MIDI note-off could stop a manual roll.
- Hihat has two track channels mapped to one synth voice area but two sequencer tracks. Roll matching should stay track-oriented, not synth-voice-oriented.
- Roll state currently quantizes starts via existing roll behavior. MIDI-held rolls will inherit early-roll and quantization rules, which is probably desired but should be tested.
- Existing code may double-trigger normal notes when global and voice channels overlap; roll logic should dedupe its own voice mask even if normal note behavior remains unchanged.
- Roll matching must be placed on the real global and voice paths as the last consumer. A separate parser-layer pre-pass would violate the requested behavior.

## Settled Decisions

1. Roll offset is positive-only: `0` means off, `1..127` means that many semitones above the assigned note.
2. Disabled should display as `off`; avoid a new dtype unless an existing one cannot be reused and a tiny parameter-specific display exception is insufficient.
3. Roll is the last consumer. If normal note trigger or any other earlier consumer takes the note, it must not fall through to roll.
4. Note-on and note-off statuses are literal. Do not treat note-on velocity `0` as note-off for this feature.
5. Roll rate uses Global NRPN 93.
6. Do not alter current Global MIDI CC mappings. Do not use voice-channel CCs for roll rate.
7. Any note on any assigned global or voice channel that would otherwise trigger the voice is eligible for roll when received with the configured offset.
8. Changing the roll-offset setting must switch off all MIDI-owned rolls to prevent hangs.
