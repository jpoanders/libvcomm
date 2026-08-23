#ifndef LIBVCOMM_OBSERVER_H
#define LIBVCOMM_OBSERVER_H

#include "list.h"
#include "sem.h"

// =============================================================================
// Observer X Observed — os "Fundamentals" do PDF.
//
// Duas famílias, e a diferença entre elas é a razão de existirem as duas:
//
//   Conditional_*  -> notificação SÍNCRONA e condicional.  update() roda no
//                     contexto de quem notificou (o SIGNAL HANDLER).  Não
//                     bloqueia ninguém.  É como NIC avisa Protocol e como
//                     Protocol avisa Communicator.
//
//   Concurrent_*   -> notificação com DESACOPLAMENTO por semáforo.  update()
//                     só enfileira e faz v(); quem chamou updated() estava
//                     dormindo em p() e acorda.  É a fronteira entre o handler
//                     e a thread da aplicação.
//
// O caminho completo de uma mensagem:
//
//   handler:   Engine::drain() -> recvfrom() -> NIC::handle() ->
//   Observed::notify(prot,buf)
//                -> Protocol::update() -> Observed::notify(port,buf)
//                -> Communicator::update() -> Concurrent_Observer::update()
//                -> _semaphore.v()          <<< o handler termina aqui
//   app thread: Communicator::receive() -> Concurrent_Observer::updated()
//                -> _semaphore.p()          <<< acorda com o buffer na mão
//
// -----------------------------------------------------------------------------
// POR QUE SÃO DUAS FAMÍLIAS, E NÃO UMA  (resposta de banca)
// -----------------------------------------------------------------------------
// Porque o topo da cadeia roda em CONTEXTO DE INTERRUPÇÃO.  No EPOS, handle()
// é chamado do handler de interrupção de hardware da NIC; aqui é o análogo
// POSIX, um signal handler.  Isso não é detalhe de implementação — é o que
// obriga o desenho inteiro:
//
//   * Conditional_* não pode bloquear -> quem bloqueia dentro de um handler
//     trava a thread que foi interrompida, que não tem nada a ver com isso;
//   * Concurrent_* usa semáforo       -> sem_post(3) é uma das poucas funções
//     async-signal-safe (man 7 signal-safety).  Não é coincidência: a cadeia
//     foi desenhada para ser legal a partir de um handler.
//
// TODA função alcançável a partir de notify() está sujeita a essa restrição.
// Nada de printf/stdio, malloc/new, std::mutex, std::string.  É por isso que
// list.h é lock-free e sem alocação, e que o pool de Buffers é pré-alocado.
// =============================================================================

// -----------------------------------------------------------------------------
// Conditional_Data_Observer<T, C> — interface pura.  Quem quer ser avisado
// implementa update().
//
// O rank é a CONDIÇÃO: o valor que o observador está esperando.  Para a NIC o
// rank é o EtherType; para o Protocol é a Port.  notify(c, d) só chama quem tem
// rank == c.  É isso, e só isso, que "conditional" significa aqui.
// -----------------------------------------------------------------------------
template <typename T, typename C = void> class Conditional_Data_Observer
{
public:
    typedef T Observed_Data;
    typedef C Observing_Condition;

    Conditional_Data_Observer() : _rank() {}
    explicit Conditional_Data_Observer(const C & c) : _rank(c) {}
    virtual ~Conditional_Data_Observer() {}

    virtual void update(const C & c, T * d) = 0;

    const C & rank() const { return _rank; }
    void rank(const C & c) { _rank = c; }

protected:
    C _rank;
};

// -----------------------------------------------------------------------------
// Conditionally_Data_Observed<T, C> — quem é observado.
//
// >>> SEUS TRÊS MÉTODOS.  Comece por aqui: são as ~15 linhas mais baratas do
// >>> projeto e destravam NIC e Protocol de uma vez.
// -----------------------------------------------------------------------------
template <typename T, typename C = void> class Conditionally_Data_Observed
{
public:
    typedef Conditional_Data_Observer<T, C> Observer;
    typedef Ordered_List<Observer, C> Observers;

    Conditionally_Data_Observed() {}
    virtual ~Conditionally_Data_Observed() {}

    // Registra um observador interessado na condição c.
    // Contrato: depois de attach(o, c), todo notify(c, d) deve chamar
    // o->update(c, d).
    void attach(Observer * o, const C & c);

    // Remove o observador.  Contrato: depois de detach(o, c), o nunca mais é
    // chamado.  Chamado do destrutor de Protocol/Communicator — se falhar,
    // notify() chama um objeto morto.
    void detach(Observer * o, const C & c);

    // Entrega d a TODOS os observadores cujo rank case com c.
    // Contrato: devolve true se pelo menos um observador foi notificado.
    // O false é o que diz à NIC "ninguém quis este frame, pode liberar o
    // buffer".
    bool notify(const C & c, T * d);

protected:
    Observers _observers;
};

