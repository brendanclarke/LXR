# PATROT_AUTOMATION_FIXUP_AUDIT.md

Implementation plan for two behaviour corrections:

1. **Pattern Rotation Destructive Commit on PAGE EXIT** — currently rotation is
   always non-destructive (temp-backup only). The plan makes rotation
   destructive when the user navigates away to a different MODE page and resets
   the rotation counters to 0.
2. **Step Automation Velocity-Gated Release** — currently step automation resets
   the parameter at every active step (velocity >= 1). The plan restricts the
   release trigger to active steps whose STEP_VOLUME_MASK portion is > 0
   (i.e. velocity > 0), matching the actual trigger semantics.

---

## Feature 1 — Pattern Rotation Destructive Commit on Page Exit

### Background / Existing Mechanism

The SHIFT+PERF page (`SELECT_MODE_PAT_GEN` / `EUKLID_PAGE`) uses a
**snapshot-and-restore** model:

- **`frontParser_beginEuklidSnapshotVisit()`** — called when the user first
  enters SHIFT+PERF. Records the visited pattern, resets
  `frontParser_euklidSnapshotTrackMask` to 0.
- **`frontParser_snapshotEuklidTrackIfNeeded(trackNr)`** — called before every
  rotation/length/step edit. On the *first* edit to a track during a visit,
  saves the current normal-pattern track state into `SEQ_TMP_PATTERN` slot, and
  saves the current Euclid cache values (`euklid_getSteps`, `euklid_getRotation`,
  `euklid_getSubStepRotation`) into per-track snapshot arrays. Sets the track's
  bit in `frontParser_euklidSnapshotTrackMask`.
- **`frontParser_endEuklidSnapshotVisit()`** — called when the user leaves
  SHIFT+PERF (navigates to any other mode page). Currently it just clears the
  snapshot state without doing any additional work. The edits are already live
  in normal-pattern storage, so exit is currently a "commit silently" path.
- **`frontParser_restoreEuklidSnapshotTracks()`** — called when the user
  re-presses SHIFT+PERF (restore gesture). Copies the backed-up temp track data
  back to normal, restores Euclid cache values, and refreshes the display.

The rotation values (`euklid_rotation[track]`, `euklid_subStepRotation[track]`)
are stored in the `EuklidGenerator` module's static per-track arrays. These are
the **caches the front-panel uses to display and compute incremental rotation
deltas**. When rotation is set to a non-zero value the underlying pattern in
`pat_patternSet` has already been physically rotated — the rotation value in the
cache records how many steps have been accumulated so the next increment knows
where to start.

**The problem**: When the user exits SHIFT+PERF, the rotation amounts stay in
`euklid_rotation[]` / `euklid_subStepRotation[]`. The next SHIFT+PERF visit
calls `frontParser_beginEuklidSnapshotVisit()` which clears the mask but does
**not** reset the rotation caches. This means:

- The display on re-entry shows the old rotation values.
- The existing snapshot captured before the first edit of the *previous* visit
  (if restore was not used) will have those stale values too.
- The user's intent is that "leaving the page = commit, rotation resets to 0 so
  edits start fresh".

### Behaviour Specification

| Exit trigger | Expected effect |
|---|---|
| User navigates to a different MODE page (VOICE, STEP, PERF, LOAD_SAVE, MENU) | **Destructive commit**: rotation values reset to 0 in `euklid_rotation[]` / `euklid_subStepRotation[]` for every touched track. Snapshot state is cleared. AVR display sees rotation = 0. |
| User changes the **track** (voice button) while still on SHIFT+PERF | **No commit**: still inside the same visit; snapshot persists, no rotation reset. |
| User re-presses SHIFT+PERF (restore gesture) | **Restore**: all touched tracks rolled back from temp, rotation caches set to saved pre-edit values, snapshot cleared. |
| Pattern switch while still in SHIFT+PERF | Out of scope (existing clear-rotation-on-pattern-change path already handles this). |

---

### Code Change 1A — `frontParser_endEuklidSnapshotVisit()` in `frontPanelReceivingProtocol.c`

**File**: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
**Function**: `frontParser_endEuklidSnapshotVisit()` (static, lines 117-122)

