#ifndef LIBVCOMM_OBSERVER_H
#define LIBVCOMM_OBSERVER_H

#include "list.h"
#include "sem.h"

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

template <typename T, typename C = void> class Conditionally_Data_Observed
{
public:
    typedef Conditional_Data_Observer<T, C> Observer;
    typedef Ordered_List<Observer, C> Observers;

    Conditionally_Data_Observed() {}
    virtual ~Conditionally_Data_Observed() {}

    void attach(Observer * o, const C & c);

    void detach(Observer * o, const C & c);

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
void Conditionally_Data_Observed<T, C>::detach(Observer * o, const C &)
{
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
// TRANSCRIBED from full_assignment.pdf (blue block, "Fundamentals for Observer
// X Observed").  The body of these methods is the assignment's, not mine — it
// is here so you have a compiling baseline, and it is worth reading line by
// line because it comes up at the panel.
//
// ONE NECESSARY CORRECTION: the PDF calls obs->rank() inside notify(), but the
// PDF's Concurrent_Observer has neither a _rank field nor a constructor that
// takes one.  As printed, it does not compile.  We inherit _rank from the same
// idea as Conditional_Data_Observer.  Note it in doc/design-decisions.md —
// showing that you found the inconsistency beats pretending it isn't there.
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

    // Returns false when NOBODY TOOK the data — either no observer has this
    // rank, or the one that has it could not accept (queue full).  The caller
    // still owns `d` in that case and must release it.
    //
    // Note the ownership rule this implies: at most ONE observer per rank may
    // accept, otherwise two of them would end up freeing the same buffer.  Each
    // process in this library binds one Communicator per port, so the rule
    // holds by construction.
    bool notify(const C & c, D * d)
    {
        bool notified = false;
        for (typename Observers::Iterator obs = _observers.begin();
             obs != _observers.end(); obs++) {
            if (obs->rank() == c) {
                if (obs->update(c, d))
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

    // Called INSIDE THE SIGNAL HANDLER.  Note that it does not block: it
    // enqueues and wakes.  It is this insert()/v() pair that replaces the
    // forbidden rendezvous — and both halves are async-signal-safe, which is
    // what makes this line legal here.
    //
    // RETURNS FALSE WHEN THE QUEUE IS FULL, and that return is load-bearing
    // (decision 1.11 in doc/design-decisions.md; the PDF's update() is void).
    // Ignoring it used to cost twice over: the message was lost AND the buffer
    // was never freed, because Observed::notify() reported success and nobody
    // upstream released it.  Now a full queue is indistinguishable from "no
    // observer wanted it" — Protocol::update() frees the buffer and
    // NIC::handle() counts it in rx_dropped.
    //
    // The v() happens only after a successful insert, so updated() can never
    // wake on an empty queue.
    virtual bool update(const C & c, D * d)
    {
        (void)c;
        if (!_data.insert(d))
            return false;
        _semaphore.v();
        return true;
    }

    // Called ON THE APPLICATION THREAD.  Blocks until there is data.
    D * updated()
    {
        _semaphore.p();
        return _data.remove();
    }

    // Same, with a ceiling.  Returns 0 if nothing arrived within timeout_ms.
    //
    // Why it exists: receive() is the only blocking point in the library, and
    // an automated test needs a receiver that gives up and REPORTS instead of
    // one that hangs until the fleet timeout kills the VM with no verdict.
    // doc/design-decisions.md §2.1 answers the guide's question 3 for the
    // Engine; this answers it for the application.
    D * updated(unsigned int timeout_ms)
    {
        if (!_semaphore.p(timeout_ms))
            return 0;
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
