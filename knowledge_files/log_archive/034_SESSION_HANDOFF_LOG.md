# 034 Session Handoff Log - Shift Toggle And Step Automation Reset

DATE: 2026-07-04

## Session Goal

This session started with a global front-panel SHIFT-toggle bug: when the global
setting made SHIFT a latch, the LED and some page paint paths behaved as
shifted, but much of the menu/button code still read the raw physical SHIFT
button. The requested behavior was a true button-level override: when the
global shift latch is active, all code should act exactly as if SHIFT is held.

The session then moved to step automation lifetime. Step automation values were
sticking after the automated step fired because the sequencer only restored the
old value when an automation node destination changed. The user wanted step
automation to be a one-step override: release the automated destination on the
next active step for that track, and also release when the lane target changes
through the menu or live recording.

The final focus was `PAR_VOICE_DECIMATION_ALL` / global SampleRt. Generic
per-track automation release was not enough for this shared global parameter.
The desired behavior was: capture the pre-automation global decimation value,
remember the track/lane that last set it from step automation, and release only
when that owning track fires another active step. Hardware testing then exposed
a separate baseline problem: the release path worked, but it restored to `0`
because STM `/Preset/` canonical storage could still retain global decimation
as zero.

## Completed

### 1. Global SHIFT Toggle Is Now An Effective Button-Level State

The audit is in `SHIFT_TOGGLE_AUDIT.md`.

Implemented behavior:

- `buttonHandler_getShift()` now returns the effective latched SHIFT state
  instead of re-reading the raw DIN mirror for `BUT_SHIFT`.
- `buttonHandler_handleShift()` routes momentary and toggle mode through one
  state-transition helper.
- Toggle mode changes the effective SHIFT state only on the physical press edge;
  release is ignored.
- External code that directly read `shiftState` now uses
  `buttonHandler_getShift()`, so mode entry, menu repaint, encoder routing, and
  comms LED parsing see the same effective state.
- `shiftState` is no longer exported as a public global.

Important intent:

- The raw physical SHIFT switch should only be read by the shift-button
  toggler.
- Everything else should ask for effective SHIFT state.
- This is what keeps a latched SHIFT from painting a shifted page while routing
  later controls through unshifted behavior.

Build verification recorded in the audit:

- `make -C front/LxrAvr avr -j4` passed.
- `make firmware` passed.

### 2. Ordinary Step Automation Is Now A One-Step Override

The main audit and implementation notes are in `STEP_AUTOMATION_AUDIT.md`.

Implemented behavior:

- Added `preset_getLiveParameterBaseline()` so step automation release reads
  the current STM `/Preset/` baseline from `PresetKitState.interpolatedParams[]`
  instead of the legacy AVR/front-panel original-value mirror.
- Added `autoNode_release()` and `autoNode_getDestination()`.
- `autoNode_release()` restores the current raw destination through
  `preset_applySingleParameterValue()` and then clears the node.
- Added Sequencer runtime release tracking:
  - `seq_automationPendingRelease[track][lane]` for ordinary parameters.
  - `seq_voiceMorphAutomationPendingRelease[track][lane]` for voice morph
    automation.
- `seq_nextStep()` releases pending automation as soon as a track reaches its
  next active step, before probability, mute, roll, or trigger decisions can
  skip the old trigger path.
- `seq_parseAutomationNodes()` now applies lanes through
  `seq_applyAutomationLane()`, which marks a lane pending release only when a
  destination is actually applied.
- Added `seq_setStepAutomationDestination()` so menu edits and live/armed
  recording destination overwrites release a currently held old destination.
- `FRONT_SET_P1_DEST` / `FRONT_SET_P2_DEST`, `seq_recordAutomation()`, and
  armed-step recording route through that Sequencer helper.
- `FRONT_SEQ_CLEAR_AUTOM`, `seq_setRunning(0)`, and pattern changes release
  held automation so old overrides cannot leak across clear/stop/pattern
  boundaries.
- Added `preset_releaseVoiceMorphAutomationValue()` so voice morph step
  automation releases back to the voice morph base amount instead of the
  generic parameter path.

Important boundary:

- Pattern storage still follows the Session 027 convention:
  `Step.param1Nr` / `param2Nr` store raw AVR/menu `PAR_*` ids.
- Low `+1` conversion still belongs only at live apply/playback boundaries.

Build verification recorded in the audit:

- `make -C mainboard/LxrStm32 -j4 stm32` passed.
- `make firmware` passed.

### 3. Global Decimation Step Automation Has Dedicated Owner Tracking

