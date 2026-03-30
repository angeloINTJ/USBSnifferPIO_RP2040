/**
 * @file diagnostic.ino
 * @brief Layer-by-layer hardware diagnostic for USBSnifferPIO_RP2040.
 *
 * Tests each subsystem independently to pinpoint where the capture
 * pipeline is failing.  Run this BEFORE the other examples if you
 * see zero packets.
 *
 * ## Diagnostic layers (tested in order)
 *
 *   Layer 1 — GPIO:  Raw pin reads (are the wires connected?)
 *   Layer 2 — PIO:   State machine producing samples?
 *   Layer 3 — DMA:   Transferring data to RAM?
 *   Layer 4 — Data:  What line states does the buffer contain?
 *   Layer 5 — Decoder: Can the decoder find SYNC patterns?
 *
 * ## Wiring
 *
 *   USB cable D+  ──[100Ω]──► GP2
 *   USB cable D-  ──[100Ω]──► GP3
 *   USB cable GND ──────────► Pico GND
 *
 * ## Interpreting results
 *
 *   Each layer prints PASS / WARNING / FAIL.
 *   Look at the FIRST failure — everything downstream will also fail.
 *
 * @note Single-core only.  Does NOT use Core 1.
 * @note Baud rate: 115200.
 */

#include <USBSnifferPIO_RP2040.h>
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "usb_sniffer.pio.h"   /* PIO program descriptor (static) */

/* ═══════════════════════════════════════════════════════════════════════════
 *  CONFIGURATION — MATCH YOUR WIRING
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PIN_DP         2      ///< D+ GPIO (D- = PIN_DP + 1)
#define PIN_DM         (PIN_DP + 1)
#define TEST_SPEED     USBSpeed::LOW_SPEED
#define DIAG_PIO_IDX   1      ///< 0 = pio0, 1 = pio1

/* ═══════════════════════════════════════════════════════════════════════════
 *  MINI DMA BUFFER FOR STANDALONE TESTS
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DIAG_BUF_WORDS  512
static uint32_t diag_buf[DIAG_BUF_WORDS] __attribute__((aligned(4)));

/* ═══════════════════════════════════════════════════════════════════════════
 *  HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void printSeparator(const char* title) {
    Serial.println();
    Serial.printf("═══════════ %s ═══════════\n", title);
}

static void printResult(const char* label, bool pass) {
    Serial.printf("  >> %s: %s\n", label, pass ? "PASS ✓" : "FAIL ✗");
}

static const char* lineStateName(uint8_t raw2bit) {
    switch (raw2bit) {
        case 0b00: return "SE0";
        case 0b01: return "D+=1 D-=0";
        case 0b10: return "D+=0 D-=1";
        case 0b11: return "SE1";
        default:   return "???";
    }
}

static const char* logicalStateName(uint8_t raw2bit, USBSpeed speed) {
    if (raw2bit == 0b00) return "SE0";
    if (raw2bit == 0b11) return "SE1(err)";
    if (speed == USBSpeed::LOW_SPEED) {
        return (raw2bit == 0b10) ? "J(idle)" : "K(active)";
    } else {
        return (raw2bit == 0b01) ? "J(idle)" : "K(active)";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LAYER 1 — GPIO RAW PIN TEST
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Read raw GPIO states to verify physical connectivity.
 *
 * Expected results with a USB device connected and idle:
 *   Low-Speed:  D+ = 0, D- = 1  (J state, pull-up on D-)
 *   Full-Speed: D+ = 1, D- = 0  (J state, pull-up on D+)
 *
 * If both pins read 0: cable not connected, or GND not shared.
 * If both pins read 1: SE1 error state, wiring problem.
 */
