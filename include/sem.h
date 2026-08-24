#ifndef LIBVCOMM_SEMAPHORE_H
#define LIBVCOMM_SEMAPHORE_H

// WHY THIS FILE IS CALLED sem.h AND NOT semaphore.h:
//   with -Iinclude, gcc searches include/ BEFORE /usr/include even for
//   #include <...>.  A file of ours named semaphore.h would make the
//   `#include <semaphore.h>` below include itself — the include guard cuts the
//   recursion and sem_t simply does not exist.  A mistake that costs half an
//   hour the first time you see it.
#include <semaphore.h>
#include <cerrno>

// =============================================================================
// Semaphore — support class.  ALREADY IMPLEMENTED.  Thin wrapper over POSIX
// sem_t.
// =============================================================================
//
// WHY A SEMAPHORE AND NOT A RENDEZVOUS (panel answer):
//   The assignment forbids a rendezvous on reception.  A semaphore decouples in
//   time: the producer (the signal handler) does v() and moves on without
//   waiting for anyone; the consumer does p() and sleeps until data is there.
//   If the message arrives BEFORE the application calls receive(), the
//   semaphore's counter is already 1 and p() returns immediately — nothing is
//   lost.  It is exactly that memory of a past event that a rendezvous lacks.
//
// Classic pitfall: sem_wait() returns -1/EINTR when a POSIX signal arrives.
// That is NOT an error, it means try again.  The loop below handles it.

class Semaphore
{
public:
    explicit Semaphore(int init = 1)
    {
        sem_init(&_sem, 0 /* threads, not processes */, init);
    }
    ~Semaphore() { sem_destroy(&_sem); }

    Semaphore(const Semaphore &) = delete;
    Semaphore & operator=(const Semaphore &) = delete;

    // p() / down / wait — blocks while the counter is zero.
    void p()
    {
        while (sem_wait(&_sem) == -1 && errno == EINTR)
            ;
    }

    // v() / up / post — never blocks.
    void v() { sem_post(&_sem); }

    // Tries without blocking.  Useful to drain leftovers at shutdown.
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
