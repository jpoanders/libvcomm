# libvcomm

Communication library for critical autonomous systems.
INE5424 — Operating Systems II — UFSC — 2026/2 — **Group M10** — Stage 1.

Each vehicle is a QEMU VM; each vehicle component is a POSIX process. The VMs
talk to one another over **raw Ethernet frames**, broadcast, without IP.

---

## The path of a message

Being able to recite this is half the presentation:

```
  application                   Communicator::send(Message*)
      |                                   |
      v                         Protocol::send()  -> builds the protocol Header
  Communicator                            |
      |                         NIC::alloc() -> takes a buffer from the pool,
      v                                        builds the Ethernet header
   Protocol                                    (dst=broadcast)
      |                         NIC::send(buf)
      v                                   |
     NIC                       Engine::engine_send() -> sendto()  <<< THE ONLY syscall
      |                                   |
      v                          [ QEMU's multicast bus ]
   Engine                                 |
      |                      [SIGIO] Engine::drain() -> recvfrom()
      v                                   |
  raw socket                     NIC::handle()  -> copies into a buffer
                                          |
                              Observed::notify(EtherType, buf)
                                          |
                                Protocol::update()
                                          |
                              Observed::notify(Port, buf)
                                          |
                              Communicator::update() -> semaphore.v()
                                          |
                    ~~~ the SIGNAL HANDLER ends here and the interrupted   ~~~
                    ~~~ thread goes back to what it was doing              ~~~
                                          |
                    the application thread wakes up in Communicator::receive()
```

No layer above the `Engine` knows about sockets. It is that promise that lets
Stage 2 (shared memory) swap only the `Engine`.

**Everything between `drain()` and `semaphore.v()` runs inside a signal
handler** — the assignment requires propagation through POSIX signals, and in
EPOS that same stretch runs in the NIC's interrupt handler. The practical
consequence bites within the first hour: no `printf`, no `new` and no
`std::mutex` on that path. See `doc/design-decisions.md` §2.1 and
`man 7 signal-safety`.

---

## Map of the files

| File | State | What it is |
|---|---|---|
| `include/traits.h` | done | EtherType, interface, pool size — configuration |
| `include/ethernet.h` | done | `Address`, `Header`, `Frame`, MTU, `Statistics` |
| `include/list.h` | done | `List` (FIFO) and `Ordered_List` (observers) |
| `include/sem.h` | done | `Semaphore` over `sem_t` |
| `include/buffer.h` | done | `Buffer<T>` and the ownership rule |
| `include/message.h` | done | Stage 1's `Message` (array of bytes) |
| `include/observer.h` | **mixed** | `Concurrent_*` transcribed from the PDF; `Conditionally_Data_Observed` is yours |
| `include/engine/raw_socket_engine.h` + `src/raw_socket_engine.cpp` | **yours** | the syscalls. The heart of Stage 1 |
| `include/nic.h` | **yours** | buffer pool, marshalling, notification |
| `include/protocol.h` | **yours** | ports, protocol header, the Observer fold |
| `include/communicator.h` | **yours** | the API the application sees |
| `app/main.cpp` | **yours** | role from `SO2_VM_ID`, forking the components |
| `scripts/*.sh` | **yours** | fleet, capture, statistics |
| `tests/*` | done | `test-support`, `test-stack` and `test-engine` |
| `doc/` | done | the assignment, the practical guide and the design decisions |
| `vendor/` | done | the instructor's starter, versioned — see `vendor/README.md` |

---

## The documents

| File | What it is |
|---|---|
| [`doc/full_assignment.pdf`](doc/full_assignment.pdf) | the course's project proposal — the global requirements and the four stages |
| [`doc/practical_class_1_guide.md`](doc/practical_class_1_guide.md) | the Practical Class 1 guide: the bench, QEMU, tshark, and the **Stage 1 acceptance checklist in section 9** |
| [`doc/design-decisions.md`](doc/design-decisions.md) | the deviations from the assignment's API and our own decisions, with justification — this is what the panel reads |
| [`ROADMAP.md`](ROADMAP.md) | working document: implementation order, estimates, emergency cuts |
| [`vendor/README.md`](vendor/README.md) | the starter's provenance and why it is versioned |

---

## The repository is self-contained

Nothing here points outside the root. The instructor's starter (kernel,
initramfs, `run-vm.sh`) is versioned in `vendor/` as a tarball, and the
assignment and the practical guide are in `doc/`. A clean clone, on any machine,
runs the whole `make` without you having to have taken the course under the same
`$HOME`.

```bash
make doctor    # what is missing on the machine, before you find out mid-fleet
make starter   # checks the tarball's sha256 and unpacks into build/vm/
```

`build/vm/` is a **working copy**: that is where `install-app.sh` runs, because
`repack-initramfs.sh` writes into the tree it lives in. The tarball is never
touched, and `make clean-vm` restores the image to factory state. The unpacked
copy matches byte for byte against the `SHA256SUMS` that came in the bundle.

Tools the bench requires: `g++` (C++17), `make`, `file`, `qemu-system-x86_64`,
`cpio`, `timeout`, `dumpcap`/`tshark`, and `setcap` for `test-engine`'s level 1.
`make doctor` checks all of them and also warns about two things that bite: a
missing `/dev/kvm` (QEMU falls back to TCG and the latency picks up an emulation
bias) and which interface the multicast bus is going out through.

---

## Implementation order

It is in **[`ROADMAP.md`](ROADMAP.md)** — 8 phases, each with what to implement,
how to know it is finished, how long to estimate, and which trap it hides. The
weekend plan and the emergency cuts are there too, in case the presentation is
brought forward.

Phases 1 through 4 are done — Observer, buffer pool, Engine and marshalling;
`test-stack` and `test-engine` prove all four. **Next is Phase 5**, `Protocol` +
`Communicator`, and it is the one that closes the demo path: five VMs, one
transmitting and four proving reception.

---

## Commands

```bash
make doctor         # check the bench tools
make app            # static x86-64 binary for inside the VM
make test-support   # support classes — must pass today
make test-stack     # the stack — green since 2026-08-24 (Observer, pool, marshalling)
make test-engine    # the Engine; level 1 needs CAP_NET_RAW
make help           # the rest
```

`test-engine`'s level 1 opens a real `AF_PACKET` socket over `lo`. Without
privileges it skips itself; to exercise it:

```bash
sudo setcap cap_net_raw+ep build/test-engine    # dies on every relink — redo it
sudo scripts/test-engine-veth.sh                # proves drain()'s error arm
```

Inside `make check`, `VCOMM_REQUIRE_RAW=1` turns that skip into a failure: the
evaluation target must not go green having skipped the only test that opens a
socket.

Variables worth knowing:

```bash
SO2_MCAST=239.10.10.10:15424   # the group's bus
VM_TIMEOUT=20                  # per-VM ceiling in the test
WORKVM=build/vm                # the image's working copy
```

---

## Before presenting

The acceptance checklist is in section 9 of
`doc/practical_class_1_guide.md`. The items that take the most work and show up
least in the code:

- `make` at the root has to **fail** when a receiver loses a frame, when a VM
  blows the timeout, or when the capture comes back empty. A test that only
  prints a warning is not gradeable.
- the latency has to be printed **automatically** at the end of `make`, with the
  right label (round-trip or one-way — do not swap one for the other).
- diagrams and slides in `doc/`.
- the graded commit has to be on `main`.

The bench's honest caveats (no `/dev/kvm`, `virtio-net` without padding, a
capture does not prove reception) are in `doc/design-decisions.md` §3. Saying so
in the presentation is stronger than hiding it.
