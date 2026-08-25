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

// -----------------------------------------------------------------------------
// Counting SIGNALS, which is a different thing from counting frames.
//
// The Engine's handler is private and cannot be instrumented from the inside.
// What can be done is CHAINING: after the Engine's constructor has installed the
// SIGIO disposition, the test reads that disposition with sigaction(NULL, &cur),
// keeps the pointer, and installs a handler of its own that counts and forwards.
// The library does not change a single line.
// -----------------------------------------------------------------------------
volatile sig_atomic_t g_signals = 0;
void (*g_engine_handler)(int) = 0;

extern "C" void counting_handler(int signo)
{
    g_signals = g_signals + 1;
    if (g_engine_handler)
        g_engine_handler(signo);
}

// -----------------------------------------------------------------------------
// The probe.  Raw_Socket_Engine is not instantiable: everything is protected and
// handle() is pure virtual.  Every Engine test goes through a derived class like
// this one.
// -----------------------------------------------------------------------------
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

    volatile sig_atomic_t frames;   // how many times handle() was called
    volatile sig_atomic_t oversize; // frame larger than the probe's buffer
    unsigned char last[Ethernet::HEADER_SIZE + 64];

protected:
    // RUNS INSIDE THE SIGNAL HANDLER.  No printf, no allocation: only
    // sig_atomic_t and memcpy.  If this function were hard to write under that
    // rule, the problem would be in the contract, not in the test.
    void handle(Ethernet::Frame * frame, unsigned int size) override
    {
        frames = frames + 1;
        if (size > sizeof(last))
            oversize = oversize + 1;
        else
            std::memcpy(last, frame, size);
    }
};

// An environment variable is on when it exists, is non-empty and is not "0".
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

// The MAC through the sysfs path — independent of the SIOCGIFHWADDR the Engine
// uses.  Comparing the results of two paths is worth more than comparing against
// a constant.
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
    // No memset() here: Ethernet::Frame is NOT trivially-constructible
    // (Ethernet::Address has a constructor), and g++ warns with
    // -Wclass-memaccess.  Value-initializing the aggregate zeroes everything
    // through the proper path — the same reason a `static Ethernet::Frame`
    // inside drain() would generate an initialization guard.
    *f = Ethernet::Frame{};
    f->dst = Ethernet::BROADCAST;
    f->src = src;
    f->prot = htons(PROT); // host order inside the lib, network order on the wire
    for (unsigned int i = 0; i < payload; i++)
        f->data[i] = static_cast<unsigned char>(i + 1);
}

// =============================================================================
// LEVEL 0 — no privileges.
//
// The interface does not exist, so the constructor fails WITH or WITHOUT
// CAP_NET_RAW (without privileges it does not even reach if_nametoindex:
// socket() already returns EPERM).  The test is identical on both machines,
// which is what you want from a test.
// =============================================================================
void level_0()
{
    std::printf("\n== level 0: failure path (no privileges) ==\n");

    Probe bad("vcomm-nothing", PROT);

    CHECK(bad.engine_valid() == false);
    CHECK(bad.engine_start() == false);
    CHECK(bad.engine_rx_errors() == 0);

    // Idempotence over an invalid Engine.  It is engine_stop()'s guard that
    // prevents an fcntl(-1, ...) here — without it this silently dirties errno.
    bad.engine_stop();
    bad.engine_stop();
    CHECK(bad.engine_start() == false);

    // Destructor over an invalid Engine: construct and die within the scope.  If
    // the destructor called close(-1) or fcntl(-1, ...) without a guard, this is
    // where it would show up.
    {
        Probe tmp("vcomm-nothing", PROT);
        CHECK(tmp.engine_valid() == false);
    }
    CHECK(true); // getting this far == the destructor did not take the process down
}

