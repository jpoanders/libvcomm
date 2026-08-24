// =============================================================================
// Testes da ENGINE — Raw_Socket_Engine.
//
// Rode com `make test-engine`.  Separado do test-stack de propósito: aquele
// promete rodar sem CAP_NET_RAW, e essa promessa vale manter.  Este aqui roda
// nos dois modos e diz em qual está:
//
//   NÍVEL 0  sem privilégio nenhum.  Exercita o caminho de FALHA do construtor,
//            o destrutor sobre Engine inválida e a idempotência do stop.
//
//   NÍVEL 1  precisa de CAP_NET_RAW.  Sobe um socket de verdade em `lo` e
//            exercita send, start, stop e drain.  Sem privilégio, é PULADO —
//            não falha, senão um checkout novo parece quebrado.
//
//            $ make app && sudo setcap cap_net_raw+ep build/test-engine
//            $ make test-engine
//
//            Melhor que rodar a suíte como root.  Para usar outra interface
//            (um par veth, por exemplo):  VCOMM_TEST_IFACE=v0 make test-engine
//
// -----------------------------------------------------------------------------
// POR QUE `lo` FUNCIONA COMO BARRAMENTO DE TESTE
//
// A loopback entrega o que ela transmite.  Cada engine_send() gera DUAS cópias
// visíveis ao socket de pacote:
//
//   1. a cópia de transmissão, com sll_pkttype == PACKET_OUTGOING;
//   2. a cópia de recepção, que passa por eth_type_trans() — e como o destino é
//      ff:ff:ff:ff:ff:ff, chega como PACKET_BROADCAST.
//
// Ou seja: um processo só se basta como emissor E receptor, e o filtro de
// PACKET_OUTGOING do drain() fica sob teste de graça.  Se ele não existisse,
// todo contador de frames abaixo daria o DOBRO.
//
// -----------------------------------------------------------------------------
// POR QUE OS TESTES BLOQUEIAM O SIGIO EM VEZ DE DORMIR
//
// "manda uma rajada e espera" é teste com relógio, e teste com relógio mente em
// máquina carregada.  Aqui o SIGIO é bloqueado com sigprocmask() durante o
// envio.  Enquanto bloqueado, os sinais gerados viram UM sinal pendente — é
// exatamente a não-enfileiração dos sinais padrão, que é o ponto em disputa.
// No sigprocmask() de desbloqueio o POSIX garante entrega antes do retorno, e
// aí a asserção é determinística, sem sleep e sem tolerância:
//
//   N frames enviados  ->  1 entrada no handler  ->  N chamadas de handle()
//
// É esse par de números que responde "por que SIGIO e não sinal de tempo real"
// com medida em vez de argumento.  Ver doc/decisoes.md.
// =============================================================================

#include "check.h"

#include "engine/raw_socket_engine.h"
#include "traits.h"

#include <sys/socket.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const unsigned short PROT = Traits<Ethernet>::PROTOCOL_NUMBER;

// -----------------------------------------------------------------------------
// Contagem de SINAIS, que é coisa diferente de contagem de frames.
//
// O handler da Engine é privado e não dá para instrumentá-lo por dentro.  O que
// dá é ENCADEAR: depois que o construtor da Engine instalou a disposição do
// SIGIO, o teste lê essa disposição com sigaction(NULL, &cur), guarda o
// ponteiro e instala um handler próprio que conta e repassa.  A biblioteca não
// muda uma linha.
// -----------------------------------------------------------------------------
volatile sig_atomic_t g_signals = 0;
void (*g_engine_handler)(int) = 0;

extern "C" void counting_handler(int signo)
{
    g_signals = g_signals + 1;
    if (g_engine_handler)
        g_engine_handler(signo);
}

// -----------------------------------------------------------------------------
// A sonda.  Raw_Socket_Engine não é instanciável: tudo é protected e handle() é
// virtual puro.  Todo teste da Engine passa por uma derivada como esta.
// -----------------------------------------------------------------------------
class Probe : public Raw_Socket_Engine
{
public:
    Probe(const char * iface, Protocol prot)
        : Raw_Socket_Engine(iface, prot), frames(0), oversize(0)
    {
        std::memset(last, 0, sizeof(last));
    }

    using Raw_Socket_Engine::engine_address;
    using Raw_Socket_Engine::engine_rx_error;
    using Raw_Socket_Engine::engine_rx_errors;
    using Raw_Socket_Engine::engine_send;
    using Raw_Socket_Engine::engine_start;
    using Raw_Socket_Engine::engine_stop;
    using Raw_Socket_Engine::engine_valid;

