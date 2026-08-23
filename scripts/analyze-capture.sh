#!/bin/bash
# Extrai os pares request/response da captura e calcula a latência.
set -euo pipefail

CAPTURES="${CAPTURES:-build/captures}"
PCAP="${1:-$CAPTURES/fleet.pcapng}"

# TODO(joao):
#
#   1. extrair com tshark: tempo + os bytes que distinguem request, response e
#      número de sequência.  Algo como:
#        tshark -r "$PCAP" -T fields -e frame.time_epoch -e data.data
#      e recortar os campos por offset — o payload UDP contém o frame Ethernet
#      inteiro, então o seu cabeçalho começa depois dele.
#
#   2. parear por número de sequência, descartar as amostras de warm-up.
#
#   3. calcular count, mean, min, max e um percentil (p95 ou p99).
#
#   4. IMPRIMIR O RÓTULO CERTO.  Se você mediu request→response, é round-trip.
#      Chamar isso de "latência de via única" sem dividir e sem justificar é
#      exatamente o tipo de coisa que o Fröhlich pergunta na banca.
#
#   5. Dizer, junto do número, que a bancada roda em QEMU TCG (esta máquina não
#      tem /dev/kvm).  O valor é dominado por ruído de emulação.  Reportar isso
#      é mais forte do que reportar um número bonito sem contexto.
echo "TODO: implementar analyze-capture.sh"
exit 1
