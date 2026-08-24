#ifndef LIBVCOMM_NIC_H
#define LIBVCOMM_NIC_H

#include <arpa/inet.h>

#include "traits.h"
#include "ethernet.h"
#include "buffer.h"
#include "observer.h"

// =============================================================================
// NIC<Engine> — the PORTABLE abstraction of the network card.
//
// No syscalls in here.  The NIC knows how to build a frame, manage a buffer
// pool and notify whoever registered for an EtherType.  How the bytes reach the
// medium is the Engine's problem, and the Engine comes in through private
// inheritance.
//
// WHY `private Engine` AND NOT A MEMBER?
//   Private inheritance gives access to the Engine's protected methods without
//   exposing any of it outside the NIC, and it allows overriding handle() —
//   which is how the Engine pushes frames upward.  With a member you would need
//   a back pointer or a std::function.  Both work; this is EPOS's choice and
//   the cheapest one.  Know how to defend it.
//
// WHY IS THE NIC OBSERVED INSTEAD OF CALLING THE PROTOCOL DIRECTLY?
//   Because one NIC may serve several protocols at once, each with its own
//   EtherType, and it cannot know any of them.  Observer is what inverts that
//   dependency.
// =============================================================================

template <typename Engine>
class NIC : public Ethernet,
            public Conditionally_Data_Observed<::Buffer<Ethernet::Frame>,
                                               Ethernet::Protocol>,
            private Engine
{
public:
    static const unsigned int BUFFER_SIZE =
        Traits<Ethernet>::SEND_BUFFERS + Traits<Ethernet>::RECEIVE_BUFFERS;

    // These typedefs hide the ones from both bases (Ethernet and Engine, which
    // both have an `Address`).  Without them, `Address` inside the NIC is
    // ambiguous.
    typedef Ethernet::Address Address;
    typedef Ethernet::Protocol Protocol_Number;
    typedef ::Buffer<Ethernet::Frame> Buffer;

    typedef Conditional_Data_Observer<Buffer, Protocol_Number> Observer;
    typedef Conditionally_Data_Observed<Buffer, Protocol_Number> Observed;

    // The PDF marks the constructor protected (the NIC is a singleton,
    // instantiated by Meta/Traits in EPOS).  Here it is public: without a
    // factory, protected would make the class unusable.  See
    // doc/design-decisions.md.
    NIC();
    ~NIC();

    // -------------------------------------------------------------------------
    // Simple (synchronous) path: copies `data` into a frame and sends it.
    // Contract: returns PAYLOAD bytes sent, or -1.
    // -------------------------------------------------------------------------
    int send(Address dst, Protocol_Number prot, const void * data,
             unsigned int size);

    // Extracts from an already received buffer.  Contract: returns payload
    // bytes copied into `data`, at most `size`.
    int receive(Address * src, Protocol_Number * prot, void * data,
                unsigned int size);

    // -------------------------------------------------------------------------
    // Zero-copy path: the sender asks for a buffer, writes straight into it and
    // sends.
    // -------------------------------------------------------------------------
    // Reserves a buffer from the pool and ALREADY FILLS the header (dst,
    // src=our MAC, prot).  Contract: returns 0 if the pool is exhausted — and
    // returning 0 is normal behaviour, not a fatal error; the caller decides.
    Buffer * alloc(Address dst, Protocol_Number prot, unsigned int size);

    // Sends a buffer built by alloc().  Contract: releases the buffer at the
    // end, whether it succeeded or not.  Document that choice: the alternative
    // is for the caller to release it, and mixing the two is a guaranteed leak.
    int send(Buffer * buf);

    // Returns the buffer to the pool.
    void free(Buffer * buf);

    // Unpacks a received buffer: who sent it, to whom, and the payload.
    // Contract: returns payload bytes copied, or -1.
    int unmarshal(Buffer * buf, Address * src, Address * dst, void * data,
                  unsigned int size);

    const Address & address();
    void address(Address address);

    const Statistics & statistics() { return _statistics; }

    // attach()/detach() come from Conditionally_Data_Observed (public base).
    // The PDF lists them with the comment "possibly inherited" — this is that
    // case.

private:
    // Called FROM INSIDE THE ENGINE'S SIGNAL HANDLER, for every frame that
    // passed the filters.  It is the entry point of everything that goes up.
    void handle(Ethernet::Frame * frame, unsigned int size) override;

    Statistics _statistics;
    Buffer _buffer[BUFFER_SIZE];
};

// -----------------------------------------------------------------------------
// Implementation.  Template => everything in the header.
// -----------------------------------------------------------------------------

template <typename Engine>
NIC<Engine>::NIC()
    : Engine(Traits<Ethernet>::INTERFACE, Traits<Ethernet>::PROTOCOL_NUMBER)
{
    // TODO(joao): arm reception — and only AFTER _buffer and _statistics
    // already exist, because from here on handle() may be called at ANY
    // INSTRUCTION, on whichever thread happens to be running when the signal
    // arrives.
    //     Engine::engine_start();
}

template <typename Engine> NIC<Engine>::~NIC()
{
    // TODO(joao): stop the Engine BEFORE destroying the buffers.
    //     Engine::engine_stop();
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

template <typename Engine>
int NIC<Engine>::receive(Address * src, Protocol_Number * prot, void * data,
                         unsigned int size)
{
    // TODO(joao): the PDF provides for this synchronous method.  In the
    // Observer-oriented architecture it is redundant with the handle()/notify()
    // pair; implement it only if your test needs it.  If it does not, leave it
    // returning -1 and EXPLAIN the decision in doc/ — a documented decision is
    // worth more than a dead method.
    (void)src;
    (void)prot;
    (void)data;
    (void)size;
    return -1;
}

template <typename Engine>
typename NIC<Engine>::Buffer *
NIC<Engine>::alloc(Address dst, Protocol_Number prot, unsigned int size)
{
    for (unsigned int i = 0; i < BUFFER_SIZE; i++) {
        if (_buffer[i].lock()) {
            Ethernet::Frame * f = _buffer[i].frame();
            f->dst  = dst;
            f->src  = Engine::engine_address();
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
    if (src) *src = f->src;
    if (dst) *dst = f->dst;

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

template <typename Engine> void NIC<Engine>::address(Address address)
{
    // TODO(joao): Stage 1 uses eth0's real MAC and lets nobody change it.  This
    // setter exists in the PDF's API; decide whether it makes sense here and
    // document it.  A method that lies is worse than a missing one.
    (void)address;
}

template <typename Engine>
void NIC<Engine>::handle(Ethernet::Frame * frame, unsigned int size)
{
    // TODO(joao): the bridge between the Engine and the rest of the stack.
    //
    //   1. alloc a reception buffer (or scan the pool directly — but note that
    //      alloc() fills in a SEND header; you may want a separate
    //      alloc_receive().  Your call, document it).
    //      No free buffer: _statistics.rx_dropped++ and RETURN.  Dropping is a
    //      legitimate answer; waiting for a buffer inside a handler is not.
    //
    //   MIND THE CONTEXT: everything here runs inside the signal handler.  No
    //   printf for debugging (use write(2)), no new.  The pool is preallocated
    //   for exactly that reason.
    //   2. copy `frame` into the buffer (the Engine's memory dies on return).
    //   3. buf->size(size); update rx_packets/rx_bytes.
    //   4. Observed::notify(ntohs(frame->prot), buf);
    //      -> if it returns false, NOBODY wanted the frame: free(buf).  This
    //         `if` is the difference between running all night and leaking 32
    //         buffers.
    (void)frame;
    (void)size;
}

#endif // LIBVCOMM_NIC_H
