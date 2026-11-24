/**
 * @file    USBSnifferPIO_RP2040.cpp
 * @brief   USBSnifferPIO with 0 ns gap 2-ch HW ping-pong DMA (4-slot ring) + Dual-Core decode.
 * @version 2.4.0
 * @license MIT
 *
 * @author Ângelo Moisés Alves
 */

#include "USBSnifferPIO_RP2040.h"
#include "usb_sniffer.pio.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include <string.h>

#if SNIFFER_DMA_IRQ_INDEX == 0
  #define SNIFFER_DMA_IRQ_NUM          DMA_IRQ_0
  #define SNIFFER_DMA_INTS_REG         (dma_hw->ints0)
  #define SNIFFER_DMA_SET_IRQ_EN(c, e) dma_channel_set_irq0_enabled((c), (e))
#else
  #define SNIFFER_DMA_IRQ_NUM          DMA_IRQ_1
  #define SNIFFER_DMA_INTS_REG         (dma_hw->ints1)
  #define SNIFFER_DMA_SET_IRQ_EN(c, e) dma_channel_set_irq1_enabled((c), (e))
#endif

USBSnifferPIO* USBSnifferPIO::_irq_instance = nullptr;

/* ═══════════════════════════════════════════════════════════════════════════
 *  CONSTRUCTION / DESTRUCTION
 * ═══════════════════════════════════════════════════════════════════════════ */

USBSnifferPIO::USBSnifferPIO()
    : _running(false)
    , _speed(USBSpeed::LOW_SPEED)
    , _pio(nullptr)
    , _sm(0)
    , _pio_offset(0)
    , _dma_wr(0)
    , _dma_rd(0)
    , _dma_rd_c0(1)
    , _ch0_next_slot(2u & SNIFFER_DMA_BUF_MASK)
    , _ch1_next_slot(3u & SNIFFER_DMA_BUF_MASK)
    , _dual_core(false)
    , _ring_overruns(0)
    , _warmup_remaining(2)
    , _warmup_config(2)
    , _decoder(USBSpeed::LOW_SPEED)
    , _decoder_c0(USBSpeed::LOW_SPEED)
    , _has_c0_cb(false)
{
    _dma_chan[0] = -1;
    _dma_chan[1] = -1;
    memset(_dma_buf, 0, sizeof(_dma_buf));
}

