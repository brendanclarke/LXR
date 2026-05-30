# `.PRF` / `.ALL` LOAD FIX AUDIT - IN FLIGHT

Date: 2026-05-29  
Status: Functional baseline restored to end-of-transcript-2 state. Handshake-based parameter pushback is verified and working. Experimental proposals for endpoint-aware switching are marked as SUSPECT.

## Purpose
This document replaces `FILEFIX_AUDIT.md` for the current in-flight work. It tracks the progress of repairing `.PRF` background-load isolation and temporary pattern/parameter cache functionality.

## Current Verified Functional Baseline (Restoration Complete)

### 1. Robust STM->AVR Handshake
A 5-phase handshake ensures reliable parameter synchronization:
- `PARAM_RESTORE_BEGIN`: STM signals start.
- `Wait for READY`: STM blocks but loops `uart_processFront()` to catch the AVR's response.
- `DATA`: STM sends valid parameters using `PRF_RESTORE_PARAM_CC/CC2`.
- `PARAM_RESTORE_DONE`: STM signals completion.
- `Wait for ACK`: STM waits for AVR to finish repainting and acknowledge.

### 2. Index Offset Correction
STM canonical index `param` maps to AVR menu index `param - 1` for CCs < 128.
- **Rule**: +1 on ingress (AVR->STM), -1 on egress (STM->AVR).
- **Verified**: Coarse oscillator values (32, 49, etc.) land in correct coarse menu slots.

### 3. AVR Feedback Protection & Whitelisting
- While `frontParser_restoreActive` is set, `frontPanel_sendData()` suppresses all outbound parameter traffic to prevent authoritative edit loops.
- Restore status bytes are explicitly whitelisted in `frontPanel_parseData` to bypass `rxDisable`.
- `PRF_RESTORE_PARAM_CC/CC2` updates `parameter_values[]` only; `parameters2[]` (morph endpoint) is preserved.

### 4. SEQ16 Temporary Pattern & STM Cache
- SEQ16 reassigned as temp-pattern 'SELECT'. Supports copy/paste, selection, and playback.
- STM-side `SeqTmpKitState` captures current voice parameters and automation targets beside the temp slot.
- Audio verified to follow temp pattern parameters correctly.

## Historical Context & Failure Modes

### Pushback Failure Theory (Resolved)
Previous failure was attributed to:
- **STM Blocking**: Wait loops prevented the UART parser from running, causing handshake timeouts.
- **AVR Silencing**: The parser was ignoring new status bytes while in transition states.
- **Initialization Bug**: `seq_tmpKitPushParamsToFrontEnabled` was being reset to 0 in `seq_init`.

### Legacy File Failures (Checkpoint 90d3f08)
- **v2 Layout Gap**: Old files lacked morph/scale blocks, causing offset desync.
- **Short-Read Fragility**: Loader previously ignored `bytesRead`, leading to stale buffer data.
- **Checkpoint Limits**: Morph automation and background loading into temp were disabled for the 90d3f08 baseline.

## SUSPECT / EXPERIMENTAL PROPOSALS (from Transcript 3)

> **WARNING**: The following proposals were part of a spike that failed and left the repository in a non-functional state. They are preserved here for context but should be treated as **highly suspect**.

### Experimental: Endpoint-Aware Temp Pattern Parameter Switching
The goal was to send full AVR endpoint arrays (`parameter_values[]` and `parameters2[]`) to the STM during copy, then push them back up on temp-entry.
- **Suspect Logic**: Relies on reusing `parameter_values_temp[]` which may conflict with legacy loader staging.
- **Hazard**: Large bidirectional bursts during pattern switching proved unstable and prone to desync.
- **Constraint**: Future re-implementation must proceed slowly and validate sound/menu stability at every step.

---
*Note: The system is currently at a verified functional baseline. Future sessions should prioritize slow, incremental validation of any changes to this core handshake.*
