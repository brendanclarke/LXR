# AVR Setup & Resource Allocation — ATmega644 Front Panel

## 1. Overview

This document details the timing, interrupt, clock, and memory resource allocation in the ATmega644 front panel firmware. The ATmega644 runs at **20 MHz** from an external crystal with no prescaler (CLKPS fuse bits = default divide-by-1). All timing calculations below assume F_CPU = 20,000,000 Hz.

---

## 2. Clock Configuration

### 2.1 System Clock

| Parameter | Value |
|---|---|
| Source | External crystal, HC-49V |
| Frequency | 20 MHz |
| Startup fuse | Default AVR crystal oscillator fuses |
| CPU clock (clkCPU) | 20 MHz (no prescaler) |
| Peripheral clock (clkIO) | 20 MHz |
| `F_CPU` define | `20000000UL` |

The crystal is loaded by C5 and C6 (22 pF each), matching the HC-49V crystal's specified load capacitance.

---

## 3. Timer Allocation

The ATmega644 has three hardware timers: Timer0 (8-bit), Timer1 (16-bit), and Timer2 (8-bit). All three are used.

### 3.1 Timer0 — Encoder Sampling (~1 ms CTC ISR)

**Source:** `encoder.c`, `encode_init()`

| Parameter | Value | Calculation |
|---|---|---|
| Mode | CTC (Clear Timer on Compare Match A) | `WGM01 = 1` |
| Prescaler | /256 | `CS02 = 1` |
| Timer clock | 78,125 Hz | 20 MHz / 256 |
| OCR0A value | 78 | `F_CPU / 256.0 * 0.001` ≈ 78.125 → 78 |
| Actual ISR frequency | **1001.64 Hz** ≈ 1 ms period | 78,125 / (78+1) |
| Interrupt | `TIMER0_COMPA_vect` |

**ISR function:** `ISR(TIMER0_COMPA_vect)` — Reads both encoder phases (PC0/PC1) using the Gray-code to binary conversion algorithm. Computes the signed delta (`enc_delta`) accumulated since last read. Also samples the encoder button (PC2) and compares with the previous state to debounce it. This ISR is the only place the encoder state machine runs; the main loop merely reads and clears `enc_delta` atomically.

**Timing note:** The 1 ms period gives adequate encoder resolution. At 24 clicks/revolution the mechanical detent period at moderate hand speed (~2 rev/sec) is ~20 ms per click, so 20 samples per detent = good anti-bounce margin.

### 3.2 Timer2 — System Timebase (Overflow ISR)

**Source:** `Hardware/timebase.c`, `time_initTimer()`

| Parameter | Value | Calculation |
|---|---|---|
| Mode | Normal (overflow) | No WGM bits set |
| Prescaler | /1024 | `CS22=1, CS21=1, CS20=1` |
| Timer clock | 19,531.25 Hz | 20 MHz / 1024 |
| Overflow period | **13.1072 ms** | 256 / 19531.25 |
| ISR frequency | **76.29 Hz** | 19531.25 / 256 |
| Interrupt | `TIMER2_OVF_vect` |

**ISR function:** `ISR(TIMER2_OVF_vect)` — Increments two global counters:
- `volatile uint16_t time_sysTick` — 16-bit system tick counter. Rolls over every 65535 × 13.1072 ms ≈ **14.3 minutes**. Used for coarse time references.
- `screensaver_timer` — dedicated screensaver tick counter (defined in `screensaver.c`), compared against a threshold to activate the display screensaver after inactivity.

**Note:** Timer1 (16-bit) is **not used** by the main application firmware. It is available for future expansion.

---

## 4. Interrupt Allocation

The ATmega644 uses a vectored interrupt controller. The following vectors are actively used:

| Vector | Source | ISR | Period / Trigger |
|---|---|---|---|
| `TIMER0_COMPA_vect` | Timer0 Compare Match A | Encoder sampling | ~1 ms |
| `TIMER2_OVF_vect` | Timer2 Overflow | System tick + screensaver tick | ~13.1 ms |
| `USART0_RX_vect` | UART RX Complete | Store byte in `uart_rxBuffer` FIFO | Per received byte at 500 kBaud |
| `USART0_UDRE_vect` | UART TX Data Register Empty | Drain `uart_txBuffer` FIFO or disable interrupt | Per transmitted byte |
| `__vector_default` | All unhandled vectors | Optional crash display (debug mode) | On unhandled interrupt |

**Interrupt enable:** The main loop calls `sei()` only after all hardware is initialised (LCD, UART, ADC, encoder, DIN, DOUT, LED, timer). SD card and preset initialisation happen after `sei()` since they require UART interrupts for communication with the STM32F4.

