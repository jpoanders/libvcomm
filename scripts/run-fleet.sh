#!/bin/bash
# Brings up the fleet (one VM per vehicle) on the same multicast bus, captures
# the bus on the host while they run, and checks every vehicle's verdict.
#
# This is the script `make fleet` calls, and it is where the assignment's "make
# must FAIL when a receiver loses a frame" is actually enforced: a log without
# its RESULT ... OK line is a non-zero exit, not a warning.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

WORKVM="${WORKVM:-build/vm}"
LOGS="${LOGS:-build/logs}"
VMS="${VMS:-1 2 3 4 5}"
VM_TIMEOUT="${VM_TIMEOUT:-60}"   # > the app's own 45 s deadline plus boot
CAPTURE="${CAPTURE:-1}"

export SO2_MCAST="${SO2_MCAST:-239.10.10.10:15424}"

# Pins QEMU's bus to loopback: no sudo, no route to add, nothing to undo, and
# the project's traffic never reaches the building's WiFi.  See capture.sh.
export SO2_LOCALADDR="${SO2_LOCALADDR:-127.0.0.1}"

VCOMM_COMPONENTS="${VCOMM_COMPONENTS:-3}"

die() { echo "run-fleet: $*" >&2; exit 1; }

NVMS=$(echo $VMS | wc -w)
[ "$NVMS" -ge 2 ] || die "the fleet needs at least 2 vehicles, got: '$VMS'"

# -----------------------------------------------------------------------------
# The run parameters have to reach the guest, and the only channel is the kernel
# command line: /init exports SO2_VM_ID and nothing else, so a shell variable
# set here never reaches /student/app.  Without this, VMS="1 2" would still
# leave every guest expecting five vehicles.
# -----------------------------------------------------------------------------
#
# VCOMM_APPEND comes FIRST because the guest takes the first match for a given
# key, so anything set there overrides the defaults computed here.  That is what
# makes the negative tests reproducible — e.g. booting four vehicles while
# telling them to expect five, which is how you prove `make` fails when a
# receiver is missing.
export SO2_APPEND="${VCOMM_APPEND:-} vcomm.vms=$NVMS vcomm.components=$VCOMM_COMPONENTS"

# -----------------------------------------------------------------------------
# Patch the starter's launcher, once, in the WORKING COPY.
#
# Two things it cannot do as shipped: pin the multicast to an interface, and
# pass anything of ours on the kernel command line.  Both are single additions
# guarded by variables that default to empty, so an unpatched-style invocation
# behaves exactly as before.  `make clean-vm` throws the whole copy away, and
# the .orig alongside says what was there.
# -----------------------------------------------------------------------------
patch_launcher() {
    local launcher="$WORKVM/run-vm.sh"
    [ -f "$launcher" ] || die "no $launcher — unpack the starter:  make starter"

    if grep -q 'SO2_LOCALADDR' "$launcher" && grep -q 'SO2_APPEND' "$launcher"; then
        return 0
    fi

    [ -f "$launcher.orig" ] || cp "$launcher" "$launcher.orig"
    sed -i \
        -e 's|mcast="$MCAST" \\|mcast="$MCAST"${SO2_LOCALADDR:+,localaddr=$SO2_LOCALADDR} \\|' \
        -e 's|so2.vm_id=$VM_ID"|so2.vm_id=$VM_ID ${SO2_APPEND:-}"|' \
        "$launcher"

    grep -q 'SO2_LOCALADDR' "$launcher" ||
        die "could not patch $launcher (the -netdev line changed shape?)"
    grep -q 'SO2_APPEND' "$launcher" ||
        die "could not patch $launcher (the -append line changed shape?)"

    echo "run-fleet: patched $launcher (localaddr + kernel command line)"
}

# -----------------------------------------------------------------------------
PIDS=()
cleanup() {
    for entry in "${PIDS[@]:-}"; do
        [ -n "$entry" ] || continue
        kill -INT "${entry#*:}" 2>/dev/null || true
    done
    [ "$CAPTURE" = "1" ] && "$HERE/capture.sh" stop >/dev/null 2>&1
    return 0
}
# Only ever the pids this script created.  Never `pkill qemu-system-x86_64`:
# this bench is shared and the next group's run is not ours to kill.
trap cleanup EXIT

