#!/bin/bash

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

    rm -f "$FRAMES" "$PREFIX.pcap" "$PCAP" "$TAP_LOG"

    start_tap
    start_dumpcap
}


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
