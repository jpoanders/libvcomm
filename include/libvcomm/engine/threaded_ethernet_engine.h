#ifndef LIBVCOMM_THREADED_ETHERNET_ENGINE_H
#define LIBVCOMM_THREADED_ETHERNET_ENGINE_H

#include <atomic>
#include <csignal>
#include <semaphore.h>
#include <thread>
#include "libvcomm/net/ethernet.h"
#include "engine.h"

class ThreadedEthernetEngine : public Engine
{
public:
    ThreadedEthernetEngine(const char * iface, Ethernet::Protocol prot);

    ~ThreadedEthernetEngine();

    ThreadedEthernetEngine(const ThreadedEthernetEngine &) = delete;

    ThreadedEthernetEngine & operator=(const ThreadedEthernetEngine &) = delete;

    bool start();

    void stop();

    int send(const void * data, unsigned int size) override;

    const Ethernet::Address & address() const { return _address; }

    bool valid() const { return _sockfd >= 0; }

    unsigned int engine_rx_errors() const
    {
        return static_cast<unsigned int>(
            _rx_errors.load(std::memory_order_relaxed));
    }

    int engine_rx_error() const
    {
        return static_cast<int>(_rx_error.load(std::memory_order_relaxed));
    }

private:
    static void signal_handler(int);

    void receive_loop();

    int _sockfd;                  // -1 while invalid
    unsigned int _ifindex;        // if_nametoindex("eth0")
    Ethernet::Address _address;   // eth0's real MAC (SIOCGIFHWADDR)
    Ethernet::Protocol _protocol; // EtherType, in HOST order
    std::thread _receiver;
    std::atomic<bool> _armed;
    std::atomic<int> _rx_error;
    std::atomic<unsigned int> _rx_errors;
    sem_t _sem;
};

#endif // LIBVCOMM_RAW_SOCKET_ENGINE_H
