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
    // TODO(joao): _channel->attach(this, _address);
}

template <typename Channel> Communicator<Channel>::~Communicator()
{
    // TODO(joao): _channel->detach(this, _address);
    // O PDF escreve `Channel::detach(this, _address)` — chamada estática num
    // método de instância.  Use a forma de instância; anote o desvio.
}

template <typename Channel>
bool Communicator<Channel>::send(const Message * message)
{
    // TODO(joao):
    //     return _channel->send(_address,
    //                           Address::broadcast(),
    //                           message->data(),
    //                           message->size()) > 0;
    (void)message;
    return false;
}

template <typename Channel>
bool Communicator<Channel>::receive(Message * message)
{
    // TODO(joao): as quatro linhas mais importantes da biblioteca.
    //
    //     Buffer * buf = Observer::updated();   // <<< dorme aqui até chegar
    //     algo Address from; int size = _channel->receive(buf, &from,
    //     message->data(), Message::MAX_SIZE); if(size > 0) {
    //     message->size(size); return true; } return false;
    //
    // ARMADILHA: o PDF passa `message->size()` como capacidade.  Numa mensagem
    // recém-construída isso é 0 e você recebe zero bytes para sempre, sem erro
    // nenhum.  Passe a CAPACIDADE na entrada e escreva o tamanho recebido na
    // saída — não use o mesmo campo para as duas coisas.
    //
    // ARMADILHA 2: updated() pode devolver 0 se você acordar o semáforo no
    // encerramento para destravar a aplicação.  Trate isso ou o teste
    // automatizado vai morrer com segfault no shutdown.
    //
    // ARMADILHA 3, nova com o modelo de sinal: sem_wait() dentro de p() volta
    // com EINTR quando um frame chega enquanto você espera — a MENOS que o
    // sigaction do SIGIO tenha SA_RESTART.  Medido nas duas configurações em
    // 23/08/2026: sem SA_RESTART -> -1/EINTR(4); com SA_RESTART -> 0, o
    // sem_wait retoma sozinho.  O sem.h já faz o laço de EINTR de qualquer
    // jeito, e é assim que deve ser: o laço custa duas linhas e vale sob as
    // duas escolhas.  Se você escrever um p() próprio, não esqueça dele.
    (void)message;
    return false;
}

template <typename Channel>
void Communicator<Channel>::update(
    const typename Channel::Observer::Observing_Condition & c, Buffer * buf)
{
    // TODO(joao): Observer::update(c, buf);
    // Uma linha: enfileira e faz v().  É ela que libera a thread parada em
    // receive().  Nada de processar a mensagem aqui — você está DENTRO DE UM
    // SIGNAL HANDLER, com a thread interrompida parada esperando você sair.
    (void)c;
    (void)buf;
}

#endif // LIBVCOMM_COMMUNICATOR_H
