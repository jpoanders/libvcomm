#ifndef LIBVCOMM_RAW_SOCKET_ENGINE_H
#define LIBVCOMM_RAW_SOCKET_ENGINE_H

#include <csignal>
#include "../ethernet.h"

// =============================================================================
// Raw_Socket_Engine — a Engine da ETAPA 1.
//
// ESTA CLASSE É O ÚNICO LUGAR DA BIBLIOTECA QUE PODE CHAMAR SYSCALL DE REDE.
//
// É esse o contrato inteiro.  NIC, Protocol e Communicator não sabem que existe
// socket, sockaddr_ll ou eth0.  Quando a Etapa 2 pedir comunicação entre
// processos da MESMA VM, você escreve Shared_Memory_Engine com estes mesmos
// métodos e NIC<Shared_Memory_Engine> funciona sem alterar uma linha da NIC.
// Se alguma syscall vazar para fora daqui, essa promessa quebra — e é a
// primeira coisa que o Fröhlich vai procurar.
//
//                 NIC<Raw_Socket_Engine>          NIC<Shared_Memory_Engine>
//                          |                                |
//                    raw socket / eth0                 shm_open / mmap
//                       (Etapa 1)                        (Etapa 2)
//
// -----------------------------------------------------------------------------
// RECEPÇÃO POR SINAL POSIX  (revisado em 23/08/2026)
// -----------------------------------------------------------------------------
// A recepção NÃO usa thread.  O enunciado é explícito:
//
//   "os eventos de recepção de pacotes pelo kernel do SO devem ser
//   imediatamente
//    propagados às camadas superiores da pilha de protocolos.  Essa propagação
//    pode se dar tanto através da implementação de módulos específicos do
//    protocolo para o kernel quanto através de SINAIS POSIX."
//
// E há uma razão de projeto por trás, não só uma regra: no EPOS, handle() é
// chamado do handler de INTERRUPÇÃO DE HARDWARE da NIC.  O análogo fiel de
// interrupção em POSIX é sinal.  Manter isso preserva a estrutura do EPOS — e
// explica o resto do desenho:
//
//   * Conditional_Data_Observer::update não pode bloquear  -> roda em contexto
//     de interrupção;
//   * Concurrent_Observer usa semáforo                     -> sem_post(3) é uma
//     das poucas funções async-signal-safe (man 7 signal-safety);
//   * o pool de Buffers é pré-alocado                      -> malloc não é.
//
// O mecanismo, em dois fcntl:
//
//   fcntl(fd, F_SETOWN, getpid())            quem recebe o sinal
//   fcntl(fd, F_SETFL, ... | O_ASYNC | O_NONBLOCK)
//
// O sinal é SIGIO, o default do O_ASYNC — sem F_SETSIG.  A objeção conhecida é
// que sinal padrão NÃO enfileira: dois frames em rajada geram um sinal só.  Não
// importa aqui, porque a garantia de entrega é o LAÇO de drenagem, não a
// contagem de sinais — um único SIGIO manda drain() esvaziar a fila inteira.
// Sinal de tempo real (SIGRTMIN+n, com F_SETSIG e SA_SIGINFO) só passaria a
// valer a pena se houvesse mais de um descritor a distinguir por si_fd.
// Ver doc/decisoes.md.
//
// Por que O_NONBLOCK: o handler tem que DRENAR — chamar recvfrom em laço até
// EAGAIN — porque vários frames podem ter chegado antes de ele rodar.  Sem
// O_NONBLOCK o laço bloquearia na última iteração e você teria um processo
// dormindo dentro de um handler de sinal.
// =============================================================================

class Raw_Socket_Engine
{
protected:
    typedef Ethernet::Address Address;
    typedef Ethernet::Protocol Protocol;

    // Abre o raw socket em `iface` filtrando por `prot`.
    //
    // Contrato: depois de construir, engine_valid() diz se deu certo.  O
    // construtor NÃO lança exceção e NÃO chama exit() — quem constrói decide o
    // que fazer com a falha.
    //
    // NÃO arma a recepção.  Motivo: handle() é virtual e a classe derivada
    // (NIC) ainda não terminou de construir neste ponto.  Pior que antes,
    // aliás: com sinal, um frame que chegasse aqui chamaria um virtual puro em
    // objeto meio-construído.  Quem chama engine_start() é o construtor da NIC,
    // no fim.
    Raw_Socket_Engine(const char * iface, Protocol prot);

    virtual ~Raw_Socket_Engine();

    Raw_Socket_Engine(const Raw_Socket_Engine &) = delete;
    Raw_Socket_Engine & operator=(const Raw_Socket_Engine &) = delete;

