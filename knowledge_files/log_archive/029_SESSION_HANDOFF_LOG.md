# 029 Session Handoff Log - Global MIDI CC/NRPN Table Implementation

DATE: 2026-06-21

## Session Goal

Implement a separate, alternative MIDI CC / MIDI NRPN parser on the configured Global MIDI channel while leaving the current non-global Channel MIDI implementation in place.

Required constraints:

- Keep the current Global-channel CC0 bank-change behavior exactly as-is.
- Keep the current Global-channel CC1 mod-wheel/global-morph behavior exactly as-is.
- Add the new Global-channel CC2-127 and NRPN table almost entirely in `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c`.
- Preserve the current Channel MIDI implementation for non-global voice/track channels.
- Preserve all future-relevant details from temporary root docs:
  - `GLOBAL_MIDI_AUDIT.md`
  - `GLOBAL_MIDI_TABLE_REQUEST.md`
  - `GLOBAL_MIDI_TABLE_TO_CREATE.md`

## Completed

Session 029 implemented the new Global MIDI table and promoted the MIDI spec into durable documentation.

Code changes:

- `mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c`
  - Added private Global NRPN selector state:
    - `globalMidiParser_activeNrpnNumber`
    - `globalMidiParser_nrpnSelected`
  - Added `globalMidiParser_ccToLxrParam[128]` for the requested Global CC table.
  - Added `globalMidiParser_nrpnToLxrParam[]` for requested Global NRPN0-92.
  - Added a Global-channel predicate so the parser ignores non-global CCs.
  - Added an internal apply helper that translates Global CC/NRPN targets into existing internal `MIDI_CC` / `MIDI_CC2` messages and calls `frontParser_applyParameterCommand()`.
  - Added Global NRPN CC98/CC99/CC6 handling.
  - Removed the old sparse Global-channel side-effect ladder for CC2-127, including old roll/pattern/loop/mute/reset uses that conflicted with the requested Global table.
- `mainboard/LxrStm32/src/MIDI/MidiParser.c`
  - Changed `midiParser_MIDIccHandler()` routing so Global-channel CC messages pre-empt overlapping voice-channel assignments.
  - Global CC0 and CC1 still route through `ChannelMidiParser.c`.
  - Global CC2-127 route only through `GlobalMidiParser.c`.
  - Non-global CC messages route through `ChannelMidiParser.c` exactly as before.
- `knowledge_files/comms_spec_reference/MIDI_TABLE.md`
  - Added the complete current external MIDI input spec:
    - ownership/routing rules;
    - system message behavior;
    - note routing;
    - program-change routing;
    - complete channel MIDI CC matrix;
    - complete Global MIDI CC table;
    - complete Global MIDI NRPN table.
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
  - Updated the MIDI boundary with a pointer to `MIDI_TABLE.md`.
  - Documented the Session 029 Global-channel CC pre-emption rule.
- `knowledge_files/log_archive/000_SESSION_INDEX.md`
  - Added Session 029 terse entry, summary, and cross-session facts.
  - Also added the missing quick-reference row for Session 028.
- `MEMORY.md`
  - Updated after implementation to make Session 029 current project context.

## Verified

Build:

```bash
make -C mainboard/LxrStm32 -j4 stm32
```

Result:

- Passed.
- The final rebuild emitted only the existing linker warning that `build//LxrStm32.elf` has a LOAD segment with RWX permissions.

Whitespace:

```bash
git diff --check -- mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c mainboard/LxrStm32/src/MIDI/MidiParser.c GLOBAL_MIDI_AUDIT.md
```

Result:

- Passed.

Hardware/user verification:

- User reported the new Global implementation "seems pretty good" and "seems to test out ok".
- User also verified/observed that Global assignment pre-empts channel assignments, which is now documented as intentional for CC routing.

## MIDI Routing Details

### CC Pre-Emption Rule

For MIDI CC messages, `midiParser_MIDIccHandler()` now checks the incoming MIDI channel against the configured Global channel first.

If the incoming channel equals `midi_MidiChannels[7]`:

- CC0 goes to `ChannelMidiParser.c` to preserve existing bank-change behavior.
- CC1 goes to `ChannelMidiParser.c` to preserve existing morph behavior.
- CC2-127 go to `GlobalMidiParser.c` and do not also run through voice-channel parsing.

If the incoming channel does not equal `midi_MidiChannels[7]`:

- The existing `ChannelMidiParser.c` behavior is used.

This pre-emption rule is specific to CC messages. Note and program-change routing keep their pre-existing behavior.

### Global CC0 And CC1 Preserve-Current Behavior

Global CC0:

- Still uses the `ChannelMidiParser.c` bank-change path.
- On the Global channel, the bank mask is `0x3f`.
- It sends the front-panel bank-change packet through `channelMidiParser_sendBankChange()`.

Global CC1:

- Still uses the `ChannelMidiParser.c` morph path.
- It still respects `preset_morphLoadDisabled`.
- On the Global channel, it calls `seq_setGlobalMorphAutomationValue(msg.data2)`.
- It remains a 7-bit MIDI input path.

### Global CC2-127 Implementation

Global CC2-127 use `globalMidiParser_ccToLxrParam[128]`.

The table maps incoming CC numbers to existing LXR apply-domain parameter IDs, then calls:

```c
frontParser_applyParameterCommand(internalMsg, updateOriginalValue);
```

Low parameters use:

```c
internalMsg.status = MIDI_CC;
internalMsg.data1 = lxrParamNr;
```

High parameters use:

```c
internalMsg.status = MIDI_CC2;
internalMsg.data1 = lxrParamNr - 128;
```

