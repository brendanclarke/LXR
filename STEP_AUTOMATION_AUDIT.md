# Step Automation Reset Audit

## Goal

Step automation should behave like a one-step override. When a step fires and applies an automated parameter value, that value must not become the new permanent live parameter value. The automated destination should be restored to its current kit/original value on the next active step for that track, and also immediately when the step automation target is changed by the menu or by live recording a different parameter into that step/lane.

This proposal preserves the Session 027 storage convention: `Step.param1Nr` and `Step.param2Nr` store raw AVR/menu `PAR_*` ids. Playback is still responsible for converting raw low destinations to the `MIDI_CC data1 = destination + 1` shape expected by `frontParser_applyParameterCommand()`.

## Current Behavior

Playback runs step automation from [sequencer.c](/Users/bc/LXR01/LXR-current/LXR/mainboard/LxrStm32/src/Sequencer/sequencer.c:328):

```c
static void seq_parseAutomationNodes(uint8_t track, Step* stepData)
{
   uint8_t param1 = stepData->param1Nr;
   uint8_t param2 = stepData->param2Nr;
   uint8_t val1 = stepData->param1Val;
   uint8_t val2 = stepData->param2Val;
   ...
   autoNode_setDestination(&seq_automationNodes[track][0], param1);
   autoNode_setDestination(&seq_automationNodes[track][1], param2);
   autoNode_updateValue(&seq_automationNodes[track][0], val1);
   autoNode_updateValue(&seq_automationNodes[track][1], val2);
}
```

That is called only from `seq_triggerVoice()` immediately before the voice is triggered: [sequencer.c](/Users/bc/LXR01/LXR-current/LXR/mainboard/LxrStm32/src/Sequencer/sequencer.c:433).

The reset mechanism is inside [automationNode.c](/Users/bc/LXR01/LXR-current/LXR/mainboard/LxrStm32/src/DSPAudio/automationNode.c:60). When the node destination changes, it applies `frontParser_originalCcValues[...]` for the previous destination before storing the new destination:

```c
void autoNode_setDestination(AutomationNode* node, uint16_t dest)
{
   if(node->destination != NO_AUTOMATION)
   {
      MidiMsg msg;

      if(autoNode_makeParameterMessage(node->destination, &msg))
      {
         msg.data2 = frontParser_originalCcValues[node->destination];
         frontParser_applyParameterCommand(msg,0);
      }
   }

   node->destination = (dest == 0) ? NO_AUTOMATION : dest;
}
```

Automation values are applied with `frontParser_applyParameterCommand(..., 0)`, so they intentionally do not update `frontParser_originalCcValues[]` or preset storage.

## Findings

1. Reset is passive and destination-change driven.

The only current release path is `autoNode_setDestination()`. If no later step causes the node to move to a different destination or to `NO_AUTOMATION`, the last automated value stays live.

2. Reset depends on a later note-trigger path.

`seq_parseAutomationNodes()` is called from `seq_triggerVoice()`, not directly from the step-advance code. If the next active step does not actually trigger the voice path, for example because of probability or because the track does not hit another active note soon, the stale automation remains live longer than intended.

3. Manual target edits do not release the previously active destination immediately.

Manual destination changes write directly into pattern storage in [frontPanelReceivingProtocol.c](/Users/bc/LXR01/LXR-current/LXR/mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:2036):

```c
pat_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, seq_selectedStep)->param1Nr = val;
```

and similarly for lane 2. That changes the stored step, but it does not tell the currently live `seq_automationNodes[track][lane]` to restore the old destination if that lane is presently holding an automation override.

4. Live recording can overwrite a lane without releasing the old destination.

`seq_recordAutomation()` writes `step->param1Nr = dest` or `step->param2Nr = dest` directly: [sequencer.c](/Users/bc/LXR01/LXR-current/LXR/mainboard/LxrStm32/src/Sequencer/sequencer.c:1609). If the lane previously automated another destination and that destination is currently held live, overwriting the step does not immediately restore the old destination.

5. The original-value lookup in `automationNode.c` is suspicious for low raw destinations.

`autoNode_makeParameterMessage()` correctly converts raw low destinations to `MIDI_CC data1 = destination + 1`. However, `frontParser_applyParameterCommand()` stores low original values at `frontParser_originalCcValues[paramNr + 1]`, where `paramNr = msg.data1 - 1`: [frontPanelReceivingProtocol.c](/Users/bc/LXR01/LXR-current/LXR/mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:286). The reset code currently reads `frontParser_originalCcValues[node->destination]`.

