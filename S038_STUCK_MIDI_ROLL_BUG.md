# S038 — Stuck MIDI Roll Bug: Investigation and Fix Plan

**Date**: 2026-09-26
**Branch**: `dev-roll-midi` (MIDI roll feature: commits `52d4ca4`, `9c9c67f`, `9576ae5`)
**Status**: implementation landed in the STM32 parser/sequencer; host-harness
and STM32 build verification pass. Hardware verification remains pending.
The implementation record is `S038_STUCK_ROLL_IMPLEMENTATION.md`.

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

### 4.4 Settled in Session 038

- **Release MIDI roll holds on sequencer stop / MIDI Stop.** Implemented as a
  stop-time parser-table clear. A MIDI roll pressed while stopped re-triggers
  immediately on a private tempo/rate clock, and a held stopped roll hands
  over to the normal quantized engine when playback starts (D3/D4).
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

1. **Settled:** a velocity-0 note-on is treated as a note-off, as required by
   MIDI 1.0 and the Octatrack reproduction.
2. Can you confirm, with a MIDI monitor or from the OT's MIDI settings, that the OT sends `9n kk 00` for note-offs? This is diagnostic only. The fix goes ahead either way.
3. **Settled:** MIDI-held rolls are cleared on every sequencer stop request;
   stopped-transport re-triggering and start handoff are implemented (§4.4).

---

## 8. Implementation Assessment (2026-09-26)

**Reviewer scope**: the uncommitted working tree on `dev-roll-midi` (on top of `43ab929`). I reviewed the four code files against `S038_STUCK_ROLL_IMPLEMENTATION.md`, rebuilt the STM32 target, independently re-ran the host harness, and checked the documentation edits.

**Verdict**: **Ready for hardware testing.** I found no defects in the change. One pre-existing timing quirk becomes more noticeable because of D4 (§8.4), and one pre-existing routing consequence is worth knowing before testing (§8.5).

### 8.1 Code vs. schedule

| Schedule item | File | Result |
|---|---|---|
| C1/C2 held-key table, `MIDI_ROLL_MAX_HELD_KEYS = 16` | `MidiParser.c` | Matches. |
| C3 find/free/union/sync/keyOn/keyOff helpers | `MidiParser.c` | Matches, with **one deliberate deviation** (below). |
| C4 `midiParser_clearMidiRollHolds()` | `MidiParser.c` | Matches. |
| C5 `isNoteOff` includes NOTE_ON velocity 0 | `MidiParser.c` | Matches. |
| C6 release first, no gate; claim only when unconsumed | `MidiParser.c` | Matches. The mask builder is untouched. |
| C7 clear on RX note-filter disable | `MidiParser.c` | Matches. |
| H1 API contract comment | `MidiParser.h` | Matches. |
| S1–S6 stopped clock, prototype, `seq_tick` hook, stop/start, re-arm | `sequencer.c` | Match. |
| SH1–SH4 header comments | `sequencer.h` | Match. |

**The deviation, which is accepted:** for a duplicate note-on of a key that is already held, `midiParser_rollKeyOn()` keeps the **original** captured mask and re-asserts it. The schedule would have ORed in the newly computed mask. The implemented behaviour is better: if routing drifts between repeats (for example an active-track change on the global chromatic route), the re-press can't grow the key's claim, and the single release still frees exactly what was originally claimed. The comment block was updated to say so. Harness test T17 below covers it.

Other code outside the plan is unchanged, as intended: `ChannelMidiParser.c`, `GlobalMidiParser.c`, `clockSync.c`, `frontPanelReceivingProtocol.c`, and the AVR side.

### 8.2 Build

- `make -C mainboard/LxrStm32 -j8 stm32`, run after touching `MidiParser.c` and `sequencer.c` to force both to recompile: **success**. The only warnings are the pre-existing `seq_init` loop-bounds, `stringop-overflow` and `seq_lastMasterStep` `memset` warnings (sequencer.c lines 253/266) and the linker RWX notice, all of which are documented in `MEMORY.md`. `MidiParser.c` compiles without warnings.
- The committed `firmware image/FIRMWARE.BIN` embeds, byte for byte, the `LxrStm32.bin` built from the current source (found at offset 57344 in the image, full 245604-byte match). **The image in the tree is the one to hardware-test.**

### 8.3 Independent host harness

The harness is not committed; it lives in the session scratchpad. It was built from **verbatim `sed` extractions** of the patched sources:

- from `MidiParser.c`: roll state and helpers (lines 91–360), clear and set-offset (403–429), the full note block of `midiParser_parseMidiMessage()` (586–796), and `midi_setFilter()` (1076–1105);
- from `sequencer.c`: roll variables (67–98), `seq_rollApplyAggregate()` through `seq_tickStoppedRolls()` (1682–1874), the stop fragment (1465–1491), the start fragment (1501–1509), and the `seq_tick` hook (1339–1341).

Stubs covered `channelMidiParser_noteOn/Off`, `seq_triggerVoice` and `systick_ticks`. It was compiled with `cc -std=c99 -Wall -Wextra -Werror`. **Result: 34/34 checks pass.**