**Atomicity:** Where the main loop reads `enc_delta`, it disables interrupts with `cli()`, reads and clears the value, then restores with `sei()`:
```c
cli();
val = enc_delta;
enc_delta = val & 3;   // preserve remainder bits
sei();
```

---

## 5. UART Timing and Throughput

| Parameter | Value |
|---|---|
| Baud rate | 500,000 baud |
| Bit period | 2 μs |
| Byte period (8N1) | 20 μs |
| Max throughput | 50,000 bytes/sec |
| RX FIFO type | `FifoBuffer` (ring buffer, software) |
| TX FIFO type | `FifoBuffer` (ring buffer, software) |

The main loop calls `uart_checkAndParse()` **five times** per iteration (once explicitly + four in a block) to keep latency low for incoming parameter updates from the STM32F4. At 500 kBaud the UART can deliver up to one byte every 20 μs; the main loop iteration time is dominated by `dout_updateOutputs()` (40 bit-bang clock cycles on PD5/PD6) and `din_readNextInput()` (10 iterations × ~6 GPIO ops each).

The `uart_waitAck()` function performs a **blocking spin** waiting for an ACK byte from the STM32F4 after certain command sequences (e.g. preset transfers). This should not be called with time-sensitive operations pending.

---

## 6. SPI Timing

| Parameter | Value |
|---|---|
| SPI clock | F_CPU / 64 = **312.5 kHz** |
| Bit period | 3.2 μs |
| Byte period | 25.6 μs |
| SD init max clock | 400 kHz (SD spec); 312.5 kHz is compliant |

The SPI is used exclusively during SD card access (preset load/save and sample management). SPI transfers are **polling** (spin on `SPIF`), meaning the CPU is blocked during each byte transfer. A 512-byte SD sector read therefore blocks for 512 × 25.6 μs ≈ **13.1 ms** — approximately one full system tick period. This is acceptable because SD access happens only on user demand (not in the audio path).

`spi_deInit()` tri-states the SPI pins after use, allowing PB4–PB7 to be used for other purposes if needed (though in this design they are dedicated to SD).

---

## 7. ADC Timing

| Parameter | Value |
|---|---|
| ADC prescaler | /64 |
| ADC clock | 312.5 kHz |
| Single conversion time | 13.5 ADC cycles = **43.2 μs** |
| Averaging samples | 4 per channel |
| Time per averaged channel | 4 × 43.2 μs ≈ **173 μs** |
| Time for all 4 channels | ≈ **690 μs** per main-loop `adc_checkPots()` call |

Conversions are triggered with `ADSC` and waited on with a busy-loop (`while (ADCSRA & (1<<ADSC))`). The first conversion after init is a dummy read to flush the sample-and-hold from power-on state. With the 3-LSB hysteresis in `adc_checkPots()`, the menu is updated only when a pot genuinely moves, avoiding constant repaint from noise.

---

## 8. Bit-Banged I/O Timing

### 8.1 Digital Input (74HC165 Chain)

`din_readNextInput()` reads 10 bits per main-loop call. Each bit requires two GPIO writes (clock low, clock high) plus one GPIO read, approximately 3–6 clock cycles each at 20 MHz = ~1.5 ns per cycle. Reading one bit takes roughly **150–300 ns**, so 10 bits ≈ **1.5–3 μs**. A full load-and-read cycle of all 39 inputs (4 calls × 10 bits each) takes ~6–12 μs.

The 74HC165 propagation delay is typically 10 ns at 5 V, well within the bit-bang timing margin.

### 8.2 Digital Output (74HC595 Chain)

`dout_updateOutputs()` shifts out all 40 bits sequentially. Each bit requires three GPIO operations (set/clear data pin, clock high, clock low). At 20 MHz this takes roughly 3–6 ns × 3 ops × 40 bits ≈ **0.4–0.7 μs** — in practice slightly longer due to function call and loop overhead, estimated at **5–10 μs** total.

---

## 9. Memory Allocation

### 9.1 Flash Usage

| Region | Contents |
|---|---|
| Interrupt vector table | 0x0000 – ~0x006A (54 vectors × 2 bytes each) |
| Application code | Starts immediately after vectors |
| `PROGMEM` strings | String literals declared with `PSTR()` or `pgm_read_byte` |
| FatFs library | ~8–10 KB (elm-chan ff.c) |
| Total available | **64 KB** |

String and menu text is stored in Flash via `pgmspace.h` to avoid exhausting the 4 KB SRAM.

### 9.2 SRAM Usage

