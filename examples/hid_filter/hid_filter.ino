/**
 * @file hid_filter.ino
 * @brief Address- and endpoint-filtered HID capture with JSON output.
 *
 * Captures USB traffic and filters by device address and endpoint
 * number.  Outputs matching HID reports as JSON lines, making it
 * easy to pipe into scripts or log analyzers.
 *
 * ## Wiring
 *
 *   USB cable D+  ──[100Ω]──► GP2
 *   USB cable D-  ──[100Ω]──► GP3
 *   USB cable GND ──────────► Pico GND
 *
 * ## Operation
 *
 * Phase 1 — Discovery:
 *   Listens to all IN tokens for 5 seconds and prints every
 *   unique (address, endpoint) pair seen on the bus.
 *
 * Phase 2 — Filtered capture:
 *   Uses the first discovered HID endpoint (8-byte reports)
 *   and outputs only matching data packets as JSON lines.
 *
 * ## JSON output format
 *
 *   {"t":123456,"addr":1,"ep":1,"len":8,"data":"0200040000000000"}
 *
 * ## Threading model
 *
 *   Core 1 captures packets and copies matching ones to a ring buffer.
 *   Core 0 drains the ring buffer and prints to Serial.
 *   Serial is NEVER accessed from Core 1.
 *
 * @note Baud rate: 115200.
 */

#include <USBSnifferPIO_RP2040.h>

#define PIN_DP  2

USBSnifferPIO sniffer;

/* ═══════════════════════════════════════════════════════════════════════════
 *  DISCOVERY STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

/// @brief Maximum discovered endpoints.
#define MAX_ENDPOINTS  16

/// @brief Discovered (address, endpoint) pairs.
struct EndpointEntry {
    uint8_t addr;
    uint8_t endp;
    uint32_t packet_count;
};

/**
 * @brief Endpoint table — written by Core 1 during discovery,
 *        read by Core 0 after discovery completes.
 *
 * The volatile qualifier on g_endpoint_count ensures Core 0
 * sees the final value after the 5-second window.
 */
EndpointEntry         g_endpoints[MAX_ENDPOINTS];
volatile uint8_t      g_endpoint_count = 0;
volatile bool         g_discovery_done = false;

/* ═══════════════════════════════════════════════════════════════════════════
 *  FILTER STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Target address/endpoint for filtered capture.
 *
 * Written by Core 0, read by Core 1 — must be volatile.
 * -1 means accept all.
 */
volatile int8_t g_target_addr = -1;
volatile int8_t g_target_endp = -1;

/// @brief Last IN token seen (for correlating DATA packets).
volatile uint8_t g_last_in_addr = 0xFF;
volatile uint8_t g_last_in_endp = 0xFF;

/* ═══════════════════════════════════════════════════════════════════════════
 *  RING BUFFER — CAPTURED DATA PACKETS (Core 1 -> Core 0)
 * ═══════════════════════════════════════════════════════════════════════════ */

/// @brief Lightweight struct for filtered data to print as JSON.
struct FilteredData {
    uint32_t timestamp_us;
    uint8_t  addr;
    uint8_t  endp;
    uint8_t  data[64];
    uint8_t  data_length;
};

#define FILT_RING_SIZE  32
#define FILT_RING_MASK  (FILT_RING_SIZE - 1)

static FilteredData       g_filt_ring[FILT_RING_SIZE];
static volatile uint32_t  g_filt_head = 0;
static volatile uint32_t  g_filt_tail = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 *  HELPER — REGISTER A DISCOVERED ENDPOINT
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Add or increment an endpoint entry (called from Core 1).
 * @param[in] addr Device address.
 * @param[in] endp Endpoint number.
 */
