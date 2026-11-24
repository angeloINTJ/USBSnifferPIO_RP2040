# Log de Otimização — USBSnifferPIO_RP2040 (Full-Speed 12 Mbps)

Documentação técnica completa das iterações de otimização validadas em bancada real (**Pico 2 W gerador Full-Speed** × **Wireshark `usbmon3` gabarito** × **Pico RP2040 rodando `USBSnifferPIO_RP2040`**).

---

## Bancada de Validação

- **Gerador:** Raspberry Pi Pico 2 W (`/dev/ttyACM1`, USB 1.1 Full-Speed 12 Mbps, `pico2w_usb_generator.ino`)
- **Gabarito:** Linux `usbmon3` + `dumpcap` / `tshark` (0% de perda em todos os ensaios)
- **DUT:** Raspberry Pi Pico RP2040 (`/dev/ttyACM0`, `sniffer_bench_dut.ino`), tap passivo `D+`/`D-` via 100 Ω em `GP2`/`GP3` + `GND`
- **Matriz de 8 perfis por rodada:** `10`, `100`, `500`, `1.000`, `5.000 pkt/s` (`16 B INC`), `1.000 pkt/s` (`64 B 0xFF` estresse de *bit-stuffing*), `1.000 pkt/s` (`64 B 0x00` máximo de transições NRZI) e `MAX` (`64 B INC`, ~9.850 pkt/s / ~615 KB/s).

---

## Evolução Medida (Iteração por Iteração)

| Iteração | Descrição Técnica | Clock / Opt | `P1` (10 Hz) | `P4` (1 kHz) | `P5` (5 kHz) | `P6` (64B `0xFF`) | `P8_MAX` (~10 kHz) | CRC Err (`P1..P7`) |
| :--- | :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Baseline v1.0.0** | PIO 1-instr (`in pins, 2` 8× bruto), DMA manual 1 canal, `USB_MAX_PACKET_BITS=600` | 200 MHz `-Os` | 1,0% | 1,6% | 1,9% | **0,0%** *(ovf=1652)* | 1,6% | 13 |
| **Opt #1** | **DPLL 8×→1× em hardware no PIO** (22 instr.) + decodificador inline + limite `680 bits` + `fifo_overruns` | 200 MHz `-Os` | 34,0% | 34,3% | 34,1% | 35,8% | 28,1% | 0 |
| **Opt #2** | `feedSamples`/`finalizePacket`/`onPacket` + LUTs CRC5/CRC16 em **SRAM** (`.time_critical`) com `-O3` + eliminação de tradução `J`/`K` | 200 MHz `-Os` | 62,0% | 66,2% | 60,8% | 62,7% | 46,1% | 0 |
| **Opt #3** | Anel DMA 4 buffers via `DMA_IRQ_1` + loops `while` aninhados por estado *(regressão por spill de registradores no M0+)* | 200 MHz `-Os` | 52,0% | 50,8% | 50,0% | 49,3% | 39,6% | 0 |
| **Opt #4** | Loop único + **empacotador bit-sentinela (`acc = 0x80`)** + abort imediato de erro + 4-buf `DMA_IRQ_1` | 200 MHz `-Os` | 73,0% | 72,8% | 67,4% | 69,4% | 50,9% | 0 |
| **Opt #5** | Mesmo código da Opt #4 compilado com `276 MHz` + `-O3` | 276 MHz `-O3` | 87,0% | 87,0% | 83,6% | 84,7% | 72,2% | 0 |
| **Opt #6** | Tentativa `ALL_J_HALF` (skip 8 símbolos) em 276 MHz *(regressão de ~6% por branch extra)*; 300 MHz instável no silício | 276 MHz `-O3` | 81,0% | 79,7% | 77,0% | 79,2% | 69,7% | 0 |
| **Opt #7a** | **DMA Ping-Pong real em hardware de 2 canais (`ch0 ↔ ch1`, gap = 0 ns)** com recarga segura de `write_addr` em `DMA_IRQ_1` | 200 MHz `-O3` | **100,0%** | 79,7% | 76,9% | 79,4% | 64,0% | 0 |
| **Opt #7b** | Mesmo código da Opt #7a em overclock leve (divisor exato `240/96 = 2,5`) | 240 MHz `-O3` | 97,0% | 96,0% | 91,8% | 93,9% | 76,8% | 0 |
| **Opt #7c** | Mesmo código da Opt #7a em overclock conservador | 250 MHz `-O3` | **100,0%** | **100,0%** | 97,1% | 91,8% | 80,0% | 0–1 |
| **Opt #7d (ÓTIMO — 8h)** | **DMA Ping-Pong real de 2 canais (`ch0 ↔ ch1`)** + DPLL PIO 22-instr + SRAM `-O3` + `acc=0x80` (**validado por 8,01 h / 420 rodadas / 15,32M pacotes**) | **276 MHz `-O3`** | **100,00%** | **100,00%** | **100,00%** | **100,00%** | **88,43%** | **0 (`fifo=0`!)** |

