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
    // 1. Port-selective delivery (regression for broadcast-to-port-0 defect)
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
        CHECK(a.receive(&in, 200) == true);
        CHECK(in.size() == sizeof(hello));
        CHECK(std::memcmp(in.data(), hello, sizeof(hello)) == 0);

        // Port B must not receive what was sent to port A.
        Message none;
        CHECK(b.receive(&none, 50) == false);

        CHECK(nic.statistics().tx_packets == 1);
        CHECK(nic.statistics().rx_packets == 1);
        CHECK(nic.statistics().rx_dropped == 0);
    }

    // -------------------------------------------------------------------------
    // 2. Buffer ownership: send+receive must return every buffer
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
    // 3. Pool partition: rx exhaustion must not affect tx
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
            a.send(&out); // never received

        CHECK(nic.statistics().rx_dropped == excess);

        // tx half unaffected
        Test_NIC::Buffer * tx =
            nic.alloc(Ethernet::BROADCAST, PROT, sizeof(payload));
        CHECK(tx != 0);
        nic.free(tx);

        // drain everything the tx half held
        Message in;
        unsigned int drained = 0;
        while (a.receive(&in, 0))
            drained++;
        CHECK(drained == Traits<Ethernet>::RECEIVE_BUFFERS);
    }

    // -------------------------------------------------------------------------
    // 4. Malformed length field: clamped to actual bytes received.
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
            pkt->_length = 60000; // forged
            std::memcpy(pkt->data<void>(), "abcd", DATA);

            nic.send(buf);

            Message in;
            CHECK(a.receive(&in, 200) == true);
            CHECK(in.size() == DATA);
            CHECK(std::memcmp(in.data(), "abcd", DATA) == 0);
        }
    }

    // -------------------------------------------------------------------------
    // 5. Full observer queue: notify() returns false, caller reclaims buffer.
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

        // Consuming one frees a slot for the next insert.
        CHECK(obs.updated() == &payload);
        CHECK(observed.notify(1, &payload) == true);
    }

    // -------------------------------------------------------------------------
    // 6. Cross-communicator delivery: A sends to B, B receives, A does not
    // -------------------------------------------------------------------------
    {
        Loopback_Engine::reset();
        Test_NIC nic;
        Test_Protocol proto(&nic);
        Test_Communicator a(&proto, addr_of(nic, PORT_A));
        Test_Communicator b(&proto, addr_of(nic, PORT_B));

        const char payload[] = "cross";

        CHECK(proto.send(addr_of(nic, PORT_A), addr_of(nic, PORT_B),
                         payload, sizeof(payload)) > 0);

        Message in;
        CHECK(b.receive(&in, 200) == true);
        CHECK(in.size() == sizeof(payload));
        CHECK(std::memcmp(in.data(), payload, sizeof(payload)) == 0);

        // A must not receive what was addressed to B.
        Message none;
        CHECK(a.receive(&none, 50) == false);
    }

    return ::test::summary("protocol");
}
