#!/usr/bin/env bash
# Captura tráfego USB do Pico 2 W com tshark (usbmon), salva .pcapng para abrir no Wireshark
# e valida a continuidade dos pacotes binários (header 0xA5 0x5A + seq).
set -euo pipefail

DURATION="${1:-10}"
OUT_DIR="$(cd "$(dirname "$0")" && pwd)/captures"
mkdir -p "$OUT_DIR"
STAMP="$(date +%Y%m%d_%H%M%S)"
PCAP_FILE="$OUT_DIR/pico2w_${STAMP}.pcapng"

# Verifica tshark e usbmon
if ! command -v tshark >/dev/null 2>&1; then
    echo "ERRO: tshark não encontrado. Rode primeiro: ./extras/test_bench/setup_wireshark.sh"
    exit 1
fi
if [ ! -e /dev/usbmon0 ]; then
    echo "Carregando módulo usbmon..."
    sudo modprobe usbmon
fi

# Permite informar BUS e DEV manualmente via env: BUS=3 DEV=11 ./capture_wireshark.sh 10
if [ -z "${BUS:-}" ] || [ -z "${DEV:-}" ]; then
    echo "Dispositivos Raspberry Pi encontrados via lsusb:"
    lsusb | grep -i "2e8a" || true
    echo ""
    # Prioriza Pico 2W (2e8a:f00f), depois qualquer outro 2e8a
    LINE="$(lsusb | grep -i "2e8a:f00f" | head -n 1 || true)"
    if [ -z "$LINE" ]; then
        LINE="$(lsusb | grep -i "2e8a" | tail -n 1 || true)"
    fi
    if [ -z "$LINE" ]; then
        echo "ERRO: Nenhum dispositivo Raspberry Pi (VID 2e8a) encontrado."
        exit 1
    fi
    BUS="$(echo "$LINE" | awk '{print $2}' | sed 's/^0*//')"
    DEV="$(echo "$LINE" | awk '{print $4}' | tr -d ':' | sed 's/^0*//')"
    echo "Selecionado automaticamente: Bus $BUS, Device $DEV ($LINE)"
    echo "(Para escolher outro, rode: BUS=3 DEV=XX $0 $DURATION)"
fi

IFACE="usbmon${BUS}"
PICO2W_PORT="$(readlink -f /dev/serial/by-id/usb-Raspberry_Pi_Pico_2W_* 2>/dev/null | head -n 1 || echo /dev/ttyACM2)"
echo ""
echo "=== Capturando na interface ${IFACE} (Dev ${DEV}) por ${DURATION}s ==="
echo "Arquivo .pcapng: ${PCAP_FILE}"
echo ">> Agora dispare o burst/envio no Pico 2 W:"
echo "   python3 extras/test_bench/usb_bench.py ${PICO2W_PORT} --reset-seq --rate 2 --burst 100"
echo ""

# libpcap/dumpcap precisa de leitura+escrita (0660) em /dev/usbmon*
if [ -r "/dev/${IFACE}" ] && [ -w "/dev/${IFACE}" ]; then
    dumpcap -i "$IFACE" -a "duration:${DURATION}" -w "$PCAP_FILE"
elif id -nG "$USER" | grep -qw wireshark; then
    # Usuário já pertence ao grupo wireshark, mas esta sessão ainda não recarregou grupos
    sg wireshark -c "dumpcap -i '$IFACE' -a 'duration:${DURATION}' -w '$PCAP_FILE'"
else
    echo "ERRO: Sem permissão rw em /dev/${IFACE}. Rode: sudo chmod 660 /dev/usbmon* && newgrp wireshark"
    exit 1
fi

chmod 644 "$PCAP_FILE" 2>/dev/null || true

echo ""
echo "=== Analisando captura (${PCAP_FILE}) ==="
python3 - "$PCAP_FILE" "$DEV" <<'PYEOF'
import subprocess, sys, struct

pcap_file = sys.argv[1]
dev_addr = sys.argv[2]

cmd = [
    "tshark", "-r", "-",
    "-Y", f"usb.device_address == {dev_addr} && usb.endpoint_address.direction == 1 && usb.data_len > 0",
    "-T", "fields",
    "-e", "frame.time_relative",
    "-e", "usbcom.data.in_payload",
    "-e", "usb.capdata",
]
with open(pcap_file, "rb") as f:
    res = subprocess.run(cmd, stdin=f, capture_output=True, text=True)

if res.returncode != 0:
    print("Erro ao executar tshark:", res.stderr)
    sys.exit(1)

seqs = []
lengths = set()
total_urbs = 0

for line in res.stdout.splitlines():
    parts = line.split("\t")
    if len(parts) < 2:
        continue
    hex_payload = (parts[1] if len(parts) > 1 and parts[1] else
                   (parts[2] if len(parts) > 2 else "")).strip()
    if not hex_payload:
        continue
    total_urbs += 1
    raw = bytes.fromhex(hex_payload.replace(":", ""))
    # Pode haver 1 ou mais pacotes binários 0xA5 0x5A no mesmo URB
    i = 0
    while i + 8 <= len(raw):
        if raw[i] == 0xA5 and raw[i + 1] == 0x5A:
            seq = struct.unpack_from("<I", raw, i + 2)[0]
            plen = raw[i + 6]
            if plen in (8, 16, 32, 64) and i + plen <= len(raw):
                seqs.append(seq)
                lengths.add(plen)
                i += plen
                continue
        i += 1

print(f"URBs Bulk IN com payload (Dev {dev_addr}): {total_urbs}")
print(f"Pacotes do gerador (0xA5 0x5A) decodificados: {len(seqs)}")
if seqs:
    expected = seqs[-1] - seqs[0] + 1
    gaps = 0
    for a, b in zip(seqs, seqs[1:]):
        if b != a + 1:
            gaps += (b - a - 1) if b > a else 1
    print(f"Primeiro seq: {seqs[0]} | Último seq: {seqs[-1]} | Esperados: {expected}")
    print(f"Tamanhos observados: {sorted(lengths)} bytes")
    print(f"Saltos/perdas detectados no Wireshark: {gaps}")
else:
    print("Nenhum pacote 0xA5 0x5A encontrado. Verifique se o burst foi disparado durante a captura.")
PYEOF

echo ""
echo "Para abrir no Wireshark gráfico:"
echo "  wireshark -Y \"usb.device_address == ${DEV} && usb.data_len > 0\" \"${PCAP_FILE}\""
