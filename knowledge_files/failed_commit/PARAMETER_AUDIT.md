# Parameter Handling Audit (Session 003)

## Scope
Audit focus:
- Parameter read/transmit/store behavior for `kit`, `performance` (`.prf`), `all` (`.all`), and global config (`glo.cfg`) paths.
- Cross-MCU handling between AVR frontpanel code and STM32 mainboard code.
- Edge/failure cases, especially modulation destinations (LFO, velocity, macro, step automation).

Primary files reviewed:
- `front/LxrAvr/Preset/presetManager.c`
- `front/LxrAvr/frontPanelParser.c`
- `front/LxrAvr/Menu/menu.c`
- `front/LxrAvr/Menu/Cc2Text.c`
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
- `mainboard/LxrStm32/src/DSPAudio/modulationNode.c`
- `mainboard/LxrStm32/src/DSPAudio/automationNode.c`
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`

## High-level flow map

### 1) Kit / Perf / All file load path (AVR)
1. File opened, version read, and blocks read into temp arrays (`parameter_values_temp`, `parameters2_temp`).
2. Voice masks move selected parameters from temp arrays to live arrays.
3. AVR transmits parameter changes to STM32 via MIDI-like messages:
   - `MIDI_CC` / `CC_2` for direct params.
   - `CC_LFO_TARGET`, `CC_VELO_TARGET`, `MACRO_CC` for modulation destination mappings.
   - SysEx-like bulk for pattern/mainstep/chain/length/scale data.
4. STM32 parser applies directly or caches while `seq_voicesLoading` is active.

### 2) Globals path (AVR)
1. `glo.cfg` is read into `parameter_values[PAR_BEGINNING_OF_GLOBALS..NUM_PARAMS)`.
2. `menu_sendAllGlobals()` pushes values to STM32 sequencer/midi settings.

### 3) Runtime automation / modulation path
1. Front step automation destination controls (`PAR_P1_DEST/PAR_P2_DEST`) are represented as indices into AVR `modTargets`.
2. Wire format sends packed 7-bit values; STM32 converts to parameter numbers and stores into sequencer step data.
3. Sequencer step trigger updates automation nodes and/or voice morph routing.

## Findings (prioritized)

## Critical

1. **Uninitialized `bytesRead` in global config read loop (undefined behavior).**
- File: `front/LxrAvr/Preset/presetManager.c:323-325`
- `bytesRead` is used in loop condition before initialization.
- Risk: globals may be partially/incorrectly read depending on stack garbage; can silently misconfigure routing, clocking, or load behavior.

2. **Out-of-bounds risk when decoding automation destination from STM32 to AVR (`paramToModTarget[dst]`).**
- File: `front/LxrAvr/frontPanelParser.c:467-470`, `476-479`
- `dst` can be 0..255, but `paramToModTarget` is sized to `END_OF_SOUND_PARAMETERS`.
- Risk path: corrupted/malformed step destination message can read past table bounds; can corrupt parameter selection state.

3. **Unchecked modulation target index into `modTargets` for macro destinations from stored kit/perf/all data.**
- Files:
  - clamp exists only for velo/lfo targets: `front/LxrAvr/Preset/presetManager.c:495-503`
  - macro send path uses unchecked index: `front/LxrAvr/Preset/presetManager.c:671-674`
- Risk: malformed file with macro target index >= num mod targets can read invalid PROGMEM entry and send invalid destination to STM32.

4. **STM32 velocity target parser can index out of bounds and perform undefined shift.**
- File: `mainboard/LxrStm32/src/MIDI/frontPanelParser.c:1004-1012`
- `velModNr` is not range-checked; used in `(0x01 << velModNr)` and `velocityModulators[velModNr]`.
- Risk: malformed wire data can cause undefined behavior / memory corruption.

5. **STM32 LFO target parser has the same unchecked index/shift class of risk.**
- File: `mainboard/LxrStm32/src/MIDI/frontPanelParser.c:869-874`, `871`
- `lfoNr` is unbounded before shift and cache indexing.
- Risk: malformed data can trigger UB and bad cache writes.

6. **Multiple SysEx receive handlers trust track index extracted from wire without bounds checks.**
- File: `mainboard/LxrStm32/src/MIDI/frontPanelParser.c`
  - mainstep: `435-451`
  - pattern length: `508-521`
  - pattern scale: `540-557`
  - begin-pattern step transfer: `653-689`
- `currentTrack` derived as 3-bit value (0..7), while valid tracks are 0..6.
- Risk: track `7` writes out of array bounds if a byte is corrupted.

7. **`modNode_setDestination()` indexes `parameterArray` by unvalidated destination.**
- File: `mainboard/LxrStm32/src/DSPAudio/modulationNode.c:155-160`
- Pointer-null check occurs after indexing, so out-of-range destination can already access invalid memory.
- Risk: invalid destination from parser path can corrupt/invalid-read memory.

## High

8. **Velocity cache-availability flag bug in STM32 front parser.**
- File: `mainboard/LxrStm32/src/MIDI/frontPanelParser.c:1007-1009`
- Writes `midi_midiLfoCacheAvailable[velModNr]=1` instead of `midi_midiVeloCacheAvailable[...]`.
- Risk: during voice-load caching, velocity destination updates may not be applied on unhold/release consistently.

9. **Invalid LFO voice correction writes `track` (0..5), but valid stored range is 1..6.**
- File: `front/LxrAvr/Preset/presetManager.c:592-593`
- For track 0, corrected value becomes `0` (still invalid).
- Follow-on risk: menu logic assumes 1..6 and subtracts 1.

10. **Menu target-scaling path assumes valid LFO target voice; possible out-of-range `modTargetVoiceOffsets` access.**
- File: `front/LxrAvr/Menu/menu.c:3811-3814`
- `voiceNr = parameter_values[PAR_VOICE_LFOx]-1` has no guard.
- Risk: if invalid values are loaded, later knob turns can index invalid offset table entries.

11. **Step morph routing bug: param2 morph path uses `param1` when deriving voice bit.**
- File: `mainboard/LxrStm32/src/Sequencer/sequencer.c:423-426`
- Risk: wrong voice gets morphed when second automation destination is a morph parameter.

## Medium

12. **`PARAM_CC2` frontpanel update writes `parameters2[paramNr]` without verifying sound-parameter range.**
- File: `front/LxrAvr/frontPanelParser.c:673-675`
- `paramNr` can be up to 255; `parameters2` length is `END_OF_SOUND_PARAMETERS`.
- Risk: malformed message can overwrite outside morph-buffer bounds.

13. **`FRONT_SEQ_LOAD_VOICE`/`UNHOLD` shift by unvalidated `data2`.**
- File: `mainboard/LxrStm32/src/MIDI/frontPanelParser.c:1607`, `1611`
- Risk: undefined behavior if malformed voice id > 31 reaches this path.

14. **File read error handling often checks `res` but not `bytesRead`, allowing short-read silent defaults/stale state.**
- Examples:
  - `front/LxrAvr/Preset/presetManager.c:1476-1478`
  - `front/LxrAvr/Preset/presetManager.c:1591-1593`
- Risk: truncated/corrupt SD files can partially apply stale parameters with weak detection.

15. **Globals loaded from file are pushed directly to parser without range sanitization.**
- Files:
  - load + send: `front/LxrAvr/Preset/presetManager.c:2507-2523`, `2762-2763`
  - channel packing masks values: `front/LxrAvr/Menu/menu.c:3368-3414`
- Risk: out-of-range values can wrap/mask into unintended channels/settings instead of being rejected.

## Notes on existing protections
- Session 002 timeout/recovery additions reduced deadlock risk in transfer handshakes.
- `preset_readKitToTemp()` does sanitize LFO/velocity mod target indices against `getNumModTargets()`.
- Parser RX disable during bulk transfers reduces interleaving, but does not replace per-field bounds validation.

## Risk summary by data class
- **Kit/perf/all modulation targets:** highest risk area due to index-based destination encoding and multiple unchecked lookup sites.
- **Global config values:** moderate risk of silent misconfiguration due to weak read/init/validation behavior.
- **Pattern step automation destinations:** high risk under malformed/corrupt transport bytes due to cross-table mapping without hard bounds.

## Recommended remediation order
1. Add strict range guards before every index-based lookup (`modTargets`, `paramToModTarget`, `velocityModulators`, `midi_*Cache`, pattern track arrays).
2. Fix known correctness bugs:
   - velocity cache available flag (`midi_midiVeloCacheAvailable`)
   - `PAR_VOICE_LFO` correction to 1..6
   - `param2` morph voice calculation in `seq_parseAutomationNodes()`.
3. Initialize and validate file-read state (`bytesRead` init, short-read checks).
4. Add clamping/validation for global and macro destination parameters before transmit.
5. Treat wire-level malformed values defensively on STM32 parser boundaries (track/lfo/velo/macrodest).

## Session 003 Follow-up (2026-05-25): `.prf` + `load file fast`

### Direct findings

1. `.prf` loader currently reads kit blocks but does not apply voice sound parameters.
- File: [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c)
- In `preset_loadPerf()`, kit data is read (`preset_readKitToTemp(1)` and `preset_readKitToTemp(0)`), but there is no call to `preset_readDrumVoice()` in that function.
- `preset_readDrumVoice()` is the function that actually copies temp kit params to live params and transmits `SEQ_LOAD_VOICE`/`SEQ_UNHOLD_VOICE`.
- `preset_loadAll()` does call `preset_readDrumVoice()`, which explains why `.all` can appear correct while `.prf` does not.

2. With `load file fast = off`, deferred apply at pattern change is bypassed for `.prf` sound params in current flow.
- Files:
  - [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c)
  - [sequencer.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/Sequencer/sequencer.c)
- STM32 only uncaches pending voice params when `seq_newVoiceAvailable` is set and `seq_loadFastMode || seq_newPatternExecuted`.
- Because `.prf` load path never performs voice load/unhold transactions, this pending-voice path is not engaged for kit params.

3. Velocity modulation target cache flag bug is still present and affects deferred/immediate application consistency.
- File: [frontPanelParser.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/MIDI/frontPanelParser.c)
- In `FRONT_CC_VELO_TARGET` cache path, code sets `midi_midiLfoCacheAvailable[...]` instead of `midi_midiVeloCacheAvailable[...]`.
- Result: cached velocity destination changes can fail to apply when voices unhold/uncache.

4. Missing `PAR_FILE_LOAD_FAST` currently defaults to OFF, not ON.
- Files:
  - [menu.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Menu/menu.c)
  - [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c)
  - [sequencer.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/mainboard/LxrStm32/src/Sequencer/sequencer.c)
- `menu_init()` zeroes `parameter_values` and does not set `PAR_FILE_LOAD_FAST`, so AVR starts at `0`.
- STM32 `seq_loadFastMode` initializes to `0` and only changes when AVR sends `SEQ_LOAD_FAST`.
- For short/legacy globals:
  - `preset_readGlobalData()` sets missing bytes to `0`.
  - `.all` global loader maps filler `0xFF` to `0`.
- Net effect: if `.cfg` or `.all` omits the field, effective default becomes OFF.

5. Global read path still has undefined behavior that can destabilize defaults.
- File: [presetManager.c](/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod/front/LxrAvr/Preset/presetManager.c)
- `preset_readGlobalData()` uses `bytesRead` in loop condition before initialization.
- This can make legacy/short `glo.cfg` handling non-deterministic.

### Practical conclusion for your current card set

- Your `.prf` files are structurally plausible and should be treated as valid.
- The main runtime issue is not only file validity; it is the `.prf` load execution path not applying voice sound parameters, plus `load file fast` defaulting OFF when absent.
- This combination can present exactly as: `.all` appears to load, `.prf` appears not to, and off-mode pattern-change application appears broken.

### No-code-change remediation pathway (next implementation pass)

1. In `preset_loadPerf()`, mirror the voice-apply stage used by `preset_loadAll()`:
- for selected voices: apply morph + main voice params via `preset_readDrumVoice()`;
- then unhold path remains consistent with STM32 cache logic.

2. Keep `load file fast = off` semantics by ensuring `.prf` uses the same voice-cache/unhold workflow as `.all`.

3. Enforce default ON when field is absent:
- initialize `PAR_FILE_LOAD_FAST = 1` in startup defaults;
- when `.cfg`/`.all` field is missing (EOF/filler `0xFF`), set this param to `1` specifically.

4. Fix the STM32 velocity-cache flag typo (`midi_midiVeloCacheAvailable`).

5. Fix `preset_readGlobalData()` uninitialized `bytesRead` usage to make missing-field defaults deterministic.
