#ifndef LIBVCOMM_COMMUNICATOR_H
#define LIBVCOMM_COMMUNICATOR_H

#include "observer.h"
#include "message.h"

// =============================================================================
// Communicator<Channel> — the application's ONLY point of contact with the
// stack.
//
// A sensor, a fuser, an ECU: none of them knows about frames, EtherTypes or
// sockets.  They all see send(Message*) and receive(Message*).  This class is
// what fulfils the requirement of a "unified API for all agents, regardless of
// whether they are autonomous systems or components thereof".
//
// receive() BLOCKS — and it is the only point in the library that blocks.  It
// blocks on the semaphore inherited from Concurrent_Observer, not on
// recvfrom().  That distinction is the answer to "where is the required
// asynchrony?": the signal handler never waits for the application; it deposits
// and moves on.
//
// Note that the Communicator does not address the destination explicitly: it
// always sends broadcast.  Who answers whom is a matter for the messages
// (Stage 2 onwards).
// =============================================================================

template <typename Channel>
class Communicator : public Concurrent_Observer<
                         typename Channel::Observer::Observed_Data,
                         typename Channel::Observer::Observing_Condition>
{
    typedef Concurrent_Observer<typename Channel::Observer::Observed_Data,
                                typename Channel::Observer::Observing_Condition>
        Observer;

public:
    typedef typename Channel::Buffer Buffer;
    typedef typename Channel::Address Address;

    Communicator(Channel * channel, Address address);
    ~Communicator();

    Communicator(const Communicator &) = delete;
    Communicator & operator=(const Communicator &) = delete;

    // Contract: true if the message was handed to the kernel.  It does not
    // guarantee anyone received it — broadcast has no acknowledgement.  Be
    // careful about counting this as "message delivered" in the presentation's
    // statistics.
    bool send(const Message * message);

    // Contract: BLOCKS until a message arrives for this address.  Returns true
    // and fills `message` with the payload and the REAL size received.
    bool receive(Message * message);

    // Same, with a ceiling.  Returns false if nothing arrived within
    // timeout_ms.  This is what lets an automated receiver give up and REPORT
    // instead of hanging until the fleet timeout kills its VM with no verdict.
    bool receive(Message * message, unsigned int timeout_ms);

    const Address & address() const { return _address; }

private:
    // Called by the Protocol, from inside the signal handler.  Returns false
    // when the queue is full — see Concurrent_Observer::update.
    bool update(const typename Channel::Observer::Observing_Condition & c,
                Buffer * buf) override;

    // Shared tail of the two receive()s: consumes a buffer the Protocol handed
    // over.  The buffer is released by the Channel, on every path.
    bool consume(Buffer * buf, Message * message);

    Channel * _channel;
    Address _address;
};

// -----------------------------------------------------------------------------

template <typename Channel>
Communicator<Channel>::Communicator(Channel * channel, Address address)
    : _channel(channel), _address(address)
{
    _channel->attach(this, _address);
}

template <typename Channel> Communicator<Channel>::~Communicator()
{
    _channel->detach(this, _address);
}

template <typename Channel>
bool Communicator<Channel>::send(const Message * message)
{
    // BROADCAST TO OUR OWN PORT, and that port is the whole addressing scheme.
    //
    // This used to be Address::broadcast(), whose port defaults to 0.  The
    // Protocol wrote that 0 into Packet::_to_port and, on the way up, notified
    // condition 0 — which no Communicator is ever attached to.  Every message
    // was delivered to nobody and freed, silently.  Nothing on the host caught
    // it because nothing tested this layer; tests/test_protocol.cpp does now.
    //
    // Sending to our own port gives the port channel semantics: agents that
    // share a port hear each other, which is what the assignment means by "the
    // Communicator does not use explicit addressing".  Identifying who sent
    // what is the message's job, not the address's.
    return _channel->send(_address, Address::broadcast(_address.port()),
                          message->data(), message->size()) > 0;
}

template <typename Channel>
bool Communicator<Channel>::receive(Message * message)
{
    return consume(Observer::updated(), message); // <<< blocks (semaphore p())
}

template <typename Channel>
bool Communicator<Channel>::receive(Message * message, unsigned int timeout_ms)
{
    return consume(Observer::updated(timeout_ms), message);
}

template <typename Channel>
bool Communicator<Channel>::consume(Buffer * buf, Message * message)
{
    if (!buf)
        return false; // timed out, or woken with nothing to take

    Address from;
    unsigned char tmp[Message::MAX_SIZE];
    int size = _channel->receive(buf, &from, tmp, Message::MAX_SIZE);
    // buf is freed by _channel->receive() do not touch buf after this

    if (size > 0) {
        message->set(tmp, static_cast<unsigned int>(size));
        return true;
    }
    return false;
}

template <typename Channel>
bool Communicator<Channel>::update(
    const typename Channel::Observer::Observing_Condition & c, Buffer * buf)
{
    return Observer::update(c, buf);
}

#endif // LIBVCOMM_COMMUNICATOR_H
