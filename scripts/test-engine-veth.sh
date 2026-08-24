#!/bin/bash
# Prova POSITIVA do braço de erro do drain(): engine_rx_errors() tem que subir
# quando o socket quebra debaixo da Engine armada.
#
# Precisa de root — cria um par veth e derruba uma ponta.  Como o binário roda
# como filho deste script, ele herda CAP_NET_RAW e o setcap não entra na história.
#
#     sudo scripts/test-engine-veth.sh
#
# POR QUE VETH E NÃO `lo`:  derrubar a loopback quebra a máquina inteira.  O par
# veth é descartável e o trap abaixo o remove mesmo se o teste falhar.
#
# O QUE ACONTECE:  packet_notifier() vê NETDEV_DOWN na interface do socket, seta
# sk_err = ENETDOWN e chama sk_error_report(), que dispara o SIGIO.  O drain()
# entra, o recvfrom() consome o sk_err e devolve -1/ENETDOWN — nem EAGAIN nem
# EINTR, ou seja, o terceiro braço.
set -euo pipefail

BIN="${BIN:-build/test-engine}"
IF0="${IF0:-vcomm0}"
IF1="${IF1:-vcomm1}"

[ "$(id -u)" -eq 0 ] || { echo "precisa de root: sudo $0"; exit 1; }
[ -x "$BIN" ] || { echo "compile antes: make test-engine"; exit 1; }

cleanup() { ip link del "$IF0" 2>/dev/null || true; rm -f "${out:-}"; }
trap cleanup EXIT

ip link del "$IF0" 2>/dev/null || true
ip link add "$IF0" type veth peer name "$IF1"
ip link set "$IF0" up
ip link set "$IF1" up

out="$(mktemp)"
VCOMM_TEST_IFACE="$IF0" VCOMM_ERROR_TEST=1 "$BIN" >"$out" 2>&1 &
pid=$!

# Espera o handshake.  Sem ele, derrubar a interface cedo demais testaria o
# construtor, não o drain().
pronto=0
for _ in $(seq 1 100); do
    if grep -q PRONTO-PARA-ERRO "$out" 2>/dev/null; then pronto=1; break; fi
    sleep 0.1
done

if [ "$pronto" -ne 1 ]; then
    echo "FALHA: o teste nunca chegou em PRONTO-PARA-ERRO"
    kill "$pid" 2>/dev/null || true
    cat "$out"
    exit 1
fi

ip link set "$IF0" down

# Escalada.  O caminho esperado é NETDEV_DOWN setar sk_err = ENETDOWN.  Se por
# alguma razão a interface cair sem sinalizar erro no socket, remover o device
# (NETDEV_UNREGISTER) faz o mesmo de forma inequívoca.  Sem isso o teste ficaria
# 10 s parado e falharia por timeout, sem dizer qual dos dois eventos faltou.
for _ in $(seq 1 30); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$pid" 2>/dev/null; then
    echo "NOTA: NETDEV_DOWN não bastou; removendo $IF0 (NETDEV_UNREGISTER)"
    ip link del "$IF0" 2>/dev/null || true
fi

rc=0
wait "$pid" || rc=$?
cat "$out"
exit "$rc"
