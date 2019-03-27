#include <avr/io.h>
#include <avr/interrupt.h>

#include "diag.h"
#include "rxin.h"
#include "motors.h"
#include "state.h"
#include "blinky.h"

#include <string.h>
#include <stdlib.h>

#define F_CPU 3333333 /* 20MHz / 6(default prescale) */
#include <util/delay.h>

// Our global state
volatile rxin_state_t rxin_state;

// Set rxin_state.next_channel to CHANNEL_SYNC
// if we are expecting a sync pulse.
#define CHANNEL_SYNC (-1)

// expected length of a sync pulse.
const uint16_t SYNC_PULSE_MIN=3000; // us
const uint16_t SYNC_PULSE_MAX=30000; // us
// Normal pulse len
// Allow a little bit of slack for weirdness etc.
const uint16_t PULSE_MIN=600; // us
const uint16_t PULSE_MAX=2500; // us

const uint32_t NOSIGNAL_TIME=50; // Centiseconds

// Minimum range of throttle before calibration is considered complete:
const uint16_t THROTTLE_RANGE_OK=600; // (microseconds)
/*

    Our rx pin has a pull-down resistor. So if it has nothing
    driving it, it should stay low.

    If we are getting serial (uart) ibus or sbus data, we'd expect
    it to be mostly high with a lot of very short pulses 
    becuase they use baud rates of 100k+, so we'll see very short pulses
    of 10-80 microseconds, maybe longer ones between packets.

    If we are getting CPPM data, we expect it to be mostly high
    with long sync pulses (3-18 ms) between normal packets.

*/
static void rxin_init_hw()
{
	// UART0- need to use "alternate" pins 
	// This puts T xD and RxD on PA1 and PA2
	PORTMUX.CTRLB = PORTMUX_USART0_ALTERNATE_gc; 

	// Timer B TCB0 is used to measure the pulse width.
	// RxD pin (PA2) 
	PORTA.DIRCLR = 1 << 2;
	// EVENTSYS - use ASYNC channel 0 (works with porta)
	EVSYS.ASYNCCH0 = EVSYS_ASYNCCH0_PORTA_PIN2_gc;
	// routing for asynch user 0 == TCB0
	EVSYS.ASYNCUSER0 = EVSYS_ASYNCUSER0_ASYNCCH0_gc;
	// Configure TCB0
	// Set up Timer/Counter B to measure pulse length.
	TCB0.CTRLB = TCB_CNTMODE_PW_gc ;
    // DO NOT SET TCB_CCMPEN_bm - because we don't want it outputting.
    // If TCB_CCMPEN_bm is enabled, then the timer will take one of our
    // gpio pins as its own, and TCA will not be able to send pulses.
	TCB0.EVCTRL = // default edge
		TCB_CAPTEI_bm; // Enable capture.
	TCB0.CTRLA = TCB_CLKSEL_CLKDIV2_gc | // Divide system clock by2
		TCB_ENABLE_bm; // Turn on
	// Now TCB0 will wait until the pin is active, then start counting,
	// then capture the counter when the pin goes inactive.
	// We can receive the interrupt from the pin and get the captured value.
	// Enable the capture interrupt from TCB0.
	TCB0.INTCTRL =	TCB_CAPT_bm;

	// Diagnostic uart output	
	// TxD pin PA1 is used for diag, should be an output
	PORTA.DIRSET = 1 << 1;
	// Set baud rate etc
	USART0.CTRLC =
        	USART_CMODE_ASYNCHRONOUS_gc | // Mode: Asynchronous[default]
        	USART_PMODE_DISABLED_gc | // Parity: None[default]
        	USART_SBMODE_1BIT_gc | // StopBit: 1bit[default]
		USART_CHSIZE_8BIT_gc; // CharacterSize: 8bit[default]
	uint32_t want_baud_hz = 115200; // Baud rate
	uint32_t clk_per_hz = 3333L * 1000; // CLK_PER after prescaler in hz
	uint16_t baud_param = (64 * clk_per_hz) / (16 * want_baud_hz);
	USART0.BAUD = baud_param;
        USART0.CTRLB = USART_TXEN_bm ; // Start Transmitter
}

