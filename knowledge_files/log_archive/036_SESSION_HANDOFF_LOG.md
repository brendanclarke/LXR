# 036 Session Handoff Log - Step-Probability Investigation and Pattern-Sync Fix

DATE: 2026-08-20

## Session Goal

Investigate a user bug report: loading a specific saved song ("Song 75" /
`P075.ALL` / `P075.PRF` / `P075.SND`, preset name `"Solumn"`) causes step
probability on the Clap/Cym track (and possibly others) to be ignored during
playback. The user could not reproduce it building a pattern from scratch,
only after a load. Three attached files (`probability_bug_files/`) were
provided as a repro vehicle. The user asked for a written investigation
first (`PROBABILITY_INVESTIGATION.md`), then, once a root cause was
identified and a fix judged low-risk, asked for the fix to be implemented
with adjacent explanatory comment blocks, verified, and the session closed
out with full documentation.

No prior session covered this territory directly; it intersects the
Session 028/033 background-load/temp-pattern machinery, an area already
flagged in `MEMORY.md` as having residual complexity. It also touches the
`PATTERN_SETTINGS_PAGE` front-panel flow (hold `SHIFT` while already on the
PERF page), which is **not** the Session 033 `EUKLID_PAGE` rollback WIP —
`menu.h` defines them as separate pages, and `MEMORY.md` already carries an
explicit warning not to conflate the two. An earlier draft of this
session's investigation made exactly that mix-up before it was caught and
corrected; see the note in `PROBABILITY_INVESTIGATION.md` Part 5.

## Investigation Findings

The complete investigation, including all forensic data, code citations, and
reasoning, lives in root `PROBABILITY_INVESTIGATION.md` (7 parts). Summary:

### Part 1 — File forensics

Parsed the raw bytes of `P075.ALL`/`P075.PRF` directly (Python, byte-offset
math against the documented `.ALL`/`.PRF` layout in `presetManager.c`,
verified byte-exact against the real file sizes for this specific file).
Found that **every step on every one of the 7 tracks x 8 patterns** is at
pure default: `note = 63` (`SEQ_DEFAULT_NOTE`), `volume = 100`, `prob = 127`
— exactly the values `pat_resetNote()` (`PatternData.c`) writes the first
time a main step is turned on. None of the 8 real saved patterns, on any
track, has ever been captured with a customized note, volume, or
probability. This ruled out "only the prob field is wrong" and pointed at
something that swallows step edits generally, not something prob-specific.

