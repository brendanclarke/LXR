# BG_LOAD_SWITCH_AUDIT.md

## Status

Queued force-switch implementation retained; prior voice-source repair attempt reverted.

Implementation notes:

- The queued `forceInstantSwitch` implementation is restored as the active code.
- The failed `seq_switchToTmpPatternNowPreservePosition()` helper attempt has
  been removed from active code.
- No new sequencer-position writes are present.
- `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` completes `pat_copyToTmpPattern()`, then
  queues `SEQ_TMP_PATTERN` with the background-only force-instant flag.
- The background ACK service now waits for STM-local temp playback readiness,
  then waits 100 ms before sending `FRONT_SEQ_BACKGROUND_SWAP_DONE`.
- No AVR code is changed in this phase.
- The attempted `preset_updateVoiceSourcesForPatternChange()` stale-state repair
  condition has been removed from active code after hardware retest failed.
- Added source comments along the background/temp switch path documenting inputs,
  callers, outputs, ownership, and why adjacent helpers cannot substitute for
  each other.
- Continuing investigation now focuses on why the first `.ALL` attempt changes
  state such that a repeated `.ALL` load from the same load screen succeeds.

Verification run after implementation:

- `make -C front/LxrAvr avr -j4` passed; no AVR rebuild was needed.
- `make -C mainboard/LxrStm32 -j4 stm32` passed.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.

## Goal

After the STM receives `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` during a background
file load, it should:

1. Copy the currently audible normal preset/kit and pattern data into the
   existing temporary storage.
2. Immediately after that copy is complete, switch playback to the temporary
   pattern and temporary preset data without waiting for the bar boundary.
3. Wait 100 ms after the switch has actually happened.
4. Send `FRONT_SEQ_BACKGROUND_SWAP_DONE` so the AVR can continue the file load.

This forced background-load switch must ignore the user's global `PCInstnt`
setting. It should not change the setting or leave it changed; it should use a
background-load-only force path.

An instant switch already means the sequencer positions are preserved. This
phase must not add any new sequencer-position rewrite for background load; it
should reuse the existing instant-switch behavior where mid-bar switches leave
the current `seq_stepIndex[]` cursors alone.

## Current State

### AVR

The AVR side is already in the right shape after the previous phase and the
user's correction:

- `front/LxrAvr/Preset/presetManager.c:154-172`
  - `preset_backgroundSwapNeeded()` only allows the background-swap handshake
    while `menu_sequencerRunning` is true and the currently played pattern is
    not `SEQ_TMP_PATTERN`.
- `front/LxrAvr/Preset/presetManager.c:2003-2009`
  - `.PAT` still shows the actual `Loading Patrn` screen.
- `front/LxrAvr/Preset/presetManager.c:2388-2391`
  - `.ALL` still shows the actual `Loading All` screen.
- `front/LxrAvr/Preset/presetManager.c:2556-2558`
  - `.PRF` still shows the actual `Loading Perf` screen.

No AVR code change is expected for this phase. The background-load addition is
conditional on playback; the actual file-loading screens remain unconditional.

### STM Background Begin

The current STM background begin handler is:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:2614-2619`

```c
case FRONT_SEQ_BACKGROUND_SWAP_BEGIN:
   pat_copyToTmpPattern(seq_activePattern);
   frontParser_backgroundSwapPending = 1;
   frontParser_backgroundSwapFileType = frontParser_command.data2;
   frontParser_backgroundSwapStartTick = systick_ticks;
   break;
```

This performs the normal-to-temp copy, then starts the dummy delay immediately.
It does not yet request playback to move to `SEQ_TMP_PATTERN`.

### Existing Copy-To-Temp Behavior

The helper we should continue to reuse is:

- `mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.c:443-460`
  - `pat_copyToTmpPattern(uint8_t srcPattern)`

While running, it copies each track from:

```c
seq_perTrackActivePattern[track]
```

into `SEQ_TMP_PATTERN`, then captures the normal preset/kit image into
`preset_tmpKitState` through `preset_captureTmpKitState()`.

That is still the correct copy primitive. Do not change it.

### Existing Temp Pattern Switch Path

The manual SEQ16 temp-pattern switch currently enters here:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:2489-2490`

```c
case FRONT_SEQ_CHANGE_TMP_PAT:
   seq_setNextPattern(SEQ_TMP_PATTERN, (frontParser_command.data2>>3));
   break;
```

`seq_setNextPattern()` is:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c:297-325`

When called with voice `0x0f` or higher, it stages all tracks to the requested
pattern and sets:

```c
preset_tempPlaybackSwitchState.loadPendingFlag = 1;
preset_tempPlaybackSwitchState.loadSeqNow = 1;
```

The actual switch is applied in:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c:640-718`

Current switch timing condition:

```c
if((!masterStepPos)||(switchOnNextStep && preset_tempPlaybackSwitchState.loadSeqNow))
```

`switchOnNextStep` is the STM mirror of the AVR `PCInstnt` setting:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c:143`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:2517-2520`

The existing instant branch already preserves sequencer positions because when
`masterStepPos != 0`, it does not call `seq_setStepIndexToStart()`. It only
clears `loadSeqNow` and leaves the current `seq_stepIndex[]` cursors in place.

That is the behavior this phase should reuse.

### Current Ack Timer

The background ACK service is:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:1213-1225`

It currently waits against:

```c
FRONT_BACKGROUND_SWAP_DELAY_TICKS
```

defined as:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:77`

```c
#define FRONT_BACKGROUND_SWAP_DELAY_TICKS 8000U
```

The STM SysTick is 4 kHz, so:

- 8000 ticks = 2000 ms
- 400 ticks = 100 ms

## Proposed Code Changes

### 1. Add A Background-Only Force-Instant Switch Flag

File:

- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.h`

Add a field to `PresetTempPlaybackSwitchState`:

```c
uint8_t forceInstantSwitch;
```

Suggested placement:

```c
uint8_t loadPendingFlag;
uint8_t loadSeqNow;
uint8_t forceInstantSwitch;
uint8_t tmpBoundaryPatternSwitchAck;
```

Reason:

- The behavior belongs to the temp-playback switch state, not the UART parser.
- It avoids writing to `switchOnNextStep`, so the user's `PCInstnt` setting is
  neither changed nor temporarily spoofed.
- It makes the forced instant behavior visible in the same state object that
  already owns `pendingPattern`, `perTrackPendingPattern[]`, `loadPendingFlag`,
  and `loadSeqNow`.

### 2. Let The Sequencer Instant Branch Honor That Flag

File:

- `mainboard/LxrStm32/src/Sequencer/sequencer.c`

Change the pattern-switch timing condition around line 641 from:

```c
if((!masterStepPos)||(switchOnNextStep && preset_tempPlaybackSwitchState.loadSeqNow))
```

to a local explicit condition:

```c
uint8_t forceInstantSwitch =
   (switchOnNextStep || preset_tempPlaybackSwitchState.forceInstantSwitch)
   && preset_tempPlaybackSwitchState.loadSeqNow;

if((!masterStepPos) || forceInstantSwitch)
```

Then clear the background force flag only after a switch has actually been
applied:

```c
preset_tempPlaybackSwitchState.forceInstantSwitch = 0;
```

Place the clear inside the inner switch block, after the new active/per-track
pattern state has been written. A good location is after the existing
`loadSeqNow` handling around lines `708-717`.

Do not clear or change `switchOnNextStep`.
Do not add any new writes to `seq_stepIndex[]` for this background path.

Expected behavior:

- Immediately after the copy-to-temp helper finishes, the queued temp switch is
  eligible to execute on the next sequencer step.
- If the transport is mid-bar, that switch keeps the current `seq_stepIndex[]`
  positions through the existing instant-switch behavior.
- If the request lands exactly on the bar boundary, the normal bar-boundary
  branch may call `seq_setStepIndexToStart()`, which is already position zero.
- Existing manual pattern switching continues to use `switchOnNextStep`.

### 3. Queue The Temp Pattern Switch After Copy-To-Temp

File:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Change the `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` case to:

```c
case FRONT_SEQ_BACKGROUND_SWAP_BEGIN:
   pat_copyToTmpPattern(seq_activePattern);
   seq_setNextPattern(SEQ_TMP_PATTERN, 0x0f);
   preset_tempPlaybackSwitchState.forceInstantSwitch = 1;
   frontParser_backgroundSwapPending = 1;
   frontParser_backgroundSwapFileType = frontParser_command.data2;
   frontParser_backgroundSwapAckDelayActive = 0;
   break;
```

Notes:

- `seq_setNextPattern(SEQ_TMP_PATTERN, 0x0f)` stages all tracks to the temp
  pattern through the existing sequencer switch path.
- The all-track selector matches the user's request to switch playback to the
  temporary pattern and parameter data for the whole currently playing pattern.
- The already-existing AVR guard should prevent this opcode while playback is
  stopped or already on `SEQ_TMP_PATTERN`.

### 4. Change The ACK Service Into A Two-Phase Wait

File:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Rename the delay constant:

```c
#define FRONT_BACKGROUND_SWAP_ACK_DELAY_TICKS 400U
```

Add one parser-local state byte:

```c
static uint8_t frontParser_backgroundSwapAckDelayActive = 0;
```

Add a small completion helper near the other parser-local helpers:

```c
static uint8_t frontParser_backgroundSwapTempPlaybackReady(void)
{
   uint8_t track;

   if(seq_activePattern != SEQ_TMP_PATTERN)
      return 0;

   for(track=0; track<NUM_TRACKS; track++)
   {
      if(seq_perTrackActivePattern[track] != SEQ_TMP_PATTERN)
         return 0;
   }

   return preset_allVoiceSourcesUseTmp();
}
```

Then change `frontParser_serviceBackgroundSwapAck()` to:

```c
void frontParser_serviceBackgroundSwapAck(void)
{
   if(!frontParser_backgroundSwapPending)
      return;

   if(!frontParser_backgroundSwapAckDelayActive)
   {
      if(!frontParser_backgroundSwapTempPlaybackReady())
         return;

      frontParser_backgroundSwapAckDelayActive = 1;
      frontParser_backgroundSwapStartTick = systick_ticks;
      return;
   }

   if((uint32_t)(systick_ticks - frontParser_backgroundSwapStartTick)
      < FRONT_BACKGROUND_SWAP_ACK_DELAY_TICKS)
      return;

   frontParser_backgroundSwapPending = 0;
   frontParser_backgroundSwapAckDelayActive = 0;
   frontPanelSending_sendPriorityTriplet(FRONT_SEQ_CC,
                                         FRONT_SEQ_BACKGROUND_SWAP_DONE,
                                         frontParser_backgroundSwapFileType);
}
```

This means the ACK timer starts only after the STM has actually crossed into
temporary playback. The switch request is made immediately after the copy
finishes, but the ACK does not start its 100 ms delay until STM-local playback
state confirms that the switch has actually landed.

## Expected Runtime Sequence

```text
AVR load starts while sequencer is running
-> AVR sends SEQ_BACKGROUND_SWAP_BEGIN
-> STM copies current audible normal pattern/kit into temp
-> immediately after copy completion, STM queues SEQ_TMP_PATTERN for all tracks
-> STM marks the queued switch as force-instant for this background load
-> sequencer applies switch on the next transport step through the existing
   instant-switch behavior, preserving step cursors