This preserves the current internal CC/CC2 DSP apply behavior, Preset ingress storage, original-value cache updates, front-panel parameter echo, and external MIDI automation recording boundary.

### Global NRPN Implementation

Global NRPN is separate from front-panel NRPN state.

State:

```c
static uint16_t globalMidiParser_activeNrpnNumber = 0;
static uint8_t globalMidiParser_nrpnSelected = 0;
```

Messages:

- CC99 sets the high/coarse selector bits.
- CC98 sets the low/fine selector bits.
- CC6 applies data entry to the selected Global NRPN.

Guards:

- CC6 before CC98 or CC99 is ignored.
- Selected NRPN numbers outside `0..92` are ignored.
- CC98 and CC99 are selector messages only; they are not echoed as pan or recorded as automation.

## Complete MIDI Tables

The complete durable MIDI spec table is now:

- `knowledge_files/comms_spec_reference/MIDI_TABLE.md`

That file contains the entire current external MIDI specification, including:

- system message table;
- note routing table;
- program-change table;
- full non-global channel MIDI CC matrix;
- full Global MIDI CC0-127 table;
- full Global MIDI NRPN0-92 table.

The table was written separately because it is now the canonical lookup document for future sessions and should survive deletion of the temporary root planning files.

### Channel MIDI CC Table Summary

The channel MIDI implementation remains in `ChannelMidiParser.c`.

Important current channel properties:

- CC0 bank change and CC1 morph still use voice masks.
- Non-global CCs are applied per configured voice/track MIDI channel.
- Hihat has two channel assignments, `midi_MidiChannels[5]` and `midi_MidiChannels[6]`; both target the same hihat sound parameter set.
- Channel MIDI has no separate NRPN parser after Session 029.
- High destinations are reached directly by their channel CC numbers.
- Channel CC83 hihat LFO sync currently applies hihat LFO sync but stores/echoes `128+CC2_SYNC_LFO5` for both hihat channel assignments. This is current code behavior, not a new mapping choice.

Full channel matrix:

