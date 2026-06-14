/************************************************************************/
/*                                                                      */
/*                      Reading rotary encoder                          */
/*                      stable four-step encoder support                 */
/*                                                                      */
/*              Author: Peter Dannegger                                 */
/*                                                                      */
/************************************************************************/
#include "encoder.h"
#include "config.h"

#include <avr/interrupt.h>
#include <avr/io.h>
#include <limits.h>
#include <util/atomic.h>

// target: ATmega644
//------------------------------------------------------------------------

#ifndef F_CPU
#define F_CPU 20000000UL
#endif

#ifndef ENCODER_SAMPLE_HZ
#define ENCODER_SAMPLE_HZ 32000UL
#endif

#ifndef ENCODER_PHASE_STABLE_SAMPLES
#define ENCODER_PHASE_STABLE_SAMPLES 6
#endif

#ifndef ENCODER_BUTTON_STABLE_SAMPLES
#define ENCODER_BUTTON_STABLE_SAMPLES 192
#endif

#ifndef ENCODER_REST_STATE
#define ENCODER_REST_STATE 0x03
#endif

#ifndef ENC_ACCEL_MIN_REV_PER_SEC
#define ENC_ACCEL_MIN_REV_PER_SEC 1
#endif

#ifndef ENC_ACCEL_MAX_REV_PER_SEC
#define ENC_ACCEL_MAX_REV_PER_SEC 2
#endif

#ifndef ENC_ACCEL_MAX_MULT
#define ENC_ACCEL_MAX_MULT 5
#endif

#if ENCODER_PHASE_STABLE_SAMPLES < 1 || ENCODER_PHASE_STABLE_SAMPLES > 8
#error "ENCODER_PHASE_STABLE_SAMPLES must be between 1 and 8"
#endif

#if ENCODER_REST_STATE > 0x03
#error "ENCODER_REST_STATE must be a two-bit AB state"
#endif

#if ENCODER_SAMPLE_HZ == 0
#error "ENCODER_SAMPLE_HZ must be greater than zero"
#endif

#if ENCODER_BUTTON_STABLE_SAMPLES > 255
#error "ENCODER_BUTTON_STABLE_SAMPLES must fit in uint8_t"
#endif

#if ENC_ACCEL_MIN_REV_PER_SEC == 0
#error "ENC_ACCEL_MIN_REV_PER_SEC must be greater than zero"
#endif

#if ENC_ACCEL_MAX_REV_PER_SEC <= ENC_ACCEL_MIN_REV_PER_SEC
#error "ENC_ACCEL_MAX_REV_PER_SEC must be greater than ENC_ACCEL_MIN_REV_PER_SEC"
#endif

#if ENC_ACCEL_MAX_MULT < 1 || ENC_ACCEL_MAX_MULT > INT8_MAX
#error "ENC_ACCEL_MAX_MULT must be between 1 and INT8_MAX"
#endif

#define ENCODER_TIMER_PRESCALE 8UL
#define ENCODER_TIMER_CLOCK_BITS (1 << CS11)
#define ENCODER_TIMER_TOP ((F_CPU / ENCODER_TIMER_PRESCALE / ENCODER_SAMPLE_HZ) - 1UL)

#if ENCODER_TIMER_TOP > 65535UL
#error "ENCODER_SAMPLE_HZ is too low for Timer1 CTC with the selected prescaler"
#endif

#define PIN_A           (1 << PC0)
#define PIN_B           (1 << PC1)
#define PIN_BUTTON      (1 << PC2)

#define ENCODER_PHASE_MASK ((uint8_t)((1U << ENCODER_PHASE_STABLE_SAMPLES) - 1U))

#define ENCODER_PORT    PORTC
#define ENCODER_PIN     PINC
#define ENCODER_DDR     DDRC

#define ENCODER_DETENTS_PER_REV 24UL
#define ENCODER_ACCEL_AVG_DETENTS 4U
#define ENCODER_ACCEL_MIN_TICKS \
    (ENCODER_SAMPLE_HZ / (ENCODER_DETENTS_PER_REV * ENC_ACCEL_MIN_REV_PER_SEC))
#define ENCODER_ACCEL_MAX_TICKS \
    (ENCODER_SAMPLE_HZ / (ENCODER_DETENTS_PER_REV * ENC_ACCEL_MAX_REV_PER_SEC))

static volatile int8_t enc_delta;       // complete four-step detents
static volatile uint8_t button_pressed; // true when encoder button is pressed
static volatile uint8_t enc_accel_multiplier;