# -----------------------------------------------------------------------------
[ -f "$WORKVM/bzImage" ] || die "no $WORKVM/bzImage — run 'make starter'"
[ -f "$WORKVM/rootfs/student/app" ] || die "no app in the image — run 'make image'"
command -v qemu-system-x86_64 >/dev/null || die "qemu-system-x86_64 not installed"

patch_launcher
mkdir -p "$LOGS"
rm -f "$LOGS"/vm-*.log

[ -e /dev/kvm ] || echo "run-fleet: NOTE /dev/kvm absent — QEMU runs in TCG and the latency is biased by emulation"

if [ "$CAPTURE" = "1" ]; then
    "$HERE/capture.sh" start
fi

echo "run-fleet: $NVMS vehicles ($VMS) x $VCOMM_COMPONENTS components, bus $SO2_MCAST via $SO2_LOCALADDR, ceiling ${VM_TIMEOUT}s"
START=$(date +%s)

for id in $VMS; do
    # `< /dev/null` is mandatory: run-vm.sh uses -nographic and the VMs fight
    # over the terminal if they have a stdin.
    #
    # `timeout --signal=INT` is the ceiling that guarantees the fleet terminates
    # even if a guest wedges.  The normal path does not reach it: the app powers
    # its vehicle off once its components have reported.
    timeout --signal=INT "$VM_TIMEOUT" "$WORKVM/run-vm.sh" "$id" \
        < /dev/null > "$LOGS/vm-$id.log" 2>&1 &
    PIDS+=("$id:$!")
done

for entry in "${PIDS[@]}"; do
    wait "${entry#*:}" || true      # a VM's exit code is the timeout's; the
done                                # verdict is in the log, not here

ELAPSED=$(( $(date +%s) - START ))

if [ "$CAPTURE" = "1" ]; then
    "$HERE/capture.sh" stop
fi

# -----------------------------------------------------------------------------
# The verdict.  Everything above is setup; this is the part that is graded.
# -----------------------------------------------------------------------------
echo
echo "run-fleet: fleet finished in ${ELAPSED}s — checking verdicts"

# Two notes on the greps below, both learned the hard way on this bench:
#
#   -a   the logs are a serial console.  They begin with SeaBIOS escape
#        sequences, so grep calls them binary and stays silent about matches.
#
#   no $ anchor.  The console terminates lines with CRLF, so every line really
#        ends in \r and "OK$" never matches.  Match ' OK' as a field instead.
failed=0
for id in $VMS; do
    log="$LOGS/vm-$id.log"

    if [ ! -s "$log" ]; then
        echo "  FAIL vm=$id  no output at all (did the VM boot?)"
        failed=$((failed + 1))
        continue
    fi

    detail() {
        grep -aE '^(RESULT|READY-TIMEOUT|SENT|STATS|FATAL)' "$log" |
            tr -d '\r' | sed 's/^/         /' || true
    }

    if ! grep -qa "RESULT vm=$id components=$VCOMM_COMPONENTS ok=$VCOMM_COMPONENTS OK" "$log"; then
        echo "  FAIL vm=$id  the vehicle did not report all $VCOMM_COMPONENTS components OK"
        detail
        failed=$((failed + 1))
        continue
    fi

    comps=$(grep -a "RESULT vm=$id comp=" "$log" | grep -c ' OK' || true)
    if [ "$comps" -ne "$VCOMM_COMPONENTS" ]; then
        echo "  FAIL vm=$id  $comps of $VCOMM_COMPONENTS component verdicts are OK"
        detail
        failed=$((failed + 1))
        continue
    fi

    echo "  ok   vm=$id  $comps/$VCOMM_COMPONENTS components reported OK"
done

echo
if [ "$failed" -ne 0 ]; then
    echo "run-fleet: $failed of $NVMS vehicles FAILED — logs in $LOGS/"
    exit 1
fi

echo "run-fleet: all $NVMS vehicles OK"
