# LXR -bc- Enhanced Firmware — Session 025 Handoff Log

**Project**: LXR -bc- Enhanced Firmware  
**Session goal**: Disable macro loading from files, deprecate the live macro application path, and leave durable breadcrumbs at every macro-related parameter/text site so the remaining cleanup can happen safely later.  
**Last session summary**: Session 024 commented out the stale opcode surface and archived the opcode audit into the durable log archive while leaving the live non-cache file-load/session path intact.  
**Working repository**: `/Users/bc/LXR01/LXR-current/LXR` on branch `master-avr-fp-clean`; the tree now contains the macro deprecation cleanup, regenerated firmware outputs, and the temporary audit file that can be deleted after this handoff is preserved.  
**Constraints today**: Keep the file-load and menu paths functional enough for testing, but do not leave any active macro send/apply path in place. Preserve the non-macro file-load/session behavior and avoid broad comms refactors.

Key files to be aware of:

- `front/LxrAvr/Preset/presetManager.c`
- `front/LxrAvr/Menu/menu.c`
- `front/LxrAvr/Menu/menu.h`
- `front/LxrAvr/Menu/MenuText.h`
- `front/LxrAvr/Menu/menuPages.h`
- `front/LxrAvr/Parameters.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
- `mainboard/LxrStm32/src/Preset/ParameterIngress.c`
- `mainboard/LxrStm32/src/Preset/MorphEngine.c`
- `mainboard/LxrStm32/src/Preset/ParameterArray.c`
- `mainboard/LxrStm32/src/Preset/KitState.h`
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
- `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`
- `MEMORY.md`
- `AUDIT_MACRO_REMOVAL.md`

## Session 025 Summary

Session 025 converted the macro deprecation audit into code and documentation changes. The AVR performance-page macro UI was already removed in the preceding work, and this session finished the deprecation pass by forcing legacy macro slots to zero during file import, disabling the AVR menu-side macro send/apply branches, disabling the STM front-panel macro receive/apply branches, and freezing the Preset-side macro storage/replay helpers so they no longer mutate live state. The session also added breadcrumb comments at the legacy macro parameter and menu-text sites so those structures can be removed later without losing context.

The practical result is that macro data no longer survives file load, the front panel no longer emits live macro changes, the STM no longer applies macro packets, and the remaining references are inert legacy context only.

## Implementation Details

### AVR file-load normalization

`front/LxrAvr/Preset/presetManager.c` now neutralizes the legacy macro slots during file import.

What changed:

- After the SD file read succeeds, the legacy macro destination slots in the active snapshot buffer are forced to zero before the buffer is copied forward.
- The zeroing applies to:
  - `PAR_MAC1_DST1`
  - `PAR_MAC1_DST1_AMT`
  - `PAR_MAC1_DST2`
  - `PAR_MAC1_DST2_AMT`
  - `PAR_MAC2_DST1`
  - `PAR_MAC2_DST1_AMT`
  - `PAR_MAC2_DST2`
  - `PAR_MAC2_DST2_AMT`
- The top-level macro amount parameters, `PAR_MAC1` and `PAR_MAC2`, are zeroed in the live parameter arrays instead of the file snapshot because they are not part of the file-backed snapshot layout.
- The existing velocity/LFO target normalization remains intact and unchanged.

Important finding:

- The first zeroing pass tried to write `PAR_MAC1` and `PAR_MAC2` into the file snapshot and that revealed they are not file-backed in the snapshot array. The fix was to move those resets to the live arrays in `preset_readDrumsetMeta()`.

Why this matters:

- File loads still work, but they no longer bring back any non-zero macro assignments.
- The live arrays start from neutral macro values even if the source file contains legacy macro data.

### AVR live macro application disabled

`front/LxrAvr/Menu/menu.c` keeps the old macro logic in source only as disabled legacy context.

What changed:

- The `PAR_MAC1` and `PAR_MAC2` branches in `menu_processSpecialCaseValues()` are wrapped in a disabled block.
- The performance-page `DTYPE_AUTOM_TARGET` path that used to emit `MACRO_CC` is disabled in both places where the menu could still generate macro traffic.
- The non-macro `DTYPE_AUTOM_TARGET` display logic stays active so the menu can still resolve and display the legacy target metadata.
- A small cast to `(void)paramNr` was added so the compiler does not complain about the now-disabled parameter path.

What stayed active:

- The rest of the menu rendering and parameter editing logic remains unchanged.
- The menu still parses the legacy structures, but the macro-specific send/apply side effects are gone.

### STM live macro receive/application disabled

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c` now ignores macro traffic.

