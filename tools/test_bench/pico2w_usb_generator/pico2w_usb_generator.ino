/**
 * @file    pico2w_usb_generator.ino
 * @brief   Deterministic USB 1.1 Full-Speed packet generator for Raspberry Pi Pico 2 W.
 *
 * Designed as a calibrated traffic source for benchmarking USBSnifferPIO_RP2040
 * against Wireshark (usbmon / USBPcap).
 *
 * ## Packet Binary Format (8, 16, 32, or 64 bytes)
 *
 *   Byte 0..1 : Magic header  (0xA5, 0x5A) — easy to filter in Wireshark & Sniffer
 *   Byte 2..5 : uint32_t seq  (little-endian sequence number: 0, 1, 2, ...)
 *   Byte 6    : uint8_t len   (total packet length in bytes: 8, 16, 32, 64)
 *   Byte 7    : uint8_t pat   (payload pattern ID: 0=INC, 1=0xFF, 2=0x00, 3=0x55)
 *   Byte 8..N : Payload fill  (based on pattern ID)
 *
 * ## Control Commands (send single ASCII char over USB Serial)
 *
 *   'b' : Deterministic burst of 100 packets (at current rate)
 *   'B' : Deterministic burst of 1000 packets (at current rate)
 *   's' : Start / Stop continuous streaming
 *   'r' : Reset sequence counter to 0
 *   '1' : Rate =    10 pkt/s
 *   '2' : Rate =   100 pkt/s  (default)
 *   '3' : Rate =   500 pkt/s
 *   '4' : Rate =  1000 pkt/s
 *   '5' : Rate =  5000 pkt/s
 *   '6' : Rate =   MAX speed  (no inter-packet delay — saturates FS Bulk IN)
 *   'l' : Cycle packet length (8 -> 16 -> 32 -> 64 bytes)
 *   'p' : Cycle payload pattern (INC -> 0xFF [bit-stuffing stress] -> 0x00 -> 0x55)
 *   '?' : Print current configuration (only when stopped)
 *
 * ## Build & Upload (arduino-cli)
 *
 *   arduino-cli compile --fqbn rp2040:rp2040:rpipico2w tools/test_bench/pico2w_usb_generator
 *   arduino-cli upload  --fqbn rp2040:rp2040:rpipico2w -p /dev/ttyACM1 tools/test_bench/pico2w_usb_generator
 */

#include <Arduino.h>

/* ── Packet format constants ─────────────────────────────────────────────── */
static constexpr uint8_t MAGIC_0 = 0xA5;
static constexpr uint8_t MAGIC_1 = 0x5A;
static constexpr uint8_t MAX_PKT_LEN = 64;

/* ── Configurable generator state ────────────────────────────────────────── */
enum PatternMode : uint8_t {
    PAT_INC  = 0,  // 0x08, 0x09, 0x0A...
    PAT_FF   = 1,  // 0xFF (worst-case USB bit-stuffing!)
    PAT_00   = 2,  // 0x00 (maximum NRZI transitions)
    PAT_55   = 3,  // 0x55 (alternating bits)
    PAT_COUNT = 4
};

static const uint8_t  kLengths[] = { 8, 16, 32, 64 };
static uint8_t        g_len_idx  = 1;        // default: 16 bytes
static uint8_t        g_pat_mode = PAT_INC;  // default: incremental

static uint32_t       g_rate_hz       = 100;     // default: 100 pkt/s (0 = MAX)
static uint32_t       g_interval_us   = 10000;   // 1_000_000 / g_rate_hz
static bool           g_continuous    = false;
static uint32_t       g_burst_left    = 0;

static uint32_t       g_seq           = 0;
static uint32_t       g_last_tx_us    = 0;
static uint8_t        g_pkt_buf[MAX_PKT_LEN];

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static const char* patternName(uint8_t p) {
    switch (p) {
        case PAT_INC: return "INC";
        case PAT_FF:  return "0xFF (bit-stuffing stress)";
        case PAT_00:  return "0x00 (max NRZI transitions)";
        case PAT_55:  return "0x55 (alternating)";
        default:      return "UNKNOWN";
    }
}

static void setRate(uint32_t hz) {
    g_rate_hz     = hz;
    g_interval_us = (hz == 0) ? 0 : (1000000UL / hz);
}

static void printStatus() {
    Serial.print("\r\n[GEN] ");
    Serial.print("state=");
    Serial.print(g_continuous ? "CONTINUOUS" : (g_burst_left ? "BURST" : "IDLE"));
    Serial.print(" | rate=");
    if (g_rate_hz == 0) Serial.print("MAX");
    else { Serial.print(g_rate_hz); Serial.print(" pkt/s"); }
    Serial.print(" | len=");
    Serial.print(kLengths[g_len_idx]);
    Serial.print("B | pat=");
    Serial.print(patternName(g_pat_mode));
    Serial.print(" | next_seq=");
    Serial.println(g_seq);
}

