# AVR Hardware Reference — ATmega644 (LXR Front Panel Controller)

## 1. Overview

The front panel of the LXR drum synthesiser is managed by an **ATmega644** running at **20 MHz** from an external HC-49V crystal (X1, BOM). The ATmega644 is a member of the megaAVR family: 8-bit Harvard architecture, single-cycle ALU, 64 KB Flash, 4 KB SRAM, 2 KB EEPROM, and a rich complement of on-chip peripherals. Its sole job is human-interface management: it reads buttons, drives LEDs, reads the encoder and four potentiometers, drives the 2×16 display, manages the SD card, and exchanges high-level control messages with the STM32F4 mainboard over a 500 kBaud UART link.

Firmware version as of the audited codebase: **0.37** (`front/LxrAvr/main.c`).

---

## 2. MCU Specifications

| Parameter | Value |
|---|---|
| Part number | ATmega644 / ATmega644A (DIP-40) |
| Architecture | 8-bit AVR (Harvard, RISC) |
| Flash | 64 KB (in-system programmable) |
| SRAM | 4 KB |
| EEPROM | 2 KB |
| Clock source | External 20 MHz crystal (HC-49V, C5=22 pF, C6=22 pF load caps) |
| Supply voltage | 5 V regulated by UA7805 on the control board |
| Compiler target | `-mmcu=atmega644` (avr-gcc) |
| Optimisation flag | `-Os` (size optimise) |
| `F_CPU` define | `20000000UL` |

---

## 3. Pin Allocation by Port

### Port A — Not Used by Firmware
Port A has no connections defined in the audited source. It appears to remain as unused I/O and should be configured as inputs with pull-ups disabled to avoid floating-pin current (the compiler default leaves them as inputs).

### Port B — LCD Data, SPI (SD Card)

| Pin | Direction | Function |
|---|---|---|
| PB0 | Output | LCD data bit DB4 (`LCD_DB = PB0`) |
| PB1 | Output | LCD data bit DB5 |
| PB2 | Output | LCD data bit DB6 |
| PB3 | Output | LCD data bit DB7 |
| PB4 | Output | SPI /SS → SD card chip-select |
| PB5 | Output | SPI MOSI → SD card |
| PB6 | Input  | SPI MISO ← SD card |
| PB7 | Output | SPI SCK → SD card |

**Notes on PB0 / CORTEX_RESET conflict:** `main.c` explicitly configures `PB0` as an input (`DDRB &= ~(1<<CORTEX_RESET_PIN)`) during early boot to monitor a Cortex reset line. `lcd_init()` is called immediately afterward and reconfigures PB0–PB3 as outputs for the 4-bit LCD interface. In normal operation this resolves itself in program order, but the two usages share the same physical pin. The Cortex reset monitor is therefore only valid before `lcd_init()` runs.

### Port C — Encoder, LCD Control, 74HC165 Shift-Register Input Bus

| Pin | Direction | Function |
|---|---|---|
| PC0 | Input (pull-up) | Encoder phase A (`PHASE_A`) |
| PC1 | Input (pull-up) | Encoder phase B (`PHASE_B`) |
| PC2 | Input (pull-up) | Encoder push-button (`ENCODER_BUTTON`) |
| PC3 | Output | LCD Enable strobe (`LCD_EN`) |
| PC4 | Output | LCD Register-Select (`LCD_RS`; 1=data, 0=command) |
| PC5 | Output | 74HC165 Parallel Load (`DIN_LOAD_PIN`) — LOW loads parallel inputs, HIGH enables clocking |
| PC6 | Output | 74HC165 Clock (`DIN_CLK_PIN`) — shift on rising edge |
| PC7 | Input  | 74HC165 Serial Output (`DIN_INPUT_PIN`) — serial data from the chain |

### Port D — UART, 74HC595 Shift-Register Output Bus

| Pin | Direction | Function |
|---|---|---|
| PD0 | Input  | USART0 RXD ← STM32F4 front-panel UART TX |
| PD1 | Output | USART0 TXD → STM32F4 front-panel UART RX |
| PD2–PD3 | — | Unused / available |
| PD4 | Output | 74HC595 Storage Register Clock / Latch (`DOUT_LOAD_PIN`) — rising edge outputs to pins |
| PD5 | Output | 74HC595 Shift Clock (`DOUT_CLK_PIN`) — rising edge shifts data in |
| PD6 | Output | 74HC595 Serial Input / Data Out (`DOUT_OUTPUT_PIN`) |
| PD7 | — | Unused |

