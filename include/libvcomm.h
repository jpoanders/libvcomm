#ifndef LIBVCOMM_H
#define LIBVCOMM_H

// =============================================================================
// libvcomm — biblioteca de comunicação para sistemas autônomos críticos.
// INE5424 (Sistemas Operacionais II) — UFSC — Grupo M10 — Etapa 1.
//
// Header guarda-chuva: a aplicação inclui só este.
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
// A pilha concreta da Etapa 1.  É AQUI que os templates viram tipos de verdade,
// e é a única linha que muda quando a Etapa 2 trouxer a Engine de memória
// compartilhada:
//
//     typedef NIC<Shared_Memory_Engine> Component_NIC;
//
// Se essa troca exigir mexer em NIC, Protocol ou Communicator, a separação de
// camadas falhou em algum lugar.
// -----------------------------------------------------------------------------

typedef NIC<Raw_Socket_Engine> Vehicle_NIC;
typedef Protocol<Vehicle_NIC> Vehicle_Protocol;
typedef Communicator<Vehicle_Protocol> Vehicle_Communicator;

#endif // LIBVCOMM_H