static void buildAndSendPacket() {
    const uint8_t len = kLengths[g_len_idx];

    g_pkt_buf[0] = MAGIC_0;
    g_pkt_buf[1] = MAGIC_1;
    g_pkt_buf[2] = (uint8_t)(g_seq & 0xFF);
    g_pkt_buf[3] = (uint8_t)((g_seq >> 8) & 0xFF);
    g_pkt_buf[4] = (uint8_t)((g_seq >> 16) & 0xFF);
    g_pkt_buf[5] = (uint8_t)((g_seq >> 24) & 0xFF);
    g_pkt_buf[6] = len;
    g_pkt_buf[7] = g_pat_mode;

    for (uint8_t i = 8; i < len; i++) {
        switch (g_pat_mode) {
            case PAT_INC: g_pkt_buf[i] = i;    break;
            case PAT_FF:  g_pkt_buf[i] = 0xFF; break;
            case PAT_00:  g_pkt_buf[i] = 0x00; break;
            case PAT_55:  g_pkt_buf[i] = 0x55; break;
            default:      g_pkt_buf[i] = 0x00; break;
        }
    }

    // Write as a single USB CDC Bulk IN packet and flush immediately so the
    // USB stack does not coalesce multiple logical packets into one frame.
    if (Serial.availableForWrite() >= len) {
        Serial.write(g_pkt_buf, len);
        Serial.flush();
        g_seq++;
    }
}

static void handleCommand(char cmd) {
    switch (cmd) {
        case 'b':
            g_continuous = false;
            g_burst_left = 100;
            g_last_tx_us = micros();
            break;

        case 'B':
            g_continuous = false;
            g_burst_left = 1000;
            g_last_tx_us = micros();
            break;

        case 's':
            g_burst_left = 0;
            g_continuous = !g_continuous;
            g_last_tx_us = micros();
            if (!g_continuous) printStatus();
            break;

        case 'r':
            g_seq = 0;
            if (!g_continuous && g_burst_left == 0) printStatus();
            break;

        case '1': setRate(10);   if (!g_continuous) printStatus(); break;
        case '2': setRate(100);  if (!g_continuous) printStatus(); break;
        case '3': setRate(500);  if (!g_continuous) printStatus(); break;
        case '4': setRate(1000); if (!g_continuous) printStatus(); break;
        case '5': setRate(5000); if (!g_continuous) printStatus(); break;
        case '6': setRate(0);    if (!g_continuous) printStatus(); break;

        case 'l':
            g_len_idx = (g_len_idx + 1) % (sizeof(kLengths) / sizeof(kLengths[0]));
            if (!g_continuous) printStatus();
            break;

        case 'p':
            g_pat_mode = (g_pat_mode + 1) % PAT_COUNT;
            if (!g_continuous) printStatus();
            break;

        /* Absolute commands (deterministic for automated 8h stress bench) */
        case '0': g_continuous = false; g_burst_left = 0; break;
        case 'a': g_len_idx = 0; break;  // 8 B
        case 'c': g_len_idx = 1; break;  // 16 B
        case 'd': g_len_idx = 2; break;  // 32 B
        case 'e': g_len_idx = 3; break;  // 64 B
        case 'w': g_pat_mode = PAT_INC; break;
        case 'x': g_pat_mode = PAT_FF;  break;
        case 'y': g_pat_mode = PAT_00;  break;
        case 'z': g_pat_mode = PAT_55;  break;

        case '?':
        case 'h':
            if (!g_continuous && g_burst_left == 0) printStatus();
            break;

        default:
            break;
    }
}

/* ── Arduino entry points ────────────────────────────────────────────────── */

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    Serial.begin(115200);
    // Wait up to 3 s for USB CDC host connection
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 3000)) {
        delay(10);
    }
    digitalWrite(LED_BUILTIN, HIGH);
}

void loop() {
    while (Serial.available() > 0) {
        handleCommand((char)Serial.read());
    }

    const bool active = g_continuous || (g_burst_left > 0);
    if (!active) return;

    const uint32_t now = micros();
    if (g_interval_us == 0 || (uint32_t)(now - g_last_tx_us) >= g_interval_us) {
        g_last_tx_us = now;

        const uint32_t prev_seq = g_seq;
        buildAndSendPacket();

        // Only decrement burst counter if the packet was actually sent
        if (g_burst_left > 0 && g_seq != prev_seq) {
            g_burst_left--;
        }
    }
}
