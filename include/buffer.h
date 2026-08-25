#ifndef LIBVCOMM_BUFFER_H
#define LIBVCOMM_BUFFER_H

#include <atomic>


template <typename T> class Buffer
{
public:
    typedef T Data;

    Buffer() : _size(0), _in_use(false) {}

    T * frame() { return &_frame; }
    const T * frame() const { return &_frame; }

    unsigned int size() const { return _size; }
    void size(unsigned int s) { _size = s; }

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
