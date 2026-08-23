#ifndef LIBVCOMM_SEMAPHORE_H
#define LIBVCOMM_SEMAPHORE_H

// POR QUE ESTE ARQUIVO SE CHAMA sem.h E NÃO semaphore.h:
//   com -Iinclude, o gcc procura include/ ANTES de /usr/include até para
//   #include <...>.  Um arquivo nosso chamado semaphore.h faria o
//   `#include <semaphore.h>` abaixo incluir a si mesmo — o include guard
//   corta a recursão e sem_t simplesmente não existe.  Erro que custa meia
//   hora quando se vê pela primeira vez.
#include <semaphore.h>
#include <cerrno>

// =============================================================================
// Semaphore — apoio.  JÁ IMPLEMENTADO.  Envelope fino sobre sem_t do POSIX.
// =============================================================================
//
// POR QUE SEMÁFORO E NÃO RENDEZVOUS (resposta de banca):
//   O enunciado proíbe rendezvous na recepção.  O semáforo desacopla no tempo:
//   quem produz (o signal handler) faz v() e segue em frente sem esperar
//   ninguém; quem consome faz p() e dorme até haver dado.  Se a mensagem chegar
//   ANTES de a aplicação chamar receive(), o contador do semáforo já está em 1
//   e o p() retorna na hora — nada se perde.  É exatamente essa memória de um
//   evento passado que um rendezvous não tem.
//
// Cuidado clássico: sem_wait() retorna -1/EINTR quando um sinal POSIX chega.
// Isso NÃO é erro, é para tentar de novo. O laço abaixo trata isso.

class Semaphore
{
public:
    explicit Semaphore(int init = 1)
    {
        sem_init(&_sem, 0 /* threads, não processos */, init);
    }
    ~Semaphore() { sem_destroy(&_sem); }

    Semaphore(const Semaphore &) = delete;
    Semaphore & operator=(const Semaphore &) = delete;

    // p() / down / wait — bloqueia enquanto o contador for zero.
    void p()
    {
        while (sem_wait(&_sem) == -1 && errno == EINTR)
            ;
    }

    // v() / up / post — nunca bloqueia.
    void v() { sem_post(&_sem); }

    // Tenta sem bloquear.  Útil para drenar o que sobrou no encerramento.
    bool try_p() { return sem_trywait(&_sem) == 0; }

    int value()
    {
        int v = 0;
        sem_getvalue(&_sem, &v);
        return v;
    }

private:
    sem_t _sem;
};

#endif // LIBVCOMM_SEMAPHORE_H
