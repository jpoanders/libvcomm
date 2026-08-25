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

### 1.11 The broadcast destination carries the **sender's port**

The PDF's `Communicator::send` addresses `Channel::Address::BROADCAST`, an
address with no port in it. Transcribed literally that writes `_to_port = 0`
into every packet, and on the way up `Protocol::update` notifies condition `0` —
which no `Communicator` is ever attached to, because each one binds a real port.

The result was not a subtle degradation. **Every message in the library was
delivered to nobody and silently freed.** Found on 2026-08-24, by writing the
first test that reached this layer at all.

**Decision:** `send()` broadcasts to the Communicator's *own* port —
`Address::broadcast(_address.port())`.

**Why this rather than making port 0 a wildcard.** Both make the message arrive.
The wildcard throws away the multiplexing the `Protocol` layer exists for; this
turns the port into the **channel**: agents that bind the same port hear each
other, and an agent bound elsewhere does not. The destination is still
`ff:ff:ff:ff:ff:ff` at the link layer, so the assignment's requirement is
untouched, and "the Communicator does not use explicit addressing" still holds —
who a message is *for* remains the message's business, not the address's.

**What it costs:** a Communicator cannot address a different port. Nothing in
Stage 1 wants to, and when Stage 5's interest/response messages do, they will
say so in the payload, which is where the assignment puts that information
anyway.

*Regression test:* `tests/test_protocol.cpp` §1 — one Communicator on port 1024
and another on 2048; the message must reach the first and not the second.
Reverting this decision turns 6 of its 23 checks red in about 40 ms.

### 1.12 `Concurrent_Observer::update` returns `bool`

The PDF's `update` is `void`. It cannot be: `List::insert` has a fixed capacity
and can fail (decision 2.3), and `Concurrent_Observed::notify` has to tell the
layer above whether anybody actually took the data.

Ignoring that return cost **twice over**. The message was lost — which was
known — and the buffer was **never released**, which was not: `notify()`
reported success, so `Protocol::update` did not free it, and the pool slot was
gone for the life of the process. After `RECEIVE_BUFFERS` such events reception
stops altogether, permanently, with no error anywhere.

**Decision:** `update` returns whether it accepted the data and posts the
semaphore only on success; `notify` ORs the results. A full queue is now
indistinguishable from "no observer wanted this": `Protocol::update` frees the
buffer and `NIC::handle` counts it in `rx_dropped` — both already written that
way. This closes the queue-full policy that §4 had left open.

A second bug went with it: `updated()` can no longer wake on an empty queue,
because the `v()` no longer happens without a successful `insert()`.

**Consequence to state at the panel:** at most **one** observer per rank may
accept, or two of them would free the same buffer. Every process here binds one
Communicator per port, so the rule holds by construction rather than by
discipline.

### 1.13 Two additions the automated evaluation required

Neither changes an existing signature; both exist because `make` has to be able
to fail honestly.

- **`Communicator::receive(Message *, timeout_ms)`**, over a new
  `Semaphore::p(timeout_ms)` (`sem_timedwait`, absolute deadline, retried on
  `EINTR` — which is the common case here, since `SIGIO` fires on every frame).
  Without it a component that loses its last frame blocks forever, its VM dies
  to the fleet timeout, and it reports **nothing** — and a test that cannot
  report a verdict is not gradeable. §2.1 answers the guide's question 3 for the
  *Engine*; this answers it for the *application*, which is where the blocking
  actually is.

- **`NIC(const char * iface = Traits<Ethernet>::INTERFACE)`.** The default is
  unchanged and is what every delivered path uses. The parameter is what lets
  the same stack be pointed at another interface without a rebuild —
  `tests/test_engine.cpp` already did the equivalent one layer down with
  `VCOMM_TEST_IFACE`.

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

### 2.6 The buffer pool **is** partitioned between TX and RX

> **Status: decided** 2026-08-24, in favour of the first option below. The
> analysis that led there is kept because the alternatives are reasonable and
> the panel may well ask why not them.

