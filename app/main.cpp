// =============================================================================
// Aplicação de teste da Etapa 1.
//
// O /init do starter roda /student/app e exporta SO2_VM_ID.  Um binário só é
// instalado em todas as VMs; o PAPEL vem do ID.  É por isso que este main tem
// um switch: uma imagem, cinco veículos, comportamentos diferentes.
//
// O enunciado exige que cada COMPONENTE de um veículo (sensor, fusor, ECU,
// powertrain) seja um PROCESSO POSIX.  Uma VM = um veículo = vários processos.
// Este main é o processo raiz do veículo; os componentes nascem de fork().
// =============================================================================

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "libvcomm.h"

namespace {

// O veículo 1 emite; 2..5 recebem e provam a recepção.  É o mínimo que fecha o
// item "quatro receptores provam a recepção do broadcast de um emissor" da
// seção 9 do guia.
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
    //   - montar uma Message identificável (id do veículo + número de
    //   sequência)
    //   - N mensagens, com as primeiras marcadas como warm-up e EXCLUÍDAS da
    //     estatística (o guia pede isso explicitamente)
    //   - imprimir uma linha por envio, em formato fácil de casar no log:
    //         TX vm=1 seq=7 bytes=32
    //   - silêncio no console durante o intervalo medido; log depois
    (void)comm;
    (void)vm_id;
}

void run_receiver(Vehicle_Communicator & comm, int vm_id)
{
    // TODO(joao):
    //   - laço de receive() com condição de parada (contador ou tempo)
    //   - imprimir  RX vm=3 from=... seq=7 bytes=32
    //   - ao final, imprimir um veredito que o script de teste possa conferir:
    //         RESULT vm=3 received=20 expected=20 OK
    //     O `make` tem que FALHAR quando um receptor perde frame; um teste que
    //     só imprime aviso não é avaliável.
    (void)comm;
    (void)vm_id;
}

} // namespace

int main()
{
    const int vm_id = vm_id_from_env();
    std::printf("[vm %d] libvcomm — Etapa 1\n", vm_id);

    // -------------------------------------------------------------------------
    // Monta a pilha.  Repare na direção das dependências: cada camada recebe a
    // de baixo pronta e nunca a constrói sozinha.  Isso é o que torna possível
    // testar Protocol com uma NIC falsa mais tarde.
    // -------------------------------------------------------------------------
    Vehicle_NIC nic;

    // TODO(joao): abortar com mensagem clara se a NIC não subiu (raw socket
    // exige CAP_NET_RAW; dentro da VM você é root, no host não).

    Vehicle_Protocol protocol(&nic);

    // A porta identifica o COMPONENTE dentro do veículo.  Aqui vai uma fixa só
    // para a pilha subir; quando você forkar os componentes, cada um recebe a
    // sua.
    const Vehicle_Protocol::Port port = 1000;
    Vehicle_Communicator comm(&protocol,
                              Vehicle_Protocol::Address(nic.address(), port));

    // TODO(joao): fork() dos componentes do veículo.  Pergunta de projeto que
    // vale decidir ANTES de escrever: cada processo filho abre o PRÓPRIO raw
    // socket (simples, mas N sockets por VM e cada um recebe cópia de tudo), ou
    // um processo dono da NIC repassa aos filhos (é para onde a Etapa 2 vai,
    // com memória compartilhada)?  A resposta muda a arquitetura — anote em
    // doc/.

    if (role_of(vm_id) == ROLE_SENDER)
        run_sender(comm, vm_id);
    else
        run_receiver(comm, vm_id);

    return 0;
}
