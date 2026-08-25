#ifndef LIBVCOMM_COMMUNICATOR_H
#define LIBVCOMM_COMMUNICATOR_H

#include "observer.h"
#include "message.h"

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

    bool send(const Message * message);

    bool receive(Message * message);

    bool receive(Message * message, unsigned int timeout_ms);

    const Address & address() const { return _address; }

private:

    bool update(const typename Channel::Observer::Observing_Condition & c,
                Buffer * buf) override;

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
