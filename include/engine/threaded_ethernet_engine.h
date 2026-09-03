#ifndef LIBVCOMM_THREADED_ETHERNET_ENGINE_H
#define LIBVCOMM_THREADED_ETHERNET_ENGINE_H

#include <csignal>
#include "../ethernet.h"
#include "engine.h"

class ThreadedEthernetEngine : public Engine
{
public:
    ThreadedEthernetEngine(const char * iface, Ethernet::Protocol prot);

    ~ThreadedEthernetEngine() = default;

    // ThreadedEthernetEngine(const ThreadedEthernetEngine &) = delete;
    // ThreadedEthernetEngine & operator=(const ThreadedEthernetEngine &) =
    // delete;

    int send(const void * frame, unsigned int size) override;

    const Ethernet::Address & address() const { return _address; }

    bool valid() const { return _sockfd >= 0; }

    unsigned int rx_errors() const
    {
        return static_cast<unsigned int>(_rx_errors);
    }
    int rx_error() const { return static_cast<int>(_rx_error); }

private:
    static void signal_handler();

    int _sockfd;                  // -1 while invalid
    unsigned int _ifindex;        // if_nametoindex("eth0")
    Ethernet::Address _address;   // eth0's real MAC (SIOCGIFHWADDR)
    Ethernet::Protocol _protocol; // EtherType, in HOST order
    volatile sig_atomic_t _armed; // has engine_start() run?
    volatile sig_atomic_t _rx_error;
    volatile sig_atomic_t _rx_errors;
};

#endif // LIBVCOMM_RAW_SOCKET_ENGINE_H
