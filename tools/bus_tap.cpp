// =============================================================================
// bus-tap — records the virtual bus WITHOUT ANY PRIVILEGE.
//
// WHY THIS EXISTS.  The assignment requires that `make` alone compile, run
// every test and print the average latency.  The obvious way to time frames is
// dumpcap/tshark, and that is what this project did first — but dumpcap is
// installed 0754 root:wireshark on Debian and Ubuntu.  A grader who is not in
// the `wireshark` group cannot even EXECUTE it, so `make` would stop before it
// ever printed a latency.  Making the delivery depend on the evaluator running
// `sudo dpkg-reconfigure wireshark-common` first is not acceptable.
//
// THE OBSERVATION THAT REMOVES THE DEPENDENCY.  QEMU's `-netdev socket,mcast=`
// bus is not a kernel device at all: it is an ordinary IPv4 UDP multicast
// group, and each datagram carries EXACTLY ONE guest Ethernet frame with no
// framing of its own (verified against the pcap: the first payload byte is the
// destination MAC).  So the bus can be observed by any process willing to join
// the group — an unprivileged socket(AF_INET, SOCK_DGRAM) — instead of by a
// packet sniffer that needs CAP_NET_RAW.  We are not sniffing an interface; we
// are simply another listener on a broadcast medium, which is what every
// vehicle already is.
//
// This is also a BETTER observer than the sniffer it replaces.  dumpcap sees
// the guest frame wrapped in a host UDP datagram, so Wireshark shows "UDP" and
// the Ethernet header we care about is buried in a payload.  Here the datagram
// IS the frame, so the .pcap written below is LINKTYPE_ETHERNET and Wireshark
// dissects our EtherType 0x88B5 frames directly.
//
// WHAT IT DOES NOT PROVE.  Same limit as any capture, and the guide is explicit
// about it: observing a frame on the bus does not prove a guest accepted it.
// Correctness is proven by the vehicles' own verdicts in build/logs/; this
// gives format and timing.
//
// Two outputs, on purpose:
//   <out>.frames   one line per frame, "<epoch>\t<hex>" — byte for byte the
//                  format `tshark -T fields -e frame.time_epoch -e udp.payload`
//                  emits, so the analysis scripts consume either source
//                  unchanged.
//   <out>.pcap     the same frames as a classic pcap, for opening in Wireshark
//                  during the presentation.
// =============================================================================
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace
{

volatile sig_atomic_t g_running = 1;

void on_signal(int) { g_running = 0; }

// Ethernet frames only; the bus never carries anything larger.
const unsigned int MAX_FRAME = 2048;

const unsigned int PCAP_SNAPLEN = 65535;
const unsigned int LINKTYPE_ETHERNET = 1;

void write_u32(std::FILE * f, unsigned int v) { std::fwrite(&v, 4, 1, f); }
void write_u16(std::FILE * f, unsigned short v) { std::fwrite(&v, 2, 1, f); }

// Classic libpcap header, host byte order — the magic is what tells the reader
// which endianness the file is in, so writing natively is correct and portable.
void pcap_write_header(std::FILE * f)
{
    write_u32(f, 0xa1b2c3d4u); // microsecond-resolution magic: read by every
                               // version of Wireshark, tcpdump and scapy
    write_u16(f, 2);
    write_u16(f, 4);
    write_u32(f, 0); // thiszone: timestamps are UTC already
    write_u32(f, 0); // sigfigs
    write_u32(f, PCAP_SNAPLEN);
    write_u32(f, LINKTYPE_ETHERNET);
}

void pcap_write_packet(std::FILE * f, const struct timespec & ts,
                       const unsigned char * data, unsigned int len)
{
    write_u32(f, static_cast<unsigned int>(ts.tv_sec));
    write_u32(f, static_cast<unsigned int>(ts.tv_nsec / 1000));
    write_u32(f, len);
    write_u32(f, len);
    std::fwrite(data, 1, len, f);
}

void usage(const char * argv0)
{
    std::fprintf(stderr,
                 "usage: %s --mcast <addr:port> [--localaddr <addr>] "
                 "--out <prefix>\n",
                 argv0);
}

} // namespace

