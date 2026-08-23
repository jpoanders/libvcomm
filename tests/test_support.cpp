// =============================================================================
// Testes das classes de APOIO — as que já vieram implementadas.
//
// Rode este primeiro: `make test-support`.  Ele tem que passar 100% ANTES de
// você escrever uma linha da Engine.  É a sua linha de base: se algo aqui
// quebrar depois, o problema é no que você mexeu, não no alicerce.
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
    std::printf("== apoio ==\n");

    // --- Ethernet: o layout do fio -------------------------------------------
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

    // --- Buffer: posse exclusiva ---------------------------------------------
    Buffer<Ethernet::Frame> bf;
    CHECK(bf.in_use() == false);
    CHECK(bf.lock() == true);
    CHECK(bf.lock() == false);  // segunda tentativa falha: é isso que
    CHECK(bf.in_use() == true); // impede duas threads pegarem o mesmo
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
    CHECK(list.remove() == &v1); // FIFO, não LIFO
    CHECK(list.remove() == &v2);
    CHECK(list.remove() == 0); // vazia devolve 0, não estoura

    // --- Semaphore: o desacoplamento no tempo --------------------------------
    Semaphore sem(0);
    CHECK(sem.try_p() == false);
    sem.v();
    CHECK(sem.try_p() == true);

    // O v() que acontece ANTES do p() não se perde — é a diferença entre
    // semáforo e rendezvous, e é o motivo de a recepção não precisar de
    // sincronia com a aplicação.
    Semaphore early(0);
    early.v();
    early.p(); // não bloqueia: o contador já estava em 1
    CHECK(true);

    // E o p() que chega antes realmente dorme até o v().
    Semaphore late(0);
    bool woke = false;
    std::thread waker([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        woke = true;
        late.v();
    });
    late.p();
    CHECK(woke == true); // só passa se p() esperou de verdade
    waker.join();

    // --- Message -------------------------------------------------------------
    Message m;
    CHECK(m.size() == 0);
    const char * hello = "M10";
    m.set(hello, 3);
    CHECK(m.size() == 3);
    CHECK(std::memcmp(m.data(), hello, 3) == 0);
    // Truncamento: origem MAIOR que MAX_SIZE.  A origem precisa existir de
    // verdade — pedir para copiar mais bytes do que o array de origem tem é
    // leitura fora dos limites, mesmo que o destino trunque.
    unsigned char big[Message::MAX_SIZE + 100];
    std::memset(big, 0xAB, sizeof(big));
    m.set(big, sizeof(big));
    CHECK(m.size() == Message::MAX_SIZE); // trunca, não estoura o buffer
    CHECK(static_cast<unsigned char *>(m.data())[Message::MAX_SIZE - 1] ==
          0xAB);

    return ::test::summary("apoio");
}