static uint8_t enc_phase_history_a;
static uint8_t enc_phase_history_b;
static uint8_t enc_rest_state;
static uint8_t enc_stable_state;
static int8_t enc_fsm_dir;
static uint8_t enc_fsm_phase;
static uint8_t enc_rest_jump_pending;
static uint8_t button_integrator;
static uint16_t enc_accel_elapsed_ticks;
static uint16_t enc_accel_intervals[ENCODER_ACCEL_AVG_DETENTS];
static uint16_t enc_accel_interval_sum;
static uint8_t enc_accel_interval_count;
static uint8_t enc_accel_interval_index;
static int8_t enc_accel_last_dir;


static uint8_t encoder_readPhaseState(uint8_t portValue)
{
    uint8_t state = 0;

    if( portValue & PIN_A )
        state |= 2;
    if( portValue & PIN_B )
        state |= 1;

    return state;
}


static int8_t encoder_decodeStep(uint8_t previous, uint8_t current)
{
    switch( (uint8_t)((previous << 2) | current) )
    {
        // Positive direction matches the original Dannegger mapping:
        // 00 -> 01 -> 11 -> 10 -> 00.
        case 0x01:
        case 0x07:
        case 0x08:
        case 0x0E:
            return 1;

        case 0x02:
        case 0x04:
        case 0x0B:
        case 0x0D:
            return -1;

        default:
            return 0;
    }
}


static void encoder_resetFsm(void)
{
    enc_fsm_dir = 0;
    enc_fsm_phase = 0;
    enc_rest_jump_pending = 0;
}


static void encoder_updateAcceleration(int8_t dir);


static void encoder_addDetent(int8_t dir)
{
    if( dir > 0 )
    {
        if( enc_delta < INT8_MAX )
            enc_delta++;
    }
    else if( dir < 0 )
    {
        if( enc_delta > INT8_MIN )
            enc_delta--;
    }

    encoder_updateAcceleration(dir);
}


static void encoder_resetAcceleration(int8_t dir)
{
    enc_accel_interval_sum = 0;
    enc_accel_interval_count = 0;
    enc_accel_interval_index = 0;
    enc_accel_last_dir = dir;
    enc_accel_multiplier = 1;
}


static void encoder_updateAcceleration(int8_t dir)
{
    const uint16_t interval = enc_accel_elapsed_ticks;
    uint16_t avgInterval;
    uint16_t span;
    uint16_t progress;

    enc_accel_elapsed_ticks = 0;

    if( dir == 0 )
        return;

    if( enc_accel_last_dir != dir || interval >= ENCODER_ACCEL_MIN_TICKS )
    {
        encoder_resetAcceleration(dir);
        return;
    }

    if( enc_accel_interval_count < ENCODER_ACCEL_AVG_DETENTS )
    {
        enc_accel_intervals[enc_accel_interval_index] = interval;
        enc_accel_interval_sum = (uint16_t)(enc_accel_interval_sum + interval);
        enc_accel_interval_count++;
    }
    else
    {
        enc_accel_interval_sum =
            (uint16_t)(enc_accel_interval_sum -
                       enc_accel_intervals[enc_accel_interval_index]);
        enc_accel_intervals[enc_accel_interval_index] = interval;
        enc_accel_interval_sum = (uint16_t)(enc_accel_interval_sum + interval);
    }

    enc_accel_interval_index++;
    if( enc_accel_interval_index >= ENCODER_ACCEL_AVG_DETENTS )
        enc_accel_interval_index = 0;

    avgInterval = (uint16_t)(enc_accel_interval_sum / enc_accel_interval_count);

    if( avgInterval <= ENCODER_ACCEL_MAX_TICKS )
    {
        enc_accel_multiplier = ENC_ACCEL_MAX_MULT;
        return;
    }

    span = (uint16_t)(ENCODER_ACCEL_MIN_TICKS - ENCODER_ACCEL_MAX_TICKS);
    progress = (uint16_t)(ENCODER_ACCEL_MIN_TICKS - avgInterval);
    enc_accel_multiplier =
        (uint8_t)(1 + ((uint32_t)progress * (ENC_ACCEL_MAX_MULT - 1) +
                      (span / 2)) / span);
}


