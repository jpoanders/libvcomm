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

    const Address & address() const { return _address; }

private:
    // Called by the Protocol, from inside the signal handler.
    void update(const typename Channel::Observer::Observing_Condition & c,
                Buffer * buf) override;

    Channel * _channel;
    Address _address;
};

// -----------------------------------------------------------------------------

template <typename Channel>
Communicator<Channel>::Communicator(Channel * channel, Address address)
    : _channel(channel), _address(address)
{
    // TODO(joao): _channel->attach(this, _address);
}

template <typename Channel> Communicator<Channel>::~Communicator()
{
    // TODO(joao): _channel->detach(this, _address);
    // The PDF writes `Channel::detach(this, _address)` — a static call in an
    // instance method.  Use the instance form; record the deviation.
}

template <typename Channel>
bool Communicator<Channel>::send(const Message * message)
{
    // TODO(joao):
    //     return _channel->send(_address,
    //                           Address::broadcast(),
    //                           message->data(),
    //                           message->size()) > 0;
    (void)message;
    return false;
}

template <typename Channel>
bool Communicator<Channel>::receive(Message * message)
{
    // TODO(joao): the four most important lines in the library.
    //
    //     Buffer * buf = Observer::updated();   // <<< sleeps here until
    //     something arrives Address from; int size = _channel->receive(buf,
    //     &from, message->data(), Message::MAX_SIZE); if(size > 0) {
    //     message->size(size); return true; } return false;
    //
    // PITFALL: the PDF passes `message->size()` as the capacity.  On a
    // freshly constructed message that is 0 and you receive zero bytes forever,
    // with no error at all.  Pass the CAPACITY on the way in and write the
    // received size on the way out — do not use the same field for both.
    //
    // PITFALL 2: updated() may return 0 if you post the semaphore at shutdown
    // to unblock the application.  Handle that or the automated test will die
    // with a segfault on shutdown.
    //
    // PITFALL 3, new with the signal model: sem_wait() inside p() comes back
    // with EINTR when a frame arrives while you are waiting — UNLESS SIGIO's
    // sigaction has SA_RESTART.  Measured in both configurations on
    // 2026-08-23: without SA_RESTART -> -1/EINTR(4); with SA_RESTART -> 0, and
    // sem_wait resumes on its own.  sem.h already does the EINTR loop either
    // way, and that is how it should be: the loop costs two lines and holds
    // under both choices.  If you write your own p(), do not forget it.
    (void)message;
    return false;
}

template <typename Channel>
void Communicator<Channel>::update(
    const typename Channel::Observer::Observing_Condition & c, Buffer * buf)
{
    // TODO(joao): Observer::update(c, buf);
    // One line: enqueue and do v().  It is what releases the thread parked in
    // receive().  Do not process the message here — you are INSIDE A SIGNAL
    // HANDLER, with the interrupted thread stopped waiting for you to leave.
    (void)c;
    (void)buf;
}

#endif // LIBVCOMM_COMMUNICATOR_H
