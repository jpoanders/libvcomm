#ifndef LIBVCOMM_TRAITS_H
#define LIBVCOMM_TRAITS_H

// =============================================================================
// The library's SINGLE configuration point.  No magic constants scattered
// through the code: if a number shows up in two places, it lives here.
// =============================================================================
//
// DELIBERATE DEVIATION FROM THE PDF — take this one to the panel:
//
//   The assignment writes  Traits<NIC>::SEND_BUFFERS  and
//   Traits<Protocol>::ETHERNET_PROTOCOL_NUMBER.  Except NIC and Protocol are
//   *class templates*, and a template is not a type — `Traits<NIC>` does not
//   compile.  EPOS solves this with non-template bases (NIC_Common,
//   Protocol_Common) and specializes Traits over those.
//
//   Here we anchor everything on Ethernet, which is already a concrete class
//   and already a base of NIC.  Same effect, still a single configuration
//   point.  See doc/design-decisions.md.

template <typename T> struct Traits
{
    static const bool debugged = false;
};

class Ethernet;

template <> struct Traits<Ethernet>
{
    // Interface inside the VM.  The starter's /init brings up eth0
    // (virtio-net-pci).
    static constexpr const char * INTERFACE = "eth0";

    // Group M10's EtherType.  0x88B5 is the "IEEE Local Experimental
    // EtherType 1" (RFC 5342 §2.3.4) — a range reserved precisely for local
    // protocols.  The kernel uses this value as the socket() FILTER, so it is
    // what separates the project's frames from the IPv6 noise the guest emits
    // on its own.
    // >>> CONFIRM with Artur and André before the presentation. <<<
    static const unsigned short PROTOCOL_NUMBER = 0x88B5;

    // The NIC's buffer pool.  BUFFER_SIZE = SEND + RECEIVE.
    static const unsigned int SEND_BUFFERS = 16;
    static const unsigned int RECEIVE_BUFFERS = 16;

    static const bool debugged = false;
};

#endif // LIBVCOMM_TRAITS_H
