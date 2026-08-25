#ifndef LIBVCOMM_FLEET_PAYLOAD_H
#define LIBVCOMM_FLEET_PAYLOAD_H

#include <cstring>

// =============================================================================
// The Stage 1 test payload.
//
// The assignment fixes Stage 1's message as M = {.*} — an array of bytes and
// nothing more — so anything the fleet test needs to identify a message is the
// APPLICATION's business, not the library's.  This header is that application
// format, and it deliberately lives in app/ rather than include/: the day
// Stage 2 turns M into {origin, payload}, the origin moves into the library and
// this file shrinks.
//
// -----------------------------------------------------------------------------
// WIRE LAYOUT.  Offsets are counted from the first byte of the guest Ethernet
// frame, which is also the first byte of QEMU's UDP payload on the host — so
// this table is what scripts/analyze-capture.sh and scripts/verify-capture.sh
// slice by.  IF YOU CHANGE IT, CHANGE THEM.
//
//   off  size  field
//   ---  ----  -------------------------------------------------------------
//     0     6  Ethernet dst      ff:ff:ff:ff:ff:ff, always (assignment)
//     6     6  Ethernet src      02:00:00:00:00:<vm id>, from eth0
//    12     2  EtherType         88 b5, network order
//    14     2  Protocol from_port   |
//    16     2  Protocol to_port     +- Protocol::Header, HOST order (x86 LE)
//    18     2  Protocol length      |
//    20     4  magic             'V' 'C' 'M' '1'
//    24     1  kind              1 READY, 2 REQUEST, 3 RESPONSE
//    25     1  vm id             the vehicle that sent it
//    26     1  component id      the process within that vehicle
//    27     1  flags             bit 0 = warm-up, excluded from the statistics
//    28     2  sequence          BIG endian
//    30     2  padding           zero
//
// Total frame: 14 + 6 + 12 = 32 bytes, comfortably under the MTU, so nothing is
// ever fragmented — which is what the assignment guarantees and what lets the
// library ignore reassembly entirely.
//
// The sequence number is big-endian on purpose: it is read back out of a hex
// dump by a shell script, and network order is the one that reads correctly
// left to right.  Protocol::Header, by contrast, is host order — every VM on
// this bus is x86, but that IS a portability limit and it is recorded as one in
// doc/design-decisions.md.
// =============================================================================

namespace fleet {

enum Kind
{
    KIND_READY = 1,    // "my stack is up" — announced until the fleet goes live
    KIND_REQUEST = 2,  // broadcast by the requester, one per sequence number
    KIND_RESPONSE = 3  // broadcast back, carrying the sequence it answers
};

enum Flags
{
    FLAG_WARMUP = 0x01 // excluded from the latency statistics
};

// Absolute offset of this payload inside the frame.  Repeated in the scripts;
// verify-capture.sh asserts the magic really is here, so a drift is caught by a
// test instead of silently mis-parsing every frame.
static const unsigned int FRAME_OFFSET = 20;

struct Payload
{
    unsigned char magic[4];
    unsigned char kind;
    unsigned char vm_id;
    unsigned char comp_id;
    unsigned char flags;
    unsigned char seq_hi; // big endian
    unsigned char seq_lo;
    unsigned char pad[2];
} __attribute__((packed));

static_assert(sizeof(Payload) == 12, "fleet::Payload must be 12 bytes");

inline void encode(Payload & p, unsigned char kind, unsigned char vm_id,
                   unsigned char comp_id, unsigned short seq,
                   unsigned char flags)
{
    p.magic[0] = 'V';
    p.magic[1] = 'C';
    p.magic[2] = 'M';
    p.magic[3] = '1';
    p.kind = kind;
    p.vm_id = vm_id;
    p.comp_id = comp_id;
    p.flags = flags;
    p.seq_hi = static_cast<unsigned char>((seq >> 8) & 0xff);
    p.seq_lo = static_cast<unsigned char>(seq & 0xff);
    p.pad[0] = 0;
    p.pad[1] = 0;
}

inline bool valid(const Payload & p)
{
    return p.magic[0] == 'V' && p.magic[1] == 'C' && p.magic[2] == 'M' &&
           p.magic[3] == '1';
}

inline unsigned short seq_of(const Payload & p)
{
    return static_cast<unsigned short>((p.seq_hi << 8) | p.seq_lo);
}

inline const char * kind_name(unsigned char kind)
{
    switch (kind) {
    case KIND_READY:
        return "READY";
    case KIND_REQUEST:
        return "REQ";
    case KIND_RESPONSE:
        return "RESP";
    default:
        return "?";
    }
}

} // namespace fleet

#endif // LIBVCOMM_FLEET_PAYLOAD_H
