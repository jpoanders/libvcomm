#ifndef LIBVCOMM_RAW_SOCKET_ENGINE_H
#define LIBVCOMM_RAW_SOCKET_ENGINE_H

#include <csignal>
#include "../ethernet.h"

class Raw_Socket_Engine
{
protected:
    typedef Ethernet::Address Address;
    typedef Ethernet::Protocol Protocol;

    Raw_Socket_Engine(const char * iface, Protocol prot);

    virtual ~Raw_Socket_Engine();

    Raw_Socket_Engine(const Raw_Socket_Engine &) = delete;
    Raw_Socket_Engine & operator=(const Raw_Socket_Engine &) = delete;

    int engine_send(const Ethernet::Frame * frame, unsigned int size);

    bool engine_start();

    void engine_stop();

    const Address & engine_address() const { return _address; }

    bool engine_valid() const { return _sockfd >= 0; }

    unsigned int engine_rx_errors() const
    {
        return static_cast<unsigned int>(_rx_errors);
    }
    int engine_rx_error() const { return static_cast<int>(_rx_error); }

    virtual void handle(Ethernet::Frame * frame, unsigned int size) = 0;

private:

    static void signal_handler(int signo);
    static Raw_Socket_Engine * _instance;

    void drain();

    int _sockfd;                  // -1 while invalid
    unsigned int _ifindex;        // if_nametoindex("eth0")
    Address _address;             // eth0's real MAC (SIOCGIFHWADDR)
    Protocol _protocol;           // EtherType, in HOST order
    volatile sig_atomic_t _armed; // has engine_start() run?
    volatile sig_atomic_t _rx_error;
    volatile sig_atomic_t _rx_errors;

};

#endif // LIBVCOMM_RAW_SOCKET_ENGINE_H
