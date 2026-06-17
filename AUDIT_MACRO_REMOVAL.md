# Macro Removal Audit

Session 025 objective: stop loading macro assignments from files, disable the live code paths that still apply macro parameter changes, and leave clear breadcrumb comments at every parameter/text site that currently exists only for legacy macro support.

This audit is based on the current working tree state plus the existing menu-page change already in progress in `front/LxrAvr/Menu/menuPages.h`.

## Current state

- The AVR performance page has already been edited so the macro controls no longer appear in the menu.
- The remaining macro surface is still present in both firmware halves:
  - AVR file-load and menu-edit paths still know how to read, send, and apply macro values.
  - STM front-panel receive logic still accepts `MACRO_CC` and applies it to live modulation nodes.
  - STM Preset code still stores and replays macro destinations during endpoint restore.
- The file-load path already has a natural choke point in `front/LxrAvr/Preset/presetManager.c`, because every file-supported sound image is first read into one of the `*_fileLoadSnapshot[]` buffers before it is copied into live parameter storage.

## Execution Log

- Started the implementation pass by confirming the current menu-page edit is already staged in the worktree.
- Next step is file-load normalization in `preset_readKitToTemp()`, then the AVR and STM live macro branches will be disabled in-place.
- File-load normalization is now in `front/LxrAvr/Preset/presetManager.c`; the legacy macro slots are forced to zero immediately after the SD read and before any snapshot is copied forward.
- The AVR menu send/apply paths are now wrapped in disabled blocks, so the front panel still parses the old menu structures but no longer emits macro changes.
- The STM receive/apply paths were frozen rather than deleted: the macro packet case now ignores the payload, the CC2 macro-amount handler is disabled, and the Preset replay/storage helpers no longer mutate macro destinations.
- The first zeroing pass revealed that `PAR_MAC1` and `PAR_MAC2` are not part of the file snapshot, so those amounts were moved to the live-array reset path in `preset_readDrumsetMeta()` instead of being written out of bounds.
- `make -C front/LxrAvr avr -j4`, `make -C mainboard/LxrStm32 -j4 stm32`, and `make firmware` all completed successfully after the cleanup.
- The remaining `MACRO_CC` / `avrComms_sendMacro()` references in the tree are now either disabled blocks or zero-value restore helpers, so there is no active macro apply path left for the deprecation test.

## Required code changes

### 1. Zero macro assignments during file import

File-supported sounds should still load, but all macro-related slots should be normalized to `0` before any copy into live parameter arrays happens.

Target file:

- `front/LxrAvr/Preset/presetManager.c`

Plan:

- Extend `preset_readKitToTemp()` so that after `f_read()` succeeds and before any downstream copy or normalization, the macro parameters in the active snapshot buffer are forced to zero.
- Apply the zeroing to both snapshot modes:
  - `parameter_values_fileLoadSnapshot[]`
  - `parameters2_fileLoadSnapshot[]`
- Zero every legacy macro slot, not just the top-level macro amount:
  - `PAR_MAC1`
  - `PAR_MAC2`
  - `PAR_MAC1_DST1`
  - `PAR_MAC1_DST1_AMT`
  - `PAR_MAC1_DST2`
  - `PAR_MAC1_DST2_AMT`
  - `PAR_MAC2_DST1`
  - `PAR_MAC2_DST1_AMT`
  - `PAR_MAC2_DST2`
  - `PAR_MAC2_DST2_AMT`
- Keep the existing `PAR_VEL_DEST_*` and `PAR_TARGET_LFO*` normalization intact. Those are not macro controls and should not be collateral damage.
- Add a comment at the zeroing site stating that macro file-load support is intentionally being retired and that these fields are being forced to neutral values until the feature is removed.

Why this matters:

- It preserves file compatibility and load flow while ensuring macro assignments do not survive load.
- It makes the later apply-path cleanup safer, because no file import will resurrect non-zero macro wiring.

### 2. Disable AVR live macro apply behavior