For raw low destinations, that appears one slot too low. The reset should use the same domain as the generated apply message, or preferably a helper owned by the front-panel apply layer. High `MIDI_CC2` destinations appear to line up because their original-value index is the raw high destination.

## Proposed Fix

Keep the fix on the STM side. AVR protocol and pattern storage do not need to change.

### Preset ownership refinement

The reset baseline should not come from the AVR, and it should not come from
the legacy `frontParser_originalCcValues[]` mirror. Canonical parameter storage
now lives in STM `/Preset/`:

- `PresetKitState.kitEndpointParams[]` stores the front/current endpoint image.
- `PresetKitState.morphEndpointParams[]` stores the alternate morph endpoint.
- `PresetKitState.interpolatedParams[]` stores the current live baseline after
  morph interpolation.

For ordinary step-automated sound parameters, the reset value should be the
current image's `interpolatedParams[param]`, not the raw endpoint byte. That
way a step automation override releases back to the value the instrument should
currently have, including active morph state, temp/normal image ownership, and
background temp playback routing. No AVR round trip is needed.

Recommended Preset helper:

```c
uint8_t preset_getLiveParameterBaseline(uint16_t param)
{
   const PresetKitState *kit = preset_getCurrentImageKitState();

   if(param >= END_OF_SOUND_PARAMETERS)
      return 0;

   return kit->interpolatedParams[param];
}
```

If voice-source split matters for temp playback, the helper should mirror the
same image-selection rule used by `preset_storeParameterIngress()` and
`preset_applyVoiceParameterValues()`: voice-owned params read from the kit image
selected for that voice, while shared params read from the active current image.
That selection policy belongs in Preset, not in `automationNode.c`.

For voice-morph step automation (`PAR_MORPH_DRUM1` through
`PAR_MORPH_HIHAT`), the generic parameter baseline is not enough because that
path is handled as control state by `preset_setVoiceMorphAutomationValue()`.
The existing `PresetKitState.voiceMorphBaseAmount[]` /
`voiceMorphAmount[]` split is enough persistent/live storage; the fix only
needs a release helper such as:

```c
void preset_releaseVoiceMorphAutomationValue(uint8_t synthVoice)
{
   PresetKitState *kit = preset_getMorphKitForImage(
      preset_getMorphImageForVoice(synthVoice));

   if(!kit || synthVoice >= PRESET_SYNTH_VOICES)
      return;

   preset_setVoiceMorphLiveAmount(synthVoice,
                                  kit->voiceMorphBaseAmount[synthVoice]);
   frontPanelSending_sendVoiceMorphRuntimeReport(
      synthVoice,
      kit->voiceMorphBaseAmount[synthVoice]);
}
```

So: no additional persistent `/Preset/` storage appears necessary for the
ordinary step automation reset. What is needed is a small read/apply API from
Preset that exposes the already-canonical baseline, plus a runtime pending
release bitset in Sequencer for step-automation lifetime.

### 1. Give `AutomationNode` an explicit release API

Add a small public helper to [automationNode.h](/Users/bc/LXR01/LXR-current/LXR/mainboard/LxrStm32/src/DSPAudio/automationNode.h:48):

```c
void autoNode_release(AutomationNode* node);
uint16_t autoNode_getDestination(const AutomationNode* node);
```

`autoNode_release()` should restore the current `node->destination` to the original value and then set `node->destination = NO_AUTOMATION`. `autoNode_setDestination()` should call this helper before storing a different destination.

This makes release a first-class operation instead of hiding it inside destination assignment.

### 2. Replace original-value lookup with Preset baseline lookup

Do not keep indexing `frontParser_originalCcValues[]` directly from raw step destinations. The first version of this audit noted the low-destination `+1` mismatch in that legacy mirror; the cleaner fix is to remove that dependency for step automation reset entirely.

`autoNode_release()` should ask Preset for the current baseline:

```c
static uint8_t autoNode_baselineForDestination(uint16_t destination)
{
   if(destination == 0 || destination == NO_AUTOMATION)
      return 0;

   return preset_getLiveParameterBaseline(destination);
}
```

