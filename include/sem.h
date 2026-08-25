#ifndef LIBVCOMM_SEMAPHORE_H
#define LIBVCOMM_SEMAPHORE_H

#include <semaphore.h>
#include <cerrno>
#include <ctime>

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

    bool p(unsigned int timeout_ms)
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
            return false;

        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += static_cast<long>(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            ts.tv_sec += 1;
        }

        while (sem_timedwait(&_sem, &ts) == -1) {
            if (errno == EINTR)
                continue;
            return false; // ETIMEDOUT
        }
        return true;
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
