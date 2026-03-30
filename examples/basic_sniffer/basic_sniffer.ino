/**
 * @file basic_sniffer.ino
 * @brief Minimal USB sniffer — prints every decoded packet to Serial.
 *
 * This is the simplest possible usage of USBSnifferPIO_RP2040.
 * It captures all USB traffic on the tapped bus and prints
 * a one-line summary for each packet.
 *
 * ## Wiring
 *
 *   USB cable D+  ──[100Ω]──► GP2
 *   USB cable D-  ──[100Ω]──► GP3
 *   USB cable GND ───────────► Pico GND
 *
 * ## Output format
 *
 *   [timestamp_ms] PID_NAME  addr=X endp=X  len=X  CRC=OK|FAIL
 *
 * ## Threading model
 *
 *   Core 1 captures packets and copies them into a ring buffer.
 *   Core 0 drains the ring buffer and prints to Serial.
 *   Serial is NEVER accessed from Core 1.
 *
 * @note Open the Serial Monitor at 115200 baud.
 */

#include <USBSnifferPIO_RP2040.h>

/// @brief GPIO number for D+ (D- is automatically pin_dp + 1).
#define PIN_DP  2

/// @brief Sniffer instance (global, used by both cores).
USBSnifferPIO sniffer;

/* ═══════════════════════════════════════════════════════════════════════════
 *  LOCK-FREE RING BUFFER (Core 1 writes, Core 0 reads)
 * ═══════════════════════════════════════════════════════════════════════════ */

/// @brief Ring buffer depth (must be power of 2).
#define PKT_RING_SIZE  32
#define PKT_RING_MASK  (PKT_RING_SIZE - 1)

/// @brief Ring buffer of captured packets.
static USBPacket  g_ring[PKT_RING_SIZE];

/// @brief Write index (only modified by Core 1).
static volatile uint32_t g_ring_head = 0;

/// @brief Read index (only modified by Core 0).
static volatile uint32_t g_ring_tail = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 *  PID NAME LOOKUP
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Return a human-readable name for a 4-bit PID.
 * @param[in] pid Lower nibble of the PID byte.
 * @return Null-terminated string.
 */
const char* pidName(uint8_t pid) {
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

/* ═══════════════════════════════════════════════════════════════════════════
 *  PACKET CALLBACK (runs on Core 1 — NO Serial access here)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Copy each captured packet into the ring buffer.
 *
 * If the ring is full the oldest unread packet is silently
 * overwritten (lossy under extreme load, but never blocks).
 *
 * @param[in] pkt Decoded packet (valid only during callback).
 */
void onPacket(const USBPacket& pkt) {
    uint32_t head = g_ring_head;
    g_ring[head & PKT_RING_MASK] = pkt;
    g_ring_head = head + 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE 0 — SERIAL SETUP & DISPLAY
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

    Serial.println("USBSnifferPIO_RP2040 — Basic Sniffer");
    Serial.println("Capturing all USB packets...\n");
}

/**
 * @brief Print a one-line summary for a captured packet.
 * @param[in] pkt Decoded packet (local copy, safe to access).
 */
void printPacket(const USBPacket& pkt) {
    Serial.printf("[%10lu] %s", pkt.timestamp_us / 1000, pidName(pkt.pid));

    switch (pkt.type) {
        case USBPacketType::TOKEN:
            if (pkt.pid == USBPID::SOF) {
                Serial.printf("  frame=%u", pkt.frame_number);
            } else {
                Serial.printf("  addr=%u endp=%u", pkt.addr, pkt.endp);
            }
            break;

        case USBPacketType::DATA:
            Serial.printf("  len=%u", pkt.data_length);
            /* Print first 8 payload bytes in hex */
            if (pkt.data_length > 0) {
                Serial.print("  [");
                uint8_t show = (pkt.data_length > 8) ? 8 : pkt.data_length;
                for (uint8_t i = 0; i < show; i++) {
                    Serial.printf("%02X", pkt.data[i]);
                    if (i < show - 1) Serial.print(" ");
                }
                if (pkt.data_length > 8) Serial.print("...");
                Serial.print("]");
            }
            break;

        default:
            break;
    }

    Serial.printf("  CRC=%s\n", pkt.crc_valid ? "OK" : "FAIL");
}

void loop() {
    /* Drain all available packets from the ring buffer */
    while (g_ring_tail != g_ring_head) {
        uint32_t tail = g_ring_tail;
        USBPacket pkt = g_ring[tail & PKT_RING_MASK];
        g_ring_tail = tail + 1;
        printPacket(pkt);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE 1 — CAPTURE ENGINE
 * ═══════════════════════════════════════════════════════════════════════════ */

void setup1() {
    sniffer.begin(PIN_DP, USBSpeed::LOW_SPEED, 1);
    sniffer.onPacket(onPacket);
}

void loop1() { sniffer.task(); }