Also decoded this song's saved global settings (byte-offset math against
`Parameters.h`'s `PAR_BEGINNING_OF_GLOBALS`-relative enum order): `PAR_BPM =
105`, `PAR_FOLLOW = 1` (ON), `PAR_FILE_LOAD_BACKGROUND = 1` ("pat" mode
only). These are real, plausible values, not defaults — this is a genuine
in-use song, not a throwaway test file.

### Part 2 — Transport ruled out

Traced the full 7-byte `Step`/`StepData` SysEx pack/unpack (`volume, prob,
note, param1Nr, param1Val, param2Nr, param2Val` + one MSB-collector byte) in
both directions (`preset_readPatternStepData` / `SYSEX_BEGIN_PATTERN_TRANSMIT`
for load; `frontPanelSending_sendStepInfo` / `SYSEX_REQUEST_STEP_DATA` for
save). Bit-for-bit symmetric on both MCUs. The RNG-vs-probability trigger
check in `sequencer.c` (`seq_rndValue[i] <= stepData->prob`) is logically
correct. Neither is the bug.

### Part 3 — Question: can a menu probability edit land on the wrong pattern?

**Yes, confirmed.** Step-parameter edit opcodes (`FRONT_SEQ_PROB`,
`FRONT_SEQ_VOLUME`, `FRONT_SEQ_NOTE`, `FRONT_SET_P1_VAL`, `FRONT_SET_P2_VAL`,
all in `uARTFrontSYX/frontPanelReceivingProtocol.c`) write into whichever
pattern is currently **displayed** (`frontParser_shownPattern`, one global
variable). Actual playback (`seq_liveStepForTrack()`, `sequencer.c`) and
Save (`frontPanelSending_sendStepInfo()`) both resolve the pattern
independently — playback per-track via `seq_perTrackActivePattern[track]`,
Save by raw index — and neither ever consults `frontParser_shownPattern`.
Two structural mechanisms separate "shown" from "actually playing":

- **Mechanism A**: per-track pattern following (`hold VOICE + press
  PATTERN`, a documented feature, `seq_setNextPattern()`) reassigns only
  `seq_perTrackActivePattern[voice]` for one track; the pattern-change ACK
  that could resync the shown pattern always reports the unchanged *global*
  active pattern, never the per-voice one.
- **Mechanism B**: `frontParser_shownPattern` can legitimately become
  `SEQ_TMP_PATTERN` (the background-load scratch pattern) via the same ACK
  path. `pat_getStepPtr()` (`PatternData.c`) routes `SEQ_TMP_PATTERN` writes
  into a completely separate scratch struct (`pat_tmpPattern`) that Save's
  per-pattern loop (`preset_writePatternData()`, always enumerates real
  patterns 0-7) never visits. An edit made in this state is invisible to
  Save on *every* real pattern — matching Part 1's forensic finding exactly.

### Part 4 — Question: does Save strip a probability value that was set?

**No evidence found.** Traced the entire save round trip
(`preset_writePatternData()` -> `preset_queryStepDataFromSeq()` -> STM
`SYSEX_REQUEST_STEP_DATA` -> `seq_sendStepInfoToFront()` /
`frontPanelSending_sendStepInfo()`). It is an unconditional, blocking,
per-`(track, pattern, step)` raw dump straight from `pat_subStepPattern[...]`
to the file — no masking beyond the expected 7-bit SysEx split, no default
substitution, no conditional skip anywhere in the path. `pat_getStepPtr()`
is a bare address calculator with no side effects. The only places `prob`
is ever programmatically reset to `127` are step-creation helpers
(`pat_resetNote()`, `seq_addNote()`), neither of which is in the save path.

### Part 5 — The confirmed defect (the actual root cause)

The user asked specifically whether there's a *separate defect* (not Follow
or background-loading themselves misbehaving) that could strand edits.
Re-examined the pattern-change ACK handler in full
(`avrCommsReceivingProtocol.c`, `case SEQ_CHANGE_PAT`):

```c
if(parameter_values[PAR_FOLLOW] || tempBoundaryAck) {
   if( menu_activePage != PATTERN_SETTINGS_PAGE)
   {
      menu_setShownPattern(patMsg);
      led_clearSequencerLeds();
      avrComms_updatePatternLeds();
      avrComms_sendData(SEQ_CC,SEQ_REQUEST_PATTERN_PARAMS,patMsg);
   }
   else {
      //store the pending pattern update for shift button release handler
      menu_shownPattern = avrCommsParser_command.data2;
   }
}
```

`tempBoundaryAck` is computed independently of `PAR_FOLLOW` and is
specifically the existing safety net meant to force a resync at the
temp/normal boundary even with Follow off — i.e. exactly the mechanism the
user hypothesized should already handle the "Follow off during background
load" edge case. It works correctly **only if** `menu_activePage !=
PATTERN_SETTINGS_PAGE`. `PATTERN_SETTINGS_PAGE` is entered by holding
`SHIFT` while already on the PERF page (`menu_shiftPerf(1)`, `menu.c`),
which repurposes the step LEDs to show the active track's rotation value.
This is a *different* page from `EUKLID_PAGE` / `SELECT_MODE_PAT_GEN` (the
real Euclid generator page, entered by holding `SHIFT` *then* pressing
`PERF` via `menu_enterPatgenMode()`, which is what the Session 033 rollback
WIP actually covers) — `menu_shiftPerf()` sends no Euclid opcode at all. If
a pattern-change ACK (including a genuine temp-boundary crossing)
arrives while that page is up, the `else` branch updates only the AVR's own
**local** `menu_shownPattern` and never sends `SEQ_SET_SHOWN_PATTERN` to the
STM. The comment claims this is deferred "for shift button release
handler," but the actual release handler, `menu_shiftPerf(0)` (`menu.c`),
never flushes it — it just switches the page back and repaints LEDs. No
other code path (checked `menu_switchPage()` and all other callers) flushes
it either. The deferred update is simply lost, and `frontParser_shownPattern`
on the STM can stay stuck — potentially on `SEQ_TMP_PATTERN` — until an
unrelated, later pattern-change ACK happens to arrive while the AVR is off
that page.

This single defect defeats *both* the Follow-driven sync and the
temp-boundary safety net at once, regardless of the Follow setting, and
fully explains how a probability (or volume/note) edit can look completely
normal in the menu yet never reach any saved pattern — exactly what Part 1
found. It is not the Follow feature or the background-load feature
misbehaving; both work as designed. It is this third, independent code path
silently failing its own stated intent.

### Part 6 — Fix feasibility assessment

Assessed whether "always sync regardless of page" is simple and low-risk.
Found that the `if` branch bundles two different jobs: (1) state sync
(`menu_setShownPattern()`) and (2) an LED refresh
(`led_clearSequencerLeds()`, `avrComms_updatePatternLeds()`,
`SEQ_REQUEST_PATTERN_PARAMS`) that would visibly clobber the Pattern
Settings page's repurposed rotation-indicator LEDs if run unconditionally. Concluded the
low-risk fix is to keep the `if`/`else` split (so LEDs stay untouched on
that page) and change only what the `else` branch does — swap the raw local
assignment for a call to the existing `menu_setShownPattern()` helper, which
has no LED side effects of its own. Flagged one narrow residual interaction
to watch on hardware: the STM's `FRONT_SEQ_SET_SHOWN_PATTERN` handler
triggers `seq_realign()` if the incoming value already matches its current
`frontParser_shownPattern`, unless a temp-boundary ack is pending (which
correctly suppresses it for genuine boundary crossings). A *non-boundary*,
Follow-driven, redundant same-pattern ACK arriving while on the page could
now trigger a harmless extra realign that previously silently no-opped —
low risk, not data loss, but worth confirming on hardware.

## Root Cause And Fix Design

Root cause: `avrCommsReceivingProtocol.c`'s `SEQ_CHANGE_PAT` ACK handler's
`PATTERN_SETTINGS_PAGE` branch stashes a shown-pattern update locally and
relies on a "shift button release" flush that was never implemented.

Fix: replace the local-only assignment with a call to the already-existing,
already-proven `menu_setShownPattern(patMsg)` helper (used at 4+ other call
sites, including every file load and `buttonHandler.c`). This is the
smallest change that closes the gap: it sends `SEQ_SET_SHOWN_PATTERN` to the
STM (fixing the actual defect) without touching any LED calls (preserving
the page's specialized display, which was almost certainly why the special
case existed in the first place). It also fixes a small pre-existing
inconsistency where this branch stored the raw, un-normalized
`avrCommsParser_command.data2` instead of the already `SEQ_TMP_PATTERN`-
normalized `patMsg` used by the sibling branch.

No STM-side change was needed — `FRONT_SEQ_SET_SHOWN_PATTERN`
(`frontPanelReceivingProtocol.c`) already handles both ordinary pattern
values and `SEQ_TMP_PATTERN` correctly and has no page awareness; it simply
needed to receive the message more often, which the AVR-side fix now
guarantees.

## Completed Changes

### `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`

- In `avrComms_parseData()`, `case SEQ_CHANGE_PAT`, the `else` branch (taken
  when `menu_activePage == PATTERN_SETTINGS_PAGE`) now calls
  `menu_setShownPattern(patMsg);` instead of directly assigning
  `menu_shownPattern = avrCommsParser_command.data2;`.
- Added a large explanatory comment block directly above the change
  covering: why the branch exists, what changed and why it's safe (no LED
  side effects), the input (`patMsg`), the output (local state update +
  `SEQ_SET_SHOWN_PATTERN` sent to STM), and the affiliated STM-side code
  (`frontPanelReceivingProtocol.c`'s `FRONT_SEQ_SET_SHOWN_PATTERN` handler,
  `PatternData.c`'s `pat_getStepPtr()`), per the user's request that all
  changes carry adjacent comment blocks with this level of detail.
- No other lines in this file were touched.

### `PROBABILITY_INVESTIGATION.md` (root)

Built up over the session into a 7-part investigation document:
1. File forensics on the attached `.ALL`/`.PRF`/`.SND`.
2. Transport/round-trip verification (ruled out).
3. Shown-pattern-vs-active-pattern divergence mechanisms (A: per-track
   following, B: temp-pattern black hole).
4. Save-path trace (ruled out as a stripping source).
5. The confirmed `PATTERN_SETTINGS_PAGE` defect.
6. Fix feasibility and risk assessment.
7. The fix as actually applied, plus build verification and hardware
   verification checklist.

This file is the durable technical record for this bug; this handoff log
summarizes it but the doc itself has the full code citations and forensic
data tables.

### `firmware image/FIRMWARE.BIN`

Rebuilt from the fixed AVR binary and the existing (unchanged) STM32
binary.

## Verification

### Static/build verification

- `make -C front/LxrAvr avr -j4` — succeeded. Only pre-existing
  `-Wimplicit-fallthrough` warnings remain, in an unrelated `LED_SEQ_MAIN_*`
  switch case elsewhere in the same file (present before this change). No
  new warnings or errors from the changed code.
- `make firmware` — succeeded end to end; `firmware image/FIRMWARE.BIN`
  rebuilt.
- `git diff --stat` confirmed only the intended files changed: the fix
  itself and the rebuilt firmware image. No STM32 (`mainboard/LxrStm32`)
  source was touched this session.

### Hardware status

**Not tested on hardware this session.** This was a static-analysis
investigation and a targeted fix; no hardware was available in this
session. See `PROBABILITY_INVESTIGATION.md` Part 7 for the specific
hardware verification checklist:

1. Reproduce the Part 5 scenario (desync `frontParser_shownPattern` via
   per-track pattern following or a background-load temp-boundary crossing
   while on `SHIFT+PERF`) and confirm a probability/volume/note edit made
   after releasing `SHIFT` now takes effect.
2. Confirm the `PATTERN_SETTINGS_PAGE` rotation-indicator LED display is
   unaffected by a pattern-change ACK arriving while the page is up (no LED
   regression).
3. Watch for the narrow residual `seq_realign()` interaction noted in Part
   6; confirm it's inaudible/inconsequential if it ever fires.
4. General regression pass: ordinary pattern switching, Follow on/off, and
   the `PATTERN_SETTINGS_PAGE` rotation-display flow (hold `SHIFT` while on
   PERF), since this is a shared code path used by every pattern change.

## Remaining Limits And Next Decisions

This fix closes the one confirmed defect (Part 5). It does not by itself:

- Close Mechanism A (per-track pattern following leaving a track's editing
  surface permanently decoupled from its playback pattern) — that is a
  consequence of the feature working as designed, not a bug, and remains an
  open UX/robustness question if the user wants it hardened later.
- Fully harden Mechanism B (a step edit made while `frontParser_shownPattern`
  is legitimately `SEQ_TMP_PATTERN`, outside of the now-fixed stuck-state
  case, e.g. during the live streaming window of an actual background load)
  — still silently lands in scratch storage in that case. `PROBABILITY_
  INVESTIGATION.md`'s "Suggested next steps" lists this as a defense-in-depth
  follow-up: reject or redirect step-edit opcodes when
  `frontParser_shownPattern == SEQ_TMP_PATTERN`.

Recommended next session goal: flash the fixed firmware and run the
hardware verification checklist above, starting with reproducing the
original song-75 bug report end to end to confirm the fix resolves it in
practice, not just in the traced code paths.

## Repository State At Session End

Current branch: `master`.

Session-owned functional changes:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`
- `firmware image/FIRMWARE.BIN`

