#!/bin/bash
# Emits the captured bus as one line per frame, "<epoch>\t<hex>", on stdout.
#
# It is the single place that knows WHERE the capture came from, so
# verify-capture.sh and analyze-capture.sh can each be about what the bytes
# mean instead of about which sniffer was available on the grader's machine.
#
# Two sources, in order of preference:
#
#   1. build/captures/fleet.frames — written by build/bus-tap, which joins the
#      QEMU multicast group with an ordinary UDP socket.  NEEDS NO PRIVILEGE
#      AND NO EXTERNAL TOOL, so this is the path a bare `make` always takes.
#
#   2. build/captures/fleet.pcapng — written by dumpcap, when the machine
#      happens to have it usable.  Read with tshark.  Kept because it observes
#      the bus through a completely different mechanism (a kernel packet socket
#      on the interface, rather than a member of the group), so when both exist
#      they cross-check each other.
#
# The output format is deliberately the one `tshark -T fields -e
# frame.time_epoch -e udp.payload` produces: that is what these scripts were
# written against, and matching it byte for byte is what let the privileged
# dependency be removed without touching a line of the analysis.
#
# In both cases the hex is the GUEST ETHERNET FRAME, starting at the
# destination MAC — for source 2 because QEMU carries exactly one frame per UDP
# datagram with no framing of its own, and for source 1 because that datagram
# is what the tap receives.
set -euo pipefail

CAPTURES="${CAPTURES:-build/captures}"
FRAMES="${FRAMES:-$CAPTURES/fleet.frames}"
PCAP="${PCAP:-$CAPTURES/fleet.pcapng}"

die() { echo "frames: $*" >&2; exit 1; }

# Names the source on stderr so the caller can report it without duplicating
# this decision.  stdout stays pure data.
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