The AVR still has two active edit-time paths that emit macro messages. Those need to be commented out or wrapped in a disabled block so the test build no longer applies macro changes from front-panel interaction.

Target file:

- `front/LxrAvr/Menu/menu.c`

Plan:

- Comment out the `PAR_MAC1` and `PAR_MAC2` branches in `menu_processSpecialCaseValues()`.
  - Those branches currently call `menu_vMorph()` for the two macro destinations and then send the macro amount with `avrComms_sendMacro()`.
  - For the test build, those branches should remain visible in source only as commented-out legacy logic.
- Disable the `DTYPE_AUTOM_TARGET` macro-send path used during performance-page editing.
  - There are two places to pay attention to:
    - the edit-value conversion path that emits `MACRO_CC` when the performance page is active
    - the value-commit path later in the file that also emits `MACRO_CC` for the same parameter family
  - Both should become no-op/commented code rather than active senders.
- Leave the non-macro `DTYPE_AUTOM_TARGET` display logic intact.
  - The menu can still render the old parameter metadata and resolve target labels for now.
  - Only the behavior that actively sends macro changes should be disabled.
- Add comments where the macro send branches used to be, explaining that these are legacy performance-macro hooks retained only during the deprecation window.

Why this matters:

- This removes the AVR-originated live path that can still push macro changes across the link.
- It also keeps the menu code readable during the transition, instead of deleting the branches before we know the new behavior is stable.

### 3. Disable STM live macro apply behavior

Even if AVR stops sending macro changes, the STM should also stop applying them so no stale or external packet can keep the feature alive during the test.