-> live pattern sources and voice sources now point at temp
-> parser observes temp playback ready
-> parser waits 100 ms
-> STM sends priority SEQ_BACKGROUND_SWAP_DONE
-> AVR continues the file load into normal storage
```

## Important Guardrails

- Do not change `pat_copyToTmpPattern()`.
- Do not change `preset_captureTmpKitState()`.
- Do not set or restore `switchOnNextStep`; leave `PCInstnt` untouched.
- Do not add any new sequencer-position manipulation for the background forced
  switch. In particular, do not write `seq_stepIndex[]` and do not call
  `seq_setStepIndexToStart()` outside the existing bar-boundary behavior.
- Do not send `FRONT_SEQ_BACKGROUND_SWAP_DONE` until temp playback is actually
  active.
- Keep the final ACK as `frontPanelSending_sendPriorityTriplet()` so quiet UI
  during `.ALL/.PRF` flow sessions cannot suppress it.
- Do not create a new cache/session model. This remains copy-to-temp plus the
  existing temp/normal source switch.

## Risk Notes

`frontPanelSending_sendPatternChange(seq_activePattern)` in the sequencer uses
ordinary front-panel traffic, not priority traffic. During some background load
session states, ordinary UI traffic may be quieted. The ACK service should not
depend on the AVR having received that pattern-change report; it should depend
on STM-local state (`seq_activePattern`, `seq_perTrackActivePattern[]`, and
`preset_allVoiceSourcesUseTmp()`).

The AVR may learn that playback moved to `SEQ_TMP_PATTERN` through the ordinary
pattern-change report before the load continues. If that report is suppressed,
the sound-side behavior is still protected because the STM has already switched
to temp before sending the priority ACK.

## Verification Plan

Build checks after implementation:

```bash
make -C front/LxrAvr avr -j4
make -C mainboard/LxrStm32 -j4 stm32
make firmware
```

Manual retest checklist:

1. Set `PCInstnt` off. Start playback mid-bar, background-load a matching file
   type, and confirm playback switches to temp immediately rather than waiting
   until the next bar.
2. Repeat with `PCInstnt` on and confirm behavior is the same.
3. Confirm the file load does not start until after the temp switch and the
   extra 100 ms delay.
4. Confirm stopped loads still do not send the background-swap opcode, but do
   still show the actual file loading screen.
5. Confirm background `.PAT` load does not alter audible parameters after the
   switch, because the file payload continues into normal pattern storage while
   playback reads from temp.

## Bug Follow-Up: Queued Switch Can Stall First Background Load

### Repro Report

User hardware repro:

1. Load `P000.ALL` while the sequencer is playing.
2. Enter the global menu and set background load to `tot`.
3. Return to load page and load `P001.ALL`.

Observed on the first `P001.ALL` attempt:

- `Bckgrnd Swap...` / background-load screen appears briefly.
- Copy-to-temp has happened.
- Playback has not switched to the temporary data.
- The actual `.ALL` file load has not occurred.
- The global background-load setting remains `tot`.

If the same `.ALL` file is selected again immediately, the load proceeds.

### Assessment

The current implementation still treats the temp switch as a queued sequencer
pattern change:

```c
pat_copyToTmpPattern(seq_activePattern);
seq_setNextPattern(SEQ_TMP_PATTERN, 0x0f);
preset_tempPlaybackSwitchState.forceInstantSwitch = 1;
```

The actual source switch only happens later inside `seq_nextStep()`:

```c
if((!masterStepPos) || forceInstantSwitch)
{
   ...
   seq_activePattern = newActivePattern;
   ...
   seq_perTrackActivePattern[i] = ...;
   ...
}
```

That does not fully match the clarified requirement. The switch should happen
immediately after `pat_copyToTmpPattern()` returns, not merely become eligible
for the next transport step.

Because the ACK service now waits for STM-local temp playback readiness:

```c
seq_activePattern == SEQ_TMP_PATTERN
seq_perTrackActivePattern[] == SEQ_TMP_PATTERN
preset_allVoiceSourcesUseTmp()
```

any case where the queued switch is not applied promptly leaves the AVR waiting
before `SEQ_FILE_BEGIN`. That matches the symptom: the copy completed, but the
actual load could not begin because `FRONT_SEQ_BACKGROUND_SWAP_DONE` was never
sent.

The second immediate load proceeding is consistent with the first attempt
having left useful state behind: the temp copy and/or queued/pending temp
switch state may have completed after the user-visible first attempt failed or
timed out, so the second attempt starts from a different state.

### Resolution Direction

Replace the queued switch with an immediate STM-side temp playback source switch
that runs synchronously after the copy-to-temp helper completes.

The new implementation should not call `seq_setNextPattern()` and wait for
`seq_nextStep()` to apply the state. Instead, add a small Sequencer/TempPlayback
helper that performs the same source-selection work currently done inside the
pattern-switch block, but without modifying sequencer positions.

Candidate helper shape:

```c
void seq_switchToTmpPatternNowPreservePosition(void);
```

or, if ownership should sit more visibly with temp playback:

```c
void preset_switchPlaybackToTmpPatternNow(void);
```

The helper should:

- snapshot `oldTrackPattern[]` from `seq_perTrackActivePattern[]`;
- set `seq_activePattern = SEQ_TMP_PATTERN`;
- call `preset_setTempPlaybackActive(1)`;
- set every `seq_perTrackActivePattern[i] = SEQ_TMP_PATTERN`;
- call `preset_updateVoiceSourcesForPatternChange(oldTrackPattern, 0)` so the
  live voice source state moves to temp without pushing ordinary restore/UI
  traffic;
- set `preset_tempPlaybackSwitchState.tmpBoundaryPatternSwitchAck = 1` if this
  crossed a normal/temp boundary;
- clear stale pending switch bits such as `loadPendingFlag`, `loadSeqNow`, and
  `forceInstantSwitch` so a later sequencer tick does not replay the same
  request.

The helper must not:

- write `seq_stepIndex[]`;
- call `seq_setStepIndexToStart()`;
- call `seq_realign()`;
- depend on `switchOnNextStep` / `PCInstnt`;
- wait for the next transport step;
- change `pat_copyToTmpPattern()`.

After that helper exists, `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` should become:

```c
case FRONT_SEQ_BACKGROUND_SWAP_BEGIN:
   pat_copyToTmpPattern(seq_activePattern);
   seq_switchToTmpPatternNowPreservePosition();
   frontParser_backgroundSwapPending = 1;
   frontParser_backgroundSwapFileType = frontParser_command.data2;
   frontParser_backgroundSwapAckDelayActive = 0;
   break;
