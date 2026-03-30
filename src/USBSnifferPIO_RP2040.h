/**
 * @file    USBSnifferPIO_RP2040.h
 * @brief   Top-level passive USB sniffer for RP2040.
 * @version 1.0.0
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
 * @brief Size of each DMA buffer in 32-bit words.
 *
 * 2048 words = 8 KB = 32 768 samples.
 * At 12 MHz (LS 8×) this covers ~2.73 ms — enough for multiple
 * USB packets per SOF frame.
 */
#define SNIFFER_DMA_BUF_WORDS  2048

/** @brief Number of ping-pong buffers. */
#define SNIFFER_DMA_BUF_COUNT  2

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

    /* ── Callback ─────────────────────────────────────────────── */

    /**
     * @brief Register the packet-ready callback.
     *
     * The callback fires inside @ref task() whenever the decoder
     * produces a complete, CRC-verified packet.
     *
     * @param[in] callback Function pointer, or @c nullptr to unregister.
     */
    void onPacket(USBPacketCallback callback);

    /* ── Accessors ────────────────────────────────────────────── */

    /**
     * @brief Access the internal decoder (e.g. for statistics).
     * @return Mutable reference to the USBPacketDecoder.
     */
    USBPacketDecoder&       decoder()       { return _decoder; }

    /**
     * @brief Const access to the internal decoder.
     * @return Const reference to the USBPacketDecoder.
     */
    const USBPacketDecoder& decoder() const { return _decoder; }

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
     * Only _dma_chan[0] is used (manual ping-pong).
     * _dma_chan[1] is kept for structural compatibility, fixed at -1.
     */
    int              _dma_chan[SNIFFER_DMA_BUF_COUNT];

    /** @brief Index of the buffer currently being filled by DMA (0 or 1). */
    int              _active_buf;

    /**
     * @brief Two capture buffers for manual ping-pong.
     *
     * While DMA fills _dma_buf[_active_buf], the decoder
     * processes _dma_buf[_active_buf ^ 1].
     */
    uint32_t         _dma_buf[SNIFFER_DMA_BUF_COUNT][SNIFFER_DMA_BUF_WORDS]
                         __attribute__((aligned(4)));

    /**
     * @brief Warm-up counter: discards the first N buffers.
     *
     * DMA starts capturing immediately in begin().  The first buffers
     * may contain a packet already in progress on the bus — the decoder
     * would enter mid-stream, accumulate bits without EOP, and produce
     * hundreds of spurious overflow_errors.
     *
     * Discarding 2 buffers (~5.5 ms at LS) and resetting the decoder
     * on the last one ensures a clean start.  The value counts down
     * to zero; once zero, buffers are processed normally.
     */
    uint8_t          _warmup_remaining;

    /* ── Decoder ──────────────────────────────────────────────── */
    USBPacketDecoder _decoder;

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
