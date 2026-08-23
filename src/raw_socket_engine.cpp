#include "../include/engine/raw_socket_engine.h"

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <cstring>
#include <cerrno>

// =============================================================================
// ROTEIRO DE IMPLEMENTAÇÃO — a Etapa 1 inteira mora neste arquivo.
//
//   1. construtor    socket / if_nametoindex / SIOCGIFHWADDR / bind      [FEITO]
//   2. engine_send   sendto                                             [FEITO]
//   3. armar o sinal fcntl + sigaction + trampolim
//                    verificar: um printf no handler prova que ele dispara
//   4. drain()       recvfrom em laço até EAGAIN + filtros + handle()
//                    verificar: VM 2 imprime o que a VM 1 mandou
//   5. engine_stop   desarmar
//                    verificar: a VM termina sozinha, sem timeout do script
//
// -----------------------------------------------------------------------------
// PASSO 3 — o que falta no SEU construtor, depois do bind:
//
//   _signo = SIGRTMIN;    (não é constante de compilação na glibc: é uma chamada
//                          a __libc_current_sigrtmin(), então tem que ser
//                          atribuído em tempo de execução, não inicializado na
//                          lista de inicialização)
//
//   _instance = this;     o trampolim precisa achar o objeto
//
//   sigaction(_signo, ...) com sa_sigaction = &signal_handler e SA_SIGINFO.
//     Cuidado com SA_RESTART: o wrapper signal() da glibc liga SA_RESTART
//     sozinho (man 2 signal, BSD semantics).  Use sigaction e decida você.
//
//   fcntl(_sockfd, F_SETOWN, getpid())
//   fcntl(_sockfd, F_SETSIG, _signo)
//   fcntl(_sockfd, F_SETFL, fcntl(_sockfd, F_GETFL, 0) | O_NONBLOCK)
//
//   O O_ASYNC NÃO entra aqui — ele é o interruptor, e quem liga é
//   engine_start(), depois que a NIC terminou de construir.
//
// ORDEM IMPORTA: sigaction ANTES do F_SETSIG.  Se um sinal chegasse com a
// disposição ainda no padrão, um SIGRTMIN não tratado mata o processo.
//
// SYSCALLS (o guia manda pesquisar, não copiar):
//   fcntl F_SETOWN / F_SETSIG / F_SETFL      man 2 fcntl
//   sigaction com SA_SIGINFO                 man 2 sigaction
//   recvfrom não bloqueante -> EAGAIN        man 2 recv
//   lista de funções permitidas no handler   man 7 signal-safety
// =============================================================================


// Ponte entre o handler (função livre, sem `this`) e o objeto.  Ver a nota
// sobre "uma Engine por processo" no header.
Raw_Socket_Engine * Raw_Socket_Engine::_instance = 0;


Raw_Socket_Engine::Raw_Socket_Engine(const char * iface, Protocol prot)
    : _sockfd(-1), _ifindex(0), _address(), _protocol(prot), _signo(0), _armed(0)
{
    // TODO(joao): PASSO 1.
    //
    //   a) socket(AF_PACKET, SOCK_RAW, htons(_protocol))
    //      Por que htons aqui?  O terceiro argumento é o filtro do kernel e ele
    //      compara com o EtherType COMO ESTÁ NO FIO.  Passar 0x88B5 em host
    //      order faz o kernel filtrar por 0xB588 e você não recebe nada.
    //      Por que não ETH_P_ALL?  Porque aí você recebe também o IPv6 que o
    //      próprio guest emite (MLD, router solicitation) e tem que descartar
    //      no espaço de usuário — trabalho que o kernel faria de graça.
    //
    //   b) _ifindex = if_nametoindex(iface);  0 significa erro.
    //
    //   c) ioctl(SIOCGIFHWADDR) para preencher _address.
    //      struct ifreq, ifr_name = iface, e o MAC sai em ifr_hwaddr.sa_data.
    //
    //   d) bind() com sockaddr_ll { sll_family=AF_PACKET,
    //                               sll_protocol=htons(_protocol),
    //                               sll_ifindex=_ifindex }.
    //      Sem bind, o socket escuta TODAS as interfaces.
    //
    //   Em qualquer falha: fechar o que já abriu, deixar _sockfd = -1 e sair.
    //   engine_valid() é quem reporta.
    _sockfd = ::socket(AF_PACKET, SOCK_RAW, htons(_protocol));
    if (_sockfd < 0) {
        return;
    }
    _ifindex = ::if_nametoindex(iface);
    if (_ifindex == 0) {
        ::close(_sockfd);
        _sockfd = -1;
        return;
    }
    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (::ioctl(_sockfd, SIOCGIFHWADDR, &ifr) < 0) {
        ::close(_sockfd);
        _sockfd = -1;
        return;
    }
    _address = Address(
        reinterpret_cast<const unsigned char *>(ifr.ifr_hwaddr.sa_data));

    struct sockaddr_ll sll;
    std::memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(_protocol);
    sll.sll_ifindex = _ifindex;

    if (::bind(_sockfd, reinterpret_cast<const struct sockaddr *>(&sll),
               sizeof(sll)) < 0) {
        ::close(_sockfd);
        _sockfd = -1;
        return;
    }
}

Raw_Socket_Engine::~Raw_Socket_Engine()
{
    // TODO(joao): desarmar ANTES de fechar o descritor, senão um sinal pode
    // chegar apontando para um fd que já não existe (ou pior, reciclado).
    //   engine_stop();
    //   _instance = 0;
    //   if(_sockfd >= 0) ::close(_sockfd);
}