Until this decision, `Traits<Ethernet>` declared `SEND_BUFFERS = 16` and `RECEIVE_BUFFERS = 16`, and
`NIC::BUFFER_SIZE` is their sum. But the two names describe nothing that the
code does: `NIC::alloc()` and `NIC::handle()` both scan the **whole** array
`[0, BUFFER_SIZE)` and take the first free slot. The split is documentation, not
behaviour.

**The consequence.** There is no reservation, so either side can starve the
other. A reception burst can consume all 32 slots and make `alloc()` return `0`
to the application; a sender holding buffers can leave `handle()` with nothing
to write into. The second case is the one that costs data: `handle()` runs in
signal context and cannot wait — it increments `rx_dropped` and the frame is
gone. The sender, by contrast, gets a `0` from `alloc()` and can retry.

**The three options, with what each costs:**

| Option | Effect | Cost |
|---|---|---|
| Partition the ranges — `alloc()` over `[0, SEND_BUFFERS)`, `handle()` over `[SEND_BUFFERS, BUFFER_SIZE)` | RX can no longer be starved by TX; matches what `Traits` already claims | Neither side can borrow from an idle other side: 16 slots is a hard ceiling per direction even when 16 sit unused |
| Keep one shared pool, delete `SEND_BUFFERS`/`RECEIVE_BUFFERS`, keep a single `BUFFER_SIZE` | Honest about the current behaviour; best use of the slots under asymmetric load | The starvation above stays possible, and the RX side of it is silent data loss |
| Shared pool with a **reserve floor** — `alloc()` refuses when fewer than *N* slots remain free | RX keeps a guaranteed margin, TX still uses the surplus | One more constant to justify, and `alloc()` gains a scan of the whole pool it does not do today |

**Decision: the first.** `alloc()` scans `[0, SEND_BUFFERS)` and `handle()`
scans `[SEND_BUFFERS, BUFFER_SIZE)`. It is what the two constants in `Traits`
already claimed, it costs one loop bound at each end, and it makes the
guaranteed reception depth a number we can state instead of a race we would have
to explain.

The asymmetry is what settles it: an `alloc()` that returns `0` hands the sender
a failure it can act on, while a `handle()` with no buffer is in signal context,
cannot wait, and loses the frame. The side that cannot recover is the side that
gets a reservation.

The third option — a shared pool with a reserve floor — is strictly better under
asymmetric load and is the right answer the day a measurement shows one
direction idling. Stage 1 does not measure that, and a constant we could not
justify would be worse than the ceiling we can.

*Verified*: `tests/test_stack.cpp` §3 proves `alloc()` yields exactly
`SEND_BUFFERS` and then `0`; `tests/test_protocol.cpp` §3 proves the mirror
case, which is the one that matters — with the whole receive half held hostage
by unread messages, reception drops and counts them in `rx_dropped` while
`alloc()` carries on unaffected.

### 2.7 Each component opens its **own** raw socket

The assignment models every component of a vehicle (sensor, fuser, ECU) as a
process. The open question was whether each of those processes runs its own
stack, or whether one NIC-owning process receives and relays to its siblings.

**Decision:** each component builds its own `NIC` / `Protocol` / `Communicator`,
and therefore opens its own `AF_PACKET` socket. The vehicle's root process owns
no socket at all: it forks first and the children build their stacks afterwards.

**Why.** Decision 2.1 already forced it. Signal disposition is process-global, so
there is one Engine per process; a relaying design would need shared memory
between the components — which is *literally Stage 2's assignment*, and building
it now would mean building it twice.

**The order matters and is not a style choice.** `Raw_Socket_Engine` arms the
socket with `fcntl(F_SETOWN, getpid())` and keeps a static `_instance`. A child
forked from a process that already had a live Engine inherits a socket
signalling the **parent's** pid and a stale `_instance`. Fork first, construct
after.

**What it costs, measured on the 5×3 fleet:** fifteen raw sockets, and every
component receives a copy of every frame on the bus — `rx_packets` is the same
537 on all three components of a vehicle. That is the cost Stage 2's
shared-memory Engine exists to remove, and it is worth showing the number.

