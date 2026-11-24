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
    , _packet_callback(nullptr)
{
    reset();
}

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

void __attribute__((optimize("O3"), section(".time_critical.usb_feed")))
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
 *  DUAL-CORE STRIPED DECODER (100% LOSSLESS BOUNDARY OVERLAP)
 * ═══════════════════════════════════════════════════════════════════════════ */

void __attribute__((optimize("O3"), section(".time_critical.usb_striped")))
USBPacketDecoder::feedSamplesStriped(
    const uint32_t* buffer,
    uint16_t        word_count,
    const uint32_t* next_buffer,
    uint16_t        tail_words
) {
    const uint32_t ALL_J_WORD = (_speed == USBSpeed::LOW_SPEED) ? 0xAAAAAAAAu : 0x55555555u;
    const uint32_t J_RAW      = (_speed == USBSpeed::LOW_SPEED) ? 0b10u       : 0b01u;

    /*
     * Step 1: Synchronize at start of this buffer.
     * If buffer[0] is ALL_J_WORD, we are already cleanly in IDLE with IPG met.
     * Otherwise we may have landed mid-packet (already finished by the other core):
     * scan forward until either SE0 (0b00) OR 8 consecutive J bits (impossible
     * inside any valid USB packet due to 6-ones bit-stuffing).
     */
    uint16_t start_w = 0;
    if (buffer[0] == ALL_J_WORD) {
        _state       = DecoderState::IDLE;
        _prev_raw    = (uint8_t)J_RAW;
        _idle_ipg_ok = true;
        start_w      = 1;
    } else {
        uint32_t j_run = 0;
        bool synced    = false;
        for (uint16_t w = 0; w < word_count && !synced; w++) {
            uint32_t word = buffer[w];
            if (word == ALL_J_WORD) {
                _state       = DecoderState::IDLE;
                _prev_raw    = (uint8_t)J_RAW;
                _idle_ipg_ok = true;
                start_w      = w + 1;
                synced       = true;
                break;
            }
            for (uint32_t s = 0; s < 16; s++) {
                const uint32_t raw = (word >> (s * 2)) & 0x03u;
                if (raw == 0u) {
                    /* Hit EOP of the previous core's boundary packet! */
                    _state       = DecoderState::IDLE;
                    _prev_raw    = (uint8_t)J_RAW;
                    _idle_ipg_ok = false;
                    /* Process remaining symbols of this word cleanly */
                    uint32_t rem_word = word >> ((s + 1) * 2);
                    uint32_t rem_bits = 15 - s;
                    if (rem_bits > 0) {
                        /* Feed remaining bits of word w manually via IDLE/SYNC */
                        for (uint32_t r = 0; r < rem_bits; r++) {
                            uint32_t sym = rem_word & 0x03u;
                            rem_word >>= 2;
                            if (_state == DecoderState::IDLE) {
                                if (sym == J_RAW) {
                                    _idle_ipg_ok = true;
                                    _prev_raw    = (uint8_t)J_RAW;
                                } else if ((sym - 1u) <= 1u) {
                                    if (_idle_ipg_ok && _prev_raw == J_RAW) {
                                        _state          = DecoderState::SYNC;
                                        _sync_bit_count = 1;
                                        _idle_ipg_ok    = false;
                                    }
                                    _prev_raw = (uint8_t)sym;
                                } else {
                                    _idle_ipg_ok = false;
                                }
                            } else {
                                /* Rare: SYNC started inside same word right after SE0 */
                                uint32_t single = sym;
                                /* Hand off remaining words starting at w+1 after advancing SYNC */
                                if ((sym - 1u) > 1u) {
                                    _state = DecoderState::IDLE; _idle_ipg_ok = false; _prev_raw = (uint8_t)J_RAW;
                                } else {
                                    bool same = (sym == _prev_raw);
                                    _prev_raw = (uint8_t)sym;
                                    _sync_bit_count++;
                                    if (_sync_bit_count <= 7) {
                                        if (same) {
                                            if (_sync_bit_count > USB_MIN_SYNC_BITS) {
                                                _state = DecoderState::PACKET_BODY; _pkt_byte_count = 0; _acc = 0x80; _consecutive_ones = 0;
                                            } else {
                                                _state = DecoderState::IDLE; _idle_ipg_ok = false; _prev_raw = (uint8_t)J_RAW;
                                            }
                                        }
                                    } else if (same) {
                                        _state = DecoderState::PACKET_BODY; _pkt_byte_count = 0; _acc = 0x80; _consecutive_ones = 0;
                                    } else {
                                        _state = DecoderState::IDLE; _idle_ipg_ok = false; _prev_raw = (uint8_t)J_RAW;
                                    }
                                }
                                (void)single;
                            }
                        }
                    }
                    start_w = w + 1;
                    synced  = true;
                    break;
                } else if (raw == J_RAW) {
                    if (++j_run >= 8u) {
                        _state       = DecoderState::IDLE;
                        _prev_raw    = (uint8_t)J_RAW;
                        _idle_ipg_ok = true;
                        /* Process remaining bits of word w so a SYNC starting in this word is never missed */
                        uint32_t rem_word = word >> ((s + 1) * 2);
                        uint32_t rem_bits = 15 - s;
                        for (uint32_t r = 0; r < rem_bits; r++) {
                            uint32_t sym = rem_word & 0x03u;
                            rem_word >>= 2;
                            if (_state == DecoderState::IDLE) {
                                if (sym == J_RAW) {
                                    _idle_ipg_ok = true;
                                    _prev_raw    = (uint8_t)J_RAW;
                                } else if ((sym - 1u) <= 1u) {
                                    if (_idle_ipg_ok && _prev_raw == J_RAW) {
                                        _state          = DecoderState::SYNC;
                                        _sync_bit_count = 1;
                                        _idle_ipg_ok    = false;
                                    }
                                    _prev_raw = (uint8_t)sym;
                                } else {
                                    _idle_ipg_ok = false;
                                }
                            } else {
                                if ((sym - 1u) > 1u) {
                                    _state = DecoderState::IDLE; _idle_ipg_ok = false; _prev_raw = (uint8_t)J_RAW;
                                } else {
                                    bool same = (sym == _prev_raw);
                                    _prev_raw = (uint8_t)sym;
                                    _sync_bit_count++;
                                    if (_sync_bit_count <= 7) {
                                        if (same) {
                                            if (_sync_bit_count > USB_MIN_SYNC_BITS) {
                                                _state = DecoderState::PACKET_BODY; _pkt_byte_count = 0; _acc = 0x80; _consecutive_ones = 0;
                                            } else {
                                                _state = DecoderState::IDLE; _idle_ipg_ok = false; _prev_raw = (uint8_t)J_RAW;
                                            }
                                        }
                                    } else if (same) {
                                        _state = DecoderState::PACKET_BODY; _pkt_byte_count = 0; _acc = 0x80; _consecutive_ones = 0;
                                    } else {
                                        _state = DecoderState::IDLE; _idle_ipg_ok = false; _prev_raw = (uint8_t)J_RAW;
                                    }
                                }
                            }
                        }
                        start_w = w + 1;
                        synced  = true;
                        break;
                    }
                } else {
                    j_run = 0;
                }
            }
        }
        if (!synced) return;
    }

    /* Step 2: Decode the entire main body of this buffer at full speed */
    if (start_w < word_count) {
        feedSamples(buffer + start_w, word_count - start_w);
    }

    /* Step 3: If a packet is still open across the boundary, finish ONLY that
     * one packet by peeking into next_buffer (1 word at a time until IDLE). */
    if (_state != DecoderState::IDLE && next_buffer != nullptr) {
        for (uint16_t tw = 0; tw < tail_words && _state != DecoderState::IDLE; tw++) {
            uint32_t w = next_buffer[tw];
            for (uint32_t s = 16; s != 0 && _state != DecoderState::IDLE; s--) {
                const uint32_t raw = w & 0x03u;
                w >>= 2;
                if (_state == DecoderState::PACKET_BODY) {
                    if ((raw - 1u) <= 1u) {
                        const uint32_t bit = (raw == _prev_raw) ? 1u : 0u;
                        _prev_raw = (uint8_t)raw;
                        if (_consecutive_ones == 6u) {
                            _consecutive_ones = 0;
                            if (bit != 0u) { stuffing_errors++; _state = DecoderState::IDLE; }
                            continue;
                        }
                        const uint32_t done = _acc & 1u;
                        _acc = (uint8_t)((_acc >> 1) | (bit << 7));
                        _consecutive_ones = bit ? (_consecutive_ones + 1u) : 0u;
                        if (done) {
                            if (_pkt_byte_count < USB_MAX_PACKET_BYTES) {
                                _pkt_bytes[_pkt_byte_count++] = _acc;
                                _acc = 0x80u;
                            } else {
                                overflow_errors++; _state = DecoderState::IDLE;
                            }
                        }
                    } else {
                        if (raw == 0u && _acc == 0x80u && _pkt_byte_count >= 1u) {
                            finalizePacket(_pkt_byte_count);
                        }
                        _state = DecoderState::IDLE;
                    }
                } else {
                    /* SYNC crossing boundary */
                    if ((raw - 1u) > 1u) { _state = DecoderState::IDLE; break; }
                    const bool same = (raw == _prev_raw);
                    _prev_raw = (uint8_t)raw;
                    _sync_bit_count++;
                    if (_sync_bit_count <= 7u) {
                        if (same) {
                            if (_sync_bit_count > USB_MIN_SYNC_BITS) {
                                _state = DecoderState::PACKET_BODY; _pkt_byte_count = 0; _acc = 0x80u; _consecutive_ones = 0;
                            } else {
                                sync_errors++; _state = DecoderState::IDLE;
                            }
                        }
                    } else if (same) {
                        _state = DecoderState::PACKET_BODY; _pkt_byte_count = 0; _acc = 0x80u; _consecutive_ones = 0;
                    } else {
                        sync_errors++; _state = DecoderState::IDLE;
                    }
                }
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PACKET FINALIZATION & PARSING (SRAM + -O3)
 * ═══════════════════════════════════════════════════════════════════════════ */

void __attribute__((optimize("O3"), section(".time_critical.usb_finalize")))
USBPacketDecoder::finalizePacket(uint8_t byte_count) {
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
