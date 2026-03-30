/**
 * @file    USBSnifferPIO_RP2040.cpp
 * @brief   Implementation of the top-level USBSnifferPIO class.
 * @version 1.0.0
 * @license MIT
 *
 * @details
 *   Responsibilities:
 *   - Allocate and configure one PIO state machine
 *   - Set up one DMA channel with manual sequential ping-pong
 *   - Drive the processing loop that feeds completed buffers
 *     into the USBPacketDecoder
 *
 * @see USBSnifferPIO_RP2040.h for the public API.
 *
 * @author Ângelo Moisés Alves
 */

#include "USBSnifferPIO_RP2040.h"
#include "usb_sniffer.pio.h"
#include "hardware/clocks.h"   /* clock_get_hz() — dynamic clock detection */
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  CONSTRUCTION / DESTRUCTION
 * ═══════════════════════════════════════════════════════════════════════════ */

USBSnifferPIO::USBSnifferPIO()
    : _running(false)
    , _speed(USBSpeed::LOW_SPEED)
    , _pio(nullptr)
    , _sm(0)
    , _pio_offset(0)
    , _active_buf(0)
    , _warmup_remaining(2)     /* discard first 2 buffers before decoding */
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
 *  INITIALIZATION
 * ═══════════════════════════════════════════════════════════════════════════ */

bool USBSnifferPIO::begin(uint pin_dp, USBSpeed speed, uint pio_instance) {
    if (_running) end();

    _speed            = speed;
    _active_buf       = 0;
    _warmup_remaining = 2;        /* discard first 2 buffers at each begin() */
    _decoder          = USBPacketDecoder(speed);

    /* Select PIO instance */
    _pio = (pio_instance == 0) ? pio0 : pio1;

    /* Claim a free state machine */
    int sm = pio_claim_unused_sm(_pio, false);
    if (sm < 0) return false;
    _sm = (uint)sm;

    /* Load the PIO program */
    if (!pio_can_add_program(_pio, &usb_sniffer_program)) {
        pio_sm_unclaim(_pio, _sm);
        return false;
    }
    _pio_offset = pio_add_program(_pio, &usb_sniffer_program);

    /* Initialize the state machine */
    usb_sniffer_program_init(_pio, _sm, _pio_offset, pin_dp, calcClockDiv());

    /* Configure DMA ping-pong */
    if (!setupDMA()) {
        pio_sm_set_enabled(_pio, _sm, false);
        pio_remove_program(_pio, &usb_sniffer_program, _pio_offset);
        pio_sm_unclaim(_pio, _sm);
        return false;
    }

    _running = true;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SHUTDOWN
 * ═══════════════════════════════════════════════════════════════════════════ */

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
    /*
     * Detect system clock at runtime to support any frequency
     * without requiring set_sys_clock_khz() in the user sketch.
     *
     * Required PIO rate = bit_rate × oversampling factor (8×):
     *   Low-Speed  : 1.5 MHz × 8 = 12 MHz
     *   Full-Speed : 12  MHz × 8 = 96 MHz
     *
     * Divider examples for common clocks:
     *   125 MHz (default Arduino RP2040) → LS: 10.417   FS: 1.302
     *   120 MHz                          → LS: 10.0     FS: 1.25
     *
     * The PIO divider has 8.8 resolution (1/256), sufficient for both cases.
     */
    const float sys_hz = static_cast<float>(clock_get_hz(clk_sys));
    const float pio_hz = (_speed == USBSpeed::FULL_SPEED) ? 96.0e6f : 12.0e6f;

    return sys_hz / pio_hz;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  DMA PING-PONG SETUP
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Configure a single DMA channel for continuous capture with manual
 *        ping-pong.
 *
 * @section design Design decision — why NOT use chain_to
 *
 * The intuitive approach of chaining two channels (0→1→0→1...) has a
 * fatal flaw on the RP2040: the hardware fires the chained channel
 * IMMEDIATELY when TRANS_COUNT reaches zero, before any software handler
 * can run.  When this happens, the WRITE_ADDR of the just-completed
 * channel has already auto-incremented BEYOND the buffer (original +
 * N×4 bytes).  The chained channel then re-triggers the previous one
 * with that wrong address, writing 8 KB past the allocated buffer —
 * corrupting TinyUSB variables, stack and heap.  Result: USB serial
 * dies moments after capture begins.
 *
 * @section solution Solution — manual restart in task()
 *
 * A single DMA channel alternates between two buffers.  When task()
 * detects the end of a transfer via dma_channel_is_busy(), it:
 *   1. Restarts the channel to the OTHER buffer immediately (minimizes gap).
 *   2. Processes the buffer that was just filled, safely.
 *
 * The gap between the end of one transfer and the start of the next is
 * on the order of tens of nanoseconds (a few ARM instructions).  The
 * PIO FIFO has a depth of 8 words, equivalent to ~10 µs of margin at
 * Low-Speed — more than sufficient.
 */
bool USBSnifferPIO::setupDMA() {

    /* Use only one DMA channel — the second slot stays inactive */
    _dma_chan[0] = dma_claim_unused_channel(false);
    if (_dma_chan[0] < 0) return false;
    _dma_chan[1] = -1;   /* not used — kept for struct compatibility */

    /* Configure the channel: PIO RX FIFO → buf[_active_buf], no chain_to */
    dma_channel_config c = dma_channel_get_default_config(_dma_chan[0]);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c,  false);   /* always read from the same FIFO register */
    channel_config_set_write_increment(&c, true);    /* advance through the destination buffer   */
    channel_config_set_dreq(&c, pio_get_dreq(_pio, _sm, false));
    /* chain_to = self → chaining disabled */
    channel_config_set_chain_to(&c, _dma_chan[0]);

    dma_channel_configure(
        _dma_chan[0], &c,
        _dma_buf[0],             /* initial write address (buf 0) */
        &_pio->rxf[_sm],         /* read address — PIO FIFO       */
        SNIFFER_DMA_BUF_WORDS,   /* number of transfers            */
        true                     /* start immediately              */
    );

    return true;
}

