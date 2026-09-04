#ifndef LIBVCOMM_TEST_LOOPBACK_ENGINE_H
#define LIBVCOMM_TEST_LOOPBACK_ENGINE_H

#include <cstring>
#include "ethernet.h"


class Loopback_Engine
{
public:
    // false = the frame leaves and never arrives, to exercise a loss.
    inline static bool deliver = true;

    // Frames handed to engine_send(), whether or not they were delivered.
    inline static unsigned int sent = 0;

    static void reset()
    {
        deliver = true;
        sent = 0;
    }

protected:
    typedef Ethernet::Address Address;
    typedef Ethernet::Protocol Protocol;

    Loopback_Engine(const char * iface, Protocol prot)
        : _address(0x02, 0x00, 0x00, 0x00, 0x00, 0x2a), _protocol(prot),
          _armed(false)
    {
        (void)iface;
    }

    virtual ~Loopback_Engine() {}

    Loopback_Engine(const Loopback_Engine &) = delete;
    Loopback_Engine & operator=(const Loopback_Engine &) = delete;

    int engine_send(const Ethernet::Frame * frame, unsigned int size)
    {
        sent++;

        if (_armed && deliver) {
            // copy
            Ethernet::Frame copy;
            std::memcpy(&copy, frame, size);
            handle(&copy, size);
        }

        return static_cast<int>(size);
    }

    bool engine_start()
    {
        _armed = true;
        return true;
    }

    void engine_stop() { _armed = false; }

    const Address & engine_address() const { return _address; }

    bool engine_valid() const { return true; }

    unsigned int engine_rx_errors() const { return 0; }
    int engine_rx_error() const { return 0; }

    virtual void handle(Ethernet::Frame * frame, unsigned int size) = 0;

private:
    Address _address;
    Protocol _protocol;
    bool _armed;
};

#endif // LIBVCOMM_TEST_LOOPBACK_ENGINE_H
