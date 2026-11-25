/**
 * @file    USBSnifferPIO_RP2040.h
 * @brief   Top-level passive USB sniffer for RP2040.
 * @version 1.2.1
 * @license MIT
 *
 * @details
 *   Integrates three hardware/software subsystems into a single,
 *   easy-to-use class:
 *
 *   | Layer       | Role                                                   |
 *   |-------------|--------------------------------------------------------|
 *   | **PIO**     | Deterministic line sampling at 8× oversampling         |
 *   | **DMA**     | Zero-copy ping-pong transfer from PIO FIFO to RAM      |
 *   | **Decoder** | NRZI / unstuffing / framing / CRC → USBPacket          |
 *
 * @section arch Architecture
 *
 *   @verbatim
 *    D+ -+  +----------+  DMA Chan 0  +----------------+
 *    D- -+--+  PIO SM   +------------>+  Double Buffer  +--> USBPacketDecoder
 *            +----------+  ping-pong  +----------------+         |
 *                                                         callback(USBPacket)
 *   @endverbatim
 *
 * @section usage Quick Start
 *
 *   @code
 *     #include <USBSnifferPIO_RP2040.h>
 *
 *     USBSnifferPIO sniffer;
 *
 *     void onPacket(const USBPacket& pkt) { ... }
 *
 *     void setup()  { Serial.begin(115200); }
 *     void setup1() { sniffer.begin(2); sniffer.onPacket(onPacket); }
 *     void loop()   { }
 *     void loop1()  { sniffer.task(); }
 *   @endcode
 *
 * @section hw Hardware
 *
 *   @verbatim
 *     USB Cable             Pico
 *     ────────────          ──────────
 *     D+  --[100R]----------> GPx       (pin_dp)
 *     D-  --[100R]----------> GPx+1     (pin_dp + 1)
 *     GND ------------------> GND
 *     VBUS                  (do NOT connect)
 *   @endverbatim
 *
 * @warning Series resistors (100 Ω) are mandatory to avoid
 *          loading the USB bus differential impedance.
 *
 * @author Ângelo Moisés Alves
 */

#ifndef USB_SNIFFER_PIO_RP2040_H
#define USB_SNIFFER_PIO_RP2040_H

#include <Arduino.h>

/*
 * When the Arduino-Pico core is configured with the "Adafruit TinyUSB"
 * USB stack, Serial becomes an Adafruit_USBD_CDC object whose symbols
 * live inside the TinyUSB library.  The Arduino build system only
 * compiles and links libraries whose headers are #included somewhere
 * in the translation unit.  Without this include the linker fails with
 *   undefined reference to `Adafruit_USBD_CDC::begin(unsigned long)'
 *   undefined reference to `Serial'
 *
 * The guard ensures zero impact when TinyUSB is not selected.
 */
#ifdef USE_TINYUSB
#include <Adafruit_TinyUSB.h>
#endif

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "USBProtocol.h"
#include "USBPacketDecoder.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  DMA CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Size of each DMA buffer in 32-bit words (override before #include if needed).
 *
 * 2048 words = 8 KB = 32 768 clock-recovered USB bits (1x PIO DPLL).
 *   - Full-Speed (12 Mbps): ~2.73 ms per buffer
 *   - Low-Speed  (1.5 Mbps): ~21.85 ms per buffer
 * Smaller values (e.g. 1024) save RAM at the cost of higher IRQ rate.
 */
#ifndef SNIFFER_DMA_BUF_WORDS
#define SNIFFER_DMA_BUF_WORDS  2048
#endif

/** @brief Number of DMA ring buffers (must be 2 or 4; 4 required for dual-core). */
#ifndef SNIFFER_DMA_BUF_COUNT
#define SNIFFER_DMA_BUF_COUNT  4
#endif
#define SNIFFER_DMA_BUF_MASK   (SNIFFER_DMA_BUF_COUNT - 1)

/**
 * @brief Words read from the end of the previous buffer and from the start of the next one
 *        to resolve packets that cross a buffer boundary in dual-core mode.
 */
#ifndef SNIFFER_DUALCORE_TAIL_WORDS
#define SNIFFER_DUALCORE_TAIL_WORDS  48
#endif
static_assert(SNIFFER_DUALCORE_TAIL_WORDS * 16 >= USB_MAX_PACKET_BITS + 16,
              "SNIFFER_DUALCORE_TAIL_WORDS must cover the longest packet");
static_assert(SNIFFER_DUALCORE_TAIL_WORDS < SNIFFER_DMA_BUF_WORDS,
              "SNIFFER_DUALCORE_TAIL_WORDS must be smaller than SNIFFER_DMA_BUF_WORDS");

