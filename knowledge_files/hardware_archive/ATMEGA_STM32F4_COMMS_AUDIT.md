# ATmega644 ↔ STM32F4 UART Communications Audit

**Project:** LXR Drum Synth — custom-develop-patload-envmod branch  
**Audit scope:** Inter-processor UART link (USART3 on STM32F4 / USART0 on ATmega644)  
**Files examined:**  
- `mainboard/LxrStm32/src/MIDI/Uart.c / Uart.h`  
- `mainboard/LxrStm32/src/MIDI/frontPanelParser.c / frontPanelParser.h`  
- `mainboard/LxrStm32/src/MIDI/MidiMessages.h`  
- `mainboard/LxrStm32/src/Hardware/FIFO.c / FIFO.h`  
- `mainboard/LxrStm32/src/Sequencer/sequencer.c`  
- `mainboard/LxrStm32/src/main.c`  
- `front/LxrAvr/IO/uart.c / uart.h`  
- `front/LxrAvr/frontPanelParser.c / frontPanelParser.h`  
- `front/LxrAvr/fifo.c / fifo.h`  
- `front/LxrAvr/Preset/presetManager.c`  
- `front/LxrAvr/main.c`  

---

## 1. System Overview

The LXR drum synthesiser uses a two-processor architecture. The **STM32F4** (Cortex-M4, referred to as "cortex" or "mainboard" throughout the codebase) handles all real-time audio DSP, pattern sequencing, MIDI I/O, and USB MIDI. The **ATmega644** (AVR, referred to as "front" or "front panel") drives the physical user interface: LCD, buttons, rotary encoders, LEDs, and SD card preset management.

Communication between the two processors is entirely over a single full-duplex UART link. There is no dedicated handshake line, no hardware flow control, and no shared memory. Everything that passes between the two processors — parameter changes, sequencer state, LED commands, preset data, and synchronisation signals — travels over this single byte-stream channel.

The protocol is a custom binary protocol that re-uses MIDI status byte conventions (high bit set = command/status byte, high bit clear = data byte) and a "fake SysEx" framing mechanism for bulk data transfers. The baud rate on both sides is 500,000 baud (8N1, no parity, no flow control).

---

## 2. Physical Layer and UART Configuration

### 2.1 Baud Rate Agreement

Both sides are configured for 500,000 baud.

**STM32F4** (`Uart.c`, `initFrontpanelUart()`):
```c
USART_InitStructure.USART_BaudRate = 500000UL;
```

**ATmega644** (`uart.c`):
```c
#define BAUD 500000UL
#include <util/setbaud.h>
```
The AVR side relies on `<util/setbaud.h>` with `F_CPU = 20000000UL`. At 20 MHz the 500 kbaud UBRR value divides cleanly, giving a real error of 0%. The STM32 APB1 clock must be set appropriately to divide to 500 kbaud; this is correct at the standard 42 MHz APB1 frequency. **The baud rates agree and are accurate on both sides.** This is not a problem, but the commented-out alternatives (`38400`, `76800`, `1000000`) left in both files are a maintenance hazard — if either clock rate ever changes, a developer may choose the wrong commented-out constant.

### 2.2 No Hardware Flow Control

Neither USART is configured for RTS/CTS. All flow control is software-only and relies entirely on the FIFO buffers being large enough to absorb bursts, and on the protocol-level ACK handshake being fast enough. As detailed below, neither of these assumptions is reliably met during bulk preset transfer.

### 2.3 STM32 GPIO Asymmetry (Minor Electrical Issue)

The MIDI UART (USART2) configures its TX pin as open-drain (`GPIO_OType_OD`) with pull-up, which is correct for the MIDI electrical standard. However, the front panel UART (USART3) configures its TX pin as push-pull (`GPIO_OType_PP`), while its RX pin is pulled down (`GPIO_PuPd_DOWN`). The MIDI RX pin is also pulled down. A floating RX line on power-up can generate spurious framing events. This is a hardware-level concern rather than a software one, but it affects the startup state.

---

## 3. Buffer Architecture

### 3.1 FIFO Sizes

| Side | Direction | Type | Size |
|------|-----------|------|------|
| STM32 | MIDI TX | `Fifo` | 256 bytes |
| STM32 | MIDI RX | `Fifo` | 256 bytes |
| STM32 | Front TX | `Fifo` | 256 bytes |
| STM32 | Front RX | `FifoBig` | 256 bytes |
| AVR | TX | `FifoBuffer` | 256 bytes |
| AVR | RX | `FifoBuffer` | 256 bytes |

A comment in the STM32 `Uart.c` acknowledges the larger front RX FIFO exists because "we have lots of data coming in for the preset" and marks it with a `//todo test if necessary!`. Both the regular `Fifo` and `FifoBig` are defined to be 256 bytes, making them identical in practice — the `FifoBig` naming is vestigial from an earlier design where it was intended to be larger.

Since both TX and RX buffers on both processors are only 256 bytes deep, and since a full preset load involves thousands of bytes of acknowledged sequential data, buffer overflow is a realistic failure mode (see §5 and §6).

### 3.2 FIFO Overflow is Silent

`fifo_bufferIn()` and `fifoBig_bufferIn()` both return `0` on overflow:

```c
uint8_t fifo_bufferIn(Fifo* fifo, uint8_t byte) {
    uint8_t next = ((fifo->write + 1) & BUFFER_MASK);
    if (fifo->read == next)
        return 0;   // OVERFLOW — byte silently dropped
    ...
}
```

The ISR on the STM32 side calls this unconditionally and ignores the return value:

```c
void USART3_IRQHandler(void) {
    if (USART_GetITStatus(USART3, USART_IT_RXNE) != RESET) {
        data = (uint8_t)USART_ReceiveData(USART3);
        fifoBig_bufferIn(&fifo_frontRx, data);  // return value discarded
    }
    ...
}
```

An overflow means an incoming byte is silently discarded with no error flag, no interrupt, and no way for the protocol to detect it. This corrupts the byte stream. Because the protocol has no framing recovery mechanism (no sequence numbers, no CRC, no length-prefixed packets), a single dropped byte desynchronises the parser until the next status byte with its high bit set arrives and resets the counter. For bulk sysex data, which consists entirely of 7-bit data bytes (high bit clear), a dropped byte cannot be detected and will silently corrupt sequencer pattern data.

**This is a critical bug.** Any condition that stalls the STM32 main loop for longer than one byte-time (2 µs at 500 kbaud) while bytes are arriving could cause an overflow. The STM32 processes audio in a DMA interrupt, executes the sequencer tick, and handles USB MIDI all from the same main loop thread. Any one of those tasks taking longer than it should creates a window for FIFO overflow.

### 3.3 AVR FIFO Overflow is Also Silent

The AVR's `uart_putc()` returns 0 when the TX FIFO is full. The send functions spin on this:

```c
void frontPanel_sendByte(uint8_t data) {
    while(uart_putc(data) == 0);
}
```

This is correct in the sense that it does not drop data, but it means the AVR can spin indefinitely with interrupts potentially restricted (see §4.3).

---

## 4. Blocking and Deadlock Hazards

### 4.1 `uart_waitAck()` Has No Timeout

```c
uint8_t uart_waitAck() {
    // TODO: timeout
    while(1) {
        uint8_t data;
        uint8_t ret = uart_getc(&data);
        if(ret) {
            return data;
        }
    }
}
```

The comment `// TODO: timeout` has been present since the original code. If the STM32 never sends an ACK — because it is stalled in the audio DSP, has overflowed its TX FIFO, or is handling an unrelated interrupt — the AVR locks up permanently with no recovery. There is no watchdog interaction visible in the code that would reset the system. The AVR's `__vector_default` ISR handler either loops forever (`DEBUG_CRASH_MODE`) or silently returns, so no external signal escapes either.

### 4.2 Preset Manager Spinloops

Throughout `presetManager.c` there are dozens of blocking spinloops waiting for sysex ACK callbacks from the STM32:

```c
while(frontParser_sysexCallback == NO_CALLBACK) {
    uart_checkAndParse();
}
```

And waiting for sysex handshakes:

```c
while(frontParser_midiMsg.status != SYSEX_START) {
    uart_checkAndParse();
}
```

These loops are necessary given the architecture, but they represent a design where the entire AVR UI thread is blocked for the full duration of a preset load, which on a 7-track × 8-pattern × 128-step preset involves thousands of individually ACK'd byte exchanges. During this time: button presses are not debounced, encoder reads are not processed, LEDs are not updated, and the LCD is static. This is accepted behaviour for a preset load but introduces edge cases (see §4.4).

Error conditions in these spinloops cause unconditional `while(1){}` hard locks:

```c
res = f_open(...);
if(res) {
    // print error on LCD
    while(1){;}     // device permanently locked
}
```

There are over 30 such hard-lock sites in `presetManager.c`. None of them can recover; the only path out is a hardware reset. A corrupted SD card, a card removal mid-read, or a filesystem error permanently bricks the session.

### 4.3 `ATOMIC_BLOCK` During FIFO-Full Spin

`frontPanel_sendData()` wraps its three-byte send in an `ATOMIC_BLOCK`:

```c
void frontPanel_sendData(uint8_t status, uint8_t data1, uint8_t data2) {
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        while(uart_putc(status) == 0);
        while(uart_putc(data1) == 0);
        while(uart_putc(data2) == 0);
    }
}
```

`ATOMIC_BLOCK(ATOMIC_RESTORESTATE)` disables interrupts for its duration. If any of the `uart_putc()` calls spin because the TX FIFO is full, interrupts remain disabled for the entire wait. Because the TX FIFO is drained by the `USART0_UDRE_vect` interrupt (which sends the next byte when the hardware register empties), and that interrupt is now masked, the TX FIFO can never drain. The result is a **hardware deadlock**: `uart_putc()` waits for the FIFO to have space, but the interrupt that makes space is disabled. The AVR halts permanently.

This is a latent deadlock that will occur if the AVR's 256-byte TX FIFO ever fills to capacity during a `frontPanel_sendData()` call. Given the volume of messages sent during preset loading and the lack of backpressure signalling from the STM32, this is a plausible failure mode.

### 4.4 STM32 Processes Only One Byte Per Main Loop Iteration

```c
void uart_processFront() {
    uint8_t data;
    if(fifoBig_bufferOut(&fifo_frontRx, &data)) {
        frontParser_parseUartData(data);
    }
}
```

`uart_processFront()` is called once per main loop iteration on the STM32. The STM32 main loop also calls `calcNextSampleBlock()`, `uart_processMidi()`, `seq_tick()`, `usb_tick()`, and `trigger_tick()`. At 500 kbaud a byte arrives every 20 µs. If the main loop takes longer than 20 µs — which `calcNextSampleBlock()` certainly does at 44.1 kHz audio generation — the STM32 can fall behind, leaving bytes in the hardware FIFO to be absorbed by the RX software FIFO. With only 256 bytes in the software FIFO and the AVR potentially blasting 128 × 9 = 1,152 bytes of step data in one sysex block, this FIFO can overflow before `uart_processFront()` drains it.

By contrast, the AVR calls `uart_checkAndParse()` five times per main loop iteration, which is slightly better, but the AVR's main loop also includes LCD, button, and ADC processing.

### 4.5 `frontPanel_holdForBuffer()` Race Condition

```c
void frontPanel_holdForBuffer() {
    frontPanel_wait = 1;
    frontPanel_sendByte(CALLBACK_ACK);
    while(frontPanel_wait) {
        uart_checkAndParse();
    }
}
```

This function sends `CALLBACK_ACK` (0xFD) to the STM32 and waits for the STM32 to echo it back, at which point `frontPanel_wait` is cleared. The STM32 handler is:

```c
else if(data == FRONT_CALLBACK_ACK) {
    uart_clearFrontFifo();
    uart_sendFrontpanelSysExByte(FRONT_CALLBACK_ACK);
}
```

The STM32 calls `uart_clearFrontFifo()` before sending the echo. This clears **both** the STM32's TX FIFO and RX FIFO for the front panel link. Any bytes the STM32 had already queued for transmission (e.g. LED updates from `seq_tick()` that ran concurrently) are silently discarded. Any bytes the STM32 had received but not yet processed from its RX FIFO are also discarded. This is an intentional "flush" mechanism, but it is aggressive: it destroys in-flight data on both directions without any indication to either processor.

If `holdForBuffer()` is called at an inopportune moment — such as during a burst of sequencer step-change LED updates — those LED updates are lost and the front panel display becomes incorrect. No retransmission is attempted.

---

## 5. Protocol Design Issues

### 5.1 No Framing, No Length, No Integrity Check

The protocol carries no packet length prefixes, no sequence numbers, no checksums, and no CRC. A single corrupted or dropped byte permanently desynchronises the parser until the next status byte (high bit set) appears and resets the counter. For the standard CC messages (3 bytes) this means a one-byte corruption causes one message to be lost and the next to be misinterpreted before re-sync. For sysex bulk transfers where all payload bytes have the high bit clear, a dropped byte cannot trigger any resynchronisation at all and will silently corrupt data.

### 5.2 Sysex Is Not Real SysEx

The in-code comment explicitly acknowledges this:

```c
// this is not a correct sysex implementation.
// in the front panel communication it is used to send sequencer data for preset saving
// while the preset saving is active, no other data has to be send over the uart!!!
```

The SysEx framing (0xF0 start, 0xF7 end) is borrowed from MIDI but the channel arbitration, manufacturer ID, and ACK semantics defined by the MIDI specification are absent. More importantly, the framing depends on the assumption that no other messages will be sent while sysex is active. The STM32 enforces this via `frontParser_sysexActive` — the regular `uart_sendFrontpanelByte()` is gated:

```c
void uart_sendFrontpanelByte(uint8_t data) {
    if(frontParser_sysexActive == 0) {         // silently suppressed if sysex is active
        fifo_bufferIn(&fifo_frontTx, data);
        USART_ITConfig(USART3, USART_IT_TXE, ENABLE);
    }
}
```

However, the STM32 sequencer's `seq_tick()` continues to run during preset load. It sends step-change messages, beat-pulse messages, and pattern-change messages using `uart_sendFrontpanelByte()`. All of these are silently dropped while `frontParser_sysexActive != 0`. The front panel will miss beat LEDs and step chase-light updates during a preset load. This is a known design trade-off but is undocumented and causes visible display glitches.

The more severe problem is the opposite direction: if the STM32 ever enters sysex mode without the AVR knowing (e.g. due to a desync), subsequent normal CC messages from the AVR are routed through `frontParser_handleSysexData()` instead of `frontParser_handleMidiMessage()`, causing completely wrong behaviour.

### 5.3 Sysex State Machine Can Enter Invalid States

On the STM32, `frontParser_sysexActive` starts at `0` (SYSEX_INACTIVE). When `SYSEX_START` (0xF0) is received, it transitions to `SYSEX_ACTIVE_MODE_NONE` (0x7F). The next data byte (high bit clear) sets the actual mode (e.g. `SYSEX_RECEIVE_STEP_DATA = 0x02`). When `SYSEX_END` (0xF7) is received, it is reset to `SYSEX_INACTIVE`.

The issue: `SYSEX_ACTIVE_MODE_NONE` is defined as `0x7F` on the STM32 but is a `uint8_t` initialised to `0` on declaration:

```c
uint8_t frontParser_sysexActive = 0;
```

Zero collides with `SYSEX_INACTIVE` which is also `0x00`. The `switch(frontParser_sysexActive)` in `frontParser_handleSysexData()` has no `case 0:` and falls through to `default:` when in an invalid state, where it sets the mode from the next data byte. This means that if sysex mode is incorrectly left at zero due to an unexpected `SYSEX_END`, the next sysex session will interpret its mode-selection byte correctly, but any state that was mid-transfer (partial `frontParser_sysexBuffer`, non-zero `frontParser_rxCnt`, non-zero `frontParser_sysexSeqStepNr`) is not cleared.

**`frontParser_sysexSeqStepNr` is never explicitly reset at the start of a new sysex session.** It is only reset in the `default:` case of `frontParser_handleSysexData()`. If a sysex transfer is interrupted (SYSEX_END received early, or link reset), the step counter carries over into the next transfer and causes the STM32 to write pattern data to wrong indices.

### 5.4 `SYSEX_BEGIN_PATTERN_TRANSMIT` Step Counter Scope Mismatch

The `SYSEX_BEGIN_PATTERN_TRANSMIT` mode on the STM32 increments `frontParser_sysexSeqStepNr` for each accepted 9-byte chunk. It resets and emits the "done" signal when `frontParser_sysexSeqStepNr >= 127` (i.e. at the 128th step, 0-indexed):

```c
if(frontParser_sysexSeqStepNr < 127) {
    frontParser_sysexSeqStepNr++;
    uart_sendFrontpanelSysExByte(SYSEX_STEP_ACK);
} else {
    // done with this (track, pattern) block
    uart_sendFrontpanelSysExByte(SYSEX_BEGIN_PATTERN_TRANSMIT);
}
```

This counter is **not reset** between calls to `preset_readPatternStepData()` for different (track, pattern) pairs. The caller on the AVR loops over all tracks and patterns:

```c
for(track = 0; track < NUM_TRACKS; track++) {
    for(pattern = 0; pattern < NUM_PATTERN; pattern++) {
        if(preset_workingVoiceArray & (0x01 << track)) {
            preset_readPatternStepData(track, pattern);
        }
    }
}
```

Each call to `preset_readPatternStepData()` opens a new sysex session (SYSEX_START … SYSEX_END) and sends exactly 128 steps. Each sysex START on the STM32 calls `uart_clearFrontFifo()` which clears both FIFOs but does **not** reset `frontParser_sysexSeqStepNr`. The `default:` branch (which would reset it) is only reached for the mode byte, not at the start of a new sysex session. In practice the STM32 transitions: `SYSEX_INACTIVE` → `SYSEX_ACTIVE_MODE_NONE` → `SYSEX_BEGIN_PATTERN_TRANSMIT`, and `frontParser_sysexSeqStepNr` is reset by the `default:` branch only at the mode-byte step. So for the first session `sysexSeqStepNr` starts at 0 and reaches 127, which is correct. For the second and subsequent sessions it starts at 0 again because the mode-byte `default:` branch resets it. **This appears safe in practice, but only because the mode byte always triggers the `default:` case.** This is fragile — a minor change to the sysex state machine entry could break it silently.

### 5.5 Parameter Number Offset Is Applied Asymmetrically

When the AVR sends a CC parameter message, its CC numbers start from 0. The STM32 adds 1 to correct for the MIDI standard convention (CC 0x01 = mod wheel):

```c
// fix offset between front and cortex
// front params start at 1, cortex at 2 (because of midi in mod wheel==0x1
// correct parameter number offset
frontParser_midiMsg.data1 += 1;
frontParser_midiMsg.data1 &= 0x7f;
```

And when the STM32 sends parameters back to the AVR (e.g. in `FRONT_SEQ_REQUEST_STEP_PARAMS`), it subtracts 1:

```c
// --AS **AUTOM subtract one for differing offsets when parameter is < 128
uint8_t dest = ...param1Nr;
if(dest < 128 && dest)
    dest--;
```

This +1/−1 correction is applied in some message paths but not others, and the code contains several comments marked `**AUTOM` flagging places where it must be applied carefully. The `FRONT_CC_2` path (parameters ≥ 128) does not apply the `+= 1` correction, meaning the offset convention is different for high-numbered parameters. This is a persistent source of off-by-one errors that has already required multiple patches and comments. A centrally defined and consistently applied offset constant — or better, a redesigned addressing scheme that does not require an offset at all — would be far less error-prone.

### 5.6 `MIDI_CC` (0xb0) Name Reuse Across Contexts

Both header files define `MIDI_CC = 0xb0`. On the STM32 side this is the standard MIDI control change status byte used by the external MIDI I/O parser. On the AVR side it is repurposed as the front panel's parameter update status byte. These are physically the same value travelling over completely different links, but the identical name in both header namespaces means that code which unintentionally includes the wrong header, or which is copied between the two codebases, will silently compile with the same constant meaning two different things.

