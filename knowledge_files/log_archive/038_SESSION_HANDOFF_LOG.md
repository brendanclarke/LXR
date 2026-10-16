# 038 Session Handoff Log — Stuck MIDI Roll: Per-Key Roll Ownership, Stop Release, and Stopped-Transport MIDI Rolls

**Date**: 2026-09-26
**Working repository**: repository root, branch `dev-roll-midi`, on top of commit `43ab929` ("roll stuck fix planning; no implementation yet"). All Session 038 code and document changes are **uncommitted**.
**Status at end**: STM32 build clean (pre-existing warnings only); `firmware image/FIRMWARE.BIN` repackaged and confirmed to embed the rebuilt STM binary; independent verbatim host harness 34/34 passing. **Not yet hardware-tested.**

---

## Session Goal

A user tested the MIDI roll-trigger firmware (the feature implemented on 2026-09-17, see *Pre-Session Baseline*) and reported this:

> Triggering notes remotely works fine but triggering roll leads to stuck roll of corresponding instrument like there is no response to note off yet. However [many] rolls I launch that's how many get stuck. You can stuck all rolls and all instrument leds would blink synchronously. Until you change midi ch on ANY instrument. After this all roll stucking on every instrument ends instantly. Also when every roll stucks brutal voice stealing takes place. Although you can tweak roll params from LXR PERF screen and it sounds pretty nice. I was sending midi from OT btw and didn't tried to tweak NRPN 93 because of that (OT can't send NRPNs).

The session had four goals:

1. Investigate and plan the fix. Output: root `S038_STUCK_MIDI_ROLL_BUG.md`.
2. After the user's decisions, write a line-cited implementation schedule. Output: root `S038_STUCK_ROLL_IMPLEMENTATION.md`.
3. Review the implementation and verify it independently, then fix what the review found.
4. Close the session: this log, the index entry, the comms spec review and `MEMORY.md`.

---

## Headline Outcome

- **Root cause found, and reproduced on the pre-fix code.** The roll feature deliberately read MIDI note statuses *literally* (Settled Decision #4 in `MIDI_ROLL_TRIGGER.md`). The Octatrack releases a key with NOTE_ON velocity 0, which is a note-off under the MIDI 1.0 standard. The roll path counted that release as a *second press*, so the per-voice hold count went from 1 to 2 and never returned to zero.
- **Fixed.** NOTE_ON velocity 0 is now a note-off everywhere. MIDI roll ownership is keyed by the MIDI key `(channel, note)`, and each release frees exactly the voices its own press claimed.
- **Two new user-requested behaviours.** Sequencer stop releases every MIDI roll. MIDI rolls also work while the sequencer is stopped, on a private tempo-locked clock, and are handed to the normal roll engine on start.
- **One pre-existing timing defect fixed after review.** A roll held through transport start produced a double hit on the downbeat. It is fixed with a one-line unsigned cast in `seq_setRoll()`.
- **Manual front-panel rolls are unchanged** in every respect.

---

## Pre-Session Baseline — The MIDI Roll-Trigger Feature (Not a Numbered Session)

The feature under test was planned and implemented on 2026-09-17, outside the numbered session sequence. This log is the first archive entry that records it.

| Commit | Content |
|---|---|
| `86dde2a` | Planning: root `MIDI_ROLL_TRIGGER.md` (design, settled decisions) and `MIDI_ROLL_TRIGGER_IMPLEMENTATION.md` (schedule) |
| `52d4ca4` | Implementation across AVR and STM (details below) |
| `9c9c67f` | "post-fix": gave shifted **chromatic** roll notes a real last-consumer lane (`midiParser_chromaticRollCandidate()`) |
| `9576ae5` | ARM Makefile updates and a `FIRMWARE.BIN` rebuild |
| `43ab929` | Session 038 planning docs only (`S038_*.md`) |

What the feature provides:

- A saved global AVR parameter, `PAR_ROLL_NOTE_OFFSET` ("rol", MIDI category, long name `RolOfset`). It is appended after `PAR_MIDI_NOTE7` so older `glo.cfg` / `.ALL` layouts stay valid. `DTYPE_0B127`, with a display exception so that raw 0 shows as `off`.
- It is sent to the STM as `SEQ_CC` / `SEQ_ROLL_NOTE_OFFSET` = `FRONT_SEQ_ROLL_NOTE_OFFSET` (`0x6f`), and stored by `midiParser_setRollNoteOffset()`.
- **Roll notes:** a roll note is a normal trigger note shifted up by the offset, 1..127 semitones.
  - With a note override, the normal route wins, and the override note + offset is the roll note.
  - On chromatic-mode tracks (no note override), every note at or above the offset is a roll note (the `9c9c67f` change), so the offset acts as a keyboard split.
- **Separate manual and MIDI ownership in the sequencer:** `seq_rollManualHeld` and `seq_rollMidiHeld` feed `seq_rollApplyAggregate()`, which drives `seq_rollTriggered`. `seq_rollChange()` is the manual source (`FRONT_SEQ_ROLL_ON_OFF`, `0x10`); `seq_rollMidiChange()` is the MIDI source.
- **Global NRPN 93** sets the roll rate (`seq_setRollRate()`, clamped to 0..15). No existing Global CC mapping changed.

---

## Part 1 — Investigation (Recorded in `S038_STUCK_MIDI_ROLL_BUG.md` §1–§3)

### Symptom mapping

| Observation | Meaning |
|---|---|
| The roll starts on the roll note | Matching, the offset maths, `seq_rollMidiChange(v,1)` and the roll engine all work |
| The roll never stops | The per-voice hold count never returns to 0 |
| Every launched roll sticks | The fault is per message, not tied to one voice, channel or mode |
| Changing any MIDI channel releases all rolls at once | `FRONT_SEQ_MIDI_CHAN` / `_CHAN_OFF` call `midiParser_clearMidiRollHolds()`, so the sequencer release path works. The fault is purely in parser hold accounting |
| Normal notes are fine | `channelMidiParser_noteOn()` already treats `vel == 0` as "no trigger", so normal notes never cared about the difference |

### Root cause

In the pre-fix code, `const uint8_t isNoteOff = (msgonly == NOTE_OFF);` ignored velocity. `midiParser_rollVoiceOn()` then incremented `midiParser_rollHoldCount[voice]`:

```
0x9n note vel>0 -> count 0 -> 1, seq_rollMidiChange(v,1)   roll starts
0x9n note vel=0 -> count 1 -> 2 ("second press")            roll continues
```

Only `midiParser_clearMidiRollHolds()` could bring the count back to 0. The original design had listed "controllers that send NOTE_ON velocity 0" as a *risk*, but underestimated it: such controllers don't merely fail to release, they add another hold.

**Why this explanation was preferred.** With genuine `0x8n` note-offs, a roll can only stick if the release takes a different routing than the press. The only inputs that can differ between press and release are `frontParser_activeTrack` (and only on the global chromatic route) or a mapping change, and every mapping change already clears all holds. The user's test changed none of these, and rolls stuck on voice-channel routes too.

**Contributing weaknesses of the per-voice counter**, present even with real `0x8n` note-offs:

1. A release recomputed its voice mask from the current routing. An active-track change mid-hold released the wrong voice.
2. A duplicate press of a key already held added a count, which then needed a matching extra release.
3. An unmatched note-off decremented some other press's hold.

### "Brutal voice stealing"

Assessed as a consequence, not a separate defect. Seven endless rolls retrigger every voice at the roll rate and cut every tail, and the two hi-hat tracks (5/6) share one synth voice, so they choke each other. The plan is to retest with one or two held rolls after the fix; no code change.

### Rejected option: an All-Notes-Off hook

There is no clean place for one. On voice channels, CC120 is already *track mute*, and Global CC123 is *Drum 2 LFO amount*.

---

## Part 2 — User Decisions

| # | Decision |
|---|---|
| D1 | **Withdraw Settled Decision #4.** NOTE_ON velocity 0 is a note-off (MIDI 1.0), on every route. |
| D2 | Key MIDI roll holds by `(channel, note)`; a release frees exactly the voices its press claimed. |
| D3 | **MIDI-held rolls release when the sequencer stops.** |
| D4 | **MIDI rolls stay re-triggerable, and audible, while the sequencer is stopped.** |
| D5 (post-review) | Fix the roll-held-through-start downbeat double hit (Part 5). |
| D6 (post-review) | Document the chromatic-mode keyboard split in `MIDI_TABLE.md`; no code change. |

---

## Part 3 — The Fix (Implemented to the `S038_STUCK_ROLL_IMPLEMENTATION.md` Schedule)

The schedule cited every change by file, line, and add/remove/modify, with verbatim documentation comment blocks (WHAT / WHY / INPUT / OUTPUT / ACCESSORS / AFFILIATES) for the `.c` and `.h` files. A separate implementation pass applied it. Current line numbers are given below.

### `mainboard/LxrStm32/src/MIDI/MidiParser.c`

- **Held-key table** (comment block at line 91). `MIDI_ROLL_MAX_HELD_KEYS = 16` slots of `MidiRollHeldKey { chan, note, voiceMask }`:
  - `chan` 1..16 means held; 0 means a free slot.
  - `midiParser_rollHeldVoiceMask` caches the union of all slot masks.
  - The table replaces `midiParser_rollHoldCount[7]`.
- **Helpers** (these replace `midiParser_rollVoiceOn/Off()` and `midiParser_applyRollVoiceMask()`):
  - `midiParser_rollFindHeldKey()` (line 197).
  - `midiParser_rollFindFreeKeySlot()` (219).
  - `midiParser_rollHeldKeysUnion()` (239).
  - `midiParser_rollSyncVoices(assertMask)` (274). Voices that leave the union get `seq_rollMidiChange(v,0)`. Voices in `assertMask` that are still held get `seq_rollMidiChange(v,1)` on *every* press. That deliberate re-assert is what re-fires a one-shot rate while running, and re-arms an immediate hit while stopped.
  - `midiParser_rollKeyOn()` (310). An empty mask or a full table means no roll; a roll that can't be recorded would have no release path. **A duplicate press keeps the key's originally captured mask** and only re-asserts it. This is a deliberate improvement over the schedule, which ORed in the new mask; the implementation is safer under routing drift.
  - `midiParser_rollKeyOff()` (350). An unknown key is ignored.
- **`midiParser_clearMidiRollHolds()`** (403) empties the table, then syncs with an assert mask of 0.
- **Note-off classification** (comment at 591): `isNoteOff = (msgonly == NOTE_OFF) || (msgonly == NOTE_ON && msg.data2 == 0)`. For normal notes this changes nothing observable, because `channelMidiParser_noteOff()` forces `vel = 0` and delegates to `channelMidiParser_noteOn()`.
- **Roll path** (comment at 699):
  - `if(isNoteOff) midiParser_rollKeyOff(chanonly, note);` — the release is **ungated**: no `normalConsumed` check, no offset check, no routing.
  - `else if(!normalConsumed && midiParser_rollOffsetEnabled())` — the unchanged mask builder, then `midiParser_rollKeyOn()` (792).
- **`midi_setFilter()`** (1076, comment at 1083): turning off the RX note filter bit clears all holds, because the note block would otherwise never see the release.

### `mainboard/LxrStm32/src/MIDI/MidiParser.h`

- Line 100: the API contract comment now describes the hold model and every accessor of `midiParser_clearMidiRollHolds()`.

### `mainboard/LxrStm32/src/Sequencer/sequencer.c`

- **Stopped-clock state** (comment at line 77): `seq_rollStoppedActive`, `seq_rollStoppedCounter[NUM_TRACKS]`, `seq_rollStoppedLastTick` and `seq_rollStoppedPhase` (float). They are separate from `seq_rollState` / `seq_rollCounter` / `seq_deltaT` / `seq_lastTick`, so the running engine, start alignment and external sync can't be disturbed.
- **Static prototype** `seq_tickStoppedRolls()` (212).
- **`seq_tick()`** (1328, comment at 1330): `if(!seq_running) seq_tickStoppedRolls();`
- **`seq_setRunning()`** (1436):
  - Stop branch (comment at 1465): `midiParser_clearMidiRollHolds(); seq_rollStoppedActive = 0; seq_rollState &= seq_rollTriggered; seq_rollPlayedEarly &= seq_rollTriggered;`. This runs on *every* stop request, so a stop is also a MIDI-roll panic.
  - Start branch (comment at 1501): `seq_rollStoppedActive = 0;`. A held MIDI roll keeps `seq_rollTriggered`, and the running engine takes it over through the quantized `seq_setRoll(v,1)` entry.
- **`seq_rollMidiChange()`** (1734): a press while stopped clears the voice's `seq_rollStoppedActive` bit, which re-arms an immediate hit.
- **`seq_tickStoppedRolls()`** (1794):
  - It follows `seq_rollMidiHeld` only; manual rolls stay silent while stopped.
  - A newly armed voice fires at once through `seq_triggerVoice(v, seq_rollVelocity, seq_rollNote)`, the same call as the stopped voice preview (`FRONT_SEQ_SET_ACTIVE_TRACK`).
  - It then decrements a per-voice counter once per sub-step and fires and reloads from `seq_tempRate` at 0.
  - Sub-step length is `((60000/seq_tempo)/96*4) * SEQ_PRESCALER_MASK` systick units, identical to `seq_calcDeltaT()`'s convention without shuffle. That is 62.5 units per sub-step at 120 BPM, so rate 8 repeats every 500 units.
  - A one-shot rate (`0xff`) parks after the first hit, and resumes if the rate later becomes repeating.
  - After a stall the clock drops the backlog instead of bursting.
  - `seq_rollMode` is deliberately ignored while stopped: TRIG/NOTE/VEL/BOTH read a frozen playhead step.
  - `seq_tempRate` is used because the quantized `seq_tempRate -> seq_rollRate` latch does not run while stopped.
  - `seq_tempo` keeps tracking the external clock while stopped (`sync_tick()` → `seq_setBpm()`), so stopped rolls follow external tempo.

### `mainboard/LxrStm32/src/Sequencer/sequencer.h`

- Roll-state comment, `seq_tick()` comment, `seq_setRunning()` comment and `seq_rollMidiChange()` comment all updated to describe the Session 038 behaviour.

### Unchanged (checked)

- `ChannelMidiParser.c`, `GlobalMidiParser.c` (NRPN 93; the MTC-timeout stop inherits D3), `clockSync.c` (MIDI Start/Stop inherits D3/D4), and `frontPanelReceivingProtocol.c`. The existing clear points stay in place: channel change and channel off, `FRONT_SEQ_TRACK_NOTE1..7`, and `FRONT_SEQ_ROLL_NOTE_OFFSET`.
- The entire AVR side.

---

## Part 4 — Implementation Review (Recorded in `S038_STUCK_MIDI_ROLL_BUG.md` §8)

- **Code vs schedule:** every item C1–C7, H1, S1–S6 and SH1–SH4 matches, except the accepted duplicate-mask deviation described above.
- **Build:** `make -C mainboard/LxrStm32 -j8 stm32`, with both changed sources touched to force a recompile, passed. The only warnings are the known ones: the `seq_init` loop-bounds / `stringop-overflow` / `seq_lastMasterStep` `memset` warnings at `sequencer.c` 253/266, and the linker RWX notice. `MidiParser.c` compiles without warnings.
- **Independent verbatim harness** (session scratchpad, not committed):
  - Built from `sed` extractions of the patched `MidiParser.c` (roll state and helpers, clear/offset, the full note block, `midi_setFilter()`) and `sequencer.c` (roll variables, `seq_rollApplyAggregate()` through `seq_tickStoppedRolls()`, the stop/start fragments, the `seq_tick` hook).
  - Stubs for `channelMidiParser_noteOn/Off`, `seq_triggerVoice` and `systick_ticks`.
  - Compiled with `cc -std=c99 -Wall -Wextra -Werror`.
  - **34/34 checks passed.** Coverage:
    - both release styles and duplicate presses;
    - shared voices and routing drift, including duplicate-after-drift (T17);
    - unmatched note-offs and 16-key capacity;
    - normal notes below the offset, a velocity-0 normal note routed to `noteOff`, and the override route;
    - stop and a repeated stop while stopped;
    - the stopped clock: immediate hit, 500-unit cadence, one-shot re-fire, release;
    - start handover;
    - manual silence while stopped, and manual and MIDI not releasing each other;
    - RX filter clear.
- **Pre-fix reproduction:** the same extraction from `43ab929` gives `holdCount = 2` after press + `9n 3C 00`, 4 after a second press/release, and 3 after a genuine `8n`. This matches the report exactly.
- **Firmware image:** `FIRMWARE.BIN` embedded `LxrStm32.bin` byte for byte (at offset 57344).

**Note on the implementation pass's own harness:** it was a focused model rather than verbatim extractions, and it lived at `/private/tmp/s038_roll_harness.c`, which is not persistent. The independent verbatim harness above is the verification of record.

---

## Part 5 — Post-Review Fix: Double Hit at the Downbeat When a Roll Is Held Through Start

**Mechanism.**

1. `seq_setStepIndexToStart()` (line 2596) sets `seq_stepIndex[NUM_TRACKS] = 8*rot - 1`, which is `-1` for rotation 0.
2. In `seq_setRoll()`, `seq_stepIndex[NUM_TRACKS] % seq_stepsPerQuant` promotes both operands to `int` and yields `-1`. `-1 < (seq_stepsPerQuant/2 - 1)` is true.
3. So on the very first step after start, the "just after a boundary" early-roll branch fired, although the playhead was *before* the boundary.
4. One step later the master index is 0 (a boundary) and the quantized entry fired again, one sub-step later: about 15.6 ms at 120 BPM.

**Measured.** A verbatim `seq_setRoll()` / `seq_checkRollStep()` simulation driven by `seq_nextStep()`'s index ordering gave hits at **0, 1, 9, 17**.

**Scope.** Pre-existing: a front-panel roll held through start behaved the same way. D4 made it far more likely to be heard. The same `-1` also occurs when a pattern change resets the playhead (`seq_setStepIndexToStart()` is also called at `sequencer.c` line ~964).

**Fix.** At line 1935 (comment block at 1917):

```c
if ( (((uint8_t)seq_stepIndex[NUM_TRACKS])%seq_stepsPerQuant)<(seq_stepsPerQuant/2 - 1) )
```

`-1` now reads as 255, and `255 % 8 = 7`: late in the window, so no early hit. For every in-play value 0..127 the cast changes nothing, so the Session 037-protected early-roll humanization is untouched. Only the early-window test changed; the boundary test `!(idx % q)` was already correct for `-1`.

**Verified.**

| Case | Result |
|---|---|
| Roll held through start | hits at **1, 9, 17, 25, 33** |
| Same under `QUANT_8` | same result |
| Mid-play presses | still fire the early hit only 1–2 sub-steps after a boundary (masters 17 and 18); later presses wait for the boundary |

**Rebuild.** `make -C mainboard/LxrStm32 -j8 stm32`: `LxrStm32.bin` went from 245604 to 245628 bytes, with only the known warnings. Then `make firmware`; embedding confirmed.

**Build-system lesson.** A plain `make firmware` right after the source edit **only repackaged the old STM binary**. The top-level `Makefile` gives `$(ARM_BINARY)` / `$(AVR_BINARY)` no source prerequisites, so they are rebuilt only if missing. Always build the sub-target first.

**Not addressed** (known, pre-existing): all quantized roll hits land at per-track sub-step `8k+1`. This is the Session 037 per-track/master one-sub-step skew in the roll engine as a whole, roughly one sub-step late relative to the grid. Leave it unless it is raised on hardware.

---

## Part 6 — Documentation Changes

- **`knowledge_files/comms_spec_reference/MIDI_TABLE.md`:**
  - Roll section rewritten:
    - NOTE_ON velocity 0 is a note-off on every route.
    - Per-key hold semantics and the 16-key limit.
    - The full list of clear points.
    - Stopped-transport behaviour and start handover.
  - At closeout, a sentence carried over from the roll feature was corrected. It had claimed that roll notes are always evaluated after the normal routes. That is only true with a note override. On chromatic-mode tracks the offset is a keyboard split, and nothing rolls while the offset is `off`.
  - Closeout also updated the header date and status, the ownership line, the note table, and the MIDI Stop row.
- **`knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`** (closeout): status line, MIDI Boundary roll-ownership paragraph, `SEQ_ROLL_NOTE_OFFSET` (`0x6f`) in the live-control family, guardrails.
- **`MIDI_ROLL_TRIGGER.md` / `MIDI_ROLL_TRIGGER_IMPLEMENTATION.md`:** the literal-status rule and the per-voice-counter design are marked withdrawn or superseded, with pointers to the S038 docs. These are historical design records and were not rewritten.
- **`S038_STUCK_MIDI_ROLL_BUG.md`:** status, settled decisions, and a full §8 implementation assessment including the §8.4 fix record.
- **`S038_STUCK_ROLL_IMPLEMENTATION.md`:** the implementation pass's work log plus a post-review entry.
- **`MEMORY.md`:** Session 038 status, reminders, and the look-up table.
- **`knowledge_files/log_archive/000_SESSION_INDEX.md`:** the 038 row, summary and cross-session facts. The implementation pass had added an index row and a draft 038 log; the user had them removed, and both were rewritten at closeout. This file is the rewrite.

---

## Completed Changes

| File | Change |
|---|---|
| `mainboard/LxrStm32/src/MIDI/MidiParser.c` | Held-key table replaces the per-voice counters; NOTE_ON velocity 0 is a note-off; ungated release-first roll path; new find/free/union/sync/keyOn/keyOff helpers; `midiParser_clearMidiRollHolds()` rewritten; RX note-filter clear in `midi_setFilter()`; full comment blocks throughout |
| `mainboard/LxrStm32/src/MIDI/MidiParser.h` | Roll API contract comment |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | Stopped-clock state and prototype; `seq_tick()` hook; stop clear and start handover in `seq_setRunning()`; stopped re-arm in `seq_rollMidiChange()`; new `seq_tickStoppedRolls()`; unsigned early-window cast in `seq_setRoll()` |
| `mainboard/LxrStm32/src/Sequencer/sequencer.h` | Four comment updates |
| `firmware image/FIRMWARE.BIN` | Rebuilt (embeds the 245628-byte STM binary) |
| `knowledge_files/comms_spec_reference/MIDI_TABLE.md` | Roll semantics, chromatic split, header/ownership/notes/stop rows |
| `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md` | Session 038 status, MIDI roll ownership, `0x6f`, guardrails |
| `MIDI_ROLL_TRIGGER.md`, `MIDI_ROLL_TRIGGER_IMPLEMENTATION.md` | Superseded markers |
| `S038_STUCK_MIDI_ROLL_BUG.md`, `S038_STUCK_ROLL_IMPLEMENTATION.md` | Session working documents (plan and assessment; schedule and work log) |
| `MEMORY.md` | Session 038 context |
| `knowledge_files/log_archive/000_SESSION_INDEX.md`, this log | Archive |

---

## Known Issues Introduced

None found. Deliberate behaviour changes the user should know about:

- **A stop is a MIDI-roll panic.** Every stop request, including one received while already stopped (for example a front-panel stop press, or an MTC timeout), clears all MIDI rolls, including rolls started during the stop.
- **Stopped MIDI rolls ignore `seq_rollMode`.** They always use roll velocity and roll note.
- **Stopped hits behave like the existing stopped voice preview.** They go through `seq_triggerVoice()`: they parse the automation of the frozen playhead step and echo a MIDI note-on whose velocity is that step's volume. Running rolls do the same.
- **A velocity-0 note-on can no longer start a roll.** That was never musically meaningful.

## Known Issues Resolved

- **The reported bug:** stuck MIDI rolls from senders that release with NOTE_ON velocity 0 (Octatrack).
- **Per-voice hold fragility:** routing-drift mis-release, duplicate-press double counting, and unmatched-release decrements.
- **Downbeat double hit** for any roll (manual or MIDI) held through transport start or a playhead-reset pattern change.
- **`MIDI_TABLE.md`'s inaccurate claim** about last-consumer evaluation on chromatic-mode tracks.

---

## Open Items / Next Decisions

1. **Hardware test (blocking).** Use the tree's `FIRMWARE.BIN` and work through `S038_STUCK_MIDI_ROLL_BUG.md` §5.3, `S038_STUCK_ROLL_IMPLEMENTATION.md` §7.3, and "a roll held through Play gives a single first hit". Test with the Octatrack first, then with Renoise or a DAW (real `0x8n` releases, NRPN 93).
2. **Confirm what the Octatrack actually sends** (diagnostic only): does it release with `9n kk 00`?
3. **"Voice stealing"**: retest with one or two held rolls. Open an investigation only if it persists.
4. **Optional, only if requested:**
   - Extend the stopped clock to manual front-panel rolls (source mask `seq_rollManualHeld | seq_rollMidiHeld`).
   - Add a stopped-trigger variant that skips `seq_parseAutomationNodes()` if a parameter seems to stick after stopped rolls.
5. **Pre-existing, not addressed:** the roll engine's `8k+1` hit placement (Session 037 skew).
6. **Commit.** Nothing is committed yet. Root `S038_*.md` can be deleted once the user is satisfied that this log preserves them; `MIDI_ROLL_TRIGGER*.md` are historical and can be kept or pruned.
7. **Carried over, still open:**
   - Front-panel row-0 sub-step addressing vs sub-step rotation (Session 036).
   - The `LengthRotate` 11-bit bitfield-in-`uint8_t`-union latent defect (Session 037).

---

## Repository State at Session End

- Branch `dev-roll-midi`, HEAD `43ab929`.
- **Modified (uncommitted):**
  - `mainboard/LxrStm32/src/MIDI/MidiParser.c` / `.h`
  - `mainboard/LxrStm32/src/Sequencer/sequencer.c` / `.h`
  - `firmware image/FIRMWARE.BIN`
  - `knowledge_files/comms_spec_reference/MIDI_TABLE.md`, `COMMS_FLOW_SPEC.md`
  - `knowledge_files/log_archive/000_SESSION_INDEX.md`
  - `MEMORY.md`, `MIDI_ROLL_TRIGGER.md`, `MIDI_ROLL_TRIGGER_IMPLEMENTATION.md`
  - `S038_STUCK_MIDI_ROLL_BUG.md`, `S038_STUCK_ROLL_IMPLEMENTATION.md`
- **New (untracked):** this log.
- The assistant performed no git mutation: nothing was committed, staged or pushed.

---

## End Of Session Block

```
DATE: 2026-09-26
SESSION GOAL: Investigate and fix the user-reported stuck MIDI roll (Octatrack): every
              MIDI-triggered roll stuck until any MIDI channel was changed. Plan, schedule,
              review the implementation, fix review findings, close out.

COMPLETED:
- Root cause: the roll feature read note statuses literally; NOTE_ON velocity 0 (the
  Octatrack's release) was counted as a second press by per-voice hold counters, so the
  count never returned to 0. Reproduced on the pre-fix code with a verbatim harness.
- User withdrew Settled Decision #4: NOTE_ON velocity 0 is a note-off on every route.
- MIDI roll ownership is now a 16-slot (channel, note) held-key table; a release frees
  exactly the captured voice mask; duplicates idempotent; unmatched releases ignored;
  release lookup ungated; RX note-filter disable added as a clear point.
- Sequencer stop clears all MIDI roll holds (every stop request).
- MIDI rolls play while stopped on a private tempo-locked clock (seq_tickStoppedRolls()),
  and are handed to the normal quantized engine on start. Manual rolls unchanged.
- Post-review fix: seq_setRoll() early-roll window uses the unsigned master index, so a
  roll held through start no longer double-hits the downbeat (0,1,9,17 -> 1,9,17).
- MIDI_TABLE.md / COMMS_FLOW_SPEC.md / MEMORY.md / index updated; chromatic-mode
  keyboard split documented.

VERIFIED ON HARDWARE: NO. Build-verified (STM32 pre-existing warnings only; FIRMWARE.BIN
                      embeds the rebuilt 245628-byte STM binary) and host-verified
                      (independent verbatim harness 34/34; start-flam simulation fixed).

CHANGES THIS SESSION:
- mainboard/LxrStm32/src/MIDI/MidiParser.c: held-key table, NOTE_ON vel 0 = note-off,
  release-first ungated roll path, new helpers, clear rewrite, RX filter clear.
- mainboard/LxrStm32/src/MIDI/MidiParser.h: roll API contract comment.
- mainboard/LxrStm32/src/Sequencer/sequencer.c: stopped roll clock + state, seq_tick hook,
  stop clear / start handover, stopped re-arm, unsigned early-window cast in seq_setRoll().
- mainboard/LxrStm32/src/Sequencer/sequencer.h: comment updates.
- firmware image/FIRMWARE.BIN: rebuilt.
- knowledge_files/comms_spec_reference/MIDI_TABLE.md, COMMS_FLOW_SPEC.md: Session 038 rules.
- MIDI_ROLL_TRIGGER.md, MIDI_ROLL_TRIGGER_IMPLEMENTATION.md: superseded markers.
- S038_STUCK_MIDI_ROLL_BUG.md, S038_STUCK_ROLL_IMPLEMENTATION.md: session working docs.
- MEMORY.md, knowledge_files/log_archive/000_SESSION_INDEX.md, this log.

KNOWN ISSUES INTRODUCED: none found. Deliberate: stop = MIDI-roll panic; stopped rolls
                         ignore roll mode; stopped hits share the stopped-preview
                         automation/echo behaviour.
KNOWN ISSUES RESOLVED: stuck MIDI rolls from NOTE_ON-vel-0 senders; per-voice hold
                       fragility (routing drift, duplicates, unmatched releases);
                       downbeat double hit for rolls held through start.

NEXT SESSION RECOMMENDED GOAL: hardware-verify S038 (Octatrack + a 0x8n sender + NRPN 93,
    stopped rolls, stop/start handover, single first hit), then commit.

BLOCKERS: hardware test by the user.

CRITICAL REMINDERS FOR NEXT SESSION:
- NOTE_ON velocity 0 is a note-off (MIDI 1.0). Never count it as a press, never reinstate
  literal-status handling.
- MIDI roll release must free the voice mask captured at press, never a mask recomputed
  from current routing; the note-off lookup must stay ungated.
- Clearing MIDI roll holds must clear the PARSER table (midiParser_clearMidiRollHolds()),
  not just seq_rollMidiHeld; a stale entry makes the next press look like a duplicate.
- seq_setStepIndexToStart() leaves the master index at -1; signed % on it is -1. Keep the
  uint8_t cast in seq_setRoll()'s early-window test.
- `make firmware` does not rebuild changed sources. Build the stm32/avr targets first,
  then package; confirm the STM binary is embedded in FIRMWARE.BIN.
- Do not re-suppress the early-roll hit's recording (Session 037 rule still stands).
- With the offset on, chromatic-mode tracks split at the roll offset; nothing rolls when
  the offset is off.
```
