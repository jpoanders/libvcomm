#ifndef LIBVCOMM_BUFFER_H
#define LIBVCOMM_BUFFER_H

#include <atomic>

// =============================================================================
// Buffer<T> — apoio.  JÁ IMPLEMENTADO.
//
// É o objeto que sobe a pilha: Engine escreve nele, NIC o entrega ao Protocol,
// Protocol ao Communicator.  Ninguém copia o frame no caminho — passa-se o
// ponteiro.  É o "zero-copy" que o EPOS persegue.
// =============================================================================
//
// PERGUNTA 4 DO GUIA DO PROFESSOR: "quem é dono de um buffer recebido, e quando
// ele é liberado?"  A resposta desta biblioteca:
//
//   - O pool vive dentro da NIC (NIC::_buffer[BUFFER_SIZE]).
//   - alloc() marca um buffer como em uso e transfere a posse a quem chamou.
//   - A posse desce junto com o ponteiro: quem recebeu o buffer é quem deve
//     chamar NIC::free() — veja Protocol::update(), que libera o buffer quando
//     NENHUM observador o quis.
//   - free() devolve o buffer ao pool.  Esquecer um free() = vazamento
//   silencioso
//     que só aparece depois de BUFFER_SIZE mensagens.  Ver o teste de exaustão
//     de pool em tests/.
//
// lock() usa test-and-set atômico porque o pool é disputado entre a thread de
// recepção (que precisa de um buffer para o frame que acabou de chegar) e a
// thread da aplicação (que precisa de um para enviar).

template <typename T> class Buffer
{
public:
    typedef T Data;

    Buffer() : _size(0), _in_use(false) {}

    // Ponteiro para a área onde o frame realmente mora.
    T * frame() { return &_frame; }
    const T * frame() const { return &_frame; }

    // Quantos bytes deste buffer são válidos (cabeçalho + payload).
    unsigned int size() const { return _size; }
    void size(unsigned int s) { _size = s; }

    // Tenta reservar este buffer.  Devolve true se conseguiu — e, se conseguiu,
    // mais ninguém consegue até unlock().  Falha sem bloquear.
    bool lock() { return !_in_use.exchange(true, std::memory_order_acq_rel); }

    void unlock()
    {
        _size = 0;
        _in_use.store(false, std::memory_order_release);
    }

    bool in_use() const { return _in_use.load(std::memory_order_acquire); }

private:
    T _frame;
    unsigned int _size;
    std::atomic<bool> _in_use;
};

#endif // LIBVCOMM_BUFFER_H
