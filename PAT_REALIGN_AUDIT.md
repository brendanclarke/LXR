# Pattern Realign Audit

## Goal

Replace the current "reset Euclid rotation in-place" idea with a track-pattern snapshot workflow built on the existing temporary pattern storage:

1. While the user is on the `SHIFT + PERF` page (`EUKLID_PAGE` / patget-rotation page), the first adjustment to any of the four parameters for a given track:
   - Euclid steps
   - Euclid length
   - Euclid main-step rotation
   - Euclid sub-step rotation

   must first copy that track's current normal pattern data into the temporary pattern storage, before applying the edit.

2. After that first copy, additional edits to the same track during the same page visit must not copy again.

3. If the user leaves the page, the snapshot is considered committed/consumed:
   - returning to the page later must allow the next edit to snapshot again.

4. If the user re-selects `SHIFT + PERF` again without leaving the page, restore every track that was snapshotted during that page visit:
   - copy that track's temporary pattern data back into the normal pattern the user is editing.

This makes the temporary pattern storage act as a per-page-entry "pre-edit track backup" for Euclid/rotation work.

## Behavioral Model

### Entry / edit / reselect lifecycle

One page visit to `SHIFT + PERF` should behave like this:

1. User enters `SHIFT + PERF`.
2. No track has been snapshotted yet for this visit.
3. First edit on track `N`:
   - copy normal track `N` to temp track `N`
   - mark track `N` as "snapshotted for this visit"
   - apply the requested Euclid/rotation change to the normal track
4. More edits on track `N` during the same visit:
   - do not copy again
   - just keep applying edits to the normal track
5. First edit on track `M` during the same visit:
   - same snapshot-once behavior for track `M`
6. User presses `SHIFT + PERF` again without leaving:
   - restore every track marked "snapshotted for this visit" from temp back into normal
   - clear the per-visit snapshot markers
   - remain on the same page
7. User leaves the page normally:
   - clear the per-visit snapshot markers
   - do not auto-restore

### Scope of the snapshot

The snapshot should be track-local, not whole-pattern:

1. copy the full track pattern payload
2. copy the track main-step mask
3. copy the track length/scale/rotation cell

This should use the existing `temporary` pattern storage track slots so the restore path is just "copy temp track back to the normal pattern track."

## Implementation Plan

### 1. Add explicit per-page-visit tracking for Euclid temp snapshots

**Files to change**

1. `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
2. `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h` only if shared declarations become necessary

**What to add**

Add STM-owned state that tracks:

1. whether the front panel is currently in the active `SHIFT + PERF` visit window
2. which tracks have already been snapshotted during that visit

The simplest form is:

1. one boolean like `frontParser_euklidSnapshotVisitActive`
2. one 7-bit mask like `frontParser_euklidSnapshotTrackMask`

**Why this must exist**

The firmware needs a precise way to distinguish:

1. "first edit to this track during this page visit"
2. "subsequent edit to this track during this page visit"
3. "new page visit after leaving"
4. "re-select of the same page without leaving"

That behavior cannot be derived reliably from the current Euclid parameter values alone, so the visit and per-track snapshot status need to be tracked explicitly.

**Comment text to add adjacent to the code**

```c
/* SHIFT+PERF edits use the temporary pattern slot as a one-visit backup for
   each touched track. This state records which tracks have already been
   snapshotted during the current page visit so we only copy once per track
   until the user leaves the page or explicitly re-selects it to restore. */
```

### 2. Add one STM helper that snapshots a track into temporary only once per visit

**Files to change**

1. `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
2. possibly `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.c/.h` only if a new copy helper is cleaner there

**What to add**

Add a helper used by all four Euclid/rotation parameter handlers:

1. input: track number being edited
2. if that track is not already marked in the visit bitmask:
   - copy that track from the currently shown/edited normal pattern into the temp pattern track slot
   - set the bit in the visit mask
3. if already marked:
   - do nothing

Important detail:

1. this copy must happen before the actual parameter edit mutates the track
2. this should copy from the normal pattern currently being edited, not from temp back to temp

**Why this must exist**

The same "snapshot once before first mutation" rule is shared by:

1. Euclid length edits
2. Euclid step-count edits
3. Euclid main-step rotation edits
4. Euclid sub-step rotation edits

Keeping that logic in one helper prevents drift where one handler snapshots correctly and another forgets or snapshots too late.

**Comment text to add adjacent to the code**

```c
/* The first Euclid/rotation edit to a track during one SHIFT+PERF visit must
   preserve the pre-edit track image in the temp pattern slot. Later edits in
   the same visit must reuse that same backup instead of overwriting it. */
```

