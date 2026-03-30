/**
 * @file    USBPacketDecoder.h
 * @brief   Software decoder: raw PIO samples → structured USB packets.
 * @version 1.0.0
 * @license MIT
 *
 * @details
 *   Implements the full receive-side signal processing pipeline:
 *
 *   @verbatim
 *     Raw samples ──► Clock Recovery ──► NRZI Decode ──► Bit Unstuffing
 *         │                                                    │
 *         │               Packet Callback ◄── CRC Check ◄── Framing
 *   @endverbatim
 *
 *   ## Pipeline stages
 *
 *   | # | Stage            | Description                                  |
 *   |---|------------------|----------------------------------------------|
 *   | 1 | Clock recovery   | Edge detection + center-of-bit sampling      |
 *   | 2 | SYNC detection   | KJKJKJKK pattern match (start-of-packet)     |
 *   | 3 | NRZI decode      | Transition → 0, no transition → 1            |
 *   | 4 | Bit unstuffing   | Remove zero after 6 consecutive ones         |
 *   | 5 | Packet framing   | EOP detection (SE0 ≥ 2 bit-times)           |
 *   | 6 | Packet parsing   | PID + Token/Data/Handshake + CRC validation  |
 *
 * @par Thread safety
 *   The decoder is @b not thread-safe.  All calls to @ref feedSamples()
 *   must originate from the same execution context.
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

/** @brief Samples per USB bit (oversampling factor). */
#define USB_OVERSAMPLING      8

/**
 * @brief Minimum consecutive SE0 samples to recognize an EOP.
 *
 * USB spec requires SE0 for at least 2 bit-times.
 * With 8x oversampling that is 16 samples; we use a slightly
 * relaxed threshold to tolerate signal ringing.
 */
#define USB_EOP_MIN_SAMPLES   (USB_OVERSAMPLING * 2 - 2)

/** @brief Maximum raw bits per packet (before unstuffing). */
#define USB_MAX_PACKET_BITS   600

/** @brief Alias for buffer sizing. */
#define USB_RAW_BITS_SIZE     USB_MAX_PACKET_BITS

/** @brief Alias for clean (unstuffed) buffer sizing. */
#define USB_CLEAN_BITS_SIZE   USB_MAX_PACKET_BITS

/**
 * @brief Minimum SYNC alternations required to accept a packet.
 *
 * A perfect SYNC is 7 alternations (KJKJKJK) followed by KK.
 * Accepting as few as 4 handles slightly degraded signals.
 */
#define USB_MIN_SYNC_BITS     4

/* ═══════════════════════════════════════════════════════════════════════════
 *  DECODER STATE MACHINE
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Internal decoder states.
 */
enum class DecoderState : uint8_t {
    IDLE,         ///< Waiting for J->K transition (start-of-packet)
    SYNC,         ///< Matching SYNC pattern (KJKJKJKK)
    PACKET_BODY,  ///< Collecting NRZI-encoded packet bits
    EOP           ///< Detecting End-of-Packet (SE0 -> J)
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  CALLBACK SIGNATURE
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Packet callback function signature.
 *
 * @param[in] packet Fully decoded packet.  Valid only for the
 *                   duration of the callback — do not store
 *                   the reference.
 */
typedef void (*USBPacketCallback)(const USBPacket& packet);

/* ═══════════════════════════════════════════════════════════════════════════
 *  CLASS USBPacketDecoder
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @class USBPacketDecoder
 * @brief Decodes oversampled PIO data into structured USB packets.
 *
 * ### Typical usage
 * @code
 *   USBPacketDecoder decoder(USBSpeed::LOW_SPEED);
 *   decoder.onPacket([](const USBPacket& pkt) {
 *       Serial.printf("PID=0x%X  len=%d\n", pkt.pid, pkt.data_length);
 *   });
 *
 *   // In the DMA completion handler:
 *   decoder.feedSamples(dma_buffer, word_count);
 * @endcode
 */
class USBPacketDecoder {
public:

    /* ── Construction / reset ─────────────────────────────────── */

