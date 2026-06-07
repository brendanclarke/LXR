# TEMP_SWITCH_WALK

Date: 2026-06-06
Scope: Walkthrough of what happens on STM when the user requests a normal/temp pattern switch.

This document intentionally does not use git commands.

## Short Answer

The STM receives one of two front-panel sequencer commands from the AVR:

- Normal pattern request:
  - status: `FRONT_SEQ_CC` (`0xb2`)
  - command/data1: `FRONT_SEQ_CHANGE_PAT` (`0x0b`)
  - data2: low 3 bits are normal pattern `0..7`; upper bits select whole-pattern vs per-track target.
- Temporary pattern request:
  - status: `FRONT_SEQ_CC` (`0xb2`)
  - command/data1: `FRONT_SEQ_CHANGE_TMP_PAT` (`0x0e`)
  - data2: upper bits select whole-pattern vs per-track target.
  - STM maps this to `SEQ_TMP_PATTERN` (`8`).

Both commands call `seq_setNextPattern(...)`.

The immediate flags/state set for later `seq_tick()` consumption are:

- `seq_pendingPattern`
  - updated for whole-pattern requests.
- `seq_perTrackPendingPattern[]`
  - updated for whole-pattern or per-track requests.
- `seq_loadPendigFlag = 1`
  - tells the later switch block to consume per-track pending pattern state as a manual/user switch.
- `seq_loadSeqNow = 1`
  - allows instant switching if `switchOnNextStep` is enabled.

## Entry Point: Front Parser

In `frontPanelParser.c`, the command dispatch is:

```c
case FRONT_SEQ_CHANGE_PAT:
   seq_setNextPattern((frontParser_midiMsg.data2 & 0x07),
                      (frontParser_midiMsg.data2 >> 3));
   break;

case FRONT_SEQ_CHANGE_TMP_PAT:
   seq_setNextPattern(SEQ_TMP_PATTERN,
                      (frontParser_midiMsg.data2 >> 3));
   break;
```

Meaning of `frontParser_midiMsg.data2`:

- For `FRONT_SEQ_CHANGE_PAT`:
  - bits `0..2`: requested normal pattern number.
  - bits `3..6`: target selector passed as `voice`.
- For `FRONT_SEQ_CHANGE_TMP_PAT`:
  - pattern is forced to `SEQ_TMP_PATTERN`.
  - bits `3..6`: target selector passed as `voice`.

Target selector behavior is implemented in `seq_setNextPattern(...)`:

- `voice >= 0x0f`
  - whole-pattern switch.
- `voice < 0x0f`
  - per-track switch.
- `voice == 5 || voice == 6`
  - hi-hat special case when a temp boundary is involved; closed and open hi-hat pending patterns are changed together because they share one synth voice/parameter set.

## Immediate STM State Changes

`seq_setNextPattern(patNr, voice)` first normalizes the pattern:

```c
uint8_t nextPattern = seq_normalizePatternNumber(patNr);
```

For a whole-pattern request:

```c
seq_pendingPattern = nextPattern;
for(i = 0; i < NUM_TRACKS; i++)
   seq_perTrackPendingPattern[i] = nextPattern;
```

For a per-track request:

```c
seq_perTrackPendingPattern[voice] = nextPattern;
```

For a hi-hat temp-boundary request:

```c
seq_perTrackPendingPattern[5] = nextPattern;
seq_perTrackPendingPattern[6] = nextPattern;
```

Then, for all of those paths:

```c
seq_loadPendigFlag = 1;
seq_loadSeqNow = 1;
```

Important spelling note:

- The code name is `seq_loadPendigFlag`, not `seq_loadPendingFlag`.

## What Has Not Happened Yet

At this point, the STM has only queued a requested switch.

These have not necessarily changed yet:

- `seq_activePattern`
- `seq_perTrackActivePattern[]`
- `seq_tmpKitActive`
- `seq_voiceSourceState[]`
- live DSP voice parameter structs
- AVR endpoint/menu restore queue

Those change later when `seq_tick()` reaches the switch block.

## How `seq_tick()` Consumes The Flags

