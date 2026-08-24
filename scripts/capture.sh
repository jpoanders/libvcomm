#!/bin/bash
# Captures the virtual bus on the HOST, in parallel with the fleet.
#
# Why capture on the host and not inside the VM: the guide is explicit — the
# host's tshark timestamps both directions with a SINGLE clock.  Comparing
# clocks from different guests before Stage 3 (time synchronization) would give
# meaningless numbers.
set -euo pipefail

CAPTURES="${CAPTURES:-build/captures}"
export SO2_MCAST="${SO2_MCAST:-239.10.10.10:15424}"

# WHICH INTERFACE TO CAPTURE — find out, do not guess:
#
#     ip route get 239.10.10.10
#
# On this machine, on 2026-08-22, the answer was `dev wlp0s20f3`, NOT `lo`.
# Two consequences, and both matter:
#
#   1. capturing on `lo` returns an empty capture and you lose an hour thinking
#      the problem is your code;
#   2. the group's bus is GOING OUT OVER WIFI — that is, onto the building's
#      network.  It may collide with another group in the same lab and it also
#      puts the project's traffic on the university network.  To pin the bus to
#      the machine:
#
#          sudo ip route add 239.10.10.0/24 dev lo
#
#      (needs sudo; disappears on reboot).  Do this BEFORE the presentation and
#      check again with `ip route get` — the route changes if you switch
#      networks.
#
# TODO(joao):
#   - QEMU encapsulates the guest's Ethernet frame INSIDE a UDP datagram.
#     EtherType 0x88B5 is in the UDP payload, not in the header tshark shows
#     up front — count the offsets from the start of the UDP data.
#   - dumpcap already has cap_net_admin,cap_net_raw on this machine — no sudo
#     needed.
#   - suggested filter: udp port <port from SO2_MCAST>
#   - write to "$CAPTURES/fleet.pcapng"
#   - start the capture BEFORE the fleet and stop it AFTER, otherwise you lose
#     the first frames — which are exactly the ones you want.
#
# LIMIT THE GUIDE REQUIRES YOU TO RESPECT: seeing the datagram leave on the host
# does NOT prove a guest processed the message.  The proof of correctness is the
# VM logs; the capture is proof of format and of timing.
echo "TODO: implement capture.sh"
exit 1