#### Current code

```c
static void frontParser_endEuklidSnapshotVisit(void)
{
   frontParser_euklidSnapshotVisitActive = 0;
   frontParser_euklidSnapshotTrackMask = 0;
   frontParser_euklidSnapshotPattern = 0;
}
```

#### Required change

Before clearing state, iterate over every track whose bit is set in
`frontParser_euklidSnapshotTrackMask` and call `euklid_clearTrackRotation(track)`.
This resets `euklid_rotation[track]` and `euklid_subStepRotation[track]` to 0
in the EuklidGenerator cache. After clearing rotations for all touched tracks,
call `frontPanelSending_sendEuklidParamsReply(frontParser_activeTrack)` so the
AVR display updates to 0 for the currently viewed track.

#### Why this change must exist

The commit path (`END_VISIT`) is the only place the STM is told the user is done
with the page. The rotation values in the Euclid cache are what the front panel
reads as the "current rotation" on next entry. Leaving them at their
post-rotation values means the next visit starts with stale rotation counters
rather than from 0, causing both display artefacts and mis-computation of the
next incremental rotation delta.

#### Replacement code

```c
static void frontParser_endEuklidSnapshotVisit(void)
{
   uint8_t track;
   /* On a committed exit (not a restore), clear the accumulated rotation
      counters for every track that was edited during this visit. The pattern
      data is already live in normal storage, so the rotation amounts are no
      longer meaningful as incremental deltas; they must be 0 for the next
      visit to start fresh. */
   for(track = 0; track < NUM_TRACKS; track++)
   {
      if(frontParser_euklidSnapshotTrackMask & (uint8_t)(0x01u << track))
      {
         euklid_clearTrackRotation(track);
      }
   }
   /* After resetting the rotation caches, send a params reply so the AVR
      display reflects the 0 rotation for the active track. Other tracks
      refresh on next track selection inside SHIFT+PERF. Only send if at
      least one track was actually edited during this visit. */
   if(frontParser_euklidSnapshotTrackMask)
      frontPanelSending_sendEuklidParamsReply(frontParser_activeTrack);

   frontParser_euklidSnapshotVisitActive = 0;
   frontParser_euklidSnapshotTrackMask = 0;
   frontParser_euklidSnapshotPattern = 0;
}
```

#### Inputs

- `frontParser_euklidSnapshotTrackMask` — bitmask recording which tracks were
  touched during the current visit (set by `frontParser_snapshotEuklidTrackIfNeeded`).
- `frontParser_activeTrack` — which track the AVR is currently viewing (used to
  address the params reply).

#### Outputs

- `euklid_rotation[track]` and `euklid_subStepRotation[track]` set to 0 for
  touched tracks (via `euklid_clearTrackRotation()`).
- Updated `SEQ_EUKLID_ROTATION` / `SEQ_EUKLID_SUBSTEP_ROTATION` packets sent to
  AVR via `frontPanelSending_sendEuklidParamsReply()`.

#### Clients / callers

`FRONT_SEQ_EUKLID_RESET` handler in `frontPanelReceivingProtocol.c` — both the
`FRONT_SEQ_EUKLID_RESET_CLEAR_ROTATION` and `FRONT_SEQ_EUKLID_RESET_END_VISIT`
sub-cases call this function.

#### Confederates

- `EuklidGenerator.c` / `euklid_clearTrackRotation()` — already exists and does
  exactly the right thing (resets both rotation fields to 0).
- `frontPanelSendingProtocol.c` / `frontPanelSending_sendEuklidParamsReply()` —
  already used in `frontParser_restoreEuklidSnapshotTracks()`, so this is the
  symmetric approach for the commit path.

#### Side effects

