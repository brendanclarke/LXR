# BG_LOAD_SWITCH_AUDIT.md

## Status

Queued force-switch implementation restored; failed synchronous-helper attempt reverted.

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