| CC | Function | Drum 1 | Drum 2 | Drum 3 | Snare | Cymbal | HH A | HH B |
|---:|---|---|---|---|---|---|---|---|
| 0 | Bank change | `BANK_1` | `BANK_2` | `BANK_3` | `BANK_4` | `BANK_5` | `BANK_6|BANK_7` | `BANK_6|BANK_7` |
| 1 | Morph/mod wheel | `BANK_1` | `BANK_2` | `BANK_3` | `BANK_4` | `BANK_5` | `BANK_6|BANK_7` | `BANK_6|BANK_7` |
| 7 | Voice volume | `VOL1` | `VOL2` | `VOL3` | `VOL4` | `VOL5` | `VOL6` | `VOL6` |
| 9 | Osc 1 waveform | `OSC_WAVE_DRUM1` | `OSC_WAVE_DRUM2` | `OSC_WAVE_DRUM3` | `OSC_WAVE_SNARE` | `CYM_WAVE1` | `WAVE1_HH` | `WAVE1_HH` |
| 10 | Pan | `PAN1` | `PAN2` | `PAN3` | `PAN4` | `PAN5` | `PAN6` | `PAN6` |
| 12 | Decimation | `VOICE_DECIMATION1` | `VOICE_DECIMATION2` | `VOICE_DECIMATION3` | `VOICE_DECIMATION4` | `VOICE_DECIMATION5` | `VOICE_DECIMATION6` | `VOICE_DECIMATION6` |
| 13 | Distortion | `OSC1_DIST` | `OSC2_DIST` | `OSC3_DIST` | `SNARE_DISTORTION` | `CYMBAL_DISTORTION` | `HAT_DISTORTION` | `HAT_DISTORTION` |
| 14 | Osc 1 coarse tune | `F_OSC1_COARSE` | `F_OSC2_COARSE` | `F_OSC3_COARSE` | `F_OSC4_COARSE` | `F_OSC5_COARSE` | `F_OSC6_COARSE` | `F_OSC6_COARSE` |
| 16 | Filter frequency | `FILTER_FREQ_DRUM1` | `FILTER_FREQ_DRUM2` | `FILTER_FREQ_DRUM3` | `SNARE_FILTER_F` | `CYM_FIL_FREQ` | `HAT_FILTER_F` | `HAT_FILTER_F` |
| 17 | Filter resonance | `RESO_DRUM1` | `RESO_DRUM2` | `RESO_DRUM3` | `SNARE_RESO` | `CYM_RESO` | `HAT_RESO` | `HAT_RESO` |
| 18 | Filter drive | `128+CC2_FILTER_DRIVE_1` | `128+CC2_FILTER_DRIVE_2` | `128+CC2_FILTER_DRIVE_3` | `128+CC2_FILTER_DRIVE_4` | `128+CC2_FILTER_DRIVE_5` | `128+CC2_FILTER_DRIVE_6` | `128+CC2_FILTER_DRIVE_6` |
| 19 | Filter type | `128+CC2_FILTER_TYPE_1` | `128+CC2_FILTER_TYPE_2` | `128+CC2_FILTER_TYPE_3` | `128+CC2_FILTER_TYPE_4` | `128+CC2_FILTER_TYPE_5` | `128+CC2_FILTER_TYPE_6` | `128+CC2_FILTER_TYPE_6` |
| 20 | Snare mix | -- | -- | -- | `SNARE_MIX` | -- | -- | -- |
| 21 | Snare noise frequency | -- | -- | -- | `SNARE_NOISE_F` | -- | -- | -- |
| 22 | Mix/mod select | `128+CC2_MIX_MOD_1` | `128+CC2_MIX_MOD_2` | `128+CC2_MIX_MOD_3` | -- | -- | -- | -- |
| 23 | Velocity volume mod on/off | `128+CC2_VOLUME_MOD_ON_OFF1` | `128+CC2_VOLUME_MOD_ON_OFF2` | `128+CC2_VOLUME_MOD_ON_OFF3` | `128+CC2_VOLUME_MOD_ON_OFF4` | `128+CC2_VOLUME_MOD_ON_OFF5` | `128+CC2_VOLUME_MOD_ON_OFF6` | `128+CC2_VOLUME_MOD_ON_OFF6` |
| 24 | Velocity mod amount | `128+CC2_VELO_MOD_AMT_1` | `128+CC2_VELO_MOD_AMT_2` | `128+CC2_VELO_MOD_AMT_3` | `128+CC2_VELO_MOD_AMT_4` | `128+CC2_VELO_MOD_AMT_5` | `128+CC2_VELO_MOD_AMT_6` | `128+CC2_VELO_MOD_AMT_6` |
| 25 | Velocity mod destination | `128+CC2_VEL_DEST_1` | `128+CC2_VEL_DEST_2` | `128+CC2_VEL_DEST_3` | `128+CC2_VEL_DEST_4` | `128+CC2_VEL_DEST_5` | `128+CC2_VEL_DEST_6` | `128+CC2_VEL_DEST_6` |
| 26 | Transient volume | `128+CC2_TRANS1_VOL` | `128+CC2_TRANS2_VOL` | `128+CC2_TRANS3_VOL` | `128+CC2_TRANS4_VOL` | `128+CC2_TRANS5_VOL` | `128+CC2_TRANS6_VOL` | `128+CC2_TRANS6_VOL` |
| 27 | Transient waveform | `128+CC2_TRANS1_WAVE` | `128+CC2_TRANS2_WAVE` | `128+CC2_TRANS3_WAVE` | `128+CC2_TRANS4_WAVE` | `128+CC2_TRANS5_WAVE` | `128+CC2_TRANS6_WAVE` | `128+CC2_TRANS6_WAVE` |
| 28 | Transient frequency | `128+CC2_TRANS1_FREQ` | `128+CC2_TRANS2_FREQ` | `128+CC2_TRANS3_FREQ` | `128+CC2_TRANS4_FREQ` | `128+CC2_TRANS5_FREQ` | `128+CC2_TRANS6_FREQ` | `128+CC2_TRANS6_FREQ` |
| 46 | Osc 1 fine tune | `F_OSC1_FINE` | `F_OSC2_FINE` | `F_OSC3_FINE` | `F_OSC4_FINE` | `F_OSC5_FINE` | `F_OSC6_FINE` | `F_OSC6_FINE` |
| 70 | Volume envelope repeat | -- | -- | -- | `REPEAT1` | `CYM_REPEAT` | -- | -- |
| 72 | Open hihat decay | -- | -- | -- | -- | -- | `VELOD6_OPEN` | `VELOD6_OPEN` |
| 73 | Volume envelope attack | `VELOA1` | `VELOA2` | `VELOA3` | `VELOA4` | `VELOA5` | `VELOA6` | `VELOA6` |
| 74 | Volume envelope slope | `VOL_SLOPE1` | `VOL_SLOPE2` | `VOL_SLOPE3` | `EG_SNARE1_SLOPE` | `CYM_SLOPE` | `VOL_SLOPE6` | `VOL_SLOPE6` |
| 75 | Volume envelope decay | `VELOD1` | `VELOD2` | `VELOD3` | `VELOD4` | `VELOD5` | `VELOD6` | `VELOD6` |
| 76 | LFO frequency | `FREQ_LFO1` | `FREQ_LFO2` | `FREQ_LFO3` | `FREQ_LFO4` | `FREQ_LFO5` | `FREQ_LFO6` | `FREQ_LFO6` |
| 77 | LFO amount | `AMOUNT_LFO1` | `AMOUNT_LFO2` | `AMOUNT_LFO3` | `AMOUNT_LFO4` | `AMOUNT_LFO5` | `AMOUNT_LFO6` | `AMOUNT_LFO6` |
| 78 | LFO phase offset | `128+CC2_OFFSET_LFO1` | `128+CC2_OFFSET_LFO2` | `128+CC2_OFFSET_LFO3` | `128+CC2_OFFSET_LFO4` | `128+CC2_OFFSET_LFO5` | `128+CC2_OFFSET_LFO6` | `128+CC2_OFFSET_LFO6` |
| 79 | LFO waveform | `128+CC2_WAVE_LFO1` | `128+CC2_WAVE_LFO2` | `128+CC2_WAVE_LFO3` | `128+CC2_WAVE_LFO4` | `128+CC2_WAVE_LFO5` | `128+CC2_WAVE_LFO6` | `128+CC2_WAVE_LFO6` |
| 80 | LFO target voice selector | `128+CC2_VOICE_LFO1` | `128+CC2_VOICE_LFO2` | `128+CC2_VOICE_LFO3` | `128+CC2_VOICE_LFO4` | `128+CC2_VOICE_LFO5` | `128+CC2_VOICE_LFO6` | `128+CC2_VOICE_LFO6` |
| 81 | LFO target selector | `128+CC2_TARGET_LFO1` | `128+CC2_TARGET_LFO2` | `128+CC2_TARGET_LFO3` | `128+CC2_TARGET_LFO4` | `128+CC2_TARGET_LFO5` | `128+CC2_TARGET_LFO6` | `128+CC2_TARGET_LFO6` |
| 82 | LFO retrigger | `128+CC2_RETRIGGER_LFO1` | `128+CC2_RETRIGGER_LFO2` | `128+CC2_RETRIGGER_LFO3` | `128+CC2_RETRIGGER_LFO4` | `128+CC2_RETRIGGER_LFO5` | `128+CC2_RETRIGGER_LFO6` | `128+CC2_RETRIGGER_LFO6` |
| 83 | LFO sync | `128+CC2_SYNC_LFO1` | `128+CC2_SYNC_LFO2` | `128+CC2_SYNC_LFO3` | `128+CC2_SYNC_LFO4` | `128+CC2_SYNC_LFO5` | `128+CC2_SYNC_LFO5` | `128+CC2_SYNC_LFO5` |
| 84 | Mod envelope decay | `PITCHD1` | `PITCHD2` | `PITCHD3` | `PITCHD4` | -- | -- | -- |
| 85 | Envelope mod amount | `MODAMNT1` | `MODAMNT2` | `MODAMNT3` | `MODAMNT4` | -- | -- | -- |
| 86 | Mod envelope slope | `PITCH_SLOPE1` | `PITCH_SLOPE2` | `PITCH_SLOPE3` | `PITCH_SLOPE4` | -- | -- | -- |
| 89 | Audio output routing | `128+CC2_AUDIO_OUT1` | `128+CC2_AUDIO_OUT2` | `128+CC2_AUDIO_OUT3` | `128+CC2_AUDIO_OUT4` | `128+CC2_AUDIO_OUT5` | `128+CC2_AUDIO_OUT6` | `128+CC2_AUDIO_OUT6` |
| 90 | Envelope position reset | `128+CC2_ENVELOPE_POSITION_1` | `128+CC2_ENVELOPE_POSITION_2` | `128+CC2_ENVELOPE_POSITION_3` | `128+CC2_ENVELOPE_POSITION_4` | `128+CC2_ENVELOPE_POSITION_5` | `128+CC2_ENVELOPE_POSITION_6` | `128+CC2_ENVELOPE_POSITION_6` |
| 102 | Mod oscillator waveform | `MOD_WAVE_DRUM1` | `MOD_WAVE_DRUM2` | `MOD_WAVE_DRUM3` | -- | -- | -- | -- |
| 103 | FM/mod oscillator coarse tune | `FMDTN1` | `FMDTN2` | `FMDTN3` | -- | -- | -- | -- |
| 104 | FM amount | `FMAMNT1` | `FMAMNT2` | `FMAMNT3` | -- | -- | -- | -- |
| 105 | Mod oscillator waveform 1 | -- | -- | -- | -- | `CYM_WAVE2` | `WAVE2_HH` | `WAVE2_HH` |
| 106 | Mod oscillator gain 1 | -- | -- | -- | -- | `CYM_MOD_OSC_GAIN1` | `MOD_OSC_GAIN1` | `MOD_OSC_GAIN1` |
| 107 | Mod oscillator coarse tune 1 | -- | -- | -- | -- | `CYM_MOD_OSC_F1` | `MOD_OSC_F1` | `MOD_OSC_F1` |
| 108 | Mod oscillator waveform 2 | -- | -- | -- | -- | `CYM_WAVE3` | `WAVE3_HH` | `WAVE3_HH` |
| 109 | Mod oscillator gain 2 | -- | -- | -- | -- | `CYM_MOD_OSC_GAIN2` | `MOD_OSC_GAIN2` | `MOD_OSC_GAIN2` |
| 110 | Mod oscillator coarse tune 2 | -- | -- | -- | -- | `CYM_MOD_OSC_F2` | `MOD_OSC_F2` | `MOD_OSC_F2` |
| 120 | Track mute | `128+CC2_MUTE_1` | `128+CC2_MUTE_2` | `128+CC2_MUTE_3` | `128+CC2_MUTE_4` | `128+CC2_MUTE_5` | `128+CC2_MUTE_6` | `128+CC2_MUTE_6` |

