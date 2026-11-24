/**
 * @file    USBSnifferPIO_RP2040.cpp
 * @brief   Implementation of USBSnifferPIO with zero-gap 2-channel hardware ping-pong DMA.
 * @version 2.2.0
 * @license MIT
 *
 * @author Ângelo Moisés Alves
 */

#include "USBSnifferPIO_RP2040.h"
#include "usb_sniffer.pio.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include <string.h>

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
    , _ring_overruns(0)
    , _warmup_remaining(2)
    , _decoder(USBSpeed::LOW_SPEED)
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
    _ring_overruns    = 0;
    _warmup_remaining = 2;
    _decoder          = USBPacketDecoder(speed);

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

/* ═══════════════════════════════════════════════════════════════════════════
 *  CLOCK DIVIDER
 * ═══════════════════════════════════════════════════════════════════════════ */

float USBSnifferPIO::calcClockDiv() const {
    const float sys_hz = static_cast<float>(clock_get_hz(clk_sys));
    const float pio_hz = (_speed == USBSpeed::FULL_SPEED) ? 96.0e6f : 12.0e6f;
    return sys_hz / pio_hz;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ZERO-GAP 2-CHANNEL HARDWARE PING-PONG DMA (0 ns GAP, SAFE WRITE_ADDR RELOAD)
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
    const uint32_t pending = dma_hw->ints1 & (m0 | m1);
    if (!pending) return;

    dma_hw->ints1 = pending;

    if (pending & m0) {
        /* ch0 finished _dma_buf[0]; hardware already chained to ch1 (_dma_buf[1]).
         * Safely reset ch0 write_addr + transfer_count (no trigger) with 2.73 ms margin. */
        dma_hw->ch[ch0].write_addr     = (uintptr_t)self->_dma_buf[0];
        dma_hw->ch[ch0].transfer_count = SNIFFER_DMA_BUF_WORDS;
        self->_dma_wr++;
    }
    if (pending & m1) {
        /* ch1 finished _dma_buf[1]; hardware already chained to ch0 (_dma_buf[0]). */
        dma_hw->ch[ch1].write_addr     = (uintptr_t)self->_dma_buf[1];
        dma_hw->ch[ch1].transfer_count = SNIFFER_DMA_BUF_WORDS;
        self->_dma_wr++;
    }

    /* Detect if consumer fell behind by >= 2 buffers */
    if ((uint32_t)(self->_dma_wr - self->_dma_rd) > 1u) {
        self->_ring_overruns++;
        self->_dma_rd = self->_dma_wr - 1u;
    }
}

bool USBSnifferPIO::setupDMA() {
    _dma_chan[0] = dma_claim_unused_channel(false);
    if (_dma_chan[0] < 0) return false;

    _dma_chan[1] = dma_claim_unused_channel(false);
    if (_dma_chan[1] < 0) {
        dma_channel_unclaim(_dma_chan[0]);
        _dma_chan[0] = -1;
        return false;
    }

    const uint dreq = pio_get_dreq(_pio, _sm, false);

    /* Channel 0: PIO RX FIFO -> _dma_buf[0], chain_to = chan 1 */
    dma_channel_config c0 = dma_channel_get_default_config(_dma_chan[0]);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);
    channel_config_set_read_increment(&c0,  false);
    channel_config_set_write_increment(&c0, true);
    channel_config_set_dreq(&c0, dreq);
    channel_config_set_chain_to(&c0, _dma_chan[1]);

    /* Channel 1: PIO RX FIFO -> _dma_buf[1], chain_to = chan 0 */
    dma_channel_config c1 = dma_channel_get_default_config(_dma_chan[1]);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);
    channel_config_set_read_increment(&c1,  false);
    channel_config_set_write_increment(&c1, true);
    channel_config_set_dreq(&c1, dreq);
    channel_config_set_chain_to(&c1, _dma_chan[0]);

    _irq_instance = this;
    dma_hw->ints1 = (1u << (uint)_dma_chan[0]) | (1u << (uint)_dma_chan[1]);
    dma_channel_set_irq1_enabled(_dma_chan[0], true);
    dma_channel_set_irq1_enabled(_dma_chan[1], true);
    irq_add_shared_handler(DMA_IRQ_1, dmaIrqHandler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);

    /* Configure chan 1 first (idle, waiting to be chained by chan 0) */
    dma_channel_configure(
        _dma_chan[1], &c1,
        _dma_buf[1],
        &_pio->rxf[_sm],
        SNIFFER_DMA_BUF_WORDS,
        false
    );

    /* Configure and start chan 0 */
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
        if (_dma_chan[0] >= 0) dma_channel_set_irq1_enabled(_dma_chan[0], false);
        if (_dma_chan[1] >= 0) dma_channel_set_irq1_enabled(_dma_chan[1], false);
        if (_dma_chan[0] >= 0) dma_channel_abort(_dma_chan[0]);
        if (_dma_chan[1] >= 0) dma_channel_abort(_dma_chan[1]);
        irq_remove_handler(DMA_IRQ_1, dmaIrqHandler);
        _irq_instance = nullptr;
        if (_dma_chan[0] >= 0) { dma_channel_unclaim(_dma_chan[0]); _dma_chan[0] = -1; }
        if (_dma_chan[1] >= 0) { dma_channel_unclaim(_dma_chan[1]); _dma_chan[1] = -1; }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PROCESSING LOOP
 * ═══════════════════════════════════════════════════════════════════════════ */

void __attribute__((optimize("O3"), section(".time_critical.usb_task")))
USBSnifferPIO::task() {
    if (!_running) return;
    if (_dma_rd == _dma_wr) return;

    const uint32_t rd_idx = _dma_rd;
    const uint32_t slot   = rd_idx & 1u;

    const uint32_t rxstall_mask = 1u << (PIO_FDEBUG_RXSTALL_LSB + _sm);
    const bool fifo_stalled = (_pio->fdebug & rxstall_mask) != 0;
    if (fifo_stalled) {
        _pio->fdebug = rxstall_mask;
    }

    if (_warmup_remaining > 0) {
        _warmup_remaining--;
        _ring_overruns = 0;
        if (_warmup_remaining == 0) {
            _decoder.reset();
        }
        _dma_rd = rd_idx + 1;
        return;
    }

    if (fifo_stalled) {
        _decoder.fifo_overruns++;
    }
    if (_ring_overruns > 0) {
        uint32_t ovf = _ring_overruns;
        _ring_overruns = 0;
        _decoder.fifo_overruns += ovf;
    }

    _decoder.feedSamples(_dma_buf[slot], SNIFFER_DMA_BUF_WORDS);
    _dma_rd = rd_idx + 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CALLBACK DELEGATION
 * ═══════════════════════════════════════════════════════════════════════════ */

void USBSnifferPIO::onPacket(USBPacketCallback callback) {
    _decoder.onPacket(callback);
}
