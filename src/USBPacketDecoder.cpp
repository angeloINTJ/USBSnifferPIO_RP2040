/**
 * @file    USBPacketDecoder.cpp
 * @brief   Implementation of the USB packet decoder.
 * @version 1.0.0
 * @license MIT
 *
 * @details
 *   Processes oversampled PIO data through six stages:
 *   1. Edge-based clock recovery with phase alignment
 *   2. SYNC pattern detection (KJKJKJKK)
 *   3. NRZI decoding (transition=0, same=1)
 *   4. Bit-stuffing removal
 *   5. EOP detection (SE0 >= 2 bit-times)
 *   6. Packet parsing with PID validation and CRC checks
 *
 * @see USBPacketDecoder.h for the public API.
 *
 * @author Ângelo Moisés Alves
 */

#include "USBPacketDecoder.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  CONSTRUCTION / RESET
 * ═══════════════════════════════════════════════════════════════════════════ */

USBPacketDecoder::USBPacketDecoder(USBSpeed speed)
    : _speed(speed)
    , _packet_callback(nullptr)
{
    reset();
}

void USBPacketDecoder::reset() {
    _state            = DecoderState::IDLE;
    _prev_line_state  = USBLineState::J;
    _phase_counter    = 0;
    _sync_bit_count   = 0;
    _raw_bit_count    = 0;
    _prev_bit_state   = USBLineState::J;
    _se0_count        = 0;
    _consecutive_ones = 0;

    /*
     * Require a full IPG (USB_OVERSAMPLING J samples) before the first
     * SYNC.  This prevents a K seen mid-packet — right after the
     * initial reset() — from triggering a false SYNC and generating
     * hundreds of spurious overflow_errors.
     */
    _idle_j_count     = 0;
    _idle_ipg_ok      = false;

    packets_decoded  = 0;
    sync_errors      = 0;
    crc_errors       = 0;
    stuffing_errors  = 0;
    overflow_errors  = 0;

    memset(_raw_bits, 0, sizeof(_raw_bits));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CALLBACK
 * ═══════════════════════════════════════════════════════════════════════════ */

void USBPacketDecoder::onPacket(USBPacketCallback cb) {
    _packet_callback = cb;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SAMPLE INPUT (DMA BUFFER -> DECODER)
 * ═══════════════════════════════════════════════════════════════════════════ */

void USBPacketDecoder::feedSamples(const uint32_t* buffer, uint16_t word_count) {

    /*
     * Fast-path constant: a 32-bit word where all 16 two-bit samples
     * are J (idle state).  This is the overwhelming majority of the
     * buffer during normal USB operation — typically >90%.
     *
     *   Low-Speed:  J = D- high = 0b10  →  16 × 0b10 = 0xAAAAAAAA
     *   Full-Speed: J = D+ high = 0b01  →  16 × 0b01 = 0x55555555
     *
     * When the decoder is in IDLE state and the entire word is all-J,
     * we can skip the 16 individual processSample() calls and update
     * the state in O(1):
     *
     *   - _idle_j_count saturates at USB_OVERSAMPLING (already guaranteed
     *     by 16 consecutive J samples)
     *   - _idle_ipg_ok latches true (IPG requirement satisfied)
     *   - _phase_counter unchanged: 16 samples = 2 full 8-sample periods,
     *     so phase wraps back to the same value
     *   - _prev_line_state = J, _prev_bit_state = J (no transitions)
     *   - _se0_count = 0 (no SE0 in an all-J word)
     *
     * This optimization reduces feedSamples() from ~5.5 ms per 2048-word
     * buffer to ~0.8 ms, keeping processing well within the 2.73 ms DMA
     * fill time and eliminating sample loss between buffers.
     */
    const uint32_t ALL_J_WORD = (_speed == USBSpeed::LOW_SPEED)
                              ? 0xAAAAAAAAu    /* 16 × 0b10 */
                              : 0x55555555u;   /* 16 × 0b01 */

    for (uint16_t w = 0; w < word_count; w++) {
        uint32_t word = buffer[w];

        /*
         * FAST PATH: skip all-J words in IDLE state.
         *
         * In a typical LS keyboard capture, ~90% of words are all-J
         * (bus idle between 27 Hz polling cycles).  Processing each
         * through 16 × processSample() at ~25 cycles each on a
         * Cortex-M0+ at 120 MHz takes ~5.5 ms per 2048-word buffer —
         * but the DMA fills a buffer in only 2.73 ms.  This causes
         * ~50% sample loss and missed DATA packets.
         *
         * The fast path handles an all-J word in ~5 cycles instead of
         * ~400, eliminating the throughput bottleneck.
         */
        if (word == ALL_J_WORD && _state == DecoderState::IDLE) {
            _idle_j_count    = USB_OVERSAMPLING;
            _idle_ipg_ok     = true;
            _prev_line_state = USBLineState::J;
            _prev_bit_state  = USBLineState::J;
            _se0_count       = 0;
            /* _phase_counter unchanged: 16 mod 8 = 0 */
            continue;
        }

        /* SLOW PATH: sample-by-sample decoding.
         * Each 32-bit word has 16 two-bit samples [D+, D-].
         * The PIO shifts right: LSB is the oldest sample. */
        for (uint8_t s = 0; s < 16; s++) {
            uint8_t raw_sample = word & 0x03;
            USBLineState logical_sample;

            /* Physical → Logical translation */
            if (raw_sample == 0b00) {
                logical_sample = USBLineState::SE0;
            } else if (raw_sample == 0b11) {
                logical_sample = USBLineState::SE1;
            } else {
                if (_speed == USBSpeed::LOW_SPEED) {
                    logical_sample = (raw_sample == 0b10)
                                   ? USBLineState::J
                                   : USBLineState::K;
                } else {
                    logical_sample = (raw_sample == 0b01)
                                   ? USBLineState::J
                                   : USBLineState::K;
                }
            }

            word >>= 2;
            processSample(logical_sample);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STAGE 1 — CLOCK RECOVERY
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Process a single oversampled line observation.
 *
 * Implements edge-triggered clock recovery:
 *   - On any J<->K transition the phase counter resets to zero,
 *     aligning the sampling window to the new bit boundary.
 *   - The bit value is captured at phase = OVERSAMPLING/2
 *     (center of the bit period) for maximum noise margin.
 *   - SE0 is tracked separately for EOP detection.
 */
void USBPacketDecoder::processSample(USBLineState sample) {

    /* ── SE0 tracking ─────────────────────────────────────────── */
    if (sample == USBLineState::SE0) {
        _se0_count++;

        if (_state == DecoderState::PACKET_BODY &&
            _se0_count >= USB_EOP_MIN_SAMPLES)
        {
            /* Legitimate EOP: finalize packet and return to IDLE. */
            finalizePacket();
            _state        = DecoderState::IDLE;
            _idle_j_count = 0;   /* bus not yet in J — wait for IPG */
            _idle_ipg_ok  = false;
        }
        else if (_state == DecoderState::SYNC) {
            /*
             * SE0 during SYNC: EOP arrived before completing the
             * pattern — hub Keep-Alive or signal glitch.  Return to
             * IDLE without incrementing sync_errors (this is not a
             * protocol failure).
             *
             * Without this block the decoder gets stuck in SYNC with
             * a stale _sync_bit_count: the next bit after the SE0
             * would almost certainly produce a spurious sync_errors++.
             */
            _state          = DecoderState::IDLE;
            _prev_bit_state = USBLineState::J;
            _idle_j_count   = 0;
            _idle_ipg_ok    = false;
        }

        _prev_line_state = sample;
        return;
    }
    _se0_count = 0;

    /*
     * ── IPG guard: count consecutive J samples in IDLE ────────────────────
     *
     * USB LS requires a minimum inter-packet gap (IPG) of 2 bit-times.
     * We require USB_OVERSAMPLING (8) consecutive J samples (= 1 bit
     * period) before accepting a SYNC.
     *
     * CRITICAL: _idle_ipg_ok is a LATCHING flag.  Once _idle_j_count
     * reaches the threshold, _idle_ipg_ok is set to true and remains
     * true even when subsequent K samples reset _idle_j_count to 0.
     *
     * This is necessary because with 8× oversampling the first K
     * samples of a SYNC edge arrive 4 samples BEFORE the center-of-bit
     * check can test the condition.  If _idle_ipg_ok were cleared on
     * each K sample (as _idle_j_count is), the SYNC detection would
     * NEVER trigger — the condition would always read false at center.
     *
     * _idle_ipg_ok is cleared only by:
     *   - reset()
     *   - EOP finalization (new IPG required for next packet)
     *   - SYNC abort paths (failed SYNC → need fresh IPG)
     *   - Successful SYNC entry (consumed by detection)
     */
    if (_state == DecoderState::IDLE) {
        if (sample == USBLineState::J) {
            if (_idle_j_count < USB_OVERSAMPLING) _idle_j_count++;
            if (_idle_j_count >= USB_OVERSAMPLING) _idle_ipg_ok = true;
        } else {
            _idle_j_count = 0;
            /* Do NOT clear _idle_ipg_ok here — K oversamples arrive
             * before center-of-bit can check the flag. */
        }
    }

    /* ── Edge detection → phase reset ────────────────────────── */
    if (sample != _prev_line_state) {
        _phase_counter = 0;
    }

    /* ── Center-of-bit sampling ───────────────────────────────── */
    if (_phase_counter == (USB_OVERSAMPLING / 2)) {
        switch (_state) {
            case DecoderState::IDLE:
                /*
                 * Detect the J->K transition that signals SYNC start.
                 *
                 * Conditions:
                 *   1. sample == K && _prev_bit_state == J  (real J->K transition)
                 *   2. _idle_ipg_ok == true                 (IPG satisfied)
                 *
                 * _idle_ipg_ok is a LATCHING flag set when _idle_j_count
                 * reaches USB_OVERSAMPLING.  Unlike _idle_j_count itself,
                 * it survives the K oversamples that arrive before this
                 * center-of-bit check.  See IPG guard section above.
                 */
                if (sample == USBLineState::K          &&
                    _prev_bit_state == USBLineState::J &&
                    _idle_ipg_ok)
                {
                    _state          = DecoderState::SYNC;
                    _sync_bit_count = 1;
                    _idle_ipg_ok    = false;   /* consumed */
                }
                _prev_bit_state = sample;   /* always update in IDLE */
                break;

            case DecoderState::SYNC:
                onSyncBit(sample);
                break;

            case DecoderState::PACKET_BODY:
                onBitRecovered(sample);
                break;

            default:
                break;
        }
    }

    /* Advance phase (wraps at oversampling boundary) */
    _phase_counter++;
    if (_phase_counter >= USB_OVERSAMPLING) {
        _phase_counter = 0;
    }
    _prev_line_state = sample;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STAGE 2 — SYNC DETECTION
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Match the SYNC pattern one bit at a time.
 *
 * Expected sequence (Low-Speed):
 *   Bit 1..7: K,J,K,J,K,J,K  (alternating — NRZI zeros)
 *   Bit 8:    K               (same as bit 7 — NRZI one → end marker)
 *
 * On success, transitions to DecoderState::PACKET_BODY.
 * On failure (after at least USB_MIN_SYNC_BITS alternations),
 * attempts a short-SYNC recovery before falling back to IDLE.
 */
void USBPacketDecoder::onSyncBit(USBLineState sample) {
    bool same = (sample == _prev_bit_state);
    _prev_bit_state = sample;
    _sync_bit_count++;

    if (_sync_bit_count <= 7) {
        /* Bits 1..7 must alternate (transition = NRZI 0) */
        if (same) {
            /* Non-alternation: accept as short SYNC if enough bits matched */
            if (_sync_bit_count > USB_MIN_SYNC_BITS) {
                _state            = DecoderState::PACKET_BODY;
                _raw_bit_count    = 0;
                _consecutive_ones = 0;
                memset(_raw_bits, 0, sizeof(_raw_bits));
                return;
            }
            /* Insufficient alternations — noise or glitch */
            sync_errors++;
            _state          = DecoderState::IDLE;
            _prev_bit_state = USBLineState::J;
            _idle_j_count   = 0;   /* require new IPG before next SYNC */
            _idle_ipg_ok    = false;
        }
    } else {
        /* Bit 8: must repeat the previous state (no transition = NRZI 1) */
        if (same) {
            /* Full SYNC complete — begin packet body capture */
            _state            = DecoderState::PACKET_BODY;
            _raw_bit_count    = 0;
            _consecutive_ones = 0;
            memset(_raw_bits, 0, sizeof(_raw_bits));
        } else {
            sync_errors++;
            _state          = DecoderState::IDLE;
            _prev_bit_state = USBLineState::J;
            _idle_j_count   = 0;   /* require new IPG before next SYNC */
            _idle_ipg_ok    = false;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STAGE 3 — NRZI DECODE
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Decode one NRZI bit and store it.
 *
 * USB NRZI encoding:
 *   - No transition (same state)   → data bit 1
 *   - Transition    (state change)  → data bit 0
 *
 * The decoded bit (which still includes stuff-bits) is appended
 * to _raw_bits.
 */
void USBPacketDecoder::onBitRecovered(USBLineState state) {
    uint8_t bit = (state == _prev_bit_state) ? 1 : 0;
    _prev_bit_state = state;
    pushRawBit(bit);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STAGE 4 — BIT UNSTUFFING
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Remove stuff-bits from the raw buffer.
 *
 * Rule: after 6 consecutive 1 bits the transmitter inserts a 0 (stuff bit).
 * The receiver must discard it.  If the bit following 6 ones is not 0,
 * the frame is corrupt.
 */
bool USBPacketDecoder::removeBitStuffing(uint8_t* out_bits, uint16_t* out_count) {
    uint16_t out_idx    = 0;
    uint8_t  ones_count = 0;

    for (uint16_t i = 0; i < _raw_bit_count; i++) {
        uint8_t bit = readBit(_raw_bits, i);

        if (ones_count == 6) {
            /* Expect a stuff-zero here */
            if (bit != 0) return false;
            ones_count = 0;
            continue;   /* discard the stuff bit */
        }

        /* Store the data bit */
        if (bit) {
            out_bits[out_idx / 8] |= (1 << (out_idx % 8));
            ones_count++;
        } else {
            ones_count = 0;
        }
        out_idx++;
    }

    *out_count = out_idx;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STAGE 5 — PACKET FINALIZATION
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Called when EOP is confirmed.
 *
 * Runs the unstuffing and parsing stages.  On success, delivers
 * the decoded packet through the registered callback.
 */
void USBPacketDecoder::finalizePacket() {
    /* Need at least 8 bits for a valid PID */
    if (_raw_bit_count < 8) return;

    /* Remove bit stuffing */
    uint8_t  clean_bits[USB_CLEAN_BITS_SIZE / 8 + 1];
    uint16_t clean_count = 0;
    memset(clean_bits, 0, sizeof(clean_bits));

    if (!removeBitStuffing(clean_bits, &clean_count)) {
        stuffing_errors++;
        return;
    }

    /* Parse the clean bit stream */
    USBPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.timestamp_us = micros();

    if (parsePacket(clean_bits, clean_count, pkt)) {
        packets_decoded++;
        if (_packet_callback) {
            _packet_callback(pkt);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STAGE 6 — PACKET PARSING
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Top-level parser: validate PID and dispatch.
 */
bool USBPacketDecoder::parsePacket(
    const uint8_t* bits,
    uint16_t       bit_count,
    USBPacket&     pkt
) {
    if (bit_count < 8) return false;

    /* ── PID validation ───────────────────────────────────────── */
    uint8_t pid_byte = (uint8_t)extractBits(bits, 0, 8);
    uint8_t pid_low  = pid_byte & 0x0F;
    uint8_t pid_high = (pid_byte >> 4) & 0x0F;

    if ((pid_low ^ pid_high) != 0x0F) return false;

    pkt.pid  = pid_low;
    pkt.type = classifyPID(pid_low);

    /* ── Dispatch by packet type ──────────────────────────────── */
    switch (pkt.type) {
        case USBPacketType::TOKEN:
            return parseTokenPacket(bits, bit_count, pkt);

        case USBPacketType::DATA:
            return parseDataPacket(bits, bit_count, pkt);

        case USBPacketType::HANDSHAKE:
            pkt.crc_valid   = true;
            pkt.data_length = 0;
            return true;

        case USBPacketType::SPECIAL:
            pkt.crc_valid   = true;
            pkt.data_length = 0;
            return true;

        default:
            return false;
    }
}

/* ── Token parser ─────────────────────────────────────────────── */

bool USBPacketDecoder::parseTokenPacket(
    const uint8_t* bits,
    uint16_t       bit_count,
    USBPacket&     pkt
) {
    /* Token: PID(8) + payload(11) + CRC5(5) = 24 bits */
    if (bit_count < 24) return false;

    if (pkt.pid == USBPID::SOF) {
        pkt.frame_number = extractBits(bits, 8, 11);
        uint8_t rx_crc   = (uint8_t)extractBits(bits, 19, 5);
        uint8_t calc_crc = crc5_usb(pkt.frame_number, 11);
        pkt.crc_valid    = (rx_crc == calc_crc);
    } else {
        pkt.addr         = (uint8_t)extractBits(bits, 8, 7);
        pkt.endp         = (uint8_t)extractBits(bits, 15, 4);
        uint8_t rx_crc   = (uint8_t)extractBits(bits, 19, 5);
        uint16_t crc_in  = pkt.addr | ((uint16_t)pkt.endp << 7);
        uint8_t calc_crc = crc5_usb(crc_in, 11);
        pkt.crc_valid    = (rx_crc == calc_crc);
    }

    if (!pkt.crc_valid) crc_errors++;
    pkt.data_length = 0;
    return true;
}

/* ── Data parser ──────────────────────────────────────────────── */

bool USBPacketDecoder::parseDataPacket(
    const uint8_t* bits,
    uint16_t       bit_count,
    USBPacket&     pkt
) {
    /* Data: PID(8) + payload(N×8) + CRC16(16) — min 24 bits */
    if (bit_count < 24) return false;

    int payload_bits = (int)bit_count - 8 - 16;
    if (payload_bits < 0 || (payload_bits % 8) != 0) return false;

    pkt.data_length = (uint8_t)(payload_bits / 8);
    if (pkt.data_length > 64) {
        pkt.data_length = 64;
        overflow_errors++;
    }

    /* Extract payload bytes */
    for (uint8_t i = 0; i < pkt.data_length; i++) {
        pkt.data[i] = (uint8_t)extractBits(bits, 8 + i * 8, 8);
    }

    /* Verify CRC16 */
    uint16_t rx_crc   = extractBits(bits, 8 + payload_bits, 16);
    uint16_t calc_crc = crc16_usb(pkt.data, pkt.data_length);
    pkt.crc_valid     = (rx_crc == calc_crc);

    if (!pkt.crc_valid) crc_errors++;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  UTILITY — BIT EXTRACTION
 * ═══════════════════════════════════════════════════════════════════════════ */

uint16_t USBPacketDecoder::extractBits(
    const uint8_t* bits,
    uint16_t       offset,
    uint8_t        count
) {
    uint16_t value = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (readBit(bits, offset + i)) {
            value |= (1u << i);
        }
    }
    return value;
}