void registerEndpoint(uint8_t addr, uint8_t endp) {
    uint8_t count = g_endpoint_count;
    for (uint8_t i = 0; i < count; i++) {
        if (g_endpoints[i].addr == addr && g_endpoints[i].endp == endp) {
            g_endpoints[i].packet_count++;
            return;
        }
    }
    if (count < MAX_ENDPOINTS) {
        g_endpoints[count].addr = addr;
        g_endpoints[count].endp = endp;
        g_endpoints[count].packet_count = 1;
        g_endpoint_count = count + 1;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PACKET CALLBACK (Core 1 — NO Serial access here)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Handle each decoded packet.
 *
 * During discovery: record IN token endpoints.
 * After discovery:  track last IN token, then push matching
 *                   DATA packets into the ring buffer for
 *                   Core 0 to print.
 */
void onPacket(const USBPacket& pkt) {

    /* ── Discovery phase ──────────────────────────────────────── */
    if (!g_discovery_done) {
        if (pkt.pid == USBPID::IN) {
            registerEndpoint(pkt.addr, pkt.endp);
        }
        return;
    }

    /* ── Filtered capture phase ───────────────────────────────── */

    /* Track the most recent IN token */
    if (pkt.pid == USBPID::IN) {
        g_last_in_addr = pkt.addr;
        g_last_in_endp = pkt.endp;
        return;
    }

    /* Only process DATA packets that follow a matching IN */
    if (pkt.type != USBPacketType::DATA) return;
    if (!pkt.crc_valid) return;

    int8_t ta = g_target_addr;
    int8_t te = g_target_endp;
    if (ta >= 0 && g_last_in_addr != (uint8_t)ta) return;
    if (te >= 0 && g_last_in_endp != (uint8_t)te) return;

    /* Push into ring buffer */
    uint32_t head = g_filt_head;
    FilteredData& slot = g_filt_ring[head & FILT_RING_MASK];
    slot.timestamp_us = pkt.timestamp_us;
    slot.addr         = g_last_in_addr;
    slot.endp         = g_last_in_endp;
    slot.data_length  = pkt.data_length;
    memcpy(slot.data, pkt.data, pkt.data_length);
    g_filt_head = head + 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE 0 — DISCOVERY & JSON OUTPUT
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

    Serial.println("USBSnifferPIO_RP2040 — HID Filter");
    Serial.println("Phase 1: Discovering endpoints (5 seconds)...\n");
}

void loop() {
    /* ── Phase 1: wait for discovery to complete ──────────────── */
    if (!g_discovery_done) {
        if (millis() > 5000) {
            g_discovery_done = true;

            uint8_t count = g_endpoint_count;

            Serial.println("Discovered endpoints:");
            Serial.println("  ADDR  ENDP  PACKETS");
            for (uint8_t i = 0; i < count; i++) {
                Serial.printf("  %4u  %4u  %7lu\n",
                              g_endpoints[i].addr,
                              g_endpoints[i].endp,
                              g_endpoints[i].packet_count);
            }

            /* Auto-select the first endpoint with traffic */
            if (count > 0) {
                g_target_addr = g_endpoints[0].addr;
                g_target_endp = g_endpoints[0].endp;
                Serial.printf("\nPhase 2: Filtering addr=%u ep=%u → JSON output\n\n",
                              (uint8_t)g_target_addr, (uint8_t)g_target_endp);
            } else {
                Serial.println("\nNo endpoints found. Capturing all DATA packets.\n");
            }
        }
        return;
    }

    /* ── Phase 2: drain ring buffer and print JSON ────────────── */
    while (g_filt_tail != g_filt_head) {
        uint32_t tail = g_filt_tail;
        const FilteredData& d = g_filt_ring[tail & FILT_RING_MASK];

        Serial.printf("{\"t\":%lu,\"addr\":%u,\"ep\":%u,\"len\":%u,\"data\":\"",
                      d.timestamp_us, d.addr, d.endp, d.data_length);

        for (uint8_t i = 0; i < d.data_length; i++) {
            Serial.printf("%02X", d.data[i]);
        }

        Serial.println("\"}");

        g_filt_tail = tail + 1;
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
