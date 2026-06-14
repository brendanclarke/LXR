# TEMPORARY / PATTERN / PARAMETER LOAD SPEC

Date: 2026-06-14
Status: current storage and switching spec after Session 020 finalization. `PresetLoadCache` and the active `presetLoad_*` cache API are gone; file loads route directly to normal Preset/Pattern storage; normal/temp Preset and Pattern switching remains the only supported staging model.

## Purpose

This document defines how the firmware should think about:

- normal versus temp parameter storage;
- normal versus temp pattern storage;
- background-load routing;
- copy-to-temp;
- normal/temp boundary switching;
- the `.pat` exception that must never move parameter read/write away from normal storage.

The goal is to keep one clear model for what is stored where and which operation is allowed to touch it.

## Ownership Model

### Parameter storage

`mainboard/LxrStm32/src/Preset/KitState.c/.h` owns the runtime parameter images:

- `preset_normalKitState`
- `preset_tmpKitState`

Each kit image carries:

- kit/front endpoint bytes;
- morph endpoint bytes;
- interpolated worker-owned bytes;
- resolved automation target images;
- global morph amount and per-voice morph values.

The exposed type names are now `PresetKitState` and `PresetAutomationTargets`.
The storage behavior did not change during the rename sweep; only the public
names did.

### Pattern storage

`mainboard/LxrStm32/src/Sequencer/Pattern/` owns pattern storage:

- `seq_patternSet`
- `seq_tmpPattern`

Pattern storage is separate from parameter storage.
Copying one does not automatically mean the other changed, unless the caller explicitly says so.

### Source selection

`mainboard/LxrStm32/src/Preset/TempPlaybackSwitch.c/.h` owns the persistent runtime decision about which image is audible.

The important state is:

- `preset_tmpKitActive`
- `preset_voiceSourceState[]`
- the boundary-switch flags that tell the sequencer when a source change is pending or committed

That module answers the question, "which image is active?"
It does not own a second copy of the storage model itself.

### File-load ingress

`mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c`
receives the old AVR file-transfer envelope and routes file bytes to the real
storage owners.

Session 020 removed the recreated
`mainboard/LxrStm32/src/Preset/PresetLoadCache.c/.h` files and the active
`presetLoad_*` cache API. The receive protocol now keeps only a tiny
file-load ingress bracket that forces parameter ingress to normal-kit endpoint
mode while the file envelope is active. That bracket is not a cache and must not
grow into one.

The long-term plan is still for file-load-while-temp-active behavior to use the
existing temp parameter/pattern copy and playback mechanisms, not a separate
load-cache authority.

## Core Rules

### 1. Copy-to-temp clones normal into temp

When the user explicitly asks for copy-to-temp:

- copy the current normal pattern into `seq_tmpPattern`;
- copy the current normal kit image into `preset_tmpKitState`;
- copy endpoint bytes, interpolated bytes, automation targets, and morph values as part of that image copy;
- mark the temp image valid only after the full copy is complete.

Copy-to-temp is a storage operation, not a transport operation.

### 2. Temp switching changes source only

When the user switches between normal and temp playback:

- do not recopy storage;
- do not re-run file loading;
- do not use the switch itself to mutate endpoint arrays;
- only change which already-existing image is considered active.

That distinction matters because the active image and the stored image are not the same thing.

### 3. Background file loads write normal storage

`.prf` / `.all` style loads refresh the normal storage images while temp playback can keep running.

The current target rule is:

- load normal parameter storage;
- load normal pattern storage;
- keep temp playback isolated unless the user explicitly requests a temp switch or copy-to-temp;
- never create a second sound-authority cache just because a file is being loaded.
- never stage through `PresetLoadCache` or a replacement cache.

Future file-load-while-temp-active behavior should use the existing
normal/temp copy/playback model, not a revived cache.

Implementation reminder:

- `.pat` is the special case that should never switch parameter read/write away
  from normal storage;
- the temp/background storage rules are about where bytes land, not about
  changing which module owns the storage model;
