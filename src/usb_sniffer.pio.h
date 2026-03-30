/**
 * @file    usb_sniffer.pio.h
 * @brief   Pre-compiled PIO program and state-machine initializer.
 * @version 1.0.0
 * @license MIT
 *
 * @details
 *   This header is the compiled equivalent of @c usb_sniffer.pio.
 *   It can be regenerated with:
 *   @code
 *     pioasm usb_sniffer.pio usb_sniffer.pio.h
 *   @endcode
 *
 *   Contents:
 *   - Binary PIO instruction array
 *   - @c pio_program descriptor
 *   - @ref usb_sniffer_program_init() — full SM configuration
 *
 * @author Ângelo Moisés Alves
 */

#ifndef USB_SNIFFER_PIO_H
#define USB_SNIFFER_PIO_H

#include "hardware/pio.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  COMPILED PROGRAM
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Compiled PIO instruction(s).
 *
 * Single instruction: @c "in pins, 2"
 * Encoding: @c 0100_0000_0000_0010 = 0x4002
 */
static const uint16_t usb_sniffer_program_instructions[] = {
    //     .wrap_target
    0x4002,  // 0: in pins, 2
    //     .wrap
};

/** @brief PIO program descriptor for @c pio_add_program(). */
static const struct pio_program usb_sniffer_program = {
    .instructions = usb_sniffer_program_instructions,
    .length       = 1,
    .origin       = -1  ///< Any available offset
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  STATE MACHINE INITIALIZER
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialize a PIO state machine for USB line sampling.
 *
 * Configures pin mapping, shift register, FIFO joining, and clock
 * divider.  After this call the SM is running and pushing samples
 * into the RX FIFO.
 *
 * @param[in] pio    PIO instance (@c pio0 or @c pio1).
 * @param[in] sm     State machine index (0–3).
 * @param[in] offset Program offset returned by @c pio_add_program().
 * @param[in] pin_dp GPIO number of the D+ pin.
 *                   D- @b must be @c pin_dp+1.
 * @param[in] div    Clock divider.
 *                   - 10.0  for Low-Speed  8× oversampling @ 120 MHz
 *                   -  1.25 for Full-Speed 8× oversampling @ 120 MHz
 *
 * @pre System clock must be set before calling this function.
 * @pre @c pin_dp and @c pin_dp+1 must not be used by other peripherals.
 */
static inline void usb_sniffer_program_init(
    PIO   pio,
    uint  sm,
    uint  offset,
    uint  pin_dp,
    float div
) {
    /* Configure both pins as high-impedance inputs */
    pio_sm_set_consecutive_pindirs(pio, sm, pin_dp, 2, false);

    gpio_disable_pulls(pin_dp);
    gpio_disable_pulls(pin_dp + 1);

    /* Enable input hysteresis for noise rejection */
    gpio_set_input_hysteresis_enabled(pin_dp,     true);
    gpio_set_input_hysteresis_enabled(pin_dp + 1, true);

    /* Assign pins to the correct PIO instance.
     * NOTE: gpio_set_function() expects gpio_function_t (typed enum).
     * The explicit cast is required on arduino-pico core >= 5.x,
     * which no longer allows implicit conversion from uint to enum. */
    gpio_function_t pio_func = (gpio_function_t)(GPIO_FUNC_PIO0 + (pio == pio1 ? 1 : 0));
    gpio_set_function(pin_dp,     pio_func);
    gpio_set_function(pin_dp + 1, pio_func);

    /* Build SM configuration */
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset);        /* Single-instruction wrap  */
    sm_config_set_in_pins(&c, pin_dp);             /* IN base = D+             */
    sm_config_set_in_shift(&c, true, true, 32);    /* Shift right, autopush@32 */
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX); /* 8-deep RX FIFO          */
    sm_config_set_clkdiv(&c, div);

    /* Apply and start */
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}

#endif /* USB_SNIFFER_PIO_H */
