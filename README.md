# libvcomm

Communication library for critical autonomous systems.
INE5424 — Operating Systems II — UFSC — 2026/2 — **Group M10** — Stage 1.

Each vehicle is a QEMU VM; each vehicle component is a POSIX process. The VMs
talk to one another over **raw Ethernet frames**, broadcast, without IP.

**Stage 1 is complete.** A bare `make` compiles the library, runs the host
tests, injects the binary into the initramfs, boots five vehicles, records the
bus and prints the latency — and returns non-zero if any of that fails.

```
make check: everything green.
```

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
Stage 2 (shared memory) swap only the `Engine` — and it is the same promise that
lets `tests/loopback_engine.h` swap it for an in-process queue, so `Protocol` and
`Communicator` are tested in milliseconds without a socket, a privilege or a VM.

**Everything between `drain()` and `semaphore.v()` runs inside a signal
handler** — the assignment requires propagation through POSIX signals, and in
EPOS that same stretch runs in the NIC's interrupt handler. The practical
consequence bites within the first hour: no `printf`, no `new` and no
`std::mutex` on that path. See `doc/design-decisions.md` §2.1 and
`man 7 signal-safety`.

### Addressing: one port is the channel

`Protocol::Address` is `(MAC, Port)`. `Communicator::send()` broadcasts to the
**sender's own port**, so two agents that bind the same number are peers on one
channel — which is what a radio cell is. The fleet uses port `1024`
(`FLEET_PORT` in `app/main.cpp`); `tests/test_protocol.cpp` exercises the
per-component-port variant the `Protocol` also supports. See
`doc/design-decisions.md` §1.11.

---

## Map of the files

| File | State | What it is |
|---|---|---|
| `include/traits.h` | done | EtherType, interface, pool size — configuration |
| `include/ethernet.h` | done | `Address`, `Header`, `Frame`, MTU, `Statistics` |
| `include/list.h` | done | `List` (FIFO) and `Ordered_List` (observers) |
| `include/sem.h` | done | `Semaphore` over `sem_t`, with a timed `p()` |
| `include/buffer.h` | done | `Buffer<T>` and the ownership rule |
| `include/message.h` | done | Stage 1's `Message` (array of bytes, `MAX_SIZE = 1024`) |
| `include/observer.h` | done | `Concurrent_*` from the PDF; `Conditionally_Data_Observed` is ours |
| `include/engine/raw_socket_engine.h` + `src/raw_socket_engine.cpp` | done | the syscalls. The heart of Stage 1 |
| `include/nic.h` | done | buffer pool (partitioned TX/RX), marshalling, notification |
| `include/protocol.h` | done | ports, protocol header, the Observer fold |
| `include/communicator.h` | done | the API the application sees (`send`, `receive`, timed `receive`) |
| `include/libvcomm.h` | done | the concrete stack: `Vehicle_NIC`, `Vehicle_Protocol`, `Vehicle_Communicator` |
| `app/main.cpp` | done | role from `SO2_VM_ID`, forks the components, asserts an exact count |
| `app/fleet_payload.h` | done | the wire payload the fleet test agrees on (magic, kind, ids, seq) |
| `tools/bus_tap.cpp` | done | records the bus by **joining** it — no dumpcap, no privileges |
| `tests/*` | done | `test-support`, `test-stack`, `test-protocol`, `test-engine` |
| `tests/loopback_engine.h` | done | an `Engine` with no socket, for the host-side protocol tests |
| `scripts/*.sh` | done | fleet, capture, verification, statistics, UML export |
| `doc/` | done | the assignment, the guide, the design decisions, the UML, the checklist |
| `vendor/` | done | the instructor's starter, versioned — see `vendor/README.md` |

---

## The documents

| File | What it is |
|---|---|
| [`doc/full_assignment.pdf`](doc/full_assignment.pdf) | the course's project proposal — the global requirements and the four stages |
| [`doc/practical_class_1_guide.md`](doc/practical_class_1_guide.md) | the Practical Class 1 guide: the bench, QEMU, tshark, and the **Stage 1 acceptance checklist in section 9** |
| [`doc/design-decisions.md`](doc/design-decisions.md) | the deviations from the assignment's API and our own decisions, with justification — this is what the panel reads |
| [`doc/DOCUMENTACAO_UML.md`](doc/DOCUMENTACAO_UML.md) | the UML: components, packages, deployment, classes, sequences, states — with a code×diagram fidelity matrix |
| [`doc/uml-png/`](doc/uml-png/) | the same diagrams exported to PNG, for the slides |
| [`doc/checklist-entrega1.md`](doc/checklist-entrega1.md) | every bullet of the assignment turned into a verifiable item, with its current state |
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
touched, and `make clean-vm` restores the image to factory state.