- None to pattern data (already live in normal storage).
- None to the snapshot temp data (orphaned by clearing the mask; it is now
  unreachable and will be overwritten on the next visit's first snapshot).

#### Note on FRONT_SEQ_EUKLID_RESET_CLEAR_ROTATION

That sub-case also calls `frontParser_endEuklidSnapshotVisit()`. With this
change a `CLEAR_ROTATION` command will also call `euklid_clearTrackRotation` for
touched tracks and send a params reply. This is correct and consistent — the
CLEAR_ROTATION path is an explicit all-clear that should also reset the cache.

---

### Code Change 1B — AVR-side display handled by STM reply (no separate AVR change)

The AVR keeps a local copy of the rotation amount in `parameter_values[]` used
to drive the menu display. Change 1A handles this by having the STM send
`frontPanelSending_sendEuklidParamsReply()` immediately after clearing rotation
caches. The AVR `avrCommsReceivingProtocol.c` handler for
`SEQ_EUKLID_ROTATION` / `SEQ_EUKLID_SUBSTEP_ROTATION` updates `parameter_values[]`
and repaints automatically.

**No separate AVR-side code change is required.**

---

### Code Change 1C — `euklid_clearTrackRotation()` confirmed correct (no code change)

**File**: `mainboard/LxrStm32/src/Sequencer/Pattern/EuklidGenerator.c` (lines 421-426)

```c
void euklid_clearTrackRotation(uint8_t trackNr)
{
   euklid_rotation[trackNr]=0;
   euklid_subStepRotation[trackNr]=0;
}
```

No changes required. Since `euklid_setRotation()` computes rotation as a delta
from the cached value (`value - euklid_rotation[trackNr]`), zeroing the cache
means the next call to `euklid_setRotation(..., N)` will rotate by N steps from
the current committed position, which is the correct fresh-start behaviour.

---

### Explicit Non-Change — Track change inside SHIFT+PERF page

The user explicitly stated that **changing the track operated on** inside
SHIFT+PERF must **not** commit or reset. The `SEQ_SET_ACTIVE_TRACK` /
`SEQ_REQUEST_EUKLID_PARAMS` path that switches which track is being displayed
does not trigger `frontParser_endEuklidSnapshotVisit()`, so no change is needed
there. This is already correct: the mode never changes and `wasPatgenMode` only
transitions to false when `nextSelectButtonMode != SELECT_MODE_PAT_GEN`.

---

## Feature 2 — Step Automation Velocity-Gated Release

### Background / Existing Mechanism

Step automation is a one-step override. The sequencer's `seq_nextStep()` loop
(around lines 1037-1047 of `sequencer.c`) contains:

```c
if(activeScaledStep
   && seq_liveMainStepActive(i, stepAcPtr[i]/8)
   && seq_liveStepActive(i, stepAcPtr[i]))
{
   /* Step automation is a one-step override. Release the previous
      override as soon as this track reaches its next active step, before
      probability, mute, roll, or note-trigger decisions can skip the
      normal trigger path. */
   seq_releaseGlobalDecimationAutomationForOwner(i);
   seq_releasePendingAutomationForTrack(i);
}
```

`seq_liveStepActive()` is defined as:

```c
static uint8_t seq_liveStepActive(uint8_t track, uint8_t step)
{
   return (seq_liveStepForTrack(track, step)->volume & STEP_ACTIVE_MASK) > 0;
}
```

`STEP_ACTIVE_MASK = 0x80`. A step is "active" if its bit 7 is set. **A step can
be active with velocity = 0** (`volume = 0x80`). A velocity-0 active step is a
"ghost step" or a zero-velocity trigger that is commonly used for control-only
purposes.

**The problem**: The current release gate fires on **any** active step, including
velocity-0 steps. If a user places a velocity-0 active step between two non-zero
velocity steps:

- The automation override set by the first non-zero step is released on the
  velocity-0 step.
- For the interval between the velocity-0 step and the next non-zero step the
  parameter has returned to baseline unexpectedly.

The user's requirement: "reset that parameter back to the default value when
another **active step with velocity greater than 0** triggers, or when the
parameter is automated again."

---

### Behaviour Specification

| Condition | Release automation? |
|---|---|
| Active step (`STEP_ACTIVE_MASK` set), velocity > 0 (`STEP_VOLUME_MASK` > 0) | **YES** — real trigger, release pending override. |
| Active step (`STEP_ACTIVE_MASK` set), velocity = 0 | **NO** — ghost step, preserve automation override. |
| Inactive step | **NO** — no change (already not releasing). |
| Automation re-applied by the next step (any velocity) | **YES** — `seq_applyAutomationLane()` already calls `autoNode_setDestination()` which calls `autoNode_release()` before setting the new one. This path is unchanged. |
| `seq_releaseAllAutomation()` (stop, pattern change) | **YES** — already unconditional, no change needed. |

---

### Code Change 2A — Add velocity gate to automation release in `seq_nextStep()`

**File**: `mainboard/LxrStm32/src/Sequencer/sequencer.c`
**Function**: `seq_nextStep()` — per-track loop, around lines 1037-1047

#### Current code

```c
if(activeScaledStep
   && seq_liveMainStepActive(i, stepAcPtr[i]/8)
   && seq_liveStepActive(i, stepAcPtr[i]))
{
   /* Step automation is a one-step override. Release the previous
      override as soon as this track reaches its next active step, before
      probability, mute, roll, or note-trigger decisions can skip the
      normal trigger path. */
   seq_releaseGlobalDecimationAutomationForOwner(i);
   seq_releasePendingAutomationForTrack(i);
}
```

#### Required change

Add a velocity test so the release only fires when the step velocity is > 0.
Hoist a `Step*` pointer before the guard to avoid calling `seq_liveStepForTrack()`
twice (once already implicit in `seq_liveStepActive()` and once for the velocity
check).

#### Replacement code

```c
{
   /* Hoist the step pointer once to avoid a redundant seq_liveStepForTrack()
      call when both the active-step and velocity checks need the same step. */
   Step *stepRelease = seq_liveStepForTrack(i, stepAcPtr[i]);
   if(activeScaledStep
      && seq_liveMainStepActive(i, stepAcPtr[i]/8)
      && (stepRelease->volume & STEP_ACTIVE_MASK)
      && (stepRelease->volume & STEP_VOLUME_MASK) > 0)
   {
      /* Step automation is a one-step override. Release the previous override
         as soon as this track reaches its next active step with non-zero
         velocity. Zero-velocity active steps (ghost steps) do not release
         the automation, so the overridden parameter value persists until a
         real trigger arrives. */
      seq_releaseGlobalDecimationAutomationForOwner(i);
      seq_releasePendingAutomationForTrack(i);
   }
}
```

#### Why this change must exist

A velocity-0 active step is not a real drum hit — it may be a ratchet
placeholder, a muted accent, or a silent program event. Releasing automation on
it breaks the user's expected behaviour where the overridden parameter continues
until the next audible trigger.

The additional bitmask test `(stepRelease->volume & STEP_VOLUME_MASK) > 0`
short-circuits the release when the step velocity byte (low 7 bits of `volume`)
is 0. `STEP_VOLUME_MASK = 0x7f`, which extracts exactly the velocity component
already isolated for `seq_triggerVoice()`.

#### Inputs

- `activeScaledStep` — already computed per-track.
- `seq_liveMainStepActive(i, stepAcPtr[i]/8)` — already in the guard.
- `stepRelease->volume & STEP_ACTIVE_MASK` — replaces the `seq_liveStepActive()`
  call; tests `STEP_ACTIVE_MASK` directly on the hoisted pointer.
- `stepRelease->volume & STEP_VOLUME_MASK` — **new**: extracts velocity from the
  same step pointer.

#### Outputs

`seq_releasePendingAutomationForTrack(i)` and
`seq_releaseGlobalDecimationAutomationForOwner(i)` are called **only** when
velocity > 0.

#### Clients / accessors of the changed condition

- `seq_releasePendingAutomationForTrack(track)` — calls
  `seq_releaseAutomationLane()` for each lane if either
  `seq_automationPendingRelease` or `seq_voiceMorphAutomationPendingRelease` is
  set.
- `seq_releaseGlobalDecimationAutomationForOwner(i)` — releases the global
  decimation automation if this track is the owner.
- No other callers are affected; this is an isolated per-track guard.

#### Confederates

- `PatternData.h` — defines `STEP_ACTIVE_MASK = 0x80` and `STEP_VOLUME_MASK = 0x7f`.
- `seq_liveStepForTrack()` — returns `Step*`, called once (hoisted) instead of
  twice.
- `autoNode_release()` / `autoNode_setDestination()` — not called here; they are
  called deeper via `seq_releaseAutomationLane()`. No change needed there.

---

### Code Change 2B — `seq_liveStepActive()` helper unchanged

**File**: `mainboard/LxrStm32/src/Sequencer/sequencer.c` (lines 610-612)

```c
static uint8_t seq_liveStepActive(uint8_t track, uint8_t step)
{
   return (seq_liveStepForTrack(track, step)->volume & STEP_ACTIVE_MASK) > 0;
}
```

**No change required.** `seq_liveStepActive()` correctly tests only the ACTIVE
bit. Keeping it unchanged preserves its meaning everywhere else it is used
(trigger processing at line 1100, live-record at line 1520, etc.) — those
callers still want the full "step is active" test, not a velocity-gated version.
The automation release site is now rewritten to use a hoisted pointer directly
instead of calling `seq_liveStepActive()`.

---

### Code Change 2C — `autoNode_setDestination()` re-application path unchanged

**File**: `mainboard/LxrStm32/src/DSPAudio/automationNode.c` (lines 82-89)

```c
void autoNode_setDestination(AutomationNode* node, uint16_t dest)
{
   autoNode_release(node);
   node->destination = (dest == 0) ? NO_AUTOMATION : dest;
}
```

**No change required.** When a new step's automation re-applies (via
`seq_applyAutomationLane()` -> `autoNode_setDestination()`), `autoNode_release()`
is called first, which restores baseline **before** the new override is set.
Even a zero-velocity step that carries its own automation data would correctly
release the previous override and apply the new one. This is the "when the
parameter is automated again" path from the user's spec.

