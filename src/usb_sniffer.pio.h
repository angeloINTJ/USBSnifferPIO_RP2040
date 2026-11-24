/**
 * @file    usb_sniffer.pio.h
 * @brief   Pre-compiled PIO hardware DPLL program and state-machine initializer.
 * @version 2.0.0
 * @license MIT
 *
 * @details
 *   Compiled from @c usb_sniffer.pio via pioasm.
 *   Implements an 8x edge-resynchronized hardware DPLL on D+ (JMP_PIN) that
 *   outputs 1x bit-rate 2-bit [D+, D-] symbols at exact bit center (t = 4/8).
 *
 *   - 22 instructions (fits in 32-instruction PIO block with 10 slots free)
 *   - 8x lower DMA/CPU bandwidth vs raw 8x oversampling
 *   - Works for both Low-Speed (1.5 Mbps @ 12 MHz PIO) and Full-Speed (12 Mbps @ 96 MHz PIO)
 *
 * @author Ângelo Moisés Alves
 */

#ifndef USB_SNIFFER_PIO_H
#define USB_SNIFFER_PIO_H

#include "hardware/pio.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  COMPILED PROGRAM (22 instructions — hardware 8x DPLL)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define usb_sniffer_wrap_target 0
#define usb_sniffer_wrap        21

static const uint16_t usb_sniffer_program_instructions[] = {
            //     .wrap_target
    0x4002, //  0: in     pins, 2
    0xa042, //  1: nop
    0x00c4, //  2: jmp    pin, 4
    0x020d, //  3: jmp    13                     [2]
    0x00c6, //  4: jmp    pin, 6
    0x020d, //  5: jmp    13                     [2]
    0x00c8, //  6: jmp    pin, 8
    0x020d, //  7: jmp    13                     [2]
    0x00ca, //  8: jmp    pin, 10
    0x020d, //  9: jmp    13                     [2]
    0x00cc, // 10: jmp    pin, 12
    0x020d, // 11: jmp    13                     [2]
    0x0000, // 12: jmp    0
    0x4002, // 13: in     pins, 2
    0xa042, // 14: nop
    0x00d5, // 15: jmp    pin, 21
    0x00d5, // 16: jmp    pin, 21
    0x00d5, // 17: jmp    pin, 21
    0x00d5, // 18: jmp    pin, 21
    0x00d5, // 19: jmp    pin, 21
    0x000d, // 20: jmp    13
    0x0200, // 21: jmp    0                      [2]
            //     .wrap
};

/** @brief PIO program descriptor for @c pio_add_program(). */
static const struct pio_program usb_sniffer_program = {
    .instructions = usb_sniffer_program_instructions,
    .length       = 22,
    .origin       = -1  ///< Relocatable anywhere in PIO instruction memory
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  STATE MACHINE INITIALIZER
 * ═══════════════════════════════════════════════════════════════════════════ */

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

    /* Assign pins to the correct PIO instance */
    gpio_function_t pio_func = (gpio_function_t)(GPIO_FUNC_PIO0 + (pio == pio1 ? 1 : 0));
    gpio_set_function(pin_dp,     pio_func);
    gpio_set_function(pin_dp + 1, pio_func);

    /* Build SM configuration */
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + usb_sniffer_wrap_target, offset + usb_sniffer_wrap);
    sm_config_set_in_pins(&c, pin_dp);             /* IN base = D+ (bit0=D+, bit1=D-) */
    sm_config_set_jmp_pin(&c, pin_dp);             /* JMP pin = D+ (edge DPLL)        */
    sm_config_set_in_shift(&c, true, true, 32);    /* Shift right, autopush @ 32 bits */
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX); /* 8-deep RX FIFO                  */
    sm_config_set_clkdiv(&c, div);

    /* Apply and start */
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}

#endif /* USB_SNIFFER_PIO_H */
