# Bancada de Teste e Estresse (8h) — Pico 2 W + Wireshark + USBSnifferPIO_RP2040

Suite completa para gerar tráfego USB 1.1 Full-Speed calibrado no **Pico 2 W**, capturar o gabarito via **Wireshark (`usbmon`)** e medir a taxa de captura real do **USBSnifferPIO_RP2040** por até **8 horas contínuas**.

## Arquivos

| Arquivo | Função |
| :--- | :--- |
| `pico2w_usb_generator/pico2w_usb_generator.ino` | Firmware gerador de pacotes binários numerados (`0xA5 0x5A` + `seq`) para Pico 2 W |
| `sniffer_bench_dut/sniffer_bench_dut.ino` | Firmware DUT de benchmark (sem gargalo de `Serial`) para o RP2040 rodando `USBSnifferPIO_RP2040` |
| `stress_8h.py` | Orquestrador automático de estresse contínuo (8 perfis por rodada, gera `REPORT.md`, `results.csv` e `results.jsonl`) |
| `setup_wireshark.sh` | Instala `wireshark`/`tshark`, carrega `usbmon` e configura permissões (`0660`) |
| `capture_wireshark.sh` | Captura pontual no `usbmon` com `dumpcap` + análise com `tshark` |
| `usb_bench.py` | Controle pontual do Pico 2 W via terminal |

## Passo a Passo para o Teste de 8 Horas

### 1. Gravar o gerador atualizado no Pico 2 W (`/dev/ttyACM2`)
```bash
arduino-cli compile --fqbn rp2040:rp2040:rpipico2w tools/test_bench/pico2w_usb_generator
arduino-cli upload  --fqbn rp2040:rp2040:rpipico2w -p /dev/ttyACM2 tools/test_bench/pico2w_usb_generator
```

### 2. Gravar o firmware de benchmark no Sniffer RP2040 (`/dev/ttyACM0`)
```bash
arduino-cli compile --fqbn rp2040:rp2040:rpipico --library . tools/test_bench/sniffer_bench_dut
arduino-cli upload  --fqbn rp2040:rp2040:rpipico -p /dev/ttyACM0 tools/test_bench/sniffer_bench_dut
```

### 3. Ligar o tap físico no cabo do Pico 2 W
```text
Cabo USB do Pico 2 W       Sniffer RP2040 (Pico)
────────────────────       ─────────────────────
D+ (verde)  ──[100 Ω]────► GP2
D- (branco) ──[100 Ω]────► GP3
GND (preto) ─────────────► GND
(NÃO ligar VBUS!)
```

### 4. Validar rapidamente (1 rodada, ~45 s) antes das 8 horas
```bash
python3 tools/test_bench/stress_8h.py --rounds 1 --sniffer /dev/ttyACM0
```

### 5. Disparar a maratona de 8 horas (com `nohup` / `tmux` se quiser desconectar)
```bash
python3 tools/test_bench/stress_8h.py --hours 8 --sniffer /dev/ttyACM0
```
*(Sem o sniffer plugado ainda, basta omitir `--sniffer /dev/ttyACM0` para rodar o baseline Gerador + Wireshark.)*

### Relatórios Gerados
A cada rodada concluída, o script atualiza automaticamente em `tools/test_bench/results/stress_<DATA_HORA>/`:
- `REPORT.md` — relatório consolidado em Markdown (pronto para anexar à Discussion #2)
- `results.csv` — série temporal completa para gráficos
- `results.jsonl` — log estruturado completo
- `fail_pcaps/` — até 20 capturas `.pcapng` preservadas automaticamente caso ocorra anomalia no Host/Wireshark
