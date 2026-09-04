#ifndef LIBVCOMM_TEST_LOOPBACK_ENGINE_H
#define LIBVCOMM_TEST_LOOPBACK_ENGINE_H

#include <cstring>
#include "ethernet.h"

// =============================================================================
// Loopback_Engine — an Engine for TESTS ONLY.  No socket, no privileges, no
// kernel.  engine_send() hands the frame straight back to handle().
//
// This is the class that proves the Engine abstraction is real: NIC, Protocol
// and Communicator compile against it without one line changing, which is the
// same promise Stage 2's Shared_Memory_Engine will cash in.  If some syscall
// ever leaks out of Raw_Socket_Engine, THIS FILE STOPS COMPILING — which makes
// it a regression test for the layering, not just a fixture.
//
// The delivery is SYNCHRONOUS and re-entrant: handle() runs inside
// engine_send(), while NIC::send() still holds the transmit buffer.  That is
// deliberate — it is the same re-entrancy the real Engine gets when SIGIO
// interrupts a thread in the middle of sendto().
//
// The knobs are STATIC because NIC inherits its Engine PRIVATELY: a test
// holding a NIC has no way to reach an instance member of the Engine
// underneath.  Process-global switches are the way in.
// =============================================================================

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
            // A COPY, because the real Engine hands up a frame that lives in
            // its own memory and is valid only for the duration of the call.
            // Passing the caller's buffer straight through would let a bug that
            // depends on that distinction pass unnoticed here.
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