---

## Resultado Consolidado da Maratona de 8 Horas (`276 MHz -O3`)

- **Duração:** `8,01 horas` (`2026-11-23 22:56:36` → `2026-11-24 06:57:09`)
- **Rodadas / Ensaios:** `420` rodadas completas (`3.360` ensaios)
- **Total gerado (Host):** `15.320.290` pacotes (`0` perdas) | **Wireshark (`usbmon3`):** `15.322.661` (`0` perdas)
- **Perfis `P1..P7` (`10` a `5.000 pkt/s`, incluindo `64B 0xFF` e `64B 0x00`):** **`2.562.000 / 2.562.000` (`100,0000%`)**, `0` erros de CRC, `0` erros de stuffing, `0` overflows, `0` FIFO overruns.
- **Perfil `P8_MAX` (~9.862 pkt/s, `64B`):** **`11.282.309 / 12.758.290` (`88,43%`)** estáveis do minuto 1 até a hora 8 (zero deriva térmica).
- **Relatório completo:** `results/stress_20261123_225636/REPORT.md`
- **Sweep de clock `240/250 MHz`:** `results/clock_sweep_20261124_065725.log`

---

## Fase 2 — Dual-Core Paralelo e Mapeamento Completo de Limites (`v1.2.0`)

### Experimento A — Filtro Antecipado de PID (`NO_POLLING`: ignora `SOF`/`NAK`/`IN` no byte 0)
- **Resultado (`200 MHz` Single-Core):** caiu de `79,7%` → **`72,3%`** (`P8_MAX`: `64,0%` → `59,6%`).
- **Conclusão:** o gargalo em `200 MHz` single-core não é `finalizePacket()`, e sim o loop bit-a-bit a 12 Mbps; adicionar um 4º estado (`SKIP_TO_EOP`) no loop quente gerou *spill* de registradores no Cortex-M0+. Revertido.

### Experimento B — Dual-Core Paralelo (`enableDualCore(true)` + `taskCore0()`, SEM filtro)
Anel de 4 buffers DMA rotacionado com 0 ns de gap pelos 2 canais hardware (`ch0`: pares `0, 2`; `ch1`: ímpares `1, 3`), com Core 1 decodificando pares e Core 0 decodificando ímpares com sobreposição exata de fronteira (`SNIFFER_DUALCORE_TAIL_WORDS = 48`):

| Configuração Dual-Core (`-Os` padrão, zero OC) | `10 Hz` | `100–1k Hz` | `5 kHz` | `1 kHz` (`64B 0xFF`) | `1 kHz` (`64B 0x00`) | `MAX` (~10k/s, 64B) | `crc` / `ovf` / `fifo` (`P8_MAX`) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **`120 MHz`** (`2048w`, 48 KB RAM) | 98,0% | 94,6–96,0% | 89,5% | 91,6% | 93,3% | 69,6% | 13 / 65 / 1627 |
| **`133 MHz`** (`2048w`, 48 KB RAM) | **100,0%** | 99,9–100,0% | **100,0%** | **100,0%** | 99,9% | 77,1% | 8 / 37 / 1424 |
| **`150 MHz`** (`2048w`, 48 KB RAM) | **100,0%** | 99,9–100,0% | 99,7% | **100,0%** | **100,0%** | 87,2% | 9 / 52 / 1454 |
| **`176 MHz`** (`2048w`, 48 KB RAM) | **100,0%** | **100,0%** | **100,0%** | 99,9% | **100,0%** | **100,0%** | **0 / 0 / 90** |
| **`200 MHz` Low-RAM** (`1024w`, 31 KB RAM) | **100,0%** | **100,0%** | **100,0%** | **100,0%** | **100,0%** | **100,0%** | **0 / 0 / 0** |
| **`200 MHz` Padrão** (`2048w`, 48 KB RAM) | **100,0%** | **100,0%** | **100,0%** | **100,0%** | **100,0%** | **100,0%** *(30.371/30.372)* | **0 / 0 / 0** |

