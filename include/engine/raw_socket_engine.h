#ifndef LIBVCOMM_RAW_SOCKET_ENGINE_H
#define LIBVCOMM_RAW_SOCKET_ENGINE_H

#include <csignal>
#include "../ethernet.h"

// =============================================================================
// Raw_Socket_Engine — STAGE 1's Engine.
//
// THIS CLASS IS THE ONLY PLACE IN THE LIBRARY ALLOWED TO MAKE NETWORK
// SYSCALLS.
//
// That is the whole contract.  NIC, Protocol and Communicator do not know that
// sockets, sockaddr_ll or eth0 exist.  When Stage 2 asks for communication
// between processes on the SAME VM, you write Shared_Memory_Engine with these
// same methods and NIC<Shared_Memory_Engine> works without changing a single
// line of the NIC.  If any syscall leaks out of here, that promise breaks — and
// it is the first thing Fröhlich will look for.
//
//                 NIC<Raw_Socket_Engine>          NIC<Shared_Memory_Engine>
//                          |                                |
//                    raw socket / eth0                 shm_open / mmap
//                       (Stage 1)                         (Stage 2)
//
// -----------------------------------------------------------------------------
// RECEPTION THROUGH A POSIX SIGNAL  (revised 2026-08-23)
// -----------------------------------------------------------------------------
// Reception does NOT use a thread.  The assignment is explicit:
//
//   "packet reception events from the OS kernel must be immediately propagated
//    to the upper layers of the protocol stack.  That propagation may happen
//    either through the implementation of protocol-specific kernel modules or
//    through POSIX SIGNALS."
//
// And there is a design reason behind it, not just a rule: in EPOS, handle() is
// called from the NIC's HARDWARE INTERRUPT handler.  The faithful POSIX
// analogue of an interrupt is a signal.  Keeping that preserves EPOS's
// structure — and explains the rest of the design:
//
//   * Conditional_Data_Observer::update must not block -> it runs in interrupt
//     context;
//   * Concurrent_Observer uses a semaphore              -> sem_post(3) is one
//     of the few async-signal-safe functions (man 7 signal-safety);
//   * the Buffer pool is preallocated                   -> malloc is not.
//
// The mechanism, in two fcntl calls:
//
//   fcntl(fd, F_SETOWN, getpid())            who receives the signal
//   fcntl(fd, F_SETFL, ... | O_ASYNC | O_NONBLOCK)
//
// The signal is SIGIO, O_ASYNC's default — no F_SETSIG.  The known objection is
// that standard signals do NOT queue: two frames in a burst generate a single
// signal.  It does not matter here, because the delivery guarantee is the drain
// LOOP, not the signal count — a single SIGIO makes drain() empty the whole
// queue.  A real-time signal (SIGRTMIN+n, with F_SETSIG and SA_SIGINFO) would
// only start to be worthwhile if there were more than one descriptor to tell
// apart by si_fd.  See doc/design-decisions.md.
//
// Why O_NONBLOCK: the handler must DRAIN — call recvfrom in a loop until EAGAIN
// — because several frames may have arrived before it ran.  Without O_NONBLOCK
// the loop would block on the last iteration and you would have a process
// sleeping inside a signal handler.
// =============================================================================

class Raw_Socket_Engine
{
protected:
    typedef Ethernet::Address Address;
    typedef Ethernet::Protocol Protocol;

    // Opens the raw socket on `iface`, filtering by `prot`.
    //
    // Contract: after construction, engine_valid() tells whether it worked.
    // The constructor does NOT throw and does NOT call exit() — the caller
    // decides what to do with the failure.
    //
    // It does NOT arm reception.  Reason: handle() is virtual and the derived
    // class (NIC) has not finished constructing at this point.  Worse than
    // before, in fact: with a signal, a frame arriving here would call a pure
    // virtual on a half-constructed object.  engine_start() is called by the
    // NIC's constructor, at the end.
    Raw_Socket_Engine(const char * iface, Protocol prot);

    virtual ~Raw_Socket_Engine();

    Raw_Socket_Engine(const Raw_Socket_Engine &) = delete;
    Raw_Socket_Engine & operator=(const Raw_Socket_Engine &) = delete;

    // -------------------------------------------------------------------------
    // Sending.  `frame` arrives fully built (dst, src, EtherType and payload)
    // and `size` is the total in bytes, header included.
    //
    // Contract: returns the number of bytes handed to the kernel, or -1 with
    // errno preserved.  It does not write to stdout — logging is the upper
    // layer's job.
    //
    // Runs on the application thread, outside the handler.  Except that it can
    // be INTERRUPTED by one: a frame may arrive in the middle of your sendto.
    // Anything engine_send and the handler share must tolerate that.
    // -------------------------------------------------------------------------
    int engine_send(const Ethernet::Frame * frame, unsigned int size);

