# USBSnifferPIO_RP2040

> Passive USB 1.1 packet sniffer for RP2040 — built entirely on the PIO coprocessor.

Captures Low-Speed and Full-Speed USB traffic without interfering with the bus.  
The host and device never know the sniffer exists.

## Features

| Feature | Details |
|---------|---------|
| **100% passive** | High-impedance tap — zero electrical interference |
| **Low-Speed + Full-Speed** | 1.5 Mbps and 12 Mbps USB 1.1 |
| **8× oversampling** | Robust clock recovery via edge-triggered phase alignment |
| **DMA ping-pong** | Zero-copy manual ping-pong — no sample loss |
| **Full protocol decode** | NRZI → bit unstuffing → framing → CRC5/CRC16 |
| **Callback API** | Receive structured `USBPacket` with PID, address, endpoint, payload |
| **Dual-core ready** | Capture on Core 1, process on Core 0 |
| **Minimal resources** | 1 PIO state machine, 1 DMA channel, ~17 KB RAM |

## How it works

```
┌───────────┐     ┌────────────────┐     ┌──────────────────┐
│  PIO SM   │────►│  DMA Ping-Pong │────►│  USBPacketDecoder│
│  in pins,2│     │  2 × 8 KB      │     │                  │
│  12 MHz   │     │  (manual swap) │     │  Clock Recovery  │
│           │     │                │     │  NRZI Decode     │
│  D+ ─┐   │     │                │     │  Bit Unstuffing  │
│  D- ─┘   │     │                │     │  Packet Parsing  │
└───────────┘     └────────────────┘     │  CRC Verification│
                                         └────────┬─────────┘
                                                   │
                                           callback(USBPacket)
```

1. **PIO program** (1 instruction in a wrap-loop) samples D+ and D- every clock cycle. 16 two-bit samples are packed into a 32-bit word and autopushed to the RX FIFO. Clock divider sets the 8× oversampling rate.
2. **DMA ping-pong** transfers words from the FIFO into two 8 KB RAM buffers. A single channel with manual restart avoids the RP2040 `chain_to` WRITE_ADDR race condition that corrupts memory.
3. **Software decoder** implements edge-triggered clock recovery, SYNC detection, NRZI decoding, bit-unstuffing, EOP framing, PID validation, and CRC5/CRC16 verification. An all-J fast-path optimization skips idle bus words in O(1).

## Hardware Setup

```
USB Cable (Device ↔ Host)           Raspberry Pi Pico
┌────────────────────┐              ┌──────────────────┐
│  VBUS (red)        │              │                  │
│  D-   (white)      │──[100Ω]────►│ GP3              │
│  D+   (green)      │──[100Ω]────►│ GP2              │
│  GND  (black)      │────────────►│ GND              │
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

## Project Structure

```
USBSnifferPIO_RP2040/
├── src/
│   ├── USBSnifferPIO_RP2040.h   # Top-level class (PIO + DMA + Decoder)
│   ├── USBSnifferPIO_RP2040.cpp # PIO/DMA setup, processing loop
│   ├── USBPacketDecoder.h       # Decoder: clock recovery → packet parse
│   ├── USBPacketDecoder.cpp     # Full decoding pipeline implementation
│   ├── USBProtocol.h            # PIDs, CRC, USBPacket struct, enums
│   ├── usb_sniffer.pio          # PIO assembly source (1 instruction)
│   └── usb_sniffer.pio.h        # Pre-compiled PIO header + SM init
├── examples/
│   ├── basic_sniffer/           # Minimal capture
│   ├── keyboard_logger/         # HID keyboard monitor
│   ├── packet_analyzer/         # Full protocol analyzer
│   ├── hid_filter/              # Address/endpoint filtering + JSON
│   ├── statistics_monitor/      # Bus health dashboard
│   └── diagnostic/              # Hardware diagnostic
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
| PIO instruction memory | 1 slot (of 32 per block) |
| DMA channels | 1 (manual ping-pong) |
| RAM | ~17 KB (2 × 8 KB buffers + decoder) |
| Flash | ~6 KB (code) |
| CPU | 0% during idle bus (fast-path optimization) |

## Requirements

- **Board:** Raspberry Pi Pico or Pico W
- **Arduino Core:** [Earle Philhower arduino-pico](https://github.com/earlephilhower/arduino-pico) 3.x+
- **PIO:** One free state machine on pio0 or pio1
- **DMA:** One free channel

## Limitations

- USB 1.1 only (Low-Speed and Full-Speed). USB 2.0 High-Speed (480 Mbps) is beyond the RP2040's PIO sampling rate.
- Full-Speed (12 Mbps) with 8× oversampling requires 96 MHz PIO clock. The divider of 1.25 at 120 MHz leaves minimal margin — consider 125 or 133 MHz system clock.
- The decoder is not interrupt-safe. Call `task()` from a single core only.
- Maximum payload per packet: 64 bytes (Full-Speed bulk/interrupt endpoint limit).

## FAQ

**Q: Can I use this with NeoPixels / WS2812?**
A: Yes. NeoPixel libraries typically use `pio0` SM0. USBSnifferPIO auto-claims the next free SM. If `pio0` is full, pass `pio_instance=1` to `begin()`.

**Q: Does this work on the Pico W?**
A: Yes. The Pico W uses `pio1` SM0 for the CYW43 Wi-Fi driver. Using `pio_instance=1` will auto-claim SM1 (leaving SM0 for Wi-Fi). Or use `pio_instance=0` to avoid `pio1` entirely.

**Q: Why not use the RP2040's native USB peripheral?**
A: The native USB peripheral can only act as a host or device — it cannot passively observe a bus it is not participating in. PIO-based sampling taps the raw differential lines without electrical interference.

**Q: What about the Pico 2 (RP2350)?**
A: The RP2350 has PIO v2 with the same instruction set. This library should work without changes, but has not been tested yet.

## Wiring

```
  RP2040 GPIO 2 (D+) ──── 100Ω ──── USB cable D+ (green)
  RP2040 GPIO 3 (D-) ──── 100Ω ──── USB cable D- (white)
  RP2040 GND ─────────────────────── USB cable GND (black)
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
* The embedded community for feedback on PIO-based USB analysis techniques
# USBSnifferPIO_RP2040
