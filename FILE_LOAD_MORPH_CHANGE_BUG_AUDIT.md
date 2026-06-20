# FILE_LOAD_MORPH_CHANGE_BUG_AUDIT.md

## Implementation Status

Implemented the STM-side fix after re-checking the AVR enum layout:

- `FRONT_SEQ_FILE_BEGIN` no longer calls `seq_resetVoiceMorphAmountsToGlobal()` for any file type.
- `seq_resetLiveMorphApplyCache()` is now scoped to `.ALL/.PRF` only, where parameter endpoint bytes are loaded and need to be re-applied by the morph worker.
- `.PAT` file begin now opens the file-load ingress bracket without touching voice morph amounts or morph live-apply cache state.
- No AVR change was needed: `PAR_MORPH` and `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` are before `PAR_BEGINNING_OF_GLOBALS`, so `menu_sendAllGlobals()` does not send them during `.ALL/.PRF` loads.

Verification run after the code change:

- `make -C front/LxrAvr avr -j4` passed; no AVR rebuild was needed.
- `make -C mainboard/LxrStm32 -j4 stm32` passed and rebuilt `frontPanelReceivingProtocol.c`.
- `make firmware` passed and rebuilt `firmware image/FIRMWARE.BIN`.

## Symptom

Repro described by user:

1. Load a `.ALL` file.
2. Change global morph.
3. Change one or more individual voice morph values.
4. Load a `.PAT` file.
5. The sound changes, apparently because morph or individual voice morph is reset.

Expected behavior:

- No file load should resend or overwrite global morph.
- No file load should resend or overwrite individual voice morph.
- `.PAT` load must never alter voice parameters at all, so the audible voice properties should remain unchanged while the pattern data loads.

## Primary Finding

The `.PAT` load path sends the normal file envelope:

- `front/LxrAvr/Preset/presetManager.c:2011`
  sends `SEQ_CC / SEQ_FILE_BEGIN / WTYPE_PATTERN`.

The STM receives that in the generic file-begin case:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:2619`
  enters `FRONT_SEQ_FILE_BEGIN`.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:2621`
  unconditionally calls `seq_resetVoiceMorphAmountsToGlobal()`.
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:2622`
  unconditionally calls `seq_resetLiveMorphApplyCache()`.

That first call is enough to explain the symptom. It does not merely suppress a pending morph event; it actively rewrites all individual voice morph values to the current global morph amount:

- `mainboard/LxrStm32/src/Preset/MorphEngine.c:409`
  `preset_resetVoiceMorphAmountsToGlobal()`.
- `mainboard/LxrStm32/src/Preset/MorphEngine.c:416`
  writes each `voiceMorphBaseAmount[synthVoice] = kit->globalMorphAmount`.
- `mainboard/LxrStm32/src/Preset/MorphEngine.c:417`
  writes each `voiceMorphAmount[synthVoice] = kit->globalMorphAmount`.
- `mainboard/LxrStm32/src/Preset/MorphEngine.c:420`
  syncs the legacy live mirrors from that overwritten state.

So if the user sets global morph, then sets Drum 2 voice morph to a different value, then loads `.PAT`, `FRONT_SEQ_FILE_BEGIN` resets Drum 2 voice morph back to global before any pattern payload matters. That is exactly the audible change described.

## `.PAT` Payload Check

The actual `.PAT` sysex receive handlers do not appear to write parameter or morph storage:

- `SYSEX_RECEIVE_MAIN_STEP_DATA` writes `pat_patternSet.pat_mainSteps` only.
- `SYSEX_RECEIVE_PAT_CHAIN_DATA` writes `pat_patternSet.pat_patternSettings` only.
- `SYSEX_RECEIVE_PAT_LEN_DATA` writes `pat_patternSet.pat_patternLengthRotate[].length` only.
- `SYSEX_RECEIVE_PAT_SCALE_DATA` writes `pat_patternSet.pat_patternLengthRotate[].scale` only.
- `SYSEX_RECEIVE_STEP_DATA` writes `pat_patternSet.pat_subStepPattern` only.

Relevant locations:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:1385-1406`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:1409-1430`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:1432-1450`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:1454-1488`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:1494-1544`

