#!/bin/bash

set -euo pipefail

CAPTURES="${CAPTURES:-build/captures}"
FRAMES="${FRAMES:-$CAPTURES/fleet.frames}"
PCAP="${PCAP:-$CAPTURES/fleet.pcapng}"

die() { echo "frames: $*" >&2; exit 1; }


if [ -s "$FRAMES" ]; then
    echo "$FRAMES (bus-tap, unprivileged)" >&2
    cat "$FRAMES"
    exit 0
fi

if [ -s "$PCAP" ] && command -v tshark >/dev/null 2>&1; then
    echo "$PCAP (tshark)" >&2
    tshark -r "$PCAP" -T fields -e frame.time_epoch -e udp.payload 2>/dev/null
    exit 0
fi

if [ -f "$FRAMES" ] || [ -f "$PCAP" ]; then
    die "the capture is empty — the fleet produced no frames.
     looked at: $FRAMES
                $PCAP"
fi

die "no capture found — run 'make fleet' first.
     looked at: $FRAMES
                $PCAP"