int Raw_Socket_Engine::engine_send(const Ethernet::Frame * frame,
                                   unsigned int size)
{
    // TODO(joao): PASSO 2.  sendto() com um sockaddr_ll de destino.
    //
    //   Para SOCK_RAW o kernel transmite o buffer exatamente como está — o
    //   cabeçalho de 14 bytes é seu.  Do sockaddr_ll, sendto() usa na prática
    //   sll_ifindex (para onde mandar); sll_halen/sll_addr também devem ser
    //   preenchidos coerentemente.
    //
    //   Contrato: devolver o retorno do sendto() (bytes) ou -1 com errno.
    struct sockaddr_ll sll;
    std::memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(_protocol);
    sll.sll_ifindex = _ifindex;
    sll.sll_halen = ETH_ALEN;
    std::memcpy(sll.sll_addr, Ethernet::BROADCAST.bytes(), ETH_ALEN);

    return ::sendto(_sockfd, frame, size, 0,
                    reinterpret_cast<const struct sockaddr *>(&sll),
                    sizeof(sll));
}

// =============================================================================
// PASSO 3 — o trampolim.
// =============================================================================
void Raw_Socket_Engine::signal_handler(int signo, siginfo_t * info, void * ucontext)
{
    // TODO(joao): uma linha de trabalho, o resto é conferência.
    //
    //   if(_instance && _instance->_armed) _instance->drain();
    //
    // Vale conferir info->si_code == SI_SIGIO e info->si_fd == _sockfd antes:
    // com F_SETSIG o kernel diz QUAL descritor acordou (man 2 fcntl).  Na Etapa
    // 1 há um socket só e a conferência é redundante — mas é ela que impede o
    // bug silencioso no dia em que houver dois.
    (void) signo; (void) info; (void) ucontext;
}


// =============================================================================
// PASSO 4 — a drenagem.  O equivalente do laço de recepção, sem o laço eterno.
// =============================================================================
void Raw_Socket_Engine::drain()
{
    // TODO(joao):
    //
    //   for(;;) {
    //       n = recvfrom(_sockfd, &frame, sizeof(frame), 0,
    //                    (sockaddr*)&from, &len);
    //       ...
    //   }
    //
    // POR QUE UM LAÇO, se o sinal já disse "chegou um": porque pode ter chegado
    // mais de um antes de o handler rodar, e porque um sinal pode se perder.  O
    // laço é a garantia real; o sinal é só o gatilho.  Sair no primeiro frame
    // deixa os outros parados no buffer do kernel até o PRÓXIMO frame chegar —
    // e aí a sua latência medida vira ficção.
    //
    // Condições de saída, e as três são diferentes:
    //   n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) -> acabou, retorne.
    //   n < 0 && errno == EINTR                            -> outro sinal
    //                                                         passou; continue.
    //   n < 0, qualquer outro                              -> erro de verdade;
    //                                                         retorne.
    //
    // Filtros que sobrevivem do modelo antigo, do mais barato para o mais caro:
    //   1. eco de si mesmo -> from.sll_pkttype == PACKET_OUTGOING.  Você MEDIU:
    //      num receptor de outra VM chega PACKET_BROADCAST; só o próprio
    //      emissor vê PACKET_OUTGOING.
    //   2. n < Ethernet::HEADER_SIZE -> frame truncado, descarte.
    //
    // ARMADILHA QUE VOCÊ JÁ MEDIU: o virtio-net do QEMU NÃO faz padding para 60
    // bytes.  Um frame de 20 bytes chega como 20.  Ou seja, calcular o tamanho
    // do payload como (n - 14) FUNCIONA aqui — e quebra em hardware real e na
    // Engine de memória compartilhada da Etapa 2.  Se você quer o tamanho certo
    // em qualquer meio, ele tem que estar DENTRO do payload, num campo do
    // Protocol::Header.  Decida agora e escreva em doc/.
    //
    // ARMADILHA NOVA, que não existia no modelo de thread: o `frame` é uma
    // variável local DENTRO DE UM HANDLER.  Ele mora na pilha da thread que foi
    // interrompida.  1514 bytes é muito para uma pilha que você não escolheu —
    // se o app tiver threads com pilha pequena, isso estoura.  Alternativa:
    // buffer estático da Engine, que é seguro porque só o handler o usa e
    // handlers do mesmo sinal não se reentram (o kernel bloqueia _signo durante
    // a execução do handler, a menos que você peça SA_NODEFER).
    //
    // Sobreviveu aos filtros -> handle(&frame, n).
}


// =============================================================================
// PASSO 5 — armar e desarmar.
// =============================================================================
bool Raw_Socket_Engine::engine_start()
{
    // TODO(joao): se !engine_valid() devolver false.  Senão:
    //
    //   _armed = 1;                       ANTES de ligar o O_ASYNC
    //   fcntl(_sockfd, F_SETFL, fcntl(_sockfd, F_GETFL, 0) | O_ASYNC);
    //
    // A ordem é o ponto: entre ligar o O_ASYNC e marcar _armed, um frame pode
    // chegar.  Marcando primeiro, o handler encontra tudo pronto.
    return false;
}


void Raw_Socket_Engine::engine_stop()
{
    // TODO(joao): o inverso, e na ordem inversa:
    //
    //   fcntl(_sockfd, F_SETFL, fcntl(_sockfd, F_GETFL, 0) & ~O_ASYNC);
    //   _armed = 0;
    //
    // Idempotente: chamar duas vezes não pode fazer mal (o destrutor chama, e o
    // main provavelmente também).
    //
    // Um sinal já ENFILEIRADO ainda pode ser entregue depois do fcntl — é para
    // isso que serve o teste de _armed no trampolim.
}
