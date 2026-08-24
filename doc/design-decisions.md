# Design decisions — libvcomm, Stage 1

Group M10 — INE5424 2026/2 — UFSC
João Pedro de Oliveira Anders · Artur Tribeck Ferreira Tomaz · André Filipe Martins

This document exists for the evaluation panel. Every time the implementation
departs from what is written in `full_assignment.pdf`, the deviation shows up
here with its reason. Showing that a deviation was deliberate is worth more than
an implementation that follows the PDF to the letter and does not compile.

---

## 1. Deviations from the assignment's API

### 1.1 `Traits<NIC>` and `Traits<Protocol>` → `Traits<Ethernet>`

The PDF uses `Traits<NIC>::SEND_BUFFERS` and
`Traits<Protocol>::ETHERNET_PROTOCOL_NUMBER`. `NIC` and `Protocol` are class
templates; a template is not a type, so `Traits<NIC>` does not compile. EPOS
solves this with non-template bases (`NIC_Common`, `Protocol_Common`) and
specializes `Traits` over those.

**Decision:** anchor the specialization on `Ethernet`, which is already a
concrete class and already a base of `NIC`. Same effect, a single configuration
point.

### 1.2 `Protocol: private typename NIC::Observer`

`typename` cannot appear in a base list — in a *base-clause* the name is already
read as a type. **Decision:** removed.

### 1.3 `Protocol::Observer` is `Concurrent_Observer`, not `Conditional_Data_Observer`

The PDF declares, inside `Protocol`:

```cpp
typedef Conditional_Data_Observer<Buffer<Ethernet::Frame>, Port> Observer;
```

but the `Communicator` in the same PDF inherits from `Concurrent_Observer` and
registers itself with `_channel->attach(this, address)`. The two do not fit: the
observer the Protocol accepts would have to be from the Conditional family, and
the Communicator is from the Concurrent one.

**Decision:** the `Communicator` wins. `Protocol::Observer` is
`Concurrent_Observer<Buffer, Port>` and `Protocol::_observed` is
`Concurrent_Observed<Buffer, Port>`.

And this is what the architecture asks for, not just what the compiler accepts:

| Boundary | Family | Why |
|---|---|---|
| `NIC` → `Protocol` | `Conditionally_Data_Observed` | runs inside the signal handler; **must not block** |
| `Protocol` → `Communicator` | `Concurrent_Observed` | on the other side there is an application thread asleep; **needs the semaphore** (and `sem_post` is async-signal-safe) |

### 1.4 `Concurrent_Observer` has no `rank`

The PDF's `Concurrent_Observed::notify()` calls `obs->rank()`, but the
`Concurrent_Observer` as printed has neither a `_rank` field nor a constructor
that takes one. As it stands, it does not compile.

**Decision:** `Concurrent_Observer` gained `_rank` and the `rank()`/`rank(C)`
pair; `attach()` fixes the observer's rank. The rest of the methods are a
literal transcription.

### 1.5 `update()` with two arguments, not three

The PDF also passes a pointer to the observed
(`update(NIC::Observed * obs, prot, buf)`). That is only useful when one
observer follows several observed objects — which is not the case in any layer
of this library.

**Decision:** `update(condition, data)` in every layer. A single signature, used
identically in `NIC`→`Protocol` and `Protocol`→`Communicator`.

### 1.6 `Protocol`'s `send()`/`receive()` are not static

The PDF declares them `static` but the body uses `_nic`, which is an instance
member, and the `Communicator` calls them as `_channel->send(...)`.
**Decision:** instance methods.

### 1.7 `static Observed _observed` → instance member

The PDF's comment says "channel protocols are usually singletons". A static
member of a template requires an out-of-class definition and prevents two
protocols in the same process. **Decision:** an instance member. Singularity, if
desired, is the responsibility of whoever builds the stack.

### 1.8 `NIC()` is public

The PDF marks the constructor `protected` (in EPOS, the instance comes from
`Meta`/`Traits`). Without a factory, `protected` makes the class unusable.
**Decision:** public.