That keeps the reset path in STM-owned canonical storage and avoids a hidden
AVR/front-panel feedback loop. `autoNode_makeParameterMessage()` can still be
used to shape the eventual live apply into `MIDI_CC` / `MIDI_CC2`, or this can
be simplified further by having `autoNode_release()` call
`preset_applySingleParameterValue(destination, baseline)` directly.

### 3. Release stale automation at the next active step

Add sequencer-level state that tracks whether each track/lane has a pending one-step automation override. The existing `seq_automationNodes[NUM_TRACKS][2]` can represent the live destination, but the sequencer needs an explicit "this was applied by the previous fired step and must be released on the next active step" rule.

Recommended flow:

```c
static uint8_t seq_automationPendingRelease[NUM_TRACKS][2];

static void seq_releasePendingAutomationForTrack(uint8_t track)
{
   uint8_t lane;
   for(lane = 0; lane < 2; lane++)
   {
      if(seq_automationPendingRelease[track][lane])
      {
         autoNode_release(&seq_automationNodes[track][lane]);
         seq_automationPendingRelease[track][lane] = 0;
      }
   }
}
```

On each active step for a track, before applying that step's automation, call `seq_releasePendingAutomationForTrack(track)`.

Then, when the current step actually applies lane automation, mark the lane pending release:

```c
autoNode_setDestination(&seq_automationNodes[track][lane], dest);
autoNode_updateValue(&seq_automationNodes[track][lane], value);
seq_automationPendingRelease[track][lane] = (dest != 0 && dest != NO_AUTOMATION);
```

The placement should be in `seq_nextStep()` around the active-step block, not only inside `seq_triggerVoice()`, so a next active step can release stale automation even if probability prevents a new voice trigger. To preserve existing "automation fires with steps" behavior, split the operation:

- release pending automation as soon as the track reaches the next active step;
- apply the new step's automation only on the same condition currently used for note firing, unless we intentionally decide that step automation should fire even when probability suppresses the note.

If we want the smallest behavior change, keep new automation application in `seq_triggerVoice()` and add only the pending release before the probability gate. That will fix stuck values without making probability-skipped steps apply new automation.

### 4. Release immediately when manual target edit overwrites the active lane

Add sequencer API:

```c
void seq_setStepAutomationDestination(uint8_t pattern,
                                      uint8_t track,
                                      uint8_t step,
                                      uint8_t lane,
                                      uint8_t dest);
```

This helper should:

1. get the `Step*`;
2. remember the old destination for the lane;
3. write the new destination;
4. if `pattern == seq_perTrackActivePattern[track]`, `step == seq_stepIndex[track]` or the currently held `seq_automationNodes[track][lane]` matches the old destination, call `autoNode_release(&seq_automationNodes[track][lane])` and clear `seq_automationPendingRelease[track][lane]`.

Then replace the direct writes in `FRONT_SET_P1_DEST` / `FRONT_SET_P2_DEST` with this helper.

The matching rule can be conservative: if the active node's destination equals the old destination, release it. That avoids needing to prove the edited step is the one that originally fired. It also matches the user's expectation that changing a target stops the old target from being held.

### 5. Release immediately when live recording overwrites a different target

Route `seq_recordAutomation()` through the same helper when writing `param1Nr` / `param2Nr`.

If the lane's old destination differs from the new `dest`, release the matching active node before or immediately after storing the new target. Then write the new value as today.

This covers both record-active quantized writes and armed-step writes.

### 6. Reset on broader transport/pattern boundaries

For completeness, release all active automation nodes when:

- sequencer stops;
- pattern changes;
- temp/normal pattern source boundary changes;
- track/pattern automation is cleared through `pat_clearAutomation()`, if the cleared pattern is currently active for that track.

The immediate bug report does not require all of these to land in the first patch, but they are the same lifetime problem. At minimum, pattern switches should call a `seq_releaseAllAutomation()` helper so an override from the old pattern cannot leak into the next pattern.

## Suggested Implementation Order

1. Add `autoNode_release()` and fix original-value lookup for raw low destinations.
2. Add `seq_releasePendingAutomationForTrack()` and the pending-release flags.
3. Call pending release from the active-step path in `seq_nextStep()` before probability/new trigger decisions.
4. Mark lanes pending release when automation is applied.
5. Add `seq_setStepAutomationDestination()` and route manual `FRONT_SET_P1_DEST` / `FRONT_SET_P2_DEST` through it.
6. Route destination overwrites in `seq_recordAutomation()` through the same helper.
7. Add `seq_releaseAllAutomation()` at stop/pattern switch if hardware testing still shows cross-pattern leakage.

