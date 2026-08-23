#!/bin/bash
# Sobe as 5 VMs (5 veículos) no mesmo barramento multicast e coleta os logs.
set -euo pipefail

WORKVM="${WORKVM:-build/vm}"
LOGS="${LOGS:-build/logs}"
VMS="${VMS:-1 2 3 4 5}"
VM_TIMEOUT="${VM_TIMEOUT:-20}"
export SO2_MCAST="${SO2_MCAST:-239.10.10.10:15424}"

# TODO(joao):
#
#   Para cada ID em $VMS, em PARALELO (& no fim, guardar os PIDs):
#
#       timeout --signal=INT "$VM_TIMEOUT" "$WORKVM/run-vm.sh" "$ID" \
#           < /dev/null > "$LOGS/vm-$ID.log" 2>&1 &
#
#   Três detalhes que você já verificou na bancada e que fazem isso funcionar:
#     - `< /dev/null` é obrigatório: o run-vm.sh usa -nographic e a VM disputa o
#       terminal com as outras se tiver stdin.
#     - `timeout --signal=INT` é o que garante que a frota termina sozinha,
#       mesmo se um receptor ficar bloqueado — sem isso o make pendura.
#     - boot em TCG (sem /dev/kvm) leva 3–5 s.  O emissor precisa esperar os
#       receptores subirem, senão manda no vazio.  Decida COMO: sleep fixo é
#       frágil; um "READY" no log ou uma rodada de warm-up é honesto.
#
#   Depois: wait nos PIDs, e conferir o veredito de CADA log:
#       grep -q 'RESULT .* OK' "$LOGS/vm-$ID.log" || exit 1
#   O make TEM que falhar se um receptor perdeu frame.
echo "TODO: implementar run-fleet.sh"
exit 1
