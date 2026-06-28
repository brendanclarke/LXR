# KIT Reload Audit

## Implementation Progress

- 2026-06-29: Started implementation. Reopened the active AVR/STM reload, file-load, temp-switch, and Preset storage paths to confirm the live code still matches the audit assumptions before patching.
- 2026-06-29: Added the first STM-side storage primitives:
  - endpoint-subset copy helper in `Preset/KitState`;
  - interpolated-cache rebuild during snapshot copies;
  - temp-preset playback flags and reload/resnapshot helpers in `Preset/TempPlaybackSwitch`;
  - tiny endpoint-ingress suppression guard in `Preset/ParameterIngress`.
- 2026-06-29: Wired the STM parser/sequencer flow to the new primitives:
  - ordinary parameter-bearing loads now mirror the loaded endpoint subset into temp;
  - protected `.prf` / `.all` background loads now defer the temp resnapshot until playback returns to normal;
  - inbound `PATCH_RESET` on STM now routes to the new normal-from-temp restore path.
- 2026-06-29: Cut the AVR side over to the new semantics:
  - `menu_reloadKit()` now only sends `PATCH_RESET` and never restores AVR snapshots locally;
  - AVR now tracks a narrow “temp preset playback active” flag for reload disable and menu-endpoint preservation;
  - inbound `PATCH_RESET` on AVR no longer restores `parameter_values_fileLoadSnapshot[]`.
- 2026-06-29: Verification pass:
  - `make -C mainboard/LxrStm32 -j4 stm32` passed;
  - `make -C front/LxrAvr avr -j4` passed;
  - top-level `make firmware` failed in the existing host-side `tools/bin/FirmwareImageBuilder` binary with a local macOS `libc++` symbol/runtime mismatch, so firmware image packaging was not re-verified in this session.
- 2026-06-29: Added the persistent `PAR_VOICE_DECIMATION_ALL` guard:
  - imported kit/morph payloads now rewrite `0` to `127` immediately at the AVR file-read boundary before the value can propagate anywhere else;
  - `menu_init()` now seeds `PAR_VOICE_DECIMATION_ALL` to `127` after the parameter array zero-fill so startup always begins from the safe value.

## Goal

Re-implement `SHIFT+PLAY` kit reload so it uses the STM-side temporary preset parameter storage instead of the AVR file-load snapshot arrays, while preserving the current background-load model for `.prf` and `.all`.

The target behavior is:

- load-menu `.snd` full-kit loads copy kit endpoints into both STM normal and STM temporary preset storage;
- load-menu individual instrument loads copy only the kit/front endpoint data into both STM normal and STM temporary preset storage;
- load-menu morph loads copy only the morph endpoint data into both STM normal and STM temporary preset storage;
- `.prf` and `.all` loads without background-load copy both kit/front and morph endpoints into both STM normal and STM temporary preset storage;
- `.prf` and `.all` loads with background-load keep the current protected behavior during the load itself:
  normal storage is overwritten while playback continues from temporary storage;
- once that protected `.prf` / `.all` background load returns to normal playback after a manual pattern change, STM immediately copies the loaded normal preset endpoints back into temporary storage so temporary storage again matches the last loaded preset image;
- `SHIFT+PLAY` copies both kit/front and morph endpoints from STM temporary storage back into STM normal storage;
- `SHIFT+PLAY` is disabled while playback is still using temporary preset parameters during a protected `.prf` / `.all` background-load window.

## Current Behavior Summary

The current implementation is split across two unrelated mechanisms:

- STM `preset_tmpKitState` is the runtime temporary preset image used for background-load playback protection.
- AVR `parameter_values_fileLoadSnapshot[]` / `parameters2_fileLoadSnapshot[]` are still the source for `SHIFT+PLAY`.

That mismatch is why the current reload path conflicts with background-loading:

- `.prf` / `.all` background loads intentionally leave STM temporary preset storage holding the old audible sound while normal storage is overwritten.
- `SHIFT+PLAY` does not restore from STM temporary storage; it sends `PATCH_RESET` and rewrites the AVR menu arrays from the AVR snapshots.
- the AVR snapshots are not the same thing as the STM temporary preset image once background-load protection is active.

