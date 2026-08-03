# Global MIDI Legacy Assignments Fix Plan

## Decision

Treat the Global-channel table as the immutable legacy external MIDI contract.
Do **not** renumber its CCs, change its destinations, or alter any per-channel
MIDI CC assignment. Fix the conversion boundary between the STM's legacy
MIDI-apply identifiers and the AVR-compatible raw parameter identifiers before
writing Preset state.

This is a real firmware defect, not a 60knobs configuration error.

## Root Cause

There are two deliberately different low-parameter number domains:

| Domain | Owner/use | Example: hihat volume |
|---|---|---:|
| External legacy Global MIDI CC | Controller-facing assignment | CC `94` |
| MIDI apply ID | STM `MidiMessages.h` switch domain | `VOL6 = 94` |
| Raw parameter ID | AVR menu/file/pattern/Preset domain | `PAR_VOL6 = 93` |

The raw parameter array was made canonical during the AVR/STM parameter split.
For low parameters, its ID is normally one less than the old STM MIDI apply
ID. This is intentional: `MIDI_CC` commands from the AVR carry a raw ID, then
`frontParser_handleMidiMessage()` adds one only for live DSP application.

`GlobalMidiParser.c` currently has the right legacy CC table, but its entries
are MIDI apply IDs. `globalMidiParser_applyInternalParameter()` forwards an
entry to `frontParser_applyParameterCommand()` with `updateOriginalValue = 1`.
That function correctly drives the DSP switch, but also sends the unconverted
MIDI ID to `preset_storeParameterIngress()`. Preset storage treats that value
as raw.

The resulting stored target is shifted by one. The examples match the report:

| Incoming Global CC | Intended MIDI apply ID | Wrong raw storage slot today | Observed result |
|---:|---:|---:|---|
| 94, hihat volume | `VOL6` = 94 | raw 94 = `PAR_PAN1` | appears to control Drum 1 pan / the next legacy MIDI assignment |
| 43, hihat filter frequency | `HAT_FILTER_F` = 43 | raw 43 = `PAR_RESO_1` | parameter state no longer belongs to hihat filter frequency |
| 74, snare mod-envelope decay | `PITCHD4` = 74 | raw 74 = `PAR_MODAMNT1` | changes Drum 1 modulation amount in stored/morphed state |

The defect is particularly visible when the stored image is subsequently
applied by the morph worker, a preset refresh/load, a step-automation release,
or an AVR echo/UI refresh. Direct DSP application can briefly appear correct,
which makes the problem look intermittent.

## Required Invariants

- Preserve every external Global CC number and destination in
  `globalMidiParser_ccToLxrParam` (renamed for clarity below).
- Preserve all `ChannelMidiParser.c` CC-number-to-voice-destination mappings.
- Keep Global CC0 bank change and CC1 global morph on their existing special
  routing path.
- Keep Global-channel pre-emption: Global CC2-127 must not also run through
  the per-channel parser.
- Keep raw IDs in AVR menu/file traffic, Preset endpoint/interpolated arrays,
  and step-automation storage.
- Keep MIDI apply IDs only at the live `frontParser_applyParameterCommand()`
  switch and at the existing MIDI-domain echo/record compatibility edges.

## Implementation Plan

1. Introduce named conversion helpers at the STM MIDI/Preset boundary.

   Add a small, documented helper pair in a shared STM-only header/source (or
   a narrowly scoped helper in `GlobalMidiParser.c` if reuse is not yet needed):

   - `rawParam -> midiApplyParam`: low raw IDs use `raw + 1`; high raw IDs
     remain in the existing `128 + CC2` domain.
   - `midiApplyParam -> rawParam`: low MIDI apply IDs use `id - 1`; high IDs
     remain unchanged.

   The helpers must validate `END_OF_SOUND_PARAMETERS`, reject the sentinel,
   and make the low/high boundary explicit. Do not encode `+1` / `-1` in
   table initializers or at individual call sites.

2. Make the Global CC and NRPN lookup tables raw-domain tables.

   In `GlobalMidiParser.c`, include `Preset/ParameterArray.h` and replace
   MIDI enum targets such as `VOL6`, `PITCHD4`, and `HAT_FILTER_F` with their
   raw `PAR_*` counterparts (`PAR_VOL6`, `PAR_MOD_EG4`,
   `PAR_FILTER_FREQ_6`, etc.). Rename the tables and local variable from
   `*LxrParam` to `*RawParam` so their domain cannot be mistaken again.

   Preserve each external CC index exactly. The change is only the table value
   representation. Apply the same raw-domain rule to the Global NRPN target
   table; its current `128 + CC2_*` entries already represent the canonical
   high/raw parameter space, so it should be mechanically verified rather
   than renumbered.

