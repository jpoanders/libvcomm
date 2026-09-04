#ifndef LIBVCOMM_PROTOCOL_H
#define LIBVCOMM_PROTOCOL_H

#include <cstring>
#include "traits.h"
#include "net/ethernet.h"
#include "observer.h"

template <typename NIC_T> class Protocol : private NIC_T::Observer
{
public:
    typedef typename NIC_T::Buffer Buffer;
    typedef typename NIC_T::Address PhysicalAddress;
    typedef typename NIC_T::ProtocolNumber ProtocolNumber;

    typedef unsigned short Port;

    static const ProtocolNumber PROTO = Traits<Ethernet>::PROTOCOL_NUMBER;

    typedef Concurrent_Observer<Buffer, Port> Observer;
    typedef Concurrent_Observed<Buffer, Port> Observed;

    class Address
    {
    public:
        enum Null
        {
            NULL_ADDRESS = 0
        };

        Address() : _paddr(), _port(0) {}
        Address(const Null &) : _paddr(), _port(0) {}
        Address(PhysicalAddress paddr, Port port) : _paddr(paddr), _port(port)
        {}

        const PhysicalAddress & paddr() const { return _paddr; }
        Port port() const { return _port; }

        operator bool() const { return (_paddr || _port); }
        bool operator==(const Address & a) const
        {
            return (_paddr == a._paddr) && (_port == a._port);
        }
        bool operator!=(const Address & a) const { return !(*this == a); }

        static Address broadcast(Port p = 0)
        {
            return Address(Ethernet::BROADCAST, p);
        }

    private:
        PhysicalAddress _paddr;
        Port _port;
    } __attribute__((packed));

    class Header
    {
    public:
        Header() : _from_port(0), _to_port(0), _length(0) {}

        Port _from_port;
        Port _to_port;
        unsigned short _length;
    } __attribute__((packed));

    static const unsigned int MTU = Ethernet::MTU - sizeof(Header);
    typedef unsigned char Data[MTU];

    class Packet : public Header
    {
    public:
        Packet() {}
        Header * header() { return this; }

        template <typename T> T * data()
        {
            return reinterpret_cast<T *>(&_data);
        }

    private:
        Data _data;
    } __attribute__((packed));

    // -------------------------------------------------------------------------
    explicit Protocol(NIC_T * nic);
    ~Protocol();

    int send(Address from, Address to, const void * data, unsigned int size);

    int receive(Buffer * buf, Address * from, void * data, unsigned int size);

    void attach(Observer * obs, const Address & address);

    void detach(Observer * obs, const Address & address);

private:
    void update(const ProtocolNumber & prot, Buffer * buf) override;

    NIC_T * _nic;
    Observed _observed;
};

// -----------------------------------------------------------------------------

template <typename NIC_T>
Protocol<NIC_T>::Protocol(NIC_T * nic) : NIC_T::Observer(PROTO), _nic(nic)
{
    _nic->attach(this, PROTO);
}

template <typename NIC_T> Protocol<NIC_T>::~Protocol()
{
    _nic->detach(this, PROTO);
}

template <typename NIC_T>
int Protocol<NIC_T>::send(Address from, Address to, const void * data,
                          unsigned int size)
{
    Buffer * buf = _nic->alloc(to.paddr(), PROTO, sizeof(Header) + size);
    if (!buf)
        return -1;

    // nic alloc already filled the ethernet header (dst, src, prot)
    Packet * pkt = reinterpret_cast<Packet *>(buf->frame()->data);
    pkt->_from_port = from.port();
    pkt->_to_port = to.port();
    pkt->_length = size;

    // copy application data right after the protocol header
    std::memcpy(pkt->template data<void>(), data, size);

    return _nic->send(buf);
}

template <typename NIC_T>
int Protocol<NIC_T>::receive(Buffer * buf, Address * from, void * data,
                             unsigned int size)
{
    PhysicalAddress src_mac;
    unsigned char raw_payload[Ethernet::MTU];
    int payload_bytes =
        _nic->unmarshal(buf, &src_mac, 0, raw_payload, sizeof(raw_payload));

    if (payload_bytes < static_cast<int>(sizeof(Header))) {
        _nic->free(buf); // always free, even on error path
        return -1;
    }

    const Header * hdr = reinterpret_cast<const Header *>(raw_payload);

    if (from)
        *from = Address(src_mac, hdr->_from_port);

    const unsigned int arrived =
        static_cast<unsigned int>(payload_bytes) - sizeof(Header);
    unsigned int data_bytes = hdr->_length;
    if (data_bytes > arrived)
        data_bytes = arrived;
    unsigned int to_copy = (data_bytes < size) ? data_bytes : size;
    std::memcpy(data, raw_payload + sizeof(Header), to_copy);

    _nic->free(buf); // always free, ownership ends here
    return static_cast<int>(to_copy);
}

template <typename NIC_T>
void Protocol<NIC_T>::attach(Observer * obs, const Address & address)
{
    _observed.attach(obs, address.port());
}

template <typename NIC_T>
void Protocol<NIC_T>::detach(Observer * obs, const Address & address)
{
    _observed.detach(obs, address.port());
}

template <typename NIC_T>
void Protocol<NIC_T>::update(const ProtocolNumber & prot, Buffer * buf)
{
    (void)prot;

    const Packet * pkt = reinterpret_cast<const Packet *>(buf->frame()->data);

    if (!_observed.notify(pkt->_to_port, buf))
        _nic->free(buf);
}

#endif // LIBVCOMM_PROTOCOL_H
