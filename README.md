# USBSnifferPIO_RP2040

> Passive USB 1.1 packet sniffer for RP2040 — built on a hardware PIO DPLL and zero-gap 2-channel DMA ping-pong.

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
| **Compact resources** | 1 PIO state machine (22 instructions), 2 DMA channels, ~28 KB RAM |

## How it works

```
┌──────────────┐     ┌──────────────────────┐     ┌────────────────────┐
│  PIO SM      │────►│ 2-Ch HW Ping-Pong DMA│────►│ USBPacketDecoder   │
│  8× HW DPLL  │     │ 2 × 8 KB (ch0 ↔ ch1) │     │ (SRAM + -O3)       │
│  22 instr.   │     │ 0 ns gap + DMA_IRQ_1 │     │                    │
│  D+ ─┐ (JMP) │     └──────────────────────┘     │ SYNC + Inline NRZI │
│  D- ─┘       │                                  │ Bit Unstuffing     │
└──────────────┘                                  │ Table CRC5 / CRC16 │
                                                  └─────────┬──────────┘
                                                            │
                                                    callback(USBPacket)
```

1. **PIO hardware DPLL** (22 instructions) runs at 8× the USB bit rate (12 MHz for LS, 96 MHz for FS), tracks edges on `D+` (`JMP_PIN`), and samples `[D+, D-]` at exact bit center (`t = 4/8`). 16 clock-recovered USB bits are packed into each 32-bit word and autopushed to the 8-deep RX FIFO — cutting DMA and CPU bandwidth by **8×** compared to raw oversampling.
2. **Zero-gap 2-channel hardware ping-pong DMA** chains two channels (`ch0 ↔ ch1`) across two 8 KB buffers. When one channel finishes, hardware switches immediately (0 ns gap) while `DMA_IRQ_1` safely resets the inactive channel's `WRITE_ADDR` and `TRANS_COUNT` with a full `2.73 ms` (FS) / `21.85 ms` (LS) margin — eliminating both inter-buffer stalls and the `WRITE_ADDR` race condition.
3. **SRAM-resident `-O3` decoder** runs directly from `.time_critical` RAM (even when the IDE is set to `-Os`), skipping all-J idle words in O(1), performing inline NRZI decoding, bit-unstuffing, sentinel-bit byte packing, PID lookup, and table-accelerated CRC5/CRC16 verification.

## Performance & Benchmarks (Full-Speed 12 Mbps)

Validated on a real hardware test bench comparing **Raspberry Pi Pico 2 W** (calibrated Full-Speed USB CDC generator) against **Wireshark (`usbmon`)** as ground truth (0% loss) and **Raspberry Pi Pico (RP2040)** running `USBSnifferPIO_RP2040`:

| Version / Clock | 10 pkt/s (16B) | 100–1,000 pkt/s (16B) | 5,000 pkt/s (16B) | 1,000 pkt/s (64B `0xFF`) | 1,000 pkt/s (64B `0x00`) | MAX (~9,860 pkt/s, 64B) | CRC Errors (10–5k pkt/s) |
|-----------------|---------------:|----------------------:|------------------:|-------------------------:|-------------------------:|------------------------:|-------------------------:|
| `v1.0.0` (200 MHz `-Os`) | 1.0% | 1.6–1.9% | 1.9% | 0.0% *(overflow)* | 1.3% | 1.6% | 13 |
| **`v1.1.0` (`200 MHz` official)** | **100.0%** | 79.5–80.8% | 76.9% | 79.4% | 78.0% | 64.0% | **0** |
| **`v1.1.0` (`240 MHz` `-O3`)** | 97.0% | 96.0–97.0% | 91.8% | 93.9% | 95.0% | 76.8% | **0** |
| **`v1.1.0` (`250 MHz` `-O3`)** | **100.0%** | **100.0%** | 97.1% | 91.8% | 98.5% | 80.0% | 0–1 |
| **`v1.1.0` (`276 MHz` `-O3`, 8h stress)** | **100.00%** | **100.00%** | **100.00%** | **100.00%** | **100.00%** | **88.43%** | **0 (`2,562,000 / 2,562,000`)** |