---

## Summary of All Code Changes

| # | File | Location | Change |
|---|---|---|---|
| 1A | `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c` | `frontParser_endEuklidSnapshotVisit()` | Before clearing state: iterate touched track mask; call `euklid_clearTrackRotation(track)` for each touched track; then call `frontPanelSending_sendEuklidParamsReply(frontParser_activeTrack)` if any tracks were touched. |
| 2A | `mainboard/LxrStm32/src/Sequencer/sequencer.c` | `seq_nextStep()` per-track loop (~line 1037) | Hoist `Step*` pointer; replace `seq_liveStepActive()` with direct bitmask check; add `(stepRelease->volume & STEP_VOLUME_MASK) > 0` to the automation-release guard. |

No header changes, no new opcodes, no separate AVR-side changes required.

---

## Verification Plan

### Feature 1 — Destructive rotation commit

1. Enter SHIFT+PERF. Edit rotation on at least two tracks (e.g. Drum1 = +2,
   Drum2 = +3).
2. Press VOICE to navigate away.
3. Re-enter SHIFT+PERF. Confirm rotation display reads 0/0 for both tracks.
4. Confirm the pattern plays with the post-rotation beat arrangement (the
   rotation is preserved as physical pattern data, not rolled back).
5. Repeat with RESTORE gesture: enter SHIFT+PERF, rotate, then re-press
   SHIFT+PERF. Confirm tracks roll back to pre-rotation content and rotation
   display shows the original 0.
