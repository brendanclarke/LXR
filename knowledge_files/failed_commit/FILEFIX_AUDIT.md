# FILEFIX Audit
Date: 2026-05-25

## Scope
Investigated why prior `.ALL`, `.PRF`, and kit files in `SdCardImage/` are not loading correctly, and mapped code-level causes plus a compatibility-first remediation path.

## Files Examined

| File | Size (bytes) | Header bytes (name + version + next fields) |
|---|---:|---|
| `P000.ALL` | 51514 | `FrstAll ` + `0x05` |
| `P001.ALL` | 50946 | `1ally   ` + `0x02` |
| `P003.ALL` | 51514 | `RW...   ` + `0x05` |
| `P001.PRF` | 50946 | `FrstPrf ` + `0x02` |
| `P002.PRF` | 50946 | `2prfm   ` + `0x02` |
| `P000.SND` | 229 | `Slak    ` + `0x03` |
| `P002.SND` | 236 | `RedSnap ` + `0x03` |

## Findings

1. Legacy v2 `.ALL`/`.PRF` layout is valid but not handled by current offset logic.
Code accepts `version <= FILE_VERSION` (`FILE_VERSION` is `5`), so v2 files are considered loadable.
But pattern-data readers use only v4/v5 offsets for `.ALL`/`.PRF`:
- `VERSION_4_ALL_*` and `VERSION_4_PERF_*` constants in [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c):29-49
- Used unconditionally in `preset_readPatternStepData`, `preset_readPatternMainStep`, `preset_readPatternChain`, `preset_readShuffle`, `preset_readPatternLength`, `preset_readPatternScale` at [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c):1268-2025.

Observed size delta is exactly `568` bytes:
- `51514 - 50946 = 568 = 512 (morph block) + 56 (scale block)`.
- This strongly indicates old files are a legacy valid format with no morph block and no scale block at tail.

2. `.PRF` legacy kit header offset appears different from current `VERSION_1_PERF_KIT_OFFSET`.
Current constant is `74` for old perf (`VERSION_1_PERF_KIT_OFFSET`) at [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c):36.
Sample v2 `.PRF` binary shows kit-like payload beginning at offset `73` (`0x49`), matching legacy `.ALL` alignment, not `74`.
This creates a 1-byte misalignment risk when loading kit/morph data from old `.PRF`.

3. EOF/short-read handling is brittle in pattern readers.
In multiple readers, only `res` is checked, not `bytesRead`:
- `preset_readPatternScale`: [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c):1341-1359
- `preset_readPatternLength`: [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c):1476-1494
- `preset_readPatternChain`: [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c):1697-1733
- `preset_readPatternMainStep`: [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c):1848-1867
- `preset_readPatternStepData`: [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c):1989-2004

If `f_lseek` lands at EOF (possible in legacy layout mismatch), `f_read` can return success with `bytesRead = 0`, leaving arrays/structs stale or undefined for transmission.

4. `preset_readShuffle` checks stale `res` instead of the `f_read` result.
In `preset_readShuffle`, `f_read` is called but `res` is not updated before `if(res)`:
- [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c):1591-1607
This is a correctness bug independent of file version.

5. Global-file loader has undefined behavior from uninitialized `bytesRead`.
`preset_readGlobalData` uses `bytesRead` in loop condition before first read:
- [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c):323-325
This can cause non-deterministic behavior on short `GLO.CFG`/`GLO2.CFG`.

6. Kit extension support is rigid (`.snd` only).
`preset_makeFileName` emits only `.snd` for kit-type loads:
- [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c):2868-2889
If actual cards contain `.kit` naming, open will fail even when data is valid.

## Why These Files Fail Today

### `.ALL` / `.PRF` version 2 files (`50946` bytes)
Loader accepts version but then seeks using v4/v5 offsets that assume larger files.
That mismatch leads to one or more of:
- wrong section boundaries,
- reads from EOF or beyond available section,
- transmission of default/uninitialized section buffers,
- misloaded or corrupted sequence/voice/morph data.

### `.SND` files
These appear structurally plausible and are likely loadable, but are still exposed to downstream parameter-index safety issues if contents are unusual (separate topic from this file-layout audit).

## Proposed Code-Change Pathway

### Phase 1: Add explicit file-layout profiles (highest impact)
Implement a small `PresetLayout` resolver and use it in all `.ALL`/`.PRF` readers instead of hardcoded v4 offsets.

Suggested profile fields:
- `kitOffset`, `morphOffset`
- `stepOffset`, `mainStepOffset`, `chainOffset`, `shuffleOffset`, `lengthOffset`, `scaleOffset`
- `hasMorph`, `hasScale`

Detection inputs:
- `workingType` (`ALL` vs `PERF`)
- `workingVersion`
- file size from FAT (`f_size`)

Known profiles from observed data:
- `ALL_V5`: existing v4/v5 constants (size `51514`)
- `ALL_V2_LEGACY`: `kit=73`, `morph=absent`, `step=585`, `main=50761`, `chain=50873`, `shuffle=50889`, `length=50890`, `scale=absent` (size `50946`)
- `PERF_V2_LEGACY`: treat same as legacy all-style 64-byte pre-kit header (kit at `73`), no morph block, no scale block (size `50946`)
- `PERF_V5`: existing v4/v5 perf constants

Important: use profile flags to skip non-existent blocks (for example, do not call scale reader when `hasScale == 0`).

### Phase 2: Make reads prefix-tolerant and deterministic
Across all section readers:
- Initialize destination arrays/structs to defaults before reading.
- Treat `bytesRead < expected` as EOF/short-data for that section, not immediate corruption.
- Load first N bytes that exist, default missing suffix.
- Keep hard failure only for true filesystem errors (`res != FR_OK`) where recovery is unsafe.

This matches your desired behavior: "more or fewer bytes than expected -> use first valid bytes."

### Phase 3: Fix correctness bugs in existing read paths
1. `preset_readShuffle`: capture and check `f_read` result, not stale `res`.
2. `preset_readGlobalData`: initialize `bytesRead` and remove undefined loop condition.
3. `preset_loadPerf` metadata parse: make v2/vlegacy perf field consumption profile-aware so bytes are not shifted.

### Phase 4: Extension compatibility for kit files
For kit loads, attempt fallback open sequence if `.snd` fails:
1. `.snd`
2. `.kit`
3. uppercase variants if needed by host tooling

This can be done without changing save format.

## Validation Plan (after implementation)

1. Load each file in `SdCardImage/` as its corresponding type:
- `P000.ALL`, `P001.ALL`, `P003.ALL`
- `P001.PRF`, `P002.PRF`
- `P000.SND`, `P001.SND`, `P002.SND`, `P003.SND`

2. Verify:
- No loader error messages for valid legacy files.
- No garbage scale/length/chain writes when sections are absent.
- Kit + morph state coherence for legacy files without morph block.
- Deterministic behavior with short `GLO.CFG`.

3. Save a new `.ALL` and `.PRF`, then reload and compare key globals/patterns/kit params for round-trip consistency.

## Bottom Line
Your sample files look valid as a mixed-generation archive.
Primary blocker is not "bad files", it is a version/layout compatibility gap in `.ALL`/`.PRF` readers, plus brittle short-read handling.
The safest path is a layout-profile layer plus prefix-tolerant reads, then targeted correctness fixes.