    volatile sig_atomic_t frames;   // quantas vezes handle() foi chamado
    volatile sig_atomic_t oversize; // frame maior que o buffer da sonda
    unsigned char last[Ethernet::HEADER_SIZE + 64];

protected:
    // RODA DENTRO DO SIGNAL HANDLER.  Nada de printf, nada de alocação: só
    // sig_atomic_t e memcpy.  Se esta função ficasse difícil de escrever sob
    // essa regra, o problema estaria no contrato, não no teste.
    void handle(Ethernet::Frame * frame, unsigned int size) override
    {
        frames = frames + 1;
        if (size > sizeof(last))
            oversize = oversize + 1;
        else
            std::memcpy(last, frame, size);
    }
};

// Variável de ambiente ligada: existe, não vazia e diferente de "0".
bool ligado(const char * nome)
{
    const char * v = std::getenv(nome);
    return v && v[0] && std::strcmp(v, "0") != 0;
}

bool tem_cap_net_raw()
{
    int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0)
        return false;
    ::close(fd);
    return true;
}

// MAC pelo caminho do sysfs — independente do SIOCGIFHWADDR que a Engine usa.
// Comparar o resultado de dois caminhos vale mais que comparar com constante.
bool mac_do_sysfs(const char * iface, unsigned char out[6])
{
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    std::FILE * f = std::fopen(path, "r");
    if (!f)
        return false;
    unsigned int b[6];
    int n = std::fscanf(f, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3],
                        &b[4], &b[5]);
    std::fclose(f);
    if (n != 6)
        return false;
    for (int i = 0; i < 6; i++)
        out[i] = static_cast<unsigned char>(b[i]);
    return true;
}

void monta_frame(Ethernet::Frame * f, const Ethernet::Address & src,
                 unsigned int payload)
{
    // Nada de memset() aqui: Ethernet::Frame NÃO é trivially-constructible
    // (Ethernet::Address tem construtor), e o g++ avisa com -Wclass-memaccess.
    // Value-initialization do agregado zera tudo pelo caminho certo — é a mesma
    // razão pela qual um `static Ethernet::Frame` dentro do drain() geraria
    // guarda de inicialização.
    *f = Ethernet::Frame{};
    f->dst = Ethernet::BROADCAST;
    f->src = src;
    f->prot = htons(PROT); // host order dentro da lib, network order no fio
    for (unsigned int i = 0; i < payload; i++)
        f->data[i] = static_cast<unsigned char>(i + 1);
}

// =============================================================================
// NÍVEL 0 — sem privilégio.
//
// A interface não existe, então o construtor falha COM ou SEM CAP_NET_RAW (sem
// privilégio ele nem chega no if_nametoindex: o socket() já volta EPERM).  O
// teste é idêntico nas duas máquinas, que é o que se quer de um teste.
// =============================================================================
void nivel_0()
{
    std::printf("\n== nível 0: caminho de falha (sem privilégio) ==\n");

    Probe bad("vcomm-nada", PROT);

    CHECK(bad.engine_valid() == false);
    CHECK(bad.engine_start() == false);
    CHECK(bad.engine_rx_errors() == 0);

    // Idempotência sobre Engine inválida.  É o guard do engine_stop() que
    // impede um fcntl(-1, ...) aqui — sem ele isto suja errno em silêncio.
    bad.engine_stop();
    bad.engine_stop();
    CHECK(bad.engine_start() == false);

    // Destrutor sobre Engine inválida: constrói e morre dentro do escopo.  Se
    // o destrutor chamasse close(-1) ou fcntl(-1, ...) sem guard, é aqui que
    // apareceria.
    {
        Probe tmp("vcomm-nada", PROT);
        CHECK(tmp.engine_valid() == false);
    }
    CHECK(true); // chegou até aqui == o destrutor não derrubou o processo
}

