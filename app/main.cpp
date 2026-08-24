// =============================================================================
// Stage 1 test application.
//
// The starter's /init runs /student/app and exports SO2_VM_ID.  A single binary
// is installed on every VM; the ROLE comes from the ID.  That is why this main
// has a switch: one image, five vehicles, different behaviours.
//
// The assignment requires each vehicle COMPONENT (sensor, fuser, ECU,
// powertrain) to be a POSIX PROCESS.  One VM = one vehicle = several processes.
// This main is the vehicle's root process; the components are born from fork().
// =============================================================================

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "libvcomm.h"

namespace {

// Vehicle 1 transmits; 2..5 receive and prove reception.  It is the minimum
// that satisfies the "four receivers prove reception of one sender's broadcast"
// item in section 9 of the guide.
enum Role
{
    ROLE_SENDER,
    ROLE_RECEIVER
};

Role role_of(int vm_id)
{
    return (vm_id == 1) ? ROLE_SENDER : ROLE_RECEIVER;
}

int vm_id_from_env()
{
    const char * id = std::getenv("SO2_VM_ID");
    return id ? std::atoi(id) : 0;
}

// -----------------------------------------------------------------------------
void run_sender(Vehicle_Communicator & comm, int vm_id)
{
    // TODO(joao):
    //   - build an identifiable Message (vehicle id + sequence number)
    //   - N messages, with the first few marked as warm-up and EXCLUDED from
    //     the statistics (the guide asks for this explicitly)
    //   - print one line per send, in a format that is easy to match in the
    //     log:
    //         TX vm=1 seq=7 bytes=32
    //   - silence on the console during the measured interval; log afterwards
    (void)comm;
    (void)vm_id;
}

void run_receiver(Vehicle_Communicator & comm, int vm_id)
{
    // TODO(joao):
    //   - a receive() loop with a stop condition (a counter or a deadline)
    //   - print  RX vm=3 from=... seq=7 bytes=32
    //   - at the end, print a verdict the test script can check:
    //         RESULT vm=3 received=20 expected=20 OK
    //     `make` MUST FAIL when a receiver loses a frame; a test that only
    //     prints a warning is not gradeable.
    (void)comm;
    (void)vm_id;
}

} // namespace

int main()
{
    const int vm_id = vm_id_from_env();
    std::printf("[vm %d] libvcomm — Stage 1\n", vm_id);

    // -------------------------------------------------------------------------
    // Build the stack.  Note the direction of the dependencies: each layer
    // receives the one below it already built and never constructs it itself.
    // That is what makes it possible to test Protocol with a fake NIC later.
    // -------------------------------------------------------------------------
    Vehicle_NIC nic;

    // TODO(joao): abort with a clear message if the NIC did not come up (a raw
    // socket requires CAP_NET_RAW; inside the VM you are root, on the host you
    // are not).

    Vehicle_Protocol protocol(&nic);

    // The port identifies the COMPONENT within the vehicle.  A fixed one goes
    // here just to bring the stack up; when you fork the components, each gets
    // its own.
    const Vehicle_Protocol::Port port = 1000;
    Vehicle_Communicator comm(&protocol,
                              Vehicle_Protocol::Address(nic.address(), port));

    // TODO(joao): fork() the vehicle's components.  A design question worth
    // deciding BEFORE writing any code: does each child process open its OWN
    // raw socket (simple, but N sockets per VM and each one receives a copy of
    // everything), or does a single NIC-owning process relay to the children
    // (which is where Stage 2 is heading, with shared memory)?  The answer
    // changes the architecture — record it in doc/.

    if (role_of(vm_id) == ROLE_SENDER)
        run_sender(comm, vm_id);
    else
        run_receiver(comm, vm_id);

    return 0;
}
