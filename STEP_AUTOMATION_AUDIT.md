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
