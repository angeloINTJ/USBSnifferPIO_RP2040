#!/usr/bin/env bash
# Executa benchmark sequencial de 1 rodada em 240 MHz e 250 MHz (-O3) no Pico RP2040.
# Deve ser executado após a conclusão do teste de 8 horas.
set -euo pipefail

SNIF_PORT="${1:-/dev/ttyACM0}"
OUT_DIR="$(cd "$(dirname "$0")" && pwd)/results"
STAMP="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="$OUT_DIR/clock_sweep_${STAMP}.log"

mkdir -p "$OUT_DIR"

echo "=== Clock Sweep Sequencial (240 MHz -> 250 MHz) ===" | tee "$LOG_FILE"
echo "Sniffer port: $SNIF_PORT | Log: $LOG_FILE" | tee -a "$LOG_FILE"

for FREQ in 240 250; do
    FQBN="rp2040:rp2040:rpipico:freq=${FREQ},opt=Optimize3"
    echo "" | tee -a "$LOG_FILE"
    echo "============================================================" | tee -a "$LOG_FILE"
    echo ">>> Testando ${FREQ} MHz (-O3) — FQBN: ${FQBN}" | tee -a "$LOG_FILE"
    echo "============================================================" | tee -a "$LOG_FILE"

    arduino-cli compile --fqbn "$FQBN" --library . tools/test_bench/sniffer_bench_dut 2>&1 | tail -n 5 | tee -a "$LOG_FILE"
    arduino-cli upload  --fqbn "$FQBN" -p "$SNIF_PORT" tools/test_bench/sniffer_bench_dut 2>&1 | tee -a "$LOG_FILE"
    sleep 2
    python3 -u tools/test_bench/stress_8h.py --rounds 1 --sniffer "$SNIF_PORT" 2>&1 | tee -a "$LOG_FILE"
done

echo "" | tee -a "$LOG_FILE"
echo "=== Clock Sweep Concluído! Log salvo em: $LOG_FILE ===" | tee -a "$LOG_FILE"