## Verification Plan

Build:

```bash
make -C mainboard/LxrStm32 -j4 stm32
make firmware
```

Hardware tests:

1. Put automation on step 1 for a clearly audible low parameter, e.g. a volume/filter parameter. Let step 1 fire, then let the next active step with no automation fire. The parameter should return to its menu value.
2. Repeat with a high `CC2` destination to confirm high destinations still reset.
3. Repeat with a low destination to confirm the suspected original-value index issue is fixed.
4. While the automated value is currently held, change that step's automation target in the step menu. The old parameter should immediately return to its menu value.
5. While the automated value is currently held, live-record a different parameter into the same step/lane. The old parameter should immediately return, and the new destination/value should be stored.
6. Test probability: if the next active step is probability-skipped, stale automation should still release. New automation on the skipped step should not apply unless we explicitly choose that behavior.
7. Test pattern change and stop/start for leakage from an automated old pattern into the next pattern.

## Implementation Notes

Implementation pass notes:

- Added `preset_getLiveParameterBaseline()` in `Preset/ParameterIngress`. It reads the STM-owned current baseline from `PresetKitState.interpolatedParams[]`, using the same normal/temp voice-source selection policy as live parameter ingress. This removes the step-automation reset dependency on `frontParser_originalCcValues[]`.
- Added `preset_releaseVoiceMorphAutomationValue()` in `Preset/MorphEngine`. Voice morph automation is control state, so it releases back to the existing `voiceMorphBaseAmount[]` rather than through the generic parameter apply path.
- Added `autoNode_release()` and `autoNode_getDestination()` in `DSPAudio/automationNode`. `autoNode_release()` applies the Preset baseline through `preset_applySingleParameterValue()` and clears the node destination to `NO_AUTOMATION`.
- Added Sequencer runtime release tracking: `seq_automationPendingRelease[track][lane]` for generic parameter automation and `seq_voiceMorphAutomationPendingRelease[track][lane]` for voice morph automation.
- `seq_nextStep()` now releases pending one-step automation as soon as the track reaches its next active step, before probability, mute, roll, or trigger decisions can skip the old `seq_triggerVoice()` path.
- `seq_parseAutomationNodes()` now applies each lane through `seq_applyAutomationLane()`, which marks lanes pending release only when automation actually applies.
- Added `seq_setStepAutomationDestination()` so manual target edits and live/armed recording writes release a currently held old target when the lane destination changes.
- `FRONT_SET_P1_DEST` / `FRONT_SET_P2_DEST` now route through `seq_setStepAutomationDestination()` instead of writing `Step.param*Nr` directly.
- `seq_recordAutomation()` now routes both record-active and armed-step destination writes through `seq_setStepAutomationDestination()`.
- Added `seq_releaseAutomationLane()` as a public boundary helper. The `FRONT_SEQ_CLEAR_AUTOM` path releases the active lane before clearing it when the shown pattern is the live source for that track.
- `seq_setRunning(0)` and pattern changes now call `seq_releaseAllAutomation()` so stopped playback or a new pattern cannot inherit a held step-automation override.

Open verification points:

- Confirm on hardware that low raw destinations release to the correct Preset baseline, since this intentionally bypasses the legacy `frontParser_originalCcValues[]` mirror.
- Confirm voice morph step automation releases to the expected base morph amount after the next active step.

Build verification from this implementation pass:

- `make -C mainboard/LxrStm32 -j4 stm32` passes. The build still reports the existing STM warning noise in mixer, oscillator, modulation node, sequencer init bounds, TriggerOut memset, main `_exit`, MIDI parser fallthrough, and linker RWX segment.
- `make firmware` passes and rebuilds `firmware image/FIRMWARE.BIN` with the updated STM binary.

## Addendum: Global Decimation Automation

User-requested behavior for `PAR_VOICE_DECIMATION_ALL` / `SampleRt`:

- retain the global decimation value that was active before step automation;
- remember whichever track most recently set global decimation from step automation;
- release back to the retained pre-automation value when that owning track fires another active step.

