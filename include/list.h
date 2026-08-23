#ifndef LIBVCOMM_LIST_H
#define LIBVCOMM_LIST_H

#include <atomic>
#include <cstddef>

// =============================================================================
// List / Ordered_List — apoio.  JÁ IMPLEMENTADAS.
//
// REESCRITAS em 23/08/2026.  A primeira versão usava std::deque/std::vector com
// std::mutex.  Isso deixou de ser legal quando a recepção passou a acontecer
// dentro de um signal handler:
//
//   - pthread_mutex_lock NÃO está na lista de funções async-signal-safe
//     (man 7 signal-safety).  Travar um mutex de dentro de um handler que
//     interrompeu a mesma thread já segurando esse mutex é deadlock imediato.
//   - deque::push_back e vector::push_back podem chamar malloc, que também não
//     está na lista.
//
// O que É permitido dentro de um handler, e é a base destas duas classes:
// objetos std::atomic LOCK-FREE.  Sem mutex, sem syscall, sem alocação.
// (C++17 [support.signal]; os static_assert abaixo cobram isso do compilador.)
//
// Isto é a mesma decisão que o EPOS toma com listas intrusivas, e pela mesma
// razão: no EPOS o topo da recepção roda em contexto de interrupção de
// hardware; aqui roda em contexto de sinal.  A restrição é a mesma.
//
// Preço pago, e você precisa saber defender: capacidade FIXA.  Sem alocação não
// existe crescer.  As duas classes dizem "não" quando enchem, em vez de alocar.
// =============================================================================

static_assert(std::atomic<unsigned int>::is_always_lock_free,
              "std::atomic<unsigned> nao e' lock-free nesta plataforma: a "
              "biblioteca padrao usaria mutex por baixo e o handler quebraria");

// -----------------------------------------------------------------------------
// List<T, CAP> — fila FIFO de ponteiros, um produtor e um consumidor (SPSC).
//
// Usada por Concurrent_Observer para guardar o que chegou e ainda não foi
// consumido.  E os dois lados dela são exatamente os dois contextos do projeto:
//
//     PRODUTOR  = o signal handler          -> insert()
//     CONSUMIDOR = a thread da aplicação    -> remove()
//
// Anel de tamanho fixo com dois índices atômicos.  Correto sem lock porque cada
// índice tem um único escritor: o produtor só escreve _tail, o consumidor só
// escreve _head.  O par release/acquire nesses índices é o que publica a
// escrita do slot para o outro lado — sem ele, o consumidor poderia ver o
// índice novo e o ponteiro velho.
//
// CAP tem que ser potência de 2 (o wraparound é um AND, não um módulo — divisão
// em caminho de interrupção é desperdício).  Um slot fica sempre vago para
// distinguir "cheia" de "vazia", então a capacidade útil é CAP-1.
// -----------------------------------------------------------------------------
template <typename T, unsigned int CAP = 32> class List
{
    static_assert(CAP >= 2 && (CAP & (CAP - 1)) == 0,
                  "CAP precisa ser potencia de 2 e >= 2");

public:
    List() : _head(0), _tail(0)
    {
        for (unsigned int i = 0; i < CAP; i++)
            _slot[i] = 0;
    }

    // Chamado do handler.  Devolve false se a fila estiver cheia.
    //
    // >>> ATENÇÃO: este bool é novo, e ignorá-lo é PERDER MENSAGEM em silêncio.
    // >>> Concurrent_Observer::update() hoje ignora.  Quando a fila enche, a
    // >>> mensagem que chegou não tem para onde ir — e esse é exatamente o
    // >>> contador Ethernet::Statistics::rx_dropped.  Decida o que fazer e
    // >>> escreva em doc/decisoes.md; "não pensei nisso" é a pior resposta.
    bool insert(T * e)
    {
        const unsigned int t = _tail.load(std::memory_order_relaxed);
        const unsigned int n = (t + 1) & (CAP - 1);
        if (n == _head.load(std::memory_order_acquire))
            return false;                                   // cheia
        _slot[t] = e;
        _tail.store(n, std::memory_order_release);          // publica o slot
        return true;
    }

    // Chamado da thread da aplicação.  Devolve 0 se estiver vazia.
    T * remove()
    {
        const unsigned int h = _head.load(std::memory_order_relaxed);
        if (h == _tail.load(std::memory_order_acquire))
            return 0;                                       // vazia
        T * e = _slot[h];
        _head.store((h + 1) & (CAP - 1), std::memory_order_release);
        return e;
    }

    bool empty() const
    {
        return _head.load(std::memory_order_acquire) ==
               _tail.load(std::memory_order_acquire);
    }

    unsigned int size() const
    {
        return (_tail.load(std::memory_order_acquire) -
                _head.load(std::memory_order_acquire)) & (CAP - 1);
    }

    static const unsigned int CAPACITY = CAP - 1;

private:
    T *                       _slot[CAP];
    std::atomic<unsigned int> _head;   // só o consumidor escreve
    std::atomic<unsigned int> _tail;   // só o produtor escreve
};