static void rxin_init_state()
{
    memset((void *) &rxin_state, 0, sizeof(rxin_state));
    rxin_state.next_channel = CHANNEL_SYNC;
}

void rxin_init()
{
    rxin_init_state();
    rxin_init_hw();
}

static volatile uint16_t last_pulse_len_ticks;

ISR(TCB0_INT_vect)
{
    // Received when the rx pulse pin goes low.
    // No need to explicitly clear the irq, it is automatically
    // cleared when we read CCPM
    uint16_t pulse_len = TCB0.CCMP;
    // Early exit: quickly ignore very short pulses.
    // This is because another short pulse might happen, we do not want
    // to waste time in interrupts.
    if (pulse_len < 20) {
        return;
    }
    // This will be in clock units, which is 3.333mhz divided by
    // whatever the divider is set to,
    // so about 1.66mhz
    uint16_t pulse_len_us = ((uint32_t) pulse_len) * 100 / 166;
    last_pulse_len_ticks = pulse_len;
    rxin_state.last_pulse_len = pulse_len_us;
    if ((pulse_len_us >= SYNC_PULSE_MIN) && (pulse_len_us < SYNC_PULSE_MAX))
    {
        // Got a sync pulse
        // expect next channel 0.
        // pulses are not yet valid.
        rxin_state.next_channel = 0;
        // Clear pulse_lengths_next
        memset((void *) rxin_state.pulse_lengths_next, 0, sizeof(rxin_state.pulse_lengths_next));
    } else {
        if ((pulse_len_us >= PULSE_MIN) && (pulse_len_us < PULSE_MAX)) {
            // Got a normal pulse
            if (rxin_state.next_channel != CHANNEL_SYNC) {
                rxin_state.pulse_lengths_next[rxin_state.next_channel] = pulse_len_us;
                rxin_state.next_channel += 1;
                if (rxin_state.next_channel >= RX_CHANNELS) {
                    rxin_state.packet_count ++;
                    // Got all channels.
                    // If the rx sends more channels, we ignore them.
                    // We have enough channels now, the rx can send whatever it wants.
                    rxin_state.pulses_valid = true; // tell main loop that the data are valid.
                    rxin_state.next_channel = CHANNEL_SYNC;
                } else {
                    // Not valid yet.
                }
            }
        }
    }
}

static void special_test_tcb() {
    diag_println("Special tcb test routine");
    // allocate a stack buffer and clear it.
    uint16_t buf[50];
    memset(buf, 0, sizeof(buf));
    // Wait for pulse to go low
    uint8_t bm = 1 <<2;
    while ( PORTA.IN & bm);
    // Now debug write the value of the timer
    // Keep spitting out the value of the timer until the pin goes back low
    for (uint8_t n=0; n<50; n++) {
        buf[n] = TCB0.CNT;
        if (! (PORTA.IN & bm)) break;
        _delay_us(50);
    }
    uint16_t c1 = TCB0.CCMP;
    uint16_t c2 = last_pulse_len_ticks;
    uint16_t c3 = rxin_state.last_pulse_len;
    diag_println("CCMP was %04d", c1);
    diag_println("last_pulse was %04d", c2);
    diag_println("last_pulse in ms was %04d", c3);
    for (uint8_t n=0; n<50; n++) {
        diag_println("%04d", buf[n]);
    }
}

static void handle_got_signal(uint32_t now) {
    // Called when we first get a signal.
    rxin_state.got_signal = true;
    rxin_state.running_mode = RUNNING_MODE_CALIBRATION;
    // init calibration data
    rxin_state.throttle_max_position = 0;
    rxin_state.throttle_min_position = 10000;
    // read the current steering and weapon pos for zero
    rxin_state.steering_centre_position = 
        rxin_state.pulse_lengths[CHANNEL_INDEX_STEERING];
    rxin_state.weapon_centre_position = 
        rxin_state.pulse_lengths[CHANNEL_INDEX_WEAPON];
    diag_println("Got tx signal, now waiting for calibration.");
    blinky_state.blue_on = true;
    special_test_tcb();
}

