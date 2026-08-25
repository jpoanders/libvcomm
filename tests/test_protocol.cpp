// =============================================================================
// Tests for PROTOCOL and COMMUNICATOR — the two layers the fleet depends on and
// that nothing used to exercise on the host.
//
// Run with `make test-protocol`.  No socket, no privileges, no VM: the stack is
// instantiated over tests/loopback_engine.h, which returns every frame it is
// given straight back up through handle().
//
// WHY THIS FILE EXISTS.  Communicator::send() used to broadcast to
// Address::broadcast(), whose port defaults to 0, so Protocol::update() notified
// condition 0 and no Communicator — all of which attach on a real port — ever
// matched.  Every message in the library was delivered to nobody and silently
// freed.  test-stack could not see it (it stops below Protocol) and the fleet
// would have shown it as "the receivers print nothing", two minutes of QEMU per
// attempt.  Section 1 below is that bug's regression test and it runs in
// milliseconds.
// =============================================================================

#include "check.h"

#include "loopback_engine.h"

#include "traits.h"
#include "ethernet.h"
#include "buffer.h"
#include "observer.h"
#include "message.h"
#include "nic.h"
#include "protocol.h"
#include "communicator.h"

namespace {

typedef NIC<Loopback_Engine> Test_NIC;
typedef Protocol<Test_NIC> Test_Protocol;
typedef Communicator<Test_Protocol> Test_Communicator;

const unsigned short PROT = Traits<Ethernet>::PROTOCOL_NUMBER;
const Test_Protocol::Port PORT_A = 1024;
const Test_Protocol::Port PORT_B = 2048;

Test_Protocol::Address addr_of(Test_NIC & nic, Test_Protocol::Port p)
{
    return Test_Protocol::Address(nic.address(), p);
}

} // namespace