> **8-Hour Continuous Stress Test (`v1.1.0` @ `276 MHz -O3`):**
> **8.01 hours**, **420 rounds** (**3,360 tests**), **15,320,290 packets generated** (`0` Host/Wireshark drops). Across all 7 profiles from `10 pkt/s` to `5,000 pkt/s` (including `64B 0xFF` and `64B 0x00`): **`2,562,000 / 2,562,000` packets captured (100.00%)** with **`0` CRC errors**, **`0` stuffing errors**, **`0` overflows**, and **`0` FIFO overruns**. Under extreme Full-Speed saturation (`~9,862 pkt/s`, `64B`): **`11,282,309 / 12,758,290` packets (88.43%)**, with zero thermal degradation over 8 hours.

> **Recommended Arduino IDE clock settings & overclock considerations:**
> - **`200 MHz` (Official RP2040 default — recommended for general use):** 100% stable across all RP2040 boards, zero overclocking. Achieves **100%** in Low-Speed (1.5 Mbps) and **~77–100%** in Full-Speed (12 Mbps) — up from 1.6% in `v1.0.0`. The decoder hot path automatically compiles with `-O3` in SRAM even when the IDE is set to `-Os`.
> - **`240–250 MHz` (Conservative overclock, recommended for heavy Full-Speed):** Mild overclock supported by virtually all RP2040 boards (`240 MHz` gives an exact `2.5` PIO divider; `250 MHz` already achieves **100.0% lossless capture up to 1,000 pkt/s** and **97.1% at 5,000 pkt/s**).
> - **`276 MHz` (Aggressive overclock, optional):** Achieves **100.00% lossless Full-Speed capture up to 5,000 pkt/s** (`0` CRC / `0` FIFO overruns) and **88.43%** at ~9,862 pkt/s saturation (validated for 8 continuous hours). Because of **silicon lottery** and ambient temperature variations, `276 MHz` is not guaranteed on every third-party RP2040 board — verify stability on your unit before relying on it in production.
> - **`300 MHz` (Not recommended):** Proven unstable on standard RP2040 silicon (fails USB enumeration).
> - In Arduino IDE (`arduino-pico` core), configure via **Tools → CPU Speed** and optionally **Tools → Optimize → Optimize Even More (-O3)**.

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
| `void task()` | Process pending DMA buffers. Call frequently. |
| `void onPacket(callback)` | Register packet callback. |
| `USBPacketDecoder& decoder()` | Access decoder for statistics. |
| `bool isRunning()` | Check if capture is active. |

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

## Project Structure

```
USBSnifferPIO_RP2040/
├── src/
│   ├── USBSnifferPIO_RP2040.h   # Top-level class (PIO + 2-ch DMA + Decoder)
│   ├── USBSnifferPIO_RP2040.cpp # PIO/DMA ping-pong setup, DMA_IRQ_1, task loop
│   ├── USBPacketDecoder.h       # SRAM -O3 decoder interface & statistics
│   ├── USBPacketDecoder.cpp     # Inline NRZI, bit-unstuffing, table CRC5/CRC16
│   ├── USBProtocol.h            # PIDs, CRC, USBPacket struct, enums
│   ├── usb_sniffer.pio          # PIO assembly source (22-instruction 8× DPLL)
│   └── usb_sniffer.pio.h        # Pre-compiled PIO header + SM init
├── examples/
│   ├── basic_sniffer/           # Minimal capture
│   ├── keyboard_logger/         # HID keyboard monitor
│   ├── packet_analyzer/         # Full protocol analyzer
│   ├── hid_filter/              # Address/endpoint filtering + JSON
│   ├── statistics_monitor/      # Bus health dashboard
│   └── diagnostic/              # Hardware diagnostic
├── tools/test_bench/            # Automated Pico 2 W + Wireshark benchmark suite
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
| RAM | ~28 KB (2 × 8 KB buffers + SRAM decoder & CRC tables) |
| Flash | ~6 KB (code) |
| CPU | Near 0% during idle bus (O(1) all-J fast-path) |

## Requirements

- **Board:** Raspberry Pi Pico or Pico W
- **Arduino Core:** [Earle Philhower arduino-pico](https://github.com/earlephilhower/arduino-pico) 3.x+
- **PIO:** One free state machine and 22 free instruction slots on `pio0` or `pio1`
- **DMA:** Two free DMA channels (uses shared `DMA_IRQ_1` handler)

## Limitations

- USB 1.1 only (Low-Speed and Full-Speed). USB 2.0 High-Speed (480 Mbps) is beyond the RP2040's PIO sampling rate.
- Full-Speed (12 Mbps) requires a 96 MHz PIO clock and fast decoding. Use the default `200 MHz` CPU clock for moderate traffic, or `276 MHz` (`-O3`) in Arduino IDE for lossless capture up to 5,000+ pkt/s.
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
