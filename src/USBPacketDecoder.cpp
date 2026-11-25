/**
 * @file    USBPacketDecoder.cpp
 * @brief   Low-register-pressure SRAM-resident USB 1.1 packet decoder (single & dual-core).
 * @version 2.4.0
 * @license MIT
 *
 * @author Ângelo Moisés Alves
 */

#include "USBPacketDecoder.h"
#include "hardware/timer.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  SRAM-RESIDENT LOOKUP TABLES (CRC16 + CRC5 + PID CLASS)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const uint16_t __not_in_flash("usb_crc16_lut") kCrc16NibbleTable[16] = {
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400
};

static const uint8_t __not_in_flash("usb_crc5_lut") kCrc5NibbleTable[16] = {
    0x00, 0x16, 0x05, 0x13, 0x0A, 0x1C, 0x0F, 0x19,
    0x14, 0x02, 0x11, 0x07, 0x1E, 0x08, 0x1B, 0x0D
};

static const USBPacketType __not_in_flash("usb_pid_lut") kPidTypeTable[16] = {
    USBPacketType::UNKNOWN,    // 0000
    USBPacketType::TOKEN,      // 0001 OUT
    USBPacketType::HANDSHAKE,  // 0010 ACK
    USBPacketType::DATA,       // 0011 DATA0
    USBPacketType::UNKNOWN,    // 0100
    USBPacketType::TOKEN,      // 0101 SOF
    USBPacketType::UNKNOWN,    // 0110
    USBPacketType::UNKNOWN,    // 0111
    USBPacketType::UNKNOWN,    // 1000
    USBPacketType::TOKEN,      // 1001 IN
    USBPacketType::HANDSHAKE,  // 1010 NAK
    USBPacketType::DATA,       // 1011 DATA1
    USBPacketType::SPECIAL,    // 1100 PRE
    USBPacketType::TOKEN,      // 1101 SETUP
    USBPacketType::HANDSHAKE,  // 1110 STALL
    USBPacketType::UNKNOWN     // 1111
};

uint16_t __attribute__((optimize("O3"), section(".time_critical.usb_crc16")))
USBPacketDecoder::crc16_fast(const uint8_t* data, uint8_t length) {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < length; i++) {
        uint8_t b = data[i];
        crc = (crc >> 4) ^ kCrc16NibbleTable[(crc ^ b) & 0x0F];
        crc = (crc >> 4) ^ kCrc16NibbleTable[(crc ^ (b >> 4)) & 0x0F];
    }
    return crc ^ 0xFFFF;
}

uint8_t __attribute__((optimize("O3"), section(".time_critical.usb_crc5")))
USBPacketDecoder::crc5_fast(uint16_t d) {
    uint8_t c = 0x1F;
    c = (c >> 4) ^ kCrc5NibbleTable[(c ^ d) & 0x0F];
    c = (c >> 4) ^ kCrc5NibbleTable[(c ^ (d >> 4)) & 0x0F];
    uint8_t d3 = (uint8_t)((d >> 8) & 0x07u);
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t fb = (c ^ (d3 >> i)) & 1u;
        c >>= 1;
        if (fb) c ^= 0x14u;
    }
    return c ^ 0x1Fu;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CONSTRUCTION / RESET
 * ═══════════════════════════════════════════════════════════════════════════ */

USBPacketDecoder::USBPacketDecoder(USBSpeed speed)
    : _speed(speed)
    , _emit_enabled(true)
    , _pkt_owned(true)
    , _packet_callback(nullptr)
{
    reset();
}

/* Leaves _emit_enabled and _pkt_owned alone: reset() may run on the other core while
 * feedSamplesStriped() is in one of its silent phases. */
void USBPacketDecoder::reset() {
    _state            = DecoderState::IDLE;
    _prev_raw         = (_speed == USBSpeed::LOW_SPEED) ? 0b10u : 0b01u;
    _sync_bit_count   = 0;
    _idle_ipg_ok      = false;

    _pkt_byte_count   = 0;
    _acc              = 0x80u;
    _consecutive_ones = 0;

    packets_decoded   = 0;
    sync_errors       = 0;
    crc_errors        = 0;
    stuffing_errors   = 0;
    overflow_errors   = 0;
    fifo_overruns     = 0;
}