3. Replace the Global helper's mixed-domain write path.

   Refactor `globalMidiParser_applyInternalParameter()` into a raw-ingress
   helper that accepts a validated raw parameter ID and performs these actions
   in this order:

   1. When `updateOriginalValue` is set, store `rawParam` through
      `preset_storeParameterIngress(rawParam, value)`; otherwise preserve the
      existing DSP-only/no-baseline-write behavior.
   2. Build the MIDI-shaped message only for live DSP application, using the
      explicit raw-to-MIDI-apply helper, and call
      `frontParser_applyParameterCommand(..., 0)`.
   3. Update the legacy original-value cache and AVR echo in their existing
      MIDI/transport representation. For low targets this is the converted
      MIDI apply ID, so the existing echo's `paramNr - 1` still emits the
      original AVR/raw parameter byte.
   4. When recording, retain `seq_recordAutomationMidiDestination()` with the
      converted MIDI apply ID, so its existing conversion stores a raw step
      destination byte.

   Passing `0` to `frontParser_applyParameterCommand()` is important: it
   prevents its legacy `updateOriginalValue` storage branch from storing the
   MIDI ID as though it were raw. This keeps the direct DSP switch unchanged
   while giving Preset exactly one correctly typed write.

4. Add regression coverage for the boundary, not a new controller mapping.

   A host-side/unit-style test or a compile-time test table should exercise:

   - raw-to-MIDI-to-raw round trips for representative low parameters,
     including raw 1, `PAR_FILTER_FREQ_6`, `PAR_MOD_EG4`, `PAR_VOL6`, and
     `PAR_PAN1`;
   - representative high/NRPN targets without a low-ID offset;
   - every defined Global CC table entry resolving to a raw ID below
     `END_OF_SOUND_PARAMETERS`;
   - unmapped CCs, CC6, CC98, and CC99 retaining their current NRPN behavior.

   Add a focused integration seam if practical: capture the raw ID supplied
   to `preset_storeParameterIngress()` and the MIDI apply ID supplied to
   `frontParser_applyParameterCommand()` for Global CC94. Expected values are
   raw `PAR_VOL6` (93) and MIDI apply `VOL6` (94), respectively.

5. Re-audit, but do not rewrite, the per-channel parser.

   Confirm each `ChannelMidiParser.c` mapping remains byte-for-byte identical
   in CC number and direct DSP destination. This task does not change its
   public mapping. Record its currently mixed storage calls as a separate
   follow-up only if testing shows the same persistence issue there; do not
   bundle a broad per-channel rewrite with this compatibility repair.

6. Update the durable MIDI documentation.

   Amend `knowledge_files/comms_spec_reference/MIDI_TABLE.md` with a brief
   “parameter domains” note and retain the published Global CC table unchanged.
   State that Global lookup targets are stored as raw `PAR_*` IDs internally,
   while their live DSP messages use the legacy MIDI apply ID. Do not change
   any user-facing CC number in the table.

## Verification

1. Build STM firmware: `make -C mainboard/LxrStm32 -j4 stm32`.
2. Build the full image: `make firmware`.
3. On the configured Global MIDI channel, sweep these CCs while observing the
   front-panel parameter and sound, then reload/morph/step-release to verify
   that stored state stays correct:

   - CC43 hihat filter frequency;
   - CC62 open-hihat decay;
   - CC74 snare mod-envelope decay;
   - CC78 snare mod amount;
   - CC94 hihat volume;
   - CC95 Drum 1 pan.

4. Verify Global CC0/CC1 retain bank/morph behavior, CC98/99/6 retain NRPN
   behavior, and an overlapping per-channel assignment is still pre-empted by
   the Global channel.
5. Smoke-test representative non-global/per-channel controls (volume, hihat
   decay, snare modulation, filter) to confirm that their mappings were not
   altered by this change.

## Files Expected To Change

- `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c`
- `knowledge_files/comms_spec_reference/MIDI_TABLE.md`

No `.h` file is expected to change: the conversion helpers are deliberately
private to `GlobalMidiParser.c`. `MidiMessages.h`, `Preset/ParameterArray.h`,
`front/LxrAvr/Parameters.h`, and the public per-channel table should not be
renumbered or otherwise changed for this fix.