**One thing it does *not* buy:** components of the same vehicle cannot hear each
other through this Engine. Their frames leave through the same device and reach
sibling sockets marked `PACKET_OUTGOING`, which `drain()` filters. Intra-vehicle
communication is Stage 2's, exactly as the assignment arranges it.

**And the identity consequence:** the three components of a vehicle share
`eth0`'s MAC, so the source address cannot tell them apart. Anything that needs
to distinguish them — including the fleet test's own self-echo filter — has to
read the identity out of the payload. Which is precisely why Stage 2 adds
`origin` to the message.

### 2.8 The bus is pinned to loopback by QEMU, not by a route

`run-vm.sh` as shipped lets the kernel route the multicast bus, and on this
machine `ip route get 239.10.10.10` answers `dev wlp0s20f3` — **WiFi**. The
project's traffic would go onto the building's network, could collide with
another group's bus, and would never appear in a loopback capture. The
previously documented fix was `sudo ip route add 239.10.10.0/24 dev lo`, which
needs root and disappears on reboot.

**Decision:** pass QEMU `localaddr=127.0.0.1` on the socket netdev instead.
`scripts/run-fleet.sh` patches the working copy of the launcher, once and
idempotently, keeping a `.orig` beside it; `make clean-vm` throws the whole copy
away.

**Why it is better than the route:** no privileges, nothing to undo, nothing to
re-apply after switching networks. QEMU turns the option into
`IP_MULTICAST_IF` plus an `IP_ADD_MEMBERSHIP` on loopback, so both directions are
pinned.

*Verified* (2026-08-24): a multicast datagram sent with `IP_MULTICAST_IF` set to
`127.0.0.1` is received by a second socket that joined the group on the same
interface, and `dumpcap -i lo` captures it — exactly one copy, which is why the
capture needs no de-duplication on this bench even though the analysis script
does it anyway.

The `sudo ip route` form is kept as `make bus-local`, for a QEMU too old for
`localaddr`.

### 2.9 The bus is recorded by joining it, not by sniffing it

The assignment requires that `make` alone run the whole evaluation and print the
average latency at the end. The obvious recorder is `dumpcap`/`tshark`, and that
is what this project used first.

**The problem that makes it unacceptable.** On Debian and Ubuntu `dumpcap` is
installed `0754 root:wireshark`. A user outside the `wireshark` group cannot
*execute* it at all — not a capability failure at runtime, a permission denied
on the file. So on a machine where the evaluator never answered "yes" to
`dpkg-reconfigure wireshark-common`, `make` would stop before printing a single
latency figure, and the graded requirement would go unmet through no fault of
the library. Telling the evaluator to run `sudo` first is not a fix: a delivery
that asks for a password before it builds reads as a delivery that does not
build.

**Decision:** record the bus with `tools/bus_tap.cpp`, built as `build/bus-tap`.

The observation that makes it possible is that our bus is not a kernel device.
`-netdev socket,mcast=` is an ordinary IPv4 UDP multicast group, and QEMU puts
**exactly one guest Ethernet frame in each datagram, with no framing of its
own**. So the bus can be observed by *joining* it — `socket(AF_INET,
SOCK_DGRAM)`, `SO_REUSEADDR`, `IP_ADD_MEMBERSHIP` on `127.0.0.1` — instead of by
a sniffer that needs `CAP_NET_RAW`. We are not tapping an interface; we are one
more listener on a broadcast medium, which is exactly what every vehicle already
is. Nothing is taken away from anyone: multicast delivers a copy to every
member.

Timestamps come from `SO_TIMESTAMPNS`, so the kernel stamps each datagram on
arrival rather than when this process gets scheduled — the same class of
timestamp a sniffer reports, and the reason the measured latency did not move.

**Why it is better than the tool it replaces, not merely cheaper:**

- it needs no privilege, no group membership and no package, so a bare clone on
  a bare machine prints a latency;