Session documentation changes:

- `PROBABILITY_INVESTIGATION.md` (root; new this session)
- `knowledge_files/log_archive/000_SESSION_INDEX.md` (added missing terse
  Session 035 row, added Session 036 row)
- `knowledge_files/log_archive/036_SESSION_HANDOFF_LOG.md` (this file)
- `knowledge_files/comms_spec_reference/` — reviewed; see note below on
  whether an update was needed.
- `MEMORY.md` — updated for this session's closeout.

Pre-existing/unrelated file at session start:

- `.DS_Store` (untracked macOS artifact, modified by Finder/OS activity
  during the session; not touched intentionally, not part of this session's
  work).

## Post-Closeout Addendum (same session)

After this session's closeout above, the user reviewed the attached files
directly and reported that `P075.ALL` genuinely *does* show a non-default
probability on Clap/Cym, pattern 1, step 12 (`PRB 25`) that has no audible
effect — contradicting this log's and `PROBABILITY_INVESTIGATION.md`'s
original Part 1 conclusion that no pattern in the file had any probability
set at all. That conclusion was wrong: Part 1's forensic scan only printed
steps flagged `STEP_ACTIVE_MASK`-active, so it never surfaced a `prob` byte
stored on an inactive step. Re-scanning without that filter found exactly
four non-default `prob` values in the entire file (32, 28, 27, and the
reported 25), all in track 4 (Clap/Cym) pattern 0, all on `STEP_ACTIVE_MASK`-
inactive steps.

