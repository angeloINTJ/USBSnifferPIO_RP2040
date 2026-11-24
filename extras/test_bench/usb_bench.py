#!/usr/bin/env python3
"""
Host controller & validator for pico2w_usb_generator.ino.

Keeps the USB CDC Bulk IN pipe drained (so the Linux cdc_acm driver continuously
polls the Pico 2 W), sends generator commands, and validates sequence numbers
in real time.

Usage examples:
  # Trigger a deterministic 100-packet burst at 100 pkt/s:
  python3 extras/test_bench/usb_bench.py /dev/ttyACM1 --rate 2 --burst 100

  # Trigger a 1000-packet burst at 1000 pkt/s:
  python3 extras/test_bench/usb_bench.py /dev/ttyACM1 --rate 4 --burst 1000

  # Continuous stream for 5 seconds at MAX speed:
  python3 extras/test_bench/usb_bench.py /dev/ttyACM1 --rate 6 --duration 5
"""

import argparse
import os
import select
import struct
import sys
import termios
import time


def open_serial_raw(port: str) -> int:
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    # Raw mode (no echo, no canonical buffering, no CR/LF translation)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def drain_serial(fd: int, duration_s: float = 0.15) -> bytes:
    out = bytearray()
    deadline = time.monotonic() + duration_s
    while time.monotonic() < deadline:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                chunk = os.read(fd, 4096)
                if chunk:
                    out.extend(chunk)
            except BlockingIOError:
                pass
    return bytes(out)


def main() -> None:
    parser = argparse.ArgumentParser(description="Pico 2 W USB Packet Generator Host Bench")
    parser.add_argument("port", help="Serial port of Pico 2 W (e.g. /dev/ttyACM1)")
    parser.add_argument("--rate", choices=["1", "2", "3", "4", "5", "6"], default="2",
                        help="1=10Hz, 2=100Hz, 3=500Hz, 4=1kHz, 5=5kHz, 6=MAX (default: 2)")
    parser.add_argument("--burst", type=int, choices=[100, 1000], default=None,
                        help="Run deterministic burst of 100 or 1000 packets")
    parser.add_argument("--duration", type=float, default=3.0,
                        help="Duration in seconds for continuous mode (if --burst not set)")
    parser.add_argument("--len-cycles", type=int, default=0,
                        help="Press 'l' N times (0=16B, 1=32B, 2=64B, 3=8B)")
    parser.add_argument("--pat-cycles", type=int, default=0,
                        help="Press 'p' N times (0=INC, 1=0xFF, 2=0x00, 3=0x55)")
    parser.add_argument("--reset-seq", action="store_true",
                        help="Reset sequence counter to 0 before starting")
    args = parser.parse_args()

    fd = open_serial_raw(args.port)
    try:
        # Configure generator
        if args.reset_seq:
            os.write(fd, b"r")
            time.sleep(0.05)

        os.write(fd, args.rate.encode())
        time.sleep(0.05)

        for _ in range(args.len_cycles):
            os.write(fd, b"l")
            time.sleep(0.05)

        for _ in range(args.pat_cycles):
            os.write(fd, b"p")
            time.sleep(0.05)

        status = drain_serial(fd, 0.2)
        if status:
            print(status.decode(errors="replace").strip())

        # Start generation
        if args.burst == 100:
            os.write(fd, b"b")
            target_count = 100
            max_wait = 15.0
        elif args.burst == 1000:
            os.write(fd, b"B")
            target_count = 1000
            max_wait = 120.0
        else:
            os.write(fd, b"s")
            target_count = None
            max_wait = args.duration

        buf = bytearray()
        rx_packets = 0
        first_seq = None
        last_seq = None
        missing = 0
        t_start = time.monotonic()
        t_last_rx = t_start

        while True:
            now = time.monotonic()
            if target_count is None and (now - t_start) >= max_wait:
                os.write(fd, b"s")  # Stop continuous mode
                break
            if target_count is not None:
                if rx_packets >= target_count:
                    break
                if (now - t_start) >= max_wait or (rx_packets > 0 and (now - t_last_rx) > 1.5):
                    break

            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    chunk = os.read(fd, 4096)
                except BlockingIOError:
                    continue
                if not chunk:
                    continue
                t_last_rx = time.monotonic()
                buf.extend(chunk)

                # Parse binary packets (0xA5 0x5A + seq + len + pat + payload)
                idx = 0
                while idx + 8 <= len(buf):
                    if buf[idx] == 0xA5 and buf[idx + 1] == 0x5A:
                        plen = buf[idx + 6]
                        if plen in (8, 16, 32, 64):
                            if idx + plen > len(buf):
                                break  # wait for rest of packet
                            seq = struct.unpack_from("<I", buf, idx + 2)[0]
                            if first_seq is None:
                                first_seq = seq
                            elif seq != last_seq + 1:
                                missing += (seq - last_seq - 1) if seq > last_seq else 1
                            last_seq = seq
                            rx_packets += 1
                            idx += plen
                            continue
                    idx += 1
                if idx > 0:
                    del buf[:idx]

        elapsed = max(time.monotonic() - t_start, 1e-6)
        rate = rx_packets / elapsed
        print("\n=== Resultado no Host (Linux cdc_acm) ===")
        print(f"Pacotes recebidos : {rx_packets}")
        print(f"Primeiro / Último : {first_seq} .. {last_seq}")
        print(f"Perdas no Host    : {missing}")
        print(f"Tempo / Taxa real : {elapsed:.2f} s ({rate:.1f} pkt/s)")
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