    /**
     * @brief Construct a decoder for the given USB speed.
     * @param[in] speed Target bus speed (default: Low-Speed).
     */
    explicit USBPacketDecoder(USBSpeed speed = USBSpeed::LOW_SPEED);

    /**
     * @brief Reset all internal state and statistics to defaults.
     */
    void reset();

    /* ── Sample input ─────────────────────────────────────────── */

    /**
     * @brief Feed a block of raw PIO samples into the decoder.
     *
     * Each 32-bit word contains 16 two-bit samples [D+,D-],
     * packed LSB-first by the PIO shift register.
     *
     * Decoded packets are delivered via the registered callback
     * @b during this call.
     *
     * @param[in] buffer     Pointer to the DMA buffer.
     * @param[in] word_count Number of 32-bit words.
     */
    void feedSamples(const uint32_t* buffer, uint16_t word_count);

    /* ── Callback registration ────────────────────────────────── */

    /**
     * @brief Register (or clear) the packet-ready callback.
     *
     * Pass @c nullptr to unregister.
     *
     * @param[in] cb Callback function pointer.
     */
    void onPacket(USBPacketCallback cb);

    /* ── Runtime statistics ────────────────────────────────────── */

    /**
     * @name Runtime statistics
     * @brief Declared volatile because they are written by Core 1
     *        (inside feedSamples / processSample / finalizePacket)
     *        and read by Core 0 (dashboard, printStats, etc.).
     *        Without volatile the compiler may cache stale values
     *        in registers on the reading core with -O2.
     * @{
     */
    volatile uint32_t packets_decoded;   ///< Packets successfully decoded
    volatile uint32_t sync_errors;       ///< Malformed SYNC patterns
    volatile uint32_t crc_errors;        ///< CRC verification failures
    volatile uint32_t stuffing_errors;   ///< Bit-stuffing violations
    volatile uint32_t overflow_errors;   ///< Packets exceeding max length
    /** @} */

private:

    /* ── Configuration ────────────────────────────────────────── */
    USBSpeed     _speed;

    /* ── Decoder FSM ──────────────────────────────────────────── */
    DecoderState _state;

    /* ── Clock recovery ───────────────────────────────────────── */
    USBLineState _prev_line_state;   ///< Previous raw sample
    uint8_t      _phase_counter;     ///< Position within bit period (0 .. OVERSAMPLING-1)
    uint8_t      _sync_bit_count;    ///< Number of SYNC bits matched so far

    /**
     * @brief Consecutive J-state samples seen while in IDLE.
     *
     * Counts toward the inter-packet gap (IPG) requirement.
     * Once it reaches USB_OVERSAMPLING, the latching flag
     * @ref _idle_ipg_ok is set.
     *
     * Saturated at USB_OVERSAMPLING to avoid uint8_t overflow during long
     * idle periods.
     */
    uint8_t      _idle_j_count;

    /**
     * @brief Latching flag: true once the IPG requirement is met.
     *
     * Set to true when _idle_j_count reaches USB_OVERSAMPLING
     * (= 1 full bit period of consecutive J).
     *
     * Cleared only by reset(), EOP finalization, and SYNC abort —
     * NOT by individual K samples.  This is critical because
     * with 8× oversampling the first K samples of a SYNC edge
     * arrive several samples before the center-of-bit check can
     * test the condition.  If the flag were cleared on each K
     * sample (as _idle_j_count is), the SYNC detection would
     * never trigger.
     */
    bool         _idle_ipg_ok;

    /* ── Raw bit buffer (post-NRZI, pre-unstuffing) ───────────── */
    uint8_t      _raw_bits[USB_RAW_BITS_SIZE / 8 + 1];
    uint16_t     _raw_bit_count;
    USBLineState _prev_bit_state;    ///< Previous bit-level line state (for NRZI)

    /* ── EOP tracking ─────────────────────────────────────────── */
    uint8_t      _se0_count;         ///< Consecutive SE0 samples

    /* ── Bit-stuffing context ─────────────────────────────────── */
    uint8_t      _consecutive_ones;

    /* ── Callback ─────────────────────────────────────────────── */
    USBPacketCallback _packet_callback;

    /* ── Internal methods ─────────────────────────────────────── */