void USBSnifferPIO::teardownDMA() {
    /* Only _dma_chan[0] is used; _dma_chan[1] remains -1 */
    if (_dma_chan[0] >= 0) {
        dma_channel_abort(_dma_chan[0]);
        dma_channel_unclaim(_dma_chan[0]);
        _dma_chan[0] = -1;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PROCESSING LOOP
 * ═══════════════════════════════════════════════════════════════════════════ */

void USBSnifferPIO::task() {
    if (!_running) return;

    /* Check if the channel is still busy transferring */
    if (dma_channel_is_busy(_dma_chan[0])) return;

    /*
     * The channel completed the transfer to buf[_active_buf].
     * Determine which buffer just finished and which comes next.
     */
    const int completed = _active_buf;
    const int next      = completed ^ 1;   /* toggle between 0 and 1 */

    /*
     * Restart the channel to the NEXT buffer BEFORE any processing.
     * This minimizes the capture gap: DMA resumes in microseconds,
     * while feedSamples() may take several milliseconds.
     *
     * Mandatory order:
     *   1. set_write_addr  — point to the correct buffer (resets the
     *      pointer that was auto-incremented during the previous transfer).
     *   2. set_trans_count — reload the count.
     *   3. start           — trigger the new transfer.
     */
    dma_channel_set_write_addr(_dma_chan[0], _dma_buf[next], false);
    dma_channel_set_trans_count(_dma_chan[0], SNIFFER_DMA_BUF_WORDS, false);
    dma_channel_start(_dma_chan[0]);
    _active_buf = next;

    /*
     * Warm-up: discard the first N buffers to avoid startup artifacts.
     *
     * The DMA starts capturing immediately in begin().  The first
     * buffers may contain a USB packet already in progress — the
     * decoder would enter mid-stream and accumulate hundreds of
     * spurious overflow_errors.
     *
     * By discarding 2 buffers (~5.5 ms at Low-Speed) and resetting
     * the decoder on the last one, the capture starts from a clean
     * idle period with zeroed counters.
     */
    if (_warmup_remaining > 0) {
        _warmup_remaining--;
        if (_warmup_remaining == 0) {
            _decoder.reset();   /* final reset: zeroes FSM + all counters */
        }
        return;
    }

    /* Process the buffer that was just filled (DMA is already writing to the other) */
    _decoder.feedSamples(_dma_buf[completed], SNIFFER_DMA_BUF_WORDS);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CALLBACK DELEGATION
 * ═══════════════════════════════════════════════════════════════════════════ */

void USBSnifferPIO::onPacket(USBPacketCallback callback) {
    _decoder.onPacket(callback);
}