What changed:

- The `FRONT_CC_MACRO_TARGET` case is disabled and left only as historical context.
- The top-level macro amount updates for both macro pairs are no longer applied.
- The destination writes through `preset_storeMacroDestinationIngress()` are no longer active.
- The direct `modNode_setDestination()` / `modNode_updateValue()` replay for macro destinations is no longer active.
- The CC2 macro-amount handler was also disabled so macro amounts cannot still change indirectly through the alternate control path.

Why this matters:

- Even if a stale packet shows up, the STM no longer treats macro traffic as live state.
- This closes the second half of the macro path so the deprecation test cannot be bypassed by one side of the link still accepting the old packets.

### STM Preset macro replay/storage frozen

`mainboard/LxrStm32/src/Preset/ParameterIngress.c` and `mainboard/LxrStm32/src/Preset/MorphEngine.c` now keep the legacy macro helpers inert.

What changed:

- `preset_storeMacroDestinationIngress()` no longer mutates `macroDestination[]` or `macroDestinationValid`; it is now a no-op stub with a deprecation comment.
- `preset_applySharedAutomationTargets()` no longer replays macro destinations into `macroModulators[]`; it is now a no-op stub with a deprecation comment.

Why this matters:

- The STM no longer has a hidden replay path that could resurrect macro routing after a boundary switch or restore.
- The helpers still exist as readable historical context, which makes later deletion safe and intentional rather than risky.

### Macro parameter and menu-text breadcrumbs

The legacy macro parameter and menu text sites were annotated so future cleanup can remove them safely.

Updated files:

- `front/LxrAvr/Parameters.h`
- `front/LxrAvr/Menu/menu.h`
- `front/LxrAvr/Menu/MenuText.h`
- `front/LxrAvr/Menu/menuPages.h`
- `mainboard/LxrStm32/src/Preset/ParameterArray.c`
- `mainboard/LxrStm32/src/Preset/KitState.h`

What the comments now say:

- `PAR_MAC1` / `PAR_MAC2` are legacy macro amount parameters.
- `PAR_MAC1_DST*` / `PAR_MAC2_DST*` are legacy performance-macro destination storage slots.
- `TEXT_MAC*`, `TEXT_MAC*_DST*`, `CAT_MACRO*`, and `DTYPE_AUTOM_TARGET` are legacy macro UI/plumbing entries.
- The performance-page macro controls were intentionally removed from the visible menu and the blanked slots are placeholders only.
- The STM macro destination storage fields are legacy compatibility storage, not part of the intended long-term data model.

### Comms/documentation refresh

The comms spec and the temp/parameter load spec were updated so they no longer read as if macro routing is still an active feature.

Updated files:

- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
- `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`
- `MEMORY.md`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`

What changed in the docs:

- `MACRO_CC` is now documented as deprecated/ignored historical context rather than live control traffic.
- `preset_storeMacroDestinationIngress()` is now treated as inert legacy storage.
- The session status text now points at the macro deprecation cleanup so future sessions do not treat the old macro path as active.
- `MEMORY.md` now records the macro deprecation closeout and the fact that the active macro apply path was removed.

## Verification

Build verification was completed after the code changes:

- `make -C front/LxrAvr avr -j4`
- `make -C mainboard/LxrStm32 -j4 stm32`
- `make firmware`

Verification notes:

- The first build pass exposed a useful edge case: `PAR_MAC1` and `PAR_MAC2` are not part of the file snapshot array, so those resets had to move into the live parameter arrays.
- After that correction, the AVR and STM32 builds completed successfully and the aggregate firmware image was regenerated.
- No hardware re-test was needed for this session because the work was a deprecation/cleanup pass, not a new behavior feature.

## Residual State

What remains in the tree:

- Legacy macro names still exist in disabled blocks and comment-only historical context.
- The menu, parameter, and Preset structures still carry the old enum/index slots for now so the code keeps compiling while the deprecation window is open.
- The next cleanup pass can remove the now-inert macro helpers and opcode surfaces once the test confirms nothing still depends on them.

What no longer exists as active behavior:

- Macro assignments are no longer loaded from files.
- The AVR no longer emits live macro changes.
- The STM no longer applies live macro packets.
- The Preset layer no longer replays or stores macro destinations as live state.

## End of Session Block

```
DATE: 2026-06-17
SESSION GOAL: Disable macro loading from files, deprecate the live macro application path, and leave durable breadcrumbs at every macro-related parameter/text site so the remaining cleanup can happen safely later.
COMPLETED: Macro file-load slots are zeroed, the AVR/STM live macro send/apply paths are disabled, the Preset macro storage/replay helpers are inert, and the macro-related parameter/text sites now carry deprecation comments. The audit notes were promoted into this permanent handoff log.
VERIFIED ON HARDWARE: no; builds only (`make -C front/LxrAvr avr -j4`, `make -C mainboard/LxrStm32 -j4 stm32`, `make firmware`)

CHANGES THIS SESSION:
- `front/LxrAvr/Preset/presetManager.c`: zeroed legacy macro slots during file import; moved `PAR_MAC1` / `PAR_MAC2` resets to the live arrays because they are not file-backed.
- `front/LxrAvr/Menu/menu.c`: disabled the macro send/apply branches while keeping non-macro menu behavior intact.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`: disabled the macro packet case and the CC2 macro-amount handler.
- `mainboard/LxrStm32/src/Preset/ParameterIngress.c`: froze `preset_storeMacroDestinationIngress()` as an inert stub.
- `mainboard/LxrStm32/src/Preset/MorphEngine.c`: froze `preset_applySharedAutomationTargets()` as an inert stub.
- `front/LxrAvr/Parameters.h`, `front/LxrAvr/Menu/menu.h`, `front/LxrAvr/Menu/MenuText.h`, `front/LxrAvr/Menu/menuPages.h`, `mainboard/LxrStm32/src/Preset/ParameterArray.c`, `mainboard/LxrStm32/src/Preset/KitState.h`: added deprecation breadcrumbs for the legacy macro parameter/text/storage sites.
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`, `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`, `MEMORY.md`, `knowledge_files/log_archive/000_SESSION_INDEX.md`: updated the durable docs to mark macro routing as deprecated and inactive.

KNOWN ISSUES INTRODUCED: none observed in the build; the remaining macro names are still present as legacy disabled context only.
KNOWN ISSUES RESOLVED: file loads no longer preserve macro assignments; the AVR and STM no longer apply macro changes; the Preset macro replay/storage helpers no longer mutate live state.
NEXT SESSION RECOMMENDED GOAL: remove the now-dormant macro helpers and opcode surface once the deprecation test is confirmed stable.
BLOCKERS: none
CRITICAL REMINDERS FOR NEXT SESSION:
- Do not re-enable the macro send/apply path unless the deprecation is explicitly reversed.
- Keep the file-load zeroing in `presetManager.c` until the legacy macro slots are deleted.
- Treat `MACRO_CC` and the macro helper names as legacy-only context from here forward.
```