```

Then `frontParser_backgroundSwapTempPlaybackReady()` can remain as a defensive
assertion before starting the 100 ms ACK delay. In the normal case, readiness
should be true immediately after the synchronous switch helper returns.

### Audit Correction

The earlier “Expected Runtime Sequence” section says:

```text
-> immediately after copy completion, STM queues SEQ_TMP_PATTERN for all tracks
-> STM marks the queued switch as force-instant for this background load
-> sequencer applies switch on the next transport step
```

That sequence should be considered superseded by this bug follow-up. The
correct sequence is:

```text
-> STM copies current audible normal pattern/kit into temp
-> immediately after copy completion, STM switches active playback sources to temp
-> sequencer positions are left untouched
-> parser observes temp playback ready
-> parser waits 100 ms
-> STM sends priority SEQ_BACKGROUND_SWAP_DONE
-> AVR continues the file load into normal storage
```

### Verification Additions

Add this exact regression test to the manual checklist:

1. Load `P000.ALL` while the sequencer is playing.
2. Change global background load to `tot`.
3. Load `P001.ALL`.
4. Confirm that the first attempt switches to temp immediately after the copy,
   sends the ACK after about 100 ms, and then performs the actual `.ALL` load.
5. Confirm a second immediate attempt is not required.

## Failed Attempt Reverted And New Evaluation

### Rollback Status

The failed synchronous-helper attempt has been removed from active code.

Reverted from the last attempt:

- Removed `seq_switchToTmpPatternNowPreservePosition()` from
  `mainboard/LxrStm32/src/Sequencer/sequencer.c`.
- Removed its declaration from `mainboard/LxrStm32/src/Sequencer/sequencer.h`.
- Restored `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` to the queued force-switch shape:

```c
pat_copyToTmpPattern(seq_activePattern);
seq_setNextPattern(SEQ_TMP_PATTERN, 0x0f);
preset_tempPlaybackSwitchState.forceInstantSwitch = 1;
```

Active code is now back to the previous queued force-switch implementation.

Rollback verification:

- `make -C front/LxrAvr avr -j4` passed; no AVR rebuild was needed.
- `make -C mainboard/LxrStm32 -j4 stm32` passed.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN` without the
  failed synchronous-helper attempt.

### What The Failed Attempt Proved

The failed helper directly wrote:

```c
seq_activePattern = SEQ_TMP_PATTERN;
seq_perTrackActivePattern[i] = SEQ_TMP_PATTERN;
```

before the background ACK service could run.

The user reported identical behavior after that change:

- copy-to-temp happened;
- switch did not complete from the user's perspective;
- ACK/file load did not proceed.

Given that the helper wrote the pattern source state synchronously, the remaining
blocking predicate in the ACK path is not the queued pattern switch itself. The
exact ACK-side function that can still refuse to proceed is:

```c
frontParser_backgroundSwapTempPlaybackReady()
```

and, after the failed helper's direct pattern-source writes, the specific
remaining false return is:

```c
return preset_allVoiceSourcesUseTmp();
```

`frontParser_backgroundSwapTempPlaybackReady()` currently requires three things:

```c
seq_activePattern == SEQ_TMP_PATTERN
seq_perTrackActivePattern[track] == SEQ_TMP_PATTERN for every track
preset_allVoiceSourcesUseTmp()
```

The failed helper should have satisfied the first two immediately. Therefore the
blocker is the final voice-source check. `preset_allVoiceSourcesUseTmp()` returns
false while at least one `preset_voiceSourceState[]` entry still says normal.

### Underlying State-Repair Bug

The function responsible for moving voice-source state after a pattern source
change is:

```c
preset_updateVoiceSourcesForPatternChange()
```

in `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c`.

Its current decision is based only on whether the old track pattern and new
track pattern imply different normal/temp ownership:

```c
if(oldUseTmp != newUseTmp)
{
   preset_markVoiceSourceTarget(synthVoice, newUseTmp);
   changedVoiceMask |= (uint8_t)(1 << synthVoice);
}
```

That is too narrow for the background-load path. It assumes the
`preset_voiceSourceState[]` bookkeeping is already coherent with the old pattern
source table. The bug report strongly suggests a stale/incoherent state is
possible:

- pattern source state and voice-source state can disagree;
- the ACK readiness check asks the voice-source state whether all voices use
  temp;
- `preset_updateVoiceSourcesForPatternChange()` may skip a voice when
  `oldUseTmp == newUseTmp`, even if `preset_voiceSourceState[synthVoice]` is
  wrong for `newUseTmp`.

That skip is the actual state repair failure. The ACK then spins forever at
`preset_allVoiceSourcesUseTmp()` because the pattern source may have moved to
temp while one or more voice source markers remain normal.

### Correct Fix Direction

Do not add another helper on top of this. Fix the existing state repair function
so it repairs mismatched voice-source state, not only pattern-source changes.

Modify `preset_updateVoiceSourcesForPatternChange()` so each synth voice is
updated when either:

- old pattern ownership differs from new pattern ownership; or
- `preset_voiceSourceState[synthVoice]` does not match the source implied by
  the new pattern ownership.

Concrete shape:

```c
uint8_t targetState = newUseTmp ? SEQ_VOICE_SOURCE_TMP
                                : SEQ_VOICE_SOURCE_NORMAL;

if((oldUseTmp != newUseTmp)
   || (preset_voiceSourceState[synthVoice] != targetState))
{
   preset_markVoiceSourceTarget(synthVoice, newUseTmp);
   changedVoiceMask |= (uint8_t)(1 << synthVoice);
}
```

This changes the existing function that owns the voice-source transition. It
does not add a parallel switch path, does not touch sequencer positions, and
does not weaken the ACK readiness check.

