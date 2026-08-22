# 037 Session Handoff Log — Live-Record Roll Duplicate Sub-Step: Simulation-Driven Root Cause and Fix

**Date**: 2026-08-22
**Working repository**: repository root, branch `dev-prb-fix`
**Status at end**: STM32 + AVR + aggregate firmware builds clean; **the live-record roll fix is hardware-verified by the user**.

---

## Session Goal

Session 036 closed with a fix for a user-reported live-record bug (roll buttons entering two sub-steps instead of one). The user tested it and reported it **did not work** — both `roll=one` and `roll=16` still produced sub-steps 0 and 1 at the start of a recorded main step.

The goal for this session was therefore: find the *true* source, make only the precise targeted fix, and document it. A secondary goal, added at the end, was the full session closeout, including **preserving the durable technical content of the two root working documents** (`PROBABILITY_INVESTIGATION.md`, `LIVE_REC_DUPLICATE_SUBSTEP_BUG.md`) into this archive, because the user intends to delete both.

---

## Headline Outcome

The Session 036 fix was **wrong on both counts** and has been **fully reverted**. It did not fix the reported bug, and it introduced a regression that silently dropped recorded notes.

The real cause was found and fixed. The user has confirmed on hardware that recording rolls with both `roll=one` and `roll=16` now enters **only one sub-step**.

---

## Method Note — What Actually Broke The Deadlock

Two prior rounds of static analysis (Session 036, and the first half of this session) both accepted a plausible-sounding mechanism that turned out to be impossible. Careful re-reading kept confirming the same wrong conclusion.

What resolved it was **building a host-side simulation of the real code**. `seq_quantize()`, `seq_addNote()`, `seq_rollTrig()`, `seq_setRoll()` and `seq_checkRollStep()` were copied **verbatim** (not paraphrased) from `sequencer.c` into a standalone C harness with minimal stubs, driven by a driver replicating `seq_process()`'s exact per-tick ordering — including the critical detail that the per-track step index is advanced at the top of the per-track loop while the master index is advanced at the very end.

That harness could then be swept exhaustively across roll mode × roll rate × quantize setting × track scale × press timing × hold length. It produced a definitive negative result in one pass where reading had produced two confident wrong answers.

**This technique is worth reusing** for any future sequencer-timing dispute. Copying the functions verbatim is the essential part — a paraphrase would only have re-encoded the same misreading.

---

## Part 1 — Disproving The Session 036 Fix

Session 036 blamed the "early roll" preview hit in `seq_setRoll()` and added an `allowRecord` parameter to `seq_rollTrig()` to suppress that hit's recording.

### It could not have fixed the reported bug

Sweeping the harness for the reported symptom (sub-step 0 **and** sub-step 1 both active in the same main step) across every configuration:

```
total configs producing ss0+ss1: 1070
of those, with the user's stated settings (quantize 1/16, track scale off): 0
```

Every one of the 1070 hits required `quant=OFF`. **With quantization on, the roll path cannot write sub-step 1 at all**, because `seq_quantize()` under `QUANT_16` with track scale 0 always returns a multiple of 8. The early-roll theory, when it did misbehave, produced two adjacent *main* steps — never two sub-steps inside one main step, which is what was actually reported.

### It introduced a regression

The early hit's record was the **correctly quantized** one. `seq_addNote()` snaps it back to the boundary that just passed — the position the player intended. The later on-time hit lands a full 16th note **later**. Suppressing the early record therefore silently dropped notes for any tap that landed 1-2 sub-steps late and was released before the next boundary:

| tap lands at | before the bad fix | after the bad fix |
|---|---|---|
| sub-step 0 | main 4 ss0 | main 4 ss0 |
| sub-step 1 | main 4 ss0 | main 4 ss0 |
| **sub-step 2** | **main 4 ss0** | **nothing recorded** |
| **sub-step 3** | **main 4 ss0** | **nothing recorded** |

The early-roll branch is a deliberate humanization feature (fire immediately so the performer hears the hit now rather than waiting out the quantize window; `seq_rollPlayedEarly` blocks repeats until release) and its recording behaviour was correct all along.

**Action taken**: `seq_rollTrig()`'s `allowRecord` parameter removed entirely; the function is back to `seq_rollTrig(uint8_t voice)`. The early-roll call site carries a new comment explaining why it *does* record and why suppressing it loses notes, so this is not "fixed" again by a future session.

---

## Part 2 — The Confirmed Root Cause

