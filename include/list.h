#ifndef LIBVCOMM_LIST_H
#define LIBVCOMM_LIST_H

#include <atomic>
#include <cstddef>

// =============================================================================
// List / Ordered_List — support classes.  ALREADY IMPLEMENTED.
//
// REWRITTEN on 2026-08-23.  The first version used std::deque/std::vector with
// std::mutex.  That stopped being legal once reception started happening inside
// a signal handler:
//
//   - pthread_mutex_lock is NOT on the list of async-signal-safe functions
//     (man 7 signal-safety).  Locking a mutex from inside a handler that
//     interrupted the very thread already holding it is immediate deadlock.
//   - deque::push_back and vector::push_back may call malloc, which is not on
//     the list either.
//
// What IS allowed inside a handler, and is the foundation of these two classes:
// LOCK-FREE std::atomic objects.  No mutex, no syscall, no allocation.
// (C++17 [support.signal]; the static_asserts below make the compiler prove
// it.)
//
// This is the same decision EPOS makes with intrusive lists, and for the same
// reason: in EPOS the top of the reception path runs in hardware interrupt
// context; here it runs in signal context.  The restriction is identical.
//
// The price paid, and you need to be able to defend it: FIXED capacity.
// Without allocation there is no growing.  Both classes say "no" when they
// fill up instead of allocating.
// =============================================================================

static_assert(std::atomic<unsigned int>::is_always_lock_free,
              "std::atomic<unsigned> is not lock-free on this platform: the "
              "standard library would use a mutex underneath and the handler "
              "would break");

// -----------------------------------------------------------------------------
// List<T, CAP> — FIFO queue of pointers, one producer and one consumer (SPSC).
//
// Used by Concurrent_Observer to hold what arrived and has not been consumed
// yet.  And its two sides are exactly the project's two contexts:
//
//     PRODUCER = the signal handler          -> insert()
//     CONSUMER = the application thread      -> remove()
//
// Fixed-size ring with two atomic indices.  Correct without a lock because each
// index has a single writer: the producer only writes _tail, the consumer only
// writes _head.  The release/acquire pair on those indices is what publishes
// the slot write to the other side — without it, the consumer could see the new
// index and the old pointer.
//
// CAP must be a power of 2 (the wraparound is an AND, not a modulo — division
// on an interrupt path is waste).  One slot is always left empty to tell "full"
// from "empty", so the usable capacity is CAP-1.
// -----------------------------------------------------------------------------
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

    // Called from the handler.  Returns false if the queue is full.
    //
    // >>> WARNING: this bool is new, and ignoring it means SILENTLY LOSING
    // >>> MESSAGES.  Concurrent_Observer::update() ignores it today.  When the
    // >>> queue fills up, the message that arrived has nowhere to go — and that
    // >>> is exactly the Ethernet::Statistics::rx_dropped counter.  Decide what
    // >>> to do and write it in doc/design-decisions.md; "I didn't think about
    // >>> it" is the worst possible answer.
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

// -----------------------------------------------------------------------------
// Ordered_List<T, C, CAP> — collection of observers, each with a rank of type
// C.  The name comes from the PDF (Observed::Observers).
//
// The asymmetry that defines the design: attach()/detach() run on the main
// thread, when objects are constructed and destroyed; notify() TRAVERSES from
// inside the handler.  In other words, lots of reading in signal context and
// very little writing outside it.
//
// Fixed-size vector of atomic slots.  detach() compacts nothing: it writes 0
// into the slot (a tombstone).  Compacting would shift the indices underneath a
// traversal that may be happening right now inside the handler.  A later
// insert() reuses the empty slot before growing.
//
// The Iterator SKIPS empty slots on its own, so that the notify() loop stays
// identical to the one printed in the assignment:
//
//     for(Observers::Iterator obs = _observers.begin(); obs != _observers.end();
//     obs++)
//         if(obs->rank() == c) ...
// -----------------------------------------------------------------------------
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
