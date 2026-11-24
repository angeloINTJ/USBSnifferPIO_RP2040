#!/usr/bin/env python3
"""
8-Hour Automated USB Sniffer Stress & Benchmark Suite.

Orchestrates:
  1. Packet Generator (Raspberry Pi Pico 2 W over USB CDC)
  2. Reference Capture (Wireshark / dumpcap + tshark on Linux usbmon)
  3. Sniffer DUT (USBSnifferPIO_RP2040 running sniffer_bench_dut.ino, optional)

Outputs (updated continuously after every round, safe against Ctrl+C):
  - results/stress_<timestamp>/results.csv
  - results/stress_<timestamp>/results.jsonl
  - results/stress_<timestamp>/REPORT.md
  - results/stress_<timestamp>/fail_pcaps/  (up to 20 anomaly captures)

Usage examples:
  # Dry run (1 full round, ~45 seconds) WITHOUT sniffer yet:
  python3 tools/test_bench/stress_8h.py --rounds 1

  # Full 8-hour stress test WITH sniffer on /dev/ttyACM0:
  python3 tools/test_bench/stress_8h.py --hours 8 --sniffer /dev/ttyACM0

  # Full 8-hour stress test specifying ports explicitly:
  python3 tools/test_bench/stress_8h.py --hours 8 --gen /dev/ttyACM2 --sniffer /dev/ttyACM0
"""

import argparse
import csv
import datetime as dt
import glob
import json
import os
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import termios
import time
from collections import defaultdict
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


# ── Test Profile Matrix (executed every round) ─────────────────────────────
# cmd_rate: '1'=10Hz, '2'=100Hz, '3'=500Hz, '4'=1kHz, '5'=5kHz, '6'=MAX
# cmd_len : 'a'=8B, 'c'=16B, 'd'=32B, 'e'=64B
# cmd_pat : 'w'=INC, 'x'=0xFF, 'y'=0x00, 'z'=0x55
TEST_PROFILES: List[Dict[str, Any]] = [
    {"id": "P1_10Hz_16B_INC",   "rate_cmd": "1", "rate_lbl": "10 pkt/s",   "len_cmd": "c", "len_b": 16, "pat_cmd": "w", "pat_lbl": "INC",  "mode": "burst", "count": 100,  "dur": 12.0},
    {"id": "P2_100Hz_16B_INC",  "rate_cmd": "2", "rate_lbl": "100 pkt/s",  "len_cmd": "c", "len_b": 16, "pat_cmd": "w", "pat_lbl": "INC",  "mode": "burst", "count": 1000, "dur": 12.0},
    {"id": "P3_500Hz_16B_INC",  "rate_cmd": "3", "rate_lbl": "500 pkt/s",  "len_cmd": "c", "len_b": 16, "pat_cmd": "w", "pat_lbl": "INC",  "mode": "burst", "count": 1000, "dur": 4.0},
    {"id": "P4_1kHz_16B_INC",   "rate_cmd": "4", "rate_lbl": "1000 pkt/s", "len_cmd": "c", "len_b": 16, "pat_cmd": "w", "pat_lbl": "INC",  "mode": "burst", "count": 1000, "dur": 3.0},
    {"id": "P5_5kHz_16B_INC",   "rate_cmd": "5", "rate_lbl": "5000 pkt/s", "len_cmd": "c", "len_b": 16, "pat_cmd": "w", "pat_lbl": "INC",  "mode": "burst", "count": 1000, "dur": 2.5},
    {"id": "P6_1kHz_64B_FF",    "rate_cmd": "4", "rate_lbl": "1000 pkt/s", "len_cmd": "e", "len_b": 64, "pat_cmd": "x", "pat_lbl": "0xFF", "mode": "burst", "count": 1000, "dur": 3.0},
    {"id": "P7_1kHz_64B_00",    "rate_cmd": "4", "rate_lbl": "1000 pkt/s", "len_cmd": "e", "len_b": 64, "pat_cmd": "y", "pat_lbl": "0x00", "mode": "burst", "count": 1000, "dur": 3.0},
    {"id": "P8_MAX_64B_INC",    "rate_cmd": "6", "rate_lbl": "MAX",        "len_cmd": "e", "len_b": 64, "pat_cmd": "w", "pat_lbl": "INC",  "mode": "cont",  "count": 0,    "dur": 3.0},
]

STOP_REQUESTED = False


def _handle_signal(signum: int, _frame: Any) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True
    print(f"\n[!] Sinal {signum} recebido — finalizando ciclo atual e salvando relatório...", flush=True)


