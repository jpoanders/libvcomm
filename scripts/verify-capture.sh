#!/bin/bash
# Proves the frame layout FROM THE CAPTURE ALONE.
#
# This is the acceptance-checklist item "an external capture independently
# proves frame layout", and the word that matters is *independently*: nothing
# here reads a VM log or trusts the application.  It reads bytes off the wire
# and asserts what the assignment requires of them.
#
# What it proves, per project frame:
#   - the destination is ff:ff:ff:ff:ff:ff, every time, with no exception;
#   - the source is a real per-VM MAC, 02:00:00:00:00:<id>, not a hard-coded
#     zero;
#   - bytes 12-13 are our EtherType in network order;
#   - the payload begins with our magic at the documented offset;
#   - and the cross-check that ties the two layers together: the vehicle id
#     INSIDE the payload equals the last byte of the source MAC, so the frame
#     really came from the vehicle it claims to.
#
# No guest IP is involved anywhere: the guest frame carries EtherType 0x88B5,
# not 0x0800/0x86DD, which is visible in the same bytes.
set -euo pipefail

CAPTURES="${CAPTURES:-build/captures}"
ETHERTYPE="${ETHERTYPE:-88b5}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die() { echo "verify-capture: $*" >&2; exit 1; }

# WHERE THE BYTES COME FROM is frames.sh's business, not ours: it prefers the
# unprivileged bus-tap recording and falls back to a dumpcap pcapng, and both
# arrive here in the same "<epoch>\t<hex>" form.  That separation is what let
# the privileged capture dependency be dropped without touching a line of the
# analysis below.
#
# An explicit argument or PCAP= means "read THAT capture" — the tap's stream
# must not silently win over it, or the negative tests would pass by reading
# the wrong file.
if [ $# -ge 1 ]; then
    case "$1" in
        *.frames) FRAMES="$1"; PCAP="${PCAP:-/nonexistent}" ;;
        *)        PCAP="$1";   FRAMES="/nonexistent" ;;
    esac
elif [ -n "${PCAP+set}" ]; then
    FRAMES="${FRAMES:-/nonexistent}"
fi
export CAPTURES
if [ -n "${FRAMES+set}" ]; then export FRAMES; fi
if [ -n "${PCAP+set}" ];   then export PCAP;   fi

STREAM="$(mktemp)"
trap 'rm -f "$STREAM"' EXIT

if ! SOURCE="$("$HERE/frames.sh" 2>&1 >"$STREAM")"; then
    echo "$SOURCE" >&2
    exit 1
fi

echo "verify-capture: $SOURCE"

awk -F'\t' -v ethertype="$ETHERTYPE" '
# Offsets, in hex characters, from the start of the guest Ethernet frame — which
# is also the start of QEMU'"'"'s UDP payload.  See app/fleet_payload.h; if that
# table moves, these move with it and this script is what says so.
#   substr is 1-based, so byte N starts at 2N+1.
function hex2dec(h,   i, c, v, n) {
    n = 0; h = tolower(h)
    for (i = 1; i <= length(h); i++) {
        c = substr(h, i, 1)
        v = index("0123456789abcdef", c) - 1
        if (v < 0) return -1
        n = n * 16 + v
    }
    return n
}
{
    total++
    p = tolower($2)
    if (length(p) < 64) { short++; next }

    et = substr(p, 25, 4)
    if (et != ethertype) { other[et]++; next }

    ours++
    dst   = substr(p, 1, 12)
    src   = substr(p, 13, 12)
    magic = substr(p, 41, 8)
    kind  = hex2dec(substr(p, 49, 2))
    vm    = hex2dec(substr(p, 51, 2))
    comp  = hex2dec(substr(p, 53, 2))
    seq   = hex2dec(substr(p, 57, 4))

    if (dst != "ffffffffffff") { bad_dst++; if (!ex_dst) ex_dst = dst }
    if (magic != "56434d31")   { bad_magic++; if (!ex_magic) ex_magic = magic }

    if (substr(src, 1, 10) != "0200000000") {
        bad_src++; if (!ex_src) ex_src = src
    } else {
        mac_vm = hex2dec(substr(src, 11, 2))
        if (mac_vm != vm) { mismatch++; if (!ex_mm) ex_mm = src " claims vm " vm }
        seen_vm[mac_vm]++
    }

    kinds[kind]++
    if (kind == 2 && seq > maxseq) maxseq = seq
    seen_comp[comp]++
}
END {
    fail = 0
    printf "  packets in capture      %d\n", total
    printf "  frames with EtherType   0x%s: %d\n", ethertype, ours + 0

    n = 0; for (et in other) n += other[et]
    if (n) printf "  other EtherTypes        %d (the guests\47 own IPv6 noise)\n", n

    if (ours == 0) { print "  FAIL no project frame in the capture at all"; exit 1 }

    if (bad_dst)  { printf "  FAIL %d frames whose destination is not broadcast (e.g. %s)\n", bad_dst, ex_dst; fail = 1 }
    else            print  "  ok   every destination is ff:ff:ff:ff:ff:ff"

    if (bad_src)  { printf "  FAIL %d frames with a source outside 02:00:00:00:00:xx (e.g. %s)\n", bad_src, ex_src; fail = 1 }
    else            print  "  ok   every source is a real per-VM MAC, none zeroed"

    if (mismatch) { printf "  FAIL %d frames whose payload vehicle id contradicts the source MAC (e.g. %s)\n", mismatch, ex_mm; fail = 1 }
    else            print  "  ok   payload vehicle id agrees with the source MAC on every frame"

    if (bad_magic){ printf "  FAIL %d frames without the VCM1 magic at offset 20 (e.g. %s)\n", bad_magic, ex_magic; fail = 1 }
    else            print  "  ok   the payload magic is where app/fleet_payload.h says it is"

    v = 0; for (x in seen_vm) v++
    c = 0; for (x in seen_comp) c++
    printf "  ok   %d distinct vehicles and %d distinct components on the wire\n", v, c

    printf "  ok   kinds seen: READY %d, REQUEST %d, RESPONSE %d (max sequence %d)\n", \
           kinds[1] + 0, kinds[2] + 0, kinds[3] + 0, maxseq + 0

    if (kinds[2] + 0 == 0) { print "  FAIL no REQUEST frame was captured"; fail = 1 }
    if (kinds[3] + 0 == 0) { print "  FAIL no RESPONSE frame was captured"; fail = 1 }

    exit fail
}
' "$STREAM" || die "the capture does not prove the frame layout"

echo "verify-capture: layout proven from the capture alone"