### 3. Route all four Euclid/rotation parameter edits through the snapshot helper

**File to change**

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

**What to change**

In these four command handlers:

1. `FRONT_SEQ_EUKLID_LENGTH`
2. `FRONT_SEQ_EUKLID_STEPS`
3. `FRONT_SEQ_EUKLID_ROTATION`
4. `FRONT_SEQ_EUKLID_SUBSTEP_ROTATION`

call the new snapshot helper before applying the edit to the track.

This means the order inside each handler becomes:

1. resolve the target track/pattern
2. ensure the track has been copied to temp for this visit
3. apply the Euclid/rotation change
4. refresh LEDs/front-panel replies as already needed

**Why this must exist**

This is the actual behavior change the user wants: every touched track gets one pre-edit backup in temp during the current page visit, regardless of which of the four page parameters caused the first mutation.

**Comment text to add adjacent to the code**

```c
/* Every Euclid-page mutation shares the same contract: preserve the untouched
   normal track in temp before the first edit of this visit, then mutate only
   the normal track until the user either leaves the page or explicitly
   re-selects SHIFT+PERF to restore from the saved temp copies. */
```

### 4. Add one STM helper that restores all touched tracks from temp back to normal

**Files to change**

1. `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
2. possibly `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.c/.h` if a reusable helper is cleaner there

**What to add**

Add a restore helper that:

1. walks every track bit set in the per-visit snapshot mask
2. copies temp track `N` back into the normal pattern track `N` currently being edited
3. clears the visit mask when finished

After restore, refresh the visible state:

1. track LEDs for the active track
2. Euclid parameter reply for the active track
3. track rotation reply if needed

**Why this must exist**

The restore action is now the meaning of re-selecting `SHIFT + PERF` without leaving the page. The restore needs to be multi-track because the user may have touched several tracks during the same page visit before asking to roll them all back.

**Comment text to add adjacent to the code**

```c
/* Re-selecting SHIFT+PERF during the same visit is a rollback request, not a
   page-navigation request. Every track backed up during this visit is restored
   from temp to the normal pattern so the user gets the exact pre-edit track
   images back in one action. */
```

### 5. Trigger that restore when `SHIFT + PERF` is re-selected without leaving

**Files to change**

1. `front/LxrAvr/buttonHandler.c`
2. `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

**What to change**

The AVR already re-sends `menu_setShownPattern(menu_getViewedPattern())` when the user re-selects `SHIFT + PERF` while already on that page. Keep using that transport event.

On the STM side, in the `FRONT_SEQ_SET_SHOWN_PATTERN` handler:

1. detect the existing "same shown pattern again" event
2. if the current front-panel context corresponds to the active `SHIFT + PERF` visit and there are snapshotted tracks in the visit mask:
   - restore from temp back to normal for those tracks
   - clear the visit mask
   - do not treat this particular re-select as some other kind of reset path

This plan intentionally reuses the current AVR resend behavior rather than introducing a new opcode.

**Why this must exist**

The user wants re-selecting `SHIFT + PERF` again without leaving to mean "restore the saved tracks." The AVR already knows how to resend the page's currently viewed pattern selection, so the STM can interpret that existing event in the page-specific context instead of creating another transport message.

**Comment text to add adjacent to the code**

```c
/* On the Euclid/rotation page, a repeated shown-pattern selection is not a
   generic pattern action. It is the explicit "restore the tracks I touched
   during this visit" gesture, so consume it here as a temp-to-normal rollback
   for every snapshotted track. */
```

### 6. Clear the visit state whenever the user leaves the page

**Files to change**

1. `front/LxrAvr/Menu/menu.c`
2. `front/LxrAvr/buttonHandler.c`
3. `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

**What to change**

Add one explicit "page visit ended" signal path for leaving `SHIFT + PERF`.

There are two reasonable implementation options:

1. preferred: send one existing or repurposed command when the AVR leaves the page so the STM can clear the visit-active flag and track mask immediately
2. fallback: infer visit end from the next non-Euclid front-panel context transition if there is already a reliable command stream for that

The plan should favor an explicit signal if one already exists or can be reused safely, because the behavior is per-page-visit and should not depend on fragile inference.

When that leave event is observed:

1. clear the visit-active flag
2. clear the snapshotted-track mask
3. do not restore anything automatically

**Why this must exist**

The user explicitly defined "commit" as leaving the page. That means:

1. leaving the page should discard the ability to restore those edits later
2. returning to the page should start a fresh visit where the next touched track snapshots again

Without a reliable leave/reset of the visit state, later edits could accidentally reuse old temp backups from a previous visit.

**Comment text to add adjacent to the code**

```c
/* Leaving SHIFT+PERF commits the current normal-pattern edits by discarding
   the per-visit temp-backup bookkeeping. Returning later must start a new
   snapshot cycle, so the old touched-track mask cannot survive across page
   exits. */
