# AUDIT: Move Morph Computation Fully To STM

Status: planning/audit document. No code changes are specified as completed here.

## Goal

Move morph computation out of the AVR front panel and onto the STM so the STM becomes the only owner of live interpolated sound state.

Required behavior:

- AVR remains responsible for UI, file I/O, and holding menu-visible endpoint bytes in `parameter_values[]` and `parameters2[]`.
- STM owns normal and temporary parameter images, morph amounts, interpolation work, live parameter application, and live automation destination application.
- Morph computation must be rate-limited to **exactly one parameter interpolation per STM main loop pass whenever morph work is pending**. There is no user-facing or runtime "off" state for morph, and pending morph work must not be skipped because endpoint restore, menu sync, file load, or any other bulk transfer is in flight. If no dirty morph work exists, the worker has nothing to interpolate and returns.
- This always-on rule applies to STM-side interpolation and modulation. It does not require accepting every incoming global morph command from the AVR/front panel during transition windows.
- File load while the temporary pattern is playing must still not affect the temporary parameter struct or temporary pattern data.
- No extraneous LCD/debug writes should be added.

Terminology guardrail:

- "Morph" means global or voice-level interpolation between kit/front endpoints and morph parameter endpoints.
- Use "morph parameter endpoint" for the `parameters2[]` endpoint image.
- Use "morph automation target endpoint" only for automation destination selector parameters that live in the morph parameter endpoint image.

## Clarified Morph Semantics

These rules supersede the looser wording in earlier audit notes:

- AVR only needs to know global morph.
- Per-voice morph is never displayed or directly set in the AVR menu.
- Global morph can be set by MIDI or by the AVR menu.
- Per-voice morph can be set by MIDI, step automation, or modulation through destinations such as velocity automation target or LFO automation target.
- Setting global morph immediately writes the effective per-voice morph amount for all six synth voices.
- Global morph has no other live role after that write; all actual interpolation uses the per-voice morph amount array.
- Individual per-voice morph step automation uses the `PAR_MORPH_DRUM1` through `PAR_MORPH_HIHAT` parameters.
- Per-voice morph automation values are 0-127 in the current parameter definitions. `0` is valid zero morph. Values `0-126` map to the STM interpolation scale by multiplying by 2. Value `127` is a specific carve-out and maps to exact full morph `255`.
- If velocity automation target, LFO automation target, macro modulation, MIDI, or any other modulation source modulates individual voice morph, that modulation is scaled between the current per-voice morph amount and full morph.
- This modulation scheme is intentionally inverted compared to ordinary parameter modulation: `0` modulation leaves the voice at its current per-voice morph amount, while full modulation produces the morph parameter endpoint.
- STM-side morph interpolation and modulation are always serviced. The only morph-related operation that may be temporarily blocked is receipt of a global morph command from the AVR/front panel.

## Current Ownership

### AVR

The AVR currently performs live morph interpolation in `preset_morph(...)`.

Current AVR inputs:

- `parameter_values[]`: kit/front endpoint bytes.
- `parameters2[]`: morph parameter endpoint bytes.
- `morphValue`: current/last global morph value.
- `frontPanel_morphArray` plus `frontPanel_longData`: one pending voice-mask/amount pair for the old incoming `MORPH_CC` / `VOICE_MORPH` live-compute route.

Current AVR behavior:

- Menu edit of `PAR_MORPH` sets `morphValue` and calls `preset_morph(0x7f, value)`.
- Incoming `MORPH_CC` / `VOICE_MORPH` sets a pending voice mask and amount, later calls `preset_morph(...)`.
- `preset_morph(...)` interpolates each parameter in the selected voice masks from `parameter_values[]` and `parameters2[]`, then sends the resolved value to STM as `MIDI_CC` or `CC_2`.
- `preset_morph(...)` does not send resolved automation destination sideband messages for interpolated automation destination selectors.

Important limitation:

