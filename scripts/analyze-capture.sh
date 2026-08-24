#!/bin/bash
# Extracts the request/response pairs from the capture and computes the latency.
set -euo pipefail

CAPTURES="${CAPTURES:-build/captures}"
PCAP="${1:-$CAPTURES/fleet.pcapng}"

# TODO(joao):
#
#   1. extract with tshark: the timestamp plus the bytes that distinguish
#      request, response and sequence number.  Something like:
#        tshark -r "$PCAP" -T fields -e frame.time_epoch -e data.data
#      and slice the fields by offset — the UDP payload contains the whole
#      Ethernet frame, so your header starts after it.
#
#   2. pair by sequence number, discard the warm-up samples.
#
#   3. compute count, mean, min, max and a percentile (p95 or p99).
#
#   4. PRINT THE RIGHT LABEL.  If you measured request->response, it is
#      round-trip.  Calling that "one-way latency" without dividing and without
#      justifying it is exactly the kind of thing Fröhlich asks about at the
#      panel.
#
#   5. State, alongside the number, that the bench runs QEMU in TCG (this
#      machine has no /dev/kvm).  The value is dominated by emulation noise.
#      Reporting that is stronger than reporting a pretty number with no
#      context.
echo "TODO: implement analyze-capture.sh"
exit 1