- the `.pcap` it writes is `LINKTYPE_ETHERNET`, so Wireshark dissects **our**
  frames directly. The `dumpcap` capture shows them buried inside a host UDP
  datagram, with our Ethernet header as an undissected payload.

`dumpcap` is still run when the machine happens to have it usable, as a
*secondary* recorder, and never fatally. It observes the same bus through a
completely different mechanism — a packet socket on the interface, rather than a
member of the group — so when both are present they corroborate each other.

*Verified* (2026-08-24, one 5-vehicle run with both recorders live): both saw
**203 frames, identical byte for byte**, and the latency computed from each
agreed to the microsecond (3.749 ms). `scripts/frames.sh` is the single place
that knows which source is in use, which is why `verify-capture.sh` and
`analyze-capture.sh` did not change by one line when the dependency was removed.

*Also verified*: a full `make` on a `PATH` containing **no `dumpcap`, no
`tshark`, no `setcap`, no `unshare` and no `sudo`** completes green and prints
the latency.

### 2.10 `test-engine` climbs for privilege, and never prompts

Level 1 of `tests/test_engine.cpp` opens a real `AF_PACKET`/`SOCK_RAW` socket
and needs `CAP_NET_RAW`. `make check` used to export `VCOMM_REQUIRE_RAW=1`,
turning the skip into a failure — correct strictness, but it made a bare `make`
fail on any machine where nobody had run `sudo setcap` first.

**Decision:** `scripts/run-engine-test.sh` tries, in order, every way of getting
the capability that *cannot block on a password prompt* — a file capability
already on the inode, being root, passwordless `sudo -n`, an unprivileged user
namespace (`unshare -Urn`, where we are root in a private netns) — and if every
rung fails, runs level 0 and says which rung it reached.

**Why that is not a hole in the evaluation.** `make check` does not stop there:
it boots five vehicles, and inside each one the application *is* root and opens
fifteen real raw sockets — the same Engine, the same code path, on a real
interface. The frames those sockets emit are then recorded independently on the
host by `bus-tap` and checked byte by byte by `verify-capture.sh`. The raw
socket path is therefore proven by every `make`; the last rung only loses the
ability to prove it a second time, on the host, a few seconds earlier. What
*would* be a hole is going green while claiming coverage we do not have, which
is why the rung reached is printed either way.

`unshare -Urn` is blocked on this machine by Ubuntu's
`kernel.apparmor_restrict_unprivileged_userns=1`, so that rung is probed before
it is used rather than attempted and reported as an error. `make caps` remains,
as an optional extra that is a prerequisite of nothing.

---

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

  **Where this stands (2026-08-24).** The `Protocol` layer is already immune:
  `send()` writes `Header::_length` (`protocol.h:198`) and `receive()` reads the
  payload size from it rather than from the frame (`protocol.h:228`), with the
  comment *"reliable across all engines"*. Real hardware padding a 20-byte frame
  to 60 would therefore still deliver 6 bytes to the application.

  **What is still exposed is the `NIC` layer**, which has no length field of its
  own and cannot have one — `Ethernet::Header` is the 14 bytes on the wire, and
  adding a fifteenth would stop it being Ethernet. So both of these are frame-
  length arithmetic and will over-report by the padding on any padding medium:

  - `NIC::handle()` — `payload = size - Ethernet::HEADER_SIZE`, which decides
    how many bytes are copied into the buffer;
  - `NIC::unmarshal()` — `buf->size() - Ethernet::HEADER_SIZE`, its return
    value.

  This is **not** a bug today and is arguably not one ever: the extra bytes are
  padding, they are copied but never read, and the only consumer of
  `unmarshal()` is `Protocol::receive()`, which uses the return value solely as
  a *lower-bound sanity check* (`payload_bytes < sizeof(Header)`) before
  switching to `_length`. A padded frame makes that check *more* permissive, not
  less.

  **Decided** (2026-08-24): `NIC::unmarshal()`'s contract is **frame bytes minus
  header, padding included** — the honest description of what the link layer can
  actually know — and it says so in `nic.h`. Renaming it "payload bytes" would
  be a promise the layer cannot keep on any padding medium. The true payload
  size travels in `Protocol::Header::_length`, which `Protocol::receive()` now
  also **clamps** to the bytes that actually arrived: the field comes off the
  wire and nothing else was checking it.
