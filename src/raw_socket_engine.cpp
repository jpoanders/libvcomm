#include "../include/engine/raw_socket_engine.h"

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <cstring>
#include <cerrno>

Raw_Socket_Engine * Raw_Socket_Engine::_instance = 0;

Raw_Socket_Engine::Raw_Socket_Engine(const char * iface, Protocol prot)
    : _sockfd(-1), _ifindex(0), _address(), _protocol(prot), _armed(0),
      _rx_error(0), _rx_errors(0)
{
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
    _address = Address(
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

    _instance = this;
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

Raw_Socket_Engine::~Raw_Socket_Engine()
{
    engine_stop();
    _instance = 0;
    if (_sockfd >= 0)
        ::close(_sockfd);
}

int Raw_Socket_Engine::engine_send(const Ethernet::Frame * frame,
                                   unsigned int size)
{
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

void Raw_Socket_Engine::signal_handler(int signo)
{
    (void)signo;
    int saved_errno = errno;
    if (_instance && _instance->_armed)
        _instance->drain();
    errno = saved_errno;
}

void Raw_Socket_Engine::drain()
{
    Ethernet::Frame frame;
    struct sockaddr_ll from;
    while (_armed) {
        socklen_t len = sizeof(from);
        ssize_t n =
            ::recvfrom(_sockfd, &frame, sizeof(frame), 0,
                       reinterpret_cast<struct sockaddr *>(&from), &len);
        if (n == 0)
            return;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            // An actual error:
            _rx_error = errno;
            _rx_errors = _rx_errors + 1;
            return;
        }

        if (from.sll_pkttype == PACKET_OUTGOING || n < Ethernet::HEADER_SIZE)
            continue;
        handle(&frame, n);
    }
}

bool Raw_Socket_Engine::engine_start()
{
    if (!engine_valid())
        return false;
    _armed = 1;
    if (::fcntl(_sockfd, F_SETFL, ::fcntl(_sockfd, F_GETFL, 0) | O_ASYNC) < 0) {
        _armed = 0;
        return false;
    }
    return true;
}

void Raw_Socket_Engine::engine_stop()
{
    if (!engine_valid()) {
        _armed = 0;
        return;
    }
    ::fcntl(_sockfd, F_SETFL, ::fcntl(_sockfd, F_GETFL, 0) & ~O_ASYNC);
    _armed = 0;
}