# ── Hardware Discovery ─────────────────────────────────────────────────────

def find_pico2w_port() -> Optional[str]:
    matches = sorted(glob.glob("/dev/serial/by-id/usb-Raspberry_Pi_Pico_2W_*"))
    if matches:
        return os.path.realpath(matches[0])
    return None


def find_pico2w_usbmon() -> Tuple[int, int]:
    out = subprocess.check_output(["lsusb"], text=True)
    for line in out.splitlines():
        if "2e8a:f00f" in line.lower():
            m = re.match(r"Bus\s+(\d+)\s+Device\s+(\d+):", line)
            if m:
                return int(m.group(1)), int(m.group(2))
    raise RuntimeError("Pico 2 W (ID 2e8a:f00f) não encontrado via lsusb.")


# ── Raw Serial Helpers ─────────────────────────────────────────────────────

def open_serial_raw(port: str) -> int:
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def drain_fd(fd: int, duration_s: float = 0.1) -> bytes:
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


def sniffer_cmd(fd: int, cmd: bytes, timeout_s: float = 1.0) -> str:
    termios.tcflush(fd, termios.TCIFLUSH)
    os.write(fd, cmd)
    buf = bytearray()
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                chunk = os.read(fd, 4096)
                if chunk:
                    buf.extend(chunk)
                    if b"\n" in buf:
                        break
            except BlockingIOError:
                pass
    return buf.decode(errors="replace").strip()


# ── Wireshark / dumpcap Helpers ────────────────────────────────────────────

def start_dumpcap(bus: int, duration_s: int, pcap_path: Path) -> subprocess.Popen:
    iface = f"usbmon{bus}"
    dev_path = f"/dev/{iface}"
    if os.access(dev_path, os.R_OK | os.W_OK):
        cmd = ["dumpcap", "-q", "-i", iface, "-a", f"duration:{duration_s}", "-w", str(pcap_path)]
    else:
        cmd = ["sg", "wireshark", "-c",
               f"dumpcap -q -i '{iface}' -a 'duration:{duration_s}' -w '{pcap_path}'"]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def analyze_pcap(pcap_path: Path, dev_addr: int) -> Dict[str, Any]:
    cmd = [
        "tshark", "-r", "-",
        "-Y", f"usb.device_address == {dev_addr} && usb.endpoint_address.direction == 1 && usb.data_len > 0",
        "-T", "fields",
        "-e", "usbcom.data.in_payload",
        "-e", "usb.capdata",
    ]
    with open(pcap_path, "rb") as f:
        res = subprocess.run(cmd, stdin=f, capture_output=True, text=True)

    seqs: List[int] = []
    urbs = 0
    for line in res.stdout.splitlines():
        parts = line.split("\t")
        hex_payload = (parts[0] if len(parts) > 0 and parts[0] else
                       (parts[1] if len(parts) > 1 else "")).strip()
        if not hex_payload:
            continue
        urbs += 1
        raw = bytes.fromhex(hex_payload.replace(":", ""))
        i = 0
        while i + 8 <= len(raw):
            if raw[i] == 0xA5 and raw[i + 1] == 0x5A:
                plen = raw[i + 6]
                if plen in (8, 16, 32, 64) and i + plen <= len(raw):
                    seq = struct.unpack_from("<I", raw, i + 2)[0]
                    seqs.append(seq)
                    i += plen
                    continue
            i += 1

    gaps = 0
    if seqs:
        for a, b in zip(seqs, seqs[1:]):
            if b != a + 1:
                gaps += (b - a - 1) if b > a else 1

    return {
        "ws_urbs": urbs,
        "ws_pkts": len(seqs),
        "ws_first": seqs[0] if seqs else -1,
        "ws_last": seqs[-1] if seqs else -1,
        "ws_gaps": gaps,
    }


# ── Single Profile Execution ───────────────────────────────────────────────