int main(int argc, char ** argv)
{
    const char * mcast = std::getenv("SO2_MCAST");
    const char * localaddr = std::getenv("SO2_LOCALADDR");
    const char * out = 0;

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--mcast") && i + 1 < argc)
            mcast = argv[++i];
        else if (!std::strcmp(argv[i], "--localaddr") && i + 1 < argc)
            localaddr = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!mcast || !out) {
        usage(argv[0]);
        return 2;
    }
    if (!localaddr || !*localaddr)
        localaddr = "127.0.0.1";

    // ---- split "addr:port" -------------------------------------------------
    char group[64];
    std::strncpy(group, mcast, sizeof(group) - 1);
    group[sizeof(group) - 1] = 0;
    char * colon = std::strrchr(group, ':');
    if (!colon) {
        std::fprintf(stderr, "bus-tap: --mcast wants addr:port, got '%s'\n",
                     mcast);
        return 2;
    }
    *colon = 0;
    const int port = std::atoi(colon + 1);

    struct in_addr group_addr;
    struct in_addr iface_addr;
    if (inet_pton(AF_INET, group, &group_addr) != 1) {
        std::fprintf(stderr, "bus-tap: '%s' is not an IPv4 address\n", group);
        return 2;
    }
    if (inet_pton(AF_INET, localaddr, &iface_addr) != 1) {
        std::fprintf(stderr, "bus-tap: '%s' is not an IPv4 address\n",
                     localaddr);
        return 2;
    }

    // ---- the socket --------------------------------------------------------
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::perror("bus-tap: socket");
        return 1;
    }

    // Mandatory here, and not for the usual reason: the QEMU processes are
    // already bound to this very port.  On a multicast group SO_REUSEADDR is
    // what lets several sockets share it and EACH receive a copy — which is
    // exactly how the vehicles hear one another, and how we hear all of them
    // without taking anything away from anyone.
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        std::perror("bus-tap: SO_REUSEADDR");
        close(fd);
        return 1;
    }

    // The fleet is a burst: five vehicles announcing readiness at once.  The
    // default receive buffer is comfortable at this scale, but a dropped
    // datagram here would silently become a missing latency sample, so ask for
    // room.  Best effort — a failure is not fatal.
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // Kernel timestamps, taken when the datagram is received rather than when
    // this process gets around to looking at it.  That is the same class of
    // timestamp dumpcap reports, and it keeps scheduling delay in this process
    // out of the measurement.
    int stamp = 1;
    const bool have_stamps =
        setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &stamp, sizeof(stamp)) == 0;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    // Bind to the GROUP address, not INADDR_ANY: it is what QEMU does, so the
    // two binds are compatible, and it keeps unrelated unicast traffic that
    // happens to use this port out of the capture.
    addr.sin_addr = group_addr;

    if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::perror("bus-tap: bind");
        close(fd);
        return 1;
    }

    // Join on the SAME interface the vehicles use.  run-fleet.sh pins the bus
    // to loopback with QEMU's localaddr=, so joining on 127.0.0.1 is what puts
    // us on their medium; joining on the default interface would leave us
    // listening to an empty group.
    struct ip_mreq mreq;
    std::memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr = group_addr;
    mreq.imr_interface = iface_addr;
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        std::perror("bus-tap: IP_ADD_MEMBERSHIP");
        std::fprintf(stderr, "bus-tap: could not join %s on %s\n", group,
                     localaddr);
        close(fd);
        return 1;
    }

    // ---- the outputs -------------------------------------------------------
    char path_frames[512];
    char path_pcap[512];
    std::snprintf(path_frames, sizeof(path_frames), "%s.frames", out);
    std::snprintf(path_pcap, sizeof(path_pcap), "%s.pcap", out);

    std::FILE * frames = std::fopen(path_frames, "w");
    if (!frames) {
        std::perror("bus-tap: fopen frames");
        close(fd);
        return 1;
    }
    std::FILE * pcap = std::fopen(path_pcap, "wb");
    if (!pcap) {
        std::perror("bus-tap: fopen pcap");
        std::fclose(frames);
        close(fd);
        return 1;
    }
    pcap_write_header(pcap);
    std::fflush(pcap);

    // Told the parent we are live only after the group is joined and the files
    // exist: run-fleet.sh waits for this line before booting the first vehicle,
    // because a tap that starts late misses precisely the frames you wanted.
    std::fprintf(stderr, "bus-tap: listening on %s:%d via %s -> %s\n", group,
                 port, localaddr, path_frames);
    std::fflush(stderr);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, 0);  // no SA_RESTART: the handler must break recvmsg
    sigaction(SIGTERM, &sa, 0);

    static const char HEX[] = "0123456789abcdef";
    unsigned char buf[MAX_FRAME];
    char line[2 * MAX_FRAME + 64];
    unsigned long long count = 0;

    while (g_running) {
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);

        char control[CMSG_SPACE(sizeof(struct timespec))];
        struct msghdr msg;
        std::memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        ssize_t n = recvmsg(fd, &msg, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue; // a signal: either shutdown, or simply spurious
            std::perror("bus-tap: recvmsg");
            break;
        }
        if (n == 0)
            continue;

        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        bool stamped = false;
        if (have_stamps) {
            for (struct cmsghdr * c = CMSG_FIRSTHDR(&msg); c;
                 c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level == SOL_SOCKET &&
                    c->cmsg_type == SO_TIMESTAMPNS) {
                    std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
                    stamped = true;
                    break;
                }
            }
        }
        if (!stamped)
            clock_gettime(CLOCK_REALTIME, &ts); // fallback, marginally later

        const unsigned int len = static_cast<unsigned int>(n);

        int off = std::snprintf(line, sizeof(line), "%lld.%09ld\t",
                                static_cast<long long>(ts.tv_sec), ts.tv_nsec);
        for (unsigned int i = 0; i < len; i++) {
            line[off++] = HEX[buf[i] >> 4];
            line[off++] = HEX[buf[i] & 0x0f];
        }
        line[off++] = '\n';
        std::fwrite(line, 1, static_cast<size_t>(off), frames);

        pcap_write_packet(pcap, ts, buf, len);

        // Flushed per frame.  The fleet stops this process with SIGINT, and
        // buffered tail frames would be exactly the last measurements.  At a
        // few hundred frames the cost does not register.
        std::fflush(frames);
        std::fflush(pcap);
        count++;
    }

    std::fclose(frames);
    std::fclose(pcap);
    close(fd);

    std::fprintf(stderr, "bus-tap: %llu frames -> %s, %s\n", count, path_frames,
                 path_pcap);
    return 0;
}