### Global MIDI CC/NRPN Table

The same complete table is also preserved in `knowledge_files/comms_spec_reference/MIDI_TABLE.md`.

Global CC table:

| CC | Function | Voice/scope | Internal target |
|---:|---|---|---|
| 0 | Bank change | Global/current behavior | Existing `ChannelMidiParser.c` CC0 path, global bank mask `0x3f` |
| 1 | Mod wheel/global morph | Global/current behavior | Existing `ChannelMidiParser.c` CC1 path, `seq_setGlobalMorphAutomationValue()` |
| 2 | Waveform Osc 1 | Drum 1 | `OSC_WAVE_DRUM1` |
| 3 | Waveform Osc 1 | Drum 2 | `OSC_WAVE_DRUM2` |
| 4 | Waveform Osc 1 | Drum 3 | `OSC_WAVE_DRUM3` |
| 5 | Waveform Osc 1 | Snare | `OSC_WAVE_SNARE` |
| 6 | NRPN data entry | Global NRPN state | Apply selected NRPN |
| 7 | Waveform Osc 1 | Cymbal | `CYM_WAVE1` |
| 8 | Waveform Osc 1 | Hihat | `WAVE1_HH` |
| 9 | Coarse tune Osc 1 | Drum 1 | `F_OSC1_COARSE` |
| 10 | Fine tune Osc 1 | Drum 1 | `F_OSC1_FINE` |
| 11 | Coarse tune Osc 1 | Drum 2 | `F_OSC2_COARSE` |
| 12 | Fine tune Osc 1 | Drum 2 | `F_OSC2_FINE` |
| 13 | Coarse tune Osc 1 | Drum 3 | `F_OSC3_COARSE` |
| 14 | Fine tune Osc 1 | Drum 3 | `F_OSC3_FINE` |
| 15 | Coarse tune Osc 1 | Snare | `F_OSC4_COARSE` |
| 16 | Fine tune Osc 1 | Snare | `F_OSC4_FINE` |
| 17 | Coarse tune Osc 1 | Cymbal | `F_OSC5_COARSE` |
| 18 | Fine tune Osc 1 | Cymbal | `F_OSC5_FINE` |
| 19 | Coarse tune Osc 1 | Hihat | `F_OSC6_COARSE` |
| 20 | Fine tune Osc 1 | Hihat | `F_OSC6_FINE` |
| 21 | Waveform mod Osc | Drum 1 | `MOD_WAVE_DRUM1` |
| 22 | Waveform mod Osc | Drum 2 | `MOD_WAVE_DRUM2` |
| 23 | Waveform mod Osc | Drum 3 | `MOD_WAVE_DRUM3` |
| 24 | Waveform mod Osc 1 | Cymbal | `CYM_WAVE2` |
| 25 | Waveform mod Osc 2 | Cymbal | `CYM_WAVE3` |
| 26 | Waveform mod Osc 1 | Hihat | `WAVE2_HH` |
| 27 | Waveform mod Osc 2 | Hihat | `WAVE3_HH` |
| 28 | Noise frequency | Snare | `SNARE_NOISE_F` |
| 29 | Mix osc/noise | Snare | `SNARE_MIX` |
| 30 | Coarse tune mod Osc 1 | Cymbal | `CYM_MOD_OSC_F1` |
| 31 | Coarse tune mod Osc 2 | Cymbal | `CYM_MOD_OSC_F2` |
| 32 | Gain mod Osc 1 | Cymbal | `CYM_MOD_OSC_GAIN1` |
| 33 | Gain mod Osc 2 | Cymbal | `CYM_MOD_OSC_GAIN2` |
| 34 | Coarse tune mod Osc 1 | Hihat | `MOD_OSC_F1` |
| 35 | Coarse tune mod Osc 2 | Hihat | `MOD_OSC_F2` |
| 36 | Gain mod Osc 1 | Hihat | `MOD_OSC_GAIN1` |
| 37 | Gain mod Osc 2 | Hihat | `MOD_OSC_GAIN2` |
| 38 | Filter frequency | Drum 1 | `FILTER_FREQ_DRUM1` |
| 39 | Filter frequency | Drum 2 | `FILTER_FREQ_DRUM2` |
| 40 | Filter frequency | Drum 3 | `FILTER_FREQ_DRUM3` |
| 41 | Filter frequency | Snare | `SNARE_FILTER_F` |
| 42 | Filter frequency | Cymbal | `CYM_FIL_FREQ` |
| 43 | Filter frequency | Hihat | `HAT_FILTER_F` |
| 44 | Filter resonance | Drum 1 | `RESO_DRUM1` |
| 45 | Filter resonance | Drum 2 | `RESO_DRUM2` |
| 46 | Filter resonance | Drum 3 | `RESO_DRUM3` |
| 47 | Filter resonance | Snare | `SNARE_RESO` |
| 48 | Filter resonance | Cymbal | `CYM_RESO` |
| 49 | Filter resonance | Hihat | `HAT_RESO` |
| 50 | Volume envelope attack | Drum 1 | `VELOA1` |
| 51 | Volume envelope decay | Drum 1 | `VELOD1` |
| 52 | Volume envelope attack | Drum 2 | `VELOA2` |
| 53 | Volume envelope decay | Drum 2 | `VELOD2` |
| 54 | Volume envelope attack | Drum 3 | `VELOA3` |
| 55 | Volume envelope decay | Drum 3 | `VELOD3` |
| 56 | Volume envelope attack | Snare | `VELOA4` |
| 57 | Volume envelope decay | Snare | `VELOD4` |
| 58 | Volume envelope attack | Cymbal | `VELOA5` |
| 59 | Volume envelope decay | Cymbal | `VELOD5` |
| 60 | Volume envelope attack | Hihat | `VELOA6` |
| 61 | Closed hihat decay time | Hihat | `VELOD6` |
| 62 | Open hihat decay time | Hihat | `VELOD6_OPEN` |
| 63 | Volume envelope slope | Drum 1 | `VOL_SLOPE1` |
| 64 | Volume envelope slope | Drum 2 | `VOL_SLOPE2` |
| 65 | Volume envelope slope | Drum 3 | `VOL_SLOPE3` |
| 66 | Volume envelope slope | Snare | `EG_SNARE1_SLOPE` |
| 67 | Volume envelope slope | Cymbal | `CYM_SLOPE` |
| 68 | Volume envelope slope | Hihat | `VOL_SLOPE6` |
| 69 | Volume envelope repeat count | Snare | `REPEAT1` |
| 70 | Volume envelope repeat count | Cymbal | `CYM_REPEAT` |
| 71 | Mod envelope decay | Drum 1 | `PITCHD1` |
| 72 | Mod envelope decay | Drum 2 | `PITCHD2` |
| 73 | Mod envelope decay | Drum 3 | `PITCHD3` |
| 74 | Mod envelope decay | Snare | `PITCHD4` |
| 75 | Envelope mod amount | Drum 1 | `MODAMNT1` |
| 76 | Envelope mod amount | Drum 2 | `MODAMNT2` |
| 77 | Envelope mod amount | Drum 3 | `MODAMNT3` |
| 78 | Envelope mod amount | Snare | `MODAMNT4` |
| 79 | Mod envelope slope | Drum 1 | `PITCH_SLOPE1` |
| 80 | Mod envelope slope | Drum 2 | `PITCH_SLOPE2` |
| 81 | Mod envelope slope | Drum 3 | `PITCH_SLOPE3` |
| 82 | Mod envelope slope | Snare | `PITCH_SLOPE4` |
| 83 | FM amount | Drum 1 | `FMAMNT1` |
| 84 | FM frequency | Drum 1 | `FMDTN1` |
| 85 | FM amount | Drum 2 | `FMAMNT2` |
| 86 | FM frequency | Drum 2 | `FMDTN2` |
| 87 | FM amount | Drum 3 | `FMAMNT3` |
| 88 | FM frequency | Drum 3 | `FMDTN3` |
| 89 | Voice volume | Drum 1 | `VOL1` |
| 90 | Voice volume | Drum 2 | `VOL2` |
| 91 | Voice volume | Drum 3 | `VOL3` |
| 92 | Voice volume | Snare | `VOL4` |
| 93 | Voice volume | Cymbal | `VOL5` |
| 94 | Voice volume | Hihat | `VOL6` |
| 95 | Voice pan | Drum 1 | `PAN1` |
| 96 | Voice pan | Drum 2 | `PAN2` |
| 97 | Voice pan | Drum 3 | `PAN3` |
| 98 | NRPN fine/LSB selector | Global NRPN state | Set active NRPN low 7 bits |
| 99 | NRPN coarse/MSB selector | Global NRPN state | Set active NRPN high 7 bits |
| 100 | Voice pan | Snare | `PAN4` |
| 101 | Voice pan | Cymbal | `PAN5` |
| 102 | Voice pan | Hihat | `PAN6` |
| 103 | Distortion | Drum 1 | `OSC1_DIST` |
| 104 | Distortion | Drum 2 | `OSC2_DIST` |
| 105 | Distortion | Drum 3 | `OSC3_DIST` |
| 106 | Distortion | Snare | `SNARE_DISTORTION` |
| 107 | Distortion | Cymbal | `CYMBAL_DISTORTION` |
| 108 | Distortion | Hihat | `HAT_DISTORTION` |
| 109 | Decimation | Drum 1 | `VOICE_DECIMATION1` |
| 110 | Decimation | Drum 2 | `VOICE_DECIMATION2` |
| 111 | Decimation | Drum 3 | `VOICE_DECIMATION3` |
| 112 | Decimation | Snare | `VOICE_DECIMATION4` |
| 113 | Decimation | Cymbal | `VOICE_DECIMATION5` |
| 114 | Decimation | Hihat | `VOICE_DECIMATION6` |
| 115 | Decimation | All voices | `VOICE_DECIMATION_ALL` |
| 116 | LFO frequency | Drum 1 | `FREQ_LFO1` |
| 117 | LFO frequency | Drum 2 | `FREQ_LFO2` |
| 118 | LFO frequency | Drum 3 | `FREQ_LFO3` |
| 119 | LFO frequency | Snare | `FREQ_LFO4` |
| 120 | LFO frequency | Cymbal | `FREQ_LFO5` |
| 121 | LFO frequency | Hihat | `FREQ_LFO6` |
| 122 | LFO amount | Drum 1 | `AMOUNT_LFO1` |
| 123 | LFO amount | Drum 2 | `AMOUNT_LFO2` |
| 124 | LFO amount | Drum 3 | `AMOUNT_LFO3` |
| 125 | LFO amount | Snare | `AMOUNT_LFO4` |
| 126 | LFO amount | Cymbal | `AMOUNT_LFO5` |
| 127 | LFO amount | Hihat | `AMOUNT_LFO6` |

