/**
 * @file    USBProtocol.h
 * @brief   USB 1.1 protocol definitions, packet structures, and CRC routines.
 * @version 1.0.0
 * @license MIT
 *
 * @details
 *   Provides the foundational types used throughout the USBSnifferPIO_RP2040 library:
 *   - Line state encoding (J, K, SE0, SE1)
 *   - Speed enumeration (Low-Speed / Full-Speed)
 *   - Packet Identifier (PID) constants and classification
 *   - Decoded packet structure with payload and CRC metadata
 *   - CRC5 and CRC16 computation per USB 1.1 specification
 *
 * @note All CRC algorithms use the bit-at-a-time method with reflected
 *       polynomials, matching the LSB-first bit ordering of USB.
 *
 * @see USB 1.1 Specification, Chapter 8 — Protocol Layer
 *
 * @author Ângelo Moisés Alves
 */

#ifndef USB_PROTOCOL_H
#define USB_PROTOCOL_H

#include <Arduino.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  LINE STATES
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief USB differential line states.
 *
 * Each PIO sample captures two bits: @c [D+, D-].
 * The interpretation depends on speed:
 *   - Low-Speed:  J = @c 0b01, K = @c 0b10  (idle = J)
 *   - Full-Speed: J = @c 0b10, K = @c 0b01  (idle = J)
 *
 * @note This library defaults to Low-Speed polarity.
 */
enum class USBLineState : uint8_t {
    SE0 = 0b00,  ///< Single-Ended Zero (both lines LOW)
    J   = 0b01,  ///< Logical J state (idle / rest)
    K   = 0b10,  ///< Logical K state (start-of-packet / transition)
    SE1 = 0b11   ///< Single-Ended One (both lines HIGH — error)
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  SPEED
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief USB signaling speed.
 */
enum class USBSpeed : uint8_t {
    LOW_SPEED  = 0,  ///< 1.5 Mbps — keyboards, mice
    FULL_SPEED = 1   ///< 12  Mbps — audio, mass-storage, hubs
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  PACKET IDENTIFIERS (PID)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief USB Packet Identifier constants (lower 4 bits only).
 *
 * A PID byte is transmitted as @c {pid[3:0], ~pid[3:0]}.
 * Only the lower nibble carries information; the upper nibble
 * is its bitwise complement and is used for validation.
 */
namespace USBPID {
    /* Token PIDs */
    constexpr uint8_t OUT   = 0b0001;  ///< Host-to-device data direction
    constexpr uint8_t IN    = 0b1001;  ///< Device-to-host data request
    constexpr uint8_t SOF   = 0b0101;  ///< Start-of-Frame marker (1 ms period)
    constexpr uint8_t SETUP = 0b1101;  ///< Control transfer setup stage

    /* Data PIDs */
    constexpr uint8_t DATA0 = 0b0011;  ///< Even data toggle
    constexpr uint8_t DATA1 = 0b1011;  ///< Odd  data toggle

    /* Handshake PIDs */
    constexpr uint8_t ACK   = 0b0010;  ///< Positive acknowledgment
    constexpr uint8_t NAK   = 0b1010;  ///< Negative acknowledgment (busy)
    constexpr uint8_t STALL = 0b1110;  ///< Endpoint halted / request error

