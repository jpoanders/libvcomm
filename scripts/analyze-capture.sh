#!/bin/bash
# Pairs REQUEST with RESPONSE in the host capture and computes the latency.
#
# This is the "latency statistics are computed automatically and labeled
# correctly" item, and it runs as the last step of `make`, which is where the
# assignment requires the number to appear.
#
# WHY THE HOST CAPTURE AND NOT THE GUESTS' CLOCKS.  Both ends of every pair are
# timestamped by ONE clock, on the host: the kernel stamps each datagram as it
# arrives (SO_TIMESTAMPNS in tools/bus_tap.cpp) exactly as a sniffer would.
# Subtracting two guest clocks would measure their disagreement as much as the
# network, and disciplining those clocks is Stage 3's job, not something to
# fake here.
#
# WHAT THE NUMBER IS.  REQUEST leaving vehicle 1 to RESPONSE arriving from a
# responder: a ROUND TRIP, including each guest's processing.  It is not a
# one-way latency and is not labelled as one.  Halving it would assume a
# symmetry nothing here measures.
set -euo pipefail

CAPTURES="${CAPTURES:-build/captures}"
ETHERTYPE="${ETHERTYPE:-88b5}"
MIN_SAMPLES="${MIN_SAMPLES:-10}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die() { echo "analyze-capture: $*" >&2; exit 1; }

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

echo "analyze-capture: $SOURCE"

awk -F'\t' -v ethertype="$ETHERTYPE" -v minsamples="$MIN_SAMPLES" '
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
    t = $1 + 0
    p = tolower($2)
    if (length(p) < 64) next
    if (substr(p, 25, 4) != ethertype) next

    kind  = hex2dec(substr(p, 49, 2))
    vm    = hex2dec(substr(p, 51, 2))
    comp  = hex2dec(substr(p, 53, 2))
    flags = hex2dec(substr(p, 55, 2))
    seq   = hex2dec(substr(p, 57, 4))

    # READY carries no timing: it is the boot-time handshake, it repeats until
    # the fleet goes live, and every copy has sequence 0.  Dropping it here
    # keeps the duplicate counter below meaning what it says.
    if (kind != 2 && kind != 3) next

    # Dedup.  Capturing on `any` can show one datagram more than once, and a
    # duplicate would otherwise become a second, wrong sample.  First
    # timestamp wins: it is the one closest to the frame really leaving.
    key = kind "/" vm "/" comp "/" seq
    if (key in seen) { dups++; next }
    seen[key] = t

    if (kind == 2) {                       # REQUEST
        requests++
        # Keyed on the sequence number alone, which is unambiguous because
        # Stage 1 has exactly ONE requester.  The guard below is what makes
        # that assumption fail loudly instead of pairing the wrong frames if a
        # second requester ever appears.
        if (requester == "") requester = vm "." comp
        else if (requester != vm "." comp) requesters++
        req_t[seq] = t
        req_warm[seq] = flags % 2
    } else if (kind == 3) {                # RESPONSE
        responses++
        if (!(seq in req_t)) { orphan++; next }
        if (req_warm[seq]) { warm++; next } # warm-up: excluded, as the guide asks

        rtt = (t - req_t[seq]) * 1000.0     # ms
        if (rtt < 0) { negative++; next }

        n++
        s[n] = rtt
        by_vm_n[vm]++
        by_vm_sum[vm] += rtt
    }
}
END {
    printf "\n"
    printf "  frames paired from       %d REQUEST and %d RESPONSE frames\n", requests + 0, responses + 0
    if (dups)     printf "  duplicates discarded     %d\n", dups
    if (warm)     printf "  warm-up samples excluded %d\n", warm
    if (orphan)   printf "  responses with no request %d\n", orphan
    if (negative) printf "  negative deltas discarded %d\n", negative

    if (requesters) {
        printf "\n  FAIL more than one vehicle sent REQUEST frames.\n"
        printf "       Pairing by sequence number alone would be ambiguous.\n"
        exit 1
    }

    if (n < minsamples) {
        printf "\n  FAIL only %d measured samples, fewer than the %d required.\n", n + 0, minsamples
        printf "       The capture cannot support a latency figure — do not report one.\n"
        exit 1
    }

    # Insertion sort.  n is a few hundred at most, and it keeps this script to
    # POSIX awk instead of requiring gawk for asort().
    for (i = 2; i <= n; i++) {
        v = s[i]; j = i - 1
        while (j >= 1 && s[j] > v) { s[j+1] = s[j]; j-- }
        s[j+1] = v
    }

    sum = 0
    for (i = 1; i <= n; i++) sum += s[i]
    mean = sum / n

    sd = 0
    for (i = 1; i <= n; i++) sd += (s[i] - mean) * (s[i] - mean)
    sd = sqrt(sd / n)

    med = (n % 2) ? s[(n+1)/2] : (s[n/2] + s[n/2+1]) / 2
    p95i = int(0.95 * n + 0.9999); if (p95i < 1) p95i = 1; if (p95i > n) p95i = n

    printf "\n"
    printf "  ============================================================\n"
    printf "   ROUND-TRIP LATENCY  (REQUEST -> RESPONSE, one host clock)\n"
    printf "  ============================================================\n"
    printf "   samples   %d\n", n
    printf "   mean      %8.3f ms\n", mean
    printf "   min       %8.3f ms\n", s[1]
    printf "   median    %8.3f ms\n", med
    printf "   p95       %8.3f ms\n", s[p95i]
    printf "   max       %8.3f ms\n", s[n]
    printf "   stddev    %8.3f ms\n", sd
    printf "  ============================================================\n"
    printf "   Round trip, NOT one-way. It includes each guest processing the\n"
    printf "   request and building its answer, and it is not halved: nothing\n"
    printf "   here measures the symmetry that would justify halving it.\n"
    printf "   The bench has no /dev/kvm, so QEMU runs in TCG and every figure\n"
    printf "   above carries the emulator%s overhead. Read it as an upper bound.\n", "\47s"
    printf "  ============================================================\n"

    printf "\n   per responding vehicle\n"
    for (v in by_vm_n)
        printf "     vehicle %-3d %4d samples   mean %8.3f ms\n", v, by_vm_n[v], by_vm_sum[v] / by_vm_n[v]

    exit 0
}
' "$STREAM" || die "the latency could not be computed"
