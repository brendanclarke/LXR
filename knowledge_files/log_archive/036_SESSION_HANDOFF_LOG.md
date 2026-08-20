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

## End Of Session Block

```
DATE: 2026-08-20
SESSION GOAL: Investigate a reported step-probability bug (Song 75, Clap/Cym track ignores probability after load) via file forensics and full AVR/STM data-path tracing, document findings, and fix any confirmed defect found along the way.
COMPLETED: Wrote a 7-part investigation (PROBABILITY_INVESTIGATION.md) that ruled out transport, RNG/trigger logic, and Save-side stripping; confirmed step edits can land on a pattern other than the one actually playing via two structural mechanisms (per-track pattern following, temp-pattern scratch storage); found and fixed a standalone defect in avrCommsReceivingProtocol.c's SEQ_CHANGE_PAT ACK handler where the PATTERN_SETTINGS_PAGE branch (hold SHIFT while already on the PERF page -- not the separate EUKLID_PAGE Euclid generator page) silently dropped the AVR-to-STM shown-pattern resync instead of flushing it later as a stale comment claimed; verified the fix builds cleanly end to end.
VERIFIED ON HARDWARE: no. Build-verified only (make -C front/LxrAvr avr -j4 and make firmware both succeeded with no new warnings/errors). Hardware verification checklist is written into PROBABILITY_INVESTIGATION.md Part 7 and this log's Verification section.

CHANGES THIS SESSION:
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`: SEQ_CHANGE_PAT ACK handler's PATTERN_SETTINGS_PAGE branch now calls menu_setShownPattern(patMsg) instead of assigning menu_shownPattern directly, so the STM's frontParser_shownPattern stays in sync even while that page is displayed; added a full explanatory comment block at the change site.
- `firmware image/FIRMWARE.BIN`: rebuilt with the fix.
- `PROBABILITY_INVESTIGATION.md`: new root document, full 7-part investigation and fix record.
- `knowledge_files/log_archive/000_SESSION_INDEX.md`: added the missing terse Session 035 row and the new Session 036 row.
- `knowledge_files/log_archive/036_SESSION_HANDOFF_LOG.md`: this handoff log.
- `MEMORY.md`: session closeout notes.

KNOWN ISSUES INTRODUCED: None confirmed. One narrow interaction to watch on hardware: a redundant same-pattern pattern-change ACK arriving while on PATTERN_SETTINGS_PAGE can now reach the STM and could trigger a harmless seq_realign() that previously silently no-opped there (see PROBABILITY_INVESTIGATION.md Part 6).
KNOWN ISSUES RESOLVED: Step-parameter edits (probability, volume, note, and the two automation-value opcodes) made while the front panel was on the PATTERN_SETTINGS_PAGE rotation-display page around a pattern-change ACK (including background-load temp/normal boundary crossings) no longer get silently stranded on a stale or temp pattern that is invisible to both playback and Save.

NEXT SESSION RECOMMENDED GOAL: Flash the fixed firmware and run the hardware verification checklist -- reproduce the original Song 75 report end to end, confirm the PATTERN_SETTINGS_PAGE LED display is unaffected, and watch for the narrow seq_realign() edge case.
BLOCKERS: No build blocker. Hardware confirmation of both the original bug's resolution and the absence of LED/realign side effects is required before considering this closed.

CRITICAL REMINDERS FOR NEXT SESSION:
- Do not remove the `if(menu_activePage != PATTERN_SETTINGS_PAGE)` / `else` split in `avrCommsReceivingProtocol.c`'s `SEQ_CHANGE_PAT` handler outright -- the `if` branch's LED refresh calls must stay gated off PATTERN_SETTINGS_PAGE, or its rotation-indicator LED display will be visibly clobbered by ordinary pattern-change traffic.
- `PATTERN_SETTINGS_PAGE` (hold SHIFT while already on PERF) and `EUKLID_PAGE` (hold SHIFT then press PERF, `menu_enterPatgenMode()`) are two separate pages -- do not conflate them; `MEMORY.md` already warns about this exact mix-up and this session's own draft made it once before catching it.
- `menu_setShownPattern()` is the only sanctioned way to update `menu_shownPattern` and keep the STM's `frontParser_shownPattern` in sync; do not reintroduce a direct local assignment anywhere in this ACK path.
- Mechanism A (per-track pattern following) and the live-streaming-window variant of Mechanism B (documented in `PROBABILITY_INVESTIGATION.md` Part 3) are still open, lower-priority hardening items, not fixed by this session's change.
- `PROBABILITY_INVESTIGATION.md` is the durable technical record for this bug; read it before touching `frontParser_shownPattern`, `seq_perTrackActivePattern`, or the `SEQ_CHANGE_PAT`/`SEQ_SET_SHOWN_PATTERN` opcodes again.
```
