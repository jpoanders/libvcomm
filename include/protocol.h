#ifndef LIBVCOMM_PROTOCOL_H
#define LIBVCOMM_PROTOCOL_H

#include <cstring>
#include "traits.h"
#include "ethernet.h"
#include "observer.h"

// =============================================================================
// Protocol<NIC> — multiplexa UMA NIC entre VÁRIOS interlocutores.
//
// A NIC separa por EtherType (todos os frames do projeto têm o mesmo).  O
// Protocol separa por Port: é ele que decide qual processo, dentro da VM, deve
// acordar com a mensagem que chegou.
//
// DUAS CARAS, e é isso que confunde na primeira leitura:
//
//   - Como OBSERVADOR da NIC:  herda NIC::Observer e implementa update().
//     Roda DENTRO DO SIGNAL HANDLER, não pode bloquear.
//   - Como OBSERVADO pelos Communicators: tem um
//   Concurrent_Observed<Buffer,Port>
//     dentro de si.  Este sim desacopla por semáforo — do outro lado tem uma
//     thread de aplicação dormindo.
//
// É essa dobra que faz a recepção ser assíncrona ponta a ponta sem rendezvous.
// =============================================================================
//
// DESVIOS DO PDF (todos anotados em doc/decisoes.md):
//
//  1. O PDF escreve `class Protocol: private typename NIC::Observer`.  O
//     `typename` não pode aparecer numa lista de bases — em base-clause o nome
//     já é lido como tipo.  Removido.
//
//  2. O PDF declara `typedef Conditional_Data_Observer<..., Port> Observer;`,
//     mas o Communicator do próprio PDF herda de Concurrent_Observer.  As duas
//     coisas não encaixam.  Quem manda é o Communicator: aqui Observer é
//     Concurrent_Observer — é ele que tem o semáforo que a aplicação precisa.
//
//  3. O PDF declara send()/receive() `static` mas usa `_nic`, que é membro de
//     instância; e o Communicator os chama como `_channel->send(...)`.  Aqui
//     são métodos de instância.
//
//  4. O PDF tem `static Observed _observed;` ("channel protocols are usually
//     singletons").  Membro estático de template exige definição fora da classe
//     e impede dois protocolos no mesmo processo.  Aqui é membro de instância.

template <typename NIC_T> class Protocol : private NIC_T::Observer
{
public:
    typedef typename NIC_T::Buffer Buffer;
    typedef typename NIC_T::Address Physical_Address;
    typedef typename NIC_T::Protocol_Number Protocol_Number;

    // O PDF escreve literalmente `typedef XXX Port;` — a escolha é sua.
    // unsigned short cobre a Etapa 1.  Na Etapa 2 o enunciado provoca: usar o
    // PID é tentador e barato, mas como a VM 2 saberia o PID de um processo da
    // VM 1?  Decida agora ou pague depois.
    typedef unsigned short Port;

    static const Protocol_Number PROTO = Traits<Ethernet>::PROTOCOL_NUMBER;

    typedef Concurrent_Observer<Buffer, Port> Observer;
    typedef Concurrent_Observed<Buffer, Port> Observed;

    // -------------------------------------------------------------------------
    // Address = (endereço físico, porta).  É o endereço LÓGICO da biblioteca.
    // -------------------------------------------------------------------------
    class Address
    {
    public:
        enum Null
        {
            NULL_ADDRESS = 0
        };

        Address() : _paddr(), _port(0) {}
        Address(const Null &) : _paddr(), _port(0) {}
        Address(Physical_Address paddr, Port port) : _paddr(paddr), _port(port)
        {}

        const Physical_Address & paddr() const { return _paddr; }
        Port port() const { return _port; }

        operator bool() const { return (_paddr || _port); }
        bool operator==(const Address & a) const
        {
            return (_paddr == a._paddr) && (_port == a._port);
        }
        bool operator!=(const Address & a) const { return !(*this == a); }

        // O PDF escreve `Channel::Address::BROADCAST`.  Um membro estático de
        // classe aninhada dentro de template precisa de definição fora da
        // classe; uma função estática dá o mesmo resultado sem a cerimônia.
        static Address broadcast(Port p = 0)
        {
            return Address(Ethernet::BROADCAST, p);
        }

    private:
        Physical_Address _paddr;
        Port _port;
    } __attribute__((packed));

    // -------------------------------------------------------------------------
    // Header / Packet — o que o Protocol acrescenta ao payload do Ethernet.
    //
    // >>> DECISÃO DE PROJETO SUA, e ela tem consequência real:
    //     você MEDIU que o virtio-net do QEMU não faz padding para 60 bytes, e
    //     por isso `tamanho = bytes_recebidos - 14` funciona nesta bancada.  Em
    //     hardware real, e na Engine de memória compartilhada da Etapa 2, não
    //     funciona.  Se você quer o tamanho correto em qualquer meio, um campo
    //     `length` aqui resolve de uma vez.  As Etapas 2 a 6 vão pedir origem,
    //     timestamp, tipo e MAC — este Header é onde eles entram.
    // -------------------------------------------------------------------------
    class Header
    {
    public:
        Header() : _from_port(0), _to_port(0), _length(0) {}

        // TODO(joao): confirme os campos que a Etapa 1 realmente precisa e
        // acrescente os acessores.  Menos é mais aqui — cada byte deste
        // cabeçalho é payload que você perde.
        Port _from_port;
        Port _to_port;
        unsigned short _length; // bytes de payload; ver a nota acima
    } __attribute__((packed));

    static const unsigned int MTU = Ethernet::MTU - sizeof(Header);
    typedef unsigned char Data[MTU];

    class Packet : public Header
    {
    public:
        Packet() {}
        Header * header() { return this; }

        template <typename T> T * data()
        {
            return reinterpret_cast<T *>(&_data);
        }

    private:
        Data _data;
    } __attribute__((packed));

    // -------------------------------------------------------------------------
    explicit Protocol(NIC_T * nic);
    ~Protocol();

    // Contrato: devolve bytes de payload enviados, ou -1.
    int send(Address from, Address to, const void * data, unsigned int size);

    // Consome um buffer JÁ recebido (o Communicator o pegou de updated()).
    // Contrato: preenche `from`, copia até `size` bytes e LIBERA o buffer —
    // com sucesso ou não.  A posse termina aqui.
    int receive(Buffer * buf, Address * from, void * data, unsigned int size);

    void attach(Observer * obs, const Address & address);
    void detach(Observer * obs, const Address & address);

private:
    // Chamado pela NIC, DENTRO DO SIGNAL HANDLER.  Não pode bloquear, e está
    // sujeito a async-signal-safety (man 7 signal-safety).
    void update(const Protocol_Number & prot, Buffer * buf) override;

    NIC_T * _nic;
    Observed _observed;
};