Similarly, `VOICE_CC` is defined as `0xb4` in the AVR header and `0xb4` as `FRONT_CODEC_CC` in the STM32 MidiMessages.h (matching), but there is also a `PRESET_NAME` defined as `0xb4` in the AVR header, which collides with `VOICE_CC = 0xb4`:

```c
// AVR frontPanelParser.h
#define VOICE_CC       0xb4   // line 88
#define PRESET_NAME    0xb4   // line 93
```

`PRESET_NAME` and `VOICE_CC` share the same status byte on the AVR side. The parser distinguishes between them by data1 content, which is fragile and undocumented.

---

## 6. Timing and Throughput Analysis

### 6.1 Preset Load Throughput Budget

At 500 kbaud (8N1), the raw bit rate is 500,000 bits/second. Each byte requires 10 bits (1 start + 8 data + 1 stop), giving a theoretical maximum of **50,000 bytes/second**, or one byte every **20 µs**.

A full preset load via `SYSEX_BEGIN_PATTERN_TRANSMIT` involves:

- 7 tracks × 8 patterns × 128 steps × 9 bytes/step (8 payload + 1 info byte) = **64,512 bytes** from AVR to STM32
- Plus ACK bytes from STM32 to AVR: 7 × 8 × 128 = 7,168 ACK bytes

At the theoretical maximum this would take about 1.4 seconds. In practice:

- Each step requires a full round-trip ACK before the next step can be sent. The AVR sends 9 bytes (9 × 20 µs = 180 µs), then waits for the STM32 to process them through its main loop and send back a 1-byte ACK.
- The STM32 main loop processes one byte per iteration. Processing 9 bytes requires 9 iterations. Each iteration includes `calcNextSampleBlock()` (audio DSP for ~32 samples at 44.1 kHz = ~726 µs per block call), `seq_tick()`, and other tasks.
- The round-trip latency per step is dominated by STM32 processing time, not UART transmission time.

This means the effective preset transfer rate is far below the theoretical UART bandwidth and is highly dependent on the STM32 audio processing load. Any increase in DSP complexity (more voices active, more automation, more modulation) increases preset load time nonlinearly.

### 6.2 Beat LED Latency

The STM32 sequencer sends a 3-byte beat-pulse message to the front panel on every sequencer step (`seq_tick()`). At typical BPM values (60–200 BPM, 4 PPQ = 1 PPQN at 16th note resolution), step events occur every 75–250 ms. The 3-byte message takes 60 µs to transmit, but then must be drained from the AVR's RX FIFO by `uart_checkAndParse()`. The AVR calls this 5 times per main loop iteration. If the main loop is executing a long SD read or a `_delay_ms()` (several of which appear in the preset manager), the beat LED update is delayed. The `_delay_ms(2000)` on LCD error display and `_delay_ms(50)` between some sysex sessions are the worst offenders — a 2-second delay with interrupts active means the beat LED will miss multiple pulses.

### 6.3 Step Data Injection During Playback Is Unprotected

The `SYSEX_RECEIVE_STEP_DATA` and `SYSEX_BEGIN_PATTERN_TRANSMIT` handlers on the STM32 write directly to `seq_patternSet.seq_subStepPattern[][]` without any atomic protection. The `seq_tick()` function reads from the same structure on every sequencer step. On a Cortex-M4 without an OS, there are no hardware-enforced memory protection guarantees between the main loop (which processes UART data) and the timer interrupt or DMA ISR (which drives `seq_tick()`). If `seq_tick()` reads a partially-written step structure mid-update — for example, after `volume` has been written but before `note` — the synthesiser triggers a voice with a corrupted note value. The `seq_tmpPattern` double-buffer mechanism mitigates this for the non-fast-mode path, but the `seq_loadFastMode` path writes directly to the live pattern set without any locking.

---

## 7. Specific Bugs and Code Defects

### 7.1 Duplicate `#define` in AVR `uart.h`

```c
#define ACK  1
#define NACK -1
// --
#define ACK  1
#define NACK -1
```

`ACK` and `NACK` are defined twice in `uart.h`. While harmless to the compiler (both definitions are identical), it indicates copy-paste editing and would cause a warning with `-Wmacro-redefined`. The `ACK`/`NACK` values (`1` and `-1`) are also inconsistent with the protocol: `ACK` is a bare integer value, not a byte value, and `-1` is not a valid `uint8_t`. `uart_waitAck()` returns a `uint8_t`, so `NACK` cannot be returned meaningfully.

### 7.2 `END_PATTERN_NOTE_ON` Defined with No Value

In `front/LxrAvr/frontPanelParser.h`:

```c
#define END_PATTERN_NOTE_ON 0x
```

This is an incomplete macro definition — it has a `0x` prefix with no hex digits following. This will cause a preprocessor or compile error if the macro is ever expanded. It is never used in the codebase, suggesting it is dead code from an incomplete feature. A compiler would catch this only if the symbol appears in compiled code; since it does not, it is silently present as a latent defect.

### 7.3 Voice Index 6 Fallthrough Without `break`

In `frontParser_unholdVoice()` and `frontParser_uncacheVoice()`:

```c
case 6:
    voice--;    // fall through to case 5
case 5:
    seq_newVoiceAvailable |= 0x60;
    presetMask = voice6presetMask;
    break;
```

