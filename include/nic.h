#ifndef LIBVCOMM_NIC_H
#define LIBVCOMM_NIC_H

#include <arpa/inet.h>

#include "traits.h"
#include "ethernet.h"
#include "buffer.h"
#include "observer.h"

// =============================================================================
// NIC<Engine> — a abstração PORTÁVEL da placa de rede.
//
// Nenhuma syscall aqui dentro.  A NIC sabe montar frame, gerenciar um pool de
// buffers e avisar quem se registrou por EtherType.  Como os bytes chegam ao
// meio é problema da Engine, que entra por herança privada.
//
// POR QUE `private Engine` E NÃO UM MEMBRO?
//   Herança privada dá acesso aos métodos protegidos da Engine sem expor nada
//   disso para fora da NIC, e permite sobrescrever handle() — que é como a
//   Engine empurra frames para cima.  Com um membro, você precisaria de um
//   ponteiro de volta ou de std::function.  Ambos funcionam; esta é a escolha
//   do EPOS e é a mais barata.  Saiba defender isso.
//
// POR QUE A NIC É OBSERVADA E NÃO CHAMA O PROTOCOL DIRETO?
//   Porque uma NIC pode servir vários protocolos ao mesmo tempo, cada um com
//   seu EtherType, e ela não pode conhecer nenhum deles.  Observer é o que
//   inverte essa dependência.
// =============================================================================

template <typename Engine>
class NIC : public Ethernet,
            public Conditionally_Data_Observed<::Buffer<Ethernet::Frame>,
                                               Ethernet::Protocol>,
            private Engine
{
public:
    static const unsigned int BUFFER_SIZE =
        Traits<Ethernet>::SEND_BUFFERS + Traits<Ethernet>::RECEIVE_BUFFERS;

    // Estes typedefs escondem os das duas bases (Ethernet e Engine, que ambas
    // têm um `Address`).  Sem eles, `Address` dentro da NIC é ambíguo.
    typedef Ethernet::Address Address;
    typedef Ethernet::Protocol Protocol_Number;
    typedef ::Buffer<Ethernet::Frame> Buffer;

    typedef Conditional_Data_Observer<Buffer, Protocol_Number> Observer;
    typedef Conditionally_Data_Observed<Buffer, Protocol_Number> Observed;

    // O PDF marca o construtor como protected (a NIC é singleton, instanciada
    // por Meta/Traits no EPOS).  Aqui fica público: sem uma factory, protected
    // deixaria a classe inutilizável.  Ver doc/decisoes.md.
    NIC();
    ~NIC();

    // -------------------------------------------------------------------------
    // Caminho simples (síncrono): copia `data` para um frame e envia.
    // Contrato: devolve bytes de PAYLOAD enviados, ou -1.
    // -------------------------------------------------------------------------
    int send(Address dst, Protocol_Number prot, const void * data,
             unsigned int size);

    // Extrai de um buffer já recebido.  Contrato: devolve bytes de payload
    // copiados para `data`, no máximo `size`.
    int receive(Address * src, Protocol_Number * prot, void * data,
                unsigned int size);

    // -------------------------------------------------------------------------
    // Caminho zero-copy: quem envia pede um buffer, escreve direto nele e
    // manda.
    // -------------------------------------------------------------------------
    // Reserva um buffer do pool e JÁ PREENCHE o cabeçalho (dst, src=nosso MAC,
    // prot).  Contrato: devolve 0 se o pool estiver esgotado — e devolver 0 é
    // comportamento normal, não erro fatal; quem chamou decide.
    Buffer * alloc(Address dst, Protocol_Number prot, unsigned int size);

    // Envia um buffer montado por alloc().  Contrato: libera o buffer ao final,
    // com sucesso ou não.  Documente essa escolha: a alternativa é o chamador
    // liberar, e misturar as duas é vazamento na certa.
    int send(Buffer * buf);

    // Devolve o buffer ao pool.
    void free(Buffer * buf);

    // Desmonta um buffer recebido: quem mandou, para quem, e o payload.
    // Contrato: devolve bytes de payload copiados, ou -1.
    int unmarshal(Buffer * buf, Address * src, Address * dst, void * data,
                  unsigned int size);

    const Address & address();
    void address(Address address);

    const Statistics & statistics() { return _statistics; }

    // attach()/detach() vêm de Conditionally_Data_Observed (base pública).
    // O PDF os lista com o comentário "possibly inherited" — é este o caso.

private:
    // Chamado DE DENTRO DO SIGNAL HANDLER da Engine, para cada frame que passou nos
    // filtros.  É a porta de entrada de tudo que sobe.
    void handle(Ethernet::Frame * frame, unsigned int size) override;

    Statistics _statistics;
    Buffer _buffer[BUFFER_SIZE];
};

// -----------------------------------------------------------------------------
// Implementação.  Template => tudo no header.
// -----------------------------------------------------------------------------

