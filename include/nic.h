#ifndef LIBVCOMM_NIC_H
#define LIBVCOMM_NIC_H

#include <arpa/inet.h>

#include "traits.h"
#include "ethernet.h"
#include "buffer.h"
#include "observer.h"

template <typename Engine>
class NIC : public Ethernet,
            public Conditionally_Data_Observed<::Buffer<Ethernet::Frame>,
                                               Ethernet::Protocol>,
            private Engine
{
public:
    // THE POOL IS PARTITIONED (decision closed 2026-08-24, §2.6 of
    // doc/design-decisions.md).  alloc() only ever takes from
    // [0, SEND_BUFFERS); handle() only ever takes from
    // [SEND_BUFFERS, BUFFER_SIZE).
    //
    // Why not one shared pool, which is what this code used to be: neither side
    // could reserve anything, so a transmission burst could leave handle() with
    // no buffer — and handle() runs in signal context and cannot wait, so the
    // frame is simply gone (rx_dropped).  The sender, by contrast, gets a 0
    // from alloc() and can retry.  Reception is the side that must not be
    // starvable, and the split makes the guaranteed RX depth a number we can
    // state rather than a race.
    //
    // The price: neither half can borrow from an idle other half.
    static const unsigned int BUFFER_SIZE =
        Traits<Ethernet>::SEND_BUFFERS + Traits<Ethernet>::RECEIVE_BUFFERS;

    typedef Ethernet::Address Address;
    typedef Ethernet::Protocol Protocol_Number;
    typedef ::Buffer<Ethernet::Frame> Buffer;

    typedef Conditional_Data_Observer<Buffer, Protocol_Number> Observer;
    typedef Conditionally_Data_Observed<Buffer, Protocol_Number> Observed;

    // The interface defaults to the one in Traits and every delivered path
    // uses that default.  The parameter exists so a test — or a bench run on
    // the host, where there is no eth0 — can point the same stack at another
    // interface without a rebuild.  tests/test_engine.cpp already does the
    // equivalent one layer down (VCOMM_TEST_IFACE).
    explicit NIC(const char * iface = Traits<Ethernet>::INTERFACE);
    ~NIC();

    NIC(const NIC &) = delete;
    NIC & operator=(const NIC &) = delete;

    bool valid() const { return Engine::engine_valid() && _armed; }

    int send(Address dst, Protocol_Number prot, const void * data,
             unsigned int size);

    // Takes a buffer from the TX HALF of the pool and builds the Ethernet
    // header in it.  Returns 0 when that half is exhausted — which is correct
    // behaviour, not an error.
    Buffer * alloc(Address dst, Protocol_Number prot, unsigned int size);

    int send(Buffer * buf);

    void free(Buffer * buf);

    // CONTRACT (decision closed 2026-08-24, doc/design-decisions.md §3):
    // returns FRAME BYTES MINUS HEADER — padding included — and NOT "payload
    // bytes".  The link layer has no length field of its own: Ethernet::Header
    // is the 14 bytes on the wire and a fifteenth would stop it being Ethernet.
    // On a medium that pads short frames to 60 bytes the two differ, and only
    // this one is achievable here.  Whoever needs the true payload size reads
    // Protocol::Header::_length, which travels in the frame.
    int unmarshal(Buffer * buf, Address * src, Address * dst, void * data,
                  unsigned int size);

    const Address & address();
    // void address(Address address);

    const Statistics & statistics() { return _statistics; }

private:
    void handle(Ethernet::Frame * frame, unsigned int size) override;
    Statistics _statistics;
    Buffer _buffer[BUFFER_SIZE];
    bool _armed;
};

template <typename Engine>
NIC<Engine>::NIC(const char * iface)
    : Engine(iface, Traits<Ethernet>::PROTOCOL_NUMBER), _statistics(), _buffer(),
      _armed(false)
{
    if (Engine::engine_valid())
        _armed = Engine::engine_start();
}

template <typename Engine> NIC<Engine>::~NIC()
{
    Engine::engine_stop();
    _armed = false;
}