// =============================================================================
// NÍVEL 1 — socket de verdade.  Precisa de CAP_NET_RAW.
// =============================================================================
void nivel_1(const char * iface)
{
    std::printf("\n== nível 1: socket em '%s' ==\n", iface);

    Probe p(iface, PROT);
    CHECK(p.engine_valid() == true);
    if (!p.engine_valid()) {
        std::printf("  (sem Engine válida, o resto do nível 1 não faz "
                    "sentido)\n");
        return;
    }

    // ---- construtor: o MAC veio do kernel, não de constante -----------------
    unsigned char sysfs[6];
    if (mac_do_sysfs(iface, sysfs))
        CHECK(std::memcmp(p.engine_address().bytes(), sysfs, 6) == 0);
    else
        std::printf("  (sysfs indisponível; comparação de MAC pulada)\n");

    // ---- o construtor instalou a disposição do SIGIO ------------------------
    struct sigaction cur;
    std::memset(&cur, 0, sizeof(cur));
    CHECK(::sigaction(SIGIO, NULL, &cur) == 0);
    CHECK(cur.sa_handler != SIG_DFL); // ação default do SIGIO é MATAR o processo
    CHECK((cur.sa_flags & SA_RESTART) == 0); // decisão de projeto: sem SA_RESTART

    // Encadeia o contador de sinais por cima do handler da Engine.
    g_engine_handler = cur.sa_handler;
    struct sigaction mine = cur;
    mine.sa_handler = counting_handler;
    CHECK(::sigaction(SIGIO, &mine, NULL) == 0);

    CHECK(p.engine_start() == true);

    const unsigned int PAYLOAD = 32;
    const unsigned int SIZE = Ethernet::HEADER_SIZE + PAYLOAD;
    Ethernet::Frame f;
    monta_frame(&f, p.engine_address(), PAYLOAD);

    sigset_t so_sigio, anterior;
    sigemptyset(&so_sigio);
    sigaddset(&so_sigio, SIGIO);

    // ---- ida e volta de UM frame -------------------------------------------
    {
        CHECK(::sigprocmask(SIG_BLOCK, &so_sigio, &anterior) == 0);

        int sent = p.engine_send(&f, SIZE);
        CHECK(sent == static_cast<int>(SIZE));
        CHECK(p.frames == 0); // bloqueado: nada foi entregue ainda

        CHECK(::sigprocmask(SIG_SETMASK, &anterior, NULL) == 0);

        // Exatamente 1, não 2: a cópia PACKET_OUTGOING foi filtrada no drain().
        CHECK(p.frames == 1);
        CHECK(p.oversize == 0);
        CHECK(std::memcmp(p.last, &f, SIZE) == 0);
    }

    // ---- coalescência: N frames, UM sinal ----------------------------------
    const int N = 50;
    {
        sig_atomic_t f0 = p.frames;
        sig_atomic_t s0 = g_signals;

        CHECK(::sigprocmask(SIG_BLOCK, &so_sigio, &anterior) == 0);
        int enviados = 0;
        for (int i = 0; i < N; i++)
            if (p.engine_send(&f, SIZE) == static_cast<int>(SIZE))
                enviados++;
        CHECK(enviados == N);
        CHECK(::sigprocmask(SIG_SETMASK, &anterior, NULL) == 0);

        // O drain() esvaziou a fila inteira a partir de um único sinal.  Se
        // frames < N, o laço está saindo cedo.  Se signals > 1, o teste não
        // provou coalescência (e aí o furado é o teste, não a Engine).
        CHECK(p.frames - f0 == N);
        CHECK(g_signals - s0 == 1);
        std::printf("     -> %d frames entregues por %d sinal(is)\n",
                    static_cast<int>(p.frames - f0),
                    static_cast<int>(g_signals - s0));
    }

    // ---- engine_stop(): para de sinalizar, NÃO descarta ---------------------
    {
        p.engine_stop();
        sig_atomic_t f1 = p.frames;
        sig_atomic_t s1 = g_signals;

        for (int i = 0; i < N; i++)
            p.engine_send(&f, SIZE);

        // Sem O_ASYNC não nasce sinal, então handle() não é chamado.
        CHECK(p.frames == f1);
        CHECK(g_signals == s1);

        p.engine_stop(); // idempotente
        CHECK(p.frames == f1);

        // Os N frames continuam na fila do kernel.  Rearmar e mandar mais um
        // faz o drain() puxar os N parados + o novo: prova que stop silencia a
        // notificação sem perder dado.
        CHECK(p.engine_start() == true);
        CHECK(::sigprocmask(SIG_BLOCK, &so_sigio, &anterior) == 0);
        CHECK(p.engine_send(&f, SIZE) == static_cast<int>(SIZE));
        CHECK(::sigprocmask(SIG_SETMASK, &anterior, NULL) == 0);
        CHECK(p.frames - f1 == N + 1);
    }

    // ---- diagnóstico de erro ------------------------------------------------
    //
    // Num caminho limpo o contador tem que estar parado.  Isto NÃO prova que o
    // braço de erro do drain() funciona — prova que ele não dispara sozinho.
    //
    // A prova positiva é manual, porque derrubar interface é coisa que uma
    // suíte de testes não faz sem ser mandada:
    //
    //   $ sudo ip link set <iface> down    # com a Engine armada
    //   e o engine_rx_errors() tem que subir.
    //
    // Faça numa veth, nunca em `lo` — derrubar a loopback quebra a máquina.
    CHECK(p.engine_rx_errors() == 0);
    if (p.engine_rx_errors() != 0)
        std::printf("     -> último errno de RX: %d (%s)\n", p.engine_rx_error(),
                    std::strerror(p.engine_rx_error()));

    p.engine_stop();

    // Devolve a disposição do SIGIO como estava, para não vazar para outro teste.
    ::sigaction(SIGIO, &cur, NULL);
    g_engine_handler = 0;
}

