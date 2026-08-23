// =============================================================================
// Testes da PILHA — os que você vai fazer passar.
//
// Rode com `make test-stack`.  HOJE ELES FALHAM, e é para falhar mesmo: cada
// CHECK aqui é um pedaço do contrato que ainda está com TODO.  Implemente na
// ordem em que os testes aparecem e use o verde como cronômetro.
//
// Estes testes não abrem raw socket (rodam no host, sem CAP_NET_RAW).  Eles
// exercitam o que NÃO depende do kernel: Observer, pool de buffers,
// marshalling. A prova de que os frames andam de verdade é a frota de 5 VMs,
// não isto aqui.
// =============================================================================

#include "check.h"
#include "libvcomm.h"

namespace {

// Observador de mentira, para testar o Observed sem subir a pilha inteira.
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
    std::printf("== pilha (esperado falhar até a implementação) ==\n");

    // -------------------------------------------------------------------------
    // 1. Conditionally_Data_Observed — attach / notify / detach.
    //    Comece por aqui: é o destravador de NIC e Protocol.
    // -------------------------------------------------------------------------
    {
        Conditionally_Data_Observed<int, int> observed;
        Spy s1(10), s2(20);
        int payload = 42;

        observed.attach(&s1, 10);
        observed.attach(&s2, 20);

        CHECK(observed.notify(10, &payload) == true);
        CHECK(s1.calls == 1);
        CHECK(s2.calls == 0); // condição diferente: NÃO é notificado
        CHECK(s1.last == 42);

        CHECK(observed.notify(99, &payload) == false); // ninguém escuta 99
        observed.detach(&s1, 10);
        CHECK(observed.notify(10, &payload) ==
              false); // depois do detach, silêncio
        CHECK(s1.calls == 1);
    }

    // -------------------------------------------------------------------------
    // 2. Concurrent_Observed — já implementado (transcrito do PDF).
    //    Deve passar HOJE.  Se falhar, a regressão é sua.
    // -------------------------------------------------------------------------
    {
        Concurrent_Observed<int, int> observed;
        Concurrent_Observer<int, int> obs;
        int payload = 7;

        observed.attach(&obs, 5);
        CHECK(obs.rank() == 5);
        CHECK(observed.notify(5, &payload) == true);
        CHECK(obs.updated() == &payload); // não bloqueia: o v() já aconteceu
        CHECK(observed.notify(6, &payload) == false);
    }

    // -------------------------------------------------------------------------
    // 3. NIC — pool de buffers.  Não precisa de socket para testar.
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

        // Exaustão: o pool tem BUFFER_SIZE posições e nem uma a mais.  Quando
        // acabar, alloc() devolve 0 — e devolver 0 é comportamento correto.
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

        // Depois de devolver tudo, o pool volta ao começo.  Este CHECK é o que
        // pega vazamento de buffer — o bug que só aparece na demo.
        Vehicle_NIC::Buffer * again = nic.alloc(
            Ethernet::BROADCAST, Traits<Ethernet>::PROTOCOL_NUMBER, 8);
        CHECK(again != 0);
        nic.free(again);
    }

    // -------------------------------------------------------------------------
    // 4. TODO(joao): marshalling ida e volta.
    //    Monte um buffer com alloc(), escreva um payload conhecido, passe por
    //    unmarshal() e confira que src, dst e os bytes voltam idênticos.
    //    É o teste que pega erro de byte order e de offset de cabeçalho ANTES
    //    de você estar depurando com tshark às duas da manhã.
    // -------------------------------------------------------------------------

    return ::test::summary("pilha");
}
