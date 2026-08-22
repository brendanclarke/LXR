# Live-Record Duplicate Sub-Step Bug (Roll Buttons)

## Report

> When I live record (record with play on, toggled by the REC button), and enter some steps by the roll buttons (SEQ buttons 0-6 while in PERF mode), both the first and second sub-steps are recorded when only the first sub-step should be. Roll set to 16 or one, quantize set to 16.

> (follow-up) Both roll=one and roll=16th notes produce two sub-steps (ss 0 and 1) at the beginning of a main step when recorded.

"Roll buttons" = the 7 `SEQ` buttons while `SELECT_MODE_PERF` is active and `SHIFT` is not held. Press sends `SEQ_ROLL_ON_OFF` with the on flag, release sends it with the flag clear ([buttonHandler.c:919](front/LxrAvr/buttonHandler.c#L919), [buttonHandler.c:1056](front/LxrAvr/buttonHandler.c#L1056)); on the STM the handler does nothing but call `seq_rollChange()` ([frontPanelReceivingProtocol.c:2754-2760](mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c#L2754-L2760)) — verified, not assumed. "Quantize 16" = `QUANT_16`, confirmed against `quantisationNames[] = {off,8,16,32,64}` ([MenuText.h:68](front/LxrAvr/Menu/MenuText.h#L68)) → menu index 2 → `seq_setQuantisation(2)` → `seq_stepsPerQuant = 8`.

---

## ⚠ Correction: the first fix in this file was wrong and has been reverted

The original version of this document blamed the "early roll" preview hit in `seq_setRoll()` and suppressed its recording via an `allowRecord` parameter on `seq_rollTrig()`. **That was incorrect on both counts**, and the change has been fully reverted.

It was disproved by simulation rather than by re-reading: `seq_quantize()`, `seq_addNote()`, `seq_rollTrig()`, `seq_setRoll()` and `seq_checkRollStep()` were copied verbatim into a host-side harness driven by a replica of `seq_process()`'s per-tick ordering, then swept across **every** combination of roll mode × roll rate × quantize setting × track scale × press timing × hold length.

- **It did not fix the reported bug.** Under the user's stated settings (quantize 1/16, track scale off) the sweep found **0 of 1070** symptom-producing configurations — the roll path cannot write sub-step 1 at all with quantization on. The early-roll theory produced two adjacent *main* steps, never two sub-steps inside one main step.
- **It introduced a regression.** The early hit's record was the *correctly quantized* one: `seq_addNote()` snaps it back to the boundary that just passed, which is the position the player intended. The later on-time hit lands a full 16th note **later**. Suppressing the early record therefore silently dropped notes:

| tap lands at | before the bad fix | after the bad fix |
|---|---|---|
| sub-step 0 | main 4 ss0 | main 4 ss0 |
| sub-step 1 | main 4 ss0 | main 4 ss0 |
| **sub-step 2** | **main 4 ss0** | **nothing recorded** |
| **sub-step 3** | **main 4 ss0** | **nothing recorded** |

The early-roll branch is a deliberate humanization feature and its recording behaviour was correct. It is now restored, with a comment saying so.

---

## Part 1 — Confirmed root cause

### `seq_addNote()` infers "MIDI note-off" from velocity alone

[sequencer.c](mainboard/LxrStm32/src/Sequencer/sequencer.c), `seq_addNote()` has always chosen between two different destination slots purely on velocity:

```c
if (vel==0)                                                   /* pre-fix */
   stepPtr = pat_getStepPtr(targetPattern, trackNr, unquantizedStep);
else
   stepPtr = pat_getStepPtr(targetPattern, trackNr, quantizedStep);
```

The un-quantized placement exists **for MIDI note-offs**. A note-off carries velocity 0 by definition — `channelMidiParser_noteOff()` forces `vel = 0` before delegating to `channelMidiParser_noteOn()` ([ChannelMidiParser.c:74-80](mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c#L74-L80)) — and marking where a held note was released is only meaningful at its true position. The resulting zero-velocity "ghost" step is an intentional concept in this sequencer (see the `STEP_VOLUME_MASK > 0` guard in `seq_process()`'s automation-release block).

### Velocity 0 reaches `seq_addNote()` from paths that are not note-offs

`seq_rollTrig()` resolves its record velocity from one of two places:

- `ROLL_MODE_VELOCITY` / `_BOTH` / `_ALL` → `vol = seq_rollVelocity` — the **roll velocity menu parameter, which can be dialled to 0**.
- `ROLL_MODE_TRIG` / `_NOTE` → `vol = stepData->volume & 0x7f` — **the stored volume of the step under the playhead, which is 0 for any zero-velocity ghost step**.

Either way an ordinary roll hit can carry velocity 0, and was then misread as a note-off.

### Why the second sub-step is always exactly sub-step 1

The raw index it falls back to is not arbitrary. In `seq_process()`:

- `seq_stepIndex[i]` is incremented at the **top** of the per-track loop ([sequencer.c:1015-1016](mainboard/LxrStm32/src/Sequencer/sequencer.c#L1015-L1016));
- `seq_stepIndex[NUM_TRACKS]` — the master index whose `% seq_stepsPerQuant` test decides "we are on a quantize boundary" in `seq_setRoll()` — is not incremented until the **end** of `seq_process()` ([sequencer.c:1151](mainboard/LxrStm32/src/Sequencer/sequencer.c#L1151)).

So the per-track index runs exactly **one sub-step ahead** of the master index. At the moment the quantize test says "boundary", `seq_stepIndex[track]` is already `mainStep*8 + 1` — **sub-step 1**. `seq_quantize()` normally folds that back to `mainStep*8`; the note-off branch bypasses `seq_quantize()` entirely and writes it raw.

`pat_setMainStep()` then switches the main step on, which un-masks **sub-step 0** — active by default in every cleared main step ([`pat_clearTrack()`, PatternData.c:330-331](mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.c#L330-L331)). Result: **sub-steps 0 and 1 both fire at the start of the main step.**

The condition is also **self-sustaining**: the step written this way holds `0 | STEP_ACTIVE_MASK`, so the next pass reads `volume & 0x7f == 0` again and takes the same wrong branch. This matches "reoccurring on all loads".

### Simulation evidence

Same harness, quantize 1/16, track scale off, sub-steps seeded with volume 0:

```
 roll mode TRIG:
   press at ss0 ->  main4:[ss1 ]  main5:[ss1 ]     <-- off-grid write
   press at ss1 ->  main4:[ss1 ]  main5:[ss1 ]
 roll mode ALL (roll velocity non-zero):
   press at ss0 ->  main4:[ss0 ]  main5:[ss0 ]     <-- correct
```

Sub-step 1 appears exactly when the resolved velocity is 0, and only then.

### Corroboration from the user's own P075 file

`pat_example.txt` (track 4, pattern 0) shows all 16 recorded notes at raw indices **1, 9, 17, 25 … 121** — every one at `8k+1` — with sub-step 0 explicitly *cleared*, `vol=100`, `prob=127`, `note=63`. That combination is the unmistakable signature of `seq_addNote()`: it is the only code that writes `prob = 127` alongside volume and note, and the only code that conditionally clears sub-step 0. A real pattern on real hardware had already landed every note one sub-step off the grid.

This is also **the missing half of the original probability bug**: the probability values the user set landed at raw 40/64/72/88 (`8k`, where the front panel's step editor addresses), while the audible notes sit at `8k+1`. That is exactly why "step 12 shows PRB 25 but this does not do anything" — the two were addressing different sub-steps. See `PROBABILITY_INVESTIGATION.md` Part 8.

---

## Part 2 — The fix

Make the un-quantized placement **explicit** rather than inferred. `seq_addNote()` gains an `isNoteOff` parameter:

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

Call sites:

| caller | passes | reason |
|---|---|---|
| `channelMidiParser_noteOn()` / `noteOff()` | `1` | the only path where `vel == 0` really means "note released" — MIDI behaviour unchanged |
| `seq_rollTrig()`, all five `ROLL_MODE_*` branches | `0` | a roll hit is always a real trigger, never a note-off |
| `seq_process()` loop re-record | `0` | replaying an existing step, never a note-off |

A zero-velocity roll now overwrites sub-step 0 on the grid (silent, as intended) instead of creating a second, off-grid sub-step beside it. The full rationale — including the one-sub-step index skew that makes the stray write land on sub-step 1 — is in the comment block above `seq_addNote()` in `sequencer.c`, with a pointer to it from `sequencer.h`.

`seq_rollTrig()`'s `allowRecord` parameter from the reverted fix is gone; the function is back to `seq_rollTrig(uint8_t voice)`.

---

## Verification

- Host-side simulation of the real functions: the bug condition now records **ss0 only**, in every roll mode, at every press offset. The dropped-note regression from the reverted fix is gone (taps at sub-steps 2 and 3 record correctly again).
- `make -C mainboard/LxrStm32 -j4 stm32` after `clean` — succeeded. The only warnings in the changed files are the **pre-existing** `seq_init` loop-bounds/`memset` warnings already documented in `MEMORY.md`. No new warnings, no errors.
- `make firmware` — succeeded end to end; `firmware image/FIRMWARE.BIN` rebuilt.
- `git diff --stat` — only `sequencer.c`, `sequencer.h`, `ChannelMidiParser.c` and the firmware image.

**Not hardware-tested.** Recommended check: REC + PLAY, quantize 16, tap a roll button; confirm one sub-step lights, not two. Then set **roll velocity to 0** and repeat — that is the most likely trigger of the original report and the case the fix targets most directly. Also worth re-testing a pattern that already exhibits the fault: existing ghost steps at `8k+1` are stored data and will **not** be repaired by this fix; those main steps need clearing and re-recording.

## Open items

- **Which zero-velocity source applied on the user's unit is unconfirmed.** The fix closes all of them, but knowing whether it was `PAR_ROLL_VELOCITY = 0` or a pre-existing ghost step would confirm the diagnosis outright. Checking the roll velocity value in the menu costs nothing.
- **Pre-existing, not touched:** a tap landing in the second half of a quantize window and released before the next boundary records **nothing at all** (sub-steps 4-6 in the tables above). This predates every change here and is a separate, arguably intentional trade-off in `seq_setRoll()`'s early-window condition `< (seq_stepsPerQuant/2 - 1)`. Flagged, not changed.
- **Separate latent defect found while reading, not touched:** `LengthRotate` ([PatternData.h:57-67](mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.h#L57-L67)) declares `length:4 + scale:3 + rotate:4` = **11 bits** inside a union with a `uint8_t value`. `sizeof(LengthRotate)` is therefore **4, not 1**, and `value` aliases only the low byte — `rotate`'s upper 3 bits live outside it entirely. Any code treating `value` as the whole packed state silently loses rotation ≥ 2. `scale` is *not* corrupted by rotation writes (verified), so this is not the cause of this bug, but it is worth its own look.
- `seq_skipFirstRoll` ([sequencer.c:77](mainboard/LxrStm32/src/Sequencer/sequencer.c#L77)) is permanently 0 — its only setter is commented out in the `FRONT_SEQ_ROLL_MODE` handler, so the early-roll branch can never be disabled from the front panel. Not a defect; noted because it means that branch is always live.