Target file:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`

Plan:

- Comment out or convert the `FRONT_CC_MACRO_TARGET` case into a no-op that only documents the legacy packet format.
- Remove the live effects inside that case:
  - the top-level macro amount updates for macro pair 1
  - the top-level macro amount updates for macro pair 2
  - the destination writes through `preset_storeMacroDestinationIngress()`
  - the direct `modNode_setDestination()` / `modNode_updateValue()` replay for macro destinations
- Keep the packet-format comment block, but mark it as deprecated legacy protocol context.

Why this matters:

- This makes the STM ignore macro traffic rather than just storing it.
- It prevents a stale packet or hidden sender from re-enabling the feature while the test build is running.

### 4. Disable STM macro replay on boundary restore

The Preset layer still has a second live-apply route for macro destinations: it replays stored macro routing when a kit boundary closes. That should be commented out as part of the same test.

Target file:

- `mainboard/LxrStm32/src/Preset/MorphEngine.c`

Plan:

- Comment out `preset_applySharedAutomationTargets()` or make it skip macro destinations entirely.
- If the function remains in place during the transition, it should not call:
  - `modNode_setDestination(&macroModulators[i], ...)`
  - `modNode_updateValue(&macroModulators[i], ...)`
- Add a comment stating that the shared macro target replay is legacy behavior and is intentionally frozen for the deprecation test.

Why this matters:

- File-load zeroing alone is not enough if a later boundary restore can still reapply old macro destinations.
- This is the STM-side equivalent of disabling the AVR edit-time macro sender.

### 5. Comment or freeze the macro destination storage helper

If we leave the storage helper active while apply logic is disabled, the code will still collect macro destinations even though they no longer do anything. That is acceptable for a short test, but it should be clearly marked as deprecated.

Target file:

- `mainboard/LxrStm32/src/Preset/ParameterIngress.c`

Plan:

- Add a deprecation comment above `preset_storeMacroDestinationIngress()` explaining that it is legacy macro routing storage and should be removed after the test.
- If the goal is to make the test build fully inert, wrap the body in a disabled block so writes no longer update:
  - `macroDestination[]`
  - `macroDestinationValid`
- If the helper is left active for now, it should at least be clearly labeled as temporary compatibility code so future cleanup can delete it safely.

Why this matters:

- It keeps the deprecation story consistent on the STM side.
- It also makes later removal simpler because the storage helper will already be isolated by comment and/or a disabled block.

## Comments to add at the legacy parameter/text sites

These sites should not be removed yet because they still anchor enum indices and menu arrays, but they should get explicit comments saying the macro feature is being retired.

### AVR parameter enum

Target file:

- `front/LxrAvr/Parameters.h`

Plan:

- Add a comment above `PAR_MAC1_DST1` through `PAR_MAC2_DST2_AMT` noting that these are legacy macro storage slots kept only while file-load and protocol cleanup are in flight.
- Add a comment above `PAR_MAC1` and `PAR_MAC2` noting that the top-level macro amount parameters are now hidden/deprecated and should eventually be removed once the last file/protocol dependencies are gone.
- Keep the enum order unchanged for now.

### AVR menu labels and category tables

Target files:

- `front/LxrAvr/Menu/menu.h`
- `front/LxrAvr/Menu/MenuText.h`

Plan:

- In `menu.h`, add comments on the `TEXT_MAC*`, `TEXT_MAC*_DST*`, `CAT_MACRO*`, and `DTYPE_AUTOM_TARGET` entries stating that these exist only for legacy macro plumbing.
- In `MenuText.h`, add comments beside the short-name and long-name entries for macro labels so it is obvious they are placeholders for deprecated UI slots, not active features.
- Keep the arrays intact for index stability until the code paths are removed.

### AVR menu page

Target file:

- `front/LxrAvr/Menu/menuPages.h`

Plan:

- Add a short comment near the already-edited performance page block explaining that the macro controls were intentionally removed from the visible UI and that the blanked slots are legacy placeholders only.

### STM Preset parameter bindings

Target file:

- `mainboard/LxrStm32/src/Preset/ParameterArray.c`

Plan:

- Add comments above the `PAR_MAC1_DST*` / `PAR_MAC2_DST*` bindings stating that these parameter array entries are legacy macro storage and are being kept only until the macro path is fully removed.
- Keep the bindings unchanged for now so the rest of the code compiles while the deprecation test is running.

### STM kit-state storage

Target file:

- `mainboard/LxrStm32/src/Preset/KitState.h`

Plan:

- Add comments above `macroDestination[]` and `macroDestinationValid` explaining that they are legacy compatibility storage, not part of the intended post-deprecation data model.

## Optional follow-on cleanup after the test passes

If the deprecation test works cleanly, the next pass can remove the now-dormant macro helpers instead of just commenting them out.

Likely follow-on targets:

- `front/LxrAvr/avrComms/avrCommsSendingProtocol.c`
  - `avrComms_sendMacro()` should become dead code or be removed if no remaining call sites exist.
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
  - `MACRO_CC` can be commented or removed once the last sender and receiver are gone.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
  - the `MACRO_CC` packet-format comment can be shortened or removed after the case is deleted.
- `mainboard/LxrStm32/src/Preset/ParameterIngress.c`
  - `preset_storeMacroDestinationIngress()` and its declarations can be deleted once nothing writes macro destinations anymore.
- `mainboard/LxrStm32/src/Preset/MorphEngine.c`
  - `preset_applySharedAutomationTargets()` can be removed once nothing needs to replay macro destinations.
- `front/LxrAvr/Menu/menu.c`
  - the remaining macro branches can be deleted after the test confirms no code path still depends on them.

## Verification plan

After the changes above, the next verification pass should check:

- AVR build still succeeds.
- STM32 build still succeeds.
- `make firmware` still succeeds.
- Loading file formats that previously carried macro data now leaves:
  - macro amounts at `0`
  - macro destination slots at `0`
  - no visible macro page in the menu
  - no live macro modulation updates from front-panel edits or file restore

## Summary

The minimum safe path is:

1. Zero all macro-related file-load slots in the AVR preset importer.
2. Comment out the AVR live send/apply macro branches.
3. Comment out the STM receive/apply macro branches.
4. Add explicit deprecation comments at the AVR parameter/menu-text sites and the STM parameter storage sites.

That gets the feature into a controllable “disabled but still readable” state so the remaining removal can happen safely later.