The generic lane-local automation model was wrong for
`PAR_VOICE_DECIMATION_ALL` because that parameter writes one shared mixer slot:
`mixer_decimation_rate[6]`.

Implemented behavior:

- Added Sequencer runtime state:
  - `seq_globalDecimAutomationActive`
  - `seq_globalDecimAutomationOwnerTrack`
  - `seq_globalDecimAutomationOwnerLane`
  - `seq_globalDecimAutomationRestoreValue`
- `seq_applyAutomationLane()` special-cases `PAR_VOICE_DECIMATION_ALL` before
  ordinary automation and voice morph automation.
- Global decimation step automation bypasses generic `AutomationNode`
  ownership.
- The first currently-held global decimation automation captures
  `preset_getLiveParameterBaseline(PAR_VOICE_DECIMATION_ALL)`.
- If another track/lane later automates global decimation before release, only
  the owner track/lane and live value change. The original restore value is
  retained.
- Only the last owning track can release the shared global override on its next
  active step.
- Clearing/changing a non-owning lane that also contains global decimation does
  not release the currently held global value.
- Clearing/changing the owning lane releases immediately.
- Stop and pattern change release any held global decimation override.

Important implementation detail:

- The global decimation branch does not call `seq_releaseAutomationLane()` before
  applying a new global value. That would let a repeat global automation hit
  from the same lane restore, then recapture the automated value as the new
  baseline. Instead it clears ordinary node state and explicitly releases any
  pending voice morph override on that lane.

Hardware result:

- User testing showed global decimation set/unset correctly, which confirmed
  the owner/release path was basically right.
- The remaining problem was the captured restore baseline being `0`.

### 4. STM `/Preset/` Now Protects The Global Decimation Baseline

The baseline bug was not in the new owner logic. It was in canonical storage:
step automation releases global decimation to
`PresetKitState.interpolatedParams[PAR_VOICE_DECIMATION_ALL]`, and STM could
still leave that byte as `0` even though the mixer and AVR menu default global
decimation to full rate.

Implemented behavior:

- Added `preset_normalizeStoredParameterValue(param, value)` in
  `ParameterArray.c/.h`.
- Stored endpoint value `PAR_VOICE_DECIMATION_ALL == 0` is interpreted as `127`
  when it is going into canonical Preset storage or being used to rebuild
  endpoint-derived caches.
- This normalization is intentionally not applied to the live step-automation
  apply value, so a step can still automate global SampleRt to `0` if desired.
- `preset_refreshInterpolatedParamsFromEndpoints()` normalizes endpoint bytes
  before using them to rebuild `interpolatedParams[]`.
- `preset_storeParameterIngress()` updates `interpolatedParams[]` immediately
  for shared/current-image writes. This is the menu baseline fix: changing
  global SampleRt in the menu now updates the exact value step automation will
  restore to.
- Normal endpoint restore ingress normalizes stored values and updates shared
  `interpolatedParams[]` as values arrive.
- `FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN` seeds global decimation endpoint and
  interpolated slots to `127` immediately after zeroing endpoint arrays, so
  older or partial files that never send the parameter cannot leave the STM
  baseline at zero.
- `seq_init()` seeds both STM Preset images' global decimation endpoint and
  interpolated slots to `127`, matching the mixer and AVR menu startup default.

Hardware result:

- The user reported the final behavior seemed good after this follow-up.

## Verification

Builds run during this session:

- `make -C front/LxrAvr avr -j4` passed during the shift-toggle implementation
  pass, with the existing AVR warning set.
- `make -C mainboard/LxrStm32 -j4 stm32` passed during the step automation and
  global decimation passes, with the existing STM warning set.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.
- `git diff --check` passed after the final global decimation baseline changes.

Hardware/user verification:

- Global decimation step automation owner/release behavior was tested by the
  user and found to set/unset correctly.
- The final baseline fix was accepted by the user as "seems good."
- The global SHIFT-toggle implementation was build-verified; no explicit final
  hardware confirmation was recorded in this closeout.

## Repository State At Session End

Current branch:

- `master`

Current local modified files at wrap:

- `STEP_AUTOMATION_AUDIT.md`
- `firmware image/FIRMWARE.BIN`
- `mainboard/LxrStm32/src/Preset/KitState.c`
- `mainboard/LxrStm32/src/Preset/ParameterArray.c`
- `mainboard/LxrStm32/src/Preset/ParameterArray.h`
- `mainboard/LxrStm32/src/Preset/ParameterIngress.c`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
- `knowledge_files/comms_spec_reference/BACKGROUND_LOAD_TEMPORARY.md`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`
- `knowledge_files/log_archive/034_SESSION_HANDOFF_LOG.md`
- `MEMORY.md`

No MIDI table update was needed; no external MIDI mapping changed.

## End Of Session Block

```
DATE: 2026-07-04
SESSION GOAL: Fix global SHIFT toggle as a true button-level latch, then fix step automation so automated values release correctly, including the special global decimation case.
COMPLETED: Implemented effective SHIFT latch semantics, made ordinary step automation release from STM Preset baselines on the next active step/target change/clear/stop/pattern change, special-cased global decimation automation with last-owner release, and fixed STM global decimation baseline storage so release returns to the current menu value instead of zero.
VERIFIED ON HARDWARE: partial. User tested global decimation step automation and accepted the final restore-baseline behavior. SHIFT-toggle was build-verified but not explicitly hardware-confirmed in this closeout.

CHANGES THIS SESSION:
- `front/LxrAvr/buttonHandler.c/.h`: SHIFT getter now returns effective latched state; shift state transition is centralized; raw DIN SHIFT is only used by the toggler; shiftState is no longer exported.
- `front/LxrAvr/Menu/menu.c`: external shifted UI/menu paths use effective SHIFT state.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`: shifted LED/packet parsing gates use effective SHIFT state.
- `mainboard/LxrStm32/src/DSPAudio/automationNode.c/.h`: added explicit release/get-destination helpers using STM Preset baselines.
- `mainboard/LxrStm32/src/Preset/ParameterIngress.c/.h`: added live baseline read/apply support for automation release; shared live/menu writes now refresh `interpolatedParams[]`; stored global decimation zero is normalized on endpoint ingress.
- `mainboard/LxrStm32/src/Preset/MorphEngine.c/.h`: added voice morph automation release back to base morph amount.
- `mainboard/LxrStm32/src/Preset/ParameterArray.c/.h`: added `preset_normalizeStoredParameterValue()` for canonical stored endpoint values.
- `mainboard/LxrStm32/src/Preset/KitState.c`: normalizes endpoint bytes when rebuilding `interpolatedParams[]`.
- `mainboard/LxrStm32/src/Sequencer/sequencer.c/.h`: added one-step automation release tracking, destination-change release helper, global decimation owner tracking, stop/pattern release, and STM global decimation startup baseline seeding.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`: routes step automation target edits through Sequencer release helpers, releases active lanes before clear, and seeds global decimation to 127 after endpoint-restore zeroing.
- `SHIFT_TOGGLE_AUDIT.md`: written with findings, plan, implementation notes, and build verification.
- `STEP_AUTOMATION_AUDIT.md`: written/extended with ordinary step automation reset plan, global decimation owner plan, STM baseline follow-up, and verification.
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`: updated to record Session 034 step-automation and global decimation storage-baseline rules.
- `knowledge_files/comms_spec_reference/BACKGROUND_LOAD_TEMPORARY.md`: updated to record STM-side global decimation storage normalization.

KNOWN ISSUES INTRODUCED: None confirmed. SHIFT-toggle still wants a final hardware smoke test if not already covered outside the transcript.
KNOWN ISSUES RESOLVED: Global SHIFT-toggle no longer splits latched UI paint from raw-button routing; step automation no longer sticks permanently after a step fires; manual/recorded target changes release the old held target; global decimation step automation no longer releases from the wrong track and no longer restores to STM baseline zero.

NEXT SESSION RECOMMENDED GOAL: Hardware smoke-test global SHIFT-toggle and a small matrix of step automation targets, then decide whether to prune the root audit docs after commit.
BLOCKERS: None for build. Hardware confirmation is still useful for SHIFT-toggle and broader non-decimation step automation targets.

CRITICAL REMINDERS FOR NEXT SESSION:
- Step automation destinations remain raw AVR/menu `PAR_*` ids in pattern storage; do not reintroduce apply-domain low `+1` ids there.
- Step automation release baselines come from STM `/Preset/` `interpolatedParams[]`, not AVR/front-panel original-value mirrors.
- `PAR_VOICE_DECIMATION_ALL` is shared global state; keep its step automation on the dedicated last-owner path instead of generic per-lane `AutomationNode` release.
- Stored/canonical `PAR_VOICE_DECIMATION_ALL == 0` means `127`, but live step automation values are not normalized.
- Global SHIFT toggle is meant to be a button-level override; all non-toggler code should ask `buttonHandler_getShift()`.
```