| Test | Checks |
|---|---|
| T1–T3 | Press then `9n kk 00`, press then `8n kk vv`, duplicate press with a single release: all release. |
| T4 | Two keys on the same voice: the voice stays held until the last release. |
| T5, T17 | Global chromatic route with an active-track change mid-hold: the original voice is released and the new track is untouched. A duplicate press after drift re-asserts only the original mask. |
| T6, T7 | Unmatched note-offs are ignored. The 16-key capacity holds, the 17th key claims nothing, and all 16 release. |
| T8, T19 | Normal notes below the offset stay normal. A velocity-0 normal note goes to `channelMidiParser_noteOff` and does not roll. An override note stays normal; override + offset rolls and releases. |
| T9, T18 | Stop releases everything and later note-offs are ignored. A repeated stop while stopped clears stopped rolls (panic). |
| T10–T12 | Stopped: an immediate hit, then one hit every 8 sub-steps (500 systick units at 120 BPM). One-shot fires once per press, and a second key re-fires it. Release stops the roll. |
| T13 | Start retires the stopped clock; `seq_rollTriggered` carries the hold into the running engine. |
| T14, T15 | Manual roll is silent while stopped (unchanged). Manual and MIDI ownership never release each other. |
| T16 | RX note-filter disable releases holds. |

**The diagnosis is confirmed against the pre-fix code.** The same extraction from `43ab929` gives, for press + `9n 3C 00`: `holdCount = 2`, roll still held. After a second press/release, `holdCount = 4`. A genuine `8n` note-off only brings it to 3. This is exactly the reported "every launched roll sticks until a MIDI channel change".

### 8.4 Finding (pre-existing, low severity): double hit at the downbeat when a roll is held through start

A verbatim extraction of `seq_setRoll()` / `seq_checkRollStep()`, driven by `seq_nextStep()`'s index ordering, simulated a roll held through transport start (rotation 0, `QUANT_16`, rate 1/16). Result: **hits at per-track sub-steps 0, 1, 9, 17, …**

- `seq_setStepIndexToStart()` leaves the master index at `-1`. In `seq_setRoll()`, `seq_stepIndex[NUM_TRACKS] % seq_stepsPerQuant` evaluates to `-1` (both operands promote to `int`), and `-1 < (8/2 - 1)` is true. So on the very first step the early-roll branch fires, even though the playhead is *before* the boundary, not just after it.
- One step later the master index is 0, a boundary, so the quantized entry fires again. That gives two hits one sub-step apart (about 15.6 ms at 120 BPM), an audible flam on the first beat.
- This is **not introduced by S038**. A front-panel roll held through start behaves identically. D4 makes it more likely to be heard, because holding a MIDI roll key through start is now a supported gesture.
- (The later hits at `8k+1` are the known Session 037 per-track/master one-sub-step skew in the roll engine, and are not addressed here.)
- **Fix applied (2026-09-26, user-approved):** `seq_setRoll()`'s early-window test now reads the master index as `uint8_t`, `((uint8_t)seq_stepIndex[NUM_TRACKS]) % seq_stepsPerQuant`, so `-1` reads as `255 % 8 = 7` (late in the window, so no early hit). For in-play values 0..127 the cast is an identity, so the Session 037 early-roll humanization is unchanged. The change carries a full comment block at the call site. Verified with a verbatim `seq_setRoll()` / `seq_checkRollStep()` simulation: a roll held through start now hits at 1, 9, 17, … (it was 0, 1, 9, 17). The same result holds under `QUANT_8`. Mid-play presses still fire the early hit only 1–2 sub-steps after a boundary. The STM target was rebuilt (only the known warnings; `LxrStm32.bin` 245604 → 245628 bytes) and `FIRMWARE.BIN` repackaged, with the STM binary embedding confirmed.

### 8.5 Note for hardware testing (pre-existing design, from `9c9c67f`)

On a **chromatic** route (voice channel with note override off, or the global channel with the active track in chromatic mode), *every* incoming note at or above the roll offset is treated as a roll note. Normal chromatic notes are therefore limited to `0 .. offset-1`. With a small offset, an Octatrack track sending ordinary notes (for example C4 = 60) to a chromatic LXR channel will roll instead of playing. This is how shifted chromatic roll was designed, not an S038 regression, but it explains "notes roll unexpectedly" if it shows up. Use note overrides, or an offset above the notes in use.

### 8.6 Minor observations (no action)

- Stopped roll hits go through `seq_triggerVoice()`, so, like running rolls and the stopped voice preview, they echo a MIDI note-on whose velocity is the **step volume under the playhead**, not the roll velocity, and they parse that step's automation. This is pre-existing behaviour, already listed as a risk in the schedule (§8).
- The stopped clock falls back to silence if `seq_tempo == 0`. This is a harmless guard, since the tempo is never 0 in practice.
- The documentation edits (`MIDI_TABLE.md`, `MIDI_ROLL_TRIGGER.md`, `MIDI_ROLL_TRIGGER_IMPLEMENTATION.md`, `MEMORY.md`, this document's status and §4.4/§7) are consistent with the code as reviewed.

### 8.7 Remaining before closeout

1. **Hardware test** with the tree's `FIRMWARE.BIN`: the §5.3 list plus the stopped-transport cases in `S038_STUCK_ROLL_IMPLEMENTATION.md` §7.3.
2. ~~The user decides on §8.4.~~ Done: the unsigned early-window fix is applied (§8.4).
3. Commit. Nothing is committed yet: code, docs, `FIRMWARE.BIN`, and the untracked `knowledge_files/log_archive/038_SESSION_HANDOFF_LOG.md`.
