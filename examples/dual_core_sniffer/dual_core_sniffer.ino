/**
 * @file    dual_core_sniffer.ino
 * @brief   High-throughput Dual-Core Full-Speed USB 1.1 sniffer (200 MHz, zero overclock).
 *
 * Demonstrates USBSnifferPIO_RP2040 v1.2.0 Dual-Core striped parallel decoding:
 *   - Core 1 decodes even DMA buffers (slots 0, 2) via sniffer.task()
 *   - Core 0 decodes odd  DMA buffers (slots 1, 3) via sniffer.taskCore0()
 *   - Achieves 100% lossless Full-Speed (12 Mbps) capture at the official 200 MHz clock
 *
 * ## Wiring
 *
 *   USB cable D+  ──[100Ω]──► GP2
 *   USB cable D-  ──[100Ω]──► GP3
 *   USB cable GND ───────────► Pico GND
 *   (Do NOT connect VBUS!)
 *
 * ## Important
 *
 *   In Dual-Core mode, keep Core 0 non-blocking so taskCore0() runs at least
 *   once every ~5 ms. Print periodic summaries instead of printing every packet.
 */

#include <USBSnifferPIO_RP2040.h>

#define PIN_DP  2

USBSnifferPIO sniffer;

/* Per-core lock-free counters (zero cross-core contention) */
static volatile uint32_t g_data_c1 = 0;
static volatile uint32_t g_data_c0 = 0;

static void __attribute__((optimize("O3"), section(".time_critical.cb1"))) onPacketCore1(const USBPacket& pkt) {
    if (pkt.type == USBPacketType::DATA && pkt.crc_valid) {
        g_data_c1++;
    }
}

static void __attribute__((optimize("O3"), section(".time_critical.cb0"))) onPacketCore0(const USBPacket& pkt) {
    if (pkt.type == USBPacketType::DATA && pkt.crc_valid) {
        g_data_c0++;
    }
}

/* ── Core 0: Parallel odd-buffer decoder + periodic stats ────────────────── */

static uint32_t g_last_report_ms = 0;

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 3000)) delay(10);

    Serial.println("USBSnifferPIO_RP2040 — Dual-Core Full-Speed Sniffer");
    Serial.println("Reporting aggregated stats every 2 seconds...\n");
}

void loop() {
    /* Decode odd DMA buffers in parallel with Core 1 */
    sniffer.taskCore0();

    uint32_t now = millis();
    if (now - g_last_report_ms >= 2000) {
        g_last_report_ms = now;
        USBSnifferStats st = sniffer.stats();
        Serial.printf(
            "[%lu s] clk=%lu MHz | dual=%d | pkts=%lu | DATA_OK=%lu | crc=%lu sync=%lu stuff=%lu ovf=%lu fifo=%lu\n",
            (unsigned long)(now / 1000),
            (unsigned long)(st.sys_clock_hz / 1000000UL),
            st.dual_core ? 1 : 0,
            (unsigned long)st.packets_decoded,
            (unsigned long)(g_data_c1 + g_data_c0),
            (unsigned long)st.crc_errors,
            (unsigned long)st.sync_errors,
            (unsigned long)st.stuffing_errors,
            (unsigned long)st.overflow_errors,
            (unsigned long)st.fifo_overruns
        );
    }
}

/* ── Core 1: PIO + DMA capture + even-buffer decoder ─────────────────────── */

void setup1() {
    sniffer.begin(PIN_DP, USBSpeed::FULL_SPEED, 1);
    sniffer.onPacket(onPacketCore1);
    sniffer.onPacketCore0(onPacketCore0);
    sniffer.enableDualCore(true);
}

void loop1() {
    sniffer.task();
}