The fix is to make STM temporary preset storage the single authoritative “last loaded preset endpoint image”, and to stop using the AVR snapshots for reload semantics.

## Storage / Behavior Matrix

| Operation | STM normal storage | STM temporary storage | Notes |
|---|---|---|---|
| `.snd` kit load | kit/front endpoint only | kit/front endpoint only | morph endpoint unchanged |
| `.snd` individual instrument load | kit/front endpoint only | kit/front endpoint only | morph endpoint unchanged |
| morph load | morph endpoint only | morph endpoint only | kit/front endpoint unchanged |
| `.prf` load, no background | both endpoints | both endpoints | current background model not involved |
| `.all` load, no background | both endpoints | both endpoints | current background model not involved |
| `.prf` / `.all` with background | both endpoints during file load | keep old audible preset until manual exit, then copy normal back into temp | this preserves the protected sound hold |
| `SHIFT+PLAY` | copy both endpoints from temp into normal | unchanged | disabled while temp preset playback is active |

## Implementation Strategy

1. Keep the existing background-swap handshake and temp-pattern switch model exactly as it is for protected `.prf` / `.all` loads.
2. Add explicit STM-side helpers that can copy endpoint subsets between `preset_normalKitState` and `preset_tmpKitState`.
3. Mark each incoming file load as one of two cases:
   - mirror-to-temp immediately;
   - preserve-temp-during-load and resnapshot-on-exit.
4. Keep `PATCH_RESET` as the transport for `SHIFT+PLAY`, but change its semantics so STM treats it as “copy temporary preset storage back into normal storage”.
5. Gate that new reload command on the authoritative STM temp-playback state so it cannot fire while temp preset playback is still the live source.

## Detailed Code Changes

### 1. Reinterpret `PATCH_RESET` as “reload normal from temporary”

#### `mainboard/LxrStm32/src/MIDI/MidiMessages.h`

- Keep `#define PATCH_RESET 0xFE`, but update the comment so it describes the new STM-owned reload behavior rather than the old generic “last loaded patch image” wording.

Adjacent comment text to add:

```c
/* SHIFT+PLAY uses PATCH_RESET as a dedicated one-byte reload request.
   STM interprets it as “copy the authoritative temporary preset endpoint image
   back into normal preset storage”, and rejects it while temporary preset
   playback is still the live sound source. */
```

#### `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`

- Keep `#define PATCH_RESET 0xFE`, but update the comment so AVR-side protocol naming matches the new meaning.

Adjacent comment text to add:

```c
/* SHIFT+PLAY sends PATCH_RESET as a reload request to STM.
   AVR no longer treats it as an instruction to restore from its local
   file-load snapshots. */
```

Why this change is necessary:

- The user explicitly wants this functionality to stay attached to `PATCH_RESET`.
- `PATCH_RESET` is already a dedicated one-byte control path, so changing its semantics is lower-risk than adding or repurposing a separate transport command.

### 2. Track whether temp playback is using temporary preset parameters or only temporary pattern data

#### `front/LxrAvr/Preset/PresetManager.h`

- Add prototypes for:
  - `uint8_t preset_isBackgroundTempPresetPlaybackActive(void);`
  - optionally a tiny wrapper like `void preset_requestPatchResetReload(void);` if we want to keep the send-side call out of the menu layer.

Adjacent comment text to add:

```c
/* AVR needs a lightweight view of whether temp playback is using temporary
   preset parameters, not just the temporary pattern slot, because SHIFT+PLAY
   must be disabled only for the protected .prf/.all background-load window. */
```

#### `front/LxrAvr/Preset/presetManager.c`

- Add a new AVR-side state flag distinct from the existing broad temp-playback LED/swap flag, for example:
  - `preset_backgroundTempPresetPlaybackActive`