    /**
     * @brief Process a single 2-bit line sample.
     * @param[in] sample Current line state.
     */
    void processSample(USBLineState sample);

    /**
     * @brief Process a bit during the SYNC detection phase.
     * @param[in] sample Line state at the center of the bit period.
     */
    void onSyncBit(USBLineState sample);

    /**
     * @brief Process a recovered bit in the packet body.
     *
     * Performs NRZI decode and pushes the result into the raw bit buffer.
     *
     * @param[in] state Line state at the center of the bit period.
     */
    void onBitRecovered(USBLineState state);

    /**
     * @brief Finalize and parse the packet after EOP detection.
     */
    void finalizePacket();

    /**
     * @brief Remove bit-stuffing from the raw bit buffer.
     *
     * After 6 consecutive 1 bits the transmitter inserts a 0 (stuff bit).
     * The receiver must discard it.  A non-zero bit in that position is a
     * stuffing violation.
     *
     * @param[out] out_bits  Destination buffer for clean bits.
     * @param[out] out_count Number of clean bits written.
     * @return @c true on success, @c false on stuffing error.
     */
    bool removeBitStuffing(uint8_t* out_bits, uint16_t* out_count);

    /**
     * @brief Parse a clean (unstuffed) bit stream into a USBPacket.
     *
     * @param[in]  bits      Clean bit buffer.
     * @param[in]  bit_count Total clean bits.
     * @param[out] pkt       Output packet structure.
     * @return @c true if parsing succeeded.
     */
    bool parsePacket(const uint8_t* bits, uint16_t bit_count, USBPacket& pkt);

    /**
     * @brief Parse a Token packet (OUT / IN / SETUP / SOF).
     *
     * Layout: PID(8) + ADDR(7) + ENDP(4) + CRC5(5) = 24 bits.
     * SOF:    PID(8) + FrameNumber(11) + CRC5(5) = 24 bits.
     *
     * @param[in]  bits      Clean bit buffer.
     * @param[in]  bit_count Total clean bits.
     * @param[out] pkt       Output packet structure.
     * @return @c true if parsing succeeded.
     */
    bool parseTokenPacket(const uint8_t* bits, uint16_t bit_count, USBPacket& pkt);

    /**
     * @brief Parse a Data packet (DATA0 / DATA1).
     *
     * Layout: PID(8) + Payload(0..512 bytes × 8) + CRC16(16).
     *
     * @param[in]  bits      Clean bit buffer.
     * @param[in]  bit_count Total clean bits.
     * @param[out] pkt       Output packet structure.
     * @return @c true if parsing succeeded.
     */
    bool parseDataPacket(const uint8_t* bits, uint16_t bit_count, USBPacket& pkt);

    /**
     * @brief Extract N bits from a packed bit buffer as an integer.
     *
     * Bits are stored and returned LSB-first (matching USB order).
     *
     * @param[in] bits   Packed bit buffer.
     * @param[in] offset Starting bit index.
     * @param[in] count  Number of bits to extract (max 16).
     * @return Extracted value.
     */
    static uint16_t extractBits(const uint8_t* bits, uint16_t offset, uint8_t count);

    /**
     * @brief Append one bit to the raw bit buffer.
     * @param[in] bit Bit value (0 or 1).
     */
    inline void pushRawBit(uint8_t bit) {
        if (_raw_bit_count < USB_RAW_BITS_SIZE) {
            uint16_t idx = _raw_bit_count;
            if (bit) _raw_bits[idx / 8] |=  (1 << (idx % 8));
            else     _raw_bits[idx / 8] &= ~(1 << (idx % 8));
            _raw_bit_count++;
        } else {
            overflow_errors++;
        }
    }

    /**
     * @brief Read one bit from a packed bit buffer.
     *
     * @param[in] buf   Packed bit buffer.
     * @param[in] index Bit index.
     * @return Bit value (0 or 1).
     */
    static inline uint8_t readBit(const uint8_t* buf, uint16_t index) {
        return (buf[index / 8] >> (index % 8)) & 1;
    }
};

#endif /* USB_PACKET_DECODER_H */
