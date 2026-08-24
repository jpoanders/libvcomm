#ifndef LIBVCOMM_BUFFER_H
#define LIBVCOMM_BUFFER_H

#include <atomic>

// =============================================================================
// Buffer<T> — support class.  ALREADY IMPLEMENTED.
//
// This is the object that travels up the stack: the Engine writes into it, the
// NIC hands it to the Protocol, the Protocol to the Communicator.  Nobody
// copies the frame along the way — the pointer is passed instead.  This is the
// "zero-copy" that EPOS pursues.
// =============================================================================
//
// QUESTION 4 OF THE INSTRUCTOR'S GUIDE: "who owns a received buffer, and when
// is it released?"  This library's answer:
//
//   - The pool lives inside the NIC (NIC::_buffer[BUFFER_SIZE]).
//   - alloc() marks a buffer as in use and transfers ownership to the caller.
//   - Ownership travels with the pointer: whoever received the buffer is the
//     one who must call NIC::free() — see Protocol::update(), which releases
//     the buffer when NO observer wanted it.
//   - free() returns the buffer to the pool.  A forgotten free() = a silent
//     leak
//     that only shows up after BUFFER_SIZE messages.  See the pool exhaustion
//     test in tests/.
//
// lock() uses an atomic test-and-set because the pool is contended between the
// reception path (which needs a buffer for the frame that just arrived) and the
// application thread (which needs one to send).

template <typename T> class Buffer
{
public:
    typedef T Data;

    Buffer() : _size(0), _in_use(false) {}

    // Pointer to the area where the frame actually lives.
    T * frame() { return &_frame; }
    const T * frame() const { return &_frame; }

    // How many bytes of this buffer are valid (header + payload).
    unsigned int size() const { return _size; }
    void size(unsigned int s) { _size = s; }

    // Tries to reserve this buffer.  Returns true on success — and, if it
    // succeeded, nobody else can until unlock().  Fails without blocking.
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