- `TempPlaybackSwitch` still owns the audible-image selection state.

### 4. Pattern and parameter temp ownership stay separate

Pattern background loading and parameter background loading are distinct ownership domains.

That means:

- a pattern load may stage pattern data into pattern temp structures;
- parameter ingress should remain normal unless the file kind explicitly requires a parameter-bearing load path;
- the pattern temp path must not be used as an excuse to redirect parameter writes;
- the parameter temp path must not be used as an excuse to steal pattern ownership.

This is the rule that prevents the `.pat` wrinkle from accidentally turning into a general parameter switch.

### 5. `.pat` background loading never switches parameter read/write away from normal storage

This is the special case that must stay true even if the interface changes later.

For `.pat` background loads:

- pattern data may be staged in pattern-owned temp structures;
- parameter ingress must remain on normal storage;
- no parameter temp switch should be implied by pattern background loading;
- if the loader needs to distinguish this case, it should use an explicit load-kind discriminator or mode bit.

This rule is easy to violate if the code only knows "background load" and does not know "pattern-only background load."

## Current Flow Shape

### Copy-to-temp

The intended current flow is:

1. User requests temp capture.
2. `Preset` copies normal pattern and parameter images into temp storage.
3. Temp image is marked valid.
4. The active source can then switch to temp when the boundary logic says so.

### Normal/temp boundary switch

The intended boundary switch is:

1. Sequencer/pattern logic decides the next pattern source.
2. `TempPlaybackSwitch` updates the active source selection.
3. `Preset` uses that source selection to choose the correct image for live routing.
4. Endpoint restore traffic, if required, keeps the AVR menu coherent.

Switching should never be confused with copying.

### Background `.prf` / `.all` load

The intended background-load flow is:

1. File-load initiator asks for a background load.
2. The loader routes parameter ingress to normal storage.
3. Pattern data and endpoint data are written to the normal images.
4. Temp playback continues from the temp image if it was already active.
5. On completion, the current active image selection does not need a second cache promotion.

### Background `.pat` load

The intended background-load flow for `.pat` is narrower:

1. Pattern data is staged.
2. Parameter storage remains normal.
3. Temp pattern ownership stays separate from parameter ownership.
4. The load completes without switching parameter read/write away from normal storage.

This flow is why the future load API may need an explicit file-kind or load-kind discriminator.

## Current Functions

These functions matter for the current normal/temp model:

- `preset_captureTmpKitState()`
- `preset_setTmpKitActive()`
- `preset_getCurrentImageKitState()`
- `preset_getMorphKitForImage()`
- `preset_storeParameterIngress()`
- `preset_storeMorphParameterIngress()`
- `preset_storeLfoDestinationIngress()`
- `preset_storeVelocityDestinationIngress()`
- `preset_storeMacroDestinationIngress()`
- `seq_setTmpKitActive()`
- `seq_updateVoiceSourcesForPatternChange()`

`presetLoad_*` is not a current ownership surface. Do not add new callers or
recreate those APIs.

## Design Note: Why The Temp Switch Survived And The Load Cache Did Not

The reason these mechanisms felt split was simple:

- `TempPlaybackSwitch` answers "which image is active?";
- the old load cache answered "what in-flight file/session work still needs to
  be staged or finalized?"

That distinction made sense while background-load flow still had special
session machinery.

Session 020 removed the need for a separate load cache by making the
temp/parameter ownership rules carry the real state directly. The receive
protocol may bracket the old file-transfer envelope, but it must not become a
second staging owner.

## Do-Not-Do List

- Do not reintroduce per-parameter validity arrays.
- Do not use background load as a second sound-authority cache.
- Do not let `.pat` loading switch parameter read/write away from normal storage.
- Do not merge pattern staging and parameter staging into one ambiguous flag.
- Do not let temp switching recopy storage when a simple source selection change is enough.
- Do not allow file-transfer compatibility code to become a permanent second
  owner.
