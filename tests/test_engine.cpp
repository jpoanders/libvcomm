#include "check.h"

#include "engine/raw_socket_engine.h"
#include "traits.h"

#include <sys/socket.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const unsigned short PROT = Traits<Ethernet>::PROTOCOL_NUMBER;

// signal counter chains on top of the engine SIGIO handler
volatile sig_atomic_t g_signals = 0;
void (*g_engine_handler)(int) = 0;

extern "C" void counting_handler(int signo)
{
    g_signals = g_signals + 1;
    if (g_engine_handler)
        g_engine_handler(signo);
}

// concrete engine subclass for testing
class Probe : public Raw_Socket_Engine
{
public:
    Probe(const char * iface, Protocol prot)
        : Raw_Socket_Engine(iface, prot), frames(0), oversize(0)
    {
        std::memset(last, 0, sizeof(last));
    }

    using Raw_Socket_Engine::engine_address;
    using Raw_Socket_Engine::engine_rx_error;
    using Raw_Socket_Engine::engine_rx_errors;
    using Raw_Socket_Engine::engine_send;
    using Raw_Socket_Engine::engine_start;
    using Raw_Socket_Engine::engine_stop;
    using Raw_Socket_Engine::engine_valid;

    volatile sig_atomic_t frames;
    volatile sig_atomic_t oversize;
    unsigned char last[Ethernet::HEADER_SIZE + 64];

protected:
    void handle(Ethernet::Frame * frame, unsigned int size) override
    {
        frames = frames + 1;
        if (size > sizeof(last))
            oversize = oversize + 1;
        else
            std::memcpy(last, frame, size);
    }
};

bool enabled(const char * name)
{
    const char * v = std::getenv(name);
    return v && v[0] && std::strcmp(v, "0") != 0;
}

bool has_cap_net_raw()
{
    int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0)
        return false;
    ::close(fd);
    return true;
}

// reads MAC via sysfs
bool mac_from_sysfs(const char * iface, unsigned char out[6])
{
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    std::FILE * f = std::fopen(path, "r");
    if (!f)
        return false;
    unsigned int b[6];
    int n = std::fscanf(f, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3],
                        &b[4], &b[5]);
    std::fclose(f);
    if (n != 6)
        return false;
    for (int i = 0; i < 6; i++)
        out[i] = static_cast<unsigned char>(b[i]);
    return true;
}

void build_frame(Ethernet::Frame * f, const Ethernet::Address & src,
                 unsigned int payload)
{
    // value-init instead of memset
    *f = Ethernet::Frame{};
    f->dst = Ethernet::BROADCAST;
    f->src = src;
    f->prot = htons(PROT);
    for (unsigned int i = 0; i < payload; i++)
        f->data[i] = static_cast<unsigned char>(i + 1);
}

// =============================================================================
// Level 0: failure path (no privileges required)
// =============================================================================
void level_0()
{
    std::printf("\n== level 0: failure path (no privileges) ==\n");

    Probe bad("vcomm-nothing", PROT);

    CHECK(bad.engine_valid() == false);
    CHECK(bad.engine_start() == false);
    CHECK(bad.engine_rx_errors() == 0);

    // stop() must be idempotent on an invalid engine.
    bad.engine_stop();
    bad.engine_stop();
    CHECK(bad.engine_start() == false);

    // Destructor on an invalid engine must not crash.
    {
        Probe tmp("vcomm-nothing", PROT);
        CHECK(tmp.engine_valid() == false);
    }
    CHECK(true); // reached here = destructor safe
}