**Conclusão:** O modo Dual-Core elimina totalmente a necessidade de overclock — atingindo **100,0% até na saturação Full-Speed (`~10.000 pkt/s`, `64B`)** no clock oficial de **`200 MHz` (`-Os` padrão da IDE)**, inclusive no modo **Low-RAM (`SNIFFER_DMA_BUF_WORDS=1024`, apenas 16 KB de DMA RAM)**!

---

## Principais Causas-Raiz Resolvidas

1. **Banda impossível em 8× bruto no Cortex-M0+ (1,6% → 35,8%):**
   Em Full-Speed (12 Mbps), 8× bruto gera 6 milhões de words/s (0,34 ms por buffer de 2048 words). Mover a recuperação de clock (DPLL 8× por borda em `D+`) para **22 instruções no PIO** ([src/usb_sniffer.pio](../../src/usb_sniffer.pio)) reduziu a banda DMA/CPU em **8×** (buffer passou para `2,73 ms`).
2. **Bug de overflow em `64B 0xFF` (0,0% → 100,0%):**
   `USB_MAX_PACKET_BITS = 600` truncava 100% dos pacotes de 64 bytes com muitos bits `1` (pior caso com *bit-stuffing* = 625 bits). Aumentado para `680 bits` / `72 bytes`.
3. **Latência XIP Flash e pressão de registradores no Cortex-M0+ (35,8% → 73,6% em 200 MHz):**
   Mover o loop quente + tabelas LUT de CRC5/CRC16/PID para SRAM (`.time_critical` com `-O3`), usar empacotador por bit-sentinela (`acc = 0x80`) e eliminar a tradução `J`/`K` dentro de `SYNC` e `PACKET_BODY` cortou o custo por bit em mais de 2×.
4. **Gap de DMA entre buffers (87,4% → 100,0% com `fifo=0`):**
   Com 1 canal DMA rearmado na ISR, a latência da ISR compartilhada estourava o FIFO RX de 8 words (10,67 µs). O **Ping-Pong real em hardware de 2 canais (`ch0 ↔ ch1`)** elimina o gap (`0 ns`) e dá à `DMA_IRQ_1` uma margem de **2,73 ms** inteiros para recarregar `write_addr` e `transfer_count` do canal inativo — sem a race condition de `WRITE_ADDR`.

---

## Resultado Final da Maratona de 8 Horas (`276 MHz -O3`) + Sweep de Clocks (`200 / 240 / 250 / 276 MHz`)

- **Maratona 8h (`stress_20261123_225636`):** `8,01 horas`, `420` rodadas (`3.360` ensaios), **`15.320.290` pacotes gerados** (`0` perdas no Host / Wireshark, `0` anomalias em `fail_pcaps/`).
  - **`P1..P7` (`10 Hz` a `5.000 pkt/s`, incluindo `64B 0xFF` e `64B 0x00`):** **`2.562.000 / 2.562.000` (100,00%)**, `0` erros de CRC, `0` erros de stuffing, `0` overflows, `0` FIFO overruns.
  - **`P8_MAX` (`~9.861,7 pkt/s`, `64B`):** **`11.282.309 / 12.758.290` (88,43%)**, sem degradação térmica ao longo das 8 horas.

### Curva Completa por Frequência (`v1.1.0` — DMA Ping-Pong 2-ch + PIO DPLL)

| Clock | `P1` (10 Hz) | `P2–P4` (100–1k Hz) | `P5` (5 kHz) | `P6` (64B `0xFF`) | `P7` (64B `0x00`) | `P8_MAX` (~9.86 kHz) | Recomendação |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| **`200 MHz`** (oficial) | **100,0%** | 79,5–80,8% | 76,9% | 79,4% | 78,0% | 64,0% | Padrão oficial (0 overclock) |
| **`240 MHz`** (`-O3`) | 97,0% | 96,0–97,0% | 91,8% | 93,9% | 95,0% | 76,8% | Overclock leve (divisor PIO 2,5 exato) |
| **`250 MHz`** (`-O3`) | **100,0%** | **100,0%** | 97,1% | 91,8% | 98,5% | 80,0% | **Overclock conservador (100% até 1k pkt/s)** |
| **`276 MHz`** (`-O3`, 8h) | **100,00%** | **100,00%** | **100,00%** | **100,00%** | **100,00%** | **88,43%** | Máximo estável (sujeito a *silicon lottery*) |
| `300 MHz` | — | — | — | — | — | — | Instável (não enumera USB) |