template <typename Engine>
NIC<Engine>::NIC()
    : Engine(Traits<Ethernet>::INTERFACE, Traits<Ethernet>::PROTOCOL_NUMBER)
{
    // TODO(joao): armar a recepção — e só DEPOIS que _buffer e _statistics já
    // existem, porque a partir daqui handle() pode ser chamado a QUALQUER
    // INSTRUÇÃO, na thread que estiver executando no momento do sinal.
    //     Engine::engine_start();
}

template <typename Engine> NIC<Engine>::~NIC()
{
    // TODO(joao): parar a Engine ANTES de destruir os buffers.
    //     Engine::engine_stop();
}

template <typename Engine>
int NIC<Engine>::send(Address dst, Protocol_Number prot, const void * data,
                      unsigned int size)
{
    // TODO(joao): alloc() -> memcpy do payload -> send(buf).
    // Repare que este método não deve conhecer sockaddr_ll: ele chama a Engine.
    (void)dst;
    (void)prot;
    (void)data;
    (void)size;
    return -1;
}

template <typename Engine>
int NIC<Engine>::receive(Address * src, Protocol_Number * prot, void * data,
                         unsigned int size)
{
    // TODO(joao): o PDF prevê este método síncrono.  Na arquitetura orientada a
    // Observer ele é redundante com o par handle()/notify(); implemente só se o
    // seu teste precisar.  Se não precisar, deixe-o devolvendo -1 e EXPLIQUE a
    // decisão em doc/ — decisão documentada vale mais que método morto.
    (void)src;
    (void)prot;
    (void)data;
    (void)size;
    return -1;
}

template <typename Engine>
typename NIC<Engine>::Buffer *
NIC<Engine>::alloc(Address dst, Protocol_Number prot, unsigned int size)
{
    for (unsigned int i = 0; i < BUFFER_SIZE; i++) {
        if (_buffer[i].lock()) {
            Ethernet::Frame * f = _buffer[i].frame();
            f->dst  = dst;
            f->src  = Engine::engine_address();
            f->prot = htons(prot);
            _buffer[i].size(Ethernet::HEADER_SIZE + size);
            return &_buffer[i];
        }
    }
    return 0;
}

template <typename Engine> int NIC<Engine>::send(Buffer * buf)
{
    // TODO(joao): Engine::engine_send(buf->frame(), buf->size()),
    //             atualizar _statistics, free(buf), devolver o resultado.
    (void)buf;
    return -1;
}

template <typename Engine> void NIC<Engine>::free(Buffer * buf)
{
    if (buf)
        buf->unlock();
}

template <typename Engine>
int NIC<Engine>::unmarshal(Buffer * buf, Address * src, Address * dst,
                           void * data, unsigned int size)
{
    // TODO(joao): ler o cabeçalho do buf->frame(), copiar até `size` bytes de
    // payload e devolver quantos.  Não liberar o buffer aqui — quem libera é
    // quem chamou (ver a nota de posse em buffer.h).
    (void)buf;
    (void)src;
    (void)dst;
    (void)data;
    (void)size;
    return -1;
}

template <typename Engine>
const typename NIC<Engine>::Address & NIC<Engine>::address()
{
    return Engine::engine_address();
}

template <typename Engine> void NIC<Engine>::address(Address address)
{
    // TODO(joao): a Etapa 1 usa o MAC real de eth0 e não deixa ninguém
    // trocá-lo. Este setter existe na API do PDF; decida se ele faz sentido
    // aqui e documente.  Um método que mente é pior que um método ausente.
    (void)address;
}

template <typename Engine>
void NIC<Engine>::handle(Ethernet::Frame * frame, unsigned int size)
{
    // TODO(joao): a ponte entre a Engine e o resto da pilha.
    //
    //   1. alloc de um buffer de recepção (ou varrer o pool direto — mas note
    //      que alloc() preenche cabeçalho de ENVIO; talvez você queira um
    //      alloc_receive() separado.  Decisão sua, documente).
    //      Sem buffer livre: _statistics.rx_dropped++ e RETORNAR.  Descartar é
    //      resposta legítima; esperar por um buffer dentro de um handler não é.
    //
    //   ATENÇÃO AO CONTEXTO: tudo aqui roda dentro do signal handler.  Nada de
    //   printf para depurar (use write(2)), nada de new.  O pool é pré-alocado
    //   exatamente por isso.
    //   2. copiar `frame` para o buffer (a memória da Engine morre no retorno).
    //   3. buf->size(size); atualizar rx_packets/rx_bytes.
    //   4. Observed::notify(ntohs(frame->prot), buf);
    //      -> se devolver false, NINGUÉM quis o frame: free(buf).  Este `if` é
    //         a diferença entre rodar a noite toda e vazar 32 buffers.
    (void)frame;
    (void)size;
}

#endif // LIBVCOMM_NIC_H