| Region | Contents | Approx. Size |
|---|---|---|
| `.data` (initialised globals) | Global variables with initial values | varies |
| `.bss` (zero-initialised) | Uninitialised globals (zeroed at startup) | varies |
| UART TX FIFO (`uart_txBuffer`) | Ring buffer | ~32–64 bytes |
| UART RX FIFO (`uart_rxBuffer`) | Ring buffer | ~32–64 bytes |
| DIN input data (`din_inputData`) | 39 bits = 5 bytes | 5 bytes |
| DOUT output data (`dout_outputData`) | 40 bits = 5 bytes | 5 bytes |
| ADC values (`adc_potValues`, `adc_potAvg`) | 4 × 2 × uint16 | 16 bytes |
| Encoder delta (`enc_delta`) | int8 | 1 byte |
| Preset / menu state | Varies by menu implementation | ~100–400 bytes |
| FatFs working buffer | `FATFS`, `FIL` structures, sector buffer | ~512+ bytes |
| Stack | Grows down from top of SRAM | remaining |

**Stack size:** With 4 KB SRAM and static allocations estimated at ~1–2 KB, approximately **2–3 KB** is available for the stack. The deeply nested call chain (main → ISR → FatFs → SPI) and multiple local variables mean stack overflow is possible if preset files are large. There is no stack overflow detection in the firmware.

### 9.3 EEPROM

The ATmega644 has 2 KB EEPROM. The audited source does not show explicit EEPROM access in the main firmware (preset data is stored on SD card). EEPROM may be used for global settings or AVR bootloader state.

---

## 10. Startup Sequence

The following sequence is executed from `main()`, in order, before the main loop:

1. 100 ms delay (`_delay_ms(100)`) for power rail stabilisation.
2. Configure `CORTEX_RESET_PIN` (PB0) as input.
3. `lcd_init()` — configures PB0–PB3 as LCD data outputs, PC3/PC4 as LCD control; sends HD44780 soft-reset sequence (three 50 ms intervals).
4. `uart_init()` — initialises FIFO buffers, configures USART0 at 500 kBaud, enables RX/TX/RX-interrupt.
5. Display boot message ("Sonic Potions / LXR Drums V0.37").
6. `adc_init()` — configures ADC, executes one dummy conversion.
7. `encode_init()` — configures encoder pins (PC0–PC2) as inputs with pull-ups; initialises Timer0 for ~1 ms CTC.
8. `din_init()` — configures shift-register pins; executes initial parallel load.
9. `dout_init()` — configures LED output shift-register pins; clears output data array.
10. `led_init()` — initialises LED state machine.
11. `time_initTimer()` — initialises Timer2 for ~13.1 ms overflow.
12. `led_toggle(0)` — toggles step 0 LED (prevents it being permanently lit at boot).
13. 2 s boot display delay.
14. `sei()` — **global interrupts enabled**.
15. `menu_init()` — initialises the UI menu state machine.
16. `preset_init()` — initialises SD card and FatFs (requires UART interrupts for STM32F4 comms).
17. `adc_checkPots()` — reads pot positions once to establish baseline before preset load.
18. `preset_loadGlobals()` — loads global settings from SD.
19. `preset_loadDrumset(0, 0)` — loads kit 0 from SD; on failure displays "Kit read error" and sends all-zero parameters to STM32F4.
20. `copyClear_clearCurrentPattern()` — resets the sequencer pattern buffer.
21. `frontPanel_sendData(SAMPLE_CC, SAMPLE_COUNT, 0x00)` — requests sample count from STM32F4.
22. Enter the **main loop**.

---

## 11. Main Loop Structure

The main loop runs continuously with no sleep or yield. Approximate per-iteration work:

| Step | Function | Estimated Time |
|---|---|---|
| 1 | `din_readNextInput()` | ~5 μs |
| 2 | `dout_updateOutputs()` | ~8 μs |
| 3 | `uart_checkAndParse()` (×1) | ~2–20 μs |
| 4 | `encode_read4()` + `encode_readButton()` | <1 μs |
| 5 | `menu_parseEncoder()` | <1 μs (no update) |
| 6 | `uart_checkAndParse()` (×4) | ~8–80 μs |
| 7 | `adc_checkPots()` | ~690 μs |
| 8 | `led_tickHandler()` | <1 μs |
| 9 | `screensaver_check()` | <1 μs |
| 10 | `buttonHandler_tick()` | <1 μs |
| 11 | `SD_checkCardAvailable()` | <1 μs (no card event) |

The ADC reads (step 7) dominate. The loop is **not deterministic** — UART ISRs fire asynchronously and `adc_checkPots()` performs blocking busy-wait conversions. Typical iteration time is estimated at **0.8–1.5 ms** under normal conditions.