// =============================================================================
// Level 1: real socket (requires CAP_NET_RAW)
// =============================================================================
void level_1(const char * iface)
{
    std::printf("\n== level 1: socket on '%s' ==\n", iface);

    Probe p(iface, PROT);
    CHECK(p.engine_valid() == true);
    if (!p.engine_valid()) {
        std::printf("  (invalid Engine, skipping level 1)\n");
        return;
    }

    // ---- MAC: cross-check with sysfs ----------------------------------------
    unsigned char sysfs[6];
    if (mac_from_sysfs(iface, sysfs))
        CHECK(std::memcmp(p.engine_address().bytes(), sysfs, 6) == 0);
    else
        std::printf("  (sysfs unavailable; MAC check skipped)\n");

    // ---- SIGIO disposition --------------------------------------------------
    struct sigaction cur;
    std::memset(&cur, 0, sizeof(cur));
    CHECK(::sigaction(SIGIO, NULL, &cur) == 0);
    CHECK(cur.sa_handler != SIG_DFL);
    CHECK((cur.sa_flags & SA_RESTART) == 0); // intentionally no SA_RESTART

    // Chain signal counter on top of the Engine's handler.
    g_engine_handler = cur.sa_handler;
    struct sigaction mine = cur;
    mine.sa_handler = counting_handler;
    CHECK(::sigaction(SIGIO, &mine, NULL) == 0);

    CHECK(p.engine_start() == true);

    const unsigned int PAYLOAD = 32;
    const unsigned int SIZE = Ethernet::HEADER_SIZE + PAYLOAD;
    Ethernet::Frame f;
    build_frame(&f, p.engine_address(), PAYLOAD);

    sigset_t just_sigio, previous;
    sigemptyset(&just_sigio);
    sigaddset(&just_sigio, SIGIO);

    // ---- Single frame roundtrip ----
    {
        CHECK(::sigprocmask(SIG_BLOCK, &just_sigio, &previous) == 0);

        int sent = p.engine_send(&f, SIZE);
        CHECK(sent == static_cast<int>(SIZE));
        CHECK(p.frames == 0); // blocked, not yet delivered

        CHECK(::sigprocmask(SIG_SETMASK, &previous, NULL) == 0);

        CHECK(p.frames == 1); // PACKET_OUTGOING filtered by drain
        CHECK(p.oversize == 0);
        CHECK(std::memcmp(p.last, &f, SIZE) == 0);
    }

    // ---- Signal coalescing: N frames, one signal ----
    const int N = 50;
    {
        sig_atomic_t f0 = p.frames;
        sig_atomic_t s0 = g_signals;

        CHECK(::sigprocmask(SIG_BLOCK, &just_sigio, &previous) == 0);
        int sent_count = 0;
        for (int i = 0; i < N; i++)
            if (p.engine_send(&f, SIZE) == static_cast<int>(SIZE))
                sent_count++;
        CHECK(sent_count == N);
        CHECK(::sigprocmask(SIG_SETMASK, &previous, NULL) == 0);

        CHECK(p.frames - f0 == N);
        CHECK(g_signals - s0 == 1); // all N drained from one signal
        std::printf("     -> %d frames delivered by %d signal(s)\n",
                    static_cast<int>(p.frames - f0),
                    static_cast<int>(g_signals - s0));
    }

    // ---- stop/re-arm: silences notification, preserves data ----
    {
        p.engine_stop();
        sig_atomic_t f1 = p.frames;
        sig_atomic_t s1 = g_signals;

        for (int i = 0; i < N; i++)
            p.engine_send(&f, SIZE);

        CHECK(p.frames == f1); // no signal, no delivery
        CHECK(g_signals == s1);

        p.engine_stop(); // idempotent
        CHECK(p.frames == f1);

        // drain pulls the n queued frames plus the new one
        CHECK(p.engine_start() == true);
        CHECK(::sigprocmask(SIG_BLOCK, &just_sigio, &previous) == 0);
        CHECK(p.engine_send(&f, SIZE) == static_cast<int>(SIZE));
        CHECK(::sigprocmask(SIG_SETMASK, &previous, NULL) == 0);
        CHECK(p.frames - f1 == N + 1);
    }

    // rx error counter
    CHECK(p.engine_rx_errors() == 0);
    if (p.engine_rx_errors() != 0)
        std::printf("     -> last RX errno: %d (%s)\n", p.engine_rx_error(),
                    std::strerror(p.engine_rx_error()));

    p.engine_stop();

    // restore original SIGIO disposition.
    ::sigaction(SIGIO, &cur, NULL);
    g_engine_handler = 0;
}

// =============================================================================
// Level 1-error: RX error detection (orchestrated by external veth script)
// =============================================================================
void level_error(const char * iface)
{
    std::printf("\n== level 1-E: RX error on '%s' ==\n", iface);

    Probe p(iface, PROT);
    CHECK(p.engine_valid() == true);
    if (!p.engine_valid())
        return;

    CHECK(p.engine_start() == true);
    CHECK(p.engine_rx_errors() == 0);

    // Synchronization point for the orchestration script
    std::printf("READY-FOR-ERROR\n");
    std::fflush(stdout);

    // poll for up to 10s. nanosleep may return EINTR (no SA_RESTART)
    for (int i = 0; i < 1000 && p.engine_rx_errors() == 0; i++) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000 * 1000; // 10 ms
        ::nanosleep(&ts, NULL);
    }

    CHECK(p.engine_rx_errors() > 0);
    if (p.engine_rx_errors() > 0) {
        std::printf("     -> errno recorded: %d (%s)\n", p.engine_rx_error(),
                    std::strerror(p.engine_rx_error()));
        CHECK(p.engine_rx_error() == ENETDOWN);
    }

    p.engine_stop();
}

} // namespace

int main()
{
    std::printf("== engine ==\n");

    const char * env_iface = std::getenv("VCOMM_TEST_IFACE");
    const char * iface = env_iface ? env_iface : "lo";

    if (enabled("VCOMM_ERROR_TEST")) {
        if (!has_cap_net_raw()) {
            ::test::report(false, "CAP_NET_RAW (VCOMM_ERROR_TEST requires it)",
                           __FILE__, __LINE__);
            return ::test::summary("engine/error");
        }
        level_error(iface);
        return ::test::summary("engine/error");
    }

    level_0();

    if (has_cap_net_raw()) {
        level_1(iface);
    } else {
        std::printf("\n== level 1: SKIPPED (no CAP_NET_RAW) ==\n");
        std::printf("   $ sudo setcap cap_net_raw+ep build/test-engine\n");
        std::printf("   (repeat after every relink)\n");

        if (enabled("VCOMM_REQUIRE_RAW"))
            ::test::report(false,
                           "level 1 executed (VCOMM_REQUIRE_RAW=1 requires it)",
                           __FILE__, __LINE__);
    }

    return ::test::summary("engine");
}
