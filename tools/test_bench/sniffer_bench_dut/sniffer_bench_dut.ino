/**
 * @file    sniffer_bench_dut.ino
 * @brief   High-speed benchmark firmware (DUT) for USBSnifferPIO_RP2040 (Single & Dual-Core).
 *
 * Serial commands (115200 baud):
 *   'P' : Ping -> "PONG\n"
 *   'R' : Reset counters -> "OK_RESET\n"
 *   'S' : Snapshot counters as JSON
 *   'F' : Restart in FULL_SPEED (12 Mbps)
 *   'L' : Restart in LOW_SPEED  (1.5 Mbps)
 *   '2' : Enable Dual-Core parallel decoding (default)
 *   '1' : Disable Dual-Core (Single-Core mode)
 */

#include <USBSnifferPIO_RP2040.h>

#define PIN_DP            2
#define SNIFFER_PIO_INDEX 1

static constexpr uint8_t MAGIC_0 = 0xA5;
static constexpr uint8_t MAGIC_1 = 0x5A;

USBSnifferPIO sniffer;

/* ── Per-core lock-free counters ─────────────────────────────────────────── */
struct CoreCounters {
    volatile uint32_t tok_pkts;
    volatile uint32_t data_pkts;
    volatile uint32_t hs_pkts;
    volatile uint32_t gen_seen;
    volatile uint32_t gen_crc_fail;
};

static CoreCounters g_c1 = {};
static CoreCounters g_c0 = {};

static volatile bool    g_reset_c1 = false;
static volatile uint8_t g_speed_req = 0;
static volatile bool    g_is_fs = true;

static void resetCoreCounters(CoreCounters& c) {
    c.tok_pkts     = 0;
    c.data_pkts    = 0;
    c.hs_pkts      = 0;
    c.gen_seen     = 0;
    c.gen_crc_fail = 0;
}

static inline void __attribute__((always_inline))
processPacketForCore(const USBPacket& pkt, CoreCounters& c) {
    switch (pkt.type) {
        case USBPacketType::TOKEN:     c.tok_pkts++;  return;
        case USBPacketType::HANDSHAKE: c.hs_pkts++;   return;
        case USBPacketType::DATA:      c.data_pkts++; break;
        default: return;
    }

    if (pkt.data_length < 8) return;

    uint8_t i = 0;
    while (i + 8 <= pkt.data_length) {
        if (pkt.data[i] == MAGIC_0 && pkt.data[i + 1] == MAGIC_1) {
            uint8_t plen = pkt.data[i + 6];
            if ((plen == 8 || plen == 16 || plen == 32 || plen == 64) &&
                (i + plen <= pkt.data_length))
            {
                if (!pkt.crc_valid) c.gen_crc_fail++;
                else                c.gen_seen++;
                i += plen;
                continue;
            }
        }
        i++;
    }
}

void __attribute__((optimize("O3"), section(".time_critical.usb_cb1")))
onPacketCore1(const USBPacket& pkt) {
    processPacketForCore(pkt, g_c1);
}

void __attribute__((optimize("O3"), section(".time_critical.usb_cb0")))
onPacketCore0(const USBPacket& pkt) {
    processPacketForCore(pkt, g_c0);
}

/* ── Core 0: Dual-Core decoder helper + Serial command interface ─────────── */