6. Confirm that switching voice buttons (track) inside SHIFT+PERF does not
   commit or reset rotation.

### Feature 2 — Velocity-gated automation release

1. Create a pattern with: step A (velocity > 0, param1 automated to X),
   step B (velocity = 0, active, no automation), step C (velocity > 0,
   no automation).
2. Verify that the automated parameter holds value X from step A through
   step B until step C triggers.
3. Verify that step C (velocity > 0) releases the override and restores
   the baseline.
4. Verify that a step with its own automation on the same destination
   (regardless of velocity) still correctly releases the old override and
   applies the new value (via the `autoNode_setDestination` release path).
5. Verify that stop, pattern change, and lane-target-change still release
   automation immediately regardless of velocity (those paths call
   `seq_releaseAllAutomation()` or `seq_releaseAutomationLane()` directly,
   which are not gated by this block).

---

## Known Constraints and Risks

- **`frontParser_euklidSnapshotTrackMask` scope**: it is `static` to
  `frontPanelReceivingProtocol.c` and cleared only by
  `frontParser_endEuklidSnapshotVisit()` and
  `frontParser_restoreEuklidSnapshotTracks()`. The new loop in change 1A must
  come **before** the `frontParser_euklidSnapshotTrackMask = 0` assignment,
  not after it.