/**
 * @brief DMA IRQ line used for ping-pong reload (0 = DMA_IRQ_0, 1 = DMA_IRQ_1).
 *
 * Defaults to DMA_IRQ_1 with a shared handler so it coexists with libraries
 * using DMA_IRQ_0 or shared DMA_IRQ_1. Override to 0 if another library claims
 * DMA_IRQ_1 exclusively.
 */
#ifndef SNIFFER_DMA_IRQ_INDEX
#define SNIFFER_DMA_IRQ_INDEX  1
#endif
#if (SNIFFER_DMA_IRQ_INDEX != 0) && (SNIFFER_DMA_IRQ_INDEX != 1)
#error "SNIFFER_DMA_IRQ_INDEX must be 0 (DMA_IRQ_0) or 1 (DMA_IRQ_1)"
#endif

/**
 * @brief Aggregated runtime statistics and active configuration snapshot.
 */
struct USBSnifferStats {
    uint32_t packets_decoded;  ///< Combined packets decoded (Core 1 + Core 0)
    uint32_t crc_errors;       ///< Combined CRC5/CRC16 errors
    uint32_t sync_errors;      ///< Combined SYNC errors
    uint32_t stuffing_errors;  ///< Combined bit-stuffing errors
    uint32_t overflow_errors;  ///< Combined packet overflow errors
    uint32_t fifo_overruns;    ///< PIO RX FIFO stall or DMA ring overrun events
    uint32_t sys_clock_hz;     ///< Detected RP2040 clk_sys frequency (Hz)
    float    pio_clock_div;    ///< Active PIO clock divider
    uint16_t dma_buf_words;    ///< Configured words per DMA buffer
    uint8_t  dma_buf_count;    ///< Configured DMA ring buffer count
    bool     dual_core;        ///< Whether dual-core striped decoding is active
    bool     running;          ///< Whether capture is active
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  CLASS USBSnifferPIO
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @class USBSnifferPIO
 * @brief High-level passive USB packet capture engine.
 *
 * Manages PIO, DMA, and the software decoder.  Call @ref begin()
 * once, then @ref task() in a tight loop.  Decoded packets are
 * delivered through a user-registered callback.
 *
 * @par Resources
 *   - 1 PIO state machine
 *   - 1 DMA channel (manual ping-pong, see setupDMA())
 *   - ~17 KB RAM (two 8 KB buffers + decoder state)
 */
class USBSnifferPIO {
public:

    /* ── Construction / destruction ───────────────────────────── */

    USBSnifferPIO();
    ~USBSnifferPIO();

    /* ── Initialization ───────────────────────────────────────── */

    /**
     * @brief Start capturing USB traffic.
     *
     * Allocates PIO, DMA, and decoder resources.
     * Capture begins immediately upon successful return.
     *
     * @param[in] pin_dp       GPIO number of D+ (D- must be pin_dp + 1).
     * @param[in] speed        Bus speed to capture.
     * @param[in] pio_instance PIO block index (0 = pio0, 1 = pio1).
     * @return @c true on success, @c false if resources unavailable.
     */
    bool begin(
        uint     pin_dp,
        USBSpeed speed        = USBSpeed::LOW_SPEED,
        uint     pio_instance = 1
    );

    /**
     * @brief Stop capture and release all hardware resources.
     *
     * Safe to call even if @ref begin() was never called or already
     * returned @c false.
     */
    void end();

    /* ── Processing loop ──────────────────────────────────────── */

    /**
     * @brief Process pending DMA buffers.
     *
     * Checks whether the active DMA transfer has completed, re-arms
     * the channel for the next buffer immediately, then feeds the
     * completed buffer into the decoder.
     *
     * The first completed buffer is discarded and the decoder is reset
     * to avoid overflow_errors caused by partial packets captured at
     * startup (warm-up cycle).
     *
     * @b Must be called frequently to avoid buffer overruns.
     * Maximum safe inter-call latency:
     *   - Low-Speed:  ~2.7 ms
     *   - Full-Speed: ~0.34 ms
     *
     * @note Callbacks registered via @ref onPacket() fire
     *       @b inside this function.
     */
    void task();

    /**
     * @brief Process odd-numbered DMA buffers on Core 0 (dual-core mode).
     *
     * Enable via @ref enableDualCore(true), then call @ref task() on Core 1
     * and @ref taskCore0() on Core 0 for 2x parallel Full-Speed decoding.
     */
    void taskCore0();