This supports the conclusion that the `.PAT` morph change is caused by generic file-envelope handling, not by the pattern data body.

## Broader File-Load Risk

The same generic `FRONT_SEQ_FILE_BEGIN` handler runs for `.ALL`, `.PRF`, `.PAT`, and `.SND` style file envelopes. It currently resets individual voice morph for every file type before the file type is examined.

The code only narrows `preset_morphLoadDisabled` to `.PRF/.ALL`:

- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:2623-2628`

But the destructive voice-morph reset has already happened at lines 2621-2622.

For `.ALL/.PRF`, there is another separate behavior to review later: AVR intentionally dumps loaded kit/morph endpoint bytes to STM normal storage after loading:

- `.ALL`: `front/LxrAvr/Preset/presetManager.c:2448`
- `.PRF`: `front/LxrAvr/Preset/presetManager.c:2679`

Those endpoint dumps are parameter-bearing by design, so they may be correct for normal storage refresh. They are not the cause of the `.PAT` symptom. Still, they are relevant to the stronger rule "no file load should resend or overwrite morph/individual voice morph" if that rule is meant to include loaded endpoint morph state as well as live current morph amounts.

## `.ALL/.PRF` Parameter Refresh Without Morph Refresh

Parameter-bearing `.ALL/.PRF` loads do need a way for newly loaded endpoint
bytes to reach the morph engine and DSP, but that does not require a global
morph update or a per-voice morph refresh.

Current `.ALL/.PRF` endpoint path:

- STM file begin calls `frontParser_beginFileLoadIngress(1)`.
- `frontParser_beginFileLoadIngress()` sets the ingress target to
  normal-kit endpoint storage:
  `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:90-97`.
- AVR later sends loaded endpoint bytes through `preset_dumpEndpointsToStm()`:
  - `.ALL`: `front/LxrAvr/Preset/presetManager.c:2448`
  - `.PRF`: `front/LxrAvr/Preset/presetManager.c:2679`
- STM receives front endpoint bytes as `PRF_RESTORE_PARAM_CC/CC2` and calls
  `preset_storeParameterIngress()`:
  `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:1648-1657`.
- STM receives morph endpoint bytes as `PRF_RESTORE_MORPH_CC/CC2` and writes
  the normal morph endpoint image:
  `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c:1661-1680`.
- In normal-kit endpoint mode, `preset_storeParameterIngress()` writes
  `preset_normalKitState.kitEndpointParams[param]` and does not apply the byte
  live as a direct edit:
  `mainboard/LxrStm32/src/Preset/ParameterIngress.c:242-280`.
- The morph worker then recomputes `interpolatedParams[param]` from
  `kitEndpointParams[param]`, `morphEndpointParams[param]`, and the existing
  `voiceMorphAmount[synthVoice]`:
  `mainboard/LxrStm32/src/Preset/MorphEngine.c:669-681`.

That means the existing current morph amounts are already the correct inputs
for refreshing loaded `.ALL/.PRF` parameters. Re-sending global morph, resetting
voice morphs to global, or sending a voice-morph refresh would be the wrong
signal: it changes the morph control state instead of merely asking the engine
to recompute from new endpoints.

The signal that may still be useful for `.ALL/.PRF` is
`preset_resetLiveMorphApplyCache()` / `seq_resetLiveMorphApplyCache()`:

- `mainboard/LxrStm32/src/Preset/MorphEngine.c:198-200`
  invalidates the live-apply cache.
- This helps the morph worker re-emit freshly loaded endpoint results through
  `preset_applyLiveMorphParameterValue()` even when the cached "already applied"
  state might otherwise suppress an update.

So the safer split is:

- `.PAT`: no morph amount reset and no live morph apply-cache reset.
- `.ALL/.PRF`: keep or move a live-apply cache invalidation if testing still
  needs loaded parameter endpoints to be forced through, but do not reset global
  morph or individual voice morph amounts.

## AVR Global Morph Send During `.ALL/.PRF`

Re-checking the enum layout shows the initially suspected AVR global-send
hazard is not active.

Both `.ALL` and `.PRF` load paths call `menu_sendAllGlobals()` after reading
global parameters from the file:

- `.ALL`: `front/LxrAvr/Preset/presetManager.c:2382-2383`
- `.PRF`: `front/LxrAvr/Preset/presetManager.c:2616-2617`

`menu_sendAllGlobals()` iterates from `PAR_BEGINNING_OF_GLOBALS` to `NUM_PARAMS`
and calls `menu_parseGlobalParam(i, parameter_values[i])` for each:

- `front/LxrAvr/Menu/menu.c:3418-3427`

However, `PAR_MORPH` and the individual voice morph controls are not in that
range:

- `PAR_MORPH_DRUM1..PAR_MORPH_HIHAT` are around
  `front/LxrAvr/Parameters.h:305-310`.
- `PAR_MORPH` is around `front/LxrAvr/Parameters.h:332`.
- `PAR_BEGINNING_OF_GLOBALS` is around `front/LxrAvr/Parameters.h:382`.

So `menu_sendAllGlobals()` does not send `PAR_MORPH`, and no AVR-side skip is
needed for the current code. `menu_parseGlobalParam()` still handles live
`PAR_MORPH` edits by sending `SEQ_SET_GLOBAL_MORPH_LSB/MSB`, and that remains
correct for actual user edits:

- `front/LxrAvr/Menu/menu.c:3564-3574`

## Likely Fix Direction

The immediate `.PAT` fix should be in the STM `FRONT_SEQ_FILE_BEGIN` case:

- Do not call `seq_resetVoiceMorphAmountsToGlobal()` for `.PAT`.
- More likely, do not call `seq_resetVoiceMorphAmountsToGlobal()` for any file load at all. File load should not change current global or individual voice morph.
- Do not use file-load begin as a place to resend or rebuild morph state.

The live-apply cache reset needs separate judgment:

- `seq_resetLiveMorphApplyCache()` does not directly overwrite morph amounts.
- It can make later morph interpolation/application treat values as not-yet-applied.
- For `.PAT`, it should almost certainly not run, because `.PAT` has no parameter endpoint payload and should produce no audible parameter changes.
- For `.ALL/.PRF`, it may have been added historically to force loaded parameter endpoints through after file data arrives. If retained for parameter-bearing loads, it should be explicitly scoped to those loads and must not be paired with resetting current morph amounts or sending global/voice morph updates.

Implemented shape:

```c
case FRONT_SEQ_FILE_BEGIN:
   frontParser_beginFileLoadIngress(1);
   if((frontParser_command.data2 == FRONT_FILE_DONE_TYPE_PERFORMANCE)
      || (frontParser_command.data2 == FRONT_FILE_DONE_TYPE_ALL))
   {
      seq_resetLiveMorphApplyCache();
      preset_morphLoadDisabled = 1;
      preset_vMorphFlag = 0;
   }
   break;
