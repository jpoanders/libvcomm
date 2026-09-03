#ifndef LIBVCOMM_ETHERNET_H
#define LIBVCOMM_ETHERNET_H

#include <cstring>
#include "../traits.h"

class Ethernet
{
public:
    static const unsigned int MTU = 1500;
    static const unsigned int HEADER_SIZE = 14;

    typedef unsigned short Protocol;

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

        operator bool() const
        {
            for (unsigned int i = 0; i < sizeof(_addr); i++)
                if (_addr[i])
                    return true;
            return false;
        }

        char * to_string(char * buf) const;

    private:
        unsigned char _addr[6];
    } __attribute__((packed));

    static const Address BROADCAST;

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
