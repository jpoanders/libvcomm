#!/bin/bash
# Records the virtual bus on the HOST, in parallel with the fleet.
#
#     scripts/capture.sh start     begin recording (returns once it is live)
#     scripts/capture.sh stop      stop and flush
#
# run-fleet.sh calls both, because only it knows when the fleet starts and
# ends — and the recording has to be live BEFORE the first VM, or the frames
# you lose are exactly the ones you wanted.
#
# WHY ON THE HOST AND NOT INSIDE A VM: the guide is explicit — the host
# timestamps both directions of a round trip with a SINGLE clock.  Subtracting
# two guest clocks would measure their disagreement as much as the network, and
# disciplining those clocks is Stage 3's job, not something to fake here.
#
# ------------------------------------------------------------------------------
# TWO RECORDERS, AND WHY THE PRIMARY ONE IS NOT A SNIFFER
#
#   build/bus-tap  (primary, ALWAYS)   joins QEMU's multicast group with an
#       ordinary UDP socket.  No CAP_NET_RAW, no group membership, no external
#       package: a bare `make` on a machine that has never heard of Wireshark
#       still produces a capture and therefore still prints a latency.  That
#       matters because dumpcap is installed 0754 root:wireshark on Debian and
#       Ubuntu — a grader outside the `wireshark` group cannot even execute it.
#
#   dumpcap        (secondary, IF USABLE)   the classic sniffer, on the
#       interface.  Never fatal.  It is kept because it observes the same bus
#       through a completely different mechanism, so when both are present they
#       corroborate each other, and because a .pcapng is what the panel expects
#       to be shown.
#
# WHICH INTERFACE, for the secondary.  QEMU sends the bus as UDP multicast from
# the host, and by default the kernel routes 239.10.10.10 out of whatever
# `ip route get` picks — on this machine, WiFi.  run-fleet.sh pins it instead,
# by passing QEMU `localaddr=127.0.0.1` on the socket netdev, which sets
# IP_MULTICAST_IF and joins the group on loopback.  Pinned bus => capture on
# `lo`; otherwise fall back to `any`.
#
# LIMIT THE GUIDE REQUIRES YOU TO RESPECT: seeing a datagram on the host does
# NOT prove a guest processed the message.  Correctness is proven by the VM
# logs; the capture is proof of format and of timing.
set -euo pipefail

CAPTURES="${CAPTURES:-build/captures}"
PREFIX="${PREFIX:-$CAPTURES/fleet}"
FRAMES="$PREFIX.frames"
PCAP="${PCAP:-$PREFIX.pcapng}"

TAP="${TAP:-build/bus-tap}"
TAP_PID="$CAPTURES/.bus-tap.pid"
TAP_LOG="$CAPTURES/.bus-tap.log"
DUMPCAP_PID="$CAPTURES/.dumpcap.pid"

export SO2_MCAST="${SO2_MCAST:-239.10.10.10:15424}"
SO2_LOCALADDR="${SO2_LOCALADDR:-127.0.0.1}"

PORT="${SO2_MCAST##*:}"
if [ "$SO2_LOCALADDR" = "127.0.0.1" ]; then
    IFACE="${CAPTURE_IFACE:-lo}"
else
    IFACE="${CAPTURE_IFACE:-any}"
fi

die() { echo "capture: $*" >&2; exit 1; }

# ------------------------------------------------------------------------------
start_tap() {
    [ -x "$TAP" ] || die "$TAP is missing — run 'make app' (or just 'make')"

    rm -f "$FRAMES" "$PREFIX.pcap" "$TAP_LOG"

    "$TAP" --mcast "$SO2_MCAST" --localaddr "$SO2_LOCALADDR" \
           --out "$PREFIX" >/dev/null 2>"$TAP_LOG" &
    echo $! > "$TAP_PID"

    # Wait for the tap to say it has JOINED THE GROUP, not merely that the
    # process exists.  Between fork and IP_ADD_MEMBERSHIP it would miss frames,
    # and the ones it would miss are the first — which is where a boot-order
    # bug shows up.
    local waited=0
    until grep -q 'listening' "$TAP_LOG" 2>/dev/null; do
        sleep 0.1
        waited=$((waited + 1))
        if ! kill -0 "$(cat "$TAP_PID")" 2>/dev/null; then
            rm -f "$TAP_PID"
            die "bus-tap died on startup:
$(sed 's/^/     /' "$TAP_LOG")"
        fi
        [ "$waited" -gt 100 ] && die "bus-tap did not join $SO2_MCAST within 10 s"
    done

    echo "capture: bus-tap on $SO2_MCAST via $SO2_LOCALADDR -> $FRAMES"
}