- **Display refresh for multiple touched tracks**: change 1A only sends a reply
  for `frontParser_activeTrack`. If the user edited multiple tracks and then
  exits, only the active track's display refreshes immediately. Other tracks
  refresh on next selection. This is an acceptable cosmetic edge case — the
  rotation in EuklidGenerator cache is correct for all touched tracks.
- **Step pointer deduplication (2A)**: the hoisted-pointer approach eliminates
  the double `seq_liveStepForTrack()` call. The automation release site must
  NOT call `seq_liveStepActive()` after the hoist (to avoid a second pointer
  lookup) — hence the direct bitmask checks on `stepRelease->volume` shown above.
- **Global decimation automation (2A)**: `seq_releaseGlobalDecimationAutomationForOwner(i)`
  is in the same velocity-gated block. This is correct — a velocity-0 step
  should not prematurely release global decimation either.
- **Voice-morph automation (2A)**: `seq_voiceMorphAutomationPendingRelease` is
  released via `seq_releasePendingAutomationForTrack()`, which is in the same
  velocity-gated block. Voice-morph overrides should also persist through
  velocity-0 steps.
- **`FRONT_SEQ_EUKLID_RESET_CLEAR_ROTATION` sub-case (1A)**: that sub-case also
  calls `frontParser_endEuklidSnapshotVisit()`. With this change a
  `CLEAR_ROTATION` command will also call `euklid_clearTrackRotation` for touched
  tracks and send a params reply. This is correct and consistent.

---

## Implementation Status

**Date applied**: 2026-07-07

### Change 1A — DONE

**File**: `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
**Function**: `frontParser_endEuklidSnapshotVisit()`

`frontParser_endEuklidSnapshotVisit()` was expanded from a 3-line state clear
into a structured exit sequence:

1. Loop over all tracks; for each track whose bit is set in
   `frontParser_euklidSnapshotTrackMask`, call `euklid_clearTrackRotation(track)`
   to zero `euklid_rotation[track]` and `euklid_subStepRotation[track]`.
2. If any tracks were touched (`frontParser_euklidSnapshotTrackMask != 0`), call
   `frontPanelSending_sendEuklidParamsReply(frontParser_activeTrack)` so the AVR
   display immediately reflects the reset rotation values for the currently
   viewed track.
3. Clear `frontParser_euklidSnapshotVisitActive`, `frontParser_euklidSnapshotTrackMask`,
   and `frontParser_euklidSnapshotPattern` as before.

The mask loop executes before the mask is zeroed (constraint from plan respected).
The reply is guarded by `if(frontParser_euklidSnapshotTrackMask)` to suppress
spurious UART traffic on no-op visits.

### Change 2A — DONE

**File**: `mainboard/LxrStm32/src/Sequencer/sequencer.c`
**Function**: `seq_nextStep()` per-track loop (~line 1037 pre-edit)

The automation-release `if` block was restructured:

1. A `Step *stepRelease = seq_liveStepForTrack(i, stepAcPtr[i])` pointer is
   hoisted into a bare compound block `{ }` immediately before the `if`, so
   `seq_liveStepForTrack()` is called exactly once (down from two calls: one
   implicit in `seq_liveStepActive()` and one that would have been needed for
   the velocity test).
2. `seq_liveStepActive(i, stepAcPtr[i])` is replaced with the direct bitmask
   check `(stepRelease->volume & STEP_ACTIVE_MASK)` on the hoisted pointer.
3. A new fourth condition `(stepRelease->volume & STEP_VOLUME_MASK) > 0` gates
   the release on non-zero velocity.  Zero-velocity active steps (ghost steps)
   no longer trigger automation release.
4. The block comment was updated to explain the ghost-step rationale and to
   clarify that the unconditional release paths (stop, pattern-change,
   lane-target change/clear) bypass this block entirely and are unchanged.

`seq_liveStepActive()` itself is unchanged — it continues to be used at the
trigger-processing site (line ~1100) and the live-record site (line ~1520)
where the full active-step test (without a velocity gate) is still correct.
