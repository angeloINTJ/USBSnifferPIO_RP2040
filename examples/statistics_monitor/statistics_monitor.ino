/**
 * @file statistics_monitor.ino
 * @brief Real-time USB bus statistics and health monitor.
 *
 * Captures all USB traffic silently and displays a periodically
 * updated dashboard with packet counts, error rates, and bus
 * utilization estimates.
 *
 * Useful for diagnosing signal integrity issues, verifying
 * the sniffer hardware setup, or monitoring bus load.
 *
 * ## Wiring
 *
 *   USB cable D+  ──[100Ω]──► GP2
 *   USB cable D-  ──[100Ω]──► GP3
 *   USB cable GND ──────────► Pico GND
 *
 * ## Dashboard (updated every 2 seconds)
 *
 *   ┌─────── USB Bus Monitor ─────────┐
 *   │ Uptime        : 00:01:23        │
 *   │ Total packets : 145230          │
 *   │ SOF frames    : 62000           │
 *   │ Data packets  : 31000           │
 *   │ Handshakes    : 31000           │
 *   │ CRC errors    : 0    (0.000%)   │
 *   │ SYNC errors   : 2    (0.001%)   │
 *   │ Stuff errors  : 0    (0.000%)   │
 *   │ SOF rate      : 1000.0 /s       │
 *   └─────────────────────────────────┘
 *
 * @note Baud rate: 115200.
 */

#include <USBSnifferPIO_RP2040.h>

#define PIN_DP  2

USBSnifferPIO sniffer;

/// @brief Dashboard update interval (milliseconds).
#define DASHBOARD_INTERVAL_MS  2000

/* ═══════════════════════════════════════════════════════════════════════════
 *  PER-TYPE COUNTERS (updated from Core 1)
 * ═══════════════════════════════════════════════════════════════════════════ */

volatile uint32_t g_sof_count        = 0;
volatile uint32_t g_token_count      = 0;
volatile uint32_t g_data_count       = 0;
volatile uint32_t g_handshake_count  = 0;
volatile uint32_t g_hid_report_count = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 *  PACKET CALLBACK (Core 1)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Count each packet by type. No serial output here.
 * @param[in] pkt Decoded USB packet.
 */
void onPacket(const USBPacket& pkt) {
    switch (pkt.type) {
        case USBPacketType::TOKEN:
            if (pkt.pid == USBPID::SOF) g_sof_count++;
            else g_token_count++;
            break;
        case USBPacketType::DATA:
            g_data_count++;
            if (pkt.isHIDKeyboardReport()) g_hid_report_count++;
            break;
        case USBPacketType::HANDSHAKE:
            g_handshake_count++;
            break;
        default:
            break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE 0 — DASHBOARD
 * ═══════════════════════════════════════════════════════════════════════════ */

/// @brief Previous snapshot for rate calculation.
uint32_t prev_sof = 0;
uint32_t prev_total = 0;

/**
 * @brief Hook executed before TinyUSB connects to the host.
 *
 * This is the correct place to change the system clock when using
 * USB serial (CDC).  Calling set_sys_clock_khz() in setup() changes
 * the clock AFTER USB enumeration, disrupting SysTick and dropping
 * the serial connection.
 *
 * USBSnifferPIO detects the clock automatically via clock_get_hz(),
 * so the line below is OPTIONAL — only uncomment if you need
 * exactly 120 MHz for another reason.
 */
void initVariant() {
    // set_sys_clock_khz(120000, true);   /* uncomment if needed */
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) delay(50);

    Serial.println("USBSnifferPIO_RP2040 — Statistics Monitor");
    Serial.println("Dashboard updates every 2 seconds.\n");
}

/**
 * @brief Format an uptime value as HH:MM:SS.
 * @param[in] ms Milliseconds since boot.
 * @param[out] buf Output buffer (at least 9 chars).
 */
void formatUptime(unsigned long ms, char* buf) {
    unsigned long sec = ms / 1000;
    uint8_t h = sec / 3600;
    uint8_t m = (sec % 3600) / 60;
    uint8_t s = sec % 60;
    sprintf(buf, "%02u:%02u:%02u", h, m, s);
}

/**
 * @brief Compute error percentage (safe against divide-by-zero).
 * @param[in] errors Error count.
 * @param[in] total  Total count.
 * @return Percentage as float.
 */
float errorPct(uint32_t errors, uint32_t total) {
    return (total > 0) ? (errors * 100.0f / total) : 0.0f;
}

void loop() {
    static unsigned long last_update = 0;

    if (millis() - last_update < DASHBOARD_INTERVAL_MS) return;
    last_update = millis();

    /* Snapshot counters (volatile reads) */
    const auto& d          = sniffer.decoder();
    uint32_t total_decoded = d.packets_decoded;
    uint32_t crc_err       = d.crc_errors;
    uint32_t sync_err      = d.sync_errors;
    uint32_t stuff_err     = d.stuffing_errors;
    uint32_t overflow_err  = d.overflow_errors;

    uint32_t sof   = g_sof_count;
    uint32_t token = g_token_count;
    uint32_t data  = g_data_count;
    uint32_t hs    = g_handshake_count;
    uint32_t hid   = g_hid_report_count;

    /* Rates */
    float sof_rate   = (sof - prev_sof) * 1000.0f / DASHBOARD_INTERVAL_MS;
    float pkt_rate   = (total_decoded - prev_total) * 1000.0f / DASHBOARD_INTERVAL_MS;
    prev_sof   = sof;
    prev_total = total_decoded;

    /* Uptime */
    char uptime[12];
    formatUptime(millis(), uptime);

    /* ── Print dashboard ──────────────────────────────────────── */
    Serial.println("┌──────────── USB Bus Monitor ────────────┐");
    Serial.printf( "│ Uptime         : %-22s│\n", uptime);
    Serial.printf( "│ Total packets  : %-22lu│\n", total_decoded);
    Serial.printf( "│ Packet rate    : %-18.1f /s  │\n", pkt_rate);
    Serial.println("│                                         │");
    Serial.printf( "│ SOF frames     : %-22lu│\n", sof);
    Serial.printf( "│ SOF rate       : %-18.1f /s  │\n", sof_rate);
    Serial.printf( "│ IN/OUT/SETUP   : %-22lu│\n", token);
    Serial.printf( "│ DATA packets   : %-22lu│\n", data);
    Serial.printf( "│ Handshakes     : %-22lu│\n", hs);
    Serial.printf( "│ HID reports    : %-22lu│\n", hid);
    Serial.println("│                                         │");
    Serial.printf( "│ CRC errors     : %-8lu (%6.3f%%)    │\n",
                   crc_err, errorPct(crc_err, total_decoded));
    Serial.printf( "│ SYNC errors    : %-8lu (%6.3f%%)    │\n",
                   sync_err, errorPct(sync_err, total_decoded + sync_err));
    Serial.printf( "│ Stuffing errors: %-8lu (%6.3f%%)    │\n",
                   stuff_err, errorPct(stuff_err, total_decoded));
    Serial.printf( "│ Overflow errors: %-22lu│\n", overflow_err);
    Serial.println("└─────────────────────────────────────────┘\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE 1 — CAPTURE
 * ═══════════════════════════════════════════════════════════════════════════ */

void setup1() {
    sniffer.begin(PIN_DP, USBSpeed::LOW_SPEED, 1);
    sniffer.onPacket(onPacket);
}

void loop1() { sniffer.task(); }
