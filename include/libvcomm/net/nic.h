#ifndef LIBVCOMM_NIC_H
#define LIBVCOMM_NIC_H

#include <arpa/inet.h>

#include "libvcomm/traits.h"
#include "libvcomm/net/ethernet.h"
#include "buffer.h"
#include "libvcomm/observer.h"

template <typename Engine>
class NIC : public Ethernet,
            public Conditionally_Data_Observed<Buffer<Ethernet::Frame>,
                                               Ethernet::Protocol>,
            private Engine
{
public:
    static const unsigned int BUFFER_SIZE =
        Traits<Ethernet>::SEND_BUFFERS + Traits<Ethernet>::RECEIVE_BUFFERS;

    typedef Ethernet::Address Address;
    typedef Ethernet::Protocol ProtocolNumber;
    typedef Buffer<Ethernet::Frame> Buffer;
    typedef Conditional_Data_Observer<Buffer, ProtocolNumber> Observer;
    typedef Conditionally_Data_Observed<Buffer, ProtocolNumber> Observed;

    explicit NIC(const char * iface = Traits<Ethernet>::INTERFACE);
    ~NIC();

    NIC(const NIC &) = delete;
    NIC & operator=(const NIC &) = delete;

    bool valid() const { return Engine::valid() && _armed; }

    // int send(Ethernet::Address dst, Ethernet::Protocol prot, const void *
    // data,
    //          unsigned int size);

    // allocate a SEND buffer with <size> payload bytes
    Buffer * alloc(Address dst, ProtocolNumber prot, unsigned int size);

    int send(Buffer * buf);

    void free(Buffer * buf);

    int unmarshal(Buffer * buf, Address * src, Address * dst, void * data,
                  unsigned int size);

    const Address & address();

    const Statistics & statistics() { return _statistics; }

private:
    enum class BufferType
    {
        SEND,
        RECEIVE
    };

    int send(const void * data, unsigned int size) override;

    int receive(void * data, unsigned int size) override;

    Buffer * alloc_by_type(BufferType type, Address src, Address dst,
                           ProtocolNumber prot, unsigned int frame_size);

    Statistics _statistics;
    Buffer _buffer[BUFFER_SIZE];
    bool _armed;
};

template <typename Engine>
NIC<Engine>::NIC(const char * iface)
    : Engine(iface, Traits<Ethernet>::PROTOCOL_NUMBER), _statistics(),
      _buffer(), _armed(false)
{
    if (Engine::valid())
        _armed = Engine::start();
}

template <typename Engine> NIC<Engine>::~NIC()
{
    Engine::stop();
    _armed = false;
}

// template <typename Engine>
// int NIC<Engine>::send(Ethernet::Address dst, Ethernet::Protocol prot,
//                       const void * data, unsigned int size)
//{
//     Buffer * buf =
//         alloc(BufferType::SEND, dst, prot, Ethernet::HEADER_SIZE + size);
//     if (!buf)
//         return -1;
//     std::memcpy(buf->frame()->data, data, size);
//     return send(buf);
// }

template <typename Engine>
typename NIC<Engine>::Buffer *
NIC<Engine>::alloc_by_type(BufferType type, Address src, Address dst,
                           ProtocolNumber prot, unsigned int size)
{
    unsigned int begin =
        (type == BufferType::SEND) ? 0 : Traits<Ethernet>::SEND_BUFFERS;

    unsigned int end = (type == BufferType::SEND)
                           ? Traits<Ethernet>::SEND_BUFFERS
                           : BUFFER_SIZE;

    for (unsigned int i = begin; i < end; i++) {
        if (_buffer[i].lock()) {
            Ethernet::Frame * f = _buffer[i].frame();
            f->dst = dst;
            f->src = src;
            f->prot = htons(prot);
            _buffer[i].size(Ethernet::HEADER_SIZE + size);
            return &_buffer[i];
        }
    }
    return 0;
}

template <typename Engine>
typename NIC<Engine>::Buffer *
NIC<Engine>::alloc(Address dst, ProtocolNumber prot, unsigned int size)
{
    return alloc_by_type(BufferType::SEND, Engine::address(), dst, prot, size);
}

template <typename Engine>
int NIC<Engine>::send(const void * data, unsigned int size)
{
    return Engine::send(data, size);
}

template <typename Engine> int NIC<Engine>::send(Buffer * buf)
{
    int ret = Engine::send(buf->frame(), buf->size());
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

    unsigned int payload_size = buf->size() - Ethernet::HEADER_SIZE;
    unsigned int to_copy = (payload_size < size) ? payload_size : size;
    std::memcpy(data, f->data, to_copy);
    return static_cast<int>(to_copy);
}

template <typename Engine>
const typename NIC<Engine>::Address & NIC<Engine>::address()
{
    return Engine::address();
}

// since alloc() fills in a SEND header, this method allocs a reception buffer.
template <typename Engine>
int NIC<Engine>::receive(void * data, unsigned int size)
{
    Ethernet::Frame * frame = reinterpret_cast<Ethernet::Frame *>(data);
    // Do not trust the Engine's filtering: NIC must hold for any Engine.
    if (size < Ethernet::HEADER_SIZE || size > sizeof(Ethernet::Frame))
        return -1;

    const unsigned int payload_size = size - Ethernet::HEADER_SIZE;

    // The RX HALF, and only it.  See the note on the pool split above alloc().
    Buffer * buf = 0;

    buf = alloc_by_type(BufferType::RECEIVE, frame->src, frame->dst,
                        ntohs(frame->prot), payload_size);

    if (!buf) {
        _statistics.rx_dropped++; // no buffer
        return -2;
    }

    Ethernet::Frame * buf_frame = buf->frame();
    std::memcpy(buf_frame->data, frame->data, payload_size);

    if (!Observed::notify(ntohs(buf_frame->prot), buf)) {
        _statistics.rx_dropped++; // no observer for this EtherType
        free(buf);
        return -2;
    }
    _statistics.rx_packets++;
    _statistics.rx_bytes += size;
    return size;
}

#endif // LIBVCOMM_NIC_H