template <typename Engine>
int NIC<Engine>::send(Address dst, Protocol_Number prot, const void * data,
                      unsigned int size)
{
    Buffer * buf = alloc(dst, prot, size);
    if (!buf)
        return -1;
    std::memcpy(buf->frame()->data, data, size);
    return send(buf);
}

// no use case for a synchronous reception method yet
// template <typename Engine>
// int NIC<Engine>::receive(Address * src, Protocol_Number * prot, void * data,
//                         unsigned int size)
//{
//    // The PDF provides for this synchronous method.  In the
//    // Observer-oriented architecture it is redundant with the
//    handle()/notify()
//    // pair; implemented only if needed
//    (void)src;
//    (void)prot;
//    (void)data;
//    (void)size;
//    return -1;
//}

template <typename Engine>
typename NIC<Engine>::Buffer *
NIC<Engine>::alloc(Address dst, Protocol_Number prot, unsigned int size)
{
    for (unsigned int i = 0; i < Traits<Ethernet>::SEND_BUFFERS; i++) {
        if (_buffer[i].lock()) {
            Ethernet::Frame * f = _buffer[i].frame();
            f->dst = dst;
            f->src = Engine::engine_address();
            f->prot = htons(prot);
            _buffer[i].size(Ethernet::HEADER_SIZE + size);
            return &_buffer[i];
        }
    }
    return 0;
}

template <typename Engine> int NIC<Engine>::send(Buffer * buf)
{
    int ret = Engine::engine_send(buf->frame(), buf->size());
    if (ret > 0) {
        _statistics.tx_packets++;
        _statistics.tx_bytes += ret;
    }
    free(buf);
    return ret;
}

template <typename Engine> void NIC<Engine>::free(Buffer * buf)
{
    if (buf)
        buf->unlock();
}

template <typename Engine>
int NIC<Engine>::unmarshal(Buffer * buf, Address * src, Address * dst,
                           void * data, unsigned int size)
{
    const Ethernet::Frame * f = buf->frame();
    if (src)
        *src = f->src;
    if (dst)
        *dst = f->dst;

    unsigned int payload_bytes = buf->size() - Ethernet::HEADER_SIZE;
    unsigned int to_copy = (payload_bytes < size) ? payload_bytes : size;
    std::memcpy(data, f->data, to_copy);
    return static_cast<int>(to_copy);
}

template <typename Engine>
const typename NIC<Engine>::Address & NIC<Engine>::address()
{
    return Engine::engine_address();
}

// no use case for this setter yet
// template <typename Engine> void NIC<Engine>::address(Address address)
//{
//    (void)address;
//}

// since alloc() fills in a SEND header, this method allocs a reception buffer.
template <typename Engine>
void NIC<Engine>::handle(Ethernet::Frame * frame, unsigned int size)
{
    // Do not trust the Engine's filtering: NIC must hold for any Engine.
    if (size < Ethernet::HEADER_SIZE || size > sizeof(Ethernet::Frame))
        return;

    const unsigned int payload = size - Ethernet::HEADER_SIZE;

    // The RX HALF, and only it.  See the note on the pool split above alloc().
    Buffer * buf = 0;
    for (unsigned int i = Traits<Ethernet>::SEND_BUFFERS; i < BUFFER_SIZE; i++) {
        if (_buffer[i].lock()) {
            buf = &_buffer[i];
            break;
        }
    }

    if (!buf) {
        _statistics.rx_dropped++; // no buffer
        return;
    }

    Ethernet::Frame * f = buf->frame();
    f->dst = frame->dst;
    f->src = frame->src;
    f->prot = frame->prot; // stays in network order, as alloc() leaves it
    std::memcpy(f->data, frame->data, payload);
    buf->size(size); // total, header included

    if (Observed::notify(ntohs(f->prot), buf)) {
        _statistics.rx_packets++;
        _statistics.rx_bytes += size;
    } else {
        _statistics.rx_dropped++; // no observer for this EtherType
        free(buf);
    }
}

#endif // LIBVCOMM_NIC_H