static void testLayer1_GPIO() {
    printSeparator("LAYER 1: GPIO");

    /* Configure as plain inputs without PIO assignment */
    gpio_init(PIN_DP);
    gpio_init(PIN_DM);
    gpio_set_dir(PIN_DP, GPIO_IN);
    gpio_set_dir(PIN_DM, GPIO_IN);
    gpio_disable_pulls(PIN_DP);
    gpio_disable_pulls(PIN_DM);

    Serial.printf("  Pins: D+ = GP%u, D- = GP%u\n", PIN_DP, PIN_DM);

    /* Sample 1000 times over ~100 ms to catch transitions */
    uint32_t count[4] = {0, 0, 0, 0};  // SE0, 01, 10, SE1
    for (int i = 0; i < 1000; i++) {
        uint8_t dp = gpio_get(PIN_DP) ? 1 : 0;
        uint8_t dm = gpio_get(PIN_DM) ? 1 : 0;
        uint8_t state = (dm << 1) | dp;   // [D-, D+] note: PIO reads [D+, D-]
        count[state]++;
        delayMicroseconds(100);
    }

    Serial.println("\n  1000 samples over 100 ms:");
    Serial.printf("    D+=0 D-=0 (SE0)      : %lu\n", count[0]);
    Serial.printf("    D+=1 D-=0             : %lu\n", count[1]);
    Serial.printf("    D+=0 D-=1             : %lu\n", count[2]);
    Serial.printf("    D+=1 D-=1 (SE1)       : %lu\n", count[3]);

    /* Interpret */
    bool all_zero = (count[0] > 950);
    bool all_se1  = (count[3] > 950);
    bool has_transitions = (count[0] + count[3] < 900);

    Serial.println();

    if (all_zero) {
        Serial.println("  DIAGNOSIS: Both pins LOW — no signal detected.");
        Serial.println("    • Check that USB cable GND is connected to Pico GND");
        Serial.println("    • Check that D+/D- wires go through 100Ω to GP2/GP3");
        Serial.println("    • Check that a USB device is plugged into the cable");
        printResult("GPIO", false);
    } else if (all_se1) {
        Serial.println("  DIAGNOSIS: Both pins HIGH (SE1) — invalid state.");
        Serial.println("    • D+ and D- might be shorted together");
        Serial.println("    • Or external pull-ups are interfering");
        printResult("GPIO", false);
    } else {
        if (TEST_SPEED == USBSpeed::LOW_SPEED) {
            bool idle_ok = (count[2] > 100);  // D+=0, D-=1 = LS idle (J)
            Serial.printf("  Low-Speed idle (D+=0 D-=1): %lu samples %s\n",
                          count[2], idle_ok ? "— looks correct" : "— too few!");
            if (!idle_ok && count[1] > 100) {
                Serial.println("  WARNING: Seeing D+=1 D-=0 dominant — is this Full-Speed?");
                Serial.println("    • Try changing TEST_SPEED to USBSpeed::FULL_SPEED");
            }
        } else {
            bool idle_ok = (count[1] > 100);  // D+=1, D-=0 = FS idle (J)
            Serial.printf("  Full-Speed idle (D+=1 D-=0): %lu samples %s\n",
                          count[1], idle_ok ? "— looks correct" : "— too few!");
        }
        if (has_transitions) {
            Serial.println("  Transitions detected — bus is active.");
        }
        printResult("GPIO", true);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LAYER 2 — PIO STATE MACHINE TEST
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Load the PIO program and manually read samples from the FIFO.
 *
 * This bypasses DMA entirely.  If the FIFO receives data, the PIO
 * program is running correctly.
 */
static void testLayer2_PIO() {
    printSeparator("LAYER 2: PIO");

    PIO pio = (DIAG_PIO_IDX == 0) ? pio0 : pio1;
    uint32_t sys_hz = clock_get_hz(clk_sys);
    float target_hz = (TEST_SPEED == USBSpeed::FULL_SPEED) ? 96.0e6f : 12.0e6f;
    float div = (float)sys_hz / target_hz;

    Serial.printf("  System clock : %lu Hz\n", sys_hz);
    Serial.printf("  PIO target   : %.0f Hz (8x oversampling)\n", target_hz);
    Serial.printf("  Clock divider: %.3f\n", div);

    if (div < 1.0f) {
        Serial.println("  FATAL: Clock divider < 1.0 — system clock too slow!");
        Serial.printf("    Full-Speed needs sysclk >= 96 MHz, you have %lu MHz\n",
                      sys_hz / 1000000);
        printResult("PIO", false);
        return;
    }

    /* Claim SM */
    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        Serial.println("  FAIL: No free state machine on PIO instance.");
        printResult("PIO", false);
        return;
    }
    Serial.printf("  Using PIO%d SM%d\n", DIAG_PIO_IDX, sm);

    /* Load program */
    if (!pio_can_add_program(pio, &usb_sniffer_program)) {
        Serial.println("  FAIL: No instruction space in PIO.");
        pio_sm_unclaim(pio, sm);
        printResult("PIO", false);
        return;
    }

    uint offset = pio_add_program(pio, &usb_sniffer_program);

    /* Init SM (replicates usb_sniffer_program_init) */
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_DP, 2, false);
    gpio_disable_pulls(PIN_DP);
    gpio_disable_pulls(PIN_DM);
    gpio_set_input_hysteresis_enabled(PIN_DP, true);
    gpio_set_input_hysteresis_enabled(PIN_DM, true);
    gpio_function_t pio_func = (gpio_function_t)(GPIO_FUNC_PIO0 + (pio == pio1 ? 1 : 0));
    gpio_set_function(PIN_DP, pio_func);
    gpio_set_function(PIN_DM, pio_func);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset);
    sm_config_set_in_pins(&c, PIN_DP);
    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, div);

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    /* Read a few words directly from the FIFO */
    Serial.println("\n  Reading 8 words directly from PIO RX FIFO...");
    uint8_t words_read = 0;
    uint32_t start_ms = millis();
    uint32_t words[8];

    while (words_read < 8 && (millis() - start_ms) < 500) {
        if (!pio_sm_is_rx_fifo_empty(pio, sm)) {
            words[words_read] = pio_sm_get(pio, sm);
            words_read++;
        }
    }

    if (words_read == 0) {
        Serial.println("  FAIL: FIFO empty after 500 ms — PIO not producing data!");
        Serial.println("    • Pins may not be assigned to PIO correctly");
    } else {
        Serial.printf("  Got %u words in %lu ms\n", words_read, millis() - start_ms);
        for (uint8_t i = 0; i < words_read; i++) {
            Serial.printf("    FIFO[%u] = 0x%08lX\n", i, words[i]);
        }
    }

    /* Cleanup */
    pio_sm_set_enabled(pio, sm, false);
    pio_remove_program(pio, &usb_sniffer_program, offset);
    pio_sm_unclaim(pio, sm);

    printResult("PIO", words_read > 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LAYER 3 — DMA TRANSFER TEST
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Run a complete DMA transfer from PIO to RAM.
 *
 * Uses a small buffer (512 words) and waits for completion.
 * Reports timing and whether the buffer contains non-zero data.
 */
static void testLayer3_DMA() {
    printSeparator("LAYER 3: DMA");

    PIO pio = (DIAG_PIO_IDX == 0) ? pio0 : pio1;
    float div = (float)clock_get_hz(clk_sys)
              / ((TEST_SPEED == USBSpeed::FULL_SPEED) ? 96.0e6f : 12.0e6f);

    /* Setup PIO */
    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) { Serial.println("  SKIP: no SM"); return; }

    if (!pio_can_add_program(pio, &usb_sniffer_program)) {
        Serial.println("  SKIP: no PIO space");
        pio_sm_unclaim(pio, sm);
        return;
    }
    uint offset = pio_add_program(pio, &usb_sniffer_program);

    /* Init SM */
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_DP, 2, false);
    gpio_disable_pulls(PIN_DP);
    gpio_disable_pulls(PIN_DM);
    gpio_set_input_hysteresis_enabled(PIN_DP, true);
    gpio_set_input_hysteresis_enabled(PIN_DM, true);
    gpio_function_t pio_func = (gpio_function_t)(GPIO_FUNC_PIO0 + (pio == pio1 ? 1 : 0));
    gpio_set_function(PIN_DP, pio_func);
    gpio_set_function(PIN_DM, pio_func);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset);
    sm_config_set_in_pins(&c, PIN_DP);
    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, div);

    pio_sm_init(pio, sm, offset, &c);

    /* Setup DMA */
    int dma_ch = dma_claim_unused_channel(false);
    if (dma_ch < 0) {
        Serial.println("  FAIL: No DMA channel");
        pio_sm_set_enabled(pio, sm, false);
        pio_remove_program(pio, &usb_sniffer_program, offset);
        pio_sm_unclaim(pio, sm);
        printResult("DMA", false);
        return;
    }

    memset(diag_buf, 0xDE, sizeof(diag_buf));   /* fill with sentinel */

    dma_channel_config dc = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, false));
    channel_config_set_chain_to(&dc, dma_ch);   /* self-chain = disabled */

    dma_channel_configure(dma_ch, &dc,
        diag_buf,             /* write addr  */
        &pio->rxf[sm],        /* read addr   */
        DIAG_BUF_WORDS,       /* count       */
        false                 /* don't start yet */
    );

    /* Start PIO then DMA */
    pio_sm_set_enabled(pio, sm, true);
    uint32_t t0 = micros();
    dma_channel_start(dma_ch);

    /* Wait with timeout */
    uint32_t timeout_ms = 2000;
    while (dma_channel_is_busy(dma_ch)) {
        if (millis() - (t0 / 1000) > timeout_ms) break;
    }
    uint32_t elapsed_us = micros() - t0;
    bool completed = !dma_channel_is_busy(dma_ch);

    uint32_t remaining = 0;
    if (!completed) {
        remaining = dma_channel_hw_addr(dma_ch)->transfer_count;
        dma_channel_abort(dma_ch);
    }

    Serial.printf("  DMA %s in %lu µs\n",
                  completed ? "completed" : "TIMED OUT", elapsed_us);
    if (!completed) {
        Serial.printf("  Transfers remaining: %lu / %u\n", remaining, DIAG_BUF_WORDS);
    }

    /* Check buffer content */
    uint32_t sentinel_count = 0;
    for (int i = 0; i < DIAG_BUF_WORDS; i++) {
        if (diag_buf[i] == 0xDEDEDEDE) sentinel_count++;
    }
    uint32_t written = DIAG_BUF_WORDS - sentinel_count;
    Serial.printf("  Words written to RAM: %lu / %u\n", written, DIAG_BUF_WORDS);

    /* Cleanup */
    pio_sm_set_enabled(pio, sm, false);
    dma_channel_unclaim(dma_ch);
    pio_remove_program(pio, &usb_sniffer_program, offset);
    pio_sm_unclaim(pio, sm);

    printResult("DMA", completed && written > 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LAYER 4 — RAW DATA ANALYSIS
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Analyze the raw sample buffer from Layer 3.
 *
 * Counts how many of each 2-bit line state appear, and whether
 * there are any J→K transitions (which would be SYNC candidates).
 */
static void testLayer4_DataAnalysis() {
    printSeparator("LAYER 4: RAW DATA ANALYSIS");

    /* Count line states across entire buffer */
    uint32_t state_count[4] = {0, 0, 0, 0};
    uint32_t transitions = 0;
    uint8_t prev_sample = 0xFF;

    for (int w = 0; w < DIAG_BUF_WORDS; w++) {
        if (diag_buf[w] == 0xDEDEDEDE) continue;  /* skip sentinel */
        uint32_t word = diag_buf[w];
        for (int s = 0; s < 16; s++) {
            uint8_t sample = word & 0x03;
            state_count[sample]++;
            if (prev_sample != 0xFF && sample != prev_sample) {
                transitions++;
            }
            prev_sample = sample;
            word >>= 2;
        }
    }

    uint32_t total = state_count[0] + state_count[1] + state_count[2] + state_count[3];

    Serial.printf("  Total samples analyzed: %lu\n", total);
    Serial.println();

    /* PIO reads [D+, D-] as 2-bit: bit0 = first pin (D+), bit1 = second pin (D-)
     * So: 0b00 = both low, 0b01 = D+ high, 0b10 = D- high, 0b11 = both high
     *
     * NOTE: PIO 'in pins, 2' with base=PIN_DP reads:
     *   bit 0 = PIN_DP (D+)
     *   bit 1 = PIN_DP+1 (D-)
     */
    Serial.println("  Raw 2-bit sample distribution:");
    Serial.printf("    0b00 (D+=0 D-=0) SE0      : %lu  (%.1f%%)\n",
                  state_count[0], total ? state_count[0] * 100.0f / total : 0);
    Serial.printf("    0b01 (D+=1 D-=0)           : %lu  (%.1f%%)\n",
                  state_count[1], total ? state_count[1] * 100.0f / total : 0);
    Serial.printf("    0b10 (D+=0 D-=1)           : %lu  (%.1f%%)\n",
                  state_count[2], total ? state_count[2] * 100.0f / total : 0);
    Serial.printf("    0b11 (D+=1 D-=1) SE1       : %lu  (%.1f%%)\n",
                  state_count[3], total ? state_count[3] * 100.0f / total : 0);
    Serial.printf("  Total transitions: %lu\n", transitions);

    Serial.println();

    /* Interpret for the configured speed */
    Serial.printf("  Logical interpretation (%s):\n",
                  (TEST_SPEED == USBSpeed::LOW_SPEED) ? "Low-Speed" : "Full-Speed");

    uint32_t j_count, k_count;
    if (TEST_SPEED == USBSpeed::LOW_SPEED) {
        /* LS: J = D- high (0b10), K = D+ high (0b01) */
        j_count = state_count[2];
        k_count = state_count[1];
    } else {
        /* FS: J = D+ high (0b01), K = D- high (0b10) */
        j_count = state_count[1];
        k_count = state_count[2];
    }

    Serial.printf("    J (idle)  : %lu  (%.1f%%)\n",
                  j_count, total ? j_count * 100.0f / total : 0);
    Serial.printf("    K (active): %lu  (%.1f%%)\n",
                  k_count, total ? k_count * 100.0f / total : 0);
    Serial.printf("    SE0 (EOP) : %lu  (%.1f%%)\n",
                  state_count[0], total ? state_count[0] * 100.0f / total : 0);
    Serial.printf("    SE1 (err) : %lu  (%.1f%%)\n",
                  state_count[3], total ? state_count[3] * 100.0f / total : 0);

    Serial.println();

    /* Diagnosis */
    bool pass = true;

    if (total == 0) {
        Serial.println("  DIAGNOSIS: No data — DMA didn't fill the buffer.");
        pass = false;
    }
    else if (state_count[0] > total * 95 / 100) {
        Serial.println("  DIAGNOSIS: >95% SE0 — both pins reading LOW.");
        Serial.println("    • No USB device connected?");
        Serial.println("    • GND not shared between USB cable and Pico?");
        Serial.println("    • Wires disconnected or broken?");
        pass = false;
    }
    else if (state_count[3] > total * 95 / 100) {
        Serial.println("  DIAGNOSIS: >95% SE1 — both pins reading HIGH.");
        Serial.println("    • D+ and D- might be shorted together");
        pass = false;
    }
    else if (j_count > total * 99 / 100 && transitions < 10) {
        Serial.println("  DIAGNOSIS: Bus is idle (>99% J, almost no transitions).");
        Serial.println("    • USB device connected but not generating traffic?");
        Serial.println("    • Host port not polling the device?");
        Serial.println("    • Try pressing keys on a keyboard or moving a mouse.");
        pass = false;
    }
    else if (k_count > j_count * 3) {
        Serial.println("  WARNING: K state dominant over J — speed might be wrong.");
        Serial.println("    • If using Low-Speed, try Full-Speed, or vice-versa");
        Serial.println("    • D+ and D- wires might be swapped");
        pass = false;
    }
    else if (transitions > 100) {
        Serial.println("  Looks good: bus active with J/K transitions.");
        Serial.printf("  Estimated activity: ~%lu transitions in %u samples\n",
                      transitions, (unsigned)total);
    }

    /* Hex dump of first 8 words for manual inspection */
    Serial.println("\n  First 8 DMA words (hex):");
    for (int i = 0; i < 8 && i < DIAG_BUF_WORDS; i++) {
        Serial.printf("    [%3d] 0x%08lX  ", i, diag_buf[i]);
        /* Print 16 samples as line-state letters */
        uint32_t w = diag_buf[i];
        for (int s = 0; s < 16; s++) {
            uint8_t sample = w & 0x03;
            char ch;
            if (sample == 0b00)      ch = '0';  // SE0
            else if (sample == 0b11) ch = '!';  // SE1
            else {
                bool is_j;
                if (TEST_SPEED == USBSpeed::LOW_SPEED)
                    is_j = (sample == 0b10);
                else
                    is_j = (sample == 0b01);
                ch = is_j ? 'J' : 'K';
            }
            Serial.print(ch);
            w >>= 2;
        }
        Serial.println();
    }

    printResult("DATA", pass);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LAYER 5 — FULL LIBRARY DECODER TEST
 * ═══════════════════════════════════════════════════════════════════════════ */

/// @brief Packet count from decoder callback.
static volatile uint32_t g_diag_pkt_count = 0;

static void diagCallback(const USBPacket& pkt) {
    g_diag_pkt_count++;
}

/**
 * @brief Run the full USBSnifferPIO library for 5 seconds.
 *
 * This is the end-to-end test.  If Layers 1–4 pass but this
 * fails, the problem is in the decoder FSM.
 */
static void testLayer5_FullLibrary() {
    printSeparator("LAYER 5: FULL LIBRARY");

    USBSnifferPIO sniffer;

    Serial.printf("  Starting capture (PIN_DP=%u, %s, PIO%d)...\n",
                  PIN_DP,
                  (TEST_SPEED == USBSpeed::LOW_SPEED) ? "Low-Speed" : "Full-Speed",
                  DIAG_PIO_IDX);

    if (!sniffer.begin(PIN_DP, TEST_SPEED, DIAG_PIO_IDX)) {
        Serial.println("  FAIL: sniffer.begin() returned false!");
        Serial.println("    • PIO or DMA resources unavailable");
        printResult("LIBRARY", false);
        return;
    }

    g_diag_pkt_count = 0;
    sniffer.onPacket(diagCallback);

    Serial.println("  Capturing for 5 seconds...");

    uint32_t start = millis();
    uint32_t task_calls = 0;

    while (millis() - start < 5000) {
        sniffer.task();
        task_calls++;
    }

    sniffer.end();

    const auto& d = sniffer.decoder();

    Serial.printf("\n  task() called   : %lu times\n", task_calls);
    Serial.printf("  Packets decoded : %lu\n", d.packets_decoded);
    Serial.printf("  Callback fired  : %lu\n", g_diag_pkt_count);
    Serial.printf("  CRC errors      : %lu\n", d.crc_errors);
    Serial.printf("  SYNC errors     : %lu\n", d.sync_errors);
    Serial.printf("  Stuffing errors : %lu\n", d.stuffing_errors);
    Serial.printf("  Overflow errors : %lu\n", d.overflow_errors);

    Serial.println();

    if (g_diag_pkt_count > 0) {
        Serial.println("  Packets are being decoded — library is working!");
    } else if (d.sync_errors > 0 || d.crc_errors > 0 || d.stuffing_errors > 0) {
        Serial.println("  DIAGNOSIS: Decoder sees activity but can't decode packets.");
        if (d.sync_errors > d.crc_errors * 10) {
            Serial.println("    • Many SYNC errors — clock divider might be wrong");
            Serial.println("    • Or signal quality is poor (add/check series resistors)");
        }
        if (d.stuffing_errors > 0) {
            Serial.println("    • Stuffing errors suggest signal integrity problems");
        }
        if (d.overflow_errors > 0) {
            Serial.println("    • Overflow errors — task() not called fast enough?");
        }
    } else if (task_calls < 10) {
        Serial.println("  DIAGNOSIS: task() barely called — loop too slow.");
    } else {
        Serial.println("  DIAGNOSIS: Zero events — decoder never found SYNC.");
        Serial.println("    • Review Layer 4 output — are there J->K transitions?");
        Serial.println("    • Confirm speed setting matches device (LS vs FS)");
        Serial.println("    • Try the opposite speed setting");
    }

    printResult("LIBRARY", g_diag_pkt_count > 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════════════ */

void initVariant() {
    // set_sys_clock_khz(120000, true);
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) delay(50);

    Serial.println("╔═══════════════════════════════════════════╗");
    Serial.println("║  USBSnifferPIO_RP2040 — Hardware Diagnostic      ║");
    Serial.println("║  Tests each pipeline layer independently  ║");
    Serial.println("╚═══════════════════════════════════════════╝");
    Serial.printf("\n  Config: D+ = GP%u, D- = GP%u, %s, PIO%d\n",
                  PIN_DP, PIN_DM,
                  (TEST_SPEED == USBSpeed::LOW_SPEED) ? "Low-Speed" : "Full-Speed",
                  DIAG_PIO_IDX);
    Serial.printf("  System clock: %lu Hz\n", clock_get_hz(clk_sys));

    testLayer1_GPIO();
    testLayer2_PIO();
    testLayer3_DMA();
    testLayer4_DataAnalysis();
    testLayer5_FullLibrary();

    printSeparator("SUMMARY");
    Serial.println("  Review the FIRST failing layer above.");
    Serial.println("  Each layer depends on the previous ones.\n");
    Serial.println("  If Layer 4 shows all-J (idle) but a device is connected:");
    Serial.println("    → The host may not be polling.  Ensure the USB device");
    Serial.println("      is plugged into an active host port, not just the cable.");
    Serial.println();
    Serial.println("  Diagnostic complete.  Re-upload to run again.");
}

void loop() {
    /* Nothing — diagnostic runs once in setup() */
}