### Why This Is Safer Than Removing The ACK Check

Removing `preset_allVoiceSourcesUseTmp()` from
`frontParser_backgroundSwapTempPlaybackReady()` would let the AVR continue the
file load even if pattern storage had switched to temp but voice parameter
ownership had not. That is exactly the state background loading is meant to
avoid.

The correct fix is to make `preset_updateVoiceSourcesForPatternChange()` repair
voice ownership reliably, then keep the readiness check strict.

### Expected Result

With the state-repair condition fixed:

1. `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` copies normal pattern/kit into temp.
2. The queued forced pattern switch moves active pattern sources to temp.
3. `preset_updateVoiceSourcesForPatternChange()` repairs any stale voice source
   marker, even if old/new pattern ownership comparison alone would have skipped
   it.
4. `preset_allVoiceSourcesUseTmp()` returns true.
5. `frontParser_backgroundSwapTempPlaybackReady()` stops early-returning.
6. The 100 ms ACK delay starts.
7. `FRONT_SEQ_BACKGROUND_SWAP_DONE` is sent.
8. AVR continues the `.ALL` load on the first attempt.

## New Attempt Implementation Notes

### Exact Early Return Being Addressed

The failed behavior is still centered on this readiness gate:

```c
static uint8_t frontParser_backgroundSwapTempPlaybackReady(void)
{
   ...
   return preset_allVoiceSourcesUseTmp();
}
```

The `.ALL` load was getting as far as `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` and
`pat_copyToTmpPattern()`, but the ACK service could still stay pending because
the final voice-source readiness check could remain false.

### Corrected Function

The targeted fix is in:

- `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c`
  - `preset_updateVoiceSourcesForPatternChange()`

Before this attempt, that function only called `preset_markVoiceSourceTarget()`
when the old/new pattern-source comparison changed:

```c
if(oldUseTmp != newUseTmp)
```

That is not enough if `preset_voiceSourceState[]` is already stale when the
forced temp switch is applied. The function now also compares the actual stored
voice-source marker against the target source:

```c
uint8_t targetState = newUseTmp ? SEQ_VOICE_SOURCE_TMP
                                : SEQ_VOICE_SOURCE_NORMAL;

if((oldUseTmp != newUseTmp)
   || (preset_voiceSourceState[synthVoice] != targetState))
```

This keeps the strict ACK readiness guard intact and makes the function that
already owns voice-source reconciliation repair stale state directly.

### Scope Of Code Changes

- No replacement synchronous sequencer switch helper was reintroduced.
- No sequencer position rewrite was added.
- The background switch still uses `seq_setNextPattern(SEQ_TMP_PATTERN, 0x0f)`
  plus `forceInstantSwitch`.
- `frontParser_backgroundSwapTempPlaybackReady()` still requires both temp
  pattern ownership and temp voice-parameter ownership before starting the
  100 ms ACK delay.
- Comments were expanded on every function touched by the temporary switch and
  background-load path in this attempt so each function documents its inputs,
  callers, output, ownership boundary, and why adjacent helpers cannot replace
  it.

### Verification After New Attempt

- `make -C front/LxrAvr avr -j4` passed; no AVR rebuild was needed.
- `make -C mainboard/LxrStm32 -j4 stm32` passed.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.

## Failed Repair Reverted; First/Second Attempt Investigation

### Reverted This Turn

The failed functional change in
`mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c` has been removed. Active
code is back to the original boundary-only transition test:

```c
if(oldUseTmp != newUseTmp)
{
   preset_markVoiceSourceTarget(synthVoice, newUseTmp);
   changedVoiceMask |= (uint8_t)(1 << synthVoice);
}
```

Useful ownership comments were left in place, with the inaccurate "stale
repair" wording removed from the function comment.

### Config/File Baseline

`GLO.CFG`, `P000.ALL`, and `P001.ALL` all store
`PAR_FILE_LOAD_BACKGROUND == 1` (`pat`) at the global-settings offset. The
observed failing case therefore depends on the user's manual change after
loading `P000.ALL`:

1. `P000.ALL` loads globals with background mode `pat`.
2. User changes AVR `parameter_values[PAR_FILE_LOAD_BACKGROUND]` to `tot` (`4`).
3. `P001.ALL` starts with AVR background mode `tot`, because `.ALL` performs the
   background-swap check before reading the target file's globals.
4. If the `.ALL` file load does not actually reach the global-read phase,
   `PAR_FILE_LOAD_BACKGROUND` remains `tot`, matching the user's observation.

### Hard Control-Flow Fact

Inside `preset_loadAll()`, once `Bckgrnd Swap...` has been displayed and
`preset_performBackgroundSwapWait()` returns, there is no normal branch that can
return to the load menu before `Loading All` is written:

- `front/LxrAvr/Preset/presetManager.c:2353-2354`
  - optional `preset_performBackgroundSwapWait(WTYPE_ALL)`
- `front/LxrAvr/Preset/presetManager.c:2356-2357`
  - sends `SEQ_FILE_BEGIN`
- `front/LxrAvr/Preset/presetManager.c:2365-2383`
  - reads globals if full `.ALL` load
- `front/LxrAvr/Preset/presetManager.c:2388-2391`
  - writes `Loading All`

The return value from `preset_performBackgroundSwapWait()` is currently ignored,
so even a timeout should fall through toward `SEQ_FILE_BEGIN` and `Loading All`.
This means the exact reported display sequence, "Bckgrnd Swap..." then back to
the load screen with no `Loading All`, is not explained by a simple false return
from the wait loop inside `preset_loadAll()`.

### Variables Changed By The First Attempt

AVR variables touched before/during the first attempt:

- `parameter_values[PAR_FILE_LOAD_BACKGROUND]`
  - Manually changed to `BACKGROUND_TOT`.
  - Stays `tot` if `P001.ALL` does not reach the global-read phase.