### `make` never calls sudo and never prompts

That is a hard requirement, not a convenience: an evaluator asked for a password
before the build will reasonably conclude the delivery does not build. The two
places that classically want privileges are handled instead of demanded:

- **capturing the bus** — `build/bus-tap` joins QEMU's multicast group with an
  ordinary UDP socket. No `dumpcap`, no `tshark`, no membership of the
  `wireshark` group (on Debian and Ubuntu `dumpcap` is installed `0754
  root:wireshark`: a grader outside that group cannot even execute it).
- **`test-engine` level 1** — `scripts/run-engine-test.sh` climbs a ladder of
  ways to get `CAP_NET_RAW` that cannot prompt, and if every rung fails it says
  so and continues. The fleet then exercises the same raw sockets inside the
  VMs anyway.

`make caps` and `make bus-local` exist for the two optional extras that do want
sudo. Neither is part of `make`.

Tools the bench requires: `g++` (C++17), `make`, `file`, `qemu-system-x86_64`,
`cpio` and `timeout`. `dumpcap`/`tshark` and `setcap` are **optional**.
`make doctor` checks all of them and also warns about a missing `/dev/kvm`
(QEMU falls back to TCG and the latency picks up an emulation bias).

---

## Commands

```bash
make                # == make check: the whole evaluation, end to end
make doctor         # check the bench tools
make all            # build + the host tests only (no VMs)

make app            # static x86-64 binary for inside the VM
make tap            # the unprivileged bus recorder
make test-support   # the support classes
make test-stack     # the stack: Observer, buffer pool, marshalling
make test-protocol  # Protocol + Communicator over a loopback Engine
make test-engine    # the Engine; level 1 uses whatever privilege it can get

make image          # inject the binary into the initramfs
make fleet          # boot the five vehicles and check their verdicts
make capture        # prove the frame layout from the last capture
make stats          # compute the latency from the last capture
make help           # the rest
```

`make check` is the default goal, and it is a pipeline: the fleet has to run
before there is a capture to prove, and the capture before there is a latency to
compute. `.NOTPARALLEL:` keeps `make -j8` from reordering them.

Variables worth knowing:

```bash
SO2_MCAST=239.10.10.10:15424   # the group's bus
VMS="1 2"                      # a smaller fleet while debugging
VM_TIMEOUT=60                  # per-VM ceiling (the app powers off when done)
WORKVM=build/vm                # the image's working copy
```

To exercise the Engine's raw socket on the host as well as inside the VMs:

```bash
make caps                        # sudo setcap cap_net_raw+ep build/test-engine
sudo scripts/test-engine-veth.sh # proves drain()'s error arm
```

The capability lives on the inode, so it dies on every relink — redo it.

---

## What the fleet test proves

Five vehicles, three POSIX processes each, one binary; the role comes from
`SO2_VM_ID`. VM 1's component 0 broadcasts `REQUEST 0..N-1`; component 0 of VMs
2–5 answers each one with a `RESPONSE`; every other component listens and
counts. Each component asserts a count it can **predict exactly** — components
on VMs 2–5 expect every REQUEST (`N`), components on VM 1 expect one RESPONSE
per remote vehicle (`4N`) — which is what makes the run gradeable instead of
merely noisy.

Neither number depends on whether the bus echoes a vehicle's own broadcast back
to it, because neither counts a kind its own vehicle emits.

`scripts/verify-capture.sh` then re-derives the verdict **from the bytes on the
wire alone**, without reading a single VM log: every destination broadcast,
every source a real per-VM MAC, the payload's vehicle id agreeing with the
source MAC, five distinct vehicles and three distinct components present.

A representative run on this bench (no `/dev/kvm`, so QEMU in TCG):

```
run-fleet: 5 vehicles (1 2 3 4 5) x 3 components, bus 239.10.10.10:15424 via 127.0.0.1
run-fleet: fleet finished in 9s — all 5 vehicles OK

verify-capture: 191 packets, 161 of EtherType 0x88b5
  ok  every destination is ff:ff:ff:ff:ff:ff
  ok  every source is a real per-VM MAC, none zeroed
  ok  payload vehicle id agrees with the source MAC on every frame
  ok  5 distinct vehicles and 3 distinct components on the wire
  ok  kinds seen: READY 36, REQUEST 25, RESPONSE 100

  ROUND-TRIP LATENCY  (REQUEST -> RESPONSE, one host clock)
   samples   80        mean   2.371 ms      min    0.705 ms
   median    2.245 ms  p95    3.990 ms      max    6.293 ms
```

**Round trip, not one-way, and not halved** — nothing here measures the symmetry
that would justify halving it. The first five requests are warm-up and are
excluded. Under TCG every figure carries the emulator's overhead: read it as an
upper bound.

---