---

## 4. Hardware Peripheral Usage

### 4.1 USART0 — Front Panel ↔ STM32F4 Link

The USART is the primary communication channel between the AVR front panel and the STM32F4 audio/sequencer mainboard.

| Parameter | Value |
|---|---|
| Baud rate | **500,000 baud** |
| Format | 8N1 (8 data bits, no parity, 1 stop bit) |
| Double-speed mode (U2X0) | Enabled if required by `<util/setbaud.h>` |
| Direction | Full-duplex |
| Interrupts | `USART0_RX_vect` (RX complete), `USART0_UDRE_vect` (TX data register empty) |
| TX buffer | Software FIFO (`uart_txBuffer`, type `FifoBuffer`) |
| RX buffer | Software FIFO (`uart_rxBuffer`, type `FifoBuffer`) |

Transmit is interrupt-driven: `uart_putc()` places a byte in the TX FIFO and enables `UDRIE0`; the `UDRE` ISR drains the FIFO and self-disables when empty. Receive is also interrupt-driven: the `RX_vect` ISR stuffs each byte directly into `uart_rxBuffer`. The main loop calls `uart_checkAndParse()` repeatedly (four times per iteration) to drain the RX FIFO through `frontPanel_parseData()`.

### 4.2 SPI — SD Card

The hardware SPI peripheral is used exclusively for the SD card (FAT filesystem via elm-chan FatFs).

| Parameter | Value |
|---|---|
| Role | Master |
| Initial clock | F_CPU / 64 = **312.5 kHz** (SD card initialisation requirement) |
| Mode | CPOL=0, CPHA=0 (Mode 0) |
| Bit order | MSB first |
| SPCR | `(1<<SPE)|(1<<MSTR)|(1<<SPR1)` |
| Data width | 8 bits |

After SD initialisation the firmware typically switches to a higher SPI clock, though the exact post-init frequency is controlled by the FatFs / SD layer calling `spi_init()` once at startup.

The SD card is connected to Port B (PB4–PB7 as described above). The `spi_deInit()` function tri-states the SPI pins when the SD card is not in use or unavailable.

### 4.3 ADC — Potentiometers

Four 10 kΩ linear potentiometers (POT1–POT4) feed into ADC channels 0–3.

| Parameter | Value |
|---|---|
| Reference | AVCC (`REFS0=1`, single-ended) |
| Prescaler | /64 → ADC clock = **312.5 kHz** (within the 50–200 kHz recommended range) |
| Resolution | 10-bit (ADCW read) |
| Channels | 0, 1, 2, 3 |
| Averaging | 4 samples per reading (`adc_readAvg(..., 4)`) |
| Hysteresis | ±3 LSBs (prevents jitter-triggered updates) |
| Mode | Polling (no ADC interrupt used) |

The ADC result is right-shifted by 2 before being passed to the menu system (`menu_parseKnobValue(i, newValue >> 2)`), reducing it to 8-bit range.

### 4.4 74HC165 Shift Registers — Button / Switch Inputs

Five 74HC165 8-bit parallel-in/serial-out shift registers are chained in series to read **39 buttons** (NUM_INPUTS = 39). The interface is fully bit-banged on Port C.

| Signal | Pin | Description |
|---|---|---|
| PL (Parallel Load) | PC5 | LOW → latch parallel inputs; HIGH → enable shifting |
| CP (Clock) | PC6 | Shift on rising edge |
| Q7 (Serial out) | PC7 | Data bit into the AVR |

**Protocol:** `din_load()` pulses PL low-then-high to capture the switch state into all five register banks simultaneously. `din_readNextInput()` then calls `din_shift()` ten times per main-loop iteration, clocking out one bit at a time, comparing each bit against the stored `din_inputData[]` array to detect press and release edges, and calling `buttonHandler_buttonPressed()` / `buttonHandler_buttonReleased()` on changes.

Input polarity: a pressed button reads as **0** (active-low, confirmed by the code inverting the raw pin state).

### 4.5 74HC595 Shift Registers — LED Outputs

Five 74HC595 8-bit serial-in/parallel-out shift registers drive **40 LED outputs** (NUM_OUTS = 40, one spare channel). The interface is bit-banged on Port D.