### `seq_addNote()` inferred "MIDI note-off" from velocity alone

`seq_addNote()` has always chosen between two different destination slots purely on velocity:

```c
if (vel==0)                                                   /* pre-fix */
   stepPtr = pat_getStepPtr(targetPattern, trackNr, unquantizedStep);
else
   stepPtr = pat_getStepPtr(targetPattern, trackNr, quantizedStep);
```

The un-quantized placement exists **for MIDI note-offs**. A note-off carries velocity 0 by definition — `channelMidiParser_noteOff()` forces `vel = 0` before delegating to `channelMidiParser_noteOn()` (`MIDI/ChannelMidiParser.c`) — and marking where a held note was released is only meaningful at its true position. The resulting zero-velocity "ghost" step is an **intentional** concept in this sequencer (see the `STEP_VOLUME_MASK > 0` guard in `seq_process()`'s automation-release block, which explicitly documents ghost steps as ratchet placeholders / muted accents / silent control events).

### Velocity 0 reaches `seq_addNote()` from paths that are not note-offs

`seq_rollTrig()` resolves its record velocity from one of two places:

- `ROLL_MODE_VELOCITY` / `_BOTH` / `_ALL` → `vol = seq_rollVelocity` — the **roll velocity menu parameter, which can be dialled to 0**.
- `ROLL_MODE_TRIG` / `_NOTE` → `vol = stepData->volume & 0x7f` — **the stored volume of the step under the playhead, which is 0 for any zero-velocity ghost step**.

Either way an ordinary roll hit can carry velocity 0, and was then misread as a note-off and written raw.

### Why the stray write is always exactly sub-step 1

The raw index it falls back to is not arbitrary. In `seq_process()`:

- `seq_stepIndex[i]` is incremented at the **top** of the per-track loop;
- `seq_stepIndex[NUM_TRACKS]` — the master index whose `% seq_stepsPerQuant` test decides "we are on a quantize boundary" in `seq_setRoll()` — is not incremented until the **very end** of `seq_process()`.

So the per-track index runs exactly **one sub-step ahead** of the master index. At the moment the quantize test says "boundary", `seq_stepIndex[track]` is already `mainStep*8 + 1` — **sub-step 1**. `seq_quantize()` normally folds that back to `mainStep*8`; the note-off branch bypasses `seq_quantize()` entirely.

`pat_setMainStep()` then switches the main step on, which un-masks **sub-step 0** — active by default in every cleared main step (`pat_clearTrack()` sets `volume |= STEP_ACTIVE_MASK` for every `k % 8 == 0`). Result: **sub-steps 0 and 1 both fire at the start of the main step**, which is exactly the reported symptom.

### It is self-sustaining

The step written this way holds `0 | STEP_ACTIVE_MASK`. On the next pass `volume & 0x7f` reads 0 again, so the same wrong branch is taken. This matches the "reoccurring" character of the report and means a pattern, once damaged, stays damaged.

### Simulation evidence

Quantize 1/16, track scale off, sub-steps seeded with volume 0:

```
 roll mode TRIG:
   press at ss0 ->  main4:[ss1 ]  main5:[ss1 ]     <-- off-grid write
   press at ss1 ->  main4:[ss1 ]  main5:[ss1 ]
 roll mode ALL (roll velocity non-zero):
   press at ss0 ->  main4:[ss0 ]  main5:[ss0 ]     <-- correct
```

Sub-step 1 appears exactly when the resolved velocity is 0, and only then.

### Corroboration from the user's own P075 file

`pat_example.txt` (the user's decode of `P075.ALL`, track 4 / pattern 0) shows all 16 recorded notes at raw indices **1, 9, 17, 25 … 121** — every one at `8k+1` — with sub-step 0 explicitly *cleared*, and `vol=100`, `prob=127`, `note=63`.

That combination is the unmistakable signature of `seq_addNote()`: it is the only code in the firmware that writes `prob = 127` alongside volume and note, and the only code that conditionally clears sub-step 0 (`if(!pat_isMainStepActive(...)) → clear ss0`). A real pattern on real hardware had already landed every single note one sub-step off the grid.

**This is also the missing half of the original Session 036 probability bug.** The probability values the user set landed at raw 40/64/72/88 (`8k` — where the front panel's step editor addresses), while the audible notes sit at `8k+1`. That is precisely why "step 12 shows PRB 25 but this does not do anything": the editor and the live note were addressing different sub-steps. Session 036 attributed the `8k+1` offset to sub-step *rotation*; this session shows live recording can produce the identical offset on its own.

---

## Part 3 — The Fix

Make the un-quantized placement **explicit** rather than inferred.

### `mainboard/LxrStm32/src/Sequencer/sequencer.c`

`seq_addNote()` gained an `isNoteOff` parameter:

```c
void seq_addNote(uint8_t trackNr, uint8_t vel, uint8_t note, uint8_t isNoteOff)
...
   /* A real MIDI note-off keeps its true, un-quantized position; every
      other event is placed on the quantize grid even at velocity 0. */
   if (vel==0 && isNoteOff)
      stepPtr = pat_getStepPtr(targetPattern, trackNr, unquantizedStep);
   else
      stepPtr = pat_getStepPtr(targetPattern, trackNr, quantizedStep);
```

A ~70-line comment block above the function documents WHY it exists, THE BUG, the one-sub-step index skew, WHAT THE CHANGE DOES, INPUT, OUTPUT, and AFFILIATES, per the project's standing requirement that changes carry adjacent explanatory comments.

`seq_rollTrig()` reverted to `seq_rollTrig(uint8_t voice)`; all five `ROLL_MODE_*` branches restored to `if(seq_recordActive)`. The early-roll call site in `seq_setRoll()` carries a new comment explaining why that hit legitimately records.

### Call sites

| caller | passes | reason |
|---|---|---|
| `channelMidiParser_noteOn()` / `noteOff()` (`MIDI/ChannelMidiParser.c`) | `1` | the only path where `vel == 0` really means "note released" — MIDI behaviour unchanged |
| `seq_rollTrig()`, all five `ROLL_MODE_*` branches (`sequencer.c`) | `0` | a roll hit is always a real trigger, never a note-off |
| `seq_process()` loop re-record (`sequencer.c`) | `0` | replaying an existing step, never a note-off |

### `mainboard/LxrStm32/src/Sequencer/sequencer.h`

Declarations updated for both functions, with the `isNoteOff` contract documented and a pointer to the full rationale in `sequencer.c`.

### Net behavioural change

A zero-velocity roll now overwrites sub-step 0 **on the grid** (silent, as intended) instead of creating a second, off-grid sub-step beside it. Nothing about MIDI note-on/note-off recording changes.

---

## Verification

### Simulation
- The bug condition now records **ss0 only**, in every roll mode, at every press offset.
- The dropped-note regression from the reverted Session 036 fix is gone — taps at sub-steps 2 and 3 record correctly again.

### Build
- `make -C mainboard/LxrStm32 clean` then `make -C mainboard/LxrStm32 -j4 stm32` — succeeded. The only warnings touching the changed files are the **pre-existing** `seq_init` loop-bounds / `memset` / `seq_lastMasterStep` array-bounds warnings already documented in `MEMORY.md`. No new warnings, no errors.
- `make firmware` — succeeded end to end; `firmware image/FIRMWARE.BIN` rebuilt.

### Hardware
**VERIFIED BY USER.** Quote: *"I tested recording rolls with roll=one and with 16ths, only one substep was recorded this time."*

---

## Completed Changes

| File | Change |
|---|---|
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | Reverted `seq_rollTrig()`'s `allowRecord` parameter and all five gates; added `isNoteOff` parameter to `seq_addNote()` and changed `if (vel==0)` to `if (vel==0 && isNoteOff)`; routed all six internal call sites with `0`; added the large rationale comment block above `seq_addNote()` and a new explanatory comment at the early-roll call site |
| `mainboard/LxrStm32/src/Sequencer/sequencer.h` | Declarations for `seq_rollTrig()` (reverted) and `seq_addNote()` (new parameter), both with contract comments |
| `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c` | `seq_addNote(voice, vel, note, 1)` with a comment stating this is the sole genuine note-off path |
| `firmware image/FIRMWARE.BIN` | Rebuilt |
| `LIVE_REC_DUPLICATE_SUBSTEP_BUG.md` (root) | Rewritten: correction banner on the reverted fix, confirmed root cause, simulation evidence, fix, verification, open items. **User intends to delete — content preserved in this log.** |
| `knowledge_files/log_archive/000_SESSION_INDEX.md` | Terse 037 row, 036 + 037 summaries, six new cross-session facts |
| `knowledge_files/log_archive/037_SESSION_HANDOFF_LOG.md` | This file |

### User-authored, not modified by this session
- `tools/rotation_correction.py` — see the dedicated section below.

---

## `tools/rotation_correction.py` (user-authored — noted for discoverability)

The user independently wrote this utility during the session. **It was not created or modified by the assistant and is fully self-documented in its own module docstring**; this entry exists only so its existence is discoverable from the logs.

**What it is**: a standalone Python 3 script that cyclically rotates sequencer pattern data *inside saved LXR SD-card files*, in place.

**Why it matters**: the firmware fix in this session stops *new* off-by-one-sub-step recordings, but **does not repair data already stored in saved songs**. This tool is the repair path for existing files — e.g. shifting a track's notes from sub-step 1 back to sub-step 0 so the front panel's row-0 step editor addresses the note that actually plays.

**Usage summary** (full detail in the file):

```
python3 rotation_correction.py <file> step=<+/-N> [substep=<+/-N>] [filters]
```

- Handles `.pat`, `.prf`, and `.all`, selected by extension; offsets are verified against `presetManager.c`.
- `step=N` rotates by whole main steps (shifts 128 sub-steps by `N*8` **and** rotates the 16-bit main-step gate mask by `N`).
- `substep=N` rotates by sub-steps only and **leaves the gate mask unchanged** — matching the firmware's own sub-step-rotation semantics.
- Optional filters `pattern=`, `track=`, `data=` restrict the operation; `data=` selects among `active, volume, probability, note, target1, amount1, target2, amount2`, treating `active` (bit 7) and `volume` (bits 0-6) as independent columns of the shared volume byte.
- Nothing is added, deleted or truncated — values wrap. Kit/morph bytes, the name, shuffle, per-track length/scale and the pattern chain are left byte-for-byte intact.
- **Modifies the file in place — back up first.**

---

## Known Issues Introduced

None identified. The change is strictly narrowing: the only behaviour that differs is that a *non-note-off* event carrying velocity 0 is now placed on the quantize grid instead of at the raw playhead index.

---

## Known Issues Resolved

- **Live-record roll duplicate sub-step** — resolved and hardware-verified.
- **Session 036's dropped-note regression** — resolved by reverting that fix (taps landing 2-3 sub-steps after a boundary record correctly again).

---

## Open Items / Next Decisions

1. **Existing damaged songs are not repaired by the firmware fix.** Ghost steps already stored at `8k+1` are data. Either clear and re-record those main steps, or use `tools/rotation_correction.py` to shift them back. Worth confirming which patterns are affected before editing.

2. **Which zero-velocity source applied on the user's unit is still unconfirmed.** The fix closes all of them, so this is diagnostic curiosity rather than a blocker — but checking whether `PAR_ROLL_VELOCITY` is at 0 would confirm the diagnosis outright.

3. **Pre-existing, deliberately not touched**: a roll tap landing in the *second half* of a quantize window and released before the next boundary records **nothing at all**. This predates every change here and is a separate trade-off in `seq_setRoll()`'s early-window condition `< (seq_stepsPerQuant/2 - 1)`. Flagged only; changing it is a design decision about roll feel, not a bug fix.

4. **Separate latent defect found while reading, not touched**: `LengthRotate` (`Sequencer/Pattern/PatternData.h`) declares `length:4 + scale:3 + rotate:4` = **11 bits** inside a union with a `uint8_t value`. `sizeof(LengthRotate)` is therefore **4, not 1**, and `value` aliases only the low byte — `rotate`'s upper 3 bits live outside it entirely. Any code treating `value` as the whole packed state silently loses rotation >= 2. Verified that `scale` is *not* corrupted by rotation writes (rotation is written through the bitfield, not through `value`), so this did not cause any bug investigated here — but it deserves its own look, particularly around file save/load.

5. **`seq_skipFirstRoll`** (`sequencer.c`) is permanently 0 — its only setter is commented out in the `FRONT_SEQ_ROLL_MODE` handler, so the early-roll branch can never be disabled from the front panel. Not a defect; noted because it means that branch is always live.

6. **Carried over from Session 036, still unfixed**: fixed row-0 sub-step addressing in the front-panel step editor (see the preserved record below). Note that this session's finding weakens the case that *rotation* is the only way to reach that mismatch — live recording could produce it too — but the addressing gap itself is unchanged and still needs a decision.

---

## Preserved Record — Session 036 `PROBABILITY_INVESTIGATION.md`

`PROBABILITY_INVESTIGATION.md` is being deleted by the user. Its durable technical content is preserved here. (Session 036's own log holds the narrative; this section keeps the forensic data and code citations that log deliberately did not repeat.)

### Original report
> When loading the attached Song (number 75), the device ignores probability settings on the Clap/Cym track (and possibly in other tracks as well). This is reoccurring on all loads, but I haven't been able to recreate when doing a track from scratch (yet). Other tracks probability seems to work ok. Using Catalyst 1.01

Attached: `P075.ALL`, `P075.PRF`, `P075.SND` (preset name `"Solumn"`, file format version `5`).

### Voice mapping
`SAVE_TYPE_CYM` (`menu.c`) labels this voice `"Clap/Cym"`. Cross-referencing the save-type ordering (`Drum1, Drum2, Drum3, Snare, Cym, HiHat`) against the track loop in `preset_readDrumVoice`/`preset_loadAll` (`presetManager.c`) puts it at **track index 4** (0-based) of `NUM_TRACKS = 7`. Tracks 5/6 are closed/open hi-hat and intentionally share one synth voice.

### File layout used for the forensics
```
stepOffset = STEPDATA_OFFSET + track*NUM_PATTERN*STEPS_PER_PATTERN*sizeof(StepData)
                             + pattern*STEPS_PER_PATTERN*sizeof(StepData)
StepData = { volume, prob, note, param1Nr, param1Val, param2Nr, param2Val }   // 7 bytes
```
`VERSION_4_ALL_STEPDATA_OFFSET = 1097` for `.ALL`. Verified still correct for this **version 5** file: `1097 + 50176 (7 tracks x 8 patterns x 128 steps x 7 bytes) = 51273`, landing exactly on `VERSION_4_ALL_MAINSTEP_OFFSET`; the full offset chain (`mainstep → patchain → shuffle → length → scale`) sums to exactly `51514` bytes, the real size of `P075.ALL`. Same check passes for `P075.PRF` (51515 bytes). **The constants are stale in *name* only** (file version is 5, constants say "VERSION_4") — worth a defensive review if the header format changes again.

Main-step bitmask location: `MAINSTEP_OFFSET + (pattern*NUM_TRACKS + track)*2`, 16-bit little-endian, one bit per main step.

### The non-default probability bytes (the data Session 036's first scan missed)

Session 036's initial scan filtered to `STEP_ACTIVE_MASK`-active steps only and therefore concluded "no probability is set anywhere in the file" — **wrong**. Re-scanning without that filter found four non-default `prob` bytes, all in track 4 (Clap/Cym), pattern 0, and nowhere else:

| Raw sub-step index | Displayed as | `STEP_ACTIVE_MASK` | `prob` |
|---|---|---|---|
| 40 | main step 6, row 0 | inactive | 32 |
| 64 | main step 9, row 0 | inactive | 28 |
| 72 | main step 10, row 0 | inactive | 27 |
| 88 | main step 12, row 0 | inactive | **25** (the one the user pointed at) |

The sub-step actually flagged `STEP_ACTIVE_MASK` for each of those main steps is **one position later** — 41, 65, 73, 89 — and each of those retains the untouched default `prob = 127`. That one-slot gap between "where the probability was written" and "where the note that plays lives" is the whole mechanism.

> **Correction to the deleted document.** `PROBABILITY_INVESTIGATION.md` stated that the main-step bitmask for track 4 pattern 0 is `0x1800` "with bits 5, 8, 9, and 11 set — main steps 6, 9, 10, 12". That is arithmetically wrong and is **not** preserved as written: `0x1800` has only **bits 11 and 12** set, i.e. main steps **12 and 13** (1-based). The user's own decode in `pat_example.txt` agrees (`raw16=0x1800 binary=0b1100000000000`, "bit 11 (main step 12): ON", "bit 12 (main step 13): ON"). Of the four probability-bearing rows, therefore, only **main step 12 is actually an enabled main step** — which is precisely the one the user reported (`step 12 shows as PRB 25 but this does not do anything`). Main steps 6, 9 and 10 carry a stored probability on a main step that is switched off entirely, so they were never going to be audible for a second, independent reason. The core finding is unaffected; the supporting bitmask claim was not.

**Standing lesson, worth keeping**: before trusting any "no X is set anywhere" forensic conclusion, scan **all** steps for the field, not just `STEP_ACTIVE_MASK`-active ones.

### Global settings decoded from the same file
Stored one byte per parameter from byte offset 9, in `PAR_BEGINNING_OF_GLOBALS`-relative enum order:

| Parameter | Offset | Value | Meaning |
|---|---|---|---|
| `PAR_BPM` | 9 | 105 | real tempo |
| `PAR_MIDI_CHAN_1..6` | 10-15 | 10..15 | — |
| `PAR_FETCH` | 16 | 0 | off |
| **`PAR_FOLLOW`** | 17 | **1 (ON)** | front panel tracks the playing pattern |
| `PAR_QUANTISATION` | 18 | 2 | `QUANT_16` |
| **`PAR_FILE_LOAD_BACKGROUND`** | 35 | **1 ("pat")** | background-swap loading for `.pat` loads only |
| `PAR_GLOBAL_SETTINGS_VERSION` | 36 | 5 | matches file version |

Also confirmed: all track scales are 0 throughout the file (relevant because `seq_quantize()` divides its multiplier by `1 << scale`, so a non-zero scale would have disabled quantization).

### Ruled out (each traced end to end)
- **SysEx transport, both directions.** The 7-byte step struct is packed as seven 7-bit values plus one MSB-collector byte. Load path (`preset_readPatternStepData` → `SYSEX_BEGIN_PATTERN_TRANSMIT`) and save path (`frontPanelSending_sendStepInfo` → `SYSEX_REQUEST_STEP_DATA`) were compared field by field including the MSB shift math (`(x & 0x80) >> N` on send vs `(data & (1<<i)) << (7-i)` on receive) — bit-for-bit symmetric, `prob` included.
- **The probability trigger comparison** in `seq_process()`: `seq_rndValue[i] = GetRngValue()&0x7f; ... if(seq_rndValue[i] <= stepData->prob)`. Correct, including the "one random value per 8-sub-step group" behaviour that deliberately allows rolls to be randomized as a unit.
- **The entire Save round trip.** `preset_writePatternData()` loops all 7168 `track x pattern x step` combinations and writes whatever `preset_queryStepDataFromSeq()` returns; that helper **blocks** with a 31-tick retry-resend (not a give-up-and-write-stale timeout) until data actually arrives. No "if muted / if inactive / if pattern mismatch, write default" branch exists anywhere in the path. The only places `prob` is programmatically reset to 127 are `pat_resetNote()` (called only from `pat_clearTrack()`) and `seq_addNote()` — neither is in the save path.

### Structural divergence: shown pattern vs playing pattern
Step-parameter edit opcodes (`FRONT_SEQ_PROB`, `FRONT_SEQ_VOLUME`, `FRONT_SEQ_NOTE`, `FRONT_SET_P1_VAL`, `FRONT_SET_P2_VAL`) all write into `frontParser_shownPattern` — a **single global**. Playback resolves per track through `seq_perTrackActivePattern[]` via `seq_liveStepForTrack()`, and Save dumps by raw `(track, pattern, step)` index. Both playback and Save agree with each other and can legitimately disagree with the menu.

Two mechanisms separate them:
- **Mechanism A — per-track pattern following** (`hold VOICE + press PATTERN`, `seq_setNextPattern(pattern, voice)`). For a single-voice call only `perTrackPendingPattern[voice]` moves, but `loadPendingFlag = 1` still fires `frontPanelSending_sendPatternChange(seq_activePattern)` — an ACK always reporting the *unchanged global* pattern.
- **Mechanism B — shown pattern becomes `SEQ_TMP_PATTERN`.** `pat_getStepPtr()` returns a pointer into `pat_tmpPattern` (scratch, never enumerated by Save's 0-7 loop) for that sentinel. Reachable for real via the background-load machinery.

That divergence is anticipated elsewhere in the codebase — `FRONT_SEQ_CLEAR_AUTOM` explicitly guards `frontParser_shownPattern == seq_perTrackActivePattern[voice]` before touching live state — just not by the step-parameter edit opcodes.

### The Session 036 fix (retained, still valid)
`avrCommsReceivingProtocol.c`'s `SEQ_CHANGE_PAT` ACK handler, on `PATTERN_SETTINGS_PAGE`, updated only the AVR-local `menu_shownPattern` and never sent `SEQ_SET_SHOWN_PATTERN` to the STM — despite a comment claiming the update was deferred to a "shift button release handler" flush. **No such flush exists**: `menu_shiftPerf(0)` does not call `menu_setShownPattern()` and never reads the stashed value, and `menu_switchPage()` has no fallback either. Fixed by calling `menu_setShownPattern(patMsg)` in that branch, deliberately keeping the `if`/`else` split so the branch's LED-refresh calls stay skipped (they would otherwise clobber the page's repurposed rotation-indicator LEDs).

**Terminology warning, repeatedly gotten wrong**: `PATTERN_SETTINGS_PAGE` (hold `SHIFT` while *already on* PERF, `menu_shiftPerf()`, shows track rotation on the step LEDs) is **not** `EUKLID_PAGE` / `SELECT_MODE_PAT_GEN` (hold `SHIFT` *then press* `PERF`, `menu_enterPatgenMode()`, sends `SEQ_EUKLID_RESET`). They are separate enum values and separate features.

### The Session 036 root-cause finding (still unfixed)
The front panel's step-edit path always computes the raw storage index as `mainStepIndex * 8` — row 0 of that step's 8-slot group — with no awareness of sub-step rotation:

```c
// buttonHandler_selectActiveStep()
buttonHandler_selectedStep = (uint8_t)(seqButtonPressed * 8);   // always row 0
avrComms_sendData(SEQ_CC, SEQ_REQUEST_STEP_PARAMS, (uint8_t)(seqButtonPressed * 8));
```

`buttonHandler_setRemoveStep()` and the `SELECT_MODE_PAT_GEN` step-select variant do the same. Sub-step rotation (`euklid_setSubStepRotation()` → `euklid_rotatePattern()`) physically shifts step *contents* within the group — the copy itself (`euklid_copySubStep()`) is correct and complete, moving note, volume, `STEP_ACTIVE_MASK` and `prob` together — and for a pure sub-step rotation deliberately **skips** rotating the main-step bitmask. So the grid still lights "step 12" while the live note lives at raw 89, and editing step 12 writes to raw 88.

**Open question, still undecided**: is fixed row-0 addressing a bug (a user editing "step 12" reasonably expects to affect the note that audibly *is* step 12) or a rotation-semantics/UX gap (row 0 always meant physical row 0, and the fix is an affordance that shows/selects the active row)? Either way a fix must touch all three call sites consistently.

### Session 036's two rotation fixes (both retained)
- **Retrigger while stopped.** `PAR_EUKLID_ROTATION` / `PAR_EUKLID_SUBSTEP_ROTATION` each resent `SEQ_SET_ACTIVE_TRACK` before every rotation nudge; STM's `FRONT_SEQ_SET_ACTIVE_TRACK` treats "same track, sequencer stopped" as the documented voice-preview gesture, so every nudge fired it. Fixed by removing the redundant resend from exactly those two handlers — safe because reaching either parameter requires the Euclid page, whose voice-button handler already sends `SEQ_SET_ACTIVE_TRACK` once. **The identical redundant-resend idiom still exists** on `PAR_EUKLID_LENGTH`, `PAR_EUKLID_STEPS`, `PAR_POS_X`, `PAR_POS_Y`, `PAR_FLUX`, `PAR_SOM_FREQ`, `PAR_TRACK_LENGTH`, `PAR_TRACK_SCALE` and was intentionally left alone.
- **Sub-step rotation moving main steps.** `euklid_rotatePattern()` folds any delta with `|delta| > 7` into a whole main-step rotation; the dial allowed 0-15, so an ordinary sub-step-only move could trigger it. Fixed by clamping `PAR_EUKLID_SUBSTEP_ROTATION` to 0-7 at its three `menu.c` clamp sites — algebraically sufficient, since every delta between two 0-7 values stays within ±7. Implemented as a `paramNr` special case inside the existing `DTYPE_0B15` branch because `menu.h`'s `Datatypes` enum is hard-capped at exactly 16 entries (4-bit packed) with no free slot. Side effect to expect: a track already rotated past 7 under the old range will snap its *value* down to 7 on its next edit (stored pattern data is untouched until then).

---

## Repository State At Session End

- Branch `dev-prb-fix`.
- Modified this session: `mainboard/LxrStm32/src/Sequencer/sequencer.c`, `sequencer.h`, `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c`, `firmware image/FIRMWARE.BIN`, `LIVE_REC_DUPLICATE_SUBSTEP_BUG.md`, `knowledge_files/log_archive/000_SESSION_INDEX.md`, `MEMORY.md`, plus this new log.
- Carried in from Session 036 and still uncommitted: `front/LxrAvr/Menu/menu.c`, `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`, `PROBABILITY_INVESTIGATION.md`, `knowledge_files/log_archive/036_SESSION_HANDOFF_LOG.md`.
- User-authored, untracked/added: `tools/file_decode.py`, `tools/rotation_correction.py`, `pat_example.txt`, `sd_unpacked_decoded/`, `SD_CARD/`.
- No git mutation was performed — nothing committed, staged or pushed by the assistant.

---

## End Of Session Block

```
DATE: 2026-08-22
SESSION GOAL: Find the true cause of the live-record roll duplicate sub-step bug after
              Session 036's fix failed hardware testing; make only the precise targeted
              fix; then close the session and preserve the two root working documents.

COMPLETED:
- Built a host-side simulation harness from verbatim copies of seq_quantize(),
  seq_addNote(), seq_rollTrig(), seq_setRoll() and seq_checkRollStep(), driven by a
  replica of seq_process()'s per-tick ordering.
- Proved Session 036's fix could not have worked (0 of 1070 symptom-producing configs
  under the user's settings) and that it had introduced a note-dropping regression.
  Reverted it in full.
- Found the true root cause: seq_addNote() inferred "MIDI note-off" from vel == 0 alone
  and wrote such events at the raw un-quantized playhead index, which for a roll hit is
  always mainStep*8 + 1 due to the per-track/master step-index skew; pat_setMainStep()
  then un-masked the default-active sub-step 0, firing both.
- Fixed with an explicit isNoteOff parameter; only the MIDI channel-parser path passes 1.
- Rewrote LIVE_REC_DUPLICATE_SUBSTEP_BUG.md, then preserved its content and that of
  PROBABILITY_INVESTIGATION.md into this log ahead of the user deleting both.
- Noted tools/rotation_correction.py (user-authored) in the logs and startup docs.

VERIFIED ON HARDWARE: YES — user confirmed recording rolls with both roll=one and
                      roll=16 now enters only one sub-step.

CHANGES THIS SESSION:
- mainboard/LxrStm32/src/Sequencer/sequencer.c: reverted seq_rollTrig()'s allowRecord
  parameter; added isNoteOff to seq_addNote() and gated the un-quantized branch on it;
  routed all six internal callers with 0; large rationale comment block added.
- mainboard/LxrStm32/src/Sequencer/sequencer.h: both declarations updated with contracts.
- mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c: passes isNoteOff = 1 with a comment.
- firmware image/FIRMWARE.BIN: rebuilt.
- LIVE_REC_DUPLICATE_SUBSTEP_BUG.md: rewritten (user will delete; preserved here).
- knowledge_files/log_archive/000_SESSION_INDEX.md: 037 row, 036+037 summaries, 6 facts.
- MEMORY.md: status, WIP docs, sequencer reminders, tools entry.

KNOWN ISSUES INTRODUCED: none identified. The change is strictly narrowing.

KNOWN ISSUES RESOLVED:
- Live-record roll duplicate sub-step (hardware-verified).
- Session 036's dropped-note regression for taps landing 2-3 sub-steps after a boundary.

NEXT SESSION RECOMMENDED GOAL: decide the approach for the still-unfixed fixed row-0
    sub-step addressing in the front-panel step editor (dynamic active-row resolution vs
    a UX indicator), which touches buttonHandler_selectActiveStep(),
    buttonHandler_setRemoveStep() and the SELECT_MODE_PAT_GEN step-select path.

BLOCKERS: none for the completed work. The row-0 addressing item needs a design
          decision from the user before implementation.

CRITICAL REMINDERS FOR NEXT SESSION:
- seq_stepIndex[track] runs exactly ONE SUB-STEP AHEAD of seq_stepIndex[NUM_TRACKS]
  during seq_process(). Any code writing at a raw per-track index while testing a
  boundary on the master index inherits this skew.
- Velocity 0 does NOT mean "note off" and does NOT mean "no step". Zero-velocity active
  steps are intentional ghost steps. Placement is decided by isNoteOff, never by velocity.
- Do NOT re-suppress the early-roll hit's recording in seq_setRoll(). That hit records
  the CORRECTLY quantized position; suppressing it drops notes. Session 036 made this
  mistake; the call site now carries a comment saying so.
- Before trusting any "no X is set anywhere in the file" forensic conclusion, scan ALL
  steps for the field, not just STEP_ACTIVE_MASK-active ones.
- PATTERN_SETTINGS_PAGE (SHIFT held while already on PERF) is NOT EUKLID_PAGE /
  SELECT_MODE_PAT_GEN (SHIFT then press PERF). Separate pages, separate features.
- The firmware fix does not repair already-damaged saved songs. Use
  tools/rotation_correction.py, or clear and re-record the affected main steps.
- When static analysis gives a confident answer that hardware contradicts, build the
  simulation. Copy the real functions verbatim.
```