```

This keeps the `.ALL/.PRF` parameter-refresh signal as cache invalidation while
removing the morph amount mutation. It also leaves `.PAT` with no morph-related
file-begin side effect.

## Guardrails For The Fix

- `.PAT` load must not call `seq_resetVoiceMorphAmountsToGlobal()`.
- `.PAT` load must not call `seq_resetLiveMorphApplyCache()`.
- `.PAT` load must not send global morph report traffic or `VOICE_MORPH` report traffic.
- `.PAT` load must not route parameter ingress to current/live parameter state.
- `.ALL/.PRF` parameter refresh should use endpoint writes plus morph-worker/cache invalidation, not global morph resend or voice-morph reset.
- `.ALL/.PRF` currently do not send `PAR_MORPH` through `menu_sendAllGlobals()` because morph controls are outside the global parameter enum range.
- File-load begin/done may still bracket pattern receive and quiet UI, but the bracket must not mutate morph/current voice parameters.

## Confidence

High for the `.PAT` symptom. The sequence is direct:

```text
AVR .PAT load
-> SEQ_FILE_BEGIN / WTYPE_PATTERN
-> STM FRONT_SEQ_FILE_BEGIN
-> unconditional seq_resetVoiceMorphAmountsToGlobal()
-> every individual voice morph is overwritten with global morph
-> audible change
```

The `.PAT` payload itself looks clean with respect to voice parameters and morph.