- `preset_backgroundSwapDone`
  - Cleared to `0` at the start of `preset_performBackgroundSwapWait()`.
  - Set to `1` only by `SEQ_BACKGROUND_SWAP_DONE` with matching file type.
- `preset_backgroundSwapExpectedType`
  - Set to `WTYPE_ALL` for the intended `.ALL` wait.
- `avrCommsParser_rxDisable`
  - Set to `1` during `.ALL` load-session setup before background swap.
  - Background-swap DONE is explicitly allowed through while disabled.
- `menu_playedPattern`
  - Used by `preset_backgroundSwapNeeded()` to skip background swap when the AVR
    believes playback is already on `SEQ_TMP_PATTERN`.
  - Updated only by a received `SEQ_CHANGE_PAT` report from STM.
- `menu_saveOptions` / `menu_currentPresetNr[]`
  - Determine which loader runs from the load menu on the next click.
  - A repeat from the same load screen can use different state if a previous
    path reset or partially changed the save/load UI.

STM variables touched by the first `FRONT_SEQ_BACKGROUND_SWAP_BEGIN`:

- `pat_tmpPattern`
  - Updated by `pat_copyToTmpPattern(seq_activePattern)`.
- `preset_tmpKitState`
  - Updated by `preset_captureTmpKitState()` inside the pattern copy.
- `preset_tempPlaybackSwitchState.pendingPattern`
  - Set to `SEQ_TMP_PATTERN` by `seq_setNextPattern()`.
- `preset_tempPlaybackSwitchState.perTrackPendingPattern[]`
  - Set to `SEQ_TMP_PATTERN` for all tracks by `seq_setNextPattern()`.
- `preset_tempPlaybackSwitchState.loadPendingFlag`
  - Set to `1`.
- `preset_tempPlaybackSwitchState.loadSeqNow`
  - Set to `1`.
- `preset_tempPlaybackSwitchState.forceInstantSwitch`
  - Set to `1`.
- `frontParser_backgroundSwapPending`
  - Set to `1`.
- `frontParser_backgroundSwapFileType`
  - Set to the AVR-requested file type.
- `frontParser_backgroundSwapAckDelayActive`
  - Cleared to `0`.

If the sequencer service gets far enough after those assignments, the following
additional STM state may change before the user's repeated attempt:

- `seq_activePattern`
  - Can become `SEQ_TMP_PATTERN`.
- `seq_perTrackActivePattern[]`
  - Can become `SEQ_TMP_PATTERN` for all tracks.
- `preset_setTempPlaybackActive(1)`
  - Selects temp kit/current-image state.
- `preset_updateVoiceSourcesForPatternChange()`
  - Moves voice sources when the old/new normal-temp boundary is detected.
- `frontParser_backgroundSwapAckDelayActive`
  - Starts the 100 ms ACK delay once readiness is true.
- `FRONT_SEQ_BACKGROUND_SWAP_DONE`
  - Eventually sent as priority traffic.

### Why The Second Attempt Can Differ

The repeat is not starting from the same state as the first attempt. The first
attempt can prime the STM by copying current normal data into temp and staging
or completing the move to `SEQ_TMP_PATTERN`. Once that has happened, a repeated
load can succeed for one of two reasons:

1. If the AVR receives a delayed `SEQ_CHANGE_PAT` report after the first
   attempt, `menu_playedPattern` becomes `SEQ_TMP_PATTERN`. The second `.ALL`
   attempt then skips `preset_backgroundSwapNeeded()` at
   `presetManager.c:161-162` and proceeds straight into the normal `.ALL` load.
2. If the AVR does not receive the pattern report but the STM has already moved
   `seq_activePattern`/`seq_perTrackActivePattern[]` to `SEQ_TMP_PATTERN`, the
   second `FRONT_SEQ_BACKGROUND_SWAP_BEGIN` reaches readiness much faster
   because the STM-side temp switch is already complete or nearly complete.

Both mechanisms explain why a repeated load from the same screen can work after
the first attempt only appears to copy-to-temp.

### Suspicious Adjacent AVR Path Historical Note

There was also a separate suspicious load-page branch, now removed from active
code:

- historical location before removal: `front/LxrAvr/buttonHandler.c:1110-1122`

When on `LOAD_PAGE`, with `menu_getWhat()` equal to performance, pattern, or
all, and `PAR_FILE_LOAD_BACKGROUND` nonzero, releasing a voice button calls:

```c
preset_loadPerf(menu_currentPresetNr[SAVE_TYPE_PERFORMANCE], menu_voiceArray);
```

It does this unconditionally, even if the current load type is
`SAVE_TYPE_ALL` or `SAVE_TYPE_PATTERN`. The normal load-menu OK path in
`front/LxrAvr/Menu/menu.c:2017-2036` dispatches correctly to
`preset_loadPattern()`, `preset_loadPerf()`, or `preset_loadAll()`.

This branch may or may not have been the user's exact button path, but it was
important because it was one of the few code paths that could make a
user-visible `.ALL` load selection not actually call `preset_loadAll()`. If the
first attempt entered through this release path, the observed "no Loading All"
result became expected, and the second attempt could work if it entered through
the normal load-menu OK dispatch instead.

### Current Best Explanation To Test

The first failed attempt is probably not a failed normal return from
`preset_loadAll()` after `preset_performBackgroundSwapWait()`. The code does not
have that shape.

The next thing to verify on hardware or with temporary counters is which of
these changes happens during the failed first attempt:

- Does AVR `menu_playedPattern` become `SEQ_TMP_PATTERN` before the repeated
  load?
- Does STM `seq_activePattern` become `SEQ_TMP_PATTERN` even though the front
  panel UI appears not to show the switch?
- Does the first attempt actually enter `preset_loadAll()`? Before the fastload
  shortcut removal, also check whether it entered the suspicious load-page
  voice-button release path and called `preset_loadPerf()`.