USBSnifferPIO::~USBSnifferPIO() {
    if (_running) end();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  INITIALIZATION / SHUTDOWN
 * ═══════════════════════════════════════════════════════════════════════════ */

bool USBSnifferPIO::begin(uint pin_dp, USBSpeed speed, uint pio_instance) {
    if (_running) end();

    _speed            = speed;
    _dma_wr           = 0;
    _dma_rd           = 0;
    _dma_rd_c0        = 1;
    _ch0_next_slot    = 2u & SNIFFER_DMA_BUF_MASK;
    _ch1_next_slot    = 3u & SNIFFER_DMA_BUF_MASK;
    _ring_overruns    = 0;
    _warmup_remaining = _warmup_config;
    _decoder          = USBPacketDecoder(speed);
    _decoder_c0       = USBPacketDecoder(speed);

    _pio = (pio_instance == 0) ? pio0 : pio1;

    int sm = pio_claim_unused_sm(_pio, false);
    if (sm < 0) return false;
    _sm = (uint)sm;

    if (!pio_can_add_program(_pio, &usb_sniffer_program)) {
        pio_sm_unclaim(_pio, _sm);
        return false;
    }
    _pio_offset = pio_add_program(_pio, &usb_sniffer_program);

    usb_sniffer_program_init(_pio, _sm, _pio_offset, pin_dp, calcClockDiv());

    if (!setupDMA()) {
        pio_sm_set_enabled(_pio, _sm, false);
        pio_remove_program(_pio, &usb_sniffer_program, _pio_offset);
        pio_sm_unclaim(_pio, _sm);
        return false;
    }

    _running = true;
    return true;
}

void USBSnifferPIO::end() {
    if (!_running) return;

    teardownDMA();
    pio_sm_set_enabled(_pio, _sm, false);
    pio_remove_program(_pio, &usb_sniffer_program, _pio_offset);
    pio_sm_unclaim(_pio, _sm);

    _running = false;
}

void USBSnifferPIO::enableDualCore(bool enable) {
    if (enable && SNIFFER_DMA_BUF_COUNT < 4) {
        _dual_core = false;
        return;
    }
    _dual_core = enable;
    if (enable) {
        /* Align Core 1 to next even index, Core 0 to next odd index */
        uint32_t wr = _dma_wr;
        _dma_rd    = (wr & ~1u);
        _dma_rd_c0 = (wr & ~1u) + 1u;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CLOCK DIVIDER
 * ═══════════════════════════════════════════════════════════════════════════ */

float USBSnifferPIO::calcClockDiv() const {
    const float sys_hz = static_cast<float>(clock_get_hz(clk_sys));
    const float pio_hz = (_speed == USBSpeed::FULL_SPEED) ? 96.0e6f : 12.0e6f;
    return sys_hz / pio_hz;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ZERO-GAP 2-CHANNEL HARDWARE PING-PONG DMA ROTATING ACROSS 4 RING BUFFERS
 *    ch0: slot 0 -> slot 2 -> slot 0 ...  (even buffers)
 *    ch1: slot 1 -> slot 3 -> slot 1 ...  (odd  buffers)
 * ═══════════════════════════════════════════════════════════════════════════ */

void __attribute__((optimize("O3"), section(".time_critical.usb_dma_irq")))
USBSnifferPIO::dmaIrqHandler() {
    USBSnifferPIO* self = _irq_instance;
    if (!self) return;

    const int ch0 = self->_dma_chan[0];
    const int ch1 = self->_dma_chan[1];
    if (ch0 < 0 || ch1 < 0) return;

    const uint32_t m0 = 1u << (uint)ch0;
    const uint32_t m1 = 1u << (uint)ch1;
    const uint32_t pending = SNIFFER_DMA_INTS_REG & (m0 | m1);
    if (!pending) return;

    SNIFFER_DMA_INTS_REG = pending;

    if (pending & m0) {
        const uint8_t next_slot = self->_ch0_next_slot;
        dma_hw->ch[ch0].write_addr     = (uintptr_t)self->_dma_buf[next_slot];
        dma_hw->ch[ch0].transfer_count = SNIFFER_DMA_BUF_WORDS;
        self->_ch0_next_slot = (next_slot + 2u) & SNIFFER_DMA_BUF_MASK;
        self->_dma_wr++;
    }
    if (pending & m1) {
        const uint8_t next_slot = self->_ch1_next_slot;
        dma_hw->ch[ch1].write_addr     = (uintptr_t)self->_dma_buf[next_slot];
        dma_hw->ch[ch1].transfer_count = SNIFFER_DMA_BUF_WORDS;
        self->_ch1_next_slot = (next_slot + 2u) & SNIFFER_DMA_BUF_MASK;
        self->_dma_wr++;
    }

    /* Detect overrun if slowest consumer fell >= 3 buffers behind DMA */
    const uint32_t wr = self->_dma_wr;
    if (!self->_dual_core) {
        if ((uint32_t)(wr - self->_dma_rd) >= SNIFFER_DMA_BUF_COUNT) {
            self->_ring_overruns++;
            self->_dma_rd = wr - (SNIFFER_DMA_BUF_COUNT - 1u);
        }
    } else {
        if ((uint32_t)(wr - self->_dma_rd) >= SNIFFER_DMA_BUF_COUNT) {
            self->_ring_overruns++;
            self->_dma_rd = (wr - 2u) & ~1u;
        }
        if ((uint32_t)(wr - self->_dma_rd_c0) >= SNIFFER_DMA_BUF_COUNT) {
            self->_ring_overruns++;
            self->_dma_rd_c0 = ((wr - 2u) & ~1u) | 1u;
        }
    }
}

bool USBSnifferPIO::setupDMA() {
    /* Guard: only one active USBSnifferPIO instance can own the DMA IRQ slot */
    if (_irq_instance != nullptr && _irq_instance != this) {
        return false;
    }

    _dma_chan[0] = dma_claim_unused_channel(false);
    if (_dma_chan[0] < 0) return false;

    _dma_chan[1] = dma_claim_unused_channel(false);
    if (_dma_chan[1] < 0) {
        dma_channel_unclaim(_dma_chan[0]);
        _dma_chan[0] = -1;
        return false;
    }

    const uint dreq = pio_get_dreq(_pio, _sm, false);

    dma_channel_config c0 = dma_channel_get_default_config(_dma_chan[0]);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);
    channel_config_set_read_increment(&c0,  false);
    channel_config_set_write_increment(&c0, true);
    channel_config_set_dreq(&c0, dreq);
    channel_config_set_chain_to(&c0, _dma_chan[1]);

    dma_channel_config c1 = dma_channel_get_default_config(_dma_chan[1]);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);
    channel_config_set_read_increment(&c1,  false);
    channel_config_set_write_increment(&c1, true);
    channel_config_set_dreq(&c1, dreq);
    channel_config_set_chain_to(&c1, _dma_chan[0]);

    _irq_instance = this;
    SNIFFER_DMA_INTS_REG = (1u << (uint)_dma_chan[0]) | (1u << (uint)_dma_chan[1]);
    SNIFFER_DMA_SET_IRQ_EN(_dma_chan[0], true);
    SNIFFER_DMA_SET_IRQ_EN(_dma_chan[1], true);
    irq_add_shared_handler(SNIFFER_DMA_IRQ_NUM, dmaIrqHandler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(SNIFFER_DMA_IRQ_NUM, true);

    dma_channel_configure(
        _dma_chan[1], &c1,
        _dma_buf[1],
        &_pio->rxf[_sm],
        SNIFFER_DMA_BUF_WORDS,
        false
    );

    dma_channel_configure(
        _dma_chan[0], &c0,
        _dma_buf[0],
        &_pio->rxf[_sm],
        SNIFFER_DMA_BUF_WORDS,
        true
    );

    return true;
}

void USBSnifferPIO::teardownDMA() {
    if (_dma_chan[0] >= 0 || _dma_chan[1] >= 0) {
        if (_dma_chan[0] >= 0) SNIFFER_DMA_SET_IRQ_EN(_dma_chan[0], false);
        if (_dma_chan[1] >= 0) SNIFFER_DMA_SET_IRQ_EN(_dma_chan[1], false);
        if (_dma_chan[0] >= 0) dma_channel_abort(_dma_chan[0]);
        if (_dma_chan[1] >= 0) dma_channel_abort(_dma_chan[1]);
        irq_remove_handler(SNIFFER_DMA_IRQ_NUM, dmaIrqHandler);
        if (_irq_instance == this) _irq_instance = nullptr;
        if (_dma_chan[0] >= 0) { dma_channel_unclaim(_dma_chan[0]); _dma_chan[0] = -1; }
        if (_dma_chan[1] >= 0) { dma_channel_unclaim(_dma_chan[1]); _dma_chan[1] = -1; }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PROCESSING LOOPS (CORE 1 + OPTIONAL CORE 0 PARALLEL STRIPING)
 * ═══════════════════════════════════════════════════════════════════════════ */

void __attribute__((optimize("O3"), section(".time_critical.usb_task")))
USBSnifferPIO::task() {
    if (!_running) return;

    const uint32_t wr     = _dma_wr;
    const uint32_t rd_idx = _dma_rd;

    if (!_dual_core) {
        /* ── Single-core mode (default, 100% backwards-compatible) ── */
        if (rd_idx == wr) return;

        const uint32_t slot = rd_idx & SNIFFER_DMA_BUF_MASK;
        const uint32_t rxstall_mask = 1u << (PIO_FDEBUG_RXSTALL_LSB + _sm);
        const bool fifo_stalled = (_pio->fdebug & rxstall_mask) != 0;
        if (fifo_stalled) _pio->fdebug = rxstall_mask;

        if (_warmup_remaining > 0) {
            _warmup_remaining--;
            _ring_overruns = 0;
            if (_warmup_remaining == 0) _decoder.reset();
            _dma_rd = rd_idx + 1u;
            return;
        }

        if (fifo_stalled) _decoder.fifo_overruns++;
        if (_ring_overruns > 0) {
            uint32_t ovf = _ring_overruns;
            _ring_overruns = 0;
            _decoder.fifo_overruns += ovf;
        }

        _decoder.feedSamples(_dma_buf[slot], SNIFFER_DMA_BUF_WORDS);
        _dma_rd = rd_idx + 1u;
    } else {
        /* ── Dual-core mode: Core 1 decodes EVEN buffers (0, 2, 4...) ──
         * Wait until wr >= rd_idx + 2 so next_buffer (slot+1) is 100% filled
         * for lossless boundary-packet completion. */
        if ((int32_t)(wr - rd_idx) < 2) return;

        const uint32_t slot      = rd_idx & SNIFFER_DMA_BUF_MASK;
        const uint32_t next_slot = (rd_idx + 1u) & SNIFFER_DMA_BUF_MASK;

        const uint32_t rxstall_mask = 1u << (PIO_FDEBUG_RXSTALL_LSB + _sm);
        const bool fifo_stalled = (_pio->fdebug & rxstall_mask) != 0;
        if (fifo_stalled) _pio->fdebug = rxstall_mask;

        if (_warmup_remaining > 0) {
            _warmup_remaining--;
            _ring_overruns = 0;
            if (_warmup_remaining == 0) _decoder.reset();
            _dma_rd = rd_idx + 2u;
            return;
        }

        if (fifo_stalled) _decoder.fifo_overruns++;
        if (_ring_overruns > 0) {
            uint32_t ovf = _ring_overruns;
            _ring_overruns = 0;
            _decoder.fifo_overruns += ovf;
        }

        _decoder.feedSamplesStriped(
            _dma_buf[slot], SNIFFER_DMA_BUF_WORDS,
            _dma_buf[next_slot], SNIFFER_DUALCORE_TAIL_WORDS
        );
        _dma_rd = rd_idx + 2u;
    }
}

void __attribute__((optimize("O3"), section(".time_critical.usb_task_c0")))
USBSnifferPIO::taskCore0() {
    if (!_running || !_dual_core) return;

    const uint32_t wr     = _dma_wr;
    const uint32_t rd_idx = _dma_rd_c0;
    if ((int32_t)(wr - rd_idx) < 2) return;

    if (_warmup_remaining > 0) {
        _dma_rd_c0 = rd_idx + 2u;
        return;
    }

    const uint32_t slot      = rd_idx & SNIFFER_DMA_BUF_MASK;
    const uint32_t next_slot = (rd_idx + 1u) & SNIFFER_DMA_BUF_MASK;

    _decoder_c0.feedSamplesStriped(
        _dma_buf[slot], SNIFFER_DMA_BUF_WORDS,
        _dma_buf[next_slot], SNIFFER_DUALCORE_TAIL_WORDS
    );
    _dma_rd_c0 = rd_idx + 2u;
}

USBSnifferStats USBSnifferPIO::stats() const {
    USBSnifferStats s;
    s.packets_decoded = _decoder.packets_decoded + _decoder_c0.packets_decoded;
    s.crc_errors      = _decoder.crc_errors      + _decoder_c0.crc_errors;
    s.sync_errors     = _decoder.sync_errors     + _decoder_c0.sync_errors;
    s.stuffing_errors = _decoder.stuffing_errors + _decoder_c0.stuffing_errors;
    s.overflow_errors = _decoder.overflow_errors + _decoder_c0.overflow_errors;
    s.fifo_overruns   = _decoder.fifo_overruns   + _decoder_c0.fifo_overruns;
    s.sys_clock_hz    = clock_get_hz(clk_sys);
    s.pio_clock_div   = calcClockDiv();
    s.dma_buf_words   = SNIFFER_DMA_BUF_WORDS;
    s.dma_buf_count   = SNIFFER_DMA_BUF_COUNT;
    s.dual_core       = _dual_core;
    s.running         = _running;
    return s;
}

void USBSnifferPIO::resetStats() {
    _decoder.reset();
    _decoder_c0.reset();
    _ring_overruns = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CALLBACK DELEGATION
 * ═══════════════════════════════════════════════════════════════════════════ */

void USBSnifferPIO::onPacket(USBPacketCallback callback) {
    _decoder.onPacket(callback);
    if (!_has_c0_cb) {
        _decoder_c0.onPacket(callback);
    }
}

void USBSnifferPIO::onPacketCore0(USBPacketCallback callback) {
    _has_c0_cb = (callback != nullptr);
    _decoder_c0.onPacket(callback);
}