def run_profile(
    gen_fd: int,
    sniffer_fd: Optional[int],
    bus: int,
    dev_addr: int,
    prof: Dict[str, Any],
    round_idx: int,
    tmp_pcap: Path,
    enable_wireshark: bool,
) -> Dict[str, Any]:
    ts_iso = dt.datetime.now().isoformat(timespec="seconds")

    # 1. Stop & configure Pico 2 W deterministically
    os.write(gen_fd, b"0")
    time.sleep(0.05)
    drain_fd(gen_fd, 0.08)

    cfg = ("r" + prof["len_cmd"] + prof["pat_cmd"] + prof["rate_cmd"]).encode()
    os.write(gen_fd, cfg)
    time.sleep(0.05)
    drain_fd(gen_fd, 0.12)

    # 2. Reset Sniffer DUT if connected
    if sniffer_fd is not None:
        sniffer_cmd(sniffer_fd, b"R", timeout_s=1.0)

    # 3. Start Wireshark dumpcap in background
    cap_proc = None
    cap_dur = int(prof["dur"]) + 2
    if enable_wireshark:
        if tmp_pcap.exists():
            tmp_pcap.unlink()
        cap_proc = start_dumpcap(bus, cap_dur, tmp_pcap)
        time.sleep(0.5)

    # 4. Trigger generation & drain Host cdc_acm
    if prof["mode"] == "burst":
        os.write(gen_fd, b"b" if prof["count"] == 100 else b"B")
        target = prof["count"]
        max_wait = prof["dur"] + 3.0
    else:
        os.write(gen_fd, b"s")
        target = None
        max_wait = prof["dur"]

    buf = bytearray()
    host_pkts = 0
    first_seq: Optional[int] = None
    last_seq: Optional[int] = None
    host_gaps = 0
    t_start = time.monotonic()
    t_last = t_start

    while True:
        now = time.monotonic()
        if target is None and (now - t_start) >= max_wait:
            os.write(gen_fd, b"0")
            # brief tail drain
            time.sleep(0.08)
            break
        if target is not None:
            if host_pkts >= target:
                break
            if (now - t_start) >= max_wait or (host_pkts > 0 and (now - t_last) > 1.5):
                os.write(gen_fd, b"0")
                break

        r, _, _ = select.select([gen_fd], [], [], 0.05)
        if r:
            try:
                chunk = os.read(gen_fd, 8192)
            except BlockingIOError:
                continue
            if not chunk:
                continue
            t_last = time.monotonic()
            buf.extend(chunk)
            idx = 0
            while idx + 8 <= len(buf):
                if buf[idx] == 0xA5 and buf[idx + 1] == 0x5A:
                    plen = buf[idx + 6]
                    if plen in (8, 16, 32, 64):
                        if idx + plen > len(buf):
                            break
                        seq = struct.unpack_from("<I", buf, idx + 2)[0]
                        if first_seq is None:
                            first_seq = seq
                        elif seq != last_seq + 1:
                            host_gaps += (seq - last_seq - 1) if seq > last_seq else 1
                        last_seq = seq
                        host_pkts += 1
                        idx += plen
                        continue
                idx += 1
            if idx > 0:
                del buf[:idx]

    elapsed = max(time.monotonic() - t_start, 1e-6)
    host_rate = host_pkts / elapsed
    throughput_kbps = (host_pkts * prof["len_b"]) / elapsed / 1024.0

    # 5. Query Sniffer DUT snapshot
    snif: Dict[str, Any] = {}
    if sniffer_fd is not None:
        time.sleep(0.15)  # allow last DMA buffer to drain
        raw_json = sniffer_cmd(sniffer_fd, b"S", timeout_s=1.5)
        try:
            snif = json.loads(raw_json)
        except Exception:
            snif = {"error": raw_json}

    # 6. Wait for dumpcap & analyze pcapng
    ws = {"ws_urbs": -1, "ws_pkts": -1, "ws_first": -1, "ws_last": -1, "ws_gaps": -1}
    if cap_proc is not None:
        cap_proc.wait(timeout=cap_dur + 10)
        if tmp_pcap.exists():
            try:
                os.chmod(tmp_pcap, 0o644)
            except OSError:
                pass
            ws = analyze_pcap(tmp_pcap, dev_addr)

    snif_seen = snif.get("gen_seen", -1)
    capture_pct = (100.0 * snif_seen / host_pkts) if (sniffer_fd is not None and host_pkts > 0 and snif_seen >= 0) else -1.0

    return {
        "timestamp": ts_iso,
        "round": round_idx,
        "profile": prof["id"],
        "rate_cfg": prof["rate_lbl"],
        "len_b": prof["len_b"],
        "pattern": prof["pat_lbl"],
        "elapsed_s": round(elapsed, 3),
        "host_pkts": host_pkts,
        "host_rate_pps": round(host_rate, 1),
        "throughput_kbs": round(throughput_kbps, 2),
        "host_gaps": host_gaps,
        "ws_pkts": ws["ws_pkts"],
        "ws_gaps": ws["ws_gaps"],
        "ws_urbs": ws["ws_urbs"],
        "snif_gen_seen": snif_seen,
        "snif_capture_pct": round(capture_pct, 2),
        "snif_gen_gaps": snif.get("gen_gaps", -1),
        "snif_gen_dups": snif.get("gen_dups", -1),
        "snif_gen_crc_fail": snif.get("gen_crc_fail", -1),
        "snif_total_decoded": snif.get("packets_decoded", -1),
        "snif_crc_err": snif.get("crc_errors", -1),
        "snif_sync_err": snif.get("sync_errors", -1),
        "snif_stuff_err": snif.get("stuffing_errors", -1),
        "snif_ovf_err": snif.get("overflow_errors", -1),
        "snif_fifo_ovf": snif.get("fifo_overruns", -1),
    }


