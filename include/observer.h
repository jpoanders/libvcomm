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
void Conditionally_Data_Observed<T, C>::detach(Observer * o, const C & c)
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

    // Called INSIDE THE SIGNAL HANDLER.  Note that it does not block: it
    // enqueues and wakes.  It is this insert()/v() pair that replaces the
    // forbidden rendezvous — and both halves are async-signal-safe, which is
    // what makes this line legal here.
    //
    // >>> _data.insert() has returned a bool since 2026-08-23 and this line
    // >>> IGNORES it.  Queue full = message silently dropped.  See the note in
    // >>> list.h.
    virtual void update(const C & c, D * d)
    {
        (void)c;
        _data.insert(d);
        _semaphore.v();
    }

    // Called ON THE APPLICATION THREAD.  Blocks until there is data.
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