    // -------------------------------------------------------------------------
    // Envio.  `frame` já vem montado por completo (dst, src, EtherType e
    // payload) e `size` é o total em bytes, cabeçalho incluído.
    //
    // Contrato: devolve o número de bytes entregues ao kernel, ou -1 com errno
    // preservado.  Não escreve em stdout — quem loga é a camada de cima.
    //
    // Roda na thread da aplicação, fora do handler.  Só que ele pode ser
    // INTERROMPIDO por um: um frame pode chegar no meio do seu sendto.  Tudo
    // que engine_send e o handler compartilharem precisa aguentar isso.
    // -------------------------------------------------------------------------
    int engine_send(const Ethernet::Frame * frame, unsigned int size);

    // -------------------------------------------------------------------------
    // Arma / desarma a recepção por sinal.
    //
    // engine_start(): a partir do retorno true, handle() pode ser chamado A
    // QUALQUER INSTRUÇÃO, na thread que estiver executando.  Tudo que handle()
    // tocar precisa estar pronto antes desta chamada.
    //
    // engine_stop(): quando retorna, nenhum sinal novo será gerado por este
    // socket e handle() não será chamado nunca mais.  Idempotente.
    //
    // >>> A pergunta 3 do guia — "how will a blocked receiver terminate cleanly
    // >>> during automated tests?" — some aqui.  Não existe receptor bloqueado
    // >>> para acordar: desarmar é tirar o O_ASYNC com um fcntl.  Diga isso na
    // >>> apresentação; é uma resposta melhor que qualquer uma das três que o
    // >>> modelo de thread produziria.
    // -------------------------------------------------------------------------
    bool engine_start();
    void engine_stop();

    // MAC real da interface, lido do kernel na construção.  Nada de MAC
    // hard-coded: o run-vm.sh gera 02:00:00:00:00:<id> por VM, e a origem do
    // frame tem que ser o endereço de verdade.
    const Address & engine_address() const { return _address; }

    bool engine_valid() const { return _sockfd >= 0; }

    // -------------------------------------------------------------------------
    // Callback de subida.  A NIC implementa.  Chamado uma vez por frame que
    // sobreviveu à filtragem.
    //
    // >>> RODA DENTRO DO SIGNAL HANDLER.  Esta linha é a mais importante do
    // >>> arquivo, e vale para TUDO que handle() alcançar — NIC::handle, alloc,
    // >>> notify, Protocol::update, Communicator::update.  Toda essa cadeia
    // está
    // >>> sujeita a async-signal-safety (man 7 signal-safety):
    // >>>
    // >>>   PODE:    recvfrom, write, sem_post, atômicos lock-free,
    // >>>            memcpy/memset, aritmética
    // >>>   NÃO PODE: printf e qualquer stdio, malloc/new, std::mutex,
    // >>>            std::string, std::cout, exceções
    // >>>
    // >>> printf é o que vai te pegar: é a primeira coisa que se quer fazer ao
    // >>> receber uma mensagem.  Use write(2) — está na lista — ou guarde o
    // dado
    // >>> e imprima no laço principal.
    //
    // Contrato: `frame` aponta para memória da Engine, válida SÓ durante a
    // chamada.  Quem quiser guardar, copia.  Retorne rápido: enquanto handle()
    // roda, a thread interrompida está parada.
    // -------------------------------------------------------------------------
    virtual void handle(Ethernet::Frame * frame, unsigned int size) = 0;

private:
    // Trampolim: um handler de sinal é função livre, não tem `this`.  A ponte
    // de volta para o objeto é um ponteiro estático.
    //
    // CONSEQUÊNCIA A DOCUMENTAR EM doc/decisoes.md: UMA Engine POR PROCESSO.
    // Disposição de sinal é estado global do processo — duas Engines no mesmo
    // processo disputariam o mesmo handler.  Para a Etapa 1 isso não incomoda
    // (um processo = um veículo = uma NIC); na Etapa 2, quando um veículo virar
    // vários processos, é a primeira suposição a revisar.
    static void signal_handler(int signo);
    static Raw_Socket_Engine * _instance;

    // O laço de drenagem: recvfrom até EAGAIN, filtrando, chamando handle().
    // Separado do trampolim para o handler ficar com uma linha só.
    void drain();

    // -------------------------------------------------------------------------
    // Estado.  Já declarado — você preenche no construtor.
    // -------------------------------------------------------------------------
    int _sockfd;                  // -1 enquanto inválido
    unsigned int _ifindex;        // if_nametoindex("eth0")
    Address _address;             // MAC real de eth0 (SIOCGIFHWADDR)
    Protocol _protocol;           // EtherType, em HOST order
    volatile sig_atomic_t _armed; // engine_start() já rodou?
    volatile sig_atomic_t _rx_error;
    volatile sig_atomic_t _rx_errors;

    // Nota: _armed é sig_atomic_t, não bool nem std::atomic<bool>.  É o único
    // tipo que o padrão garante ser seguro compartilhar entre um handler e o
    // código interrompido.  (std::atomic lock-free também serve; sig_atomic_t é
    // o que a norma POSIX nomeia, e não custa nada.)
};

#endif // LIBVCOMM_RAW_SOCKET_ENGINE_H
