// =============================================================================
// Tests for the SUPPORT classes — the ones that came already implemented.
//
// Run this first: `make test-support`.  It must pass 100% BEFORE you write a
// single line of the Engine.  It is your baseline: if something here breaks
// later, the problem is in what you touched, not in the foundation.
// =============================================================================

#include <thread>
#include <chrono>

#include "check.h"
#include "ethernet.h"
#include "buffer.h"
#include "list.h"
#include "sem.h"
#include "message.h"

int main()
{
    std::printf("== support ==\n");

    // --- Ethernet: the wire layout -------------------------------------------
    CHECK(sizeof(Ethernet::Header) == 14);
    CHECK(sizeof(Ethernet::Address) == 6);
    CHECK(Ethernet::MTU == 1500);

    Ethernet::Address a(0x02, 0x00, 0x00, 0x00, 0x00, 0x01);
    Ethernet::Address b(0x02, 0x00, 0x00, 0x00, 0x00, 0x01);
    Ethernet::Address c(0x02, 0x00, 0x00, 0x00, 0x00, 0x02);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(bool(a) == true);
    CHECK(bool(Ethernet::Address()) == false);
    CHECK(Ethernet::BROADCAST.bytes()[0] == 0xff);
    CHECK(Ethernet::BROADCAST.bytes()[5] == 0xff);

    char buf[18];
    CHECK(std::strcmp(a.to_string(buf), "02:00:00:00:00:01") == 0);

    // --- Buffer: exclusive ownership -----------------------------------------
    Buffer<Ethernet::Frame> bf;
    CHECK(bf.in_use() == false);
    CHECK(bf.lock() == true);
    CHECK(bf.lock() == false);  // the second attempt fails: that is what
    CHECK(bf.in_use() == true); // stops two threads taking the same one
    bf.unlock();
    CHECK(bf.lock() == true);
    bf.unlock();

    // --- List: FIFO ----------------------------------------------------------
    List<int> list;
    int v1 = 1, v2 = 2;
    CHECK(list.empty());
    list.insert(&v1);
    list.insert(&v2);
    CHECK(list.size() == 2);
    CHECK(list.remove() == &v1); // FIFO, not LIFO
    CHECK(list.remove() == &v2);
    CHECK(list.remove() == 0); // empty returns 0, does not blow up

    // --- Semaphore: decoupling in time ---------------------------------------
    Semaphore sem(0);
    CHECK(sem.try_p() == false);
    sem.v();
    CHECK(sem.try_p() == true);

    // A v() that happens BEFORE the p() is not lost — that is the difference
    // between a semaphore and a rendezvous, and it is why reception does not
    // need to be in sync with the application.
    Semaphore early(0);
    early.v();
    early.p(); // does not block: the counter was already 1
    CHECK(true);

    // And a p() that arrives first really does sleep until the v().
    Semaphore late(0);
    bool woke = false;
    std::thread waker([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        woke = true;
        late.v();
    });
    late.p();
    CHECK(woke == true); // only passes if p() genuinely waited
    waker.join();

    // --- Message -------------------------------------------------------------
    Message m;
    CHECK(m.size() == 0);
    const char * hello = "M10";
    m.set(hello, 3);
    CHECK(m.size() == 3);
    CHECK(std::memcmp(m.data(), hello, 3) == 0);
    // Truncation: a source LARGER than MAX_SIZE.  The source has to really
    // exist — asking to copy more bytes than the source array holds is an
    // out-of-bounds read, even if the destination truncates.
    unsigned char big[Message::MAX_SIZE + 100];
    std::memset(big, 0xAB, sizeof(big));
    m.set(big, sizeof(big));
    CHECK(m.size() == Message::MAX_SIZE); // truncates, does not overflow
    CHECK(static_cast<unsigned char *>(m.data())[Message::MAX_SIZE - 1] ==
          0xAB);

    return ::test::summary("support");
}