`seq_tick()` calls the sequencer stepping logic, which enters the pattern-switch block when:

```c
if((!masterStepPos) || (switchOnNextStep && seq_loadSeqNow))
{
   if((seq_activePattern != seq_pendingPattern) || seq_loadPendigFlag)
   {
      ...
   }
}
```

So there are two gates:

1. Timing gate:
   - normal bar/start boundary: `!masterStepPos`
   - or instant switch mode: `switchOnNextStep && seq_loadSeqNow`
2. Work-needed gate:
   - active pattern differs from pending pattern
   - or `seq_loadPendigFlag` is set

The reason `seq_loadPendigFlag` matters:

- Per-track temp switches may not change `seq_pendingPattern`.
- The global active pattern may already equal the global pending pattern.
- `seq_loadPendigFlag` still forces the switch block to run so per-track pending state can be committed.

## Switch Commit Sequence

Inside the switch block:

1. Optionally reset bar counter:

```c
if(seq_resetBarOnPatternChange)
   seq_barCounter = 0;
```

2. If `seq_newPatternAvailable` is set:

```c
seq_activateTmpPattern();
seq_newPatternAvailable = 0;
```

This is separate background-loaded pattern activation, not the same thing as selecting `SEQ_TMP_PATTERN`.

3. Snapshot old state into locals:

```c
oldTrackPattern[i] = seq_perTrackActivePattern[i];
oldActivePattern = seq_activePattern;
newActivePattern = seq_normalizePatternNumber(seq_pendingPattern);
activePatternChanged = (oldActivePattern != newActivePattern);
```

4. Commit global active pattern:

```c
seq_activePattern = newActivePattern;
```

5. Update global temp-kit active state:

```c
seq_setTmpKitActive(seq_activePattern == SEQ_TMP_PATTERN);
```

This may:

- set `seq_tmpKitActive` to `1` or `0`;
- lazily create `seq_tmpKitState` if temp state does not yet exist;
- sync `seq_vMorphAmount[]` mirrors from the selected live sources;
- invalidate the live morph apply cache for the selected image;
- queue full endpoint restore to AVR for temp/normal menu sync.

6. Mark a pattern switch has executed:

```c
seq_newPatternExecuted = 1;
```

7. Consume pending track state:

If `seq_loadPendigFlag` is set:

```c
frontParser_applyDeferredVoiceCache();
euklid_clearRotation();
for(i = 0; i < NUM_TRACKS; i++)
   seq_perTrackActivePattern[i] =
      seq_normalizePatternNumber(seq_perTrackPendingPattern[i]);
```

If `seq_loadPendigFlag` is not set:

```c
euklid_clearRotation();
for(i = 0; i < NUM_TRACKS; i++)
   seq_perTrackActivePattern[i] =
      seq_normalizePatternNumber(seq_pendingPattern);
```

For user-initiated normal/temp switching, the `seq_loadPendigFlag` path is the expected path.

8. Update per-voice normal/temp source state:

```c
seq_updateVoiceSourcesForPatternChange(oldTrackPattern,
                                       !activePatternChanged);
```

This compares old vs new per-track pattern source for each synth voice.

For each changed synth voice:

```c
seq_markVoiceSourceTarget(synthVoice, newUseTmp);
```

That can:

- set `seq_voiceSourceState[synthVoice]`;
- update `seq_vMorphAmount[synthVoice + 1]`;
- call `seq_applyVoiceSource(synthVoice, useTmp)`;
- therefore immediately apply the selected normal/temp voice parameter image to the live DSP voice.

Endpoint push-up detail:

- If the global active pattern changed, `activePatternChanged` is true and this call does **not** request per-voice endpoint push-up, because full endpoint push-up was already queued by `seq_setTmpKitActive(...)`.
- If only per-track sources changed while the global active pattern stayed the same, `activePatternChanged` is false and per-voice endpoint push-up can be queued here.

9. Detect whether a temp boundary happened:

```c
tmpBoundaryPatternChanged =
   (seq_trackPatternUsesTmp(oldActivePattern)
    != seq_trackPatternUsesTmp(newActivePattern));

for(i = 0; i < NUM_TRACKS; i++)
{
   if(seq_trackPatternUsesTmp(oldTrackPattern[i])
      != seq_trackPatternUsesTmp(seq_perTrackActivePattern[i]))
      tmpBoundaryPatternChanged = 1;
}
```