- Set it when `preset_backgroundSwapDoneFromStm()` completes for `.prf` or `.all`.
- Do not set it for `.pat`.
- Clear it in `preset_notePlayedPatternChanged()` when playback leaves `SEQ_TMP_PATTERN`.
- Add the optional helper that sends `PATCH_RESET`, if we want a named wrapper instead of calling `avrComms_sendByte(PATCH_RESET)` directly from the menu layer.

Adjacent comment text to add near the new flag:

```c
/* This flag tracks the narrower case where playback is still sourcing preset
   parameters from STM temporary storage after a protected .prf/.all background
   load. The broader temp-playback LED/UI state also covers pattern-only temp
   playback, but SHIFT+PLAY must only be blocked while temporary preset data is
   the live sound source. */
```

Adjacent comment text to add near the send helper:

```c
/* SHIFT+PLAY now delegates reload to STM.
   AVR snapshots still exist as file-read staging buffers, but they are no
   longer the authoritative reload source once background-load temp protection
   is active. */
```

Why this change is necessary:

- AVR currently only knows “temp pattern playback is active”.
- The new requirement is more specific: disable reload only when temporary preset parameters are the live source.
- AVR also needs a clean place to document that `PATCH_RESET` is still the transport, but no longer means “restore from AVR snapshots”.

### 3. Replace the AVR snapshot-based `SHIFT+PLAY` path

#### `front/LxrAvr/Menu/menu.c`

- Rewrite `menu_reloadKit()` so it:
  - returns immediately when `preset_isBackgroundTempPresetPlaybackActive()` is true;
  - otherwise sends `PATCH_RESET` to STM;
  - does not copy `parameter_values_fileLoadSnapshot[]` back into `parameter_values[]`;
  - does not perform any local snapshot restore.

Adjacent comment text to add:

```c
/* Reload now comes from STM temporary preset storage, not the AVR file-load
   snapshots. That keeps SHIFT+PLAY aligned with the STM-owned temp/normal
   storage model and prevents reload from fighting the protected .prf/.all
   background-load path. */
```

Adjacent comment text to add on the early return:

```c
/* While a protected .prf/.all background load is still playing from temporary
   preset storage, reload must stay disabled. Copying temp back into normal at
   that point would either be a no-op or would blur the distinction between the
   protected old sound and the newly loaded normal sound. */
```

#### `front/LxrAvr/Menu/menu.h`

- Keep the prototype, but update the surrounding comment if one is added later so it no longer describes an AVR snapshot reset.

Why this change is necessary:

- This is the user-facing entry point for `SHIFT+PLAY`.
- The old implementation is the exact behavior being replaced.

### 4. Add STM helpers to copy endpoint subsets between normal and temporary kit storage

#### `mainboard/LxrStm32/src/Preset/KitState.h`

- Add a new helper API that can copy endpoint subsets between kit images, for example:
  - `void preset_copyKitEndpoints(PresetKitState *dst, const PresetKitState *src, uint8_t endpointMode);`
  - or two convenience wrappers for `normal -> tmp` and `tmp -> normal`.

Adjacent comment text to add:

```c
/* File loads and SHIFT+PLAY need a storage copy primitive that can move only
   the kit/front endpoint, only the morph endpoint, or both, without forcing
   every caller to open-code the PresetKitState layout. This keeps the temp
   snapshot semantics centralized in Preset instead of scattering memcpy rules
   through the parser and sequencer. */
```

#### `mainboard/LxrStm32/src/Preset/KitState.c`

- Implement the copy helper so it:
  - copies the selected endpoint arrays;
  - copies the matching automation sideband blocks;
  - marks the destination kit valid;
  - preserves untouched endpoint groups for partial copies;
  - refreshes the destination-side cached fields needed for a future temp/normal switch.

Recommended implementation detail:

- `FRONT_ONLY` copies:
  - `kitEndpointParams`
  - `frontPanelAutomationTargets`
  - the front-end-derived cached automation image
- `MORPH_ONLY` copies:
  - `morphEndpointParams`
  - `morphParameterEndpointAutomationTargets`
