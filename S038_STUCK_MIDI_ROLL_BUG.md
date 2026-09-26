# S038 — Stuck MIDI Roll Bug: Investigation and Fix Plan

**Date**: 2026-09-26
**Branch**: `dev-roll-midi` (MIDI roll feature: commits `52d4ca4`, `9c9c67f`, `9576ae5`)
**Status**: root cause identified by code reading; **no code changed yet**. Waiting for the user to approve the plan, and in particular the reversal of one design decision (see §4.1).

---

## 1. User Report

> Triggering notes remotely works fine but triggering roll leads to stuck roll of corresponding instrument like there is no response to note off yet. However [many] rolls I launch that's how many get stuck. You can stuck all rolls and all instrument leds would blink synchronously. Until you change midi ch on ANY instrument. After this all roll stucking on every instrument ends instantly. Also when every roll stucks brutal voice stealing takes place. Although you can tweak roll params from LXR PERF screen and it sounds pretty nice. I was sending midi from OT btw and didn't tried to tweak NRPN 93 because of that (OT can't send NRPNs).

Source device: **Elektron Octatrack** MIDI tracks.

---

## 2. Symptoms Mapped to Code

| Observation | What it tells us |
|---|---|
| Roll starts on the roll note | Note-on matching, the offset maths, `seq_rollMidiChange(v,1)` and the sequencer roll engine all work. |
| Roll never stops on key release | The release never takes the per-voice hold count to 0, so `seq_rollMidiChange(v,0)` is never called. |
| Every launched roll sticks, independently | The problem is per-message and not tied to one voice, channel or mode. |
| Changing the MIDI channel on **any** track releases **all** stuck rolls at once | `FRONT_SEQ_MIDI_CHAN` / `FRONT_SEQ_MIDI_CHAN_OFF` call `midiParser_clearMidiRollHolds()`, which zeroes every counter and calls `seq_rollMidiChange(v,0)`. The sequencer's release path therefore works. The fault is entirely in the parser's hold accounting. |
| Normal MIDI notes work fine | Normal notes are one-shots. A lost or misread note-off makes no audible difference to them (see §3.3). |
| PERF-screen roll params work | `PAR_ROLL`, `PAR_ROLL_NOTE`, `PAR_ROLL_VELOCITY` and `PAR_ROLL_MODE` are not involved in the bug. |

---

## 3. Root Cause

### 3.1 The primary defect: a velocity-0 note-on is counted as a *second press*

The feature was built on a settled design decision (`MIDI_ROLL_TRIGGER.md`, *Settled Decisions* #4):

> Note-on and note-off statuses are literal. Do not treat note-on velocity `0` as note-off for this feature.

The implementation does exactly that:

- [MidiParser.c:401](mainboard/LxrStm32/src/MIDI/MidiParser.c#L401): `const uint8_t isNoteOff = (msgonly == NOTE_OFF);` The velocity is ignored.
- [MidiParser.c:547](mainboard/LxrStm32/src/MIDI/MidiParser.c#L547): `midiParser_applyRollVoiceMask(rollVoiceMask, isNoteOff)`
- [MidiParser.c:153](mainboard/LxrStm32/src/MIDI/MidiParser.c#L153) `midiParser_rollVoiceOn()` increments `midiParser_rollHoldCount[voice]`.

The MIDI 1.0 specification defines **Note-On with velocity 0 as equivalent to Note-Off**. Many hardware sequencers send it this way on purpose, because it keeps running status unbroken and saves a byte per release. When a device releases a roll key like that, this happens:

```
0x9n  note  vel>0   -> hold count 0 -> 1, seq_rollMidiChange(v,1)   roll starts
0x9n  note  vel=0   -> hold count 1 -> 2  ("second press")            roll continues
... every further tap on that key adds 2 and never subtracts ...
```

Nothing but `midiParser_clearMidiRollHolds()` can bring the count back to 0. That matches the report exactly: each launched roll sticks, and any channel change releases them all.

This decision was listed as a *risk* in the original design ("Those controllers may need to send real note-off to release MIDI rolls"). The consequence was underestimated: such controllers don't simply fail to release, they *add another hold*.

### 3.2 Why this explanation is favoured over others

Suppose the OT were sending genuine `0x8n` note-offs. Then the note-off would have to take a different route through the parser than the note-on did, for the count to stay above 0. The only inputs that can differ between the note-on and the note-off of the same key are:

- `frontParser_activeTrack`, but only on the global-channel chromatic route;
- the channel map, note overrides, or offset. Changes to any of these already clear all holds.

None of these changed in the user's test. Every launched roll stuck on every instrument, so the note-off must be failing on the voice-channel routes too, where nothing is asymmetric. A velocity-0 note-on is the only mechanism that fits every observation.

**Still to confirm** (a cheap check, not a blocker): put a MIDI monitor on the Octatrack's output, or ask the user, to see whether its MIDI-track note-offs are `9n kk 00`. The fix in §4.1 is correct either way, because it is what the MIDI specification requires.

### 3.3 Why normal notes looked fine

`channelMidiParser_noteOn()` ([ChannelMidiParser.c:45](mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c#L45)) already treats `vel == 0` as "no trigger". It skips `voiceControl_noteOn()`, records with `isNoteOff = 1`, and echoes a velocity-0 note-on. `channelMidiParser_noteOff()` sets `vel = 0` and delegates to the same function. For normal notes, the two statuses already behave identically. Only the new roll path distinguishes them.

### 3.4 Contributing weakness: the hold count is per voice, and the release target is recomputed

Even with genuine `0x8n` note-offs, the current design has these fragilities:

1. **The note-off recomputes its voice mask** from the *current* routing instead of releasing what its note-on claimed. On the global-channel chromatic route the mask is `1 << frontParser_activeTrack`. Change the active track on the front panel while holding a roll key, and the note-off decrements the wrong voice. The original voice sticks, and the other voice's count underflows (it is guarded, so nothing happens).
2. **Duplicate note-ons for a key that is already held** (a legato or overlapping trig, or an instrument that resends) each add a count. They need the same number of note-offs, and most senders send only one per key.
3. **Unmatched note-offs** decrement whichever voice they map to. The implementation doc lists this as a residual risk.

All three have the same cause: the hold is keyed by *voice* when it should be keyed by the *MIDI key* (channel + note). The fix below addresses the primary defect and this weakness together.

### 3.5 "Brutal voice stealing" — probably a consequence, not a separate defect

This happened only when *every* roll was stuck. While a roll is active on a track, that track's voice is retriggered at the roll rate (default 1/16), and every retrigger cuts the previous hit's tail. The two hi-hat tracks (5 and 6) share one synth voice, so both rolling at once makes them choke each other continuously. Seven simultaneous endless rolls would sound like aggressive voice stealing.

**Plan**: no code change. Retest after the fix. If it still happens with only one or two held rolls, open a separate investigation.

---

## 4. Fix Plan

All changes are STM-side and confined to `mainboard/LxrStm32/src/MIDI/MidiParser.c`, plus documentation. The sequencer's source-aware aggregation (`seq_rollMidiChange()` / `seq_rollApplyAggregate()`, [sequencer.c:1613](mainboard/LxrStm32/src/Sequencer/sequencer.c#L1613)) is correct and stays as it is.

### 4.1 Treat Note-On velocity 0 as Note-Off (reverses Settled Decision #4)

At [MidiParser.c:401](mainboard/LxrStm32/src/MIDI/MidiParser.c#L401):

```c
const uint8_t isNoteOff = (msgonly == NOTE_OFF)
                       || (msgonly == NOTE_ON && msg.data2 == 0);
```

- **Effect on normal notes: none audible or recorded.** With this change a velocity-0 note-on is routed to `channelMidiParser_noteOff()` instead of `channelMidiParser_noteOn()`. As §3.3 shows, those two functions already do the same thing for `vel == 0`: no trigger, `seq_addNote(..., 1)`, and a velocity-0 echo.
- **Effect on rolls:** a velocity-0 note-on now releases. It can no longer *start* a roll. That was never musically meaningful, because roll hits take their velocity from the roll engine, not from the incoming note.
- Add an adjacent comment block explaining *why* (MIDI 1.0 equivalence, running-status senders such as the Octatrack, and this session's bug), so it is not "corrected" back to literal statuses later.

**This needs explicit user sign-off**, because it reverses a documented settled decision.

### 4.2 Key MIDI roll holds by (channel, note), not by voice

Replace `midiParser_rollHoldCount[7]` ([MidiParser.c:95](mainboard/LxrStm32/src/MIDI/MidiParser.c#L95)) with a small held-key table:

```c
typedef struct {
   uint8_t chan;       /* 1..16, 0 = free slot */
   uint8_t note;       /* incoming (shifted) note number */
   uint8_t voiceMask;  /* voices this key claimed at note-on */
} MidiRollHeldKey;

#define MIDI_ROLL_MAX_HELD_KEYS 16
static MidiRollHeldKey midiParser_rollHeldKeys[MIDI_ROLL_MAX_HELD_KEYS];
static uint8_t midiParser_rollHeldVoiceMask = 0;   /* OR of all slot masks */
```

Behaviour:

- **Note-on**, with a non-empty computed `rollVoiceMask`:
  - If this `(chan, note)` is already held, do nothing (idempotent, which fixes §3.4 point 2).
  - Otherwise store it in a free slot along with the mask. **If the table is full, ignore the note-on.** It is safer not to start a roll than to start one with no way to release it.
- **Note-off**: look up `(chan, note)`. If found, free the slot. **The released voices are the ones stored in the slot**, not a mask recomputed from current routing (fixes §3.4 point 1). If not found, ignore it (fixes §3.4 point 3).
- After any change to the table, recompute the OR of all slot masks. For each voice bit that changed against `midiParser_rollHeldVoiceMask`, call `seq_rollMidiChange(v, newBit)`, then store the new mask. This keeps "a voice rolls while *any* held key claims it" without per-voice counters.
- **Where the note-off lookup goes:** do it for every note-off that reaches the note block. Do not put it behind the `!normalConsumed && midiParser_rollOffsetEnabled()` gate at [MidiParser.c:489](mainboard/LxrStm32/src/MIDI/MidiParser.c#L489). A key that was accepted as a roll must always be releasable, even if routing has since changed in some way that doesn't trigger a clear. The *note-on* side keeps the existing gate, so roll stays the last consumer.
- `midiParser_clearMidiRollHolds()` ([MidiParser.c:212](mainboard/LxrStm32/src/MIDI/MidiParser.c#L212)) clears the table and releases every voice in `midiParser_rollHeldVoiceMask`. All of its existing call sites stay as they are: offset change, channel change or off, note-override change, `midi_clearCache()`.

The table costs about 49 bytes of RAM. A lookup is a linear scan of 16 entries per note message, which is negligible.

### 4.3 Not changing (explicitly)

- `seq_setRoll()` early-roll behaviour and its recording (Session 037 rule).
- `seq_addNote()`'s `isNoteOff` contract. Only `ChannelMidiParser` passes `1`, and this fix does not change that.
- The last-consumer ordering and the chromatic-candidate logic from the `9c9c67f` post-fix.
- The Global CC table and NRPN 93.
- AVR-side code. No AVR change is needed.

### 4.4 Optional, needs a user decision

- **Release MIDI roll holds on sequencer stop / MIDI Stop.** Today a held roll whose sequencer is stopped simply pauses (roll processing runs inside `seq_process()`) and resumes on restart. Once note-offs are handled correctly this should not matter, and most senders flush note-offs on stop. Recommendation: **don't add it** unless hardware testing shows a need.
- **An All-Notes-Off hook.** It is not available cleanly. On voice channels, CC120 is already *track mute*, and on the global channel CC123 is *Drum 2 LFO amount*. Do not add one.

---

## 5. Verification

### 5.1 Host-side harness (recommended, following the Session 037 method)

Copy the roll section of `midiParser_parseMidiMessage()` and its helpers **verbatim** into a host C harness, with stubs for `seq_rollMidiChange()`, `channelMidiParser_noteOn/Off()` and the channel and override tables. Drive it with byte-level message sequences and assert on the stubbed `seq_rollMidiHeld` mask:

1. `9n k 64`, then `9n k 00`: the mask is set, then cleared. Reproduces the bug before the fix; passes after.
2. `9n k 64`, then `8n k 40`: set, then cleared.
3. `9n k 64` twice, then one release: cleared (idempotent note-on).
4. Two different keys that both map to voice v, released one at a time: v stays held until the second release.
5. Global chromatic route: hold a key, change `frontParser_activeTrack`, release: the original voice is released and no other voice is touched.
6. An unmatched note-off: no change.
7. A full table: the 17th distinct key does not start a roll, and all 16 held keys still release.
8. Manual hold plus MIDI release, and the reverse: the aggregate stays correct. This is a sequencer-side check, and the code for it is unchanged.

### 5.2 Build

```
make -C mainboard/LxrStm32 clean && make -C mainboard/LxrStm32 -j4 stm32
make firmware
```

The build is expected to be clean apart from the pre-existing warnings documented in `MEMORY.md`.

### 5.3 Hardware (user)

- **Octatrack** (the original reproduction): each roll key starts on press and stops on release, on voice channels and on the global channel.
- **Renoise or a DAW** (senders that use `0x8n` note-offs): the same result.
- Hold several roll keys on different instruments and release them in any order: each instrument stops on its own release.
- Hold a front-panel roll button and send a MIDI roll release: the manual roll continues. The reverse holds as well.
- Changing the MIDI channel, note override or roll offset still releases everything.
- Normal (unshifted) MIDI notes still trigger and record as before, including velocity-0 recording placement.
- The "voice stealing" impression is gone when only one or two rolls are held (§3.5).
- If Renoise is available: NRPN 93 changes the roll rate of a MIDI-held roll. This has not been tested yet.

---

## 6. Documentation Updates (after the fix is verified)

- `knowledge_files/comms_spec_reference/MIDI_TABLE.md`: replace "NOTE_ON/NOTE_OFF statuses are interpreted literally, including NOTE_ON with velocity `0`" with the velocity-0-equals-note-off rule and the per-key hold semantics.
- `MIDI_ROLL_TRIGGER.md`: amend Settled Decision #4 and the verification-matrix row "Note-on with velocity 0 is still treated as note-on", with a pointer to this document.
- `MIDI_ROLL_TRIGGER_IMPLEMENTATION.md`: note the correction. Its "Residual Risks" per-voice hold item is resolved by §4.2.
- `MEMORY.md` reminder: *"MIDI roll holds are keyed by (channel, note) and release the voice mask captured at note-on. A Note-On with velocity 0 is a Note-Off, per the MIDI spec. Never count it as a press."*
- Session handoff log `038_SESSION_HANDOFF_LOG.md` at closeout.

---

## 7. Open Questions for the User

1. **Approve reversing Settled Decision #4?** Should a velocity-0 note-on be treated as a note-off? This is required to fix the Octatrack case.
2. Can you confirm, with a MIDI monitor or from the OT's MIDI settings, that the OT sends `9n kk 00` for note-offs? This is diagnostic only. The fix goes ahead either way.
3. Should MIDI-held rolls be released on sequencer or MIDI Stop (§4.4)? Recommendation: no.
