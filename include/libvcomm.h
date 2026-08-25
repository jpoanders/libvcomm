#ifndef LIBVCOMM_H
#define LIBVCOMM_H

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

typedef NIC<Raw_Socket_Engine> Vehicle_NIC;
typedef Protocol<Vehicle_NIC> Vehicle_Protocol;
typedef Communicator<Vehicle_Protocol> Vehicle_Communicator;

#endif // LIBVCOMM_H