// -----------------------------------------------------------------------------
// Ordered_List<T, C, CAP> — coleção de observadores, cada um com um rank do
// tipo C.  O nome vem do PDF (Observed::Observers).
//
// Assimetria que define o desenho: attach()/detach() rodam na thread principal,
// na construção e destruição dos objetos; notify() PERCORRE de dentro do
// handler.  Ou seja, é muita leitura em contexto de sinal e pouquíssima escrita
// fora dele.
//
// Vetor de slots atômicos de tamanho fixo.  detach() não compacta nada: escreve
// 0 no slot (tombstone).  Compactar mexeria nos índices debaixo de um percurso
// que pode estar acontecendo neste instante dentro do handler.  Um insert()
// posterior reaproveita o slot vago antes de crescer.
//
// O Iterator PULA os slots vazios sozinho, para que o laço do notify() fique
// idêntico ao impresso no enunciado:
//
//     for(Observers::Iterator obs = _observers.begin(); obs != _observers.end();
//     obs++)
//         if(obs->rank() == c) ...
// -----------------------------------------------------------------------------
template <typename T, typename C, unsigned int CAP = 16> class Ordered_List
{
    static_assert(std::atomic<T *>::is_always_lock_free,
                  "std::atomic<T*> nao e' lock-free nesta plataforma");

public:
    class Iterator
    {
    public:
        Iterator(const std::atomic<T *> * p, const std::atomic<T *> * e)
            : _p(p), _e(e) { skip(); }

        T * operator*()  const { return _p->load(std::memory_order_acquire); }
        T * operator->() const { return _p->load(std::memory_order_acquire); }

        Iterator & operator++() { ++_p; skip(); return *this; }
        Iterator   operator++(int) { Iterator t(*this); ++(*this); return t; }

        bool operator==(const Iterator & i) const { return _p == i._p; }
        bool operator!=(const Iterator & i) const { return _p != i._p; }

    private:
        // Avança até o próximo slot ocupado.  É isto que faz o tombstone ser
        // invisível para quem percorre.
        void skip()
        {
            while (_p != _e && _p->load(std::memory_order_acquire) == 0)
                ++_p;
        }

        const std::atomic<T *> * _p;
        const std::atomic<T *> * _e;
    };

    Ordered_List() : _size(0)
    {
        for (unsigned int i = 0; i < CAP; i++)
            _slot[i].store(0, std::memory_order_relaxed);
    }

    Iterator begin() const
    {
        const unsigned int n = _size.load(std::memory_order_acquire);
        return Iterator(_slot, _slot + n);
    }

    Iterator end() const
    {
        const unsigned int n = _size.load(std::memory_order_acquire);
        return Iterator(_slot + n, _slot + n);
    }

    // Thread principal.  false = lotado (CAP observadores vivos).
    bool insert(T * e)
    {
        const unsigned int n = _size.load(std::memory_order_acquire);

        for (unsigned int i = 0; i < n; i++)               // reaproveita vago
            if (!_slot[i].load(std::memory_order_relaxed)) {
                _slot[i].store(e, std::memory_order_release);
                return true;
            }

        if (n >= CAP)
            return false;

        _slot[n].store(e, std::memory_order_release);      // slot ANTES
        _size.store(n + 1, std::memory_order_release);     // tamanho DEPOIS
        return true;
    }

    // Thread principal.  A partir do retorno, notify() não alcança mais `e`.
    void remove(T * e)
    {
        const unsigned int n = _size.load(std::memory_order_acquire);
        for (unsigned int i = 0; i < n; i++)
            if (_slot[i].load(std::memory_order_relaxed) == e)
                _slot[i].store(0, std::memory_order_release);
    }

    // Conta observadores VIVOS (ignora tombstones).
    unsigned int size() const
    {
        const unsigned int n = _size.load(std::memory_order_acquire);
        unsigned int live = 0;
        for (unsigned int i = 0; i < n; i++)
            if (_slot[i].load(std::memory_order_acquire))
                live++;
        return live;
    }

    bool empty() const { return size() == 0; }

    static const unsigned int CAPACITY = CAP;

private:
    mutable std::atomic<T *>  _slot[CAP];
    std::atomic<unsigned int> _size;   // marca d'água: só cresce
};

#endif // LIBVCOMM_LIST_H