The exact changed variable most likely allowing the second load to proceed is
either AVR `menu_playedPattern == SEQ_TMP_PATTERN` or STM
`seq_activePattern/seq_perTrackActivePattern[] == SEQ_TMP_PATTERN`, both primed
by the first failed attempt. Before it was removed, the suspicious
`buttonHandler_voiceButtonReleased` path remained the leading explanation for
the "Bckgrnd Swap but no Loading All" display sequence if the first attempt was
initiated by any voice-button release while background load was enabled.

### Verification After Revert/Research Pass

- `make -C front/LxrAvr avr -j4` passed; no AVR rebuild was needed.
- `make -C mainboard/LxrStm32 -j4 stm32` passed after recompiling
  `TempPlaybackSwitch.c`.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN` with the
  failed functional repair removed.

## Load-Page Voice-Button Fastload Research

### Origin

The suspicious load-page voice-button release path is not new background-swap
code. It predates the current background-load work.

Current code:

- `front/LxrAvr/buttonHandler.c:1110-1124`
  - gated by `parameter_values[PAR_FILE_LOAD_BACKGROUND]`
  - calls `preset_loadPerf(menu_currentPresetNr[SAVE_TYPE_PERFORMANCE],
    menu_voiceArray)` when the final held voice button is released.

Historical code at `1e5fe6b`:

- same branch and same unconditional `preset_loadPerf(...)` call;
- but gated by `parameter_values[PAR_FILE_LOAD_FAST]`, not background load.

`git blame` shows:

- load-page release branch added by `1a919d1` on 2016-09-23:
  `slicker button handling on fastload - resolve some discrepancies in loading
  morph data`;
- load-page voice-button press selection started in `2763bdb` on 2016-08-26:
  `add individual voice load for pattern/perf/all`;
- Session 023 commit `d09e940a` only changed the gate name from
  `PAR_FILE_LOAD_FAST` to `PAR_FILE_LOAD_BACKGROUND`.

### Original Intent

The old behavior was a quick-load shortcut:

1. On `LOAD_PAGE`, pressing voice buttons while `LoadFast` was enabled built
   `menu_voiceArray`.
2. Releasing the last held voice button immediately called
   `preset_loadPerf(...)`.
3. The UI reset and jumped back to `SELECT_MODE_PERF`.

That matches the old parameter comment:

```c
PAR_FILE_LOAD_FAST, // bool, apply 'kit' load immediately
```

The important point is that the immediate release action was hardcoded to
performance load. It was never a correct file-type dispatcher. The branch tests
`SAVE_TYPE_PERFORMANCE || SAVE_TYPE_PATTERN || SAVE_TYPE_ALL`, but the action is
always `preset_loadPerf(...)`.

### Relation To Old PRF Cache

This is adjacent to the same historical load-fast/background-load era, but it is
not specifically an old `SEQ_PRF_CACHE_*` opcode helper or STM-side
`PresetLoadCache` path.

The old PRF/cache system was explicitly retired in Sessions 014/020/024. This
button branch is AVR UI behavior from the older fastload/partial-performance
workflow. It became dangerous when Session 023 renamed the `LoadFast` boolean
slot into the 5-state `PAR_FILE_LOAD_BACKGROUND` selector without separating
"immediate voice-button performance quickload" from "file type may
background-load."

### Why It Matters Now

With the current 5-state selector, any nonzero background-load mode enables this
old immediate-release shortcut. That means `pat`, `prf`, `all`, and `tot` all
make voice-button release on the load page eligible to call
`preset_loadPerf(...)`, even when the visible load type is `.PAT` or `.ALL`.

This branch can therefore explain the observed shape if the user's first
attempt involves a voice-button release while `LoadBgnd` is `tot`:

- background-swap prep may run because the wrongly-entered `preset_loadPerf()`
  sees `WTYPE_PERFORMANCE` and `tot` allows it;
- no `Loading All` is printed because `preset_loadAll()` was not called;
- no `P001.ALL` bytes are loaded;
- `LoadBgnd` remains `tot` because the `.ALL` global block was never read.

### Implication

The branch should not be treated as part of the new background-load system. It
is stale AVR UI fastload behavior that now uses the wrong gate. If retained, it
needs its own explicit setting or it needs to dispatch by `menu_getWhat()`
instead of always loading performance. If the new design does not include this
voice-button quickload gesture, the correct direction is to disable/remove this
release-triggered loader from the background-load selector path.

### Removed From Active Code

The load-page voice-button fastload path has now been removed from
`front/LxrAvr/buttonHandler.c`.

Removed active behavior:

- load-page voice-button release no longer calls
  `preset_loadPerf(menu_currentPresetNr[SAVE_TYPE_PERFORMANCE], menu_voiceArray)`;
- load-page voice-button press no longer changes behavior when
  `PAR_FILE_LOAD_BACKGROUND` is nonzero;
- load-page voice-button press always uses the normal toggle-selection behavior
  for `menu_voiceArray`;
- the now-unused `Preset/PresetManager.h` include was removed from
  `buttonHandler.c`.

This separates the new background-load selector from the deprecated `LoadFast`
gesture. Background loading should now be initiated only by the explicit file
load paths and their normal/temporary swap handshake.

## Remaining LoadFast / PRF-Cache Suspects

### Active Runtime Suspect: `SEQ_LOAD_BACKGROUND` Still Feeds `seq_loadFastMode`

There is still one active path that appears to be a `LoadFast` holdover:

- `front/LxrAvr/Menu/menu.c:3756-3758`
  - changing `PAR_FILE_LOAD_BACKGROUND` sends opcode `SEQ_LOAD_BACKGROUND`
    (`0x50`) to STM.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h:202`
  - AVR names opcode `0x50` as `SEQ_LOAD_BACKGROUND`.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h:157`
  - STM still names the same opcode `FRONT_SEQ_LOAD_FAST`.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:2660-2661`
  - STM handles it by assigning `seq_loadFastMode = frontParser_command.data2`.
- `mainboard/LxrStm32/src/Sequencer/sequencer.c:425-436`
  - `seq_loadFastMode` affects how `seq_newVoiceAvailable` is cleared during
    voice triggering.

