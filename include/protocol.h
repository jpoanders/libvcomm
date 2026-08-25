#ifndef LIBVCOMM_PROTOCOL_H
#define LIBVCOMM_PROTOCOL_H

#include <cstring>
#include "traits.h"
#include "ethernet.h"
#include "observer.h"

// =============================================================================
// Protocol<NIC> — multiplexes ONE NIC across SEVERAL peers.
//
// The NIC separates by EtherType (every project frame has the same one).  The
// Protocol separates by Port: it is what decides which process, inside the VM,
// should wake up with the message that arrived.
//
// TWO FACES, and this is what confuses on a first reading:
//
//   - As an OBSERVER of the NIC: it inherits NIC::Observer and implements
//     update().  Runs INSIDE THE SIGNAL HANDLER, must not block.
//   - As OBSERVED by the Communicators: it holds a
//     Concurrent_Observed<Buffer,Port>
//     inside itself.  That one does decouple through a semaphore — on the other
//     side there is an application thread asleep.
//
// It is this fold that makes reception asynchronous end to end without a
// rendezvous.
// =============================================================================
//
// DEVIATIONS FROM THE PDF (all recorded in doc/design-decisions.md):
//
//  1. The PDF writes `class Protocol: private typename NIC::Observer`.  The
//     `typename` cannot appear in a base list — in a base-clause the name is
//     already read as a type.  Removed.
//
//  2. The PDF declares `typedef Conditional_Data_Observer<..., Port> Observer;`
//     but the PDF's own Communicator inherits from Concurrent_Observer.  The
//     two do not fit together.  The Communicator wins: here Observer is
//     Concurrent_Observer — it is the one with the semaphore the application
//     needs.
//
//  3. The PDF declares send()/receive() `static` but uses `_nic`, which is an
//     instance member; and the Communicator calls them as
//     `_channel->send(...)`.  Here they are instance methods.
//
//  4. The PDF has `static Observed _observed;` ("channel protocols are usually
//     singletons").  A static member of a template requires an out-of-class
//     definition and prevents two protocols in the same process.  Here it is an
//     instance member.

template <typename NIC_T> class Protocol : private NIC_T::Observer
{
public:
    typedef typename NIC_T::Buffer Buffer;
    typedef typename NIC_T::Address Physical_Address;
    typedef typename NIC_T::Protocol_Number Protocol_Number;

    // The PDF literally writes `typedef XXX Port;` — the choice is yours.
    // unsigned short covers Stage 1.  In Stage 2 the assignment provokes you:
    // using the PID is tempting and cheap, but how would VM 2 know the PID of a
    // process on VM 1?  Decide now or pay later.
    typedef unsigned short Port;

    static const Protocol_Number PROTO = Traits<Ethernet>::PROTOCOL_NUMBER;

    typedef Concurrent_Observer<Buffer, Port> Observer;
    typedef Concurrent_Observed<Buffer, Port> Observed;

    // -------------------------------------------------------------------------
    // Address = (physical address, port).  It is the library's LOGICAL address.
    // -------------------------------------------------------------------------
    class Address
    {
    public:
        enum Null
        {
            NULL_ADDRESS = 0
        };

        Address() : _paddr(), _port(0) {}
        Address(const Null &) : _paddr(), _port(0) {}
        Address(Physical_Address paddr, Port port) : _paddr(paddr), _port(port)
        {}

        const Physical_Address & paddr() const { return _paddr; }
        Port port() const { return _port; }

        operator bool() const { return (_paddr || _port); }
        bool operator==(const Address & a) const
        {
            return (_paddr == a._paddr) && (_port == a._port);
        }
        bool operator!=(const Address & a) const { return !(*this == a); }

        // The PDF writes `Channel::Address::BROADCAST`.  A static member of a
        // nested class inside a template needs an out-of-class definition; a
        // static function gives the same result without the ceremony.
        static Address broadcast(Port p = 0)
        {
            return Address(Ethernet::BROADCAST, p);
        }

    private:
        Physical_Address _paddr;
        Port _port;
    } __attribute__((packed));

    // -------------------------------------------------------------------------
    // Header / Packet — what the Protocol adds to the Ethernet payload.
    //
    // >>> YOUR OWN DESIGN DECISION, and it has a real consequence:
    //     you MEASURED that QEMU's virtio-net does not pad to 60 bytes, and
    //     that is why `size = bytes_received - 14` works on this bench.  On
    //     real hardware, and on Stage 2's shared-memory Engine, it does not.
    //     If you want the correct size on any medium, a `length` field here
    //     settles it once and for all.  Stages 2 through 6 will ask for origin,
    //     timestamp, type and MAC — this Header is where they go.
    // -------------------------------------------------------------------------
    class Header
    {
    public:
        Header() : _from_port(0), _to_port(0), _length(0) {}

        // TODO(joao): confirm which fields Stage 1 actually needs and add the
        // accessors.  Less is more here — every byte of this header is payload
        // you lose.
        Port _from_port;
        Port _to_port;
        unsigned short _length; // payload bytes; see the note above
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

    // Contract: returns payload bytes sent, or -1.
    int send(Address from, Address to, const void * data, unsigned int size);

    // Consumes an ALREADY received buffer (the Communicator got it from
    // updated()).  Contract: fills `from`, copies up to `size` bytes and
    // RELEASES the buffer — success or not.  Ownership ends here.
    int receive(Buffer * buf, Address * from, void * data, unsigned int size);

    void attach(Observer * obs, const Address & address);
    void detach(Observer * obs, const Address & address);

private:
    // Called by the NIC, INSIDE THE SIGNAL HANDLER.  Must not block, and is
    // subject to async-signal-safety (man 7 signal-safety).
    void update(const Protocol_Number & prot, Buffer * buf) override;

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
    // unmarshal the ethernet layer to get the source MAC and raw payload
    Physical_Address src_mac;
    unsigned char raw_payload[Ethernet::MTU];
    int payload_bytes =
        _nic->unmarshal(buf, &src_mac, 0, raw_payload, sizeof(raw_payload));

    if (payload_bytes < static_cast<int>(sizeof(Header))) {
        _nic->free(buf); // always free, even on error path
        return -1;
    }

    // read the protocol header from the start of the payload
    const Header * hdr = reinterpret_cast<const Header *>(raw_payload);

    if (from)
        *from = Address(src_mac, hdr->_from_port);

    // Use the _length field from the header: reliable across all engines,
    // unlike frame-length arithmetic (see NIC::unmarshal's contract).
    //
    // CLAMPED first.  _length arrives from the wire and nothing so far has
    // checked it against what actually turned up.  to_copy is already bounded
    // by the caller's `size`, so a bogus value could not overflow `data` — but
    // it could read past the end of the bytes unmarshalled into raw_payload.
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
void Protocol<NIC_T>::update(const Protocol_Number & prot, Buffer * buf)
{
    (void)prot;

    const Packet * pkt = reinterpret_cast<const Packet *>(buf->frame()->data);

    if (!_observed.notify(pkt->_to_port, buf))
        _nic->free(buf);
}

#endif // LIBVCOMM_PROTOCOL_H