// -----------------------------------------------------------------------------

template <typename NIC_T>
Protocol<NIC_T>::Protocol(NIC_T * nic) : NIC_T::Observer(PROTO), _nic(nic)
{
    // TODO(joao): registrar-se na NIC para o nosso EtherType.
    //     _nic->attach(this, PROTO);
}

template <typename NIC_T> Protocol<NIC_T>::~Protocol()
{
    // TODO(joao): _nic->detach(this, PROTO);
    // Sem isto, a NIC guarda um ponteiro para um objeto destruído e o próximo
    // frame que chegar chama um método de memória morta.
}

template <typename NIC_T>
int Protocol<NIC_T>::send(Address from, Address to, const void * data,
                          unsigned int size)
{
    // TODO(joao): o roteiro está no próprio PDF:
    //     Buffer * buf = _nic->alloc(to.paddr(), PROTO, sizeof(Header) + size);
    //     montar o Header (portas de origem/destino, length) no payload do
    //     frame copiar `data` logo depois do Header return _nic->send(buf);
    // Lembre: `to.paddr()` na Etapa 1 é SEMPRE broadcast — exigência do
    // enunciado.
    (void)from;
    (void)to;
    (void)data;
    (void)size;
    return -1;
}

template <typename NIC_T>
int Protocol<NIC_T>::receive(Buffer * buf, Address * from, void * data,
                             unsigned int size)
{
    // TODO(joao):
    //     _nic->unmarshal(...) para pegar o MAC de origem e o payload
    //     ler o Header para descobrir a porta de origem -> montar *from
    //     copiar o payload do protocolo para `data`
    //     _nic->free(buf);   <<< SEMPRE, inclusive no caminho de erro
    (void)buf;
    (void)from;
    (void)data;
    (void)size;
    return -1;
}

template <typename NIC_T>
void Protocol<NIC_T>::attach(Observer * obs, const Address & address)
{
    // TODO(joao): _observed.attach(obs, address.port());
    // A condição é a PORTA, não o Address inteiro: dois Communicators com o
    // mesmo paddr e portas diferentes têm que ser distinguíveis.
    (void)obs;
    (void)address;
}

template <typename NIC_T>
void Protocol<NIC_T>::detach(Observer * obs, const Address & address)
{
    // TODO(joao): _observed.detach(obs, address.port());
    (void)obs;
    (void)address;
}

template <typename NIC_T>
void Protocol<NIC_T>::update(const Protocol_Number & prot, Buffer * buf)
{
    // TODO(joao): este método é curto e é o coração da pilha.  O PDF já dá:
    //
    //     if(!_observed.notify(<porta de destino lida do Header>, buf))
    //         _nic->free(buf);
    //
    // Traduzindo: se nenhum Communicator estava escutando aquela porta, o frame
    // não interessa a ninguém e o buffer volta para o pool AGORA.  Se algum
    // estava, a posse passa para ele e quem libera é o receive() dele.
    (void)prot;
    (void)buf;
}

#endif // LIBVCOMM_PROTOCOL_H
