================================================================================
START OF REFACTOR_DIAGRAM.md
================================================================================

# ARCHITECTURAL REFACTOR DIAGRAM & API SPECIFICATION
**Project**: LXR Enhanced Firmware (STM32 Audio Engine Subsystems)  
**Date**: 2026-06-11  
**Status**: Target Refactor Specification (updated through Sessions 007-011; Phase 4 is complete, Phase 5 is next)

---

## 1. EXECUTIVE SUMMARY

The objective of this architectural refactor is to decouple the sound parameter authority, pattern playback state, and front-panel communication protocols from the monolithic structures in the current sequencer and MIDI parser modules. 

Historically, `sequencer.c` and parts of the `MIDI/` parser mixed audio-rate real-time thread logic with block-level storage transfers, parameter interpolation, file-system caching, and raw UART stream decoding. This refactor establishes clean, structured APIs across three newly isolated sub-directories within `mainboard/LxrStm32/src/`:

1.  **`/uARTFrontSYX/`**: implementation of the (future SysEx) protocol specification. Physical front-panel UART traffic feeds directly into this layer, the `/MIDI/` module is eventually updated to act as a front interpreter to map generic incoming MIDI messages (CC, Notes, RTC) straight into local SysEx packets to go here.
2.  **`/Preset/`**: Centralized authority for all sound state parameters (normal kit endpoints, morph endpoints, interpolated run-time baselines, and active modulation target caches). The concrete implementation now lives in `KitState.c/h`, `ParameterMap.c/h`, `ParameterIngress.c/h`, `MorphEngine.c/h`, `EndpointRestore.c/h`, `TempPlaybackSwitch.c/h`, and `PresetLoadCache.c/h` for restore policy, temp switching, and the single overwriteable background-load session.
3.  **`/Sequencer/Pattern/`**: Forms a specialized sub-directory under the sequencer subsystem that exclusively manages pattern data and generative algorithms (Euclidean/SOM). Temp/background pattern load policy and temp/normal boundary decisions remain in `Preset`.

---

## 2. SUBSYSTEM INTERCONNECTION DIAGRAM

The following ASCII structural diagram models the modular blocks, the layout of their respective files, and the directional flow of data across their API boundaries.

[START_OF_TEXT_BLOCK]
======================================================================================================
                                     EXTERNAL I/O BOUNDARY
======================================================================================================
     || Physical ATmega UART Link (500k Baud)                || External MIDI DIN / USB (CC, Notes)
     ||                                                      \/
     ||                      +-----------------------------------------------------------------------+
     ||                      |                             /MIDI/                                    |
     ||                      |  [Translates standard MIDI parameters into SysEx instruction packets] |
     ||                      |                     |                                                 |
     ||                      |                     | (SysEx Packets)                                 |
     \/                      |                     v                                                 |
| +------------------------------------------------------------------------------------------------+ |
| |                                        /uARTFrontSYX/                                          | |
| |  [Unified SysEx Specification Layer: Owns all protocol parsing, unpacking, and formatting]     | |
| |                                                                                                | |
| |  +---------------------------+              +------------------------------------------------+ | |
| |  |          Uart.c/h         | -----------> |                frontPanelParser.c/h            | | |
| |  |  - Low-level ring buffers |  (Raw Bytes) |  - Comprehensive SysEx Protocol Interpreter    | | |
| |  |  - IRQ byte streaming     |              |  - Dispatches validated specs to downstreams   | | |
| |  +---------------------------+              +------------------------------------------------+ | |
| +------------------------------------------------------------------------------------------------+ |
+----------------------------------------------------------------------------------------------------+
       ^                                              ^                                       ^
       | (Pushes Decoded Endpoint Data)               | (Pushes Pattern Steps / Triggers)     |
       |                                              |                                       |
       v                                              v                                       |
+------------------------------------+      +-----------------------------------------+       | (Triggers
|              /Preset/              |      |           /Sequencer/Pattern/           |       |  Menu
|  [Canonical Sound Parameter Store] |      |  [Pattern Layout & Generative Logic]    |       |  Pushbacks/
|                                    |      |                                         |       |  Endpoint
|  +------------------------------+  |      |  +-----------------------------------+  |       |  Restores)
|  |  KitState / ParameterMap / ParameterIngress / MorphEngine  |  |      |  |           PatternData.c/h         |  |       |
|  | - normal/tmpKitState arrays  |  |      |  | - Pattern structs (Steps, Gates)  |  |       |
|  | - Ingress routing matrices   |  |      |  | - Pattern access/mutation API     |  |       |
|  +------------------------------+  |      |  | - Copy/clear helpers              |  |       |
|                |                   |      |  +-----------------------------------+  |       |
|                v                   |      |    ^                  ^                 |       |
|  +------------------------------+  |      |    | (Generates       | (Fills          |       |
|  |         MorphEngine          |  |      |    |  Steps)          |  Weights)       |       |
|  | - Standard Morph Worker      |  |      |  +-----------------------------------+  |       |
|  | - Phased LFO-to-Morph Drain  |  |      |  | EuklidGenerator | SomData/Generator| |       |
|  | - Live Apply Cache Matrix    |  |      |  | - Euclidean math| - neural-net gen | |       |
|  +------------------------------+  |      |  +-----------------------------------+  |       |
+------------------------------------+      +-----------------------------------------+       |
       ^                                                              ^                       |
       | (v Provides Live Sound Values)                               | (Provides Real-Time   |
       | (^ Sets values from automation)                              |  Step Triggers)       |
       v                                                              v                       v