    /* Special PIDs */
    constexpr uint8_t PRE   = 0b1100;  ///< Low-Speed preamble via Full-Speed hub
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PACKET TYPE CLASSIFICATION
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief High-level packet type classification.
 */
enum class USBPacketType : uint8_t {
    TOKEN,       ///< OUT, IN, SOF, SETUP
    DATA,        ///< DATA0, DATA1
    HANDSHAKE,   ///< ACK, NAK, STALL
    SPECIAL,     ///< PRE (preamble)
    UNKNOWN      ///< Unrecognized PID
};

/**
 * @brief Classify a 4-bit PID into its packet type.
 *
 * @param[in] pid4 Lower 4 bits of the PID byte.
 * @return Corresponding @ref USBPacketType.
 */
static inline USBPacketType classifyPID(uint8_t pid4) {
    switch (pid4) {
        case USBPID::OUT:
        case USBPID::IN:
        case USBPID::SOF:
        case USBPID::SETUP:  return USBPacketType::TOKEN;

        case USBPID::DATA0:
        case USBPID::DATA1:  return USBPacketType::DATA;

        case USBPID::ACK:
        case USBPID::NAK:
        case USBPID::STALL:  return USBPacketType::HANDSHAKE;

        case USBPID::PRE:    return USBPacketType::SPECIAL;
        default:              return USBPacketType::UNKNOWN;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  DECODED PACKET STRUCTURE
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Fully decoded USB packet.
 *
 * Populated by @ref USBPacketDecoder after successful parsing.
 * The @c data[] array holds up to 64 bytes of payload
 * (maximum for Full-Speed endpoints).
 *
 * @note The structure is valid only within the scope of the
 *       packet callback.  Do not store pointers to it.
 */
struct USBPacket {
    /* ── Metadata ────────────────────────────────────────────── */
    uint32_t      timestamp_us;    ///< Capture instant (micros())
    USBPacketType type;            ///< High-level classification
    bool          crc_valid;       ///< @c true if CRC matches

    /* ── Header fields ───────────────────────────────────────── */
    uint8_t       pid;             ///< PID lower nibble (4 bits)
    uint8_t       addr;            ///< Device address  (7 bits, token only)
    uint8_t       endp;            ///< Endpoint number (4 bits, token only)
    uint16_t      frame_number;    ///< Frame number   (11 bits, SOF only)

    /* ── Payload (DATA0 / DATA1 only) ────────────────────────── */
    uint8_t       data[64];        ///< Raw payload bytes
    uint8_t       data_length;     ///< Actual payload size (excluding CRC)

    /**
     * @brief Check whether this packet looks like an 8-byte HID report.
     *
     * A standard HID keyboard interrupt-IN transfer carries exactly
     * 8 bytes inside a DATA0 or DATA1 packet.
     *
     * @return @c true if the packet matches the HID keyboard heuristic.
     */
    inline bool isHIDKeyboardReport() const {
        return (pid == USBPID::DATA0 || pid == USBPID::DATA1)
            && data_length == 8
            && crc_valid;
    }
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  CRC COMPUTATION
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Compute USB CRC5 over a token field.
 *
 * Polynomial: @c x^5 + x^2 + 1 (0x05, reflected: 0x14).
 * Seed: @c 0x1F (all ones).
 * Final XOR: @c 0x1F (complement).
 *
 * @param[in] data      Bit-packed data (LSB first).
 * @param[in] bit_count Number of data bits (typically 11).
 * @return 5-bit CRC value.
 */
static inline uint8_t crc5_usb(uint16_t data, uint8_t bit_count) {
    uint8_t crc = 0x1F;
    for (uint8_t i = 0; i < bit_count; i++) {
        uint8_t feedback = (crc & 1) ^ ((data >> i) & 1);
        crc >>= 1;
        if (feedback) crc ^= 0x14;
    }
    return crc ^ 0x1F;
}

/**
 * @brief Compute USB CRC16 over a data payload.
 *
 * Polynomial: @c x^16 + x^15 + x^2 + 1 (0x8005, reflected: 0xA001).
 * Seed: @c 0xFFFF.
 * Final XOR: @c 0xFFFF.
 *
 * @param[in] data   Pointer to payload bytes.
 * @param[in] length Number of bytes.
 * @return 16-bit CRC value.
 */
static inline uint16_t crc16_usb(const uint8_t* data, uint16_t length) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < length; i++) {
        uint8_t byte_val = data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            uint8_t feedback = (crc & 1) ^ ((byte_val >> bit) & 1);
            crc >>= 1;
            if (feedback) crc ^= 0xA001;
        }
    }
    return crc ^ 0xFFFF;
}

#endif /* USB_PROTOCOL_H */
