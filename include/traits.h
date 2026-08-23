#ifndef LIBVCOMM_TRAITS_H
#define LIBVCOMM_TRAITS_H

// =============================================================================
// Ponto ÚNICO de configuração da biblioteca.  Nada de constante mágica
// espalhada pelo código: se um número aparece em dois lugares, ele mora aqui.
// =============================================================================
//
// DESVIO CONSCIENTE DO PDF — leve isso para a banca:
//
//   O enunciado escreve  Traits<NIC>::SEND_BUFFERS  e
//   Traits<Protocol>::ETHERNET_PROTOCOL_NUMBER.  Só que NIC e Protocol são
//   *templates de classe*, e um template não é um tipo — `Traits<NIC>` não
//   compila.  O EPOS resolve isso com bases não-template (NIC_Common,
//   Protocol_Common) e especializa Traits sobre elas.
//
//   Aqui ancoramos tudo em Ethernet, que já é classe concreta e já é base de
//   NIC.  Mesmo efeito, um ponto de configuração só.  Ver doc/decisoes.md.

template <typename T> struct Traits
{
    static const bool debugged = false;
};

class Ethernet;

template <> struct Traits<Ethernet>
{
    // Interface dentro da VM.  O /init do starter sobe eth0 (virtio-net-pci).
    static constexpr const char * INTERFACE = "eth0";

    // EtherType do Grupo M10.  0x88B5 é o "IEEE Local Experimental EtherType 1"
    // (RFC 5342 §2.3.4) — faixa reservada justamente para protocolos locais.
    // O kernel usa este valor como FILTRO no socket(), então ele é o que separa
    // os frames do projeto do lixo IPv6 que o guest emite sozinho.
    // >>> CONFIRMAR com Artur e André antes da apresentação. <<<
    static const unsigned short PROTOCOL_NUMBER = 0x88B5;

    // Pool de buffers da NIC.  BUFFER_SIZE = SEND + RECEIVE.
    static const unsigned int SEND_BUFFERS = 16;
    static const unsigned int RECEIVE_BUFFERS = 16;

    static const bool debugged = false;
};

#endif // LIBVCOMM_TRAITS_H
