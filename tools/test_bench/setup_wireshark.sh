#!/usr/bin/env bash
# Instala Wireshark + tshark e configura captura USB (usbmon) sem precisar rodar GUI como root.
set -euo pipefail

echo "=== 1. Instalando Wireshark e tshark ==="
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y wireshark tshark

echo "=== 2. Habilitando captura para usuários do grupo 'wireshark' ==="
echo "wireshark-common wireshark-common/install-setuid boolean true" | sudo debconf-set-selections
sudo dpkg-reconfigure -f noninteractive wireshark-common
sudo usermod -aG wireshark "$USER"

echo "=== 3. Carregando módulo usbmon e criando regra udev ==="
sudo modprobe usbmon

# Carregar usbmon automaticamente no boot
echo "usbmon" | sudo tee /etc/modules-load.d/usbmon.conf >/dev/null

# Permitir leitura/escrita (O_RDWR exigido pelo libpcap/usbmon) para o grupo wireshark
echo 'SUBSYSTEM=="usbmon", GROUP="wireshark", MODE="0660"' \
    | sudo tee /etc/udev/rules.d/99-usbmon.rules >/dev/null

sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usbmon
sudo chgrp wireshark /dev/usbmon* 2>/dev/null || true
sudo chmod 660 /dev/usbmon* 2>/dev/null || true

echo ""
echo "=== Instalação concluída! ==="
echo "Versões instaladas:"
wireshark --version | head -n 1
tshark --version | head -n 1
echo ""
echo "Interfaces usbmon disponíveis:"
ls -l /dev/usbmon*
echo ""
echo "IMPORTANTE: Para usar wireshark/tshark sem 'sudo' neste terminal, execute:"
echo "  newgrp wireshark"
echo "(ou faça logout/login da sessão)."