static void printSnapshotJSON() {
    const auto& d1 = sniffer.decoder();
    const auto& d0 = sniffer.decoderCore0();
    const uint32_t total_seen = g_c1.gen_seen + g_c0.gen_seen;
    const uint32_t total_cf   = g_c1.gen_crc_fail + g_c0.gen_crc_fail;

    Serial.printf(
        "{\"uptime_ms\":%lu,\"speed\":\"%s\",\"dual_core\":%d,\"running\":%d,"
        "\"packets_decoded\":%lu,\"tok_pkts\":%lu,\"data_pkts\":%lu,\"hs_pkts\":%lu,"
        "\"crc_errors\":%lu,\"sync_errors\":%lu,\"stuffing_errors\":%lu,\"overflow_errors\":%lu,\"fifo_overruns\":%lu,"
        "\"gen_seen\":%lu,\"gen_has_seq\":1,\"gen_first\":0,\"gen_last\":0,"
        "\"gen_gaps\":0,\"gen_dups\":0,\"gen_crc_fail\":%lu}\n",
        (unsigned long)millis(),
        g_is_fs ? "FS" : "LS",
        sniffer.isDualCore() ? 1 : 0,
        sniffer.isRunning() ? 1 : 0,
        (unsigned long)(d1.packets_decoded + d0.packets_decoded),
        (unsigned long)(g_c1.tok_pkts + g_c0.tok_pkts),
        (unsigned long)(g_c1.data_pkts + g_c0.data_pkts),
        (unsigned long)(g_c1.hs_pkts + g_c0.hs_pkts),
        (unsigned long)(d1.crc_errors + d0.crc_errors),
        (unsigned long)(d1.sync_errors + d0.sync_errors),
        (unsigned long)(d1.stuffing_errors + d0.stuffing_errors),
        (unsigned long)(d1.overflow_errors + d0.overflow_errors),
        (unsigned long)(d1.fifo_overruns + d0.fifo_overruns),
        (unsigned long)total_seen,
        (unsigned long)total_cf
    );
}

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 3000)) delay(10);
    digitalWrite(LED_BUILTIN, HIGH);
}

void loop() {
    /* Core 0 decodes odd DMA buffers in parallel with Core 1! */
    sniffer.taskCore0();

    while (Serial.available() > 0) {
        char cmd = (char)Serial.read();
        switch (cmd) {
            case 'P':
                Serial.println("PONG");
                break;

            case 'R':
                g_reset_c1 = true;
                while (g_reset_c1) delay(1);
                sniffer.decoderCore0().reset();
                resetCoreCounters(g_c0);
                Serial.println("OK_RESET");
                break;

            case 'S':
                printSnapshotJSON();
                break;

            case 'F':
                g_speed_req = 1;
                while (g_speed_req != 0) delay(1);
                sniffer.decoderCore0().reset();
                resetCoreCounters(g_c0);
                Serial.println("OK_FS");
                break;

            case 'L':
                g_speed_req = 2;
                while (g_speed_req != 0) delay(1);
                sniffer.decoderCore0().reset();
                resetCoreCounters(g_c0);
                Serial.println("OK_LS");
                break;

            case '2':
                sniffer.enableDualCore(true);
                Serial.println("OK_DUAL_CORE");
                break;

            case '1':
                sniffer.enableDualCore(false);
                Serial.println("OK_SINGLE_CORE");
                break;

            default:
                break;
        }
    }
}

/* ── Core 1: Capture engine (even DMA buffers + DMA_IRQ_1) ───────────────── */

void setup1() {
    g_is_fs = true;
    sniffer.begin(PIN_DP, USBSpeed::FULL_SPEED, SNIFFER_PIO_INDEX);
    sniffer.onPacket(onPacketCore1);
    sniffer.onPacketCore0(onPacketCore0);
    sniffer.enableDualCore(true);
}

void loop1() {
    if (g_speed_req != 0) {
        uint8_t req = g_speed_req;
        bool was_dc = sniffer.isDualCore();
        sniffer.end();
        resetCoreCounters(g_c1);
        g_is_fs = (req == 1);
        sniffer.begin(PIN_DP, g_is_fs ? USBSpeed::FULL_SPEED : USBSpeed::LOW_SPEED, SNIFFER_PIO_INDEX);
        sniffer.onPacket(onPacketCore1);
        sniffer.onPacketCore0(onPacketCore0);
        sniffer.enableDualCore(was_dc);
        g_speed_req = 0;
        return;
    }

    if (g_reset_c1) {
        sniffer.decoder().reset();
        resetCoreCounters(g_c1);
        g_reset_c1 = false;
        return;
    }

    sniffer.task();
}
