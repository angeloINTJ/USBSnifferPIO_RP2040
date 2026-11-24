/**
 * @file    USBPacketDecoder.h
 * @brief   High-speed software decoder: 1x PIO DPLL symbols → structured USB packets.
 * @version 2.0.0
 * @license MIT
 *
 * @details
 *   Processes clock-recovered 2-bit [D+, D-] symbols produced by the PIO
 *   hardware DPLL (16 USB bits per 32-bit word):
 *   1. Fast-path O(1) idle word skipping (all-J)
 *   2. SYNC pattern detection (KJKJKJKK)
 *   3. Inline NRZI decode + bit-unstuffing + byte packing
 *   4. EOP detection (SE0)
 *   5. Packet parsing with PID validation and table-accelerated CRC16 / CRC5
 *
 * @author Ângelo Moisés Alves
 */

#ifndef USB_PACKET_DECODER_H
#define USB_PACKET_DECODER_H

#include <Arduino.h>
#include "USBProtocol.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  COMPILE-TIME CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief PIO hardware DPLL oversampling factor (8x). */
#define USB_OVERSAMPLING      8

/** @brief Maximum unstuffed bytes per packet (PID[1] + DATA[64] + CRC16[2] + margin). */
#define USB_MAX_PACKET_BYTES  72

/** @brief Maximum raw bits per packet (64B 0xFF worst-case bit-stuffing = 625 bits). */
#define USB_MAX_PACKET_BITS   680

/** @brief Minimum SYNC alternations required to accept a packet. */
#define USB_MIN_SYNC_BITS     4

/* ═══════════════════════════════════════════════════════════════════════════
 *  DECODER STATE MACHINE
 * ═══════════════════════════════════════════════════════════════════════════ */

enum class DecoderState : uint8_t {
    IDLE,         ///< Waiting for J->K transition (start-of-packet)
    SYNC,         ///< Matching SYNC pattern (KJKJKJKK)
    PACKET_BODY,  ///< Collecting NRZI-decoded, unstuffed packet bytes
    EOP           ///< End-of-Packet
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  CALLBACK SIGNATURE
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef void (*USBPacketCallback)(const USBPacket& packet);

/* ═══════════════════════════════════════════════════════════════════════════
 *  CLASS USBPacketDecoder
 * ═══════════════════════════════════════════════════════════════════════════ */

class USBPacketDecoder {
public:
    explicit USBPacketDecoder(USBSpeed speed = USBSpeed::LOW_SPEED);

    void reset();
    void feedSamples(const uint32_t* buffer, uint16_t word_count);
    void onPacket(USBPacketCallback cb);

    /* ── Runtime statistics (volatile for cross-core reads) ───── */
    volatile uint32_t packets_decoded;   ///< Packets successfully decoded
    volatile uint32_t sync_errors;       ///< Malformed SYNC patterns
    volatile uint32_t crc_errors;        ///< CRC verification failures
    volatile uint32_t stuffing_errors;   ///< Bit-stuffing violations
    volatile uint32_t overflow_errors;   ///< Packets exceeding max length
    volatile uint32_t fifo_overruns;     ///< PIO RX FIFO stall/overrun events

private:
    USBSpeed          _speed;
    DecoderState      _state;

    /* ── SYNC & IPG tracking (1x bit-rate symbols, raw 2-bit) ─── */
    uint8_t           _prev_raw;
    uint8_t           _sync_bit_count;
    bool              _idle_ipg_ok;

    /* ── Inline unstuffed byte packer (sentinel-bit shift reg) ─ */
    uint8_t           _pkt_bytes[USB_MAX_PACKET_BYTES];
    uint8_t           _pkt_byte_count;
    uint8_t           _acc;               ///< Sentinel shift register (init 0x80)
    uint8_t           _consecutive_ones;

    USBPacket         _pkt;
    USBPacketCallback _packet_callback;

    void finalizePacket(uint8_t byte_count);
    static uint16_t crc16_fast(const uint8_t* data, uint8_t length);
    static uint8_t  crc5_fast(uint16_t payload11);
};

#endif /* USB_PACKET_DECODER_H */