+---------------------------------------------------------------------------------------------+-------+
|                                         /Sequencer/                                                 |
|  [Core Real-Time Sound Generation & Hardware Interrupt Processing]                                  |
|                                                                                                     |
|  +-----------------------------------------------------------------------------------------------+  |
|  |                                         sequencer.c/h                                         |  |
|  |  - Real-time `seq_tick()` step engine & clock synchronization                                 |  |
|  |  - High-Hz Modulation matrix traversal & direct interface to DSP Audio Rendering Pipeline     |  |
|  +-----------------------------------------------------------------------------------------------+  |
+----------------------------------------------------------------------------------------------------+
[END_OF_TEXT_BLOCK]

---

## 3. FUNCTION GROUPS

### 3.1 /uARTFrontSYX/
This folder contains the protocol interface. It is responsible for parsing serial byte data against the core (future SysEx) specification layout

#### `Uart.c/h`
* **Purpose**: Low-level interrupt handling and basic byte-stream ring buffering.

#### `frontPanelParser.c/h`
* **Purpose**: Comprehensive SysEx specification implementation, message verification, and routing. Receives raw traffic from the physical link, as well as pre-interpreted SysEx packets funneled down by the standard `/MIDI/` module.
* **Primary Function Groups**:
    * Reads from the UART input ring buffer, tracking state machines for active opcodes, packet headers, lengths, and checksums.
    * Main interpreter loop that isolates SysEx IDs, data payloads, and validation bits.
    * Maps validated SysEx instructions straight to functional APIs in `/Preset/` and `/Sequencer/Pattern/`. Examples:
        * `0x65` / `0x66` -> `preset_storeParameter()`
        * Pattern configuration changes -> `pattern_setNextPattern()`
    * Encodes outbound parameters into proper SysEx framing to update the ATmega display layout.

---

### 3.2 /Preset/
The preset module retains sole authority over parameter image states. It completely manages sound parameter images and time-sliced interpolation mechanics.

#### `KitState.c/h`, `ParameterMap.c/h`, `ParameterIngress.c/h`, `MorphEngine.c/h`
* **Purpose**: Consolidates direct memory layout representation, state routing boundaries, parameter configuration ingestion, image states, and morph/live-apply ownership that used to sit directly inside `sequencer.c`.
* **Primary Function Groups**:
    * Stores incoming parameters or kit byte values into the designated destination array
    * Reconciles raw selector bytes with resolved operational target maps (e.g., resolving LFO destination mappings synchronously to eliminate detached assignments)
	* Transmits parameter updates to sequencer/DSP as necessary
    * Internal data operations as necessary for various sequencer or pattern states

#### `MorphEngine.c/h`
* **Purpose**: Handles async parameter drain, interpolation mechanics, and voice morph modulations.
* **Primary Function Groups**:
    * Owns fundamental, single-pass async worker function. Executed exactly once per main loop iteration, it processes one individual parameter, interpolating across current states to populate `interpolatedParams`.
    * Evaluates LFO modulations targeting voice morph PAR_MORPH_X as a second-pass interpolation on parameters for X, appended to the single pass, one-param-per-loop, morph async drain.
    * Captures note trigger velocity into voice morph value if so automated

#### `TempPlaybackSwitch.c/h`
* **Purpose**: Owns normal/temp source selection, temp-kit validity, voice-source routing, and the switch-state bookkeeping that decides when a pattern change should flip the active image.
* **Primary Function Groups**:
    * Captures the temp kit from the current normal image when temp playback is first needed.
    * Applies the hihat-coupled per-voice source selection rules at normal/temp boundaries.
    * Owns the pending-switch flags and the commit path that tells the sequencer when a source change is complete.

#### `EndpointRestore.c/h`
* **Purpose**: Handshaking, rate-limiting, and managing the state of async block parameters transmitted to update the ATmega display layout.
* **Primary Function Groups**:
    * Schedules background state pushbacks to the physical user interface.
    * Iterates through background tasks safely, metering packets to prevent receiver FIFO overflows on the ATmega hardware.

#### `PresetLoadCache.c/h`
* **Purpose**: Owns the single in-flight background file-load session, deferred replay bookkeeping, and live snapshot/cache state that replaces the older voice-cache promotion path.
* **Primary Function Groups**:
    * Accepts a new background file-load request and, if one is already in flight, replaces/discards the older pending one.
    * Tracks deferred replay / session completion state until the background load is committed or aborted.
    * Keeps the protocol layer thin by centralizing the "load into normal storage while temp continues playing" behavior here instead of in `frontPanelParser.c`.

---

### 3.3 /Sequencer/Pattern/
This component encapsulates step allocations, track parameters, and mathematical generation algorithms, separating sequencing databases from runtime trigger loops.

#### `PatternData.c/h`
* **Purpose**: Holds structural layouts for patterns (`seq_patternSet`, `seq_tmpPattern`) and the pure pattern copy/clear/mutation helpers. Temp/normal source switching is owned by `Preset/TempPlaybackSwitch.c/h`, not by the pattern data module.
* **Primary Function Groups**:
    * Erases and formats data matrices.
    * Flags and tracks pattern storage states and transitions
    * Provides the read/write API for pattern storage so the sequencer no longer indexes the storage arrays directly
    * Standard read-only endpoint utilized by the real-time playback sequencer to analyze triggers and parameter automation flags without exposing interior arrays

#### `EuklidGenerator.c/h` & `SomGenerator.c/h` / `SomData.c/h`
* **Purpose**: Procedural rhythmic generation within the local pattern memory.
* **Primary Function Groups**:
    * Generates Euclidean rhythms mathematically, filling target arrays directly.
    * Computes self-organizing map modifications to alter pattern densities based on local configurations.

---
