#ifndef LIBVCOMM_H
#define LIBVCOMM_H

#include "libvcomm/traits.h"
#include "libvcomm/net/ethernet.h"
#include "libvcomm/net/buffer.h"
#include "libvcomm/list.h"
#include "libvcomm/sem.h"
#include "libvcomm/observer.h"
#include "libvcomm/message.h"
#include "libvcomm/engine/threaded_ethernet_engine.h"
#include "libvcomm/net/nic.h"
#include "libvcomm/protocol.h"
#include "libvcomm/communicator.h"

typedef NIC<ThreadedEthernetEngine> VehicleNIC;
typedef Protocol<VehicleNIC> VehicleProtocol;
typedef Communicator<VehicleProtocol> VehicleCommunicator;

#endif // LIBVCOMM_H