### Current Implementation Shape

The front-panel menu sends all decimation parameters through the same path:

```c
avrComms_sendData(SEQ_CC, SEQ_SET_ACTIVE_TRACK,
                  ((uint8_t)(paramNr - PAR_VOICE_DECIMATION1)));
avrComms_sendData(VOICE_CC, VOICE_DECIMATION, value);
```

For `PAR_VOICE_DECIMATION_ALL`, that active-track value is `6`, and the STM
apply path writes mixer slot `msg.data1 - VOICE_DECIMATION1`:

```c
case VOICE_DECIMATION_ALL:
   mixer_decimation_rate[msg.data1 - VOICE_DECIMATION1] =
      valueShaperI2F(msg.data2, -0.7f);
   break;
```

`mixer_decimation_rate[6]` is the global multiplier used by all voices:

```c
mixer_decimation_cnt[voiceNr] +=
   mixer_decimation_rate[voiceNr] * mixer_decimation_rate[6];
```

Step automation reaches this same raw destination through the Session 027 raw
`PAR_*` convention. `PAR_VOICE_DECIMATION_ALL` is also listed as automation
target selector index 1 in `preset_modTargetParams[]`.

### Potential Problems

1. The current generic one-step release is lane/track-local, but global
   decimation is one shared runtime parameter.

   If track 1 automates global decimation, then track 3 automates it before
   track 1 reaches its next active step, track 1's generic pending release could
   restore the global value even though track 3 is now the last writer. That
   violates the desired "whatever track last set it" ownership rule.

2. The pre-automation value needs to be captured once per active global
   automation hold, not recomputed at release.

   `preset_getLiveParameterBaseline(PAR_VOICE_DECIMATION_ALL)` is the right
   source for the value before automation starts, but after automation is active
   we should not overwrite that retained value with the automated value or with
   another track's automated value. If a second track takes ownership, it should
   update the owner track, not the retained pre-automation value.

3. The generic `AutomationNode` per-track state is the wrong owner for this
   destination.

   `AutomationNode` can still convert/apply ordinary destinations, but
   `PAR_VOICE_DECIMATION_ALL` should bypass generic per-track pending release
   and go through one Sequencer-owned global-decimation automation state.

4. Clear/target-change boundaries need to respect last-writer ownership.

   If a non-owning track clears or changes a lane that used to automate global
   decimation, it should not release the current global override. If the owning
   track/lane is changed or cleared, it should release to the retained
   pre-automation value.

### Proposed Special Case

Add Sequencer runtime state:

```c
#define SEQ_AUTOM_OWNER_NONE 0xff

static uint8_t seq_globalDecimAutomationActive;
static uint8_t seq_globalDecimAutomationOwnerTrack;
static uint8_t seq_globalDecimAutomationOwnerLane;
static uint8_t seq_globalDecimAutomationRestoreValue;
```

This state is runtime-only. No additional persistent `/Preset/` storage is
needed because the retained pre-automation value is the current Preset baseline
at the moment the first global-decimation step automation override begins.

Add helpers:

```c
static void seq_applyGlobalDecimationAutomation(uint8_t track,
                                                uint8_t lane,
                                                uint8_t value);
static void seq_releaseGlobalDecimationAutomationForOwner(uint8_t track);
static void seq_releaseGlobalDecimationAutomationIfLane(uint8_t track,
                                                        uint8_t lane);
static void seq_releaseGlobalDecimationAutomation(void);
```

`seq_applyGlobalDecimationAutomation(track, lane, value)`:

1. If `seq_globalDecimAutomationActive == 0`, set
   `seq_globalDecimAutomationRestoreValue =
   preset_getLiveParameterBaseline(PAR_VOICE_DECIMATION_ALL)`.
2. Set active = 1.
3. Set owner track/lane to the track/lane that just applied automation.
4. Apply `value` to `PAR_VOICE_DECIMATION_ALL` through
   `preset_applySingleParameterValue(PAR_VOICE_DECIMATION_ALL, value)`.

If another track applies global decimation while already active, only the owner
track/lane and live value are updated. The retained restore value is left alone.

`seq_releaseGlobalDecimationAutomationForOwner(track)`:

1. If active and owner track equals `track`, apply
   `seq_globalDecimAutomationRestoreValue` to `PAR_VOICE_DECIMATION_ALL`.