static void handle_lost_signal(uint32_t now) {
    // Called when we lose the signal, i.e.
    // no packets for a while.
    diag_println("Lost tx signal");
    rxin_state.got_signal = false;
    rxin_state.running_mode = RUNNING_MODE_NOSIGNAL;
    motors_all_off();
    blinky_state.blue_on = false;
}

// min / max macros (used below)
#define MAX(a,b) ((a) > (b) ? a : b)
#define MIN(a,b) ((a) < (b) ? a : b)

static void handle_data_calibration() {
    // Called in calibration state.
        // Do calibration
        uint16_t throttle = rxin_state.pulse_lengths[CHANNEL_INDEX_THROTTLE];
        rxin_state.throttle_min_position = MIN(rxin_state.throttle_min_position, throttle);
        rxin_state.throttle_max_position = MAX(rxin_state.throttle_max_position, throttle);
        rxin_state.throttle_centre_position = 
            (rxin_state.throttle_min_position + rxin_state.throttle_max_position) / 2;
        // Determine if calibration is finished?
        int16_t throttlediff = (int16_t)throttle - (int16_t)rxin_state.throttle_centre_position;
        uint16_t throttlerange = rxin_state.throttle_max_position - rxin_state.throttle_min_position;
        if ((throttlerange > THROTTLE_RANGE_OK) && (abs(throttlediff) < 20) ) {
            // Throttle moved up, down and is now centred.
            diag_println("Calibration finished.");
            rxin_state.running_mode = RUNNING_MODE_READY;
        }
}

static void handle_data_ready() {
    // get signed throttle, steering data etc
    int16_t rel_throttle = (int16_t) rxin_state.pulse_lengths[CHANNEL_INDEX_THROTTLE] - 
        (int16_t) rxin_state.throttle_centre_position;
    int16_t rel_steering = (int16_t) rxin_state.pulse_lengths[CHANNEL_INDEX_STEERING] - 
        (int16_t) rxin_state.steering_centre_position;
    // Scale the throttle and steering data...
    if (rxin_state.debug_count == 0) {
        diag_println("ready: thr: %04d steer: %04d",
            rel_throttle, rel_steering);
    }
    // TODO: Actually drive the motors
}

static void handle_stick_data() {
    // Process the data in rxin_state.pulse_lengths
    // Called every time we have new data.
    uint32_t now = get_tickcount();
    if (! rxin_state.got_signal) {
        handle_got_signal(now);
    }
    rxin_state.last_signal_time = now;
    if (rxin_state.running_mode == RUNNING_MODE_READY) {
        handle_data_ready();
    }
    if (rxin_state.running_mode == RUNNING_MODE_CALIBRATION) {
        handle_data_calibration();
    }

    if (rxin_state.debug_count > 0) {
        rxin_state.debug_count -= 1;
    } else {
        rxin_state.debug_count = 50;
        diag_puts("rxin:");
        for (int i=0; i< RX_CHANNELS; i++) {
            diag_print(" chan:%d %05d", i, rxin_state.pulse_lengths[i]);
        }
        diag_puts("\r\n");
    }
}

void rxin_loop()
{
    // check cppm pulses:
    if (rxin_state.pulses_valid) {
        // Great! read the pulses.
        // Avoid race condition - take a copy with irq disabled.
        cli(); // interrupts off
        memcpy((void *) rxin_state.pulse_lengths, (void *) rxin_state.pulse_lengths_next, sizeof(rxin_state.pulse_lengths));
        // clear valid flag so we do not do the same work again.
        rxin_state.pulses_valid = 0;
        sei(); // interrupts on
        handle_stick_data();
    } else {
        if (rxin_state.got_signal)  {
            uint32_t now = get_tickcount();
            uint32_t nosig_time = (now - rxin_state.last_signal_time);
            if (nosig_time > NOSIGNAL_TIME) {
                handle_lost_signal(now);
            }
        }
    }
    
}