### 1.9 `Channel::Address::BROADCAST` → `Address::broadcast()`

A static member of a nested class inside a template requires an out-of-class
definition. A static function gives the same result without the ceremony.

### 1.10 `send(Buffer *)` releases the buffer unconditionally

The assignment does not specify who releases the buffer after `send(Buffer *)`.
The natural alternative would be: release on success, hand it back to the caller
on error.

**Decision:** `send(Buffer *)` calls `free(buf)` on **every** path, success and
failure. After calling `send`, the caller **must not** touch the buffer.

**Why.** The pool has a fixed capacity (`SEND_BUFFERS + RECEIVE_BUFFERS`). If
releasing on error is the caller's responsibility, a single forgotten `free` on
an error path locks a slot permanently. After 32 failed sends without a `free`,
the pool is exhausted and `alloc()` returns `0` forever. That bug only shows up
under load, typically during the demo.

With unconditional release:

- **A single ownership rule:** `send` is a transfer of ownership. No conditional
  logic for the caller.
- **Leaks are impossible:** the buffer returns to the pool on any path.
- **Consistency with `send(Address, prot, data, size)`:** the simple path
  already calls `alloc` + `send(buf)` internally, so the lifecycle is
  self-contained.

**Consequence:** the caller cannot retry with the same buffer. If `send` fails
and a retry is needed, a fresh `alloc` and a rebuilt frame are required.

---

## 2. Our own decisions

### 2.1 Reception through a POSIX signal, not a thread

**Decision:** frame reception happens inside a *signal handler*, armed with
`fcntl(F_SETOWN)` + `O_ASYNC | O_NONBLOCK` and handled by a `sigaction` for
`SIGIO`. There is no reception thread.

**Why.** It is what the assignment mandates:

> "packet reception events from the OS kernel must be immediately propagated to
> the upper layers of the protocol stack. That propagation may happen either
> through the implementation of protocol-specific kernel modules or through
> **POSIX signals**."

A thread blocked in `recvfrom` is neither of the two named mechanisms.

And there is a design reason behind the rule: in EPOS, `handle()` is called from
the NIC's **hardware interrupt handler**. The faithful POSIX analogue of an
interrupt is a signal. Keeping that preserves the structure Stage 2 will reuse —
and it explains the rest of the design, which would otherwise look arbitrary:
`Conditional_Data_Observer::update` must not block because it runs in interrupt
context; `Concurrent_Observer` uses a semaphore because `sem_post(3)` is
async-signal-safe; the `Buffer` pool is preallocated because `malloc` is not.

**`SIGIO`, not a real-time signal.** We considered `fcntl(F_SETSIG, SIGRTMIN)`
and discarded it. The argument in favour of real-time signals is that they
queue, while standard ones coalesce — two frames in a burst may generate a
single signal. But that **costs no frames** here, because the real guarantee is
not the signal but the drain loop (below): when the handler runs, it collects
everything sitting in the kernel's buffer.

**Measured, not argued** (2026-08-24, `make test-engine` with `cap_net_raw+ep`,
interface `lo`): with `SIGIO` blocked by `sigprocmask()`, 50 frames sent in a
burst generated **one** pending signal, and `drain()` delivered all **50** to
`handle()` from that single entry into the handler. That is the coalescing
happening — and it is exactly why it costs no frames. The test is
deterministic: there is no `sleep` and no tolerance, because POSIX guarantees
delivery of the pending signal before the unblocking `sigprocmask()` returns.

Against real-time signals there was one more failure mode. If the queue of
queued signals overflows, *"the kernel reverts to delivering `SIGIO`"*
(`man 2 fcntl`, `F_SETSIG`) — and `SIGIO`'s default action on Linux is to
**terminate the process** (`man 7 signal`). Using RT safely would require
handling `SIGIO` **anyway**, as a fallback. Going straight to `SIGIO` means that
path does not exist.

**What is lost:** `si_fd`. Without a non-zero `F_SETSIG` the kernel does not say
which descriptor generated the event. In Stage 1 there is a single socket; the
information would be ignored. If Stage 2 brings more than one descriptor per
Engine, this is the decision to revisit.