void USBPacketDecoder::onPacket(USBPacketCallback cb) {
    _packet_callback = cb;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HOT DECODING LOOP (MINIMAL REGISTER PRESSURE, SENTINEL-BIT PACKER)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* noinline: the dual-core helpers below call this loop several times; one RAM copy is enough. */
void __attribute__((optimize("O3"), noinline, section(".time_critical.usb_feed")))
USBPacketDecoder::feedSamples(const uint32_t* buffer, uint16_t word_count) {
    const uint32_t ALL_J_WORD = (_speed == USBSpeed::LOW_SPEED) ? 0xAAAAAAAAu : 0x55555555u;
    const uint32_t J_RAW      = (_speed == USBSpeed::LOW_SPEED) ? 0b10u       : 0b01u;

    uint32_t state            = (uint32_t)_state;
    uint32_t prev_raw         = _prev_raw;
    uint32_t sync_bit_count   = _sync_bit_count;
    uint32_t idle_ipg_ok      = _idle_ipg_ok ? 1u : 0u;

    uint32_t pkt_byte_count   = _pkt_byte_count;
    uint32_t acc              = _acc;
    uint32_t consecutive_ones = _consecutive_ones;

    const uint32_t* ptr = buffer;
    const uint32_t* end = buffer + word_count;

    while (ptr < end) {
        uint32_t word = *ptr++;

        /* FAST PATH: skip 16 idle J bits in O(1) */
        if (word == ALL_J_WORD && state == (uint32_t)DecoderState::IDLE) {
            idle_ipg_ok = 1u;
            prev_raw    = J_RAW;
            continue;
        }

        for (uint32_t s = 16; s != 0; s--) {
            const uint32_t raw = word & 0x03u;
            word >>= 2;

            if (state == (uint32_t)DecoderState::PACKET_BODY) {
                if (__builtin_expect((raw - 1u) <= 1u, 1)) {
                    const uint32_t bit = (raw == prev_raw) ? 1u : 0u;
                    prev_raw = raw;

                    if (__builtin_expect(consecutive_ones == 6u, 0)) {
                        consecutive_ones = 0u;
                        if (__builtin_expect(bit != 0u, 0)) {
                            stuffing_errors++;
                            state       = (uint32_t)DecoderState::IDLE;
                            idle_ipg_ok = 0u;
                            prev_raw    = J_RAW;
                        }
                        continue;
                    }

                    const uint32_t done = acc & 1u;
                    acc = (acc >> 1) | (bit << 7);
                    consecutive_ones = bit ? (consecutive_ones + 1u) : 0u;

                    if (__builtin_expect(done != 0u, 0)) {
                        if (__builtin_expect(pkt_byte_count < USB_MAX_PACKET_BYTES, 1)) {
                            _pkt_bytes[pkt_byte_count++] = (uint8_t)acc;
                            acc = 0x80u;
                        } else {
                            overflow_errors++;
                            state       = (uint32_t)DecoderState::IDLE;
                            idle_ipg_ok = 0u;
                            prev_raw    = J_RAW;
                        }
                    }
                } else {
                    /* SE0 (EOP) or SE1 */
                    if (raw == 0u && acc == 0x80u && pkt_byte_count >= 1u) {
                        finalizePacket((uint8_t)pkt_byte_count);
                    }
                    state       = (uint32_t)DecoderState::IDLE;
                    idle_ipg_ok = 0u;
                    prev_raw    = J_RAW;
                }
            } else if (state == (uint32_t)DecoderState::IDLE) {
                if (raw == J_RAW) {
                    idle_ipg_ok = 1u;
                    prev_raw    = J_RAW;
                } else if ((raw - 1u) <= 1u) {
                    if (idle_ipg_ok && prev_raw == J_RAW) {
                        state          = (uint32_t)DecoderState::SYNC;
                        sync_bit_count = 1u;
                        idle_ipg_ok    = 0u;
                        _pkt_owned     = _emit_enabled;
                    }
                    prev_raw = raw;
                } else {
                    idle_ipg_ok = 0u;
                }
            } else {
                /* SYNC */
                if (__builtin_expect((raw - 1u) > 1u, 0)) {
                    state       = (uint32_t)DecoderState::IDLE;
                    idle_ipg_ok = 0u;
                    prev_raw    = J_RAW;
                    continue;
                }

                const bool same = (raw == prev_raw);
                prev_raw = raw;
                sync_bit_count++;

                if (sync_bit_count <= 7u) {
                    if (same) {
                        if (sync_bit_count > USB_MIN_SYNC_BITS) {
                            state            = (uint32_t)DecoderState::PACKET_BODY;
                            pkt_byte_count   = 0u;
                            acc              = 0x80u;
                            consecutive_ones = 0u;
                        } else {
                            sync_errors++;
                            state       = (uint32_t)DecoderState::IDLE;
                            prev_raw    = J_RAW;
                            idle_ipg_ok = 0u;
                        }
                    }
                } else {
                    if (same) {
                        state            = (uint32_t)DecoderState::PACKET_BODY;
                        pkt_byte_count   = 0u;
                        acc              = 0x80u;
                        consecutive_ones = 0u;
                    } else {
                        sync_errors++;
                        state       = (uint32_t)DecoderState::IDLE;
                        prev_raw    = J_RAW;
                        idle_ipg_ok = 0u;
                    }
                }
            }
        }
    }

    _state            = (DecoderState)state;
    _prev_raw         = (uint8_t)prev_raw;
    _sync_bit_count   = (uint8_t)sync_bit_count;
    _idle_ipg_ok      = (idle_ipg_ok != 0u);
    _pkt_byte_count   = (uint8_t)pkt_byte_count;
    _acc              = (uint8_t)acc;
    _consecutive_ones = (uint8_t)consecutive_ones;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  DUAL-CORE STRIPED DECODER (A PACKET BELONGS TO THE BUFFER WHERE ITS SYNC STARTS)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Finds the first symbol in @p words after which the decoder is in the same state
 * whatever state it was in before, and decodes the rest from the symbol right after
 * it. Two kinds of symbol qualify:
 *   - an SE0: every state goes to IDLE without the inter-packet gap flag;
 *   - the 10th J in a row. Inside a valid packet bit stuffing caps a run at 7 equal
 *     symbols, and the decoder also accepts a SYNC that ends in JJ, which allows 8.
 *     In a corrupted stream the next J is a stuffing error that leaves the packet for
 *     IDLE without the gap flag, and the J after it sets the flag. So after 10 J the
 *     decoder is in IDLE with the flag set, from any state. (The 8th J is not enough:
 *     after a stuffing error the decoder would still be waiting for the gap.)
 * Returns false if no such symbol was found; @p j_run carries the J run into the next
 * call. Kept out of feedSamples() so the hot loop stays as small as in v1.2.0 (its
 * branches are at the Thumb-1 range limit). Runs once per buffer over at most
 * margin_words words and usually stops within a few symbols, so it is built for size.
 */
bool __attribute__((optimize("Os"), section(".time_critical.usb_resync")))
USBPacketDecoder::feedFromSafePoint(const uint32_t* words, uint16_t count, uint32_t& j_run) {
    const uint32_t J_RAW      = (_speed == USBSpeed::LOW_SPEED) ? 0b10u       : 0b01u;
    const uint32_t ALL_J_WORD = (_speed == USBSpeed::LOW_SPEED) ? 0xAAAAAAAAu : 0x55555555u;

    for (uint16_t w = 0; w < count; w++) {
        const uint32_t word = words[w];
        for (uint32_t s = 0; s < 16u; s++) {
            const uint32_t raw = (word >> (2u * s)) & 0x03u;
            if (raw == J_RAW) {
                if (++j_run < 10u) continue;
            } else if (raw != 0u) {
                j_run = 0u;
                continue;
            }

            /* The state a continuous decoder has right after this symbol. (The replay
             * below sets the gap flag the same way, from the fill symbols.) */
            _state       = DecoderState::IDLE;
            _prev_raw    = (uint8_t)J_RAW;
            _idle_ipg_ok = (raw == J_RAW);

            /* Decode the rest of this word with symbols 0..s replaced by the one just
             * seen: in IDLE, more SE0 (after an SE0) or more J (after 10 J) change nothing. */
            const uint32_t keep = (s == 15u) ? 0u : (0xFFFFFFFFu << (2u * (s + 1u)));
            const uint32_t fill = (raw == 0u) ? 0u : ALL_J_WORD;
            const uint32_t rest = (word & keep) | (fill & ~keep);
            feedSamples(&rest, 1);
            if (w + 1u < count) {
                feedSamples(words + w + 1u, (uint16_t)(count - w - 1u));
            }
            return true;
        }
    }
    return false;
}

void __attribute__((optimize("O3"), section(".time_critical.usb_striped")))
USBPacketDecoder::feedSamplesStriped(
    const uint32_t* buffer,
    uint16_t        word_count,
    const uint32_t* next_buffer,
    uint16_t        margin_words,
    const uint32_t* prev_buffer
) {
    /*
     * Phase 1, entry (silent): decode the last margin_words words of the previous
     * buffer from their first safe point, so that at the boundary this decoder is in
     * the state the previous buffer's decoder reaches. In valid traffic the window
     * always holds a safe point (it is longer than a packet plus 10 J); a corrupted
     * stream may hold none, and then decoding resumes at the first safe point of this
     * buffer. Packets that start here belong to the previous buffer: finalizePacket()
     * drops them, and the error counters are restored afterwards. A stats() read on the
     * other core during this phase may briefly include them, and a reset() from the
     * other core during this phase is undone for these three counters.
     */
    const uint32_t sync0 = sync_errors, stuff0 = stuffing_errors, ovf0 = overflow_errors;
    _emit_enabled = false;
    _pkt_owned    = false;
    uint32_t j_run = 0;
    bool synced = false;
    const uint16_t lead = (margin_words < word_count) ? margin_words : word_count;
    if (prev_buffer != nullptr && lead > 0) {
        synced = feedFromSafePoint(prev_buffer + (word_count - lead), lead, j_run);
    }
    sync_errors     = sync0;
    stuffing_errors = stuff0;
    overflow_errors = ovf0;

    /*
     * Phase 2, buffer: every packet whose SYNC starts here is emitted and counted.
     * A packet left open by phase 1 is followed to its EOP but is not emitted; if it
     * is corrupted, its errors after the boundary are counted by both cores.
     * Without a previous buffer, decoding starts at the first safe point.
     */
    _emit_enabled = true;
    if (synced) {
        feedSamples(buffer, word_count);
    } else {
        feedFromSafePoint(buffer, word_count, j_run);
    }

    /*
     * Phase 3, tail: finish a packet of this buffer that is still open, reading
     * next_buffer one word at a time, at most lead words (next_buffer holds word_count).
     * Packets that start there belong to the next call and are not emitted; in a
     * corrupted stream, errors right after that packet's end, in its last word, are
     * also counted by the next call.
     */
    _emit_enabled = false;
    for (uint16_t tw = 0; next_buffer != nullptr && tw < lead && _pkt_owned
             && (_state == DecoderState::SYNC || _state == DecoderState::PACKET_BODY); tw++) {
        feedSamples(next_buffer + tw, 1);
    }
    _emit_enabled = true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PACKET FINALIZATION & PARSING (SRAM + -O3)
 * ═══════════════════════════════════════════════════════════════════════════ */

void __attribute__((optimize("O3"), section(".time_critical.usb_finalize")))
USBPacketDecoder::finalizePacket(uint8_t byte_count) {
    /* Dual-core overlap: the buffer where this packet's SYNC started reports it. */
    if (!_pkt_owned) return;

    const uint8_t pid_byte = _pkt_bytes[0];
    const uint8_t pid_low  = pid_byte & 0x0Fu;
    if (__builtin_expect(((pid_low ^ (pid_byte >> 4)) & 0x0Fu) != 0x0Fu, 0)) {
        return;
    }

    USBPacket& pkt   = _pkt;
    pkt.timestamp_us = timer_hw->timerawl;
    pkt.pid          = pid_low;
    pkt.type         = kPidTypeTable[pid_low];
    pkt.data_length  = 0;

    switch (pkt.type) {
        case USBPacketType::HANDSHAKE:
        case USBPacketType::SPECIAL:
            if (byte_count != 1) return;
            pkt.crc_valid = true;
            break;

        case USBPacketType::TOKEN: {
            if (byte_count != 3) return;
            const uint16_t raw16     = (uint16_t)_pkt_bytes[1] | ((uint16_t)_pkt_bytes[2] << 8);
            const uint16_t payload11 = raw16 & 0x07FFu;
            const uint8_t  rx_crc    = (uint8_t)(raw16 >> 11);
            pkt.crc_valid = (rx_crc == crc5_fast(payload11));
            if (!pkt.crc_valid) crc_errors++;

            if (pkt.pid == USBPID::SOF) {
                pkt.frame_number = payload11;
            } else {
                pkt.addr = (uint8_t)(payload11 & 0x7Fu);
                pkt.endp = (uint8_t)((payload11 >> 7) & 0x0Fu);
            }
            break;
        }

        case USBPacketType::DATA: {
            if (byte_count < 3) return;
            const uint8_t dlen = (uint8_t)(byte_count - 3);
            if (dlen > 64) {
                overflow_errors++;
                return;
            }
            pkt.data_length = dlen;
            if (dlen > 0) {
                memcpy(pkt.data, &_pkt_bytes[1], dlen);
            }
            const uint16_t rx_crc = (uint16_t)_pkt_bytes[1 + dlen]
                                  | ((uint16_t)_pkt_bytes[2 + dlen] << 8);
            pkt.crc_valid = (rx_crc == crc16_fast(pkt.data, dlen));
            if (!pkt.crc_valid) crc_errors++;
            break;
        }

        default:
            return;
    }

    packets_decoded++;
    if (_packet_callback) {
        _packet_callback(pkt);
    }
}