This led to a second, independently verified root cause, written up as
`PROBABILITY_INVESTIGATION.md` Part 8: the front panel's normal step-edit
addressing (`buttonHandler_selectActiveStep()`, `buttonHandler_setRemoveStep()`,
and the `SELECT_MODE_PAT_GEN` step-select variant, all `buttonHandler.c`)
always computes the raw sub-step index as `mainStepIndex*8` — row 0 of that
main step's 8-slot sub-step group — with no awareness of sub-step rotation.
This track has a baked-in `+1` sub-step rotation (`PAR_EUKLID_SUBSTEP_ROTATION`
/ `euklid_rotatePattern()`, `EuklidGenerator.c`), which is a real, documented
feature that physically moves a track's live note within each 8-slot group
without moving the main-step on/off bitmask (`euklid_rotatePattern()`'s own
logic explicitly skips rotating that bitmask for a pure sub-step rotation).
`euklid_copySubStep()` was checked and correctly copies `prob` along with
note/volume/active-flag during that rotation — it is not a rotation-copy
bug. The mismatch is purely in the front panel always addressing row 0
regardless of where rotation has since moved the live note.

This is a **different, separate defect** from the one this session already
fixed (`PATTERN_SETTINGS_PAGE`/`SEQ_CHANGE_PAT`, above) and is a much more
direct, deterministic match for the original report: it requires no
Follow/background-load/temp-boundary timing, reproduces on every load
because the rotation is physically baked into saved data, and does not
reproduce on a freshly built pattern (zero rotation by default). It has
**not** been fixed in this session — it was presented to the user as an
open question (dynamic active-row resolution vs. a UX indicator, since both
are defensible and the fix touches a widely-used editing path) rather than
implemented unilaterally. `PROBABILITY_INVESTIGATION.md` Part 8 has the full
trace, evidence table, and code citations. `MEMORY.md`'s Session 036 status
paragraph and Sequencer/PATGEN Reminders section were both updated with this
correction.