This is not the new background-swap handshake. It is an old setting-byte path
from the previous `LoadFast` behavior, and the current 5-state background
selector sends values `0..4` into a variable that is effectively tested as
boolean. It should be considered for removal or neutralization after confirming
that no current normal/temp background-load step still depends on
`seq_loadFastMode`.

Likely cleanup direction:

- stop sending `SEQ_LOAD_BACKGROUND` from the AVR menu setter;
- remove or ignore `FRONT_SEQ_LOAD_FAST` on STM;
- remove `seq_loadFastMode` if no remaining caller needs it;
- keep `PAR_FILE_LOAD_BACKGROUND` as AVR-side policy for deciding whether to
  send `SEQ_BACKGROUND_SWAP_BEGIN`, not as STM runtime mode.

### Commented PRF Cache Opcode Surface

The old PRF-cache opcode family is still present as commented-out historical
surface:

- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
  - commented `PRF_CACHE_STATUS`, `PRF_CACHE_REJECTED`,
    `PRF_CACHE_ACCEPTED`, `SEQ_PRF_CACHE_BEGIN`,
    `SEQ_PRF_PENDING_BEGIN`, `SEQ_PRF_PENDING_DONE`,
    `SEQ_PRF_CACHE_ABORT`, `SEQ_PRF_AVR_SNAPSHOT_BEGIN`,
    `SEQ_PRF_AVR_SNAPSHOT_END`.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`
  - matching commented `FRONT_SEQ_PRF_*` and `PRF_CACHE_*` definitions.
- `front/LxrAvr/avrComms/avrCommsSendingProtocol.c/.h`
  - commented `avrComms_prfCacheBegin()`,
    `avrComms_prfCacheControl()`, and PRF-cache status wait state.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`
  - commented PRF-cache receive handling.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c/.h`
  - commented `frontPanelSending_sendPrfCacheStatus()`.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
  - an inactive `#if 0` handler block for the `FRONT_SEQ_PRF_*` commands.

These are not active runtime behavior, but they are stale enough to confuse
future audits. If the project is ready to stop carrying historical opcode
context in source, these can be deleted in a cleanup pass. Until then, they
should remain clearly marked as deprecated and must not be reactivated for the
new background-load work.

### Commented AVR Parameter Slot

`front/LxrAvr/Parameters.h` still contains a commented historical parameter:

```c
// PAR_CACHE_FOR_PERF, // bool, 0=perf load while playing works exactly like 'all',
//                     // 1=kit, pattern, settings cached until pattern change
```

This is documentation-only, but it describes the retired cache model. It can be
removed when doing the broader stale-cache source cleanup.

### Verification After Fastload Shortcut Removal

- `rg` found no remaining `preset_loadPerf`, `preset_loadPattern`,
  `preset_loadAll`, `PAR_FILE_LOAD_BACKGROUND`, or `PresetManager` references in
  `front/LxrAvr/buttonHandler.c`.
- `make -C front/LxrAvr avr -j4` passed after rebuilding `buttonHandler.c`;
  existing fallthrough warning remains in `buttonHandler_handleModeButtons`.
- `make -C mainboard/LxrStm32 -j4 stm32` passed; no STM rebuild was needed.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.

## Targeted First-Attempt `.ALL` Early-Exit Fix

### User Test Result That Changed The Diagnosis

The failed first attempt leaves playback on temp, and the background-swap screen
flashes briefly rather than waiting for the timeout. That means the STM-side
copy/switch/ACK path is succeeding. The broken part is the AVR continuation of
the ordinary `.ALL` file load after the ACK.

The repeat load works because the first failed attempt leaves playback on
`SEQ_TMP_PATTERN`; `preset_backgroundSwapNeeded()` then skips the background
swap on the repeat, so the normal loader runs without the same pre-load wait.

### Code-Level Cause Addressed

`preset_loadPattern()` already closes its `.PAT` file before calling
`preset_performBackgroundSwapWait()`. `preset_loadAll()` and
`preset_loadPerf()` did not: they opened the file, read name/version, waited for
the background swap with the FatFs file object still open, then continued
reading globals/performance metadata. If the next `f_read()` after the wait
returned zero, the loader jumped to `closeFile` before printing `Loading All`.

### Implemented Fix

Changes made in `front/LxrAvr/Preset/presetManager.c`:

- added `PRESET_NAME_VERSION_BYTES` for the shared 8-byte name plus 1-byte
  version header used by `.ALL` and `.PRF`;
- in `preset_loadAll()`, after validating name/version and before the background
  swap wait:
  - remember the normal pattern slot being protected by the temp switch;
  - close the `.ALL` file;
  - perform the background swap wait;
  - reopen the `.ALL` file;
  - `f_lseek()` back to byte 9 before reading globals;
- in `preset_loadPerf()`, perform the same close/wait/reopen/seek sequence
  before reading performance metadata;
- in `preset_loadPattern()` and `preset_loadAll()`, route
  `preset_readPatternStepData()` through a normal-pattern target captured before
  the swap, so `SEQ_TMP_PATTERN` is never used as a normal file pattern index.

The helper `preset_backgroundLoadNormalPatternTarget()` resolves the normal
target slot from `menu_playedPattern`. If playback is already on temp, it uses
the last normal pattern remembered at the previous background swap; if none is
known, it falls back to the non-temp shown pattern or pattern 0. This is not a
transport change and does not alter sequencer position. It only prevents file
load code from treating the temp sentinel as one of the eight normal file
patterns.

### Verification

- `git diff --check -- front/LxrAvr/Preset/presetManager.c` passed.
- `make -C front/LxrAvr avr -j4` passed and rebuilt `front/LxrAvr/LxrAvr.bin`.
  Existing warnings remain in `presetManager.c` for the old drum-voice
  fallthrough, the AVR `ATOMIC_BLOCK` macro analysis, and historical
  `parameters2` bounds warnings.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.