**`O_NONBLOCK` and looped draining — the mechanism that holds up the rest.** The
handler calls `recvfrom` repeatedly until `EAGAIN`. The signal is the trigger;
the loop is the guarantee, and it is what makes the coalescing harmless. Leaving
after the first frame would strand the rest in the kernel's buffer until the
next arrival — and the measured latency would become fiction.

**Three exit arms, not two.** `EAGAIN`/`EWOULDBLOCK` is a normal end and exits
quietly; `EINTR` continues; any other `errno` is recorded before exiting. The
recording cannot print — `drain()` runs inside the handler — so it goes into
monotonic `volatile sig_atomic_t` counters that the main loop reads
(`engine_rx_errors()` / `engine_rx_error()`). Without that third arm, an
interface going down manifests as reception simply falling silent, without a
trace.

*Measured* (2026-08-24, `sudo scripts/test-engine-veth.sh`): with the Engine
armed over a `veth` pair, an `ip link set vcomm0 down` made `packet_notifier()`
set `sk_err = ENETDOWN` and fire the `SIGIO`; `drain()`'s `recvfrom()` consumed
the error and the counter recorded **`errno` 100, `ENETDOWN`**. `NETDEV_DOWN`
was enough — escalating to device removal was not necessary.

**Documented trade-off:** signal disposition is process-global state, therefore
**one Engine per process**. In Stage 1 this does not hurt (one process = one
vehicle = one NIC). When a vehicle becomes several processes, it is the first
assumption to revisit.

**Side benefit:** question 3 of the practical class guide ("how will a blocked
receiver terminate cleanly during automated tests?") ceases to exist. There is
no blocked receiver to wake up — disarming is removing `O_ASYNC` with an
`fcntl`.

> **Honesty note:** `practical_class_1_guide.md` §6 allows
> *"receive asynchronously **or in a dedicated receive thread**"*, contradicting
> the assignment. We follow the assignment, which is the artefact being graded.

### 2.2 EtherType `0x88B5`

*IEEE Local Experimental EtherType 1*, a range reserved for local and
experimental use (RFC 5342 §2.3.4; IANA registry "IEEE 802 Numbers"). Chosen
because it is the formally correct range for a coursework protocol, rather than
some arbitrary free value.

The value is passed to `socket()` as the kernel's filter. That matters in
practice: with an idle VM, **8 of 11 frames** captured on the bus were IPv6 from
the guest's own kernel (MLD, router solicitation — EtherType `0x86DD`,
destination `33:33:00:00:00:16`). The kernel filter eliminates that noise before
the copy into user space; a receiver filtered by `htons(0x88B5)` saw **only**
the project's frames.

> **Pending:** confirm `0x88B5` with Artur and André before the presentation.

### 2.3 Lock-free, allocation-free lists

EPOS uses intrusive lists so as not to allocate memory on the reception path,
which there runs in interrupt context. **By decision 2.1, exactly the same
restriction applies here** — the reception path runs in signal context.

The first version used `std::deque`/`std::vector` with `std::mutex`, resting on
the fact that the assignment allows the C++ Standard Library. That **stopped
being legal**: `pthread_mutex_lock` is not on the list of async-signal-safe
functions (`man 7 signal-safety`), and locking from inside a handler that
interrupted the very thread already holding the mutex is immediate deadlock;
`push_back` may call `malloc`, which is not on the list either.

**Decision:** lock-free, fixed-capacity, allocation-free structures.

| | |
|---|---|
| `List<T,CAP>` | an SPSC ring with two `std::atomic` indices — the handler only writes `_tail`, the application only writes `_head` |
| `Ordered_List<T,C,CAP>` | a vector of `std::atomic<T*>`; `detach()` writes a *tombstone* instead of compacting, because compacting would shift indices underneath a traversal in progress |

`static_assert(is_always_lock_free)` on both: if `std::atomic` were not
lock-free on the platform, the standard library would use a mutex underneath and
the problem would come back silently.

