# Changelog

All notable changes to `USBSnifferPIO_RP2040` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0] - 2026-11-24

### Added
- **Dual-Core Parallel Decoding (`enableDualCore(true)` + `taskCore0()`):** Core 1 decodes even DMA buffers (`0, 2`) while Core 0 decodes odd buffers (`1, 3`) with lossless boundary overlap, achieving **100.0% lossless Full-Speed (12 Mbps) capture even under ~10,000 pkt/s (64B) saturation at the official `200 MHz` clock (`-Os`, zero overclocking)**.
- **Dedicated Core 0 callback (`onPacketCore0()`):** Allows lock-free per-core packet processing in Dual-Core mode.
- **Aggregated statistics API (`stats()`, `resetStats()`, `USBSnifferStats`):** Single-call access to combined Core 1 + Core 0 counters, active `sys_clock_hz`, `pio_clock_div`, buffer sizes, and capture mode.
- **Configurable compile-time limits (`#ifndef` overrides):** `SNIFFER_DMA_BUF_WORDS` (default `2048`; `1024` Low-RAM mode cuts DMA RAM in half while maintaining 100% capture at 200 MHz Dual-Core), `SNIFFER_DMA_BUF_COUNT` (`4` or `2`), `SNIFFER_DUALCORE_TAIL_WORDS` (`48`), `SNIFFER_DMA_IRQ_INDEX` (`1` = `DMA_IRQ_1` default, `0` = `DMA_IRQ_0`), `USB_MAX_PACKET_BYTES` (`72`), `USB_MAX_PACKET_BITS` (`680`), and `USB_MIN_SYNC_BITS` (`4`).
- **Runtime warm-up configuration (`setWarmupBuffers()`):** Configure how many initial DMA buffers are discarded on `begin()`.
- **Multi-instance protection:** `setupDMA()` safely rejects a second simultaneous active `USBSnifferPIO` instance with clean rollback instead of overwriting the shared IRQ pointer.
- **New example (`examples/dual_core_sniffer/`):** Demonstrates Dual-Core 200 MHz Full-Speed capture with `sniffer.stats()`.
- **Repository hygiene & CI:** Added GitHub Actions build workflow compiling all examples and build-flag variants, `.editorconfig`, `.gitattributes`, `CODE_OF_CONDUCT.md`, `SECURITY.md`, and GitHub issue/PR templates.

### Fixed
- Eliminated `PIN_LED` macro redefinition warning in `examples/keyboard_logger/keyboard_logger.ino` on `arduino-pico` >= 5.x.

## [1.1.0] - 2026-11-24

### Added
- **Hardware 8× DPLL inside PIO** (22 instructions in `src/usb_sniffer.pio`): moves clock recovery into PIO hardware, reducing DMA/CPU bandwidth by **8×** (`2.73 ms` per 8 KB buffer in Full-Speed / `21.85 ms` in Low-Speed).
- **Zero-gap 2-channel hardware ping-pong DMA** (`ch0 ↔ ch1` chained with safe `DMA_IRQ_1` reload — 0 ns gap between buffers, no `WRITE_ADDR` race condition).
- **SRAM-resident `-O3` decoder** (`src/USBPacketDecoder.cpp`): inline NRZI + bit-unstuffing, sentinel-bit byte packer (`acc = 0x80`), and table-accelerated CRC5/CRC16/PID lookup.
- **New diagnostic counter:** `decoder().fifo_overruns` to expose PIO RX FIFO stalls and DMA ring overruns.
- **Automated hardware test bench (`tools/test_bench/`):** Pico 2 W calibrated Full-Speed packet generator, Wireshark (`usbmon`) validator, clock sweep script, and 8-hour continuous stress harness (validated for 8.01 h / 420 rounds / 15.32M packets).

### Fixed
- Fixed 100% packet drop on 64-byte worst-case bit-stuffing payloads (`64B 0xFF`, up to 625 raw bits) by expanding the packet buffer from `600` to `680` bits.

## [1.0.0] - 2026-03-29

### Added
- Initial public release of `USBSnifferPIO_RP2040`: passive Low-Speed and Full-Speed USB 1.1 packet sniffer using RP2040 PIO, manual DMA ping-pong, full NRZI/unstuffing/CRC5/CRC16 decoder, callback API, and 6 example sketches.