- AVR does not maintain a complete canonical per-voice morph amount array.
- AVR only has the last/current scalar plus one pending voice-mask/amount operation.
- After the move, AVR should not be taught a per-voice morph model. The front panel should only retain/display global morph and endpoint bytes.

### STM

STM already has most of the storage needed:

- `seq_normalKitState.frontPanelParams[]`
- `seq_normalKitState.morphParams[]`
- `seq_normalKitState.interpolatedParams[]`
- `seq_tmpKitState.frontPanelParams[]`
- `seq_tmpKitState.morphParams[]`
- `seq_tmpKitState.interpolatedParams[]`
- separate automation target structs for kit/front endpoint, morph parameter endpoint, and interpolated state

STM also has partial morph-related runtime state:

- `seq_vMorphAmount[]`
- `seq_vMorphFlag`
- `modNode_vMorph(...)` computes per-voice morph amounts for modulation destinations `PAR_MORPH_DRUM1` through `PAR_MORPH_HIHAT`.
- Current code paths are not yet the desired final model because modulation currently produces an absolute morph amount instead of scaling between the current per-voice morph amount and the morph parameter endpoint.

Important limitation:

- STM currently does not own the AVR `modTargets[]` selector-to-parameter mapping.
- Without that mapping or an equivalent generated table, STM cannot fully resolve interpolated velocity, LFO, or macro automation destination selectors from raw selector bytes.

## Why The Move Is Needed

The current split creates fragile state boundaries:

- AVR endpoint arrays can be correct while STM live automation destination assignments are stale.
- AVR computes morph from menu endpoint arrays, but STM owns the actual DSP/modulation nodes.
- Rate-limited endpoint restore fixed the chirp, but it makes AVR-side morph computation unsafe unless restore and morph are carefully ordered.
- AVR lacks a canonical per-voice morph state model, so it cannot reliably recompute all voice-scoped morph state after endpoint restore.

Moving morph to STM removes the need for the AVR to compute live sound state from `parameter_values[]` / `parameters2[]`.

## Target Architecture

### AVR responsibilities after move

AVR should:

- Continue storing and displaying `parameter_values[]`.
- Continue storing and displaying `parameters2[]`.
- Continue reading `.kit`, `.prf`, and `.all` files.
- Continue sending endpoint bytes to STM during load/copy flows.
- Continue sending raw user edits to STM.
- Send global morph amount changes to STM.
- Send macro value, macro destination, and macro amount edits to STM.

AVR should stop:

- Calling `preset_morph(...)` to update live STM sound state.
- Sending interpolated voice parameter streams produced by `preset_morph(...)`.
- Treating AVR endpoint arrays as authoritative live morph computation inputs.
- Handling, displaying, or attempting to author per-voice morph state.

### STM responsibilities after move

STM should:

- Store endpoint bytes for normal and temporary images.
- Store global morph for display/protocol compatibility if needed.
- Store the authoritative per-voice morph amount array.
- When global morph changes, immediately copy that amount into all six per-voice morph amounts and mark all six voices dirty.
- Compute interpolated parameters from endpoint bytes.
- Apply interpolated parameters to live DSP only when the corresponding normal/temporary image is currently sounding.
- Resolve and apply interpolated automation destination assignments.
- Run morph interpolation work through a rate-limited worker that performs exactly one pending parameter interpolation per STM main loop.

## Required STM State

Add an STM-owned morph runtime state, probably in `sequencer.c`:

- `seq_globalMorphAmount`
- `seq_voiceMorphAmount[SEQ_SYNTH_VOICES]`
- `seq_voiceMorphDirtyMask`
- worker state:
  - active kit image: normal or temporary
  - voice mask to recompute
  - parameter cursor
  - current phase: voice parameters and automation destination selectors
  - apply-live flag

Canonical state rule:

- `seq_globalMorphAmount` is not an independent interpolation source.
- On global morph change, STM writes the same effective amount into all six `seq_voiceMorphAmount[]` entries and marks all voices dirty.
- Step automation or modulation of `PAR_MORPH_DRUM1` through `PAR_MORPH_HIHAT` writes only the addressed voice amount and marks only that voice dirty.
- Shared/global parameter morphing should not be added as an accidental consequence of global morph. The existing behavior is voice-mask based, and the STM move should preserve that unless there is a later explicit design change.

Value range rule:

- AVR global morph is currently menu-visible as 0-255.
- Per-voice morph automation parameters are currently 0-127 in the enum/type tables.
- STM should keep a single effective interpolation scale internally, preferably 0-255.
- For per-voice step automation, convert the 0-127 value before storing/applying the effective morph amount:
  - `0-126` maps to `value * 2`;
  - `127` maps to `255`.
- This preserves valid zero morph and the old AVR exact full-endpoint behavior.

## One-Parameter-Per-Loop Worker

Add `seq_serviceMorphInterpolation()` and call it once per STM main loop.

The worker must be serviced once per STM main loop and, when dirty morph work exists, must perform exactly one parameter interpolation per call:

1. Pick the next dirty voice.
2. Pick the next parameter in that voice's parameter mask.
3. Read kit/front endpoint byte from the active `SeqKitState`.
4. Read morph parameter endpoint byte from the same `SeqKitState`.
5. Interpolate using `seq_voiceMorphAmount[voice]`.
6. Store the result in `interpolatedParams[]`.
7. If the affected image/voice is currently sounding, apply the single parameter to DSP.
8. If the interpolated parameter is an automation destination selector, update the corresponding interpolated automation target storage and apply the modulation destination if live.
9. Advance the cursor.

When the cursor reaches the end of the selected voice, clear that voice's dirty bit.

If no dirty voice exists, the service call performs no interpolation and returns. If dirty work exists, one parameter is interpolated on that loop pass. There should be no mode where morph servicing is disabled or starved to make some other bulk path run instead.

Ordering with existing services:

- `seq_serviceMorphInterpolation()` should not run inside pattern boundary code.
- It should run from the STM main loop near `seq_serviceEndpointRestore()`.
- If endpoint restore and morph interpolation both need service, choose a deterministic order.
- Recommendation: endpoint restore remains menu synchronization; morph interpolation should not depend on AVR endpoint restore completion once STM owns endpoint storage. Therefore morph interpolation may run independently of endpoint restore.
- Normal/temp pattern set changeover may block incoming AVR/front-panel global morph commands, but it must not block the STM morph interpolation worker itself.

## Automation Destination Resolution

This is the largest concrete dependency.

Current AVR code resolves selector bytes using `modTargets[]` in `front/LxrAvr/Menu/Cc2Text.c`.

To move morph fully to STM, one of these must happen:

1. Port/generate the `modTargets[]` selector-to-parameter table for STM.
2. Generate a shared header/table consumed by both AVR and STM.
3. Replace selector-byte interpolation with a protocol where AVR sends resolved endpoint automation destinations for both endpoint images, and STM interpolates or selects between resolved destinations according to agreed rules.

Recommendation:

- Generate or port the selector-to-parameter map to STM.
- Keep AVR menu selector bytes unchanged.
- Let STM resolve interpolated selector bytes for:
  - velocity automation destinations from `PAR_VEL_DEST_1..6`;
  - LFO automation destinations from `PAR_TARGET_LFO1..6` plus `PAR_VOICE_LFO1..6`;
  - macro modulation destinations from `PAR_MAC1_DST1`, `PAR_MAC1_DST2`, `PAR_MAC2_DST1`, `PAR_MAC2_DST2`.

Critical behavior:

- Do not change the existing rule that automation destination selector behavior around morph is dangerous to alter casually.
- First implementation should reproduce the currently intended visible behavior, then separately test whether interpolated automation destination selectors should affect live routing.

## Protocol Changes

### AVR-to-STM morph amount messages

Add an explicit AVR-to-STM message for global morph amount changes.

Suggested messages:

- `SEQ_CC, SEQ_SET_GLOBAL_MORPH, amount`

Alternative:

- Reuse `MORPH_CC` / `VOICE_MORPH` opcodes in the AVR-to-STM direction only, but that risks confusion because those names are currently part of the old STM-to-AVR morph compute route.

Recommendation:

- Add an explicit `SEQ_CC` message with a name that describes an STM-side global morph state update.
- Do not add an AVR menu/display pathway for per-voice morph. Per-voice morph updates should enter STM from MIDI, step automation, or modulation sources that already live on or route directly into STM.

### Global morph ingress blocking

Keep a guard that can reject incoming AVR/front-panel global morph commands during sensitive STM transition windows.

Intended use:

- normal pattern set to temporary pattern set changeover;
- temporary pattern set to normal pattern set changeover;
- any future transition where accepting a new global morph value would rewrite all six per-voice morph amounts while voice source ownership is in flux.

Non-goals:

- Do not pause STM-side morph interpolation.
- Do not pause STM-side per-voice morph modulation.
- Do not block MIDI, step automation, or modulation paths that directly update per-voice morph unless a later bug proves that is necessary.
- Do not rely on this primarily for file load protection; the menu is already blocked during file load, and file-load isolation should be handled by normal/temp storage rules.

Behavior:

- If a global morph command arrives from the AVR while global morph ingress is blocked, ignore it or queue only the most recent value, depending on desired UI feel.
- Initial recommendation: ignore front-panel global morph commands during the blocked window, because pattern set changeover should not accumulate hidden global rewrites.
- Once the transition completes, AVR/front-panel global morph commands are accepted again and copy their value into all six per-voice morph amounts.

### Retire STM-to-AVR live morph compute route

Remove or disable:

- `sequencer_sendVMorph(...)` sending `FRONT_SEQ_VOICE_MORPH` as a live-compute request to AVR.
- AVR `MORPH_CC` / `VOICE_MORPH` handler as a live sound path.
- AVR `preset_morph(...)` calls for live sound updates.

Keep only if still needed for old file/UI compatibility:

- `preset_getMorphValue(...)` for save/load helper logic, if it is used only as an AVR-side data helper and not as a live sound path.
- `parameters2[]` storage and display.

### Endpoint restore

The STM-to-AVR endpoint restore remains necessary for menu correctness:

- restore kit/front endpoint bytes into AVR `parameter_values[]`;
- restore morph parameter endpoint bytes into AVR `parameters2[]`.

But after the move:

- endpoint restore should not be required for live morph correctness;
- morph interpolation should use STM endpoint storage directly;
- AVR restore-active guards around `preset_morph(...)` should become unnecessary once live AVR morph computation is removed.

## File Load Changes

For `.all` and `.prf`:

- AVR reads file data as now.
- AVR sends kit/front endpoints and morph parameter endpoints to STM normal storage.
- STM stores both endpoint groups in `seq_normalKitState`.
- STM schedules morph interpolation for normal storage using STM-owned current morph amounts.
- If temporary pattern is currently sounding, do not apply the recomputed normal interpolated values live.

For `.kit` normal load:

- AVR sends kit/front endpoints to STM normal storage.
- STM recomputes normal interpolated values for affected voices using current STM morph amounts.
- Morph parameter endpoints remain unchanged unless explicitly loaded.

For `.kit` load morph:

- AVR sends morph parameter endpoints to STM normal storage.
- STM recomputes normal interpolated values for affected voices using current STM morph amounts.

For copy normal pattern to temporary pattern:

- STM copies normal interpolation state and endpoint images according to current copy rules.
- STM schedules temp interpolation recompute if needed.
- Live apply depends on whether temp is currently sounding.

## Menu Edit Changes

Normal parameter edit:

- AVR updates `parameter_values[]` for display.
- AVR sends raw edit to STM.
- STM stores it into the active image's kit/front endpoint or interpolated state according to the existing edit/storage rule.
- STM schedules morph interpolation for the affected parameter/voice if the edit affects an endpoint.

SHIFT morph parameter endpoint edit:

- AVR updates `parameters2[]` for display.
- AVR sends raw morph parameter endpoint edit to STM.
- STM stores it into the active image's morph parameter endpoint.
- STM schedules interpolation for the affected parameter/voice.

Global morph edit:

- AVR updates display value.
- AVR sends global morph amount to STM.
- STM updates `seq_globalMorphAmount`.
- STM copies the global morph amount into all six `seq_voiceMorphAmount[]` entries.
- STM marks all six synth voices dirty for morph interpolation.

Per-voice morph automation:

- There is no AVR menu edit for per-voice morph.
- MIDI or step automation writes the addressed per-voice amount on STM.
- For step automation of `PAR_MORPH_DRUM1` through `PAR_MORPH_HIHAT`, convert the 0-127 automation value before storing the effective morph amount: `0-126` becomes `value * 2`, and `127` becomes `255`.
- STM marks that voice dirty for morph interpolation.

Modulation of per-voice morph:

- Velocity automation target, LFO automation target, macro modulation, or any other modulation source that addresses per-voice morph should not overwrite the baseline per-voice amount directly.
- The modulation output should be scaled between the current per-voice morph amount and full morph.
- Formula intent: `effective = currentVoiceMorph + modulationDepth * modulationSignal * (fullMorph - currentVoiceMorph)`, adjusted for the project's fixed-point conventions.
- At zero modulation, interpolation remains at the current per-voice morph amount.
- At full modulation, interpolation reaches the morph parameter endpoint.
- This is intentionally inverted compared to ordinary parameter modulation because the modulation depth describes travel toward full morph, not offset away from the current value.

## Application Rules

The worker must respect normal/temporary isolation:

- If a recomputed parameter belongs to normal storage and normal pattern set is sounding for that voice, apply live.
- If normal storage is recomputed while temp is sounding for that voice, store only.
- If temp storage is recomputed while temp is sounding for that voice, apply live.
- If temp storage is recomputed while normal is sounding for that voice, store only.

Hihat special case:

- Tracks 5 and 6 share synth voice 5.
- Any per-track source transition involving either hi-hat track must keep both tracks on the same parameter image for synth voice 5.
- Morph interpolation for synth voice 5 should use the same source decision already used by temp/normal parameter switching.

## Suggested Implementation Phases

### Phase 1: Stop adding more AVR live morph dependencies

- Remove the follow-up deferred STM-to-AVR morph queue once STM morph worker is ready.
- Keep endpoint restore rate limiting.
- Keep all current storage isolation rules.

### Phase 2: Add STM morph state and worker skeleton

- Add STM morph state arrays and dirty flags.
- Add `seq_serviceMorphInterpolation()`.
- Call it once per main loop.
- Initially support only ordinary voice parameter interpolation.
- Confirm one parameter per call with a cursor.

### Phase 3: Route morph amount changes to STM

- Add an explicit AVR-to-STM message for global morph amount.
- Change AVR `PAR_MORPH` edit path to send global morph amount instead of calling `preset_morph(...)`.
- On STM global morph receipt, copy the amount into all six per-voice morph amounts and mark all six voices dirty.
- Change STM step automation morph paths to update STM per-voice morph state directly rather than sending morph compute requests to AVR.
- Change STM modulation-node morph paths to compute an effective amount between the current per-voice morph amount and full morph, then schedule that voice for interpolation.
- Remove or bypass AVR `menu_vMorph(...)` as a live morph path; the AVR should not own per-voice morph state.

### Phase 4: Port automation destination resolution

- Port or generate the `modTargets[]` selector-to-parameter mapping for STM.
- Add STM helpers to resolve selector bytes into destination parameter ids.
- Update interpolated automation target storage when interpolation touches automation destination selectors.
- Apply resolved destination changes live only when the affected image/voice is sounding.

### Phase 5: Remove AVR live morph compute