The fallthrough from `case 6` to `case 5` is intentional (the HiHat voice uses indices 5 and 6 interchangeably). However, in `uncacheVoice()`, after the `switch` statement, there is a second `switch` on the (now decremented) `voice` value that does the LFO modulation target assignment. Voice 6 enters as 6, gets decremented to 5, and the second `switch` correctly uses 5 for `hatVoice.lfo.modTarget`. But voice 6 also has `case 6` in the second switch that calls the same thing — meaning there are two separate `case 6: voice--; case 5:` patterns in the same function, one of which modifies the local variable `voice` before the second switch uses it. If the two switches fall through differently (they don't currently, but they look like they do at a glance), this is a future maintenance trap.

### 7.4 `SYSEX_RECEIVE_PAT_CHAIN_DATA` ACK Sent on Both Paths

```c
case SYSEX_RECEIVE_PAT_CHAIN_DATA:
    if(frontParser_rxCnt < 1) {
        frontParser_sysexBuffer[frontParser_rxCnt++] = data;
    } else {
        // process second byte ...
        frontParser_sysexSeqStepNr++;
        frontParser_rxCnt = 0;
    }
    uart_sendFrontpanelSysExByte(SYSEX_RECEIVE_PAT_CHAIN_DATA);  // sent on BOTH paths
    break;
```

The ACK byte is sent after both the first and second data byte of each 2-byte chain data packet. The AVR side waits for this ACK (`while(frontParser_sysexCallback != PATCHAIN_CALLBACK)`), but it sends two data bytes between ACK waits. This means the ACK for the first byte causes the AVR to incorrectly advance its loop, sending the second packet's first byte prematurely. Alternatively the AVR's two-byte send appears to pair with a single wait that catches the second ACK, but the first ACK then queues up and confuses the next iteration's wait. This causes the chain data to be written offset by one pattern. The correct behaviour is to send the ACK only after both bytes of the pair are received (i.e. only inside the `else` block).

### 7.5 `frontParser_rxDisable` Can Leave the Parser in a Broken State

The AVR `frontParser_rxDisable` flag, when set, suppresses `data2` processing:

```c
else if((frontParser_rxCnt == 0) && !frontParser_rxDisable) {
    frontParser_midiMsg.data1 = data;
    frontParser_rxCnt++;
} else if(!frontParser_rxDisable) {
    frontParser_midiMsg.data2 = data;
    frontParser_rxCnt = 0;
    // process message
}
```

If `frontParser_rxDisable` is set after `data1` has been received (i.e. when `frontParser_rxCnt == 1`), `data2` is silently discarded and `frontParser_rxCnt` is never reset to 0. The parser is now stuck with `rxCnt == 1` permanently — every subsequent `data1` byte is silently dropped and every `data2` byte is also dropped. No message will be processed until `rxDisable` is cleared AND a new status byte arrives to reset `rxCnt`. This flag is not widely used but its interaction with the parser state machine is incorrectly specified.

### 7.6 STM32 Sends Non-SysEx Bytes During Sysex Session in `seq_sendMainStepInfoToFront`

`seq_sendMainStepInfoToFront()` uses `uart_sendFrontpanelSysExByte()` (correct — bypasses the sysex gate). However, the function is called from inside `frontParser_handleSysexData()`, which is itself called from the main parser when in sysex mode. The bytes it sends are raw 7-bit data values (no status bytes), so the AVR parser's sysex handler must be in the correct state to receive them. If the AVR's sysex receive handler finishes early (e.g. because of a byte count mismatch) and the AVR sends `SYSEX_END` while the STM32 is still sending the data bytes, those trailing bytes from the STM32 are received by the AVR's normal parser after sysex mode exits and are interpreted as CC data. The result is phantom parameter changes applied to voice parameters.

### 7.7 `seq_sendMainStepInfoToFront` Sends 5 Bytes; Parser Expects 5 Bytes But `rxCnt` Threshold Is Off-By-One

The STM32 sends: 3 bytes for `mainStepData` + 1 for `length` + 1 for `scale` = 5 bytes.

The AVR parser handles this as:

```c
case SYSEX_REQUEST_MAIN_STEP_DATA:
    if(frontParser_rxCnt < 4) {       // bytes 0, 1, 2, 3
        frontParser_sysexBuffer[frontParser_rxCnt++] = data;
    } else {                           // byte 4 (fifth byte)
        frontParser_sysexBuffer[frontParser_rxCnt++] = data;
        // decode 5 bytes
    }
```

This looks correct (4 bytes buffered, 5th triggers processing), but `frontParser_rxCnt` starts at 0 (it is reset in the `default:` mode-selection path). The condition `frontParser_rxCnt < 4` is true for values 0, 1, 2, 3 — so it accumulates 4 bytes correctly. The 5th byte triggers the `else` branch. This is technically correct, but the inconsistency with the `SYSEX_REQUEST_STEP_DATA` handler (which uses `frontParser_rxCnt < 7` for a 7+1 = 8-byte frame) makes the code difficult to reason about. A unified "collect N bytes, then process" pattern with an explicit count would be far clearer.

### 7.8 `SYSEX_RECEIVE_MAIN_STEP_DATA` Index Calculation Bug

On the STM32:

```c
case SYSEX_RECEIVE_MAIN_STEP_DATA:
    ...
    uint8_t currentTrack   = (frontParser_sysexBuffer[0] >> 3) & 0x07;
    uint8_t currentPattern = (frontParser_sysexBuffer[0] & 0x07);
    uint16_t mainStepData  = frontParser_sysexBuffer[1] |
                             frontParser_sysexBuffer[2] << 7 |
                             frontParser_sysexBuffer[3] << 14;
```

This is the request path (AVR asks STM32 for data), but the `SYSEX_RECEIVE_MAIN_STEP_DATA` case is used both to receive data being sent from the AVR (preset load) and to respond to requests. The track/pattern indices derived from `sysexBuffer[0]` are correct only if the AVR encodes them the same way in the info byte (`(trkNum << 3) | patNum`). This is in fact what `preset_readPatternStepData()` does, so they match. However, the `mainStepData` reconstruction uses `<< 14` on a `uint8_t` value without a cast to `uint16_t`, which is undefined behaviour in C for shifts that overflow the operand type. The compiler will likely widen it to `int` (16-bit on AVR, 32-bit on Cortex), so in practice this probably works, but it is technically UB. The STM32 version uses the same pattern.

---

## 8. STM32 NVIC Priority Configuration

Both USART2 (MIDI) and USART3 (front panel) are configured at the **lowest** NVIC priority (PreemptionPriority = 0x0F, SubPriority = 0x0F):

```c
NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0x0F;
NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0x0F;
```

The audio DMA interrupt that drives `calcNextSampleBlock()` and `seq_tick()` will be at a higher priority and will preempt the UART ISR. If the audio DMA interrupt fires while the USART3 RX ISR is executing, the ISR is preempted. If the audio DMA handler is long enough (which it can be), and if bytes are arriving in a tight stream, the USART3 hardware receive register can overrun before the ISR runs again. A hardware overrun sets the `ORE` (Overrun Error) flag on the STM32 USART, which clears the `RXNE` flag and loses the byte. This hardware-level drop is in addition to the software FIFO overflow discussed in §3.2. The STM32 code does not check `USART_GetFlagStatus(USART3, USART_FLAG_ORE)` anywhere.

---

## 9. Potential for Mismatch Between Processor States

### 9.1 `seq_voicesLoading` Can Be Left Set

`seq_voicesLoading` is a bitmask set by `FRONT_SEQ_LOAD_VOICE` and cleared by `FRONT_SEQ_FILE_DONE` or individual `FRONT_SEQ_UNHOLD_VOICE` messages. If the preset load is interrupted (SD card error → `while(1)` hard lock on the AVR, or link desync), `seq_voicesLoading` on the STM32 remains set. All subsequent CC messages from the AVR are then silently diverted to the MIDI cache instead of being applied immediately, and live parameter control stops working until a reset or a corrective `FRONT_SEQ_FILE_DONE` message.

### 9.2 `frontParser_shownPattern` vs `seq_activePattern` Divergence

The STM32 holds `frontParser_shownPattern` as the pattern the front panel is currently *displaying*, and `seq_activePattern` as the pattern currently *playing*. These can differ during live performance. The STM32 uses `frontParser_shownPattern` to index into `seq_patternSet` when responding to most LED queries and step parameter requests. If the front panel and STM32 disagree on which pattern is shown — for example, if a `FRONT_SEQ_SET_SHOWN_PATTERN` message is lost during a busy FIFO — subsequent LED and step data updates are pulled from the wrong pattern. The user sees correct-looking LEDs for the wrong pattern. There is no periodic resync mechanism; the display remains wrong until the user manually navigates away and back.

### 9.3 `frontParser_activeTrack` and `frontParser_activeFrontTrack` Duality

Two separate variables track the active sequencer track on the STM32:
- `frontParser_activeTrack`: Set by `FRONT_SEQ_SET_ACTIVE_TRACK`, used for all parameter and automation operations.
- `frontParser_activeFrontTrack`: Set inside `frontParser_updateTrackLeds()`, used to track which track the front panel is currently displaying for LED purposes.

These can diverge if `updateTrackLeds()` is called with a different track than the one set via `SET_ACTIVE_TRACK`. There is no enforcement that they are consistent, and no message type that queries or syncs both simultaneously.

### 9.4 Inconsistent `frontParser_rxCnt` Reset on Status Byte

On the STM32, when a new status byte (high bit set) is received, `frontParser_rxCnt` is reset to 0 — but only for non-sysex-control status bytes. For `SYSEX_START`, `SYSEX_END`, `PATCH_RESET`, and `FRONT_CALLBACK_ACK`, the `rxCnt` is not reset. If these control bytes arrive in the middle of a 3-byte message (after the status byte but before data2 has been received), the `rxCnt` is left at 1. When the next normal 3-byte message arrives, the parser will treat its status byte as a new message correctly (because the high bit triggers `rxCnt = 0`), but if a data byte follows one of these control messages without an intervening status byte (which should be impossible under the protocol but can happen on desync), the message is misrouted.

---

## 10. Summary of Issues by Severity

### Critical (potential data loss, permanent lock, or silent corruption)

1. **FIFO overflow is silently dropped** — the ISR on both sides ignores the return value of `fifo_bufferIn()`. A single dropped byte during bulk transfer corrupts the entire stream with no recovery path.
2. **`ATOMIC_BLOCK` during full-FIFO spin deadlock** — if the AVR TX FIFO is full inside `frontPanel_sendData()`, the system deadlocks permanently. Interrupts are disabled while waiting for a condition that requires an interrupt to resolve.
3. **`uart_waitAck()` has no timeout** — if the STM32 never replies, the AVR hangs forever.
4. **Over 30 unconditional `while(1){}` hard-locks** on SD card errors in `presetManager.c` — any filesystem error permanently bricks the session.
5. **`seq_loadFastMode` writes directly to live pattern data without atomic protection** — `seq_tick()` can read a partially written step structure, causing glitched voice triggers.

### High (incorrect behaviour, data corruption, display errors)

6. **`SYSEX_RECEIVE_PAT_CHAIN_DATA` sends ACK on both half-message paths** — the first byte triggers a premature ACK, advancing the AVR's loop out of sync with the STM32's receive state.
7. **`uart_clearFrontFifo()` on CALLBACK_ACK discards queued outbound data** — LED updates and response messages queued by `seq_tick()` are silently lost.
8. **`seq_sendMainStepInfoToFront` UB: left-shifting a `uint8_t` by 14** — undefined behaviour; result depends on compiler width decisions.
9. **`frontParser_sysexSeqStepNr` not reset on sysex session open** — only reset by the mode-byte `default:` branch; a state machine change could break this silently.
10. **`frontParser_rxDisable` mid-message leaves parser stuck at `rxCnt == 1`** — parser deadlocked until a full status+data pair arrives.
11. **STM32 USART hardware overrun not checked** — `ORE` flag silently drops bytes before the software FIFO even sees them; this is in addition to the software FIFO overflow.

### Medium (protocol fragility, maintenance hazard)

12. **Parameter number offset (+1/−1) applied inconsistently** across CC paths — high-numbered parameters (≥ 128) do not use the offset, creating a divergent addressing scheme in different parts of the same protocol.
13. **`MIDI_CC` (0xb0) repurposed on AVR side** — same constant name and value means different things on each side of the link; copying code between them will silently miscompile.
14. **`PRESET_NAME` and `VOICE_CC` both defined as 0xb4** in AVR header — silent collision; current code paths happen not to conflict but future additions will.
15. **`uart_sendFrontpanelByte()` silently suppressed during sysex** — STM32 sequencer LED messages are dropped without any indication; front panel display is incorrect during preset load.
16. **Only one byte processed per main loop iteration on STM32** — at 500 kbaud and with audio DSP sharing the main loop, the STM32 cannot keep pace with peak AVR output, creating pressure towards RX FIFO overflow.
17. **`_delay_ms(2000)` on error path with interrupts enabled** — beats and step events are missed; front panel display glitches during error display.

### Low (code quality, maintenance hazard)

18. **`FifoBig` is identical to `Fifo`** — the "big" naming is vestigial and misleading.
19. **Commented-out baud rate alternatives** in both `Uart.c` and `uart.c` — creates confusion if clock rates change.
20. **`END_PATTERN_NOTE_ON` defined as `0x`** (incomplete hex literal) — latent compile error if ever referenced.
21. **Duplicate `#define ACK 1` / `#define NACK -1`** in AVR `uart.h`.
22. **`ACK` / `NACK` values are integer constants, not byte values** — `uart_waitAck()` returns a `uint8_t`; `NACK = -1` cannot be returned and is not a valid protocol signal.
23. **Voice 6 `case` fallthrough** in uncache/unhold functions is fragile and undocumented; the second switch does not mirror the first correctly.
24. **No protocol versioning** — there is no handshake at startup to confirm both processors are running compatible firmware, meaning a firmware mismatch on one side fails silently with wrong behaviour.
25. **`//TODO test if necessary!`** comment on the FifoBig declaration has never been resolved.

---

## 11. Refactoring and Improvement Recommendations

### 11.1 Implement Hardware Flow Control or a Credit-Based Scheme

The most impactful single change would be to introduce a software-level credit counter. Before sending a burst, the AVR should query the STM32's available receive buffer space. A simple "I have N bytes of free space" message sent periodically by the STM32 would allow the AVR to throttle its output without spinning. Alternatively, configuring RTS/CTS on the USART hardware (both chips support it) would provide this automatically.

### 11.2 Add Timeout to All Blocking Waits

Every `while(condition) { poll; }` loop that depends on the remote processor should have a deadline based on the AVR's hardware timer:

```c
uint16_t timeout = time_getMs() + COMM_TIMEOUT_MS;
while(frontParser_sysexCallback == NO_CALLBACK) {
    uart_checkAndParse();
    if((int16_t)(time_getMs() - timeout) >= 0) {
        // timeout: abort, set error state, return failure
        return PRESET_ERR_TIMEOUT;
    }
}
```

The AVR has a `timebase` module; it should be used here. Timeouts should propagate up to the caller and be surfaced to the user on the LCD.

### 11.3 Fix the `ATOMIC_BLOCK` Deadlock

`frontPanel_sendData()` must not spin inside an `ATOMIC_BLOCK`. The solution is to remove the atomic wrapper and instead ensure that every 3-byte message is treated as logically atomic at the protocol level: because the protocol uses status bytes to begin each message, a status byte from the sequencer timer ISR can always be detected and properly reset the parser. If true atomicity is required for a 3-byte sequence, the correct approach is to disable only the UART TX interrupt (which shares the FIFO), not all interrupts:

```c
UCSR0B &= ~(1 << UDRIE0);   // disable TX empty ISR
uart_putc(status);
uart_putc(data1);
uart_putc(data2);
UCSR0B |= (1 << UDRIE0);    // re-enable
```

This prevents the ISR from racing the FIFO writes without risking interrupt starvation.

### 11.4 Introduce Message Framing with Length and Checksum

For bulk transfers, each packet should carry a length byte and a simple XOR or CRC-8 checksum. This allows the receiver to detect corruption and request a retransmit. This does not require a full protocol redesign — the sysex framing already provides a natural packet boundary; adding a length byte at the start and a checksum byte at the end of each sysex frame would be sufficient.

### 11.5 Centralise the Parameter Offset and Make It Explicit

Replace the scattered `+= 1` / `-= 1` corrections with a single header-defined macro applied in one place:

```c
#define FRONT_TO_CORTEX_PARAM(x)  ((uint8_t)(((x) + 1) & 0x7f))
#define CORTEX_TO_FRONT_PARAM(x)  ((uint8_t)(((x) - 1) & 0x7f))
```

And define the high-parameter (CC2) path explicitly to use no offset, documenting why.

### 11.6 Reset `frontParser_sysexSeqStepNr` Explicitly on Sysex Open

In `frontParser_parseUartData()`, when `SYSEX_START` is received:

```c
if(data == SYSEX_START) {
    frontParser_sysexActive = SYSEX_ACTIVE_MODE_NONE;
    frontParser_sysexSeqStepNr = 0;   // add this
    frontParser_rxCnt = 0;             // add this
    uart_clearFrontFifo();
    uart_sendFrontpanelSysExByte(SYSEX_START);
}
```

This makes the state machine robust against partial sessions.

### 11.7 Fix `SYSEX_RECEIVE_PAT_CHAIN_DATA` ACK Placement

Move the ACK send to inside the `else` block only:

```c
case SYSEX_RECEIVE_PAT_CHAIN_DATA:
    if(frontParser_rxCnt < 1) {
        frontParser_sysexBuffer[frontParser_rxCnt++] = data;
    } else {
        // ... process two-byte pair ...
        frontParser_rxCnt = 0;
        uart_sendFrontpanelSysExByte(SYSEX_RECEIVE_PAT_CHAIN_DATA);  // only here
    }
    break;
```

### 11.8 Replace Hard-Lock Error Paths with Recoverable Error States

Replace every `while(1){}` in `presetManager.c` with a structured error return:

```c
if(res) {
    preset_showError(PSTR("SD open err"), res);
    return PRESET_ERR_IO;
}
```

The caller should handle the error, potentially retrying or returning to the main menu. The `while(1){}` traps are reasonable for debugging but must not ship in production firmware.

### 11.9 Add USART Overrun Detection on STM32

In `USART3_IRQHandler()`:

```c
if(USART_GetFlagStatus(USART3, USART_FLAG_ORE)) {
    USART_ClearFlag(USART3, USART_FLAG_ORE);
    // increment an error counter, set a flag
    comm_overrunCount++;
}
```

This at minimum gives visibility into the problem and can inform backpressure logic.

### 11.10 Add a Startup Handshake

On power-up, before either processor begins normal operation, a version exchange should be performed:

1. AVR sends `COMM_HELLO` + firmware version byte.
2. STM32 responds with its firmware version byte.
3. If versions are incompatible, both sides display an error and refuse to operate.

This prevents a firmware mismatch from causing silent, inexplicable misbehaviour.

---

## Appendix A: Quick Reference — Protocol Status Bytes

| Value | AVR Symbol | STM32 Symbol | Direction | Description |
|-------|-----------|--------------|-----------|-------------|
| 0xAA | `MACRO_CC` | `FRONT_CC_MACRO_TARGET` | AVR→STM32 | Macro modulator change |
| 0xAC | `MORPH_CC` | — | AVR→STM32 | Voice morph |
| 0xAD | `BANK_CHANGE_CC` | — | STM32→AVR | Bank change |
| 0xAE | `PARAM_CC` | — | STM32→AVR | Parameter update (bc) |
| 0xAF | `PARAM_CC2` | — | STM32→AVR | Parameter update >127 (bc) |
| 0xB0 | `MIDI_CC` | `MIDI_CC` | AVR→STM32 | Parameter CC (reuses MIDI CC) |
| 0xB1 | `LED_CC` | `FRONT_STEP_LED_STATUS_BYTE` | Both | LED control |
| 0xB2 | `SEQ_CC` | `FRONT_SEQ_CC` | Both | Sequencer commands |
| 0xB4 | `VOICE_CC` / `PRESET_NAME` ⚠️ | `FRONT_CODEC_CC` | Both | Voice/preset name (**collision**) |
| 0xB5 | `SET_BPM` / `PRESET` ⚠️ | `FRONT_SET_BPM` | AVR→STM32 | BPM / preset (**collision**) |
| 0xB6 | `CC_2` | `FRONT_CC_2` | AVR→STM32 | High CC (≥128) |
| 0xB7 | `CC_LFO_TARGET` | `FRONT_CC_LFO_TARGET` | AVR→STM32 | LFO target |
| 0xB8 | `CC_VELO_TARGET` | `FRONT_CC_VELO_TARGET` | AVR→STM32 | Velocity mod target |
| 0xB9 | `STEP_CC` | `FRONT_STEP_CC` | AVR→STM32 | Toggle substep |
| 0xBA | `SET_P1_DEST` | `FRONT_SET_P1_DEST` | Both | Step param 1 destination |
| 0xBE | `MAIN_STEP_CC` | `FRONT_MAIN_STEP_CC` | AVR→STM32 | Toggle main step |
| 0xC0 | `SAMPLE_CC` | `SAMPLE_CC` | Both | Sample management |
| 0xF0 | `SYSEX_START` | `SYSEX_START` | Both | SysEx begin |
| 0xF7 | `SYSEX_END` | `SYSEX_END` | Both | SysEx end |
| 0xFD | `CALLBACK_ACK` | `FRONT_CALLBACK_ACK` | Both | Buffer flush / sync |
| 0xFE | `PATCH_RESET` | `PATCH_RESET` | STM32→AVR | Reset voice params |

---

## Appendix B: Key Variable Cross-Reference

| Variable | Side | Meaning | Risk |
|----------|------|---------|------|
| `frontParser_sysexActive` | STM32 | Current sysex transfer mode | Not reset on SYSEX_START; step counter not reset |
| `frontParser_sysexSeqStepNr` | STM32 | Step counter in bulk transfer | Not explicitly reset per session |
| `frontParser_rxCnt` | Both | Byte counter within 3-byte message | Left at 1 if rxDisable set mid-message |
| `seq_voicesLoading` | STM32 | Bitmask of voices being loaded | Left set on preset load abort |
| `frontParser_shownPattern` | STM32 | Pattern the front panel shows | Can diverge from AVR's view silently |
| `frontParser_activeTrack` | STM32 | Active track for param ops | Separate from `frontParser_activeFrontTrack` |
| `frontPanel_wait` | AVR | Blocking flag for holdForBuffer | Cleared by STM32 CALLBACK_ACK only |
| `frontPanel_longOp` | AVR | Pending slow operation cache | PATTERN_CHANGE_OP path does nothing |
| `midi_midiCacheAvailable[]` | STM32 | Flags for cached MIDI params | Not cleared after uncache (commented out) |

*Note: the commented-out cache-clearing lines in `frontParser_uncacheVoice()` mean the cache available flags are never cleared after a voice is released. Subsequent preset loads will find stale flags and incorrectly skip loading parameters that were already written, unless the whole kit is replaced.*

---

---

## 12. Baud Rate Headroom Analysis

The current 500 kbaud rate is not the theoretical maximum for either processor, and increasing it is a viable way to reduce round-trip latency during preset transfers and tight timing windows. However, the achievable rates are constrained by the AVR's clock, and a baud rate increase alone is not sufficient without also addressing the processing bottlenecks identified above.

### 12.1 Achievable Rates

The AVR (20 MHz, U2X=1) can only hit exact baud rates at discrete UBRR values. The STM32 (USART3 on APB1 = 42 MHz) uses a fractional BRR divider and can hit almost any target; the AVR is always the limiting side.

The "clock error" columns express how far each processor's hardware divider deviates from the nominal target baud rate, as a percentage. It is **not** a bit error rate — it is a clock accuracy figure. UART has no shared clock; the receiver samples each bit at the centre of its expected window, so if the transmitter runs slightly fast or slow, the sampling point drifts across the 10-bit frame (start + 8 data + stop). The practical rule is that the **combined** clock error from both ends must stay below approximately **4–5%** for reliable 8N1 reception. Both figures in the table are the per-device deviation; add them for the worst-case combined drift.

| Rate | AVR UBRR | AVR clock error | STM32 BRR | STM32 clock error | Combined | Notes |
|------|----------|-----------------|-----------|-------------------|----------|-------|
| 500 k *(current)* | 4 | 0% | 5 + 4/16 | 0% | **0%** | Divides cleanly into both clocks |
| 625 k | 3 | 0% | 67 (integer) | +0.3% | **0.3%** | Drop-in, lowest risk |
| 1.25 M | 1 | 0% | 2 + 1/16 (fractional) | −0.06% | **0.06%** | 2.5× faster, excellent accuracy |
| 2.5 M | 0 | 0% | 17 (integer) | +0.6% | **0.6%** | Signal integrity risk |

The 4–5% combined budget means all four rates are well within tolerance in isolation. The current 500 kbaud divides exactly into both the AVR's 20 MHz oscillator and the STM32's 42 MHz APB1 clock — which is the main reason it was a sensible original choice. 2.5 Mbaud leaves very little margin for cable capacitance or PCB trace length on a noisy embedded board.

### 12.2 Candidate Assessment

**625 kbaud** is the safest first step. It requires changing one constant on each side (`BAUD` on the AVR, `USART_BaudRate` on the STM32) and nothing else. The 0.3% STM32 error is negligible. Risk is minimal.

**1.25 Mbaud** is the most attractive target. The AVR hits it exactly (UBRR = 1) and the STM32 achieves near-zero error using its fractional divider. At 8 µs/byte instead of 20 µs, the raw transmission time for one acknowledged step packet (9 bytes out + 1 byte ACK) drops from ~200 µs to ~80 µs. The one implementation caveat is that the STM32's `USART_Init()` helper performs integer arithmetic when computing the BRR value and will choose the wrong divisor at this rate — the BRR register should be written directly:

```c
// APB1 = 42 MHz, target = 1.25 Mbaud
// BRR = 42000000 / (16 * 1250000) = 2.1 → mantissa=2, fraction=1 (1/16)
USART3->BRR = (2 << 4) | 1;
```

**2.5 Mbaud** is not recommended without hardware review. The PCB trace routing, connector, and inter-board cable between the STM32 and AVR were designed for 500 kbaud. At 2.5 Mbaud, edge rates and cable impedance become relevant and the 0.6% clock error, while within spec, leaves almost no margin for any physical-layer degradation.

### 12.3 Baud Rate Is Not the Actual Bottleneck

The dominant latency in the current protocol is not transmission time — it is the STM32 processing only one byte per `uart_processFront()` call while audio DSP shares the same main loop. If the main loop iteration takes 700 µs (one audio block at 44.1 kHz, 32 samples), bytes arriving every 8 µs at 1.25 Mbaud will saturate the 256-byte RX FIFO in about 2 ms. Increasing the baud rate without also increasing the drain rate **raises the probability of FIFO overflow** rather than lowering it.

The fix on the STM32 side is straightforward: drain all available bytes in `uart_processFront()` rather than just one:

```c
void uart_processFront() {
    uint8_t data;
    while(fifoBig_bufferOut(&fifo_frontRx, &data)) {
        frontParser_parseUartData(data);
    }
}
```

This is safe because `frontParser_parseUartData()` is a pure state machine with no unbounded loops of its own. Draining the full FIFO per call eliminates the per-byte main-loop-iteration penalty entirely and makes the actual round-trip time a function of STM32 response latency rather than FIFO drain rate.

### 12.4 Recommended Sequence

1. **Fix correctness bugs first** (§7 items 1–5: FIFO overflow handling, `ATOMIC_BLOCK` deadlock, `uart_waitAck()` timeout, SD error hard-locks, `seq_loadFastMode` atomicity). A faster baud rate with these bugs present amplifies their severity.
2. **Fix the single-byte drain** on the STM32 (`uart_processFront()` while-loop). This alone may reduce preset load time more than any baud rate change.
3. **Bump to 625 kbaud** as a conservative, low-risk improvement.
4. **Evaluate 1.25 Mbaud** once the protocol is stable and correct. At that point the lower per-byte time translates directly to reduced round-trip latency and measurable preset load speed improvement.
5. **Leave 2.5 Mbaud** as a future option pending hardware signal integrity verification.

*Audit completed. Working tree extracted and verified at `/home/claude/LXR_project/LXR-custom-develop-patload-envmod`.*