    // -------------------------------------------------------------------------
    // Arms / disarms signal-driven reception.
    //
    // engine_start(): from the moment it returns true, handle() may be called
    // at ANY INSTRUCTION, on whichever thread is running.  Everything handle()
    // touches must be ready before this call.
    //
    // engine_stop(): when it returns, no new signal will be generated by this
    // socket and handle() will never be called again.  Idempotent.
    //
    // >>> Question 3 of the guide — "how will a blocked receiver terminate
    // >>> cleanly during automated tests?" — disappears here.  There is no
    // >>> blocked receiver to wake up: disarming is removing O_ASYNC with one
    // >>> fcntl.  Say that in the presentation; it is a better answer than any
    // >>> of the three the thread model would produce.
    // -------------------------------------------------------------------------
    bool engine_start();
    void engine_stop();

    // The interface's real MAC, read from the kernel at construction.  No
    // hard-coded MACs: run-vm.sh generates 02:00:00:00:00:<id> per VM, and the
    // frame's source has to be the real address.
    const Address & engine_address() const { return _address; }

    bool engine_valid() const { return _sockfd >= 0; }

    // -------------------------------------------------------------------------
    // RX path diagnostics.  drain() cannot print — it is inside the handler —
    // so it records here and the main loop reads it.
    //
    // The counters are MONOTONIC: the Engine never resets them.  The reader
    // keeps the last value it saw and reports the difference.  The alternative
    // (an accessor that reads and clears) has a real window: the handler may
    // increment between the read and the write of the zero, and that count
    // vanishes.
    //
    // Read the COUNTER first and errno afterwards — drain() writes them in the
    // opposite order, so a counter that went up always has an errno at least as
    // new as itself.
    // -------------------------------------------------------------------------
    unsigned int engine_rx_errors() const
    {
        return static_cast<unsigned int>(_rx_errors);
    }
    int engine_rx_error() const { return static_cast<int>(_rx_error); }

    // -------------------------------------------------------------------------
    // Upward callback.  The NIC implements it.  Called once per frame that
    // survived filtering.
    //
    // >>> RUNS INSIDE THE SIGNAL HANDLER.  This line is the most important one
    // >>> in the file, and it holds for EVERYTHING handle() reaches —
    // >>> NIC::handle, alloc, notify, Protocol::update, Communicator::update.
    // >>> That whole chain is subject to async-signal-safety
    // >>> (man 7 signal-safety):
    // >>>
    // >>>   ALLOWED:     recvfrom, write, sem_post, lock-free atomics,
    // >>>                memcpy/memset, arithmetic
    // >>>   NOT ALLOWED: printf and any stdio, malloc/new, std::mutex,
    // >>>                std::string, std::cout, exceptions
    // >>>
    // >>> printf is the one that will get you: it is the first thing you want
    // >>> to do when a message arrives.  Use write(2) — it is on the list — or
    // >>> stash the data and print it from the main loop.
    //
    // Contract: `frame` points into the Engine's memory, valid ONLY during the
    // call.  Whoever wants to keep it, copies it.  Return fast: while handle()
    // runs, the interrupted thread is stopped.
    // -------------------------------------------------------------------------
    virtual void handle(Ethernet::Frame * frame, unsigned int size) = 0;

private:
    // Trampoline: a signal handler is a free function, it has no `this`.  The
    // bridge back to the object is a static pointer.
    //
    // CONSEQUENCE TO DOCUMENT IN doc/design-decisions.md: ONE Engine PER
    // PROCESS.  Signal disposition is process-global state — two Engines in the
    // same process would fight over the same handler.  For Stage 1 this does
    // not hurt (one process = one vehicle = one NIC); in Stage 2, when a
    // vehicle becomes several processes, it is the first assumption to revisit.
    static void signal_handler(int signo);
    static Raw_Socket_Engine * _instance;

    // The drain loop: recvfrom until EAGAIN, filtering, calling handle().
    // Split out of the trampoline so the handler stays one line long.
    void drain();

    // -------------------------------------------------------------------------
    // State.  Already declared — you fill it in the constructor.
    // -------------------------------------------------------------------------
    int _sockfd;                  // -1 while invalid
    unsigned int _ifindex;        // if_nametoindex("eth0")
    Address _address;             // eth0's real MAC (SIOCGIFHWADDR)
    Protocol _protocol;           // EtherType, in HOST order
    volatile sig_atomic_t _armed; // has engine_start() run?
    volatile sig_atomic_t _rx_error;
    volatile sig_atomic_t _rx_errors;

    // Note: _armed is sig_atomic_t, not bool nor std::atomic<bool>.  It is the
    // only type the standard guarantees is safe to share between a handler and
    // the interrupted code.  (A lock-free std::atomic works too; sig_atomic_t
    // is what the POSIX standard names, and it costs nothing.)
};

#endif // LIBVCOMM_RAW_SOCKET_ENGINE_H
