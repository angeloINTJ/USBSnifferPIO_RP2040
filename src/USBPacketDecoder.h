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
#ifndef USB_OVERSAMPLING
#define USB_OVERSAMPLING      8
#endif

/** @brief Maximum unstuffed bytes per packet (PID[1] + DATA[64] + CRC16[2] + margin). */
#ifndef USB_MAX_PACKET_BYTES
#define USB_MAX_PACKET_BYTES  72
#endif

/** @brief Maximum raw bits per packet (64B 0xFF worst-case bit-stuffing = 625 bits). */
#ifndef USB_MAX_PACKET_BITS
#define USB_MAX_PACKET_BITS   680
#endif

/** @brief Minimum SYNC alternations required to accept a packet (4..7). */
#ifndef USB_MIN_SYNC_BITS
#define USB_MIN_SYNC_BITS     4
#endif

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

    /**
     * @brief Decode one independent buffer in dual-core striped mode.
     *
     * The owner of a packet is the buffer where its SYNC starts. The call runs
     * the same loop as feedSamples() in three phases:
     *   1. Entry (silent): finds, in the last @p margin_words words of
     *      @p prev_buffer, the first symbol after which every decoder state has
     *      converged (an SE0, or the 10th J in a row) and decodes from there to the
     *      boundary without emitting packets or counting errors. The state at the
     *      boundary is then the one the previous buffer's decoder reaches. Valid
     *      traffic always gives such a symbol; a corrupted stream may not, and
     *      then decoding resumes at the first one in @p buffer.
     *   2. Buffer: decodes @p buffer. The packet left open at the boundary by
     *      the entry phase is followed to its EOP but is not emitted (the
     *      previous buffer owns it). Every packet whose SYNC starts in
     *      @p buffer is emitted and counted. In a corrupted stream, errors of a
     *      packet that crosses the boundary can be counted by both decoders.
     *   3. Tail: if a packet that started in @p buffer is still open at its
     *      end, it is finished in @p next_buffer, one word at a time, up to
     *      @p margin_words words (and never more than @p word_count). Packets
     *      that start in @p next_buffer are not emitted: the next call owns them.
     *
     * With @p prev_buffer == nullptr, decoding starts at the first such symbol
     * of @p buffer, so a packet that starts before it is not decoded.
     *
     * @param[in] buffer       Buffer to decode.
     * @param[in] word_count   Words in @p buffer (and in @p prev_buffer).
     * @param[in] next_buffer  Buffer right after @p buffer, or nullptr.
     * @param[in] margin_words Words read from the end of @p prev_buffer and
     *                         from the start of @p next_buffer.
     * @param[in] prev_buffer  Buffer right before @p buffer, or nullptr.
     */
    void feedSamplesStriped(
        const uint32_t* buffer,
        uint16_t        word_count,
        const uint32_t* next_buffer,
        uint16_t        margin_words = 48,
        const uint32_t* prev_buffer  = nullptr
    );

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

    /* ── Dual-core boundary ownership (see feedSamplesStriped) ─ */
    bool              _emit_enabled;      ///< false while replaying overlap words: new packets are not owned
    bool              _pkt_owned;         ///< packet in progress began while _emit_enabled was true

    /* ── Inline unstuffed byte packer (sentinel-bit shift reg) ─ */
    uint8_t           _pkt_bytes[USB_MAX_PACKET_BYTES];
    uint8_t           _pkt_byte_count;
    uint8_t           _acc;               ///< Sentinel shift register (init 0x80)
    uint8_t           _consecutive_ones;

    USBPacket         _pkt;
    USBPacketCallback _packet_callback;

    void finalizePacket(uint8_t byte_count);
    bool feedFromSafePoint(const uint32_t* words, uint16_t count, uint32_t& j_run);
    static uint16_t crc16_fast(const uint8_t* data, uint8_t length);
    static uint8_t  crc5_fast(uint16_t payload11);
};

#endif /* USB_PACKET_DECODER_H */