## Deep-Dive Implementation Specification

This section replaces the initial plan's optional helper-file choice with the
smallest safe implementation scope.

### Final Code Scope

| File | Change? | Reason |
|---|---|---|
| `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c` | Yes | Owns the Global lookup tables and is the caller that gives a MIDI apply ID to raw Preset storage. |
| `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.h` | No | The repair needs only private `static` helpers and tables; its public router API is unchanged. |
| `mainboard/LxrStm32/src/MIDI/MidiMessages.h` | No | Owns legacy live-DSP apply IDs. Renumbering it would alter channel MIDI. |
| `mainboard/LxrStm32/src/Preset/ParameterArray.h` | No | Owns canonical raw `PAR_*` IDs and AVR/Preset ABI. |
| `mainboard/LxrStm32/src/Preset/ParameterIngress.c/.h` | No | Its raw-only input contract is correct; the Global caller violates it. |
| `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.c/.h` | No | Already converts raw AVR low IDs to DSP IDs correctly. The fix calls its DSP switch with storage disabled. |
| `mainboard/LxrStm32/src/MIDI/ChannelMidiParser.c/.h` | No | Per-channel CC assignments and direct behavior are explicitly preserved. |
| `mainboard/LxrStm32/src/MIDI/MidiParser.c/.h` | No | Global pre-emption plus CC0/CC1 special routing are already correct. |
| `knowledge_files/comms_spec_reference/MIDI_TABLE.md` | Yes | Add the internal domain rule; leave every public MIDI row unchanged. |

No new C or header file is needed. Keeping the conversion private to
`GlobalMidiParser.c` avoids turning a Global compatibility repair into a new
shared API before the separately scoped per-channel persistence audit.

### Verified Existing Flow

```text
Global CC94 -> global lookup returns MIDI VOL6 (94)
            -> frontParser_applyParameterCommand(MIDI_CC, 94, update=1)
               -> DSP switch case VOL6: correct
               -> preset_storeParameterIngress(94): incorrect raw PAR_PAN1
            -> later morph/restore/release uses raw slot 94 and controls pan
```

The repaired flow must be:

```text
Global CC94 -> global lookup returns raw PAR_VOL6 (93)
            -> preset_storeParameterIngress(93): correct
            -> DSP message MIDI_CC/data1 VOL6 (94), update=0: same live sound
            -> MIDI-domain cache/echo retains 94, whose existing echo emits raw 93
```

### `GlobalMidiParser.c` Change Details

#### A. Make raw parameter ownership explicit

Add a direct `#include "Preset/ParameterArray.h"` adjacent to the existing
`ParameterIngress.h` include.

- **Purpose:** allow the Global tables to use canonical raw `PAR_*` values.
- **Inputs/outputs:** compile-time declarations only; no runtime behavior.
- **Affiliates:** `ParameterArray.h` remains unmodified and owns `PAR_NONE`,
  `PAR_RESERVED4`, low/high `PAR_*`, and `END_OF_SOUND_PARAMETERS`.
- **Comment text:** “Global lookup table values are raw Preset/AVR IDs;
  MidiMessages values are a separate live-apply domain.”

#### B. Make the tables raw-domain tables

Replace `GLOBAL_MIDI_UNMAPPED I_DUNNO` with
`GLOBAL_MIDI_RAW_UNMAPPED PAR_NONE`, and rename:

- `globalMidiParser_ccToLxrParam` → `globalMidiParser_ccToRawParam`;
- `globalMidiParser_nrpnToLxrParam` → `globalMidiParser_nrpnToRawParam`.

Change table values only—never table indices. Each Global CC index remains the
published legacy controller CC. Convert values mechanically to the matching
raw `PAR_*` token. Representative required conversions are:

| CC | Old MIDI apply table value | New raw table value |
|---:|---|---|
| 43 | `HAT_FILTER_F` | `PAR_FILTER_FREQ_6` |
| 62 | `VELOD6_OPEN` | `PAR_VELOD6_OPEN` |
| 74 | `PITCHD4` | `PAR_MOD_EG4` |
| 94 | `VOL6` | `PAR_VOL6` |
| 95 | `PAN1` | `PAR_PAN1` |

The full replacement uses the corresponding raw families: oscillator
wave/tuning (`PAR_OSC_*`, `PAR_COARSE*`, `PAR_FINE*`), modulation waves and
amounts, filter/resonance, envelope fields, FM fields, volume/pan,
distortion/decimation, and LFO frequency/amount.