static void encoder_handleStateChange(uint8_t previous, uint8_t current)
{
    const int8_t step = encoder_decodeStep(previous, current);
    const uint8_t oppositeRest = (uint8_t)(enc_rest_state ^ 0x03);

    if( step == 0 )
    {
        encoder_resetFsm();

        if( previous == enc_rest_state && current == oppositeRest )
            enc_rest_jump_pending = 1;

        return;
    }

    if( enc_rest_jump_pending )
    {
        enc_rest_jump_pending = 0;

        if( previous == oppositeRest && current != enc_rest_state )
        {
            enc_fsm_dir = step;
            enc_fsm_phase = 3;
            return;
        }
    }

    if( enc_fsm_dir == 0 )
    {
        if( previous == enc_rest_state && current != enc_rest_state )
        {
            enc_fsm_dir = step;
            enc_fsm_phase = 1;
        }
        return;
    }

    if( step != enc_fsm_dir )
    {
        encoder_resetFsm();
        return;
    }

    if( enc_fsm_phase < 4 )
        enc_fsm_phase++;

    if( current == enc_rest_state )
    {
        if( enc_fsm_phase == 4 )
            encoder_addDetent(enc_fsm_dir);

        encoder_resetFsm();
    }
    else if( enc_fsm_phase >= 4 )
    {
        encoder_resetFsm();
    }
}


void encode_init(void)
{
    const uint8_t pins = PIN_A | PIN_B | PIN_BUTTON;
    uint8_t initialPort;
    uint8_t initialState;

    ENCODER_DDR &= (uint8_t)~pins;
    ENCODER_PORT |= pins;

    initialPort = ENCODER_PIN;
    initialState = encoder_readPhaseState(initialPort);

    enc_phase_history_a = (initialState & 2) ? ENCODER_PHASE_MASK : 0;
    enc_phase_history_b = (initialState & 1) ? ENCODER_PHASE_MASK : 0;
    enc_rest_state = ENCODER_REST_STATE;
    enc_stable_state = initialState;
    encoder_resetFsm();
    enc_delta = 0;
    enc_accel_elapsed_ticks = ENCODER_ACCEL_MIN_TICKS;
    encoder_resetAcceleration(0);

    button_pressed = (initialPort & PIN_BUTTON) ? 0 : 1;
    button_integrator = button_pressed ? ENCODER_BUTTON_STABLE_SAMPLES : 0;

    // Timer1 CTC, prescaler 8. At 20 MHz and 32 kHz, OCR1A is 77.
    TCCR1A = 0;
    TCCR1B = (1 << WGM12) | ENCODER_TIMER_CLOCK_BITS;
    OCR1A = (uint16_t)ENCODER_TIMER_TOP;
    TIMSK1 |= (1 << OCIE1A);
}


ISR( TIMER1_COMPA_vect )
{
    const uint8_t portValue = ENCODER_PIN;
    const uint8_t rawA = (portValue & PIN_A) ? 1 : 0;
    const uint8_t rawB = (portValue & PIN_B) ? 1 : 0;
    uint8_t filteredState = enc_stable_state;

    if( enc_accel_elapsed_ticks < ENCODER_ACCEL_MIN_TICKS )
        enc_accel_elapsed_ticks++;

    enc_phase_history_a =
        (uint8_t)(((enc_phase_history_a << 1) | rawA) & ENCODER_PHASE_MASK);
    enc_phase_history_b =
        (uint8_t)(((enc_phase_history_b << 1) | rawB) & ENCODER_PHASE_MASK);

    if( enc_phase_history_a == 0 )
        filteredState &= (uint8_t)~2;
    else if( enc_phase_history_a == ENCODER_PHASE_MASK )
        filteredState |= 2;

    if( enc_phase_history_b == 0 )
        filteredState &= (uint8_t)~1;
    else if( enc_phase_history_b == ENCODER_PHASE_MASK )
        filteredState |= 1;

    if( filteredState != enc_stable_state )
    {
        const uint8_t previousState = enc_stable_state;

        enc_stable_state = filteredState;
        encoder_handleStateChange(previousState, filteredState);
    }

    if( portValue & PIN_BUTTON )
    {
        if( button_integrator > 0 )
            button_integrator--;
    }
    else
    {
        if( button_integrator < ENCODER_BUTTON_STABLE_SAMPLES )
            button_integrator++;
    }

    if( button_integrator == 0 )
        button_pressed = 0;
    else if( button_integrator >= ENCODER_BUTTON_STABLE_SAMPLES )
        button_pressed = 1;
}


int8_t encode_stableRead4(void)
{
    int8_t val;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        val = enc_delta;
        enc_delta = 0;
    }

    return val;
}


uint8_t encode_getAccelerationMultiplier(void)
{
    return enc_accel_multiplier;
}


// get the button value - returns true if button is pressed
uint8_t encode_readButton(void)
{
    return button_pressed;
}