2. Clear active and owner fields.

Call this from the same active-step release point that currently calls
`seq_releasePendingAutomationForTrack(i)`, before generic lane release. This
ensures only the track that most recently set the global override can release
it.

`seq_releaseGlobalDecimationAutomationIfLane(track, lane)`:

1. If active and owner track/lane match, release.
2. Otherwise do nothing.

Use this from manual target-change, live-record overwrite, and clear-lane paths.
That keeps non-owning lanes from releasing a global override they no longer own.

`seq_releaseGlobalDecimationAutomation()`:

1. If active, apply restore value and clear state.
2. Use this on stop and pattern change before or alongside
   `seq_releaseAllAutomation()`.

### Integration Points

1. In `seq_applyAutomationLane()`, before the voice-morph special case:

```c
if(param == PAR_VOICE_DECIMATION_ALL)
{
   autoNode_setDestination(&seq_automationNodes[track][lane], 0);
   seq_automationPendingRelease[track][lane] = 0;
   seq_voiceMorphAutomationPendingRelease[track][lane] = 0;
   seq_applyGlobalDecimationAutomation(track, lane, value);
   return;
}
```

This prevents the generic per-track `AutomationNode` from owning global
decimation.

2. In the active-step release point in `seq_nextStep()`:

```c
seq_releaseGlobalDecimationAutomationForOwner(i);
seq_releasePendingAutomationForTrack(i);
```

The order matters less once global decimation bypasses generic lanes, but this
documents the ownership rule: global release is track-owner based.

3. In `seq_releaseLaneIfHoldingDestination()`:

```c
if(oldDest == PAR_VOICE_DECIMATION_ALL)
{
   seq_releaseGlobalDecimationAutomationIfLane(track, lane);
   return;
}
```

4. In `seq_releaseAutomationLane()`:

```c
seq_releaseGlobalDecimationAutomationIfLane(track, lane);
```

This covers clear-lane calls and any explicit lane release.

5. In `seq_releaseAllAutomation()`:

```c
seq_releaseGlobalDecimationAutomation();
```

Then proceed with normal per-track lane releases.

### Verification Additions

1. Put global `SampleRt` step automation on track 1. Let it fire. Confirm it
   releases to the pre-automation value when track 1's next active step fires.
2. Put global `SampleRt` automation on track 1 and track 3 with different
   values. Let track 1 fire, then track 3 fire, then track 1 fire again. Track
   1 must not release the value after track 3 has become owner. The value should
   release only when track 3 reaches its next active step.
3. While global decimation is held by track 3/lane 1, clear or change a
   non-owning track/lane that also contains global decimation automation. It
   should not release the held global value.
4. While global decimation is held by the owning track/lane, change that lane's
   automation target or clear the lane. It should release immediately to the
   retained pre-automation value.
5. Confirm stop and pattern change release any held global decimation value.

### Implementation Notes: Global Decimation Automation

Implemented in [sequencer.c](/Users/bc/LXR01/LXR-current/LXR/mainboard/LxrStm32/src/Sequencer/sequencer.c:176).

The special case now keeps four sequencer-runtime fields:

```c
static uint8_t seq_globalDecimAutomationActive;
static uint8_t seq_globalDecimAutomationOwnerTrack = SEQ_AUTOM_OWNER_NONE;
static uint8_t seq_globalDecimAutomationOwnerLane = SEQ_AUTOM_OWNER_NONE;
static uint8_t seq_globalDecimAutomationRestoreValue;
```

`PAR_VOICE_DECIMATION_ALL` no longer enters the generic
`AutomationNode` ownership path. `seq_applyAutomationLane()` detects that raw
destination, clears any ordinary automation node state for the lane, releases a
pending voice-morph override if that lane was holding one, and calls
`seq_applyGlobalDecimationAutomation()`.

The helper captures
`preset_getLiveParameterBaseline(PAR_VOICE_DECIMATION_ALL)` only when no global
decimation automation is already active. If another track applies global
decimation while it is active, ownership moves to the new track/lane and the
live value changes, but the restore value remains the original pre-automation
value. This matches the intended "last track owns release, first automation
captures restore" behavior.