- **Purpose:** table results become directly legal inputs to
  `preset_storeParameterIngress()`.
- **Inputs:** CC index `0..127`, or an NRPN selection `0..92`.
- **Outputs:** raw ID in `[1, END_OF_SOUND_PARAMETERS)`, or the raw sentinel.
- **Affiliates:** only `globalMidiParser_MIDIccHandler()` and
  `globalMidiParser_handleNrpnControl()` consume these private tables.
- **Comment text:** “Indices are external legacy CC/NRPN numbers; values are
  raw `PAR_*` storage IDs. Do not insert low `MidiMessages.h` constants.”

For the high NRPN table, verify the existing `128 + CC2_*` values equal their
matching high `PAR_*` values, then rename the table. They may be rewritten as
`PAR_*` names for readability, but must not be offset: high raw parameters
start at 128 and do not have the low-ID split.

#### C. Add one private conversion helper

Add a `static uint16_t` helper, for example
`globalMidiParser_midiApplyParamFromRaw(uint16_t rawParam)`.

- **Input:** a raw `PAR_*` value from a private Global lookup table.
- **Output:** the ID accepted by `frontParser_applyParameterCommand()`, or
  `GLOBAL_MIDI_RAW_UNMAPPED` for a sentinel, reserved, gap, or out-of-range
  value.
- **Rules:** reject `PAR_NONE` and `PAR_RESERVED4`; for
  `rawParam < PAR_RESERVED4`, return `rawParam + 1`; for
  `rawParam >= PAR_FILTER_DRIVE_1`, return `rawParam` unchanged; reject any
  value not below `END_OF_SOUND_PARAMETERS`.
- **Purpose:** create exactly one visible low-ID conversion point. Concrete
  invariant: `PAR_VOL6` (93) converts to `VOL6` (94).
- **Affiliates:** `frontParser_handleMidiMessage()` already does the analogous
  raw-AVR-to-live conversion. This helper must stay private and must not be
  used for raw restore packets.
- **Comment text:** “Converts raw Global lookup targets only for the live DSP
  switch; endpoint/Preset/AVR raw traffic must not use this conversion.”

#### D. Replace the mixed-domain apply helper

Rename `globalMidiParser_applyInternalParameter()` to
`globalMidiParser_applyRawParameter()` and change its contract to raw input.
Its ordered behavior must be:

1. Validate/convert `rawParam` through the helper above; return with no side
   effect for unmapped, reserved, or invalid targets.
2. If `updateOriginalValue` is set, call
   `preset_storeParameterIngress(rawParam, value)` exactly once. This is the
   authoritative Preset write. If it is clear, preserve the existing DSP-only
   semantics and do not mutate the stored baseline.
3. Build `MidiMsg liveMsg`: low parameters use `MIDI_CC` and converted data1;
   high parameters use `MIDI_CC2` and data1 `rawParam - 128`; copy `value`,
   `source`, length 2, and clear the SysEx flag.
4. Call `frontParser_applyParameterCommand(liveMsg, 0)`. Zero is essential:
   the existing `updateOriginalValue=1` branch would store the MIDI apply ID
   again, recreating the defect.
5. If `updateOriginalValue` is set, preserve cache, automation, and echo in
   their existing MIDI/transport representation: low cache/echo ID is
   `rawParam + 1`, high cache/echo ID is raw/high; pass that MIDI ID to
   `seq_recordAutomationMidiDestination()` and
   `channelMidiParser_sendParameterEcho()`.

- **Inputs:** raw target, 7-bit value, update flag, DIN/USB source.
- **Outputs:** when `updateOriginalValue` is set, correct endpoint/interpolated
  Preset state plus optional original cache, recorded raw step destination,
  and AVR UI echo; when clear, the same DSP-only result as current firmware.
- **Side effects:** writes the active normal/temp kit selected by Preset,
  invokes the existing DSP switch, and may record automation or send UI data.
- **Affiliates:** `preset_storeParameterIngress()`,
  `frontParser_applyParameterCommand()`,
  `seq_recordAutomationMidiDestination()`,
  `channelMidiParser_sendParameterEcho()`, and
  `frontPanelSending_sendParameterEcho()`.
- **Comment text:** “Store raw Preset state first, then apply the converted
  MIDI-shaped DSP message. Do not merge these calls or change the zero flag.”

#### E. Update only the two private callers