- Remove or disable live calls to `preset_morph(...)`.
- Remove live use of `MORPH_CC` / `VOICE_MORPH` on AVR.
- Keep `parameters2[]` for menu/file endpoint storage.
- Keep endpoint dumps and restores as display/storage synchronization only.

### Phase 6: File-load and copy retest

Test these cases:

- `.prf` load while normal pattern set is sounding.
- `.prf` load while temporary pattern is sounding.
- `.all` load while temporary pattern is sounding.
- `.kit` load into kit/front endpoint.
- `.kit` load morph into morph parameter endpoint.
- Copy normal pattern to temporary pattern.
- Full normal/temp pattern switch.
- Individual track normal/temp switch.
- Hi-hat track normal/temp switch.
- Per-voice morph automation from steps.
- Velocity automation target or LFO automation target modulation of per-voice morph.
- Macro modulation of per-voice morph, if still supported as an STM modulation source.
- LFO/velocity/macro automation destination selectors in morph parameter endpoints.

## Risks

- Porting `modTargets[]` incorrectly will break automation destination assignment.
- Interpolating selector bytes can produce intermediate selector values that are musically or structurally invalid unless clamped/validated like AVR menu edits do.
- LFO selector logic depends on paired `PAR_VOICE_LFO*` and `PAR_TARGET_LFO*` values; both may need special handling.
- Macro modulation destinations are shared/global; applying them per voice would be wrong.
- Rate limiting one parameter per loop means a full six-voice recompute may take many main-loop passes. The UI should tolerate short convergence time.
- Avoid calling `modNode_setDestination(...)` too often if selector interpolation changes rapidly; it can reset modulation state.
- Current `ParameterArray.c` and `modulationNode.c` should be checked for per-voice morph array indexing before implementation. The audit found evidence of different indexing conventions around `seq_vMorphAmount[]`, and the STM move should normalize this to six synth voices without hidden offset rules.
- If per-voice step automation scaling is implemented as a plain `*2` without the `127 -> 255` carve-out, full morph will stop one unit short of the morph parameter endpoint.

## Assumption Check

Your clarified model is internally consistent and fits the desired STM ownership split:

- AVR-global-only morph state is compatible with the current menu architecture because per-voice morph is not a menu-visible edit surface.
- Copying global morph into all six per-voice morph amounts is a clean rule and avoids having two active interpolation authorities.
- Using only per-voice morph amounts for interpolation is the right simplification for the STM worker.
- Inverted modulation toward full morph is implementable, but it requires separating the stored baseline per-voice morph amount from the temporary effective amount produced by velocity/LFO/macro modulation.

Potential corrections or implementation cautions:

- Per-voice morph value `0` is valid zero morph and should not be clamped upward.
- Per-voice morph value conversion is now settled: `0-126` maps to `value * 2`; `127` maps to exact full morph `255`.
- Existing STM code appears to have mixed indexing conventions around `seq_vMorphAmount[]`; do not build the new worker on that array without normalizing it.
- Modulated per-voice morph needs at least two concepts: the stored baseline amount and the current effective amount. Otherwise modulation would overwrite the user's/automation's baseline morph position.

## Open Questions

- Should `PAR_MORPH` continue to affect all six voice parameter masks only, matching current AVR behavior, or should it also interpolate shared/global sound parameters?
- Should automation destination selectors be interpolated continuously, quantized, or switched at a threshold?
- Should STM keep `seq_globalMorphAmount` after copying it into all six per-voice amounts, or should it only exist long enough to support AVR menu echo/save behavior?
- During endpoint restore to AVR, should menu show the latest endpoint immediately, or should menu sync be allowed to lag behind STM live state?

## Recommended First Coding Change

Do not start by deleting AVR `preset_morph(...)`.

Start by adding STM-owned morph state and a worker that can recompute one ordinary voice parameter per main loop using `frontPanelParams[]` and `morphParams[]`.

Once that works for ordinary parameters:

1. route global morph amount edits to STM;
2. verify audible morph without AVR `preset_morph(...)`;
3. add automation destination resolution;
4. then retire the AVR live morph path.