The `PATTERN_SETTINGS_PAGE` fix already applied and build-verified earlier
in this session remains correct and worth keeping — it closes a real,
independently-confirmed gap — it just is not confirmed to be what produced
Song 75's specific symptom.

## Second Post-Closeout Addendum: Two Rotation Bugs, Investigated And Fixed

After the Part 8 correction above, the user reported two further, separate
bugs specifically in pattern rotation (not step-probability) and asked for
them to be investigated and fixed in the same session. Both were found,
confirmed, and fixed. Full user-facing summary: `PROBABILITY_INVESTIGATION.md`
Part 9. Technical detail here.

### Bug 1: voice retriggers on every rotation change while the sequencer is stopped

**Root cause.** `PAR_EUKLID_ROTATION` and `PAR_EUKLID_SUBSTEP_ROTATION`'s AVR
setter handlers (`menu.c`, `menu_parseGlobalParam()`'s parameter switch) each
resent `avrComms_sendData(SEQ_CC,SEQ_SET_ACTIVE_TRACK,menu_getActiveVoice());`
before their real opcode, on *every* encoder nudge — a defensive
"make sure STM has the right active track" idiom shared by several other
parameter handlers in the same file (`PAR_EUKLID_LENGTH`, `PAR_EUKLID_STEPS`,
`PAR_POS_X`, `PAR_POS_Y`, `PAR_FLUX`, `PAR_SOM_FREQ`, `PAR_TRACK_LENGTH`,
`PAR_TRACK_SCALE`). On STM, `FRONT_SEQ_SET_ACTIVE_TRACK`
(`frontPanelReceivingProtocol.c`) contains:

```c
case FRONT_SEQ_SET_ACTIVE_TRACK:
   if ( (frontParser_activeTrack==frontParser_command.data2)&&(!seq_isRunning()) )
      seq_triggerVoice(frontParser_activeTrack, seq_rollVelocity, seq_rollNote);
   frontParser_activeTrack = frontParser_command.data2;
   ...
```

This is the documented "press the already-selected voice button to preview
it while stopped" feature (README). Since the redundant resend always names
the track already being edited on the Euclid page, and the sequencer is
commonly stopped while dialing in a pattern, every rotation nudge satisfied
both conditions and fired the preview trigger — audibly retriggering the
voice on every single encoder click.

**Fix.** Removed the redundant `SEQ_SET_ACTIVE_TRACK` resend from exactly the
two reported handlers, `PAR_EUKLID_ROTATION` and `PAR_EUKLID_SUBSTEP_ROTATION`
(`menu.c`). Verified safe by tracing the only path into either parameter:
`PAR_EUKLID_ROTATION`/`SUBSTEP_ROTATION` are editable only via the encoder
while `SELECT_MODE_PAT_GEN` (the Euclid page, `EUKLID_PAGE`) is active, and
the *only* way to select which track's Euclid parameters are being edited is
pressing a VOICE button, which independently sends `SEQ_SET_ACTIVE_TRACK`
exactly once (`buttonHandler.c`, "select active voice" branch, which also
calls `menu_enterPatgenMode()`) before any rotation opcode can be sent.
`frontParser_activeTrack` on STM is therefore already correct by the time
either handler runs; the resend was pure redundancy with an unwanted side
effect. The genuine voice-button preview gesture (`buttonHandler.c:1295`,
`:1335`) is a separate code path and was not touched.

**Not fixed, same session:** the other 8 parameter handlers sharing the
identical redundant-resend idiom (`PAR_EUKLID_LENGTH`, `PAR_EUKLID_STEPS`,
`PAR_POS_X`, `PAR_POS_Y`, `PAR_FLUX`, `PAR_SOM_FREQ`, `PAR_TRACK_LENGTH`,
`PAR_TRACK_SCALE`) likely share the same stopped-sequencer retrigger bug.
Only rotation was reported, so only rotation was fixed. Flagged for a
follow-up decision.

### Bug 2: sub-step rotation sometimes offsets the main steps

**Root cause, verified mathematically.** `euklid_rotatePattern()`
(`EuklidGenerator.c:304-378`) computes a signed sub-step delta and folds any
`|delta| > 7` into a whole main-step rotation:

```c
if (subSteps>7)
{
   mainSteps=(int)(mainSteps+subSteps/8);
   subSteps=subSteps%8;
}
else if (subSteps<-7) { ... }
...
if (!subSteps)
{
   pat_setMainSteps(patternNr, trackNr, 0x00);   // rotates the main-step bitmask
   for (i=0;i<length;i++) { ... }
}
```

This carry-into-mainstep logic is *correct* for a genuine multi-main-step
rotation request. The bug: `PAR_EUKLID_SUBSTEP_ROTATION`'s dial range was
`DTYPE_0B15` (0-15), so an ordinary dial movement whose delta happened to
exceed +-7 (e.g. jumping from 0 straight to 8, or any other multiple-of-8
net delta) triggered this fold and rotated the main-step on/off bitmask as
an unintended side effect of what the user intended as a pure sub-step edit.
This only fires when the delta happens to cross the threshold, matching the
report's "sometimes."

**Fix — matches the user's own proposed approach ("limit substep rotation to
0-7"), verified sufficient before implementing.** Traced the boundary case
algebraically: clamping the *value itself* to 0-7 bounds every possible
single-edit delta between two in-range values to exactly [-7, 7] inclusive,
which never reaches the `subSteps>7` / `subSteps<-7` thresholds -- confirmed
for the exact boundary deltas of +7 and -7 (the -7 case still normalizes to
a nonzero final `subSteps`, so the `if(!subSteps)` mainstep-bitmask rotation
never fires). This closes the mechanism completely, not just empirically.

**Implementation constraint discovered:** `PAR_EUKLID_ROTATION` (main-step
rotation, which legitimately needs the full 0-15 range) shares the exact
same `DTYPE_0B15` dtype as `PAR_EUKLID_SUBSTEP_ROTATION`, and `menu.h`'s
`Datatypes` enum is already at its documented hard ceiling of exactly 16
entries (0-15; the dtype is packed into the low 4 bits of a byte everywhere
it's read, e.g. `parameter_dtypes[parNr] & 0x0F`, and the enum itself carries
an explicit comment: *"we can only have 16 on this list the way things are
laid out"*). Adding a dedicated `DTYPE_0B7` was therefore not possible
without a larger encoding change. Implemented instead as a
`paramNr == PAR_EUKLID_SUBSTEP_ROTATION` special case inside the existing
`DTYPE_0B15` branch, at all three places that branch is handled in `menu.c`:
`menu_encoderChangeParameter()`, `menu_encoderChangeShiftParameter()`, and
`getDtypeValue()` (the pot/knob absolute-value path, not currently reachable
for this parameter but guarded defensively for consistency). Each site has
its own comment; the full WHY is written once at the first site and the
other two point back to it to avoid tripling the same explanation.
`PAR_EUKLID_ROTATION`'s real 0-15 ceiling is untouched at all three sites.

**Advice on whether 0-7 is expected to fully resolve this (asked directly by
the user): yes.** The fix was verified algebraically against the exact
normalization logic before implementation, not just applied speculatively.
Two adjacent, non-blocking items worth hardware-testing awareness, not
further code changes:
1. A track/song already rotated past 7 using the old unclamped 0-15 range
   will show its rotation value silently clamp down to 7 on its next edit
   (the displayed/edited value clamps; already-rotated pattern data is
   untouched until then). Expected fix behavior, not a new bug.
2. The clamp lives entirely in the AVR UI layer, the only current way to
   set this parameter. If sub-step rotation is ever exposed through MIDI
   NRPN or automation later, it would bypass this clamp; the same `& 0x07`
   mask could additionally be applied on the STM side in
   `FRONT_SEQ_EUKLID_SUBSTEP_ROTATION` (`frontPanelReceivingProtocol.c`)
   for defense-in-depth. Not implemented this session since no such
   alternate path exists today.

### Verification (both bugs)

- `make -C front/LxrAvr avr -j4` -- clean, zero warnings from the changed
  code (`menu.c`).
- `make firmware` -- succeeded, `firmware image/FIRMWARE.BIN` rebuilt again.
- `git diff --stat` -- confirms only `front/LxrAvr/Menu/menu.c` and the
  firmware image changed for this addendum; no STM32 source touched.
- **Not hardware-tested.** Recommended check: exercise both main-step and
  sub-step rotation across their full ranges, sequencer stopped and running,
  on a freshly built track and on a track loaded from a song with
  pre-existing rotation, confirming (a) no voice retrigger while stopped,
  (b) main-step LEDs never move during a pure sub-step rotation, (c) correct
  behavior at the 0/7 wrap boundary.

## End Of Session Block

```
DATE: 2026-08-20
SESSION GOAL: Investigate a reported step-probability bug (Song 75, Clap/Cym track ignores probability after load) via file forensics and full AVR/STM data-path tracing, document findings, and fix any confirmed defect found along the way.
COMPLETED: Wrote a 7-part investigation (PROBABILITY_INVESTIGATION.md) that ruled out transport, RNG/trigger logic, and Save-side stripping; confirmed step edits can land on a pattern other than the one actually playing via two structural mechanisms (per-track pattern following, temp-pattern scratch storage); found and fixed a standalone defect in avrCommsReceivingProtocol.c's SEQ_CHANGE_PAT ACK handler where the PATTERN_SETTINGS_PAGE branch (hold SHIFT while already on the PERF page -- not the separate EUKLID_PAGE Euclid generator page) silently dropped the AVR-to-STM shown-pattern resync instead of flushing it later as a stale comment claimed; verified the fix builds cleanly end to end. POST-CLOSEOUT ADDENDUM 1: the user then found the original Part 1 forensic scan had a filtering bug (active-steps-only) that hid real, non-default probability values in the file; re-scanning found the actual root cause (Part 8) -- fixed row-0 sub-step addressing in buttonHandler.c ignoring sub-step rotation -- which is a separate, more direct match for the original report and was NOT fixed this session, only documented, pending a UX decision. POST-CLOSEOUT ADDENDUM 2: the user then reported two further, separate pattern-rotation bugs and asked for fixes -- (1) every rotation encoder nudge retriggered the voice while stopped, caused by a redundant SEQ_SET_ACTIVE_TRACK resend before the rotation opcode coinciding with the "preview already-selected voice" gesture on STM; fixed by removing the redundant resend from the two Euclid rotation handlers only. (2) sub-step rotation could non-deterministically rotate the main-step bitmask too, caused by the sub-step rotation dial allowing 0-15 while euklid_rotatePattern() folds any delta >7 into a main-step rotation; fixed by clamping the parameter to 0-7 (the user's own proposed fix, verified algebraically sufficient before implementing) via a paramNr special case, since the dtype enum has no free slot for a dedicated 0-7 range. Both fixes build-verified.
VERIFIED ON HARDWARE: no. Build-verified only (make -C front/LxrAvr avr -j4 and make firmware both succeeded with no new warnings/errors, for all three fixes applied this session). Hardware verification checklists are written into PROBABILITY_INVESTIGATION.md Parts 7 and 9, and this log's Verification sections.

CHANGES THIS SESSION:
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`: SEQ_CHANGE_PAT ACK handler's PATTERN_SETTINGS_PAGE branch now calls menu_setShownPattern(patMsg) instead of assigning menu_shownPattern directly, so the STM's frontParser_shownPattern stays in sync even while that page is displayed; added a full explanatory comment block at the change site.
- `front/LxrAvr/Menu/menu.c`: (a) PAR_EUKLID_ROTATION and PAR_EUKLID_SUBSTEP_ROTATION handlers no longer resend a redundant SEQ_SET_ACTIVE_TRACK before their opcode, fixing the stopped-sequencer voice retrigger. (b) PAR_EUKLID_SUBSTEP_ROTATION is now clamped to 0-7 (instead of the shared DTYPE_0B15 dtype's normal 0-15) at all three value-clamp sites, fixing sub-step rotation from sometimes rotating the main-step bitmask. Both have full explanatory comment blocks at each change site.
- `firmware image/FIRMWARE.BIN`: rebuilt with all three fixes.
- `PROBABILITY_INVESTIGATION.md`: new root document; grew to 9 parts covering the original investigation, the Part 8 correction, and the Part 9 rotation-bug fixes.
- `knowledge_files/log_archive/000_SESSION_INDEX.md`: added the missing terse Session 035 row and the Session 036 row (updated again to reflect the full scope of this session).
- `knowledge_files/log_archive/036_SESSION_HANDOFF_LOG.md`: this handoff log, including two post-closeout addenda.
- `MEMORY.md`: session closeout notes, updated across the session as findings evolved.

KNOWN ISSUES INTRODUCED: None confirmed. One narrow interaction to watch on hardware from the PATTERN_SETTINGS_PAGE fix: a redundant same-pattern pattern-change ACK arriving while on that page can now reach the STM and could trigger a harmless seq_realign() that previously silently no-opped there (see PROBABILITY_INVESTIGATION.md Part 6).
KNOWN ISSUES RESOLVED: (1) Step-parameter edits made while the front panel was on the PATTERN_SETTINGS_PAGE rotation-display page around a pattern-change ACK no longer get silently stranded on a stale or temp pattern. (2) Rotation encoder nudges (PAR_EUKLID_ROTATION, PAR_EUKLID_SUBSTEP_ROTATION) no longer retrigger the voice while the sequencer is stopped. (3) Sub-step rotation no longer has a chance of rotating the main-step on/off bitmask as an unintended side effect.
KNOWN ISSUES STILL OPEN (found this session, not yet fixed): The Part 8 root cause of the original probability report -- fixed row-0 sub-step addressing in buttonHandler.c not accounting for sub-step rotation -- is documented but not fixed, pending a design decision. The identical redundant-SEQ_SET_ACTIVE_TRACK-resend pattern behind rotation Bug 1 also exists on PAR_EUKLID_LENGTH, PAR_EUKLID_STEPS, PAR_POS_X, PAR_POS_Y, PAR_FLUX, PAR_SOM_FREQ, PAR_TRACK_LENGTH, and PAR_TRACK_SCALE and was not fixed (only rotation was reported).

NEXT SESSION RECOMMENDED GOAL: Flash this session's firmware and hardware-test all three fixes (PATTERN_SETTINGS_PAGE shown-pattern sync, rotation retrigger-while-stopped, sub-step rotation main-step offset) per the checklists in PROBABILITY_INVESTIGATION.md Parts 7 and 9. Then get a decision on the Part 8 fix approach (dynamic active-row resolution vs. a UX/LED indicator) and implement it across buttonHandler_selectActiveStep(), buttonHandler_setRemoveStep(), and the SELECT_MODE_PAT_GEN step-select variant -- this is the actual root cause of the original Song-75 report and remains unfixed.
BLOCKERS: Part 8 needs a design decision before implementation (see PROBABILITY_INVESTIGATION.md Part 8's "Open question"). Hardware confirmation of all three fixes applied this session is required before considering any of them closed. The 8 other parameter handlers sharing rotation Bug 1's redundant-resend pattern are an open question: fix them too, or leave as-is since only rotation was reported.

CRITICAL REMINDERS FOR NEXT SESSION:
- Do not remove the `if(menu_activePage != PATTERN_SETTINGS_PAGE)` / `else` split in `avrCommsReceivingProtocol.c`'s `SEQ_CHANGE_PAT` handler outright -- the `if` branch's LED refresh calls must stay gated off PATTERN_SETTINGS_PAGE, or its rotation-indicator LED display will be visibly clobbered by ordinary pattern-change traffic.
- `PATTERN_SETTINGS_PAGE` (hold SHIFT while already on PERF) and `EUKLID_PAGE` (hold SHIFT then press PERF, `menu_enterPatgenMode()`) are two separate pages -- do not conflate them; `MEMORY.md` already warns about this exact mix-up and this session's own draft made it once before catching it.
- `menu.h`'s `Datatypes` enum is hard-capped at 16 entries (4-bit packed, `parameter_dtypes[...] & 0x0F` everywhere) and is already full. Do not add a new `DTYPE_*` value without first freeing a slot (e.g. `DTYPE_AUTOM_TARGET` is legacy-macro-only and may be reclaimable, but that was not investigated or touched this session) -- use a `paramNr`-based special case within an existing dtype instead, as done for `PAR_EUKLID_SUBSTEP_ROTATION`.
- `PAR_EUKLID_SUBSTEP_ROTATION` must stay clamped to 0-7 at all three `menu.c` sites (`menu_encoderChangeParameter()`, `menu_encoderChangeShiftParameter()`, `getDtypeValue()`); `PAR_EUKLID_ROTATION` correctly keeps 0-15. If a fourth path to set this parameter is ever added (MIDI, automation), it needs the same clamp or an equivalent STM-side `& 0x07` mask in `FRONT_SEQ_EUKLID_SUBSTEP_ROTATION`.
- Do not resend `SEQ_SET_ACTIVE_TRACK` redundantly before a parameter opcode when the track hasn't changed and the sequencer might be stopped -- STM's `FRONT_SEQ_SET_ACTIVE_TRACK` treats same-track-while-stopped as an intentional voice-preview gesture and will trigger it.
- The PATTERN_SETTINGS_PAGE fix is NOT confirmed to be the cause of the original Song-75 report. The actual verified root cause is Part 8 (fixed row-0 sub-step addressing vs. sub-step rotation) -- read PROBABILITY_INVESTIGATION.md Part 8 before assuming this bug is closed.
- Before trusting any "no probability/data is set anywhere" forensic conclusion again, scan ALL steps for a field, not just STEP_ACTIVE_MASK-active ones -- the Part 1 scan's active-only filter is exactly what hid the real evidence here.
- `menu_setShownPattern()` is the only sanctioned way to update `menu_shownPattern` and keep the STM's `frontParser_shownPattern` in sync; do not reintroduce a direct local assignment anywhere in this ACK path.
- Mechanism A (per-track pattern following) and the live-streaming-window variant of Mechanism B (documented in `PROBABILITY_INVESTIGATION.md` Part 3) are still open, lower-priority hardening items, not fixed by this session's change.
- `PROBABILITY_INVESTIGATION.md` is the durable technical record for this bug; read it before touching `frontParser_shownPattern`, `seq_perTrackActivePattern`, or the `SEQ_CHANGE_PAT`/`SEQ_SET_SHOWN_PATTERN` opcodes again.
```
