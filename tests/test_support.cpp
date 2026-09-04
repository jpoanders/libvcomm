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

    // --- Ethernet wire layout ------------------------------------------------
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

    // --- Buffer: mutual exclusion --------------------------------------------
    Buffer<Ethernet::Frame> bf;
    CHECK(bf.in_use() == false);
    CHECK(bf.lock() == true);
    CHECK(bf.lock() == false);  // second lock must fail
    CHECK(bf.in_use() == true);
    bf.unlock();
    CHECK(bf.lock() == true);   // reusable after unlock
    bf.unlock();

    // --- List: FIFO order ----------------------------------------------------
    List<int> list;
    int v1 = 1, v2 = 2;
    CHECK(list.empty());
    list.insert(&v1);
    list.insert(&v2);
    CHECK(list.size() == 2);
    CHECK(list.remove() == &v1); // FIFO
    CHECK(list.remove() == &v2);
    CHECK(list.remove() == 0);   // empty -> null, no crash

    // --- Semaphore -----------------------------------------------------------
    Semaphore sem(0);
    CHECK(sem.try_p() == false);
    sem.v();
    CHECK(sem.try_p() == true);

    // v() before p() must not be lost (counting, not rendezvous).
    Semaphore early(0);
    early.v();
    early.p();
    CHECK(early.try_p() == false); // counter consumed, back to 0

    // p() blocks until the v() from another thread.
    Semaphore late(0);
    bool woke = false;
    std::thread waker([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        woke = true;
        late.v();
    });
    late.p();
    CHECK(woke == true);
    waker.join();

    // --- Message -------------------------------------------------------------
    Message m;
    CHECK(m.size() == 0);
    const char * hello = "M10";
    m.set(hello, 3);
    CHECK(m.size() == 3);
    CHECK(std::memcmp(m.data(), hello, 3) == 0);

    // Overflow: set() must truncate, not overrun.
    unsigned char big[Message::MAX_SIZE + 100];
    std::memset(big, 0xAB, sizeof(big));
    m.set(big, sizeof(big));
    CHECK(m.size() == Message::MAX_SIZE);
    CHECK(static_cast<unsigned char *>(m.data())[Message::MAX_SIZE - 1] ==
          0xAB);

    return ::test::summary("support");
}