// =============================================================================
// NÍVEL 1-E — a prova POSITIVA do braço de erro do drain().
//
// Os outros testes só mostram que engine_rx_errors() NÃO sobe sozinho.  Este
// mostra que ele sobe quando deve.
//
// O gatilho é derrubar a interface embaixo de um socket armado.  Em
// packet_notifier(), NETDEV_DOWN sobre a interface do socket faz
// `sk->sk_err = ENETDOWN` e chama sk_error_report(), que acorda o FASYNC — ou
// seja, dispara um SIGIO.  O drain() entra, o recvfrom() consome o sk_err e
// devolve -1/ENETDOWN, que não é EAGAIN nem EINTR: cai no terceiro braço.
//
// NÃO roda junto com o nível 1, por dois motivos:
//
//   1. derrubar interface é coisa que suíte de teste não faz sem ser mandada;
//   2. o meio tem que ser um par veth, não `lo`.  Em veth, o que sai por vcomm0
//      chega em vcomm1 — o socket ligado em vcomm0 só veria a própria cópia
//      PACKET_OUTGOING, que o drain() filtra.  As asserções de recepção do
//      nível 1 dependem da auto-entrega da loopback e falhariam aqui.
//
// Quem orquestra é scripts/test-engine-veth.sh:
//
//     sudo scripts/test-engine-veth.sh
// =============================================================================
void nivel_erro(const char * iface)
{
    std::printf("\n== nível 1-E: erro de RX em '%s' ==\n", iface);

    Probe p(iface, PROT);
    CHECK(p.engine_valid() == true);
    if (!p.engine_valid())
        return;

    CHECK(p.engine_start() == true);
    CHECK(p.engine_rx_errors() == 0);

    // Handshake com o script: só depois desta linha é que derrubar a interface
    // testa alguma coisa.  fflush obrigatório — a saída está redirecionada para
    // arquivo, e aí o stdout é block-buffered.
    std::printf("PRONTO-PARA-ERRO\n");
    std::fflush(stdout);

    // Até ~10 s esperando o contador subir.  nanosleep pode voltar EINTR a cada
    // sinal que chegar: a Engine instala o handler SEM SA_RESTART, de propósito.
    // Aqui isso não incomoda — o laço só tenta de novo.
    for (int i = 0; i < 1000 && p.engine_rx_errors() == 0; i++) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000 * 1000; // 10 ms
        ::nanosleep(&ts, NULL);
    }

    CHECK(p.engine_rx_errors() > 0);
    if (p.engine_rx_errors() > 0) {
        std::printf("     -> errno registrado: %d (%s)\n", p.engine_rx_error(),
                    std::strerror(p.engine_rx_error()));
        CHECK(p.engine_rx_error() == ENETDOWN);
    }

    p.engine_stop();
}

} // namespace

int main()
{
    std::printf("== engine ==\n");

    const char * env_iface = std::getenv("VCOMM_TEST_IFACE");
    const char * iface = env_iface ? env_iface : "lo";

    // VCOMM_ERROR_TEST=1  -> só o nível 1-E, orquestrado pelo script da veth.
    if (ligado("VCOMM_ERROR_TEST")) {
        if (!tem_cap_net_raw()) {
            ::test::report(false, "CAP_NET_RAW (VCOMM_ERROR_TEST exige)",
                           __FILE__, __LINE__);
            return ::test::summary("engine/erro");
        }
        nivel_erro(iface);
        return ::test::summary("engine/erro");
    }

    nivel_0();

    if (tem_cap_net_raw()) {
        nivel_1(iface);
    } else {
        std::printf("\n== nível 1: PULADO — sem CAP_NET_RAW ==\n");
        std::printf("   $ sudo setcap cap_net_raw+ep build/test-engine\n");
        std::printf("   (o setcap vive no inode: REPITA depois de cada "
                    "relink)\n");

        // VCOMM_REQUIRE_RAW=1 transforma o pulo em falha.  É o que o `make
        // check` usa: o alvo da avaliação não pode ficar verde tendo pulado o
        // único teste que exercita o socket de verdade.
        if (ligado("VCOMM_REQUIRE_RAW"))
            ::test::report(false,
                           "nível 1 executado (VCOMM_REQUIRE_RAW=1 exige)",
                           __FILE__, __LINE__);
    }

    return ::test::summary("engine");
}
