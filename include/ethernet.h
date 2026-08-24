#ifndef LIBVCOMM_ETHERNET_H
#define LIBVCOMM_ETHERNET_H

#include <cstring>
#include "traits.h"

// =============================================================================
// class Ethernet — "all necessary definitions and formats", in the PDF's own
// words.
//
// This class does NOT talk to the kernel.  It only describes the format of what
// travels on the wire.  The Engine is what makes syscalls.  Keeping that
// separation is half the answer to "why does the Engine isolate the raw
// socket?".
// =============================================================================

class Ethernet
{
public:
    // Largest payload that fits in a frame without fragmentation.  The
    // assignment guarantees every project message is smaller than this => we
    // never fragment.
    static const unsigned int MTU = 1500;
    static const unsigned int HEADER_SIZE = 14;

    // EtherType.  WARNING: on the wire it goes in network byte order
    // (big-endian).  Inside the library we keep it in host order and convert
    // with htons() only at the Engine boundary.  Picking one convention and not
    // mixing the two is what avoids the classic "the receiver never sees
    // anything" bug.
    typedef unsigned short Protocol;

    // -------------------------------------------------------------------------
    // Address — a 6-byte MAC address.  NIC::Address *is* this.
    // -------------------------------------------------------------------------
    class Address
    {
    public:
        enum Null
        {
            NULL_ADDRESS = 0
        };

        Address() { std::memset(_addr, 0, sizeof(_addr)); }
        Address(const Null &) { std::memset(_addr, 0, sizeof(_addr)); }

        Address(unsigned char a0, unsigned char a1, unsigned char a2,
                unsigned char a3, unsigned char a4, unsigned char a5)
        {
            _addr[0] = a0;
            _addr[1] = a1;
            _addr[2] = a2;
            _addr[3] = a3;
            _addr[4] = a4;
            _addr[5] = a5;
        }

        explicit Address(const unsigned char * raw)
        {
            std::memcpy(_addr, raw, sizeof(_addr));
        }

        unsigned char * bytes() { return _addr; }
        const unsigned char * bytes() const { return _addr; }

        bool operator==(const Address & a) const
        {
            return !std::memcmp(_addr, a._addr, sizeof(_addr));
        }
        bool operator!=(const Address & a) const { return !(*this == a); }

        // True if the address is not all zeros.  Used by Protocol::Address.
        operator bool() const
        {
            for (unsigned int i = 0; i < sizeof(_addr); i++)
                if (_addr[i])
                    return true;
            return false;
        }

        // Writes "aa:bb:cc:dd:ee:ff" into buf (>= 18 bytes).  Returns buf.
        char * to_string(char * buf) const;

    private:
        unsigned char _addr[6];
    } __attribute__((packed));

    // MANDATORY destination of every project frame: the medium models a radio
    // cell, not a point-to-point cable.  (full_assignment.pdf, "Identifiers")
    static const Address BROADCAST;

    // -------------------------------------------------------------------------
    // Header / Frame — the literal wire layout.
    //
    //   +--------------+--------------+-----------+------------------+
    //   | destination  | source       | EtherType | payload          |
    //   |   6 bytes    |   6 bytes    |  2 bytes  |  <= MTU          |
    //   +--------------+--------------+-----------+------------------+
    // -------------------------------------------------------------------------
    struct Header
    {
        Address dst;
        Address src;
        Protocol prot; // network order when on the wire
    } __attribute__((packed));

    struct Frame : public Header
    {
        unsigned char data[MTU];
    } __attribute__((packed));

    // -------------------------------------------------------------------------
    // Statistics — the PDF asks for NIC::statistics().  The counters live here
    // because they are a link-layer concept, not something specific to one
    // Engine.
    // -------------------------------------------------------------------------
    struct Statistics
    {
        Statistics()
            : tx_packets(0), tx_bytes(0), rx_packets(0), rx_bytes(0),
              rx_dropped(0)
        {}

        unsigned int tx_packets;
        unsigned int tx_bytes;
        unsigned int rx_packets;
        unsigned int rx_bytes;
        unsigned int
            rx_dropped; // frame arrived but there was no buffer / observer
    };
};

// The link header is 14 bytes.  If this line fails, some field gained padding
// and the frame on the wire is wrong — the compiler warns before the VM does.
static_assert(sizeof(Ethernet::Header) == 14,
              "Ethernet::Header must be 14 bytes");
static_assert(sizeof(Ethernet::Address) == 6,
              "Ethernet::Address must be 6 bytes");

inline const Ethernet::Address Ethernet::BROADCAST(0xff, 0xff, 0xff, 0xff, 0xff,
                                                   0xff);

inline char * Ethernet::Address::to_string(char * buf) const
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 6; i++) {
        buf[i * 3 + 0] = hex[_addr[i] >> 4];
        buf[i * 3 + 1] = hex[_addr[i] & 0x0f];
        buf[i * 3 + 2] = ':';
    }
    buf[17] = '\0';
    return buf;
}

#endif // LIBVCOMM_ETHERNET_H
