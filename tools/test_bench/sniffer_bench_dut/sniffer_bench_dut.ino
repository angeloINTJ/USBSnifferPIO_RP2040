/**
 * @file    sniffer_bench_dut.ino
 * @brief   High-speed benchmark firmware (DUT) for USBSnifferPIO_RP2040.
 *
 * Measures the pure capture & decode performance of USBSnifferPIO_RP2040
 * without Serial printing overhead during capture.
 *
 * ## Wiring (Passive Tap on Pico 2 W USB cable)
 *
 *   USB cable D+  ──[100Ω]──► GP2
 *   USB cable D-  ──[100Ω]──► GP3
 *   USB cable GND ───────────► Pico GND
 *   (Do NOT connect VBUS!)
 *
 * ## Serial Control Protocol (115200 baud, single ASCII command)
 *
 *   'P' : Ping -> replies "PONG\n"
 *   'R' : Reset decoder + benchmark counters -> replies "OK_RESET\n"
 *   'S' : Snapshot counters as a single JSON line
 *   'F' : Restart sniffer in FULL_SPEED (12 Mbps) -> replies "OK_FS\n"
 *   'L' : Restart sniffer in LOW_SPEED  (1.5 Mbps) -> replies "OK_LS\n"
 *
 * ## Build & Upload (Raspberry Pi Pico — RP2040)
 *
 *   arduino-cli compile --fqbn rp2040:rp2040:rpipico --library . tools/test_bench/sniffer_bench_dut
 *   arduino-cli upload  --fqbn rp2040:rp2040:rpipico -p /dev/ttyACM0 tools/test_bench/sniffer_bench_dut
 */

#include <USBSnifferPIO_RP2040.h>

#define PIN_DP            2
#define SNIFFER_PIO_INDEX 1

static constexpr uint8_t MAGIC_0 = 0xA5;
static constexpr uint8_t MAGIC_1 = 0x5A;

USBSnifferPIO sniffer;

/* ── Shared benchmark counters (Core 1 writes, Core 0 reads) ─────────────── */
static volatile bool     g_reset_req    = false;
static volatile uint8_t  g_speed_req    = 0;   // 0=none, 1=FS, 2=LS
static volatile bool     g_is_fs        = true;

static volatile uint32_t g_tok_pkts     = 0;
static volatile uint32_t g_data_pkts    = 0;
static volatile uint32_t g_hs_pkts      = 0;

static volatile uint32_t g_gen_seen     = 0;
static volatile uint32_t g_gen_first    = 0;
static volatile uint32_t g_gen_last     = 0;
static volatile bool     g_gen_has_seq  = false;
static volatile uint32_t g_gen_gaps     = 0;
static volatile uint32_t g_gen_dups     = 0;
static volatile uint32_t g_gen_crc_fail = 0;

static void resetBenchCounters() {
    g_tok_pkts     = 0;
    g_data_pkts    = 0;
    g_hs_pkts      = 0;
    g_gen_seen     = 0;
    g_gen_first    = 0;
    g_gen_last     = 0;
    g_gen_has_seq  = false;
    g_gen_gaps     = 0;
    g_gen_dups     = 0;
    g_gen_crc_fail = 0;
}

/* ── Core 1 packet callback (SRAM + -O3, zero Serial I/O) ────────────────── */
void __attribute__((optimize("O3"), section(".time_critical.usb_cb")))
onPacket(const USBPacket& pkt) {
    switch (pkt.type) {
        case USBPacketType::TOKEN:     g_tok_pkts++;  return;
        case USBPacketType::HANDSHAKE: g_hs_pkts++;   return;
        case USBPacketType::DATA:      g_data_pkts++; break;
        default: return;
    }

    // Scan DATA payload for generator frames (0xA5 0x5A + uint32 seq + len + pat)
    if (pkt.data_length < 8) return;

    uint8_t i = 0;
    while (i + 8 <= pkt.data_length) {
        if (pkt.data[i] == MAGIC_0 && pkt.data[i + 1] == MAGIC_1) {
            uint8_t plen = pkt.data[i + 6];
            if ((plen == 8 || plen == 16 || plen == 32 || plen == 64) &&
                (i + plen <= pkt.data_length))
            {
                if (!pkt.crc_valid) {
                    g_gen_crc_fail++;
                } else {
                    uint32_t seq = (uint32_t)pkt.data[i + 2]
                                 | ((uint32_t)pkt.data[i + 3] << 8)
                                 | ((uint32_t)pkt.data[i + 4] << 16)
                                 | ((uint32_t)pkt.data[i + 5] << 24);
                    if (!g_gen_has_seq) {
                        g_gen_first   = seq;
                        g_gen_last    = seq;
                        g_gen_has_seq = true;
                    } else if (seq == g_gen_last) {
                        g_gen_dups++;
                    } else if (seq > g_gen_last + 1) {
                        g_gen_gaps += (seq - g_gen_last - 1);
                        g_gen_last  = seq;
                    } else if (seq == g_gen_last + 1) {
                        g_gen_last  = seq;
                    } else {
                        // Out-of-order / duplicate older seq
                        g_gen_dups++;
                    }
                    g_gen_seen++;
                }
                i += plen;
                continue;
            }
        }
        i++;
    }
}

