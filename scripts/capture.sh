#!/bin/bash
# Captura o barramento virtual no HOST, em paralelo com a frota.
#
# Por que capturar no host e não dentro da VM: o guia é explícito — o tshark do
# host carimba os dois sentidos com UM relógio só.  Comparar relógios de guests
# diferentes antes da Etapa 3 (sincronização temporal) daria número sem sentido.
set -euo pipefail

CAPTURES="${CAPTURES:-build/captures}"
export SO2_MCAST="${SO2_MCAST:-239.10.10.10:15424}"

# QUAL INTERFACE CAPTURAR — descubra, não chute:
#
#     ip route get 239.10.10.10
#
# Nesta máquina, em 22/08/2026, a resposta foi `dev wlp0s20f3`, NÃO `lo`.
# Duas consequências, e as duas importam:
#
#   1. capturar em `lo` devolve captura vazia e você perde uma hora achando que
#      o problema é o seu código;
#   2. o barramento do grupo está SAINDO PELA WIFI — ou seja, para a rede do
#      prédio.  Isso pode colidir com outro grupo no mesmo laboratório e ainda
#      põe o tráfego do projeto na rede da universidade.  Para prender o
#      barramento à máquina:
#
#          sudo ip route add 239.10.10.0/24 dev lo
#
#      (precisa de sudo; some no reboot).  Faça isso ANTES da apresentação e
#      confira de novo com `ip route get` — a rota muda se você trocar de rede.
#
# TODO(joao):
#   - o QEMU encapsula o frame Ethernet do guest DENTRO de um datagrama UDP.
#     O EtherType 0x88B5 está no payload UDP, não no cabeçalho que o tshark
#     mostra de cara — conte os offsets a partir do começo dos dados UDP.
#   - dumpcap já tem cap_net_admin,cap_net_raw nesta máquina — não precisa sudo.
#   - filtro sugerido: udp port <porta do SO2_MCAST>
#   - gravar em "$CAPTURES/fleet.pcapng"
#   - subir a captura ANTES da frota e derrubar DEPOIS, senão você perde os
#     primeiros frames — que são justamente os que você quer.
#
# LIMITE QUE O GUIA MANDA RESPEITAR: ver o datagrama sair no host NÃO prova que
# um guest processou a mensagem.  A prova de correção é o log das VMs; a captura
# é prova de formato e de tempo.
echo "TODO: implementar capture.sh"
exit 1
