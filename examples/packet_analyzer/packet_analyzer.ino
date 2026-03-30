/**
 * @file packet_analyzer.ino
 * @brief Full USB protocol analyzer with hex dump and transaction grouping.
 *
 * Prints every captured packet with full detail: PID, address, endpoint,
 * payload hex dump, and CRC status.  Also groups consecutive packets
 * into USB transactions (SETUP/IN/OUT + DATA + ACK/NAK).
 *
 * Useful for understanding low-level USB communication, debugging
 * enumeration sequences, or reverse-engineering device protocols.
 *
 * ## Wiring
 *
 *   USB cable D+  ──[100Ω]──► GP2
 *   USB cable D-  ──[100Ω]──► GP3
 *   USB cable GND ──────────► Pico GND
 *
 * ## Serial commands
 *
 *   'p' — Pause / resume capture display
 *   's' — Print statistics summary
 *   'r' — Reset statistics counters
 *   'f' — Toggle SOF filtering (SOF packets are very frequent)
 *
 * ## Threading model
 *
 *   Core 1 captures packets and copies them into a ring buffer.
 *   Core 0 drains the ring buffer and prints to Serial.
 *   Serial is NEVER accessed from Core 1.
 *
 * @note Baud rate: 115200.
 */

#include <USBSnifferPIO_RP2040.h>

#define PIN_DP  2

USBSnifferPIO sniffer;

/// @brief Display control flags (set from Core 0, read from Core 1).
volatile bool g_paused     = false;
volatile bool g_filter_sof = true;   ///< Filter out SOF by default

/* ═══════════════════════════════════════════════════════════════════════════
 *  LOCK-FREE RING BUFFER (Core 1 writes, Core 0 reads)
 * ═══════════════════════════════════════════════════════════════════════════ */

/// @brief Ring buffer depth (must be power of 2).
#define PKT_RING_SIZE  64
#define PKT_RING_MASK  (PKT_RING_SIZE - 1)

/// @brief Ring buffer of captured packets.
static USBPacket  g_ring[PKT_RING_SIZE];

/// @brief Write index (only modified by Core 1).
static volatile uint32_t g_ring_head = 0;

/// @brief Read index (only modified by Core 0).
static volatile uint32_t g_ring_tail = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 *  PID HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Return a fixed-width PID label.
 * @param[in] pid Lower 4 bits of PID.
 * @return 5-char null-terminated string.
 */
const char* pidLabel(uint8_t pid) {
    switch (pid) {
        case USBPID::OUT:   return "OUT  ";
        case USBPID::IN:    return "IN   ";
        case USBPID::SOF:   return "SOF  ";
        case USBPID::SETUP: return "SETUP";
        case USBPID::DATA0: return "DATA0";
        case USBPID::DATA1: return "DATA1";
        case USBPID::ACK:   return "ACK  ";
        case USBPID::NAK:   return "NAK  ";
        case USBPID::STALL: return "STALL";
        case USBPID::PRE:   return "PRE  ";
        default:             return "?????";
    }
}

/**
 * @brief Return an indentation prefix based on packet type.
 *
 * Token packets start a transaction (no indent), DATA packets
 * are indented once, and handshakes twice.
 */
const char* indentForType(USBPacketType type) {
    switch (type) {
        case USBPacketType::TOKEN:     return "";
        case USBPacketType::DATA:      return "  ";
        case USBPacketType::HANDSHAKE: return "    ";
        default:                        return "";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PACKET CALLBACK (Core 1 — NO Serial access here)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Copy captured packets into the ring buffer (Core 1).
 *
 * Filtering by pause state and SOF is done here to avoid
 * filling the ring with packets the user doesn't want to see.
 */
void onPacket(const USBPacket& pkt) {
    if (g_paused) return;
    if (g_filter_sof && pkt.pid == USBPID::SOF) return;

    uint32_t head = g_ring_head;
    g_ring[head & PKT_RING_MASK] = pkt;
    g_ring_head = head + 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE 0 — UI & SERIAL OUTPUT
 * ═══════════════════════════════════════════════════════════════════════════ */

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

    Serial.println("╔══════════════════════════════════╗");
    Serial.println("║  USBSnifferPIO_RP2040 — Packet Analyzer ║");
    Serial.println("╚══════════════════════════════════╝");
    Serial.println();
    Serial.println("Commands:  p=pause  s=stats  r=reset  f=toggle SOF");
    Serial.println("SOF filter: ON (press 'f' to show SOF packets)\n");
}

/**
 * @brief Print a detailed line for one captured packet.
 * @param[in] pkt Local copy of decoded packet.
 */
void printPacket(const USBPacket& pkt) {
    const char* indent = indentForType(pkt.type);

    /* Timestamp + PID */
    Serial.printf("%s[%10lu] %s", indent, pkt.timestamp_us, pidLabel(pkt.pid));

    /* Type-specific fields */
    switch (pkt.type) {
        case USBPacketType::TOKEN:
            if (pkt.pid == USBPID::SOF) {
                Serial.printf(" frame=%u", pkt.frame_number);
            } else {
                Serial.printf(" addr=%u ep=%u", pkt.addr, pkt.endp);
            }
            break;

        case USBPacketType::DATA:
            Serial.printf(" len=%-2u  ", pkt.data_length);
            for (uint8_t i = 0; i < pkt.data_length && i < 32; i++) {
                Serial.printf("%02X ", pkt.data[i]);
            }
            if (pkt.data_length > 32) Serial.print("...");
            break;

        default:
            break;
    }

    /* CRC status */
    if (pkt.type != USBPacketType::HANDSHAKE) {
        Serial.printf(" [%s]", pkt.crc_valid ? "OK" : "CRC_ERR");
    }

    Serial.println();
}

/**
 * @brief Print decoder statistics.
 */
void printStats() {
    const auto& d = sniffer.decoder();
    Serial.println("\n──── Statistics ────");
    Serial.printf("  Packets decoded : %lu\n", d.packets_decoded);
    Serial.printf("  CRC errors      : %lu\n", d.crc_errors);
    Serial.printf("  SYNC errors     : %lu\n", d.sync_errors);
    Serial.printf("  Stuffing errors : %lu\n", d.stuffing_errors);
    Serial.printf("  Overflow errors : %lu\n", d.overflow_errors);
    Serial.println("────────────────────\n");
}

void loop() {
    /* ── Serial commands ──────────────────────────────────────── */
    if (Serial.available()) {
        char cmd = Serial.read();
        switch (cmd) {
            case 'p':
                g_paused = !g_paused;
                Serial.printf(">> %s\n", g_paused ? "PAUSED" : "RESUMED");
                break;
            case 's':
                printStats();
                break;
            case 'r':
                sniffer.decoder().reset();
                Serial.println(">> Statistics reset");
                break;
            case 'f':
                g_filter_sof = !g_filter_sof;
                Serial.printf(">> SOF filter: %s\n",
                              g_filter_sof ? "ON" : "OFF");
                break;
        }
    }

    /* ── Drain ring buffer ────────────────────────────────────── */
    while (g_ring_tail != g_ring_head) {
        uint32_t tail = g_ring_tail;
        USBPacket pkt = g_ring[tail & PKT_RING_MASK];
        g_ring_tail = tail + 1;
        printPacket(pkt);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE 1 — CAPTURE
 * ═══════════════════════════════════════════════════════════════════════════ */

void setup1() {
    sniffer.begin(PIN_DP, USBSpeed::LOW_SPEED, 1);
    sniffer.onPacket(onPacket);
}

void loop1() { sniffer.task(); }