# Best effort, by design: every failure here is reported and then ignored.
start_dumpcap() {
    if ! command -v dumpcap >/dev/null 2>&1 || [ ! -x "$(command -v dumpcap)" ]; then
        echo "capture: dumpcap unavailable — skipping the secondary recorder" \
             "(the bus-tap capture above is the one \`make\` uses)"
        return 0
    fi

    dumpcap -q -i "$IFACE" -f "udp port $PORT" -w "$PCAP" >/dev/null 2>&1 &
    local pid=$!
    echo "$pid" > "$DUMPCAP_PID"

    local waited=0
    while [ ! -s "$PCAP" ]; do
        sleep 0.1
        waited=$((waited + 1))
        if ! kill -0 "$pid" 2>/dev/null; then
            rm -f "$DUMPCAP_PID" "$PCAP"
            echo "capture: dumpcap could not start (no permission on '$IFACE'?)" \
                 "— continuing without it"
            return 0
        fi
        if [ "$waited" -gt 30 ]; then
            echo "capture: dumpcap did not open $PCAP — continuing without it"
            kill -INT "$pid" 2>/dev/null || true
            rm -f "$DUMPCAP_PID"
            return 0
        fi
    done

    echo "capture: dumpcap on '$IFACE', filter 'udp port $PORT' -> $PCAP"
}

start() {
    stop >/dev/null 2>&1 || true
    mkdir -p "$CAPTURES"

    # Delete EVERY recording of the previous run before starting this one, and
    # do it here rather than inside each recorder.  If the secondary does not
    # run today — no dumpcap on this machine — its file from yesterday would
    # otherwise survive, and frames.sh would fall back to it the moment the tap
    # produced nothing.  A stale capture that analyses cleanly is worse than no
    # capture at all: it turns a failed fleet run into a green one.
    rm -f "$FRAMES" "$PREFIX.pcap" "$PCAP" "$TAP_LOG"

    start_tap
    start_dumpcap
}

# ------------------------------------------------------------------------------
# SIGINT, then escalate.  Both recorders flush on SIGINT; SIGKILL would cost the
# tail of the capture, which is the last measurements.
stop_one() {
    local pidfile="$1"
    [ -f "$pidfile" ] || return 0
    local pid
    pid="$(cat "$pidfile")"
    rm -f "$pidfile"

    kill -INT "$pid" 2>/dev/null || return 0
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 0.1
        waited=$((waited + 1))
        if [ "$waited" -gt 50 ]; then
            kill -KILL "$pid" 2>/dev/null || true
            break
        fi
    done
    wait "$pid" 2>/dev/null || true
}

stop() {
    stop_one "$TAP_PID"
    stop_one "$DUMPCAP_PID"

    if [ -s "$FRAMES" ]; then
        echo "capture: stopped -> $FRAMES ($(wc -l < "$FRAMES") frames)"
    else
        echo "capture: stopped -> $FRAMES"
    fi
    [ -s "$PCAP" ] && echo "capture: stopped -> $PCAP"
    [ -s "$PREFIX.pcap" ] && \
        echo "capture: $PREFIX.pcap holds the same frames as LINKTYPE_ETHERNET" \
             "(open it in Wireshark: the guest frames are dissected directly)"
    return 0
}

case "${1:-}" in
    start) start ;;
    stop)  stop  ;;
    *)     die "usage: $0 start|stop" ;;
esac
