// =============================================================================
// Tests for the STACK — the ones you are going to make pass.
//
// Run with `make test-stack`.  THEY FAIL TODAY, and they are meant to: each
// CHECK here is a piece of the contract that is still a TODO.  Implement in the
// order the tests appear and use the green as your stopwatch.
//
// These tests do not open a raw socket (they run on the host, without
// CAP_NET_RAW).  They exercise what does NOT depend on the kernel: Observer,
// the buffer pool, marshalling.  The proof that frames really travel is the
// five-VM fleet, not this.
// =============================================================================

#include "check.h"
#include "libvcomm.h"

namespace {

// A fake observer, to test Observed without bringing up the whole stack.
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
    // 1. Conditionally_Data_Observed — attach / notify / detach.
    //    Start here: it is what unblocks NIC and Protocol.
    // -------------------------------------------------------------------------
    {
        Conditionally_Data_Observed<int, int> observed;
        Spy s1(10), s2(20);
        int payload = 42;

        observed.attach(&s1, 10);
        observed.attach(&s2, 20);

        CHECK(observed.notify(10, &payload) == true);
        CHECK(s1.calls == 1);
        CHECK(s2.calls == 0); // different condition: NOT notified
        CHECK(s1.last == 42);

        CHECK(observed.notify(99, &payload) == false); // nobody listens to 99
        observed.detach(&s1, 10);
        CHECK(observed.notify(10, &payload) ==
              false); // after the detach, silence
        CHECK(s1.calls == 1);
    }

    // -------------------------------------------------------------------------
    // 2. Concurrent_Observed — already implemented (transcribed from the PDF).
    //    Must pass TODAY.  If it fails, the regression is yours.
    // -------------------------------------------------------------------------
    {
        Concurrent_Observed<int, int> observed;
        Concurrent_Observer<int, int> obs;
        int payload = 7;

        observed.attach(&obs, 5);
        CHECK(obs.rank() == 5);
        CHECK(observed.notify(5, &payload) == true);
        CHECK(obs.updated() == &payload); // does not block: the v() already
                                          // happened
        CHECK(observed.notify(6, &payload) == false);
    }

    // -------------------------------------------------------------------------
    // 3. NIC — buffer pool.  No socket needed to test it.
    // -------------------------------------------------------------------------
    {
        Vehicle_NIC nic;

        Vehicle_NIC::Buffer * first = nic.alloc(
            Ethernet::BROADCAST, Traits<Ethernet>::PROTOCOL_NUMBER, 64);
        CHECK(first != 0);

        if (first) {
            CHECK(first->frame()->dst == Ethernet::BROADCAST);
            CHECK(first->size() == Ethernet::HEADER_SIZE + 64);
            nic.free(first);
        }

        // Exhaustion: the pool has BUFFER_SIZE slots and not one more.  When it
        // runs out, alloc() returns 0 — and returning 0 is correct behaviour.
        Vehicle_NIC::Buffer * all[Vehicle_NIC::BUFFER_SIZE];
        unsigned int got = 0;
        for (unsigned int i = 0; i < Vehicle_NIC::BUFFER_SIZE; i++) {
            all[i] = nic.alloc(Ethernet::BROADCAST,
                               Traits<Ethernet>::PROTOCOL_NUMBER, 8);
            if (all[i])
                got++;
        }
        CHECK(got == Vehicle_NIC::BUFFER_SIZE);
        CHECK(nic.alloc(Ethernet::BROADCAST, Traits<Ethernet>::PROTOCOL_NUMBER,
                        8) == 0);

        for (unsigned int i = 0; i < got; i++)
            nic.free(all[i]);

        // After giving everything back, the pool returns to the start.  This
        // CHECK is what catches a buffer leak — the bug that only shows up in
        // the demo.
        Vehicle_NIC::Buffer * again = nic.alloc(
            Ethernet::BROADCAST, Traits<Ethernet>::PROTOCOL_NUMBER, 8);
        CHECK(again != 0);
        nic.free(again);
    }

    // -------------------------------------------------------------------------
    // 4. Marshalling — roundtrip through alloc() + unmarshal().
    // -------------------------------------------------------------------------
    {
        Vehicle_NIC nic;
        const unsigned char payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
        Ethernet::Address dst = Ethernet::BROADCAST;

        Vehicle_NIC::Buffer * buf = nic.alloc(
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
