# Changelog

All notable changes to `USBSnifferPIO_RP2040` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed
- `src/usb_sniffer.pio.h` is now pioasm output generated from `src/usb_sniffer.pio` (pioasm 2.3.0, the version installed with arduino-pico 6.3.1), instead of a hand-written copy. The state-machine initializer moved into the `.pio` file's `c-sdk` block.
  - The PIO program is the same 22 instructions.
  - On 2026-11-25 all 13 firmware images that the release checks build (examples, build variants and bench sketches) were byte-identical to v1.2.1 with arduino-pico 6.3.1.
  - The build no longer warns that `pio_program::pio_version` (and, on RP2350, `used_gpio_ranges`) has no initializer.

### Fixed
- The README listed arduino-pico 3.x as supported, but v1.2.1 does not compile on 3.9.5: its pico-sdk 1.5.1 has no `gpio_function_t`. The requirement now reads 4.0 or later. On 2026-11-25 the library compiled on 4.0.0 (pico-sdk 2.0.0) and 6.3.1.

## [1.2.1] - 2026-11-25

### Fixed
- **Dual-core decoding dropped packets at DMA buffer boundaries.** `feedSamplesStriped()` now gives each packet to the buffer where its SYNC starts. Each core re-reads the last `SNIFFER_DUALCORE_TAIL_WORDS` words of the previous buffer from the first symbol after which every decoder state converges (an SE0, or the 10th J in a row), so at the boundary it is in the state the previous buffer's core reaches, and it finishes its own last packet in the next buffer.
  - Hardware bench (2026-11-25, 200 MHz `-Os`, Pico 2 W generator, Wireshark as reference): v1.2.0 lost 41 of 182,602 packets with 2048-word buffers (5 rounds) and 54 of 109,673 with 1024-word buffers (3 rounds). v1.2.1 lost none of 183,913 and of 182,730 (5 rounds each), with no CRC, SYNC, stuffing or overflow error. Single-core capture was the same in both versions. Those v1.2.1 rounds ran the code from before the next three fixes in this list, which do not change how valid traffic is decoded; a confirmation run with the released sources (3 rounds with 2048 words, 2 with 1024) also lost none of 109,640 and 73,005 packets.
  - Host test (30,000 polling transactions per case): v1.2.0 lost 34 of 76,299 packets (Full-Speed, 2048 words), 82 of 76,497 (1024 words) and 25 of 74,994 (Low-Speed); v1.2.1 returns all of them, and single-core output is unchanged.
- The resynchronization point is the 10th J in a row, not the 8th: after a bit-stuffing error the decoder is still waiting for the inter-packet gap, so with a corrupted stream the 8th J could start the next SYNC two symbols early.
- `feedSamplesStriped()` could read past `next_buffer` when `margin_words` was larger than `word_count`. Only a direct call to the decoder could do that; it was also in v1.2.0.
- `reset()` no longer touches the dual-core ownership flags, so a reset from the other core cannot turn emission back on while a buffer is being re-read.
- Removed a shift by 32 bits (undefined behaviour) from the old start-of-buffer scan.

### Added
- **Host-side decoder test (`extras/host_test/`)**, run by CI with AddressSanitizer and UBSan. It synthesizes Full-Speed and Low-Speed traffic, compares single-core and dual-core decoding packet by packet, slides a transaction across a buffer boundary one symbol at a time, checks corrupted packets next to a boundary, and keeps single-core output on a corrupted stream identical to v1.2.0.

### Changed
- `USBPacketDecoder::feedSamplesStriped()` takes an optional fifth argument, `prev_buffer`; the four-argument form still compiles. `SNIFFER_DUALCORE_TAIL_WORDS` now also sets how much of the previous buffer is re-read, and a `static_assert` checks that it covers the longest packet (at least 44 words).
- The library takes about 3 KB less RAM and flash, because the v1.2.0 `feedSamplesStriped()` alone was 3,474 bytes of SRAM-resident code. A minimal dual-core sketch minus an empty one (2048-word buffers, arduino-pico 6.3.1, 2026-11-25): RAM 38,732 → 35,748 bytes, flash image 8,896 → 5,912 bytes.
- In dual-core mode, error counters are exact for valid traffic. With a corrupted stream, an error in a packet that crosses a buffer boundary can be counted by both cores, and after a stretch with no resynchronization point (no SE0, no run of 10 J) packets and errors up to the next one can be missed; the README says so.
- README: dual-core figures re-measured on 2026-11-25 against Wireshark. The v1.2.0 rows had been computed against the count the host program reads, which at the maximum rate misses about 6 packets per round. RAM corrected from ~28 KB to ~35 KB.

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
