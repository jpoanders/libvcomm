#include "../include/engine/threaded_ethernet_engine.h"

#include <atomic>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

ThreadedEthernetEngine::ThreadedEthernetEngine(const char * iface,
                                               Ethernet::Protocol prot)
    : _sockfd(-1), _ifindex(0), _address(), _protocol(prot), _receiver(),
      _armed(false), _rx_error(0), _rx_errors(0)
{
    if (sem_init(&_sem, 0, 0) < 0) {
        return;
    }

    _sockfd = ::socket(AF_PACKET, SOCK_RAW, htons(_protocol));
    if (_sockfd < 0) {
        return;
    }
    _ifindex = ::if_nametoindex(iface);
    if (_ifindex == 0) {
        ::close(_sockfd);
        _sockfd = -1;
        return;
    }
    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (::ioctl(_sockfd, SIOCGIFHWADDR, &ifr) < 0) {
        ::close(_sockfd);
        _sockfd = -1;
        return;
    }
    _address = Ethernet::Address(
        reinterpret_cast<const unsigned char *>(ifr.ifr_hwaddr.sa_data));

    struct sockaddr_ll sll;
    std::memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(_protocol);
    sll.sll_ifindex = _ifindex;

    if (::bind(_sockfd, reinterpret_cast<const struct sockaddr *>(&sll),
               sizeof(sll)) < 0) {
        ::close(_sockfd);
        _sockfd = -1;
        return;
    }

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (::sigaction(SIGIO, &sa, NULL) < 0) {
        ::close(_sockfd);
        _sockfd = -1;
        return;
    }
    if (::fcntl(_sockfd, F_SETOWN, getpid()) < 0) {
        ::close(_sockfd);
        _sockfd = -1;
        return;
    }
    if (::fcntl(_sockfd, F_SETFL, fcntl(_sockfd, F_GETFL, 0) | O_NONBLOCK) <
        0) {
        ::close(_sockfd);
        _sockfd = -1;
        return;
    }
}

ThreadedEthernetEngine::~ThreadedEthernetEngine()
{
    if (_armed.load(std::memory_order_relaxed))
        stop();
    if (_sockfd >= 0)
        ::close(_sockfd);
    sem_destroy(&_sem);
}

bool ThreadedEthernetEngine::start()
{
    // check this
    if (!valid()) {
        return false;
    }

    if (_armed.exchange(true))
        return false;

    if (::fcntl(_sockfd, F_SETFL, ::fcntl(_sockfd, F_GETFL, 0) | O_ASYNC) < 0) {
        _armed.exchange(false);
        return false;
    }
    _armed.exchange(true);
    // what if start() is called AFTER a stop() and the counter is different
    // than 0? Should this verification actually be made? Think about the stop()
    // implementation before removing this:
    int sval;
    if (sem_getvalue(&_sem, &sval) < 0) {
        _armed.exchange(false);
        return false;
    }

    while (sval != 0) {
        sem_wait(&_sem);
    }
    _receiver = std::thread(&ThreadedEthernetEngine::receive_loop, this);
    return true;
}

void ThreadedEthernetEngine::stop()
{
    // if (!valid()) {
    //     _armed.exchange(false);
    // } else {
    //     ::fcntl(_sockfd, F_SETFL, ::fcntl(_sockfd, F_GETFL, 0) & ~O_ASYNC);
    //     _armed.exchange(false);
    // }

    if (valid()) {
        ::fcntl(_sockfd, F_SETFL, ::fcntl(_sockfd, F_GETFL, 0) & ~O_ASYNC);
    }
    _armed.exchange(false);

    if (_receiver.joinable()) {
        sem_post(&_sem);
        _receiver.join();
    }
}

int ThreadedEthernetEngine::send(const void * data, unsigned int size)
{
    const Ethernet::Frame * frame =
        reinterpret_cast<const Ethernet::Frame *>(data);
    struct sockaddr_ll sll;
    std::memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(_protocol);
    sll.sll_ifindex = _ifindex;
    sll.sll_halen = ETH_ALEN;
    std::memcpy(sll.sll_addr, frame->dst.bytes(), ETH_ALEN);

    return ::sendto(_sockfd, frame, size, 0,
                    reinterpret_cast<const struct sockaddr *>(&sll),
                    sizeof(sll));
}

void ThreadedEthernetEngine::signal_handler(int)
{
    sem_post(&_sem);
}

void ThreadedEthernetEngine::receive_loop()
{
    Ethernet::Frame frame;
    struct sockaddr_ll from;
    while (_armed.load(std::memory_order_relaxed)) {
        while (sem_wait(&_sem) == -1 && errno == EINTR)
            ;
        while (_armed.load(std::memory_order_relaxed)) {
            socklen_t len = sizeof(from);
            ssize_t n =
                ::recvfrom(_sockfd, &frame, sizeof(frame), 0,
                           reinterpret_cast<struct sockaddr *>(&from), &len);
            if (n == 0)
                break;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                    continue;
                // An actual error:
                _rx_error.store(errno, std::memory_order_relaxed);
                _rx_errors.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            if (from.sll_pkttype == PACKET_OUTGOING ||
                n < Ethernet::HEADER_SIZE)
                continue;

            receive(reinterpret_cast<void *>(&frame), n);
        }
    }
}