// =============================================================================
// LEVEL 1 — a real socket.  Needs CAP_NET_RAW.
// =============================================================================
void level_1(const char * iface)
{
    std::printf("\n== level 1: socket on '%s' ==\n", iface);

    Probe p(iface, PROT);
    CHECK(p.engine_valid() == true);
    if (!p.engine_valid()) {
        std::printf("  (without a valid Engine, the rest of level 1 makes no "
                    "sense)\n");
        return;
    }

    // ---- constructor: the MAC came from the kernel, not from a constant -----
    unsigned char sysfs[6];
    if (mac_from_sysfs(iface, sysfs))
        CHECK(std::memcmp(p.engine_address().bytes(), sysfs, 6) == 0);
    else
        std::printf("  (sysfs unavailable; MAC comparison skipped)\n");

    // ---- the constructor installed the SIGIO disposition --------------------
    struct sigaction cur;
    std::memset(&cur, 0, sizeof(cur));
    CHECK(::sigaction(SIGIO, NULL, &cur) == 0);
    CHECK(cur.sa_handler != SIG_DFL); // SIGIO's default action is to KILL the process
    CHECK((cur.sa_flags & SA_RESTART) == 0); // design decision: no SA_RESTART

    // Chains the signal counter on top of the Engine's handler.
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

    // ---- round trip of ONE frame -------------------------------------------
    {
        CHECK(::sigprocmask(SIG_BLOCK, &just_sigio, &previous) == 0);

        int sent = p.engine_send(&f, SIZE);
        CHECK(sent == static_cast<int>(SIZE));
        CHECK(p.frames == 0); // blocked: nothing has been delivered yet

        CHECK(::sigprocmask(SIG_SETMASK, &previous, NULL) == 0);

        // Exactly 1, not 2: the PACKET_OUTGOING copy was filtered in drain().
        CHECK(p.frames == 1);
        CHECK(p.oversize == 0);
        CHECK(std::memcmp(p.last, &f, SIZE) == 0);
    }

    // ---- coalescing: N frames, ONE signal -----------------------------------
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

        // drain() emptied the whole queue from a single signal.  If frames < N,
        // the loop is leaving early.  If signals > 1, the test did not prove
        // coalescing (and then the broken thing is the test, not the Engine).
        CHECK(p.frames - f0 == N);
        CHECK(g_signals - s0 == 1);
        std::printf("     -> %d frames delivered by %d signal(s)\n",
                    static_cast<int>(p.frames - f0),
                    static_cast<int>(g_signals - s0));
    }

    // ---- engine_stop(): stops signalling, does NOT discard ------------------
    {
        p.engine_stop();
        sig_atomic_t f1 = p.frames;
        sig_atomic_t s1 = g_signals;

        for (int i = 0; i < N; i++)
            p.engine_send(&f, SIZE);

        // Without O_ASYNC no signal is born, so handle() is not called.
        CHECK(p.frames == f1);
        CHECK(g_signals == s1);

        p.engine_stop(); // idempotent
        CHECK(p.frames == f1);

        // The N frames are still in the kernel's queue.  Re-arming and sending
        // one more makes drain() pull the N parked ones plus the new one: proof
        // that stop silences the notification without losing data.
        CHECK(p.engine_start() == true);
        CHECK(::sigprocmask(SIG_BLOCK, &just_sigio, &previous) == 0);
        CHECK(p.engine_send(&f, SIZE) == static_cast<int>(SIZE));
        CHECK(::sigprocmask(SIG_SETMASK, &previous, NULL) == 0);
        CHECK(p.frames - f1 == N + 1);
    }

    // ---- error diagnostics --------------------------------------------------
    //
    // On a clean path the counter must stay put.  This does NOT prove drain()'s
    // error arm works — it proves it does not fire on its own.
    //
    // The positive proof is manual, because bringing an interface down is not
    // something a test suite does unless told to:
    //
    //   $ sudo ip link set <iface> down    # with the Engine armed
    //   and engine_rx_errors() must go up.
    //
    // Do it on a veth, never on `lo` — bringing loopback down breaks the machine.
    CHECK(p.engine_rx_errors() == 0);
    if (p.engine_rx_errors() != 0)
        std::printf("     -> last RX errno: %d (%s)\n", p.engine_rx_error(),
                    std::strerror(p.engine_rx_error()));

    p.engine_stop();

    // Restores the SIGIO disposition, so as not to leak into another test.
    ::sigaction(SIGIO, &cur, NULL);
    g_engine_handler = 0;
}


void level_error(const char * iface)
{
    std::printf("\n== level 1-E: RX error on '%s' ==\n", iface);

    Probe p(iface, PROT);
    CHECK(p.engine_valid() == true);
    if (!p.engine_valid())
        return;

    CHECK(p.engine_start() == true);
    CHECK(p.engine_rx_errors() == 0);

    // Handshake with the script: only after this line does bringing the
    // interface down test anything.  The fflush is mandatory — the output is
    // redirected to a file, which makes stdout block-buffered.
    std::printf("READY-FOR-ERROR\n");
    std::fflush(stdout);

    // Up to ~10 s waiting for the counter to rise.  nanosleep may return EINTR
    // on every signal that arrives: the Engine installs the handler WITHOUT
    // SA_RESTART, on purpose.  It does not hurt here — the loop just tries
    // again.
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

    // VCOMM_ERROR_TEST=1  -> level 1-E only, orchestrated by the veth script.
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
        std::printf("\n== level 1: SKIPPED — no CAP_NET_RAW ==\n");
        std::printf("   $ sudo setcap cap_net_raw+ep build/test-engine\n");
        std::printf("   (setcap lives on the inode: REPEAT it after every "
                    "relink)\n");

        // VCOMM_REQUIRE_RAW=1 turns the skip into a failure.  It is what `make
        // check` uses: the evaluation target must not go green having skipped
        // the only test that exercises a real socket.
        if (enabled("VCOMM_REQUIRE_RAW"))
            ::test::report(false,
                           "level 1 executed (VCOMM_REQUIRE_RAW=1 requires it)",
                           __FILE__, __LINE__);
    }

    return ::test::summary("engine");
}
