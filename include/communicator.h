#ifndef LIBVCOMM_COMMUNICATOR_H
#define LIBVCOMM_COMMUNICATOR_H

#include "observer.h"
#include "message.h"

// =============================================================================
// Communicator<Channel> — o ÚNICO ponto de contato da aplicação com a pilha.
//
// Um sensor, um fusor, uma ECU: nenhum deles conhece frame, EtherType ou
// socket.  Todos veem send(Message*) e receive(Message*).  É esta classe que
// cumpre o requisito de "API unificada para todos os agentes, independente de
// serem sistemas autônomos ou componentes dos mesmos".
//
// receive() BLOQUEIA — e é o único ponto da biblioteca que bloqueia.  Ele
// bloqueia no semáforo herdado de Concurrent_Observer, não em recvfrom().  Essa
// distinção é a resposta para "onde está o assincronismo exigido?": o handler
// de sinal nunca espera pela aplicação; ele deposita e segue.
//
// Note que o Communicator não endereça explicitamente o destino: manda sempre
// em broadcast.  Quem responde a quem é assunto das mensagens (Etapa 2 em
// diante).
// =============================================================================

template <typename Channel>
class Communicator : public Concurrent_Observer<
                         typename Channel::Observer::Observed_Data,
                         typename Channel::Observer::Observing_Condition>
{
    typedef Concurrent_Observer<typename Channel::Observer::Observed_Data,
                                typename Channel::Observer::Observing_Condition>
        Observer;

public:
    typedef typename Channel::Buffer Buffer;
    typedef typename Channel::Address Address;

    Communicator(Channel * channel, Address address);
    ~Communicator();

    Communicator(const Communicator &) = delete;
    Communicator & operator=(const Communicator &) = delete;

    // Contrato: true se a mensagem foi entregue ao kernel.  Não garante que
    // alguém recebeu — broadcast não tem confirmação.  Cuidado ao contar isso
    // como "mensagem entregue" na estatística da apresentação.
    bool send(const Message * message);

    // Contrato: BLOQUEIA até chegar mensagem para este endereço.  Devolve true
    // e preenche `message` com o payload e o tamanho REAL recebido.
    bool receive(Message * message);

    const Address & address() const { return _address; }

private:
    // Chamado pelo Protocol, de dentro do signal handler.
    void update(const typename Channel::Observer::Observing_Condition & c,
                Buffer * buf) override;

    Channel * _channel;
    Address _address;
};

// -----------------------------------------------------------------------------

template <typename Channel>
Communicator<Channel>::Communicator(Channel * channel, Address address)
    : _channel(channel), _address(address)
{
    _channel->attach(this, _address);
}

template <typename Channel> Communicator<Channel>::~Communicator()
{
    _channel->detach(this, _address);
}

template <typename Channel>
bool Communicator<Channel>::send(const Message * message)
{
    return _channel->send(_address,
                          Address::broadcast(),
                          message->data(),
                          message->size()) > 0;
}

template <typename Channel>
bool Communicator<Channel>::receive(Message * message)
{
    

    Buffer * buf = Observer::updated();  // <<< blocks here (semaphore p())

    if (!buf)
        return false;

    Address from;
    unsigned char tmp[Message::MAX_SIZE];
    int size = _channel->receive(buf, &from, tmp, Message::MAX_SIZE);
    // buf is freed by _channel->receive() do not touch buf after this

    if (size > 0) {
        message->set(tmp, static_cast<unsigned int>(size));
        return true;
    }
    return false;
}

template <typename Channel>
void Communicator<Channel>::update(
    const typename Channel::Observer::Observing_Condition & c, Buffer * buf)
{
    Observer::update(c, buf);
}

#endif // LIBVCOMM_COMMUNICATOR_H