# ── Markdown Report Generator ──────────────────────────────────────────────

def write_report(
    out_dir: Path,
    rows: List[Dict[str, Any]],
    start_time: dt.datetime,
    has_sniffer: bool,
    has_wireshark: bool,
    gen_port: str,
    sniffer_port: Optional[str],
    bus: int,
    dev_addr: int,
) -> None:
    now = dt.datetime.now()
    elapsed_h = (now - start_time).total_seconds() / 3600.0
    total_host_pkts = sum(r["host_pkts"] for r in rows)
    total_ws_pkts = sum(r["ws_pkts"] for r in rows if r["ws_pkts"] >= 0)
    total_snif_pkts = sum(r["snif_gen_seen"] for r in rows if r["snif_gen_seen"] >= 0)
    rounds_done = max((r["round"] for r in rows), default=0)

    by_prof: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
    for r in rows:
        by_prof[r["profile"]].append(r)

    lines: List[str] = []
    lines.append("# Relatório de Estresse e Benchmark — USBSnifferPIO_RP2040")
    lines.append("")
    lines.append("## Resumo da Execução")
    lines.append("")
    lines.append(f"- **Início:** {start_time.strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"- **Última atualização:** {now.strftime('%Y-%m-%d %H:%M:%S')} (**{elapsed_h:.2f} horas**)")
    lines.append(f"- **Rodadas completas:** {rounds_done} ({len(rows)} ensaios executados)")
    lines.append(f"- **Gerador (Pico 2 W):** `{gen_port}` (`usbmon{bus}`, `Dev {dev_addr}`)")
    lines.append(f"- **Wireshark (`usbmon`):** {'Ativo' if has_wireshark else 'Desativado'}")
    lines.append(f"- **Sniffer DUT (`USBSnifferPIO_RP2040`):** `{sniffer_port}`" if has_sniffer else "- **Sniffer DUT:** Não conectado (modo baseline Gerador + Wireshark)")
    lines.append(f"- **Total de pacotes gerados (Host):** `{total_host_pkts:,}`")
    if has_wireshark:
        lines.append(f"- **Total de pacotes capturados (Wireshark):** `{total_ws_pkts:,}`")
    if has_sniffer:
        overall_pct = (100.0 * total_snif_pkts / total_host_pkts) if total_host_pkts else 0.0
        lines.append(f"- **Total de pacotes do gerador decodificados (Sniffer):** `{total_snif_pkts:,}` (**{overall_pct:.2f}%**)")
    lines.append("")

    lines.append("## Resultados Agregados por Perfil")
    lines.append("")
    if has_sniffer:
        lines.append("| Perfil | Ensaios | Taxa Média (pkt/s) | Host Pkts (Perdas) | Wireshark Pkts (Perdas) | Sniffer Pkts | Taxa Captura Sniffer | CRC / SYNC / Stuff / Ovf |")
        lines.append("| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    else:
        lines.append("| Perfil | Ensaios | Taxa Média (pkt/s) | Throughput (KB/s) | Host Pkts (Perdas) | Wireshark Pkts (Perdas) |")
        lines.append("| :--- | ---: | ---: | ---: | ---: | ---: |")

    for prof in TEST_PROFILES:
        pid = prof["id"]
        pr = by_prof.get(pid, [])
        if not pr:
            continue
        n = len(pr)
        avg_rate = sum(x["host_rate_pps"] for x in pr) / n
        avg_kbps = sum(x["throughput_kbs"] for x in pr) / n
        h_pkts = sum(x["host_pkts"] for x in pr)
        h_gaps = sum(x["host_gaps"] for x in pr)
        w_pkts = sum(x["ws_pkts"] for x in pr if x["ws_pkts"] >= 0)
        w_gaps = sum(x["ws_gaps"] for x in pr if x["ws_gaps"] >= 0)

        if has_sniffer:
            s_pkts = sum(x["snif_gen_seen"] for x in pr if x["snif_gen_seen"] >= 0)
            pct = (100.0 * s_pkts / h_pkts) if h_pkts else 0.0
            c_err = sum(x["snif_crc_err"] for x in pr if x["snif_crc_err"] >= 0)
            y_err = sum(x["snif_sync_err"] for x in pr if x["snif_sync_err"] >= 0)
            t_err = sum(x["snif_stuff_err"] for x in pr if x["snif_stuff_err"] >= 0)
            o_err = sum(x["snif_ovf_err"] for x in pr if x["snif_ovf_err"] >= 0)
            lines.append(
                f"| `{pid}` | {n} | {avg_rate:,.1f} | {h_pkts:,} ({h_gaps}) | {w_pkts:,} ({w_gaps}) | "
                f"{s_pkts:,} | **{pct:.2f}%** | {c_err} / {y_err} / {t_err} / {o_err} |"
            )
        else:
            lines.append(
                f"| `{pid}` | {n} | {avg_rate:,.1f} | {avg_kbps:,.1f} | {h_pkts:,} ({h_gaps}) | {w_pkts:,} ({w_gaps}) |"
            )

    lines.append("")
    lines.append("## Arquivos de Dados Brutos")
    lines.append("")
    lines.append("- `results.csv` — série temporal completa (1 linha por ensaio)")
    lines.append("- `results.jsonl` — log estruturado em JSON Lines")
    lines.append("- `fail_pcaps/` — capturas `.pcapng` preservadas automaticamente quando há anomalia (máx. 20)")
    lines.append("")

    (out_dir / "REPORT.md").write_text("\n".join(lines), encoding="utf-8")


# ── Main Orchestrator ──────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="8-Hour Automated USB Sniffer Stress & Benchmark Suite")
    parser.add_argument("--hours", type=float, default=8.0, help="Total stress duration in hours (default: 8.0)")
    parser.add_argument("--rounds", type=int, default=0, help="If >0, stop after N rounds instead of --hours")
    parser.add_argument("--gen", default=None, help="Pico 2 W serial port (auto-detected if omitted)")
    parser.add_argument("--sniffer", default=None, help="USBSnifferPIO_RP2040 DUT serial port (optional)")
    parser.add_argument("--no-wireshark", action="store_true", help="Disable dumpcap/tshark capture")
    parser.add_argument("--cooldown", type=float, default=5.0, help="Cooldown seconds between rounds (default: 5)")
    parser.add_argument("--max-fail-pcaps", type=int, default=20, help="Max anomaly .pcapng files to keep")
    args = parser.parse_args()

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)

    gen_port = args.gen or find_pico2w_port()
    if not gen_port:
        print("ERRO: Pico 2 W não encontrado em /dev/serial/by-id/. Informe com --gen /dev/ttyACM2.")
        sys.exit(1)

    bus, dev_addr = find_pico2w_usbmon()
    enable_ws = not args.no_wireshark

    base_dir = Path(__file__).resolve().parent / "results"
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = base_dir / f"stress_{stamp}"
    fail_pcap_dir = out_dir / "fail_pcaps"
    out_dir.mkdir(parents=True, exist_ok=True)
    fail_pcap_dir.mkdir(parents=True, exist_ok=True)
    tmp_pcap = out_dir / "_current.pcapng"

    print("╔══════════════════════════════════════════════════════════════╗")
    print("║   USBSnifferPIO_RP2040 — Suite de Estresse Contínuo (8h)     ║")
    print("╚══════════════════════════════════════════════════════════════╝")
    print(f"  Gerador (Pico 2 W) : {gen_port} (Bus {bus}, Dev {dev_addr})")
    print(f"  Sniffer DUT        : {args.sniffer or 'NÃO CONECTADO (baseline Host + Wireshark)'}")
    print(f"  Wireshark (usbmon) : {'ATIVO' if enable_ws else 'DESATIVADO'}")
    print(f"  Meta               : {f'{args.rounds} rodada(s)' if args.rounds > 0 else f'{args.hours:.2f} horas'}")
    print(f"  Saída              : {out_dir}\n", flush=True)

    gen_fd = open_serial_raw(gen_port)
    sniffer_fd = None
    if args.sniffer:
        sniffer_fd = open_serial_raw(args.sniffer)
        pong = sniffer_cmd(sniffer_fd, b"P", timeout_s=1.5)
        if "PONG" not in pong:
            print(f"AVISO: Sniffer em {args.sniffer} não respondeu PONG (recebido: {pong!r}). Continuando mesmo assim...")
        else:
            print(f"  [OK] Sniffer DUT respondeu PONG em {args.sniffer}", flush=True)

    csv_path = out_dir / "results.csv"
    jsonl_path = out_dir / "results.jsonl"
    fieldnames = [
        "timestamp", "round", "profile", "rate_cfg", "len_b", "pattern",
        "elapsed_s", "host_pkts", "host_rate_pps", "throughput_kbs", "host_gaps",
        "ws_pkts", "ws_gaps", "ws_urbs",
        "snif_gen_seen", "snif_capture_pct", "snif_gen_gaps", "snif_gen_dups",
        "snif_gen_crc_fail", "snif_total_decoded",
        "snif_crc_err", "snif_sync_err", "snif_stuff_err", "snif_ovf_err", "snif_fifo_ovf",
    ]

    rows: List[Dict[str, Any]] = []
    start_dt = dt.datetime.now()
    deadline = time.monotonic() + args.hours * 3600.0
    saved_fail_pcaps = 0
    round_idx = 0

    try:
        with open(csv_path, "w", newline="", encoding="utf-8") as csv_f, \
             open(jsonl_path, "w", encoding="utf-8") as jsonl_f:
            writer = csv.DictWriter(csv_f, fieldnames=fieldnames)
            writer.writeheader()
            csv_f.flush()

            while not STOP_REQUESTED:
                if args.rounds > 0 and round_idx >= args.rounds:
                    break
                if args.rounds == 0 and time.monotonic() >= deadline:
                    break

                round_idx += 1
                print(f"\n── Rodada {round_idx} ({dt.datetime.now().strftime('%H:%M:%S')}) ──", flush=True)

                for prof in TEST_PROFILES:
                    if STOP_REQUESTED:
                        break
                    try:
                        row = run_profile(
                            gen_fd, sniffer_fd, bus, dev_addr,
                            prof, round_idx, tmp_pcap, enable_ws,
                        )
                    except Exception as exc:
                        print(f"  [ERRO] {prof['id']}: {exc}", flush=True)
                        continue

                    rows.append(row)
                    writer.writerow(row)
                    csv_f.flush()
                    jsonl_f.write(json.dumps(row) + "\n")
                    jsonl_f.flush()

                    # Save anomaly pcap if unexpected host/ws drop (capped)
                    anomaly = (row["host_gaps"] > 0) or (enable_ws and row["ws_gaps"] > 0)
                    if enable_ws and anomaly and saved_fail_pcaps < args.max_fail_pcaps and tmp_pcap.exists():
                        dest = fail_pcap_dir / f"r{round_idx:04d}_{prof['id']}.pcapng"
                        shutil.copy2(tmp_pcap, dest)
                        saved_fail_pcaps += 1

                    snif_str = (f" | Snif={row['snif_gen_seen']} ({row['snif_capture_pct']:.1f}%, "
                                f"crc={row['snif_crc_err']} ovf={row['snif_ovf_err']} fifo={row['snif_fifo_ovf']})") if sniffer_fd is not None else ""
                    ws_str = f" | WS={row['ws_pkts']} (gaps={row['ws_gaps']})" if enable_ws else ""
                    print(
                        f"  {prof['id']:18s} | Host={row['host_pkts']:6d} ({row['host_rate_pps']:7.1f}/s, "
                        f"gaps={row['host_gaps']}){ws_str}{snif_str}",
                        flush=True,
                    )

                write_report(out_dir, rows, start_dt, sniffer_fd is not None, enable_ws, gen_port, args.sniffer, bus, dev_addr)

                if not STOP_REQUESTED and args.cooldown > 0:
                    if (args.rounds == 0 and time.monotonic() < deadline) or (args.rounds > 0 and round_idx < args.rounds):
                        time.sleep(args.cooldown)
    finally:
        if tmp_pcap.exists():
            try:
                tmp_pcap.unlink()
            except OSError:
                pass
        os.close(gen_fd)
        if sniffer_fd is not None:
            os.close(sniffer_fd)
        write_report(out_dir, rows, start_dt, sniffer_fd is not None, enable_ws, gen_port, args.sniffer, bus, dev_addr)
        print(f"\n=== Concluído! Relatório salvo em: {out_dir / 'REPORT.md'} ===", flush=True)


if __name__ == "__main__":
    main()