int main()
{
    std::printf("== protocol + communicator ==\n");

    // -------------------------------------------------------------------------
    // 1. A message reaches the Communicator on ITS port, and only that one.
    //    This is the regression test for the broadcast-to-port-0 defect.
    // -------------------------------------------------------------------------
    {
        Loopback_Engine::reset();
        Test_NIC nic;
        Test_Protocol proto(&nic);
        Test_Communicator a(&proto, addr_of(nic, PORT_A));
        Test_Communicator b(&proto, addr_of(nic, PORT_B));

        CHECK(nic.valid());

        const char hello[] = "hello";
        Message out(hello, sizeof(hello));
        CHECK(a.send(&out) == true);
        CHECK(Loopback_Engine::sent == 1);

        Message in;
        CHECK(a.receive(&in, 200) == true); // <<< false before the fix
        CHECK(in.size() == sizeof(hello));
        CHECK(std::memcmp(in.data(), hello, sizeof(hello)) == 0);

        // The other port heard nothing.  Without this check, "deliver to
        // everybody" would pass section 1 just as well as the correct fix.
        Message none;
        CHECK(b.receive(&none, 50) == false);

        CHECK(nic.statistics().tx_packets == 1);
        CHECK(nic.statistics().rx_packets == 1);
        CHECK(nic.statistics().rx_dropped == 0);
    }

    // -------------------------------------------------------------------------
    // 2. Ownership: a full send -> receive round trip returns every buffer it
    //    borrowed.  Run it well past the size of the pool — a leak of one
    //    buffer per message shows up as a failure on the BUFFER_SIZE-th
    //    iteration, which is exactly the bug that would otherwise wait for the
    //    demo.
    // -------------------------------------------------------------------------
    {
        Loopback_Engine::reset();
        Test_NIC nic;
        Test_Protocol proto(&nic);
        Test_Communicator a(&proto, addr_of(nic, PORT_A));

        const unsigned char payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
        Message out(payload, sizeof(payload));

        const unsigned int rounds = 8 * Test_NIC::BUFFER_SIZE;
        unsigned int completed = 0;
        for (unsigned int i = 0; i < rounds; i++) {
            Message in;
            if (!a.send(&out))
                break;
            if (!a.receive(&in, 200))
                break;
            if (in.size() != sizeof(payload))
                break;
            completed++;
        }
        CHECK(completed == rounds);
        CHECK(nic.statistics().rx_dropped == 0);
    }

    // -------------------------------------------------------------------------
    // 3. The pool partition (doc/design-decisions.md §2.6), mirror case.
    //    Messages that are never received hold their RECEIVE half hostage.
    //    Reception starts dropping — correctly, and counted — while
    //    TRANSMISSION carries on untouched.  That separation is the whole
    //    reason the pool was split.
    // -------------------------------------------------------------------------
    {
        Loopback_Engine::reset();
        Test_NIC nic;
        Test_Protocol proto(&nic);
        Test_Communicator a(&proto, addr_of(nic, PORT_A));

        const unsigned char payload[] = {1, 2, 3, 4};
        Message out(payload, sizeof(payload));

        const unsigned int excess = 4;
        for (unsigned int i = 0; i < Traits<Ethernet>::RECEIVE_BUFFERS + excess;
             i++)
            a.send(&out); // deliberately never received

        CHECK(nic.statistics().rx_dropped == excess);

        Test_NIC::Buffer * tx =
            nic.alloc(Ethernet::BROADCAST, PROT, sizeof(payload));
        CHECK(tx != 0); // the transmit half was never at risk
        nic.free(tx);

        // And everything the RX half did hold is still there to be taken.
        Message in;
        unsigned int drained = 0;
        while (a.receive(&in, 0))
            drained++;
        CHECK(drained == Traits<Ethernet>::RECEIVE_BUFFERS);
    }

    // -------------------------------------------------------------------------
    // 4. A lying length field in the header is clamped to what actually
    //    arrived.  The packet is assembled by hand because Protocol::send()
    //    would never produce this frame — the point is that a REMOTE sender
    //    can, and nothing on the wire is trustworthy.
    // -------------------------------------------------------------------------
    {
        Loopback_Engine::reset();
        Test_NIC nic;
        Test_Protocol proto(&nic);
        Test_Communicator a(&proto, addr_of(nic, PORT_A));

        const unsigned int DATA = 4;
        Test_NIC::Buffer * buf =
            nic.alloc(Ethernet::BROADCAST, PROT,
                      sizeof(Test_Protocol::Header) + DATA);
        CHECK(buf != 0);

        if (buf) {
            Test_Protocol::Packet * pkt =
                reinterpret_cast<Test_Protocol::Packet *>(buf->frame()->data);
            pkt->_from_port = 7;
            pkt->_to_port = PORT_A;
            pkt->_length = 60000; // <<< the lie
            std::memcpy(pkt->data<void>(), "abcd", DATA);

            nic.send(buf);

            Message in;
            CHECK(a.receive(&in, 200) == true);
            CHECK(in.size() == DATA);
            CHECK(std::memcmp(in.data(), "abcd", DATA) == 0);
        }
    }

    // -------------------------------------------------------------------------
    // 5. A full queue is REPORTED, not swallowed (decision 1.11).  notify()
    //    returning false is what tells the layer above that nobody took the
    //    data and the buffer has to go back — without it the message was lost
    //    AND the buffer leaked.
    // -------------------------------------------------------------------------
    {
        Concurrent_Observed<int, int> observed;
        Concurrent_Observer<int, int> obs;
        int payload = 42;

        observed.attach(&obs, 1);

        unsigned int accepted = 0;
        for (unsigned int i = 0; i < 4 * List<int>::CAPACITY; i++)
            if (observed.notify(1, &payload))
                accepted++;

        CHECK(accepted == List<int>::CAPACITY);
        CHECK(observed.notify(1, &payload) == false);

        // Take one out and the next insert fits again.
        CHECK(obs.updated() == &payload);
        CHECK(observed.notify(1, &payload) == true);
    }

    return ::test::summary("protocol");
}
