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

    virtual bool update(const C & c, D * d)
    {
        (void)c;
        if (!_data.insert(d))
            return false;
        _semaphore.v();
        return true;
    }

    D * updated()
    {
        _semaphore.p();
        return _data.remove();
    }

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