**Price:** fixed capacity. `insert()` returns `false` when it fills up, and that
is `Statistics::rx_dropped` — not an error to hide.

Verified empirically: a producer in a signal handler at 20 kHz, a consumer in
`main`, 3 s — **58,937 signals, 176,811 items, zero lost, FIFO preserved.**

### 2.4 Our own multicast bus

`run-vm.sh`'s default is `230.0.0.1:1234`, shared by the whole class.
**Decision:** `SO2_MCAST=239.10.10.10:15424` (an administratively scoped range,
port = the course number).

> **Warning, verified on 2026-08-22:** `ip route get 239.10.10.10` answers
> `dev wlp0s20f3`. The bus goes out over **WiFi**, onto the building's network —
> it may collide with another group and it takes the project's traffic off the
> machine. Pin it to loopback with `sudo ip route add 239.10.10.0/24 dev lo`
> before any measurement, and re-check the route after switching networks.

### 2.5 The instructor's starter is versioned inside the repository

The assignment requires a root Makefile capable of driving the compilation and
execution of every evaluation test. That is a statement about **the delivered
commit**, not about the machine of whoever wrote it: if `make image` points at
`$HOME/work/so2/...`, the clone the instructor evaluates dies at the first
command, and the requirement was not met — it only looked met.

**Decision:** the `INE5424-x86_64-starter-6.15.5.tar.gz` bundle (15 MB) lives in
`vendor/`, versioned, with the `.sha256` that came with it. The assignment is in
`doc/full_assignment.pdf` and the practical guide in
`doc/practical_class_1_guide.md`. No Makefile target references a path outside
the root.

What that costs: 15 MB permanently in the git history. The alternative —
downloading the bundle during `make` — trades a path dependency for a network
dependency, which is worse at exactly the moment it would fail: the
presentation.

**A side effect that removed a real risk.** While the starter was an external
directory, the rule "do not run `repack-initramfs.sh` in it, because it writes
into the tree it lives in" was human discipline, and human discipline fails at
two in the morning. Now the rule is mechanical: the `starter` target unpacks the
tarball into `build/vm/`, and that is the only place `install-app.sh` runs. The
tarball is read-only by virtue of there being no command that writes to it, and
`make clean-vm` restores the image to factory state.

*Verified* (2026-08-24): the copy unpacked by `make starter` matches
byte-for-byte against the bundle's own `SHA256SUMS` — 13 of 13 files.

---

## 3. Honest limitations of the bench

- **One Engine per process** (a consequence of decision 2.1). Signal disposition
  is process-global, so two Engines in the same process would fight over the
  same handler. Does not affect Stage 1.
- **Fixed-capacity queues** (decision 2.3). Under a burst above capacity the
  message is dropped and counted in `rx_dropped`, rather than the library
  growing the queue by allocating memory in signal context.

- **No `/dev/kvm`.** The VMs run under TCG (pure emulation). Every measured
  latency is dominated by emulation noise. The number goes on the slide **with
  that caveat**.
- **QEMU's `virtio-net` does not pad to 60 bytes.** Measured: a 20-byte frame
  reaches the other guest's `recvfrom()` as 20. In other words,
  `payload_size = bytes_received - 14` works *on this bench* and breaks on real
  hardware and on Stage 2's shared-memory Engine. That is why the true size must
  travel in a `Protocol::Header` field.
- **A capture on the host does not prove reception.** Seeing the datagram leave
  proves format and timing; the proof that a guest processed the message is the
  VM's log.

---

## 4. Open items

- [ ] Diagrams in `doc/` (fleet topology; layers; the sequence of a message from
      `send()` until the application thread wakes up, marking where the signal
      context ends). The assignment requires them.
- [ ] Stage 1 slides in `doc/`.
- [ ] Confirm the EtherType with the group.
- [ ] Decide and record: does each component open its own raw socket, or does a
      NIC-owning process relay to the children? (With one Engine per process,
      the first option is the one that comes for free.)
- [ ] Decide and record the queue-full policy: today `Concurrent_Observer::
      update()` ignores `insert()`'s `bool`, i.e. it drops silently.
