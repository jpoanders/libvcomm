#!/bin/bash
# POSITIVE proof of drain()'s error arm: engine_rx_errors() must go up when the
# socket breaks underneath an armed Engine.
#
# Needs root — it creates a veth pair and brings one end down.  Since the binary
# runs as a child of this script, it inherits CAP_NET_RAW and setcap never
# enters the picture.
#
#     sudo scripts/test-engine-veth.sh
#
# WHY VETH AND NOT `lo`:  bringing loopback down breaks the whole machine.  The
# veth pair is disposable and the trap below removes it even if the test fails.
#
# WHAT HAPPENS:  packet_notifier() sees NETDEV_DOWN on the socket's interface,
# sets sk_err = ENETDOWN and calls sk_error_report(), which fires the SIGIO.
# drain() runs, recvfrom() consumes the sk_err and returns -1/ENETDOWN — neither
# EAGAIN nor EINTR, i.e. the third arm.
set -euo pipefail

BIN="${BIN:-build/test-engine}"
IF0="${IF0:-vcomm0}"
IF1="${IF1:-vcomm1}"

[ "$(id -u)" -eq 0 ] || { echo "root required: sudo $0"; exit 1; }
[ -x "$BIN" ] || { echo "build it first: make test-engine"; exit 1; }

cleanup() { ip link del "$IF0" 2>/dev/null || true; rm -f "${out:-}"; }
trap cleanup EXIT

ip link del "$IF0" 2>/dev/null || true
ip link add "$IF0" type veth peer name "$IF1"
ip link set "$IF0" up
ip link set "$IF1" up

out="$(mktemp)"
VCOMM_TEST_IFACE="$IF0" VCOMM_ERROR_TEST=1 "$BIN" >"$out" 2>&1 &
pid=$!

# Wait for the handshake.  Without it, bringing the interface down too early
# would test the constructor, not drain().
ready=0
for _ in $(seq 1 100); do
    if grep -q READY-FOR-ERROR "$out" 2>/dev/null; then ready=1; break; fi
    sleep 0.1
done

if [ "$ready" -ne 1 ]; then
    echo "FAILURE: the test never reached READY-FOR-ERROR"
    kill "$pid" 2>/dev/null || true
    cat "$out"
    exit 1
fi

ip link set "$IF0" down

# Escalation.  The expected path is NETDEV_DOWN setting sk_err = ENETDOWN.  If
# for some reason the interface goes down without signalling an error on the
# socket, removing the device (NETDEV_UNREGISTER) does the same unambiguously.
# Without this the test would sit for 10 s and fail on timeout, without saying
# which of the two events was missing.
for _ in $(seq 1 30); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$pid" 2>/dev/null; then
    echo "NOTE: NETDEV_DOWN was not enough; removing $IF0 (NETDEV_UNREGISTER)"
    ip link del "$IF0" 2>/dev/null || true
fi

rc=0
wait "$pid" || rc=$?
cat "$out"
exit "$rc"