`globalMidiParser_handleNrpnControl()` passes the selected
`globalMidiParser_nrpnToRawParam[]` value to the new helper.
`globalMidiParser_MIDIccHandler()` loads
`globalMidiParser_ccToRawParam[msg.data1 & 0x7f]` and passes it to the helper.

- **Purpose:** complete the typed path without translating an external CC.
- **Inputs/outputs:** exact same MIDI bytes, NRPN selector semantics, and
  routing results as current firmware.
- **Affiliates:** `MidiParser.c` still routes Global CC0/CC1 to
  `ChannelMidiParser.c` and Global CC2-127 here; that logic is not edited.
- **Comment text:** “Lookup index is external legacy CC; lookup result is raw
  Preset parameter ID.”

### Header Change Specification

There are deliberately **no `.h` changes**. All proposed symbols are `static`
to `GlobalMidiParser.c`; no caller needs a new declaration, protocol packet,
or data-layout change. The unchanged header contracts are part of the fix:

- `ParameterIngress.h`: `preset_storeParameterIngress(param, value)` accepts
  raw parameter IDs only.
- `frontPanelReceivingProtocol.h`: `frontParser_applyParameterCommand()`
  accepts a MIDI-shaped live command; zero update means DSP-only application.
- `ChannelMidiParser.h`: parameter echo remains MIDI apply domain because its
  send implementation subtracts one for low AVR packets.
- `GlobalMidiParser.h`: remains a public Global routing/system API only.

Do not add shared conversion macros. They would obscure validation and make it
too easy to apply the low offset to raw restore or high-parameter traffic.

### Documentation Change: `MIDI_TABLE.md`

Add a short internal note after Global routing rules: Global CC numbers are the
fixed legacy external contract; the implementation lookup values are raw
`PAR_*` IDs; only the live DSP switch translates a low raw ID to its old MIDI
apply ID. Raw Preset, AVR parameter, and pattern-automation IDs are never
incremented. Do not modify any published CC or NRPN row.

### Implementation Review Checklist

- No low `MidiMessages.h` token remains as a Global CC table value.
- With `updateOriginalValue` set, the new raw apply helper calls
  `preset_storeParameterIngress()` once; with it clear, it makes no storage
  write. It calls `frontParser_applyParameterCommand(..., 0)` once per valid
  target in both cases.
- CC94 stores `PAR_VOL6` and applies live `VOL6`; CC74 stores `PAR_MOD_EG4`
  and applies live `PITCHD4`.
- High NRPN targets remain raw and are applied as CC2 data1 `raw - 128`.
- `git diff` contains no `.h` changes, no `ChannelMidiParser` changes, and no
  user-facing MIDI table-row changes.

## Implementation Log

### 2026-08-03 — Implemented

- Updated `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c` only on the firmware
  side. The Global CC lookup now uses raw `PAR_*` values, and the NRPN lookup
  now uses matching high raw `PAR_*` values.
- Added private, adjacent documentation and the static
  `globalMidiParser_midiApplyParamFromRaw()` conversion helper. It rejects raw
  sentinels/reserved values, converts valid low raw IDs exactly once, and
  leaves high raw IDs unchanged.
- Replaced the mixed-domain Global apply helper with the documented raw-store,
  MIDI-apply sequence. Stored baseline writes remain conditional on
  `updateOriginalValue`; DSP-only calls preserve their prior no-storage
  behavior. The live DSP call now passes zero for that flag so it cannot write
  the converted MIDI apply ID into Preset.
- Preserved existing MIDI-domain cache, automation-record, and AVR echo edges
  by passing the converted MIDI apply ID there. This retains the established
  echo encoder's low-ID `-1` conversion back to the AVR raw byte.
- Added the internal-domain explanation to
  `knowledge_files/comms_spec_reference/MIDI_TABLE.md`; no published Global
  CC/NRPN or per-channel MIDI table row changed.
- No `.h` file changed: all new firmware symbols are private `static` symbols
  in `GlobalMidiParser.c`, so adding a public declaration would be misleading.

### Verification Status

- Completed source-level checks: all raw `PAR_*` tokens used by the two Global
  lookup tables resolve in `Preset/ParameterArray.h`; the retired mixed-domain
  Global table/helper names no longer occur; the reported CC43, CC62, CC74,
  CC94, and CC95 table rows resolve to their intended raw targets; and
  `git diff --check` is clean.
- No build was attempted because this environment does not provide the project
  toolchain. Firmware build and hardware verification remain follow-up steps
  in an environment with the STM32 toolchain and the target device.
