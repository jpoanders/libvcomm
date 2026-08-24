#ifndef LIBVCOMM_H
#define LIBVCOMM_H

// =============================================================================
// libvcomm — communication library for critical autonomous systems.
// INE5424 (Operating Systems II) — UFSC — Group M10 — Stage 1.
//
// Umbrella header: the application includes only this one.
// =============================================================================

#include "traits.h"
#include "ethernet.h"
#include "buffer.h"
#include "list.h"
#include "sem.h"
#include "observer.h"
#include "message.h"
#include "engine/raw_socket_engine.h"
#include "nic.h"
#include "protocol.h"
#include "communicator.h"

// -----------------------------------------------------------------------------
// Stage 1's concrete stack.  This is WHERE the templates become real types, and
// it is the only line that changes when Stage 2 brings the shared-memory
// Engine:
//
//     typedef NIC<Shared_Memory_Engine> Component_NIC;
//
// If that swap requires touching NIC, Protocol or Communicator, the layer
// separation failed somewhere.
// -----------------------------------------------------------------------------

typedef NIC<Raw_Socket_Engine> Vehicle_NIC;
typedef Protocol<Vehicle_NIC> Vehicle_Protocol;
typedef Communicator<Vehicle_Protocol> Vehicle_Communicator;

#endif // LIBVCOMM_H