/* ── Core 0: Serial command interface ────────────────────────────────────── */

static void printSnapshotJSON() {
    const auto& d = sniffer.decoder();
    Serial.printf(
        "{\"uptime_ms\":%lu,\"speed\":\"%s\",\"running\":%d,"
        "\"packets_decoded\":%lu,\"tok_pkts\":%lu,\"data_pkts\":%lu,\"hs_pkts\":%lu,"
        "\"crc_errors\":%lu,\"sync_errors\":%lu,\"stuffing_errors\":%lu,\"overflow_errors\":%lu,\"fifo_overruns\":%lu,"
        "\"gen_seen\":%lu,\"gen_has_seq\":%d,\"gen_first\":%lu,\"gen_last\":%lu,"
        "\"gen_gaps\":%lu,\"gen_dups\":%lu,\"gen_crc_fail\":%lu}\n",
        (unsigned long)millis(),
        g_is_fs ? "FS" : "LS",
        sniffer.isRunning() ? 1 : 0,
        (unsigned long)d.packets_decoded,
        (unsigned long)g_tok_pkts,
        (unsigned long)g_data_pkts,
        (unsigned long)g_hs_pkts,
        (unsigned long)d.crc_errors,
        (unsigned long)d.sync_errors,
        (unsigned long)d.stuffing_errors,
        (unsigned long)d.overflow_errors,
        (unsigned long)d.fifo_overruns,
        (unsigned long)g_gen_seen,
        g_gen_has_seq ? 1 : 0,
        (unsigned long)g_gen_first,
        (unsigned long)g_gen_last,
        (unsigned long)g_gen_gaps,
        (unsigned long)g_gen_dups,
        (unsigned long)g_gen_crc_fail
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
    while (Serial.available() > 0) {
        char cmd = (char)Serial.read();
        switch (cmd) {
            case 'P':
                Serial.println("PONG");
                break;

            case 'R':
                g_reset_req = true;
                while (g_reset_req) delay(1);
                Serial.println("OK_RESET");
                break;

            case 'S':
                printSnapshotJSON();
                break;

            case 'F':
                g_speed_req = 1;
                while (g_speed_req != 0) delay(1);
                Serial.println("OK_FS");
                break;

            case 'L':
                g_speed_req = 2;
                while (g_speed_req != 0) delay(1);
                Serial.println("OK_LS");
                break;

            default:
                break;
        }
    }
}

/* ── Core 1: Capture engine ──────────────────────────────────────────────── */

void setup1() {
    g_is_fs = true;
    sniffer.begin(PIN_DP, USBSpeed::FULL_SPEED, SNIFFER_PIO_INDEX);
    sniffer.onPacket(onPacket);
}

void loop1() {
    if (g_speed_req != 0) {
        uint8_t req = g_speed_req;
        sniffer.end();
        resetBenchCounters();
        g_is_fs = (req == 1);
        sniffer.begin(PIN_DP, g_is_fs ? USBSpeed::FULL_SPEED : USBSpeed::LOW_SPEED, SNIFFER_PIO_INDEX);
        sniffer.onPacket(onPacket);
        g_speed_req = 0;
        return;
    }

    if (g_reset_req) {
        sniffer.decoder().reset();
        resetBenchCounters();
        g_reset_req = false;
        return;
    }

    sniffer.task();
}