- `BOTH` can reuse a full-image copy path equivalent to `preset_captureTmpKitState()`, or can call the new subset helper twice if that is cleaner.

Adjacent comment text to add:

```c
/* Temporary preset storage now doubles as the canonical “last loaded preset”
   image. Partial file loads therefore need partial snapshot copies:
   .snd/instrument loads refresh only the kit/front endpoint image, morph loads
   refresh only the morph endpoint image, and .prf/.all refresh both. */
```

Why this change is necessary:

- The current full-image `preset_captureTmpKitState()` is correct for background-load protection, but it is too blunt for the new partial-load rules.
- The new behavior depends on copying only the endpoint groups the user actually loaded.

### 5. Add a tiny STM-side endpoint-copy critical section

#### `mainboard/LxrStm32/src/Preset/ParameterIngress.h`

- Add a small API for temporarily suppressing endpoint ingress while a storage copy is in progress, for example:
  - `void preset_setEndpointIngressSuppressed(uint8_t suppressed);`

Adjacent comment text to add:

```c
/* This is not a long-running lock.
   It exists only so a temp/normal resnapshot cannot interleave with live
   front-panel or MIDI endpoint writes and leave one endpoint group copied
   while the other still reflects an older preset image. */
```

#### `mainboard/LxrStm32/src/Preset/ParameterIngress.c`

- Add a private suppression flag.
- Early-return from:
  - `preset_storeParameterIngress()`
  - `preset_storeMorphParameterIngress()`
  - `preset_storeLfoDestinationIngress()`
  - `preset_storeVelocityDestinationIngress()`
when the suppression flag is set.

Adjacent comment text to add:

```c
/* Resnapshot and reload copies are intentionally atomic at the preset-storage
   level. Dropping endpoint writes during the tiny copy window is safer than
   allowing front and morph endpoint groups to land in different generations of
   the temp/normal image pair. */
```

Why this change is necessary:

- The user explicitly requested that kit and morph endpoint changes be disabled briefly during the post-background-load normal-to-temp resnapshot.
- Even though the copy window is short, making the rule explicit in Preset is the safest way to preserve endpoint coherence.

### 6. Teach STM which file loads should mirror to temporary immediately and which should preserve temporary until exit

#### `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

- Add parser-local state for the current file load, for example:
  - `frontParser_fileLoadShouldMirrorToTmp`
  - `frontParser_fileLoadEndpointMode`
  - `frontParser_fileLoadUsedBackgroundSwap`
  - `frontParser_fileLoadPreservedTempPresetPlayback`

- On `FRONT_SEQ_BACKGROUND_SWAP_BEGIN`:
  - keep the existing copy-to-temp / switch-to-temp behavior unchanged;
  - record that the next matching file load used the background-swap prelude.

- On `FRONT_SEQ_FILE_BEGIN`:
  - classify the file load:
    - `.snd` / instrument / morph load: mirror to temp immediately after the endpoint dump closes;
    - `.prf` / `.all` without the background-swap prelude: mirror to temp immediately after the endpoint dump closes;
    - `.prf` / `.all` with the background-swap prelude: do not mirror now; mark the temp-preset playback state as protected and leave resnapshot pending for the later return-to-normal boundary.

- On `FRONT_SEQ_TMP_KIT_ENDPOINT_BEGIN`:
  - store the endpoint mode so `END` knows whether the load was front-only, morph-only, or both.

- On `FRONT_SEQ_TMP_KIT_ENDPOINT_END`:
  - keep the existing normal-storage finalization;
  - if the current file load is flagged as “mirror now”, call the new `normal -> tmp` endpoint copy helper using the stored endpoint mode;
  - if temp preset storage is currently the live source, invalidate/reapply that destination image so the audible sound tracks the newly mirrored temp state instead of waiting for a later boundary switch.

- On `FRONT_SEQ_FILE_DONE`:
  - clear the per-file mirror flags;
  - keep the protected `.prf` / `.all` resnapshot-on-exit flag armed if that file load intentionally preserved the old temp preset image.

Adjacent comment text to add near the classification logic:

```c
/* Not every file load treats temporary storage the same way.
   Ordinary loads mirror the loaded endpoint subset straight into temp so temp
   always stays the last-loaded preset image. Protected .prf/.all background
   loads are the only exception: they must leave temp holding the old audible
   sound until playback explicitly returns to normal. */
```

Adjacent comment text to add near the endpoint-end mirror:

```c
/* The AVR endpoint dump always lands in normal storage first.
   For ordinary loads we immediately mirror the same endpoint subset into temp
   so STM temporary storage remains the authoritative reload snapshot. */
```

Adjacent comment text to add near the protected `.prf` / `.all` branch:

```c
/* Protected .prf/.all background loads intentionally do not mirror into temp
   here. Temp is still the currently audible old sound, so overwriting it now
   would break the background-load hold. The normal image is copied back into
   temp only after playback later returns to normal. */
```

Why this change is necessary:

- This is where file-load transport becomes real storage policy.
- The parser already owns the ingress brackets and knows the file type, endpoint mode, and background-swap context.

### 7. Extend temp-playback state so STM can distinguish “protected temp preset playback” from other temp uses

#### `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.h`

- Extend `PresetTempPlaybackSwitchState` with fields like:
  - `tempPresetPlaybackActive`
  - `resnapshotTmpFromNormalPending`

Adjacent comment text to add:

```c
/* Background temp playback has two materially different forms:
   pattern-only temp playback for .pat, and preset-carrying temp playback for
   protected .prf/.all loads. Reload gating and temp resnapshot timing depend
   on that distinction, so it must live in the canonical temp-switch state. */
```

#### `mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c`

- Add helpers or direct state handling for:
  - testing whether temporary preset storage is still the live source;
  - copying temp back into normal for `SHIFT+PLAY`;
  - resnapshoting normal back into temp when the protected background-load window ends.

- The `SHIFT+PLAY` helper should:
  - reject if temp preset playback is still active;
  - suppress endpoint ingress briefly;
  - copy both endpoint groups from `preset_tmpKitState` into `preset_normalKitState`;
  - invalidate the normal live cache;
  - reapply the now-restored normal image to the live DSP;
  - queue a normal-image endpoint push back to AVR so the menu reflects the restored preset.

- The resnapshot-on-exit helper should:
  - run only when the protected `.prf` / `.all` window is ending;
  - suppress endpoint ingress briefly;
  - copy both endpoint groups from normal into temp;
  - invalidate the temp cache for future use;
  - clear `tempPresetPlaybackActive` and `resnapshotTmpFromNormalPending`.

Adjacent comment text to add near the reload helper:

```c
/* SHIFT+PLAY is now a pure STM storage restore.
   It copies the authoritative temporary preset endpoint image back into normal
   storage, then reapplies the normal image because normal is the live sound
   source whenever this command is allowed to run. */
```

Adjacent comment text to add near the resnapshot-on-exit helper:

```c
/* Once playback has returned to normal after a protected .prf/.all background
   load, temp must stop holding the old sound and become the new last-loaded
   preset snapshot again. This resnapshot happens only after no voices are
   still sourcing temporary preset data. */
```

Why this change is necessary:

- The parser can detect file-load intent, but TempPlaybackSwitch is the canonical owner of “which image is live right now”.
- Reload safety and post-background-load resnapshot timing both depend on that live-source truth.

### 8. Trigger the post-background-load resnapshot at the normal/temp boundary, not earlier

#### `mainboard/LxrStm32/src/Sequencer/sequencer.c`

- In the existing pattern-switch block, after `preset_updateVoiceSourcesForPatternChange(...)`, detect the transition where the protected `.prf` / `.all` temp-preset window has genuinely ended.

The trigger condition should be:

- the protected-temp flag is armed;
- after the new pattern selection, all preset voice sources are normal again;
- the switch is no longer in pattern-only temp mode.

- Call the new TempPlaybackSwitch resnapshot helper there.

Important detail:

- Do not resnapshot merely because `seq_activePattern` stopped being `SEQ_TMP_PATTERN`.
- Resnapshot only when the preset voice sources have actually returned to normal, otherwise per-track temp cases can still be live.

Adjacent comment text to add:

```c
/* The protected .prf/.all temp image must survive until playback is truly back
   on normal preset data. Active-pattern change alone is not enough; per-voice
   source ownership must also be normal again before temp is repurposed as the
   new last-loaded snapshot. */
```

Why this change is necessary:

- The user explicitly wants the normal-to-temp copy delayed until playback is back on normal parameter data.
- Sequencer already owns the exact boundary where voice-source ownership changes.

### 9. Handle `PATCH_RESET` on STM authoritatively

#### `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

- Rewrite the existing top-level `else if(data==PATCH_RESET)` handling so it:
  - reject while protected temp preset playback is still active;
  - otherwise call the new TempPlaybackSwitch reload helper;
  - no longer set `seq_newVoiceAvailable = 0x7f`.

Adjacent comment text to add:

```c
/* AVR performs a UI-level disable for SHIFT+PLAY, but STM remains the
   authoritative guard. If temp preset playback is still the active sound
   source, reload is rejected here so mixed or stale front-panel state cannot
   trigger a forbidden temp->normal restore. PATCH_RESET remains the transport,
   but its implementation is now an STM-owned storage restore rather than a
   legacy voice-refresh flag. */
```

Why this change is necessary:

- AVR disable logic is intentionally lightweight and pattern-ack driven.
- STM must be the final authority because it knows the real voice-source state.

### 10. Preserve the role of the AVR file-load snapshots, but narrow it

#### `front/LxrAvr/Preset/presetManager.c`

- Do not remove `parameter_values_fileLoadSnapshot[]` or `parameters2_fileLoadSnapshot[]`.
- Update comments around them to clarify that they remain file-read staging buffers for AVR-side menu population and per-load transport, not the authoritative reload snapshot.

Adjacent comment text to add:

```c
/* These arrays remain AVR-side file-read staging buffers.
   They still seed live menu arrays during file import, but they no longer
   define SHIFT+PLAY reload semantics. STM temporary preset storage is now the
   only authoritative reload image. */
```

Why this change is necessary:

- The arrays are still used heavily by the file loader.
- The behavioral change is about ownership of reload semantics, not about deleting the AVR staging path.

### 11. Audit every current `PATCH_RESET` touchpoint and update each one deliberately

The current `PATCH_RESET` footprint is:

- `mainboard/LxrStm32/src/MIDI/MidiMessages.h`
  - canonical opcode definition/comment.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
  - AVR-side mirrored opcode definition/comment.
- `front/LxrAvr/Menu/menu.c`
  - `menu_reloadKit()` currently sends `PATCH_RESET` and also performs the local AVR snapshot restore.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`
  - AVR currently interprets inbound `PATCH_RESET` from STM by restoring `parameter_values[]` from `parameter_values_fileLoadSnapshot[]` and repainting.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
  - STM currently interprets inbound `PATCH_RESET` from AVR by setting `seq_newVoiceAvailable = 0x7f`.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.c`
  - STM still exposes `frontPanelSending_sendPatchReset()`, which currently emits outbound `PATCH_RESET` to AVR.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelSendingProtocol.h`
  - comment/API declaration for the send helper.
- `knowledge_files/hardware_archive/ATMEGA_STM32F4_COMMS_AUDIT.md`
  - parser/control-byte audit references `PATCH_RESET` as a special status byte and documents its historical meaning.

Plan impact at each occurrence:

- `menu.c`
  - keep the send; remove the local restore.
- AVR receive parser
  - either delete inbound `PATCH_RESET` handling entirely if STM will never send it anymore, or make it a no-op/compatibility comment block;
  - it must not restore AVR snapshot arrays anymore.
- STM receive parser
  - replace the `seq_newVoiceAvailable` behavior with the new temp->normal preset restore.
- STM sending protocol `.c/.h`
  - audit whether any live caller still needs outbound `PATCH_RESET`;
  - if no live caller remains, remove the helper or leave it documented as unused compatibility surface pending cleanup.
- opcode definition comments
  - update both sides to describe the new semantics.
- archived hardware audit note
  - no code change required for the feature itself, but the session closeout should note that the documented functional meaning changed even though the byte value stayed the same.

Adjacent comment text to add near any surviving outbound STM helper:

```c
/* PATCH_RESET is now an AVR->STM reload request path.
   If this helper remains for compatibility, it must not be used to ask AVR to
   restore local snapshot arrays; that behavior is retired. */