Global NRPN table:

| NRPN | Function | Voice/scope | Internal target |
|---:|---|---|---|
| 0 | Filter drive | Drum 1 | `128+CC2_FILTER_DRIVE_1` |
| 1 | Filter drive | Drum 2 | `128+CC2_FILTER_DRIVE_2` |
| 2 | Filter drive | Drum 3 | `128+CC2_FILTER_DRIVE_3` |
| 3 | Filter drive | Snare | `128+CC2_FILTER_DRIVE_4` |
| 4 | Filter drive | Cymbal | `128+CC2_FILTER_DRIVE_5` |
| 5 | Filter drive | Hihat | `128+CC2_FILTER_DRIVE_6` |
| 6 | Mix/Mod select | Drum 1 | `128+CC2_MIX_MOD_1` |
| 7 | Mix/Mod select | Drum 2 | `128+CC2_MIX_MOD_2` |
| 8 | Mix/Mod select | Drum 3 | `128+CC2_MIX_MOD_3` |
| 9 | Velocity volume mod on/off | Drum 1 | `128+CC2_VOLUME_MOD_ON_OFF1` |
| 10 | Velocity volume mod on/off | Drum 2 | `128+CC2_VOLUME_MOD_ON_OFF2` |
| 11 | Velocity volume mod on/off | Drum 3 | `128+CC2_VOLUME_MOD_ON_OFF3` |
| 12 | Velocity volume mod on/off | Snare | `128+CC2_VOLUME_MOD_ON_OFF4` |
| 13 | Velocity volume mod on/off | Cymbal | `128+CC2_VOLUME_MOD_ON_OFF5` |
| 14 | Velocity volume mod on/off | Hihat | `128+CC2_VOLUME_MOD_ON_OFF6` |
| 15 | Velocity mod amount | Drum 1 | `128+CC2_VELO_MOD_AMT_1` |
| 16 | Velocity mod amount | Drum 2 | `128+CC2_VELO_MOD_AMT_2` |
| 17 | Velocity mod amount | Drum 3 | `128+CC2_VELO_MOD_AMT_3` |
| 18 | Velocity mod amount | Snare | `128+CC2_VELO_MOD_AMT_4` |
| 19 | Velocity mod amount | Cymbal | `128+CC2_VELO_MOD_AMT_5` |
| 20 | Velocity mod amount | Hihat | `128+CC2_VELO_MOD_AMT_6` |
| 21 | Velocity mod destination | Drum 1 | `128+CC2_VEL_DEST_1` |
| 22 | Velocity mod destination | Drum 2 | `128+CC2_VEL_DEST_2` |
| 23 | Velocity mod destination | Drum 3 | `128+CC2_VEL_DEST_3` |
| 24 | Velocity mod destination | Snare | `128+CC2_VEL_DEST_4` |
| 25 | Velocity mod destination | Cymbal | `128+CC2_VEL_DEST_5` |
| 26 | Velocity mod destination | Hihat | `128+CC2_VEL_DEST_6` |
| 27 | LFO waveform | Drum 1 | `128+CC2_WAVE_LFO1` |
| 28 | LFO waveform | Drum 2 | `128+CC2_WAVE_LFO2` |
| 29 | LFO waveform | Drum 3 | `128+CC2_WAVE_LFO3` |
| 30 | LFO waveform | Snare | `128+CC2_WAVE_LFO4` |
| 31 | LFO waveform | Cymbal | `128+CC2_WAVE_LFO5` |
| 32 | LFO waveform | Hihat | `128+CC2_WAVE_LFO6` |
| 33 | LFO target voice | Drum 1 | `128+CC2_VOICE_LFO1` |
| 34 | LFO target voice | Drum 2 | `128+CC2_VOICE_LFO2` |
| 35 | LFO target voice | Drum 3 | `128+CC2_VOICE_LFO3` |
| 36 | LFO target voice | Snare | `128+CC2_VOICE_LFO4` |
| 37 | LFO target voice | Cymbal | `128+CC2_VOICE_LFO5` |
| 38 | LFO target voice | Hihat | `128+CC2_VOICE_LFO6` |
| 39 | LFO target | Drum 1 | `128+CC2_TARGET_LFO1` |
| 40 | LFO target | Drum 2 | `128+CC2_TARGET_LFO2` |
| 41 | LFO target | Drum 3 | `128+CC2_TARGET_LFO3` |
| 42 | LFO target | Snare | `128+CC2_TARGET_LFO4` |
| 43 | LFO target | Cymbal | `128+CC2_TARGET_LFO5` |
| 44 | LFO target | Hihat | `128+CC2_TARGET_LFO6` |
| 45 | LFO retrigger | Drum 1 | `128+CC2_RETRIGGER_LFO1` |
| 46 | LFO retrigger | Drum 2 | `128+CC2_RETRIGGER_LFO2` |
| 47 | LFO retrigger | Drum 3 | `128+CC2_RETRIGGER_LFO3` |
| 48 | LFO retrigger | Snare | `128+CC2_RETRIGGER_LFO4` |
| 49 | LFO retrigger | Cymbal | `128+CC2_RETRIGGER_LFO5` |
| 50 | LFO retrigger | Hihat | `128+CC2_RETRIGGER_LFO6` |
| 51 | LFO sync | Drum 1 | `128+CC2_SYNC_LFO1` |
| 52 | LFO sync | Drum 2 | `128+CC2_SYNC_LFO2` |
| 53 | LFO sync | Drum 3 | `128+CC2_SYNC_LFO3` |
| 54 | LFO sync | Snare | `128+CC2_SYNC_LFO4` |
| 55 | LFO sync | Cymbal | `128+CC2_SYNC_LFO5` |
| 56 | LFO sync | Hihat | `128+CC2_SYNC_LFO6` |
| 57 | LFO offset | Drum 1 | `128+CC2_OFFSET_LFO1` |
| 58 | LFO offset | Drum 2 | `128+CC2_OFFSET_LFO2` |
| 59 | LFO offset | Drum 3 | `128+CC2_OFFSET_LFO3` |
| 60 | LFO offset | Snare | `128+CC2_OFFSET_LFO4` |
| 61 | LFO offset | Cymbal | `128+CC2_OFFSET_LFO5` |
| 62 | LFO offset | Hihat | `128+CC2_OFFSET_LFO6` |
| 63 | Filter type | Drum 1 | `128+CC2_FILTER_TYPE_1` |
| 64 | Filter type | Drum 2 | `128+CC2_FILTER_TYPE_2` |
| 65 | Filter type | Drum 3 | `128+CC2_FILTER_TYPE_3` |
| 66 | Filter type | Snare | `128+CC2_FILTER_TYPE_4` |
| 67 | Filter type | Cymbal | `128+CC2_FILTER_TYPE_5` |
| 68 | Filter type | Hihat | `128+CC2_FILTER_TYPE_6` |
| 69 | Transient generator volume | Drum 1 | `128+CC2_TRANS1_VOL` |
| 70 | Transient generator volume | Drum 2 | `128+CC2_TRANS2_VOL` |
| 71 | Transient generator volume | Drum 3 | `128+CC2_TRANS3_VOL` |
| 72 | Transient generator volume | Snare | `128+CC2_TRANS4_VOL` |
| 73 | Transient generator volume | Cymbal | `128+CC2_TRANS5_VOL` |
| 74 | Transient generator volume | Hihat | `128+CC2_TRANS6_VOL` |
| 75 | Transient generator waveform | Drum 1 | `128+CC2_TRANS1_WAVE` |
| 76 | Transient generator waveform | Drum 2 | `128+CC2_TRANS2_WAVE` |
| 77 | Transient generator waveform | Drum 3 | `128+CC2_TRANS3_WAVE` |
| 78 | Transient generator waveform | Snare | `128+CC2_TRANS4_WAVE` |
| 79 | Transient generator waveform | Cymbal | `128+CC2_TRANS5_WAVE` |
| 80 | Transient generator waveform | Hihat | `128+CC2_TRANS6_WAVE` |
| 81 | Transient generator frequency | Drum 1 | `128+CC2_TRANS1_FREQ` |
| 82 | Transient generator frequency | Drum 2 | `128+CC2_TRANS2_FREQ` |
| 83 | Transient generator frequency | Drum 3 | `128+CC2_TRANS3_FREQ` |
| 84 | Transient generator frequency | Snare | `128+CC2_TRANS4_FREQ` |
| 85 | Transient generator frequency | Cymbal | `128+CC2_TRANS5_FREQ` |
| 86 | Transient generator frequency | Hihat | `128+CC2_TRANS6_FREQ` |
| 87 | Audio output routing | Drum 1 | `128+CC2_AUDIO_OUT1` |
| 88 | Audio output routing | Drum 2 | `128+CC2_AUDIO_OUT2` |
| 89 | Audio output routing | Drum 3 | `128+CC2_AUDIO_OUT3` |
| 90 | Audio output routing | Snare | `128+CC2_AUDIO_OUT4` |
| 91 | Audio output routing | Cymbal | `128+CC2_AUDIO_OUT5` |
| 92 | Audio output routing | Hihat | `128+CC2_AUDIO_OUT6` |