If yes:

```c
seq_tmpBoundaryPatternSwitchAck = 1;
```

This is later consumed when AVR/front asks to set/show the same pattern and the STM decides whether to realign or just acknowledge the temp-boundary switch.

10. Reset/adjust pattern position:

If boundary-aligned:

```c
seq_setStepIndexToStart();
```

If instant:

```c
seq_loadSeqNow = 0;
if(seq_resetBarOnPatternChange)
   seq_barCounter = -1;
```

11. Send external/status messages:

```c
seq_sendProgChg(seq_activePattern);
voiceControl_noteOff(0xFF);
uart_sendFrontpanelByte(FRONT_SEQ_CC);
uart_sendFrontpanelByte(FRONT_SEQ_CHANGE_PAT);
uart_sendFrontpanelByte(seq_activePattern);
```

This means:

- MIDI/USB program change may be queued.
- STM MIDI active-note bookkeeping is cleared.
- AVR receives pattern-change ACK.

12. Clear the pending switch flag:

```c
seq_loadPendigFlag = 0;
```

13. Realign only if not a temp boundary:

```c
if(!tmpBoundaryPatternChanged)
   seq_realign();
```

Temp-boundary switches intentionally skip `seq_realign()`.

## Flags And Variables Set Before `seq_tick()` Consumes Them

Set immediately by the AVR switch command:

- `seq_pendingPattern`
  - whole-pattern only.
- `seq_perTrackPendingPattern[]`
  - whole-pattern, per-track, and hi-hat paired cases.
- `seq_loadPendigFlag = 1`
  - consumed by the switch block.
- `seq_loadSeqNow = 1`
  - consumed by the instant-switch timing gate if `switchOnNextStep` is enabled.

Parser message variables used to call the switch:

- `frontParser_midiMsg.status`
- `frontParser_midiMsg.data1`
- `frontParser_midiMsg.data2`
- `frontParser_rxCnt`

## Variables Changed When `seq_tick()` Consumes The Switch

Core switch variables:

- `seq_activePattern`
- `seq_perTrackActivePattern[]`
- `seq_tmpKitActive`
- `seq_newPatternExecuted`
- `seq_tmpBoundaryPatternSwitchAck`
- `seq_loadPendigFlag`
- `seq_loadSeqNow`
- `seq_barCounter`
- `seq_stepIndex[]`

Per-voice source variables:

- `seq_voiceSourceState[]`
- `seq_vMorphAmount[]`
- `seq_liveMorphAppliedKnown[][]`
- possibly `seq_liveMorphAppliedValue[][]` cache relevance through invalidation

Kit state variables:

- `seq_tmpKitState` if lazily captured.
- `seq_normalKitState` is normally read, not modified by the switch itself.

Endpoint/menu restore queue:

- `seq_endpointRestoreQueue[]`
- `seq_endpointRestoreQueueCount`
- possibly tail request `voiceMask`

MIDI/front output bookkeeping:

- `active_voices` in `MidiVoiceControl.c`, cleared by `voiceControl_noteOff(0xFF)`.
- `midi_notes_on`, cleared by `seq_midiNoteOff(0xff)`.
- front-panel TX FIFO receives the pattern-change ACK.
- MIDI TX FIFO may receive program change and note-off messages.

## Why This Matters For The Current Glitch

The crucial handoff is:

```c
seq_updateVoiceSourcesForPatternChange(...)
  -> seq_markVoiceSourceTarget(...)
     -> seq_voiceSourceState[synthVoice] = targetState;
     -> seq_selectVoiceMorphAmountFromKit(...);
     -> seq_applyVoiceSource(...);
```

`seq_applyVoiceSource(...)` immediately applies the selected normal/temp parameter image to the live DSP voice.

So the switch is not only a read-source selection. It is currently also a live DSP voice rebuild for every synth voice whose normal/temp source changed.

That is the current leading suspect for the boundary sound.
