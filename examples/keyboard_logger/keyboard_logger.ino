/**
 * @file keyboard_logger.ino
 * @brief Passive HID keyboard capture with human-readable output.
 *
 * Filters USB traffic to show only keyboard HID reports (8-byte
 * DATA0/DATA1 packets).  Displays modifier keys and scancodes
 * in a readable format.
 *
 * ## Wiring
 *
 *   USB cable D+  ──[100Ω]──► GP2
 *   USB cable D-  ──[100Ω]──► GP3
 *   USB cable GND ──────────► Pico GND
 *
 * ## HID Report Format (8 bytes)
 *
 *   Byte 0: Modifier bitmask (Ctrl, Shift, Alt, Gui × L/R)
 *   Byte 1: Reserved (always 0x00)
 *   Byte 2–7: Up to 6 simultaneous scancodes (0x00 = empty)
 *
 * ## Threading model
 *
 *   Core 1 captures packets and writes to a double-buffer bank.
 *   Core 0 reads from the opposite bank and prints to Serial.
 *   A single volatile uint8_t bank index acts as an implicit
 *   memory fence — the reader always gets a consistent 8-byte
 *   snapshot with no tearing.
 *
 * @note Uses dual-core: Core 1 captures, Core 0 displays.
 */

#include <USBSnifferPIO_RP2040.h>

/// @brief GPIO for D+ (D- = GP3).
#define PIN_DP   2

/// @brief Onboard LED for visual feedback.
#define PIN_LED  25

/// @brief Sniffer instance.
USBSnifferPIO sniffer;

/* ═══════════════════════════════════════════════════════════════════════════
 *  DOUBLE-BUFFER FOR HID REPORTS (tear-free cross-core transfer)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Two 8-byte banks.  Core 1 writes to bank[write_bank ^ 1],
 *        then atomically flips write_bank.  Core 0 reads from
 *        bank[write_bank] — always the most recently completed bank.
 */
static volatile uint8_t  g_report_bank[2][8];

/**
 * @brief Index of the bank most recently written by Core 1.
 *
 * Updated only AFTER all 8 bytes have been written to the opposite
 * bank, so Core 0 always reads a complete, consistent report.
 */
static volatile uint8_t  g_write_bank    = 0;
static volatile bool     g_new_report    = false;
static volatile uint32_t g_report_count  = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 *  MODIFIER NAME TABLE
 * ═══════════════════════════════════════════════════════════════════════════ */

/// @brief Short names for each modifier bit (byte 0 of HID report).
static const char* MOD_NAMES[8] = {
    "LCtrl", "LShft", "LAlt", "LGui",
    "RCtrl", "RShft", "RAlt", "RGui"
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  PACKET CALLBACK (Core 1 — NO Serial access here)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Accept only HID keyboard reports and copy to double-buffer.
 *
 * Runs on Core 1 inside @c sniffer.task().  Must be fast — only
 * a memcpy, index flip, and flag set.
 *
 * @param[in] pkt Decoded USB packet.
 */
void onPacket(const USBPacket& pkt) {
    if (!pkt.isHIDKeyboardReport()) return;

    /* Write to the bank that Core 0 is NOT currently reading */
    uint8_t target_bank = g_write_bank ^ 1;
    for (uint8_t i = 0; i < 8; i++) {
        g_report_bank[target_bank][i] = pkt.data[i];
    }
    /* Atomic uint8_t write flips the published bank */
    g_write_bank   = target_bank;
    g_new_report   = true;
    g_report_count++;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE 0 — DISPLAY
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
    pinMode(PIN_LED, OUTPUT);
    Serial.begin(115200);
    while (!Serial && millis() < 3000) delay(50);

    Serial.println("USBSnifferPIO_RP2040 — Keyboard Logger");
    Serial.println("Waiting for HID reports...\n");
}

void loop() {
    if (g_new_report) {
        g_new_report = false;

        /* Read from the most recently completed bank (tear-free) */
        uint8_t bank = g_write_bank;
        uint8_t rpt[8];
        for (uint8_t i = 0; i < 8; i++) rpt[i] = g_report_bank[bank][i];

        /* Print report number */
        Serial.printf("#%06lu  ", g_report_count);

        /* Print active modifiers */
        bool any_mod = false;
        for (uint8_t b = 0; b < 8; b++) {
            if (rpt[0] & (1 << b)) {
                Serial.printf("%s ", MOD_NAMES[b]);
                any_mod = true;
            }
        }

        /* Print active scancodes */
        bool any_key = false;
        for (uint8_t i = 2; i < 8; i++) {
            if (rpt[i] > 0x03) {
                Serial.printf("[0x%02X] ", rpt[i]);
                any_key = true;
            }
        }

        /* Empty report = all keys released */
        if (!any_mod && !any_key) {
            Serial.print("(all released)");
        }

        Serial.println();

        /* Blink LED on each report */
        digitalWrite(PIN_LED, HIGH);
        delay(5);
        digitalWrite(PIN_LED, LOW);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE 1 — CAPTURE
 * ═══════════════════════════════════════════════════════════════════════════ */

void setup1() {
    sniffer.begin(PIN_DP, USBSpeed::LOW_SPEED, 1);
    sniffer.onPacket(onPacket);
}

void loop1() {
    sniffer.task();
}