## Documents Updated

- `knowledge_files/log_archive/000_SESSION_INDEX.md`
- `knowledge_files/log_archive/029_SESSION_HANDOFF_LOG.md`
- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
- `knowledge_files/comms_spec_reference/MIDI_TABLE.md`
- `MEMORY.md`

No update was needed for `knowledge_files/comms_spec_reference/BACKGROUND_LOAD_TEMPORARY.md`; the Session 029 change is external MIDI parsing/routing and does not change background-load storage semantics.

## Known Issues Introduced

None known.

## Known Issues / Watch Items

- Channel hihat CC83 currently stores/echoes `128+CC2_SYNC_LFO5` while applying hihat LFO sync. This is inherited behavior documented in `MIDI_TABLE.md`, not a Session 029 regression.
- The new Global parser intentionally removes old Global CC2-127 side effects such as old CC16 roll-rate, CC102 pattern change, CC103 loop, CC113-120 mutes, and CC121 reset because they conflicted with the requested Global table.

## Next Session Recommended Goal

Hardware-smoke any Global CC/NRPN rows that were not exercised during this session, especially selector-style high-parameter NRPNs such as velocity destination, LFO target, filter type, transient, and audio routing.

## Blockers

None.

## Critical Reminders For Next Session

- Do not reintroduce old Global CC2-127 side effects unless a new explicit compatibility mode is designed.
- Keep Global CC0 and CC1 on the existing bank/morph path.
- Keep the Global-channel CC pre-emption rule: Global CC2-127 must not also trigger overlapping voice-channel assignments.
- Use `knowledge_files/comms_spec_reference/MIDI_TABLE.md` as the durable external MIDI table after the root scratch docs are deleted.