```

### 7. Ensure the temp copy uses the pattern the user is actually editing

**Files to review/change**

1. `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
2. `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.c/.h`

**What to verify**

The source for the snapshot copy must be:

1. the normal pattern slot currently being edited/shown
2. not the temp slot
3. not merely the currently playing source if that differs from the shown/edit target

The restore destination must be that same normal shown/edit pattern.

If the existing pattern-copy helpers are too source-agnostic or too playback-oriented, add a dedicated helper for:

1. copy one track from one explicit source pattern to one explicit destination pattern

**Why this must exist**

This feature is about restoring the track the user edited on the page, not whichever source is currently audible or routed elsewhere. Using explicit source/destination pattern selection avoids accidental interaction with temp playback, background loading, or per-track active playback sources.

**Comment text to add adjacent to the code**

```c
/* These Euclid-page backups are edit-target snapshots, not playback-source
   snapshots. Always copy between the explicitly shown normal pattern and the
   temp pattern slot so background/temp playback ownership cannot redirect the
   restore to the wrong source image. */
```

## Expected Result After Implementation

After the planned changes:

1. the first Euclid/rotation edit to a track during one `SHIFT + PERF` visit snapshots that track into temp before the edit
2. additional edits to the same track during that same visit do not overwrite the temp backup
3. touching a second or third track during the same visit snapshots each of those tracks once as well
4. leaving the page commits the edits by clearing the per-visit snapshot bookkeeping without restoring
5. returning to the page starts a fresh visit, so the next touched track snapshots again
6. re-selecting `SHIFT + PERF` again without leaving restores every track touched during that visit from temp back into the normal pattern being edited

## Current Understanding

This plan is understood as written. No questions right now.

## Implementation Notes

- 2026-06-29: Implemented the page-visit lifecycle using the existing `SEQ_EUKLID_RESET` / `FRONT_SEQ_EUKLID_RESET` opcode with explicit `data2` actions. `0x01` keeps its legacy "clear Euclid caches" meaning for preset/file operations; new `BEGIN_VISIT`, `END_VISIT`, and `RESTORE_VISIT` values now let the AVR tell the STM exactly when `SHIFT + PERF` starts, commits, or rolls back one snapshot window.
- 2026-06-29: Added STM-owned per-visit snapshot bookkeeping in `frontPanelReceivingProtocol.c`: active-visit flag, touched-track mask, source normal pattern, and per-track backups of Euclid steps/main rotation/sub-step rotation values. The four Euclid edit handlers now all call one shared snapshot-once helper before mutating the normal track.
- 2026-06-29: The actual track backup/restore uses the existing temp pattern track storage via `pat_copyTrackPattern()`. On first edit, the untouched normal track is copied into temp. On rollback, temp track data is copied back into the original normal pattern and the cached Euclid page values for that track are restored with the new `euklid_restoreTrackState()` helper without rotating the track again.
- 2026-06-29: AVR `SHIFT + PERF` behavior now explicitly maps to the new lifecycle: entering the page begins a fresh visit, leaving any other mode ends/commits it, and pressing `SHIFT + PERF` again while already on the page sends a rollback request and immediately refreshes the Euclid page view.
- 2026-06-29: Corrected the AVR hook-up after re-test: `hold SHIFT, then press PERF` enters `SELECT_MODE_PAT_GEN` and opens `EUKLID_PAGE` via `menu_enterPatgenMode()`. That is the real snapshot scope for this feature. The visit now begins there, commits when the user switches away from `EUKLID_PAGE`, and rollback fires only when PERF is pressed again while SHIFT is held and `EUKLID_PAGE` is already open.
- 2026-06-29: Corrected the rollback trigger after another re-test: once Euclid mode is active, pressing SHIFT temporarily overlays `SEQ_PAGE`, so checking `menu_getActivePage() == EUKLID_PAGE` at PERF-button time was wrong. The live rollback/commit detection now keys off whether `SELECT_MODE_PAT_GEN` was already active before the PERF press, which reliably distinguishes "re-press SHIFT+PERF" from "enter Euclid page for the first time."
- 2026-06-29: Corrected the multi-track rollback bug: selecting a different track while already on the Euclid page also called `menu_enterPatgenMode()`, and the first implementation re-sent `BEGIN_VISIT` every time. That cleared the STM touched-track mask on each track change, so rollback only had the last edited track left to restore. `BEGIN_VISIT` is now sent only on real page entry, not on in-page track refreshes.