| Signal | Pin | Description |
|---|---|---|
| RCLK / ST_CP (Latch) | PD4 | Rising edge transfers shift register to output latches |
| SHCP / SH_CP (Clock) | PD5 | Rising edge shifts data in |
| SER (Serial in) | PD6 | Data bit from AVR |

`dout_updateOutputs()` iterates all 40 output bits from `dout_outputData[]`, places each bit on PD6, and generates a clock pulse on PD5. After all 40 bits are clocked in it pulses PD4 to latch the values to the LED driver outputs. A 220 Ω resistor network (RR1–RR5) limits LED current.

A software brightness/PWM mode (`USE_BRIGHTNESS`) is present in the source but compiled out by default (`#define USE_BRIGHTNESS 0`).

### 4.6 LCD — 2×16 HD44780-Compatible Display

The display is driven in **4-bit mode** with the upper nibble of the data bus on PB0–PB3.

| Signal | AVR Pin | Description |
|---|---|---|
| DB4 | PB0 | Data bit 4 |
| DB5 | PB1 | Data bit 5 |
| DB6 | PB2 | Data bit 6 |
| DB7 | PB3 | Data bit 7 |
| RS | PC4 | Register select (1=data, 0=instruction) |
| EN | PC3 | Enable strobe (active-high pulse) |

The BOM specifies a **2×16 OLED** (NHD-0216KZW-AG5) as default, with optional LCD back-compatibility. An optional contrast trimmer (VR1, 10 kΩ) and backlight resistor (R51) exist for LCD-only users; OLED users leave these unpopulated.

Timing constants from `lcd.h`: enable pulse = 30 μs; write data = 56 μs; command = 52 μs; soft-reset phases = 50 ms each.

The `lcd_string_F()` function reads strings from Flash using `pgm_read_byte()`, conserving SRAM.

### 4.7 Encoder

A single incremental encoder with push-button (ENC1, 24 clicks per revolution, 25 mm shaft) is connected to Port C.

| Signal | Pin | Pull-up | Description |
|---|---|---|---|
| A | PC0 | Internal | Quadrature phase A |
| B | PC1 | Internal | Quadrature phase B |
| Button | PC2 | Internal | Encoder push-button (active-low) |

The encoder driver uses **Timer1 compare A** to sample both phases at 16 kHz. The physical detent rest phase is hardware-verified as `AB=11`, encoded in firmware as `ENCODER_REST_STATE = 0x03`. The ISR applies a two-sample symmetric phase filter, runs a rest-phase anchored quadrature FSM, and accumulates only complete detents into `enc_delta`. The main loop reads rotation only through `encode_stableRead4()`. Legacy one-step/two-step/read-wrapper modes, PCINT decoding, and Timer0 encoder sampling are intentionally not supported. Encoder button debouncing is handled in the same Timer1 ISR with a 48-sample integrator.

---

## 5. External Connections to the STM32F4 Mainboard

The control board connects to the STM32F4 audio mainboard via two 2×13 female headers (P1, P2) plus a 2×10 expansion header (P4). The primary data link is:

- **UART link**: AVR PD1 (TX) → STM32 USART3 RX (PB11); STM32 USART3 TX (PB10) → AVR PD0 (RX). 500 kBaud, 8N1.

The AVR monitors a `CORTEX_RESET_PIN` (PB0, configured as input at startup) which can be driven by the STM32F4 to trigger a controlled reset of the AVR if needed.

---

## 6. Power Supply

The control board generates its own regulated 5 V supply from a 7–9 V DC input (J6):

- **D1** (1N4001): reverse-polarity protection
- **U1** (UA7805, TO-220 with heatsink): linear 5 V regulator
- **C1, C3** (220 μF / 25 V): bulk PSU smoothing
- **C2, C4** (100 nF): local decoupling
- **L1** (10 μH): LC-filter inductor on the ADC supply rail to reduce switching noise on the potentiometer readings

The optocoupler (OK1, 6N138) provides galvanic isolation of the MIDI DIN input ground from the synth ground. D2 (1N4148) protects the optocoupler input.

---

## 7. MIDI DIN Interface

MIDI IN is received by the **6N138 optocoupler** (OK1) on the bottom of the PCB and connected to the STM32F4 USART2 RX pin (PA3). MIDI OUT is driven directly by the STM32F4 USART2 TX pin (PA2) through R3 (220 Ω). The AVR itself does not process MIDI DIN; all DIN MIDI is handled by the STM32F4.