## End Of Session Block

```text
DATE: 2026-06-21
SESSION GOAL: Add a separate Global-channel MIDI CC/NRPN implementation while preserving current Channel MIDI plus Global CC0/CC1 behavior.
COMPLETED: Implemented Global CC2-127 lookup table, Global NRPN selector/data-entry table, CC routing pre-emption, durable MIDI docs, session index, comms reference, and memory updates.
VERIFIED ON HARDWARE: Yes. User reported the new Global implementation tested out ok and confirmed Global CC assignments pre-empt overlapping channel assignments.

CHANGES THIS SESSION:
- mainboard/LxrStm32/src/MIDI/GlobalMidiParser.c: added Global CC/NRPN lookup tables and internal apply helper; replaced old Global sparse CC ladder for CC2-127.
- mainboard/LxrStm32/src/MIDI/MidiParser.c: changed CC routing so Global CC0/CC1 preserve old behavior and Global CC2-127 pre-empt channel assignments.
- knowledge_files/comms_spec_reference/MIDI_TABLE.md: added complete durable external MIDI input spec.
- knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md: documented MIDI table location and Global CC pre-emption.
- knowledge_files/log_archive/000_SESSION_INDEX.md: added Session 029 and cross-session facts.
- knowledge_files/log_archive/029_SESSION_HANDOFF_LOG.md: added this handoff.
- MEMORY.md: updated current project context.

KNOWN ISSUES INTRODUCED: None known.
KNOWN ISSUES RESOLVED: Global-channel CC2-127 now uses the requested separate table instead of the old sparse side-effect ladder, and overlapping Global/voice CC assignments now resolve predictably with Global pre-emption.

NEXT SESSION RECOMMENDED GOAL: Hardware-smoke remaining Global NRPN rows and any controller workflows not exercised in Session 029.
BLOCKERS: None.

CRITICAL REMINDERS FOR NEXT SESSION:
- Global CC0/CC1 must remain current bank/morph behavior.
- Global CC2-127 must pre-empt overlapping voice-channel assignments.
- The durable external MIDI table is knowledge_files/comms_spec_reference/MIDI_TABLE.md.
```
