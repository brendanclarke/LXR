# Opcode Audit

Scope:
- `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`
- `mainboard/LxrStm32/src/uARTFrontSYX/frontPanelReceivingProtocol.h`

Method:
- I traced literal opcode references across the tree and checked the main AVR and STM dispatchers.
- I treated names with zero literal references as suspects, but I also checked for cases where the code emits the numeric value by offset arithmetic instead of naming each slot directly.
- Cache-related names were treated as first-priority cleanup candidates, per the session goal.

## High-confidence cleanup candidates

These have no live call sites in the current tree, or the only handling left is a legacy placeholder/no-op path.

| Opcode | Side | Why it is suspicious |
|---|---|---|
| `CODEC_CC` | AVR | No literal callers or handlers found. |
| `FRONT_CODEC_CONTROL` | STM | No literal callers or handlers found. |
| `PRESET_SAVE` | AVR | No literal callers found. |
| `PATTERN_LOAD` | AVR | No literal callers found. |
| `SEQ_ROLL_ON` | AVR | No literal callers or handlers found. |
| `SEQ_ROLL_OFF` | AVR | No literal callers or handlers found. |
| `SEQ_MIDI_MODE` | AVR | The header marks it unused and there are no live handlers. |
| `FRONT_SEQ_MIDI_MODE` | STM | The header marks it unused and there are no live handlers. |
| `PRF_CACHE_ACCEPTED` | AVR / STM | Defined as a status byte, but nothing in-tree currently consumes it. |

## Cache-family suspects

Anything with `cache` in the name should be reviewed together, because these opcodes all belong to the older PRF/background-load handshake.

| Opcode | Side | Current state |
|---|---|---|
| `SEQ_PRF_CACHE_BEGIN` | AVR | Still emitted by the AVR sender helper, but this is the oldest part of the load-session handshake. |
| `SEQ_PRF_PENDING_BEGIN` | AVR | No literal callers found. |
| `SEQ_PRF_PENDING_DONE` | AVR | No literal callers found. |
| `SEQ_PRF_CACHE_ABORT` | AVR | No literal callers found. |
| `SEQ_PRF_AVR_SNAPSHOT_BEGIN` | AVR | No literal callers found. |
| `SEQ_PRF_AVR_SNAPSHOT_END` | AVR | No literal callers found. |
| `SEQ_PRF_RESTORE_AVR_LIVE` | AVR | No literal callers found. |
| `PRF_CACHE_STATUS` | AVR / STM | Still used as a status-report slot, but only for the deprecated handshake. |
| `PRF_CACHE_REJECTED` | AVR / STM | Still used as the reject code for the deprecated handshake. |
| `PRF_CACHE_ACCEPTED` | AVR / STM | Defined but not consumed anywhere in-tree. |
| `FRONT_SEQ_PRF_CACHE_BEGIN` | STM | The STM handler explicitly rejects this now. |
| `FRONT_SEQ_PRF_PENDING_BEGIN` | STM | Handler only grants flow and otherwise does nothing. |
| `FRONT_SEQ_PRF_PENDING_DONE` | STM | Handler only grants flow and otherwise does nothing. |
| `FRONT_SEQ_PRF_CACHE_ABORT` | STM | Handler only ends ingress and grants flow. |
| `FRONT_SEQ_PRF_AVR_SNAPSHOT_BEGIN` | STM | Handler only grants flow and otherwise does nothing. |
| `FRONT_SEQ_PRF_AVR_SNAPSHOT_END` | STM | Handler only grants flow and otherwise does nothing. |
| `FRONT_SEQ_PRF_RESTORE_AVR_LIVE` | STM | Handler only waits/grants flow; no other work remains. |

## Do not delete blindly

These names do not show up as literal call sites, but that does not automatically mean they are dead.

| Opcode family | Why it needs caution |
|---|---|
| `SEQ_TRACK_NOTE2` through `SEQ_TRACK_NOTE7` | The AVR side emits the note slots by arithmetic from `SEQ_TRACK_NOTE1`, so the zero-reference count is expected. |
| `FRONT_SEQ_TRACK_NOTE2` through `FRONT_SEQ_TRACK_NOTE7` | The STM side still handles each slot explicitly in the receive switch. |
| `SEQ_TMP_PATTERN` | This is a sentinel/index constant, not a wire command. |

## Cleanup plan

1. Confirm whether the deprecated PRF/background-load handshake still needs any compatibility path beyond the current direct file-load flow.
2. If not, remove the AVR sender helpers for the cache-family opcodes first, then delete the STM reject/no-op branches that only exist to answer them.
3. Remove the dead opcode constants from both headers together so the wire contract stays consistent.
4. Delete any helper state that only exists to support the retired handshake, including the cache status tracking variables and any one-off parser branches that only forward those messages.
5. Re-run the AVR and STM builds, then smoke-test file load, temp switch, and normal/background-load behavior on hardware.

## Notes

- I already updated the opcode headers so every opcode line now carries a short comment.
- The cache family is the biggest cleanup target, but it should be retired as one protocol decision rather than by deleting a few constants at a time.
- If the team wants to keep any of the cache opcodes for a future protocol version, they should be renamed or isolated behind a clearly marked compatibility shim before removal of the rest of the legacy path.