template <typename T, typename C>
void Conditionally_Data_Observed<T, C>::attach(Observer * o, const C & c)
{
    o->rank(c);
    _observers.insert(o);
}

template <typename T, typename C>
void Conditionally_Data_Observed<T, C>::detach(Observer * o, const C & c)
{
    (void)c;
    _observers.remove(o);
}

template <typename T, typename C>
bool Conditionally_Data_Observed<T, C>::notify(const C & c, T * d)
{
    bool notified = false;
    for (typename Observers::Iterator obs = _observers.begin();
         obs != _observers.end(); obs++) {
        if (obs->rank() == c) {
            obs->update(c, d);
            notified = true;
        }
    }
    return notified;
}

// -----------------------------------------------------------------------------
// Concurrent_Observed / Concurrent_Observer
//
// TRANSCRITOS do full_assignment.pdf (bloco azul, "Fundamentals for Observer X
// Observed").  O corpo destes métodos é do enunciado, não meu — está aqui para
// você ter a base compilando, e vale a pena ler linha a linha porque cai na
// banca.
//
// UMA CORREÇÃO NECESSÁRIA: o PDF chama obs->rank() dentro de notify(), mas o
// Concurrent_Observer do PDF não tem campo _rank nem construtor que o receba.
// Do jeito que está impresso, não compila.  Herdamos o _rank da mesma ideia do
// Conditional_Data_Observer.  Anote em doc/decisoes.md — mostrar que você achou
// a inconsistência é melhor do que fingir que ela não existe.
// -----------------------------------------------------------------------------

template <typename D, typename C = void> class Concurrent_Observer;

template <typename D, typename C = void> class Concurrent_Observed
{
    friend class Concurrent_Observer<D, C>;

public:
    typedef D Observed_Data;
    typedef C Observing_Condition;
    typedef Ordered_List<Concurrent_Observer<D, C>, C> Observers;

    Concurrent_Observed() {}
    virtual ~Concurrent_Observed() {}

    void attach(Concurrent_Observer<D, C> * o, const C & c)
    {
        o->rank(c);
        _observers.insert(o);
    }

    void detach(Concurrent_Observer<D, C> * o, const C &)
    {
        _observers.remove(o);
    }

    bool notify(const C & c, D * d)
    {
        bool notified = false;
        for (typename Observers::Iterator obs = _observers.begin();
             obs != _observers.end(); obs++) {
            if (obs->rank() == c) {
                obs->update(c, d);
                notified = true;
            }
        }
        return notified;
    }

protected:
    Observers _observers;
};

template <typename D, typename C> class Concurrent_Observer
{
    friend class Concurrent_Observed<D, C>;

public:
    typedef D Observed_Data;
    typedef C Observing_Condition;

    Concurrent_Observer() : _semaphore(0), _rank() {}
    virtual ~Concurrent_Observer() {}

    // Chamado DENTRO DO SIGNAL HANDLER.  Repare que ele não bloqueia: enfileira
    // e acorda.  É este par insert()/v() que substitui o rendezvous proibido —
    // e as duas metades são async-signal-safe, o que é o que torna esta linha
    // legal aqui.
    //
    // >>> _data.insert() devolve bool desde 23/08 e esta linha IGNORA.  Fila
    // >>> cheia = mensagem descartada em silêncio.  Ver a nota em list.h.
    virtual void update(const C & c, D * d)
    {
        (void)c;
        _data.insert(d);
        _semaphore.v();
    }

    // Chamado NA THREAD DA APLICAÇÃO.  Bloqueia até haver dado.
    D * updated()
    {
        _semaphore.p();
        return _data.remove();
    }

    const C & rank() const { return _rank; }
    void rank(const C & c) { _rank = c; }

private:
    Semaphore _semaphore;
    List<D> _data;
    C _rank;
};

#endif // LIBVCOMM_OBSERVER_H