    /**
     * @brief Enable or disable dual-core striped parallel decoding.
     *
     * When enabled, Core 1 decodes even buffers (0, 2) and Core 0 decodes
     * odd buffers (1, 3). A packet is decoded by the core of the buffer where
     * its SYNC starts; each core also reads the end of the previous buffer and
     * the start of the next one to handle packets that cross a boundary.
     */
    void enableDualCore(bool enable = true);
    bool isDualCore() const { return _dual_core; }

    /* ── Callback ─────────────────────────────────────────────── */

    void onPacket(USBPacketCallback callback);

    /**
     * @brief Optional dedicated callback for Core 0 in dual-core mode
     *        (avoids cross-core contention; defaults to onPacket() callback).
     */
    void onPacketCore0(USBPacketCallback callback);

    /* ── Accessors ────────────────────────────────────────────── */

    USBPacketDecoder&       decoder()            { return _decoder; }
    const USBPacketDecoder& decoder() const      { return _decoder; }
    USBPacketDecoder&       decoderCore0()       { return _decoder_c0; }
    const USBPacketDecoder& decoderCore0() const { return _decoder_c0; }

    /**
     * @brief Return combined statistics (Core 1 + Core 0) and active config.
     */
    USBSnifferStats stats() const;

    /**
     * @brief Reset decoder statistics on both Core 1 and Core 0.
     */
    void resetStats();

    /**
     * @brief Configure how many initial DMA buffers are discarded on begin() (default: 2).
     */
    void setWarmupBuffers(uint8_t count) { _warmup_config = count; }

    /**
     * @brief Check whether capture is active.
     * @return @c true if @ref begin() succeeded and @ref end()
     *         has not been called.
     */
    bool isRunning() const { return _running; }

private:

    /* ── State ────────────────────────────────────────────────── */
    bool             _running;
    USBSpeed         _speed;

    /* ── PIO ──────────────────────────────────────────────────── */
    PIO              _pio;
    uint             _sm;
    uint             _pio_offset;

    /* ── DMA ──────────────────────────────────────────────────── */

    /**
     * @brief DMA channel handles.
     *
     * Only _dma_chan[0] is used (IRQ-driven 4-buffer ring).
     */
    int              _dma_chan[2];

    /** @brief Index of the buffer currently being filled by DMA (0..COUNT-1). */
    volatile uint32_t _dma_wr;

    /** @brief Index of the next completed buffer for Core 1 task() to decode. */
    volatile uint32_t _dma_rd;

    /** @brief Index of the next completed odd buffer for Core 0 taskCore0(). */
    volatile uint32_t _dma_rd_c0;

    /** @brief Next slot toggle for ch0 (0/2) and ch1 (1/3). */
    volatile uint8_t  _ch0_next_slot;
    volatile uint8_t  _ch1_next_slot;

    /** @brief Dual-core parallel decode flag. */
    volatile bool     _dual_core;

    /** @brief Count of ring-buffer overwrites when consumer falls behind DMA. */
    volatile uint32_t _ring_overruns;

    /**
     * @brief Capture ring buffers filled by 2-channel hardware ping-pong DMA.
     */
    uint32_t         _dma_buf[SNIFFER_DMA_BUF_COUNT][SNIFFER_DMA_BUF_WORDS]
                         __attribute__((aligned(4)));

    /** @brief Active singleton pointer for DMA_IRQ_1 handler. */
    static USBSnifferPIO* _irq_instance;

    /** @brief SRAM-resident DMA_IRQ_1 handler for zero-gap 4-slot rotation. */
    static void dmaIrqHandler();

    uint8_t          _warmup_remaining;
    uint8_t          _warmup_config;

    /* ── Decoders (Core 1 + optional Core 0) ──────────────────── */
    USBPacketDecoder _decoder;
    USBPacketDecoder _decoder_c0;
    bool             _has_c0_cb;

    /* ── Internal helpers ─────────────────────────────────────── */

    /**
     * @brief Configure a single DMA channel for manual ping-pong.
     *
     * Uses a single channel with no chain_to (chaining disabled).
     * The restart between buffers is done explicitly in task(),
     * avoiding the WRITE_ADDR race condition that occurs with chaining.
     *
     * @return @c true on success.
     */
    bool setupDMA();

    /**
     * @brief Abort and release DMA channels.
     */
    void teardownDMA();

    /**
     * @brief Compute the PIO clock divider for the active speed.
     *
     * @return Floating-point divider for sm_config_set_clkdiv().
     */
    float calcClockDiv() const;
};

#endif /* USB_SNIFFER_PIO_RP2040_H */
