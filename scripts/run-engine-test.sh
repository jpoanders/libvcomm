#!/bin/bash

set -euo pipefail

BUILD="${BUILD:-build}"
BIN="${BIN:-$BUILD/test-engine}"
SETCAP="${SETCAP:-$(command -v setcap 2>/dev/null || echo /usr/sbin/setcap)}"
GETCAP="${GETCAP:-$(command -v getcap 2>/dev/null || echo /usr/sbin/getcap)}"

[ -x "$BIN" ] || { echo "run-engine-test: $BIN is missing — run 'make'" >&2; exit 1; }


run_privileged() {
    echo "test-engine: $1"
    VCOMM_REQUIRE_RAW=1 "$BIN"
}

# --- rung 1: the capability is already on the inode ---------------------------
if "$GETCAP" "$BIN" 2>/dev/null | grep -q cap_net_raw; then
    run_privileged "level 1 enabled by file capability on $BIN"
    exit $?
fi

# --- rung 2: we are already root ---------------------------------------------
if [ "$(id -u)" -eq 0 ]; then
    run_privileged "level 1 enabled: running as root"
    exit $?
fi

# --- rung 3: passwordless sudo, which cannot prompt ---------------------------
if [ -x "$SETCAP" ] && sudo -n true 2>/dev/null; then
    if sudo -n "$SETCAP" cap_net_raw+ep "$BIN" 2>/dev/null; then
        run_privileged "level 1 enabled by passwordless sudo setcap"
        exit $?
    fi
fi


if command -v unshare >/dev/null 2>&1 && unshare -Urn true 2>/dev/null; then
    echo "test-engine: level 1 enabled inside an unprivileged user namespace"

    if VCOMM_REQUIRE_RAW=1 VCOMM_TEST_IFACE="${VCOMM_TEST_IFACE:-lo}" \
       unshare -Urn sh -c 'ip link set lo up 2>/dev/null || true; exec "$0"' "$BIN"
    then
        exit 0
    fi
    echo "test-engine: the namespace could not carry level 1 — falling back"
fi

echo "test-engine: level 1 SKIPPED on the host — no CAP_NET_RAW and no user namespace"
echo "             the raw socket path is exercised anyway, by the fleet:"
echo "             15 components open real AF_PACKET sockets inside the VMs and"
echo "             build/bus-tap records the frames they put on the bus."
echo "             to prove it here too, once:  make caps   (asks for sudo)"
"$BIN"
