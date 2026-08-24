#!/bin/bash
# Brings up the 5 VMs (5 vehicles) on the same multicast bus and collects the
# logs.
set -euo pipefail

WORKVM="${WORKVM:-build/vm}"
LOGS="${LOGS:-build/logs}"
VMS="${VMS:-1 2 3 4 5}"
VM_TIMEOUT="${VM_TIMEOUT:-20}"
export SO2_MCAST="${SO2_MCAST:-239.10.10.10:15424}"

# TODO(joao):
#
#   For each ID in $VMS, in PARALLEL (& at the end, keeping the PIDs):
#
#       timeout --signal=INT "$VM_TIMEOUT" "$WORKVM/run-vm.sh" "$ID" \
#           < /dev/null > "$LOGS/vm-$ID.log" 2>&1 &
#
#   Three details you have already verified on the bench that make this work:
#     - `< /dev/null` is mandatory: run-vm.sh uses -nographic and the VM fights
#       the other ones for the terminal if it has a stdin.
#     - `timeout --signal=INT` is what guarantees the fleet terminates on its
#       own, even if a receiver stays blocked — without it, make hangs.
#     - booting under TCG (no /dev/kvm) takes 3-5 s.  The sender has to wait for
#       the receivers to come up, otherwise it transmits into the void.  Decide
#       HOW: a fixed sleep is fragile; a "READY" line in the log or a warm-up
#       round is honest.
#
#   Afterwards: wait on the PIDs, and check EACH log's verdict:
#       grep -q 'RESULT .* OK' "$LOGS/vm-$ID.log" || exit 1
#   make MUST fail if a receiver lost a frame.
echo "TODO: implement run-fleet.sh"
exit 1
