#ifndef LIBVCOMM_LIST_H
#define LIBVCOMM_LIST_H

#include <atomic>
#include <cstddef>

static_assert(std::atomic<unsigned int>::is_always_lock_free,
              "std::atomic<unsigned> is not lock-free on this platform: the "
              "standard library would use a mutex underneath and the handler "
              "would break");

template <typename T, unsigned int CAP = 32> class List
{
    static_assert(CAP >= 2 && (CAP & (CAP - 1)) == 0,
                  "CAP must be a power of 2 and >= 2");

public:
    List() : _head(0), _tail(0)
    {
        for (unsigned int i = 0; i < CAP; i++)
            _slot[i] = 0;
    }

    bool insert(T * e)
    {
        const unsigned int t = _tail.load(std::memory_order_relaxed);
        const unsigned int n = (t + 1) & (CAP - 1);
        if (n == _head.load(std::memory_order_acquire))
            return false;                                   // full
        _slot[t] = e;
        _tail.store(n, std::memory_order_release);          // publishes the slot
        return true;
    }

    // Called from the application thread.  Returns 0 if empty.
    T * remove()
    {
        const unsigned int h = _head.load(std::memory_order_relaxed);
        if (h == _tail.load(std::memory_order_acquire))
            return 0;                                       // empty
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
    std::atomic<unsigned int> _head;   // only the consumer writes
    std::atomic<unsigned int> _tail;   // only the producer writes
};

template <typename T, typename C, unsigned int CAP = 16> class Ordered_List
{
    static_assert(std::atomic<T *>::is_always_lock_free,
                  "std::atomic<T*> is not lock-free on this platform");

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
        // Advances to the next occupied slot.  This is what makes the tombstone
        // invisible to whoever is traversing.
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

    // Main thread.  false = full (CAP live observers).
    bool insert(T * e)
    {
        const unsigned int n = _size.load(std::memory_order_acquire);

        for (unsigned int i = 0; i < n; i++)               // reuse a free slot
            if (!_slot[i].load(std::memory_order_relaxed)) {
                _slot[i].store(e, std::memory_order_release);
                return true;
            }

        if (n >= CAP)
            return false;

        _slot[n].store(e, std::memory_order_release);      // slot FIRST
        _size.store(n + 1, std::memory_order_release);     // size AFTER
        return true;
    }

    // Main thread.  From the moment it returns, notify() no longer reaches `e`.
    void remove(T * e)
    {
        const unsigned int n = _size.load(std::memory_order_acquire);
        for (unsigned int i = 0; i < n; i++)
            if (_slot[i].load(std::memory_order_relaxed) == e)
                _slot[i].store(0, std::memory_order_release);
    }

    // Counts LIVE observers (ignores tombstones).
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
    std::atomic<unsigned int> _size;   // high-water mark: only grows
};

#endif // LIBVCOMM_LIST_H