```

Why this change is necessary:

- `PATCH_RESET` already exists on both MCUs in both send and receive paths.
- Reusing it safely requires an explicit full-path audit so the old bidirectional meanings do not linger in one parser while the other side has moved on.

## Implementation Order

Recommended implementation order:

1. Update the audit/meaning of `PATCH_RESET` on both MCUs so the intended transport is explicit.
2. Add the AVR-side temp-preset-playback flag and optional `PATCH_RESET` send helper.
3. Replace `menu_reloadKit()` so `SHIFT+PLAY` sends `PATCH_RESET` without doing a local AVR snapshot restore.
4. Add the STM endpoint-copy helper in `KitState`.
5. Add the STM endpoint-copy suppression guard in `ParameterIngress`.
6. Add the STM temp-preset state fields and reload/resnapshot helpers in `TempPlaybackSwitch`.
7. Add parser-side file-load classification and immediate mirror logic in `frontPanelReceivingProtocol.c`.
8. Hook the post-background-load resnapshot into `sequencer.c`.
9. Rewrite the STM `PATCH_RESET` receive path and retire the AVR inbound snapshot-restore meaning.
10. Audit/remove any obsolete outbound STM `PATCH_RESET` helper usage.
11. Retest the full matrix below.

This order keeps the storage primitives in place before the parser and sequencer start relying on them.

## Verification Matrix

Minimum manual regression tests after implementation:

1. Load `.snd` full kit with sequencer stopped, edit a front endpoint, press `SHIFT+PLAY`, confirm both sound and menu return to the loaded kit and morph endpoint remains unchanged.
2. Load a single instrument from `.snd`, edit only that voice, press `SHIFT+PLAY`, confirm the loaded instrument returns and unrelated morph endpoint data does not change.
3. Load morph kit, edit morph-only parameters, press `SHIFT+PLAY`, confirm morph endpoint restores without overwriting kit/front endpoint edits that were never part of that morph load.
4. Load `.prf` with background mode off, edit kit and morph parameters, press `SHIFT+PLAY`, confirm both endpoint groups restore.
5. Load `.all` with background mode off, same as above.
6. Start playback on a normal pattern, background-load `.prf`, confirm old sound holds in temp while new preset loads into normal.
7. During that protected `.prf` temp window, confirm `SHIFT+PLAY` does nothing.
8. Manually switch back to a normal pattern, then edit parameters and press `SHIFT+PLAY`, confirm the newly loaded `.prf` restores from STM temp.
9. Repeat the same protected-window test for `.all`.
10. Background-load `.pat`, confirm `SHIFT+PLAY` is not blocked by the pattern-only temp case and preset reload behavior stays unchanged.
11. While still on temp after a protected `.prf` / `.all`, perform a non-background load and confirm the newly loaded preset becomes the new temp snapshot immediately.

## Main Risk Areas

- Partial endpoint copies must not accidentally overwrite the untouched endpoint group.
- The post-background-load resnapshot must wait for real voice-source return to normal, not just active-pattern change.
- The live DSP must be refreshed after temp->normal reload, otherwise storage will be correct but the audible state may lag.
- AVR UI disable is only a convenience layer; STM must remain the authority for rejecting reload while temp preset playback is live.

## Non-Goals For This Pass

- No new cache model.
- No revival of `PresetLoadCache` or the old PRF cache opcodes.
- No change to `.pat`’s pattern-only background behavior.
- No new opcode for reload; this pass intentionally keeps `PATCH_RESET` as the transport and changes only its semantics.