The active-step release point now calls
`seq_releaseGlobalDecimationAutomationForOwner(i)` before the normal per-lane
release. Only the most recent owner track can release the shared global value
on a later active step; a different track firing cannot accidentally release a
global SampleRt override that it no longer owns.

Manual target changes, live-record target overwrites, and lane clears now flow
through `seq_releaseGlobalDecimationAutomationIfLane(track, lane)` when the old
target is `PAR_VOICE_DECIMATION_ALL`. This makes an owning lane release
immediately, while non-owning lanes that merely contain the same target leave
the current shared override alone.

`seq_releaseAllAutomation()` releases any active global decimation override
before iterating the ordinary track/lane releases. Stop and pattern-change
cleanup therefore restore global SampleRt even if there is no later owning
track step.

One deliberate implementation detail: when a lane applies global decimation,
the code does not call `seq_releaseAutomationLane()` wholesale. That helper
also releases global decimation when the lane is the current owner, which would
allow a repeat global automation hit from the same lane to restore and then
recapture the automated value as its new baseline. Instead the global branch
only clears ordinary `AutomationNode` state and explicitly releases a pending
voice-morph override.

Build verification from the global decimation implementation pass:

- `make -C mainboard/LxrStm32 -j4 stm32` passes. The compiler still reports the
  existing sequencer init bounds warnings and the linker RWX segment warning.
- `make firmware` passes and rebuilds `firmware image/FIRMWARE.BIN` from the
  updated STM binary.

### Follow-up: Global Decimation Restore Baseline

Hardware test showed the special-case owner/release behavior working, but the
released value could still be `0` instead of the value set from the menu. That
pointed away from Sequencer ownership and into `/Preset/` baseline storage:
`seq_applyGlobalDecimationAutomation()` captures
`preset_getLiveParameterBaseline(PAR_VOICE_DECIMATION_ALL)`, and that baseline
comes from `PresetKitState.interpolatedParams[]`.

Remaining ways `PAR_VOICE_DECIMATION_ALL == 0` could be retained:

- STM `seq_init()` zeroed both `preset_tmpKitState` and
  `preset_normalKitState`. The live mixer and AVR menu default global
  decimation to full rate, but the STM canonical preset images did not.
- `FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN` zeroed normal-kit endpoint arrays before
  file/restore traffic. If an older or partial file did not send global
  decimation, the endpoint image kept the zero.
- `preset_storeParameterIngress()` live/current-image writes updated
  `kitEndpointParams[]` and the live shared-parameter cache, but did not update
  `interpolatedParams[]` for shared parameters. Since global decimation is a
  shared parameter, a menu edit could apply live audio while leaving the release
  baseline stale.
- Normal endpoint restore writes could store a raw zero into
  `kitEndpointParams[]`, and shared params are not rebuilt by the per-voice
  morph scanner.

Implemented follow-up:

- Added `preset_normalizeStoredParameterValue()` in
  [ParameterArray.c](/Users/bc/LXR01/LXR-current/LXR/mainboard/LxrStm32/src/Preset/ParameterArray.c:1045).
  For canonical stored endpoints, `PAR_VOICE_DECIMATION_ALL` value `0` is
  interpreted as `127`. This helper is intentionally on stored values, not on
  the live step-automation apply path.
- `preset_refreshInterpolatedParamsFromEndpoints()` now normalizes endpoint
  bytes before using them to rebuild `interpolatedParams[]`.
- `preset_storeParameterIngress()` now updates `interpolatedParams[]` for
  shared/current-image writes, so a menu edit to global SampleRt immediately
  becomes the baseline that step automation restores to.
- Normal endpoint restore ingress normalizes stored values and updates
  `interpolatedParams[]` for shared parameters as they arrive.
- `FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN` seeds the global-decimation endpoint and
  interpolated slots to `127` after zeroing endpoint arrays, so missing fields
  cannot retain the zero default.
- `seq_init()` seeds both STM Preset images' global-decimation endpoint and
  interpolated slots to `127`, matching the mixer and AVR menu startup default.

Build verification from the restore-baseline follow-up:

- `git diff --check` passes.
- `make -C mainboard/LxrStm32 -j4 stm32` passes. It still reports the existing
  mixer/Oscillator/modulationNode/sequencer-init/TriggerOut/main/MIDI/linker
  warning set.
- `make firmware` passes and rebuilds `firmware image/FIRMWARE.BIN` with the
  updated STM binary.
