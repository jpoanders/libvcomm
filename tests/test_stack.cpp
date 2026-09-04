#include "check.h"

#include "loopback_engine.h"

#include "traits.h"
#include "ethernet.h"
#include "buffer.h"
#include "observer.h"
#include "message.h"
#include "nic.h"

namespace {

typedef NIC<Loopback_Engine> Test_NIC;

// Minimal observer for testing dispatch without the full stack.
class Spy : public Conditional_Data_Observer<int, int>
{
public:
    explicit Spy(int rank)
        : Conditional_Data_Observer<int, int>(rank), calls(0), last(0)
    {}
    void update(const int & c, int * d) override
    {
        (void)c;
        calls++;
        last = *d;
    }
    int calls;
    int last;
};

} // namespace

int main()
{
    std::printf("== stack ==\n");

    // -------------------------------------------------------------------------
    // 1. Conditionally_Data_Observed: attach, notify, detach
    // -------------------------------------------------------------------------
    {
        Conditionally_Data_Observed<int, int> observed;
        Spy s1(10), s2(20);
        int payload = 42;

        observed.attach(&s1, 10);
        observed.attach(&s2, 20);

        CHECK(observed.notify(10, &payload) == true);
        CHECK(s1.calls == 1);
        CHECK(s2.calls == 0); // condition mismatch, not notified
        CHECK(s1.last == 42);

        CHECK(observed.notify(99, &payload) == false); // no listener
        observed.detach(&s1, 10);
        CHECK(observed.notify(10, &payload) == false); // detached, silent
        CHECK(s1.calls == 1);
    }

    // -------------------------------------------------------------------------
    // 2. Concurrent_Observed: semaphore-based observer
    // -------------------------------------------------------------------------
    {
        Concurrent_Observed<int, int> observed;
        Concurrent_Observer<int, int> obs;
        int payload = 7;

        observed.attach(&obs, 5);
        CHECK(obs.rank() == 5);
        CHECK(observed.notify(5, &payload) == true);
        CHECK(obs.updated() == &payload); // non-blocking: v() already called
        CHECK(observed.notify(6, &payload) == false);
    }

    // -------------------------------------------------------------------------
    // 3. NIC buffer pool (engine-independent, uses Loopback_Engine)
    // -------------------------------------------------------------------------
    {
        Loopback_Engine::reset();
        Test_NIC nic;

        Test_NIC::Buffer * first = nic.alloc(
            Ethernet::BROADCAST, Traits<Ethernet>::PROTOCOL_NUMBER, 64);
        CHECK(first != 0);

        if (first) {
            CHECK(first->frame()->dst == Ethernet::BROADCAST);
            CHECK(first->size() == Ethernet::HEADER_SIZE + 64);
            nic.free(first);
        }

        // Exhaust the TX half. alloc() only draws from [0, SEND_BUFFERS),
        // so RX is never starved by a TX burst.
        Test_NIC::Buffer * all[Traits<Ethernet>::SEND_BUFFERS];
        unsigned int got = 0;
        for (unsigned int i = 0; i < Traits<Ethernet>::SEND_BUFFERS; i++) {
            all[i] = nic.alloc(Ethernet::BROADCAST,
                               Traits<Ethernet>::PROTOCOL_NUMBER, 8);
            if (all[i])
                got++;
        }
        CHECK(got == Traits<Ethernet>::SEND_BUFFERS);
        CHECK(nic.alloc(Ethernet::BROADCAST, Traits<Ethernet>::PROTOCOL_NUMBER,
                        8) == 0);

        for (unsigned int i = 0; i < got; i++)
            nic.free(all[i]);

        // Pool fully recovered after freeing everything.
        Test_NIC::Buffer * again = nic.alloc(
            Ethernet::BROADCAST, Traits<Ethernet>::PROTOCOL_NUMBER, 8);
        CHECK(again != 0);
        nic.free(again);
    }

    // -------------------------------------------------------------------------
    // 4. Marshalling: alloc() + unmarshal() roundtrip
    // -------------------------------------------------------------------------
    {
        Loopback_Engine::reset();
        Test_NIC nic;
        const unsigned char payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
        Ethernet::Address dst = Ethernet::BROADCAST;

        Test_NIC::Buffer * buf = nic.alloc(
            dst, Traits<Ethernet>::PROTOCOL_NUMBER, sizeof(payload));
        CHECK(buf != 0);

        if (buf) {
            std::memcpy(buf->frame()->data, payload, sizeof(payload));

            Ethernet::Address got_src, got_dst;
            unsigned char got_data[sizeof(payload)];
            int n = nic.unmarshal(buf, &got_src, &got_dst, got_data,
                                  sizeof(got_data));

            CHECK(n == static_cast<int>(sizeof(payload)));
            CHECK(got_dst == Ethernet::BROADCAST);
            CHECK(got_src == nic.address());
            CHECK(std::memcmp(got_data, payload, sizeof(payload)) == 0);
            CHECK(buf->frame()->prot ==
                  htons(Traits<Ethernet>::PROTOCOL_NUMBER));

            nic.free(buf);
        }
    }

    return ::test::summary("stack");
}