- **A capture on the host does not prove reception.** Seeing the datagram leave
  proves format and timing; the proof that a guest processed the message is the
  VM's log. `scripts/verify-capture.sh` is deliberately confined to what the
  capture *can* prove — layout, addressing, EtherType — and the correctness
  verdict comes from `scripts/run-fleet.sh` reading the guests' own output.
- **`Protocol::Header` travels in host byte order.** `_from_port`, `_to_port`
  and `_length` are written and read as native `unsigned short`. Every vehicle
  on this bus is x86, so it works and the capture confirms it (`0004` reads back
  as port 1024 little-endian). It is still a portability limit, and the one
  place in the project where the "pick one convention" rule of `ethernet.h` is
  not followed. The application payload above it *is* big-endian, precisely
  because a shell script has to read it out of a hex dump.
- **The latency is a round trip and the statistics say so.** It includes each
  guest's processing of the request. It is not halved: nothing here measures the
  symmetry that would justify halving it.

- **The recorder must be on the same interface as the bus.** `bus-tap` joins the
  group through `SO2_LOCALADDR` (`127.0.0.1` by default, matching the
  `localaddr=` the vehicles are launched with). Move the bus to a real NIC
  without moving `SO2_LOCALADDR` with it and the capture comes back empty — it
  will not silently record the wrong thing, but it will not find the right thing
  either.

---

## 4. Open items

- [ ] Stage 1 slides in `doc/`, with the performance evaluation. The only
      delivery artefact that does not exist yet.
- [ ] Confirm the EtherType `0x88B5` with Artur and André before the
      presentation.

### Closed on 2026-08-25

| Was open | Where it landed |
|---|---|
| Diagrams in `doc/` (fleet topology; layers; the sequence of a message from `send()` until the application thread wakes up, marking where the signal context ends) | `doc/DOCUMENTACAO_UML.md` §1–§4, exported to `doc/uml-png/`. The signal context is marked in §3.3. |
| `make` had to compile **and run** the whole evaluation | `.DEFAULT_GOAL := check`; the pipeline runs to `stats` and returns non-zero on any failure |
| The average latency had to be computed automatically at the end of `make` | `scripts/analyze-capture.sh`, the last target of `check`. Measured 2.371 ms mean round-trip over 80 samples on 2026-08-25 |
| Five vehicles as VMs, components as POSIX processes | `scripts/run-fleet.sh` + `fork()` in `app/main.cpp`: 5 VMs × 3 processes, 5/5 OK |

### Closed on 2026-08-24

Each of these was an open decision in this document; each is now implemented and
covered by a test that fails if it is undone.

| Was open | Decision | Where |
|---|---|---|
| Does each component open its own raw socket, or does one process relay? | Its own — one Engine per process, fork before constructing | §2.7 |
| Queue-full policy: `update()` ignored `insert()`'s `bool` | `update()` returns it; a full queue frees the buffer and counts `rx_dropped` | §1.12, `test_protocol.cpp` §5 |
| Buffer pool partition | Partitioned: `alloc()` over the send half, `handle()` over the receive half | §2.6, `test_protocol.cpp` §3 |
| `NIC::unmarshal()`'s contract under padding | Frame bytes minus header, padding included — stated in `nic.h` | §3 |
| `Protocol::receive()` trusted `hdr->_length` unclamped | Clamped to the bytes that actually arrived | §3, `test_protocol.cpp` §4 |
| `make` needed `sudo`/`wireshark` group to capture, and `sudo setcap` for the Engine test | Neither: the bus is recorded by joining it, and the Engine test climbs for privilege without prompting | §2.9, §2.10 |

And one that was not open because nobody had found it: `Communicator::send()`
broadcast to port 0, so **every message was delivered to nobody**. See §1.11.
