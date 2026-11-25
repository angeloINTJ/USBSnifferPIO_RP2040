# USBSnifferPIO_RP2040

> Passive USB 1.1 packet sniffer for RP2040 — built on a hardware PIO DPLL, zero-gap 2-channel DMA ping-pong, and Dual-Core parallel decoding.

[![License: MIT](https://img.shields.io/badge/License-MIT-1f6355?style=flat-square&labelColor=5f5b54)](LICENSE)
[![Platform: RP2040](https://img.shields.io/badge/Platform-RP2040-1f6355?style=flat-square&labelColor=5f5b54)](https://www.raspberrypi.com/products/raspberry-pi-pico/)
[![Framework: Arduino](https://img.shields.io/badge/Framework-Arduino-1f6355?style=flat-square&labelColor=5f5b54)](https://arduino-pico.readthedocs.io/)
[![CI](https://img.shields.io/github/actions/workflow/status/angeloINTJ/USBSnifferPIO_RP2040/build.yml?branch=main&label=CI&style=flat-square&labelColor=5f5b54)](https://github.com/angeloINTJ/USBSnifferPIO_RP2040/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/angeloINTJ/USBSnifferPIO_RP2040?label=Release&color=1f6355&style=flat-square&labelColor=5f5b54)](https://github.com/angeloINTJ/USBSnifferPIO_RP2040/releases/latest)
[![Contributions welcome](https://img.shields.io/badge/Contributions-welcome-1f6355?style=flat-square&labelColor=5f5b54)](CONTRIBUTING.md)

Captures Low-Speed (1.5 Mbps) and Full-Speed (12 Mbps) USB traffic without interfering with the bus.  
The host and device never know the sniffer exists.

> **⚠️ Legal Disclaimer**
>
> This project is provided **strictly for educational purposes, authorized security research, and legitimate hardware auditing**. It is intended to help security professionals, researchers, and enthusiasts understand USB HID protocol internals, PIO-based signal capture, and embedded cryptography.
>
> **You are solely responsible for ensuring that your use of this software complies with all applicable laws in your jurisdiction.** Intercepting, recording, or monitoring keyboard input from devices you do not own — or without the explicit, informed consent of the device owner — is **illegal** in most jurisdictions and may violate statutes including, but not limited to:
>
> * **Brazil** — Lei 12.737/2012, Art. 154-A (invasão de dispositivo informático)
> * **United States** — Computer Fraud and Abuse Act (18 U.S.C. § 1030), Wiretap Act (18 U.S.C. § 2511)
> * **European Union** — GDPR (Art. 5, 6), national implementations of the Computer Misuse Directive
> * **United Kingdom** — Computer Misuse Act 1990
>
> The author and contributors **do not condone, encourage, or support** the use of this tool for unauthorized surveillance, data theft, or any form of illegal activity. By using, downloading, or distributing this software, you acknowledge that:
>
> 1. You will only use it on devices you own or have written authorization to test
> 2. You assume all legal liability arising from your use
> 3. The MIT License governs copyright and redistribution only — it does not grant permission to violate any law
>
> **If you are unsure whether your intended use is lawful, consult a qualified legal professional before proceeding.**

## Features

| Feature | Details |
|---------|---------|
| **100% passive** | High-impedance tap — zero electrical interference |
| **Low-Speed + Full-Speed** | 1.5 Mbps and 12 Mbps USB 1.1 |
| **PIO hardware DPLL** | 22-instruction 8× edge-resynchronized clock recovery inside PIO (8× lower DMA/CPU load) |
| **Zero-gap DMA ping-pong** | 2 chained DMA channels (`ch0 ↔ ch1`) + SRAM `DMA_IRQ_1` reload — 0 ns gap between buffers |
| **SRAM `-O3` decoder** | Inline NRZI + bit-unstuffing + sentinel-bit byte packer + table-accelerated CRC5/CRC16 |
| **Full protocol decode** | Handles worst-case 64-byte `0xFF` bit-stuffing (680-bit buffer) + `fifo_overruns` tracking |
| **Callback API** | Receive structured `USBPacket` with PID, address, endpoint, payload |
| **Dual-core ready** | Capture on Core 1, process/display on Core 0 |
| **Compact resources** | 1 PIO state machine (22 instructions), 2 DMA channels, ~35 KB RAM (~19 KB in Low-RAM mode) |

## How it works

```
┌──────────────┐     ┌──────────────────────┐     ┌────────────────────┐
│  PIO SM      │────►│ 2-Ch HW Ping-Pong DMA│────►│ USBPacketDecoder   │
│  8× HW DPLL  │     │ 4 × 8 KB ring buffer │     │ (SRAM + -O3)       │
│  22 instr.   │     │ 0 ns gap + DMA_IRQ_1 │     │                    │
│  D+ ─┐ (JMP) │     └──────────────────────┘     │ SYNC + Inline NRZI │
│  D- ─┘       │                                  │ Bit Unstuffing     │
└──────────────┘                                  │ Table CRC5 / CRC16 │
                                                  └─────────┬──────────┘
                                                            │
                                                    callback(USBPacket)
```

1. **PIO hardware DPLL** (22 instructions) runs at 8× the USB bit rate (12 MHz for LS, 96 MHz for FS), tracks edges on `D+` (`JMP_PIN`), and samples `[D+, D-]` at exact bit center (`t = 4/8`). 16 clock-recovered USB bits are packed into each 32-bit word and autopushed to the 8-deep RX FIFO — cutting DMA and CPU bandwidth by **8×** compared to raw oversampling.
2. **Zero-gap 2-channel hardware ping-pong DMA** chains two channels (`ch0 ↔ ch1`) across a ring of four 8 KB buffers (`ch0` fills buffers 0 and 2, `ch1` fills 1 and 3). When one channel finishes, hardware switches immediately (0 ns gap) while `DMA_IRQ_1` safely resets the inactive channel's `WRITE_ADDR` and `TRANS_COUNT` with a full `2.73 ms` (FS) / `21.85 ms` (LS) margin — eliminating both inter-buffer stalls and the `WRITE_ADDR` race condition.
3. **SRAM-resident `-O3` decoder** runs directly from `.time_critical` RAM (even when the IDE is set to `-Os`), skipping all-J idle words in O(1), performing inline NRZI decoding, bit-unstuffing, sentinel-bit byte packing, PID lookup, and table-accelerated CRC5/CRC16 verification.

## Performance & Benchmarks (Full-Speed 12 Mbps)

Validated on a real hardware test bench comparing **Raspberry Pi Pico 2 W** (calibrated Full-Speed USB CDC generator) against **Wireshark (`usbmon`)** as ground truth (0% loss) and **Raspberry Pi Pico (RP2040)** running `USBSnifferPIO_RP2040`:

| Version / Mode / Clock | 10 pkt/s (16B) | 100–1,000 pkt/s (16B) | 5,000 pkt/s (16B) | 1,000 pkt/s (64B `0xFF`) | 1,000 pkt/s (64B `0x00`) | MAX (~9,860–10k/s, 64B) | CRC / FIFO (`P8_MAX`) |
|------------------------|---------------:|----------------------:|------------------:|-------------------------:|-------------------------:|------------------------:|----------------------:|
| `v1.0.0` Single-Core (`200 MHz -Os`) | 1.0% | 1.6–1.9% | 1.9% | 0.0% *(overflow)* | 1.3% | 1.6% | 26 / — |
| `v1.1.0` Single-Core (`200 MHz` official) | **100.0%** | 79.5–80.8% | 76.9% | 79.4% | 78.0% | 64.0% | 3 / 1369 |
| `v1.1.0` Single-Core (`250 MHz -O3`) | **100.0%** | **100.0%** | 97.1% | 91.8% | 98.5% | 80.0% | 3 / 1183 |
| `v1.1.0` Single-Core (`276 MHz -O3`, 8h) | **100.00%** | **100.00%** | **100.00%** | **100.00%** | **100.00%** | 88.43% | 3 / 1181 |
| `v1.2.0` **Dual-Core** (`120 MHz -Os`, zero OC) | 98.0% | 94.6–96.0% | 89.5% | 91.6% | 93.3% | 69.6% | 13 / 1627 |
| `v1.2.0` **Dual-Core** (`133 MHz -Os`, zero OC) | **100.0%** | 99.9–100.0% | **100.0%** | **100.0%** | 99.9% | 77.1% | 8 / 1424 |
| `v1.2.0` **Dual-Core** (`150 MHz -Os`, zero OC) | **100.0%** | 99.9–100.0% | 99.7% | **100.0%** | **100.0%** | 87.2% | 9 / 1454 |
| `v1.2.0` **Dual-Core** (`176 MHz -Os`, zero OC) | **100.0%** | **100.0%** | **100.0%** | 99.9% | **100.0%** | **100.0%** | **0 / 90** |
| `v1.2.0` **Dual-Core** (`200 MHz -Os`, 2026-11-25, 5 rounds) | **100.0%** | 99.96–99.98% | 99.98% | 99.98% | 99.96% | 99.978% *(152,069/152,102)* | 0 / 0 |
| `v1.2.1` **Dual-Core** (`200 MHz -Os`, 2026-11-25, 5 rounds) | **100.0%** | **100.0%** | **100.0%** | **100.0%** | **100.0%** | **100.0%** *(153,413/153,413)* | **0 / 0** |
| `v1.2.0` **Dual-Core Low-RAM** (`200 MHz`, `1024w`, 2026-11-25, 3 rounds) | **100.0%** | 99.87–100.0% | 99.90% | 99.93% | **100.0%** | 99.952% *(91,329/91,373)* | 0 / 2 |
| `v1.2.1` **Dual-Core Low-RAM** (`200 MHz`, `1024w`, 2026-11-25, 5 rounds) | **100.0%** | **100.0%** | **100.0%** | **100.0%** | **100.0%** | **100.0%** *(152,230/152,230)* | 0 / 4 |
| `v1.2.1` Single-Core (`200 MHz -Os`, 2026-11-25, 3 rounds) | 79.7% | 78.8–79.6% | 75.0% | 76.6% | 77.3% | 59.2% | 3 / 4107 |

Rows dated 2026-11-25 are an A/B run of `v1.2.0` against `v1.2.1` on the same bench and compare the sniffer with Wireshark. Earlier rows compare it with the count the host program reads, which in the MAX profile misses about 6 packets per round that Wireshark sees, so their MAX figures read slightly high. In the `1024w` rows, the few FIFO overruns happen while the bench firmware resets its counters between profiles, in both versions, and no packet was lost to them.

> **Dual-core mode at the default `200 MHz` clock (`v1.2.1`):**
> Enable `sniffer.enableDualCore(true)` on Core 1 and call `sniffer.taskCore0()` in Core 0's `loop()`. Core 1 decodes even DMA buffers (`0, 2`) and Core 0 decodes odd ones (`1, 3`). Each packet belongs to the buffer where its SYNC starts, and each core re-reads the end of the previous buffer to find where that is. On 2026-11-25 (5 rounds per buffer size, against Wireshark), no packet was lost and no CRC, SYNC, stuffing or overflow error was counted, up to ~9,970 pkt/s of 64-byte packets with 2048-word buffers and ~9,900 pkt/s with 1024-word buffers. On the same bench, `v1.2.0` lost 41 of 182,602 packets with 2048 words and 54 of 109,673 with 1024 words, at the buffer boundaries. Those `v1.2.1` rounds ran a revision of the code from before three fixes that do not change how valid traffic is decoded; a confirmation run with the released sources (3 rounds with 2048 words, 2 with 1024) also lost none of 109,640 and 73,005 packets.

```cpp
// Dual-core Full-Speed example (default 200 MHz clock)
void setup()  { Serial.begin(115200); }
void loop()   { sniffer.taskCore0(); }   // Core 0 decodes odd DMA buffers

void setup1() {
    sniffer.begin(2, USBSpeed::FULL_SPEED, 1);
    sniffer.onPacket(onPacketCore1);
    sniffer.onPacketCore0(onPacketCore0); // optional lock-free Core 0 callback
    sniffer.enableDualCore(true);
}
void loop1()  { sniffer.task(); }        // Core 1 decodes even DMA buffers
```

> **8-Hour Continuous Stress Test (`v1.1.0` Single-Core @ `276 MHz -O3`):**
> **8.01 hours**, **420 rounds** (**3,360 tests**), **15,320,290 packets generated** (`0` Host/Wireshark drops). Across all 7 profiles from `10 pkt/s` to `5,000 pkt/s` (including `64B 0xFF` and `64B 0x00`): **`2,562,000 / 2,562,000` packets captured (100.00%)** with **`0` CRC errors**, **`0` stuffing errors**, **`0` overflows**, and **`0` FIFO overruns**. Under extreme Full-Speed saturation (`~9,862 pkt/s`, `64B`): **`11,282,309 / 12,758,290` packets (88.43%)**, with zero thermal degradation over 8 hours.

> **Recommended Arduino IDE clock settings & overclock considerations:**
> - **`200 MHz` (default RP2040 clock, recommended):** no overclocking. With **dual-core mode** (`v1.2.1`), no packet was lost up to ~9,900 pkt/s of 64-byte packets, also in Low-RAM mode (`SNIFFER_DMA_BUF_WORDS=1024`, 16 KB of DMA buffers): 5 rounds each, 2026-11-25. In **single-core mode**, Full-Speed capture was 75–80% from 10 to 5,000 pkt/s and 59% at ~10,000 pkt/s (3 rounds, 2026-11-25), so use dual-core for Full-Speed.
> - **`133–176 MHz` (Underclock / legacy RP2040 stock):** With **Dual-Core mode** (`v1.2.0`, 2026-11-24), `133 MHz` already achieved `99.9–100%` up to 5,000 pkt/s, and `176 MHz` `100.0%` under `P8_MAX` saturation (against the host count; see the note under the table).
> - **`240–250 MHz` (Conservative overclock, Single-Core only):** Useful only if Core 0 cannot participate in decoding (`250 MHz` Single-Core reaches **100.0%** up to 1,000 pkt/s).
> - **`276 MHz` (Aggressive overclock, Single-Core only):** Validated for 8 hours on our board (`100.00%` up to 5,000 pkt/s Single-Core), but due to **silicon lottery** and temperature variations it is not guaranteed on every RP2040 board. Prefer **Dual-Core @ `200 MHz`** instead!
> - **`300 MHz` (Not recommended):** Proven unstable on standard RP2040 silicon (fails USB enumeration).

## Hardware Setup

```
USB Cable (Device ↔ Host)            Raspberry Pi Pico
┌────────────────────┐              ┌──────────────────┐
│  VBUS (red)        │              │                  │
│  D-   (white)      │───[100Ω]────►│ GP3              │
│  D+   (green)      │───[100Ω]────►│ GP2              │
│  GND  (black)      │─────────────►│ GND              │
└────────────────────┘              └──────────────────┘
```

> **⚠ Important:**
> - **Series resistors (100 Ω) are mandatory** — they prevent the Pico's GPIO from loading the USB differential impedance (~90 Ω).
> - **Do NOT connect VBUS (5V) to the Pico** — power the Pico from a separate USB port or other supply.
> - **D+ and D- must be consecutive GPIOs** — the PIO reads both pins in a single instruction.

## Installation

### Arduino IDE

1. Download the [latest release](https://github.com/angeloINTJ/USBSnifferPIO_RP2040/releases) as a `.zip` file
2. In Arduino IDE: **Sketch → Include Library → Add .ZIP Library…**
3. Select the downloaded file

### Arduino Library Manager

Search for **USBSnifferPIO_RP2040** in the Library Manager (Tools → Manage Libraries...).

### PlatformIO

Add to `platformio.ini`:

```ini
lib_deps =
    https://github.com/angeloINTJ/USBSnifferPIO_RP2040.git
```

## Quick Start

```cpp
#include <USBSnifferPIO_RP2040.h>

USBSnifferPIO sniffer;

void onPacket(const USBPacket& pkt) {
    if (pkt.isHIDKeyboardReport()) {
        Serial.printf("Mods=0x%02X Key=0x%02X\n",
                      pkt.data[0], pkt.data[2]);
    }
}

void setup()  { Serial.begin(115200); }
void setup1() { sniffer.begin(2); sniffer.onPacket(onPacket); }
void loop()   { }
void loop1()  { sniffer.task(); }
```

## Examples

| Example | Description |
|---------|-------------|
| [`basic_sniffer`](examples/basic_sniffer/) | Minimal capture — prints every packet to Serial |
| [`dual_core_sniffer`](examples/dual_core_sniffer/) | **Dual-Core Full-Speed** at the default 200 MHz clock + `sniffer.stats()` |
| [`keyboard_logger`](examples/keyboard_logger/) | HID keyboard reports with modifier decoding |
| [`packet_analyzer`](examples/packet_analyzer/) | Full protocol analyzer with hex dump and serial commands |
| [`hid_filter`](examples/hid_filter/) | Auto-discovery + address/endpoint filtering + JSON output |
| [`statistics_monitor`](examples/statistics_monitor/) | Real-time bus health dashboard with error rates |
| [`diagnostic`](examples/diagnostic/) | Layer-by-layer hardware diagnostic (GPIO → PIO → DMA → Decoder) |

## API Reference

### USBSnifferPIO

| Method | Description |
|--------|-------------|
| `bool begin(pin_dp, speed, pio_instance)` | Start capture. Returns `false` if resources unavailable. |
| `void end()` | Stop capture and release all hardware resources. |
| `void task()` | Process pending DMA buffers on Core 1 (all buffers in single-core, even buffers in dual-core). |
| `void taskCore0()` | Process odd DMA buffers on Core 0 when dual-core mode is enabled. |
| `void enableDualCore(bool enable)` | Enable/disable 2× parallel dual-core decoding (default: `false`, backwards-compatible). |
| `bool isDualCore()` | Check whether dual-core mode is active. |
| `void onPacket(callback)` | Register packet callback (Core 1; also used by Core 0 unless `onPacketCore0` is set). |
| `void onPacketCore0(callback)` | Optional dedicated lock-free callback for Core 0 in dual-core mode. |
| `USBSnifferStats stats()` | Combined statistics (Core 1 + Core 0) + active clock/buffer configuration. |
| `void resetStats()` | Reset decoder statistics on both cores. |
| `void setWarmupBuffers(uint8_t n)` | Configure initial warm-up buffers discarded on `begin()` (default: `2`). |
| `USBPacketDecoder& decoder()` | Access Core 1 decoder directly. |
| `USBPacketDecoder& decoderCore0()` | Access Core 0 decoder directly. |
| `bool isRunning()` | Check if capture is active. |

### Configurable Limits (Compile-Time `#ifndef` Overrides)

Define any of these **before** `#include <USBSnifferPIO_RP2040.h>` (or via `-D` build flags in PlatformIO):

| Macro | Default | Description |
|-------|---------|-------------|
| `SNIFFER_DMA_BUF_WORDS` | `2048` | Words per DMA buffer (`2048` = 8 KB / 2.73 ms FS; `1024` = 4 KB Low-RAM mode, no packet lost at 200 MHz dual-core on 2026-11-25) |
| `SNIFFER_DMA_BUF_COUNT` | `4` | DMA ring slots (`4` required for Dual-Core; `2` allowed for Single-Core ultra-low RAM) |
| `SNIFFER_DUALCORE_TAIL_WORDS` | `48` | Words read from the end of the previous buffer and from the start of the next one to resolve packets that cross a buffer boundary in Dual-Core mode (at least `44`, checked at compile time) |
| `SNIFFER_DMA_IRQ_INDEX` | `1` | DMA IRQ line (`1` = `DMA_IRQ_1`, `0` = `DMA_IRQ_0`); override if another library claims `DMA_IRQ_1` exclusively |
| `USB_MAX_PACKET_BYTES` | `72` | Max unstuffed bytes per packet (`PID[1] + DATA[64] + CRC16[2] + margin`) |
| `USB_MAX_PACKET_BITS` | `680` | Max raw bits per packet (64B `0xFF` worst-case bit-stuffing = 625 bits) |
| `USB_MIN_SYNC_BITS` | `4` | Minimum SYNC alternations (`4..7`) required to accept a packet |

### USBPacket

| Field | Type | Description |
|-------|------|-------------|
| `timestamp_us` | `uint32_t` | Capture timestamp (microseconds) |
| `type` | `USBPacketType` | TOKEN, DATA, HANDSHAKE, SPECIAL, UNKNOWN |
| `pid` | `uint8_t` | Packet Identifier (lower 4 bits) |
| `addr` | `uint8_t` | Device address (token packets only) |
| `endp` | `uint8_t` | Endpoint number (token packets only) |
| `frame_number` | `uint16_t` | Frame number (SOF only) |
| `data[64]` | `uint8_t[]` | Payload bytes (data packets only) |
| `data_length` | `uint8_t` | Payload size excluding CRC |
| `crc_valid` | `bool` | CRC verification result |
| `isHIDKeyboardReport()` | `bool` | Heuristic: DATA0/DATA1 with 8 bytes |

### Decoder Statistics

Access via `sniffer.decoder()`:

| Counter | Description |
|---------|-------------|
| `packets_decoded` | Successfully decoded packets |
| `crc_errors` | CRC5/CRC16 mismatches |
| `sync_errors` | Malformed SYNC patterns |
| `stuffing_errors` | Bit-stuffing violations |
| `overflow_errors` | Packets exceeding maximum length |
| `fifo_overruns` | PIO RX FIFO stall or DMA buffer overrun events |

In dual-core mode, `sniffer.stats()` adds both cores' counters. They are exact for valid traffic. With a corrupted stream, an error in a packet that crosses a DMA buffer boundary can be counted by both cores. After a stretch with no SE0 and no run of 10 J symbols, which the decoder needs to resynchronize at a buffer boundary, packets and errors up to the next such point can be missed.

## Project Structure

```
USBSnifferPIO_RP2040/
├── src/
│   ├── USBSnifferPIO_RP2040.h   # Top-level class (PIO + 2-ch DMA + Decoder)
│   ├── USBSnifferPIO_RP2040.cpp # PIO/DMA ping-pong setup, DMA_IRQ_1, task loop
│   ├── USBPacketDecoder.h       # SRAM -O3 decoder interface & statistics
│   ├── USBPacketDecoder.cpp     # Inline NRZI, bit-unstuffing, table CRC5/CRC16
│   ├── USBProtocol.h            # PIDs, CRC, USBPacket struct, enums
│   ├── usb_sniffer.pio          # PIO source: 22-instruction 8× DPLL + SM init
│   └── usb_sniffer.pio.h        # Generated by pioasm from usb_sniffer.pio
├── examples/
│   ├── basic_sniffer/           # Minimal capture
│   ├── keyboard_logger/         # HID keyboard monitor
│   ├── packet_analyzer/         # Full protocol analyzer
│   ├── hid_filter/              # Address/endpoint filtering + JSON
│   ├── statistics_monitor/      # Bus health dashboard
│   └── diagnostic/              # Hardware diagnostic
├── extras/host_test/            # Host-side decoder test, run by CI (make -C extras/host_test)
├── extras/test_bench/           # Automated Pico 2 W + Wireshark benchmark suite
├── library.properties           # Arduino Library Manager metadata
├── library.json                 # PlatformIO metadata
├── keywords.txt                 # Arduino IDE syntax highlighting
├── CONTRIBUTING.md              # Contribution guidelines
├── LICENSE                      # MIT License
├── .gitignore
└── README.md
```

## Resource Usage

| Resource | Usage |
|----------|-------|
| PIO state machines | 1 (auto-claimed) |
| PIO instruction memory | 22 slots (of 32 per block) |
| DMA channels | 2 (chained hardware ping-pong + `DMA_IRQ_1`) |
| RAM | ~35 KB: 32 KB DMA ring (4 × 8 KB) + SRAM-resident decoder code and tables; ~19 KB with `SNIFFER_DMA_BUF_WORDS=1024` (a minimal dual-core sketch minus an empty one, arduino-pico 6.3.1, 2026-11-25) |
| Flash | ~6 KB, including the decoder code copied to RAM at boot (same method, size of the `.bin` image) |
| CPU | Near 0% during idle bus (O(1) all-J fast-path) |

## Requirements

- **Board:** Raspberry Pi Pico or Pico W
- **Arduino Core:** [Earle Philhower arduino-pico](https://github.com/earlephilhower/arduino-pico) 4.0 or later (pico-sdk 2.x). On 2026-11-25 the library compiled on 4.0.0 and 6.3.1, and the bench ran on 6.3.1; 3.9.5 (pico-sdk 1.5.1) does not compile it.
- **PIO:** One free state machine and 22 free instruction slots on `pio0` or `pio1`
- **DMA:** Two free DMA channels (uses shared `DMA_IRQ_1` handler by default; configurable to `DMA_IRQ_0`)

## Coexistence with Other PIO / DMA Modules

`USBSnifferPIO_RP2040` is designed to coexist safely in multi-peripheral projects:

- **Non-panicking resource allocation:** `begin()` uses `pio_claim_unused_sm(..., false)`, `pio_can_add_program()`, and `dma_claim_unused_channel(..., false)`. If resources are unavailable (or another `USBSnifferPIO` instance is already active), it rolls back cleanly and returns `false`.
- **Shared DMA IRQ:** Uses `irq_add_shared_handler()` and only acknowledges its own two DMA channels. If another library claims `DMA_IRQ_1` *exclusively*, define `#define SNIFFER_DMA_IRQ_INDEX 0` before `#include <USBSnifferPIO_RP2040.h>`.
- **Zero-gap ISR tolerance:** Hardware ping-pong (`ch0 ↔ ch1`) gives `DMA_IRQ` up to `~2.7 ms` (Full-Speed) or `~21.8 ms` (Low-Speed) of latency headroom, so brief ISRs from SPI, Audio, or Wi-Fi will not drop PIO samples.

| Coexisting Module | Recommended Setup | Notes |
|---|---|---|
| **Pico W Wi-Fi (CYW43)** | `sniffer.begin(2, speed, 0)` *(use `pio0`)* | CYW43 uses ~24 instruction slots on `pio1`; `pio1` cannot fit the 22-instruction sniffer alongside Wi-Fi. |
| **NeoPixel / WS2812** | NeoPixel on `pio0`, sniffer on `pio1` *(default)* | Can even share a PIO block if combined instructions ≤ 32 (22 + ~4–8 ≤ 32). |
| **`DHT22PIO_RP2040`** | DHT22 on `pio0`, sniffer on `pio1` *(default)* | Combined instructions (>32) exceed one PIO block — keep on separate PIO blocks. |
| **Blocking Core 0 I/O** *(Wi-Fi HTTP, SD card, I2C OLED, heavy `Serial`)* | Use **Single-Core mode** *(default)* | **Important:** In Dual-Core mode, Core 0 must call `taskCore0()` every `< 5.4 ms` (FS `2048w`). If Core 0 runs blocking calls, keep Single-Core mode so Core 1 stays isolated and Core 0 is 100% free. |
| **Tight RAM projects** *(Pico W + Wi-Fi + TLS)* | `#define SNIFFER_DMA_BUF_WORDS 1024` | Cuts DMA RAM in half (16 KB instead of 32 KB); no packet lost at 200 MHz dual-core on 2026-11-25. |

> **Tip:** Always monitor `sniffer.stats().fifo_overruns` when integrating additional libraries — any non-zero count immediately tells you if another module is starving CPU or DMA bandwidth.

## Limitations

- USB 1.1 only (Low-Speed and Full-Speed). USB 2.0 High-Speed (480 Mbps) is beyond the RP2040's PIO sampling rate.
- Full-Speed (12 Mbps) requires a 96 MHz PIO clock and fast decoding. At the default `200 MHz` clock, use dual-core mode: single-core captured 75–80% of Full-Speed packets on 2026-11-25.
- Avoid heavy blocking `Serial.print()` inside `onPacket()` — copy packets into a ring buffer on Core 1 and print from Core 0 (as shown in the examples).
- The decoder is not interrupt-safe. Call `task()` from a single core only.
- Maximum payload per packet: 64 bytes (Full-Speed bulk/interrupt endpoint limit).

## FAQ

**Q: Can I use this with NeoPixels / WS2812?**
A: Yes. NeoPixel libraries typically use `pio0` SM0. USBSnifferPIO auto-claims the next free SM. If `pio0` does not have 22 free instruction slots, pass `pio_instance=1` to `begin()`.

**Q: Does this work on the Pico W?**
A: Yes. The Pico W uses `pio1` SM0 for the CYW43 Wi-Fi driver. Using `pio_instance=0` avoids `pio1` entirely and leaves all 32 instruction slots available for the sniffer.

**Q: Why not use the RP2040's native USB peripheral?**
A: The native USB peripheral can only act as a host or device — it cannot passively observe a bus it is not participating in. PIO-based sampling taps the raw differential lines without electrical interference.

**Q: What about the Pico 2 (RP2350)?**
A: The RP2350 has PIO v2 with the same instruction set and a faster Cortex-M33 core. This library should work without changes, and has even more headroom for Full-Speed decoding.

## Wiring

```
  RP2040 GPIO 2 (D+) ──── 100Ω ──── USB cable D+ (green)
  RP2040 GPIO 3 (D-) ──── 100Ω ──── USB cable D- (white)
  RP2040 GND ────────────────────── USB cable GND (black)
```

**Do NOT connect VBUS.** Power the Pico from its own USB port or an external supply.

## Contributing

Contributions are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting a pull request.

### Quick Guide

1. Fork this repository
2. Create a feature branch: `git checkout -b feature/my-improvement`
3. Commit your changes: `git commit -m "Add: description of change"`
4. Push to the branch: `git push origin feature/my-improvement`
5. Open a Pull Request

## License

MIT License — see [LICENSE](LICENSE).

## Acknowledgments

* Raspberry Pi Foundation for the RP2040 PIO architecture
* The Arduino-Pico community for the RP2040 Arduino core (Earle Philhower)
* The embedded community for feedback and Wireshark validation on PIO-based USB analysis techniques

## See Also

- [DHT22PIO_RP2040](https://github.com/angeloINTJ/DHT22PIO_RP2040) — PIO-accelerated DHT22 library by the same author
