#ifndef LIBVCOMM_TRAITS_H
#define LIBVCOMM_TRAITS_H

template <typename T> struct Traits
{
    static const bool debugged = false;
};

class Ethernet;

template <> struct Traits<Ethernet>
{
    static constexpr const char * INTERFACE = "eth0";

    static const unsigned short PROTOCOL_NUMBER = 0x88B5;

    // The NIC's buffer pool.  BUFFER_SIZE = SEND + RECEIVE.
    static const unsigned int SEND_BUFFERS = 16;
    static const unsigned int RECEIVE_BUFFERS = 16;

    static const bool debugged = false;
};

#endif // LIBVCOMM_TRAITS_H
