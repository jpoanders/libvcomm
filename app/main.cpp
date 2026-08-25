// =============================================================================
// Stage 1 fleet application.
//
// One binary, installed on every VM.  The starter's /init exports SO2_VM_ID and
// runs /student/app, so the ROLE comes from the id — five vehicles, one image.
//
// WHAT THE TEST PROVES, AND HOW
//
//   VM 1, component 0        the REQUESTER.  Broadcasts REQUEST 0..N-1.
//   VMs 2-5, component 0     the RESPONDERS.  Broadcast a RESPONSE per REQUEST.
//   every other component    listens and counts.
//
// Every component asserts a count it can predict exactly, which is what makes
// the fleet gradeable instead of merely noisy:
//
//   components on VMs 2-5    expect every REQUEST                  (= N)
//   components on VM 1       expect one RESPONSE per remote vehicle (= 4N)
//
// Both numbers are independent of whether the QEMU bus echoes a vehicle's own
// broadcast back to it, because neither counts a kind its own vehicle emits.
// The self-echo filter below removes the rest of the question.
//
// The pairing REQUEST->RESPONSE is also what the host capture times: one clock,
// on the host, for both directions.  See scripts/analyze-capture.sh.
// =============================================================================

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/reboot.h>

#include "libvcomm.h"
#include "fleet_payload.h"

namespace {

// -----------------------------------------------------------------------------
// Configuration.  Defaults are the delivery's; the environment overrides exist
// so the scripts can run a short fleet while debugging without a rebuild.
// -----------------------------------------------------------------------------
const unsigned int MAX_VMS = 16;
const unsigned int MAX_COMPONENTS = 8;

struct Config
{
    unsigned int vms;
    unsigned int components;
    unsigned int warmup;
    unsigned int measured;
    unsigned int interval_ms;
    unsigned int ready_interval_ms;
    unsigned int ready_timeout_ms;
    unsigned int idle_timeout_ms;
    unsigned int hard_deadline_ms;
    bool poweroff;
    const char * iface;

    unsigned int requests() const { return warmup + measured; }
};

// -----------------------------------------------------------------------------
// Where a run parameter comes from, in order: the KERNEL COMMAND LINE, then the
// environment, then the default.
//
// The command line is not a flourish — it is the only channel there is.  The
// starter's /init exports SO2_VM_ID and nothing else, so a shell variable set
// on the host never reaches this process inside the guest.  Without
// `vcomm.*=` parameters, a two-VM debugging run would still expect five
// vehicles and fail every assertion for reasons that have nothing to do with
// the code.  scripts/run-fleet.sh passes them through SO2_APPEND.
//
// The environment is kept as the second source because on the host — where
// tests and the fleet-over-lo run live — there is no command line to speak of.
// -----------------------------------------------------------------------------
bool cmdline_param(const char * key, char * out, unsigned int outlen)
{
    std::FILE * f = std::fopen("/proc/cmdline", "r");
    if (!f)
        return false;

    char line[2048];
    const char * got = std::fgets(line, sizeof(line), f);
    std::fclose(f);
    if (!got)
        return false;

    char needle[64];
    std::snprintf(needle, sizeof(needle), "vcomm.%s=", key);
    const unsigned int nlen = static_cast<unsigned int>(std::strlen(needle));

    for (const char * p = line; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        const char * tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
            p++;

        const unsigned int len = static_cast<unsigned int>(p - tok);
        if (len > nlen && std::strncmp(tok, needle, nlen) == 0) {
            unsigned int n = len - nlen;
            if (n > outlen - 1)
                n = outlen - 1;
            std::memcpy(out, tok + nlen, n);
            out[n] = '\0';
            return n > 0;
        }
    }
    return false;
}

const char * param_str(const char * key, const char * env, char * buf,
                       unsigned int buflen, const char * def)
{
    if (cmdline_param(key, buf, buflen))
        return buf;
    const char * v = std::getenv(env);
    if (v && *v)
        return v;
    return def;
}

int param_int(const char * key, const char * env, int def)
{
    char buf[64];
    if (cmdline_param(key, buf, sizeof(buf)))
        return std::atoi(buf);
    const char * v = std::getenv(env);
    if (!v || !*v)
        return def;
    return std::atoi(v);
}

unsigned int clamp(int v, unsigned int lo, unsigned int hi)
{
    if (v < static_cast<int>(lo))
        return lo;
    if (v > static_cast<int>(hi))
        return hi;
    return static_cast<unsigned int>(v);
}

char g_iface[64];

Config config()
{
    Config c;
    c.vms = clamp(param_int("vms", "VCOMM_VMS", 5), 2, MAX_VMS - 1);
    c.components =
        clamp(param_int("components", "VCOMM_COMPONENTS", 3), 1, MAX_COMPONENTS);
    c.warmup = clamp(param_int("warmup", "VCOMM_WARMUP", 5), 0, 1000);
    c.measured = clamp(param_int("measured", "VCOMM_MEASURED", 20), 1, 10000);
    c.interval_ms =
        clamp(param_int("interval", "VCOMM_INTERVAL_MS", 100), 1, 10000);
    c.ready_interval_ms = clamp(
        param_int("ready_interval", "VCOMM_READY_INTERVAL_MS", 200), 10, 5000);
    c.ready_timeout_ms = clamp(
        param_int("ready_timeout", "VCOMM_READY_TIMEOUT_MS", 30000), 100, 600000);
    c.idle_timeout_ms = clamp(
        param_int("idle_timeout", "VCOMM_IDLE_TIMEOUT_MS", 4000), 100, 600000);
    c.hard_deadline_ms =
        clamp(param_int("deadline", "VCOMM_DEADLINE_MS", 45000), 1000, 600000);
    c.poweroff = param_int("poweroff", "VCOMM_POWEROFF", 1) != 0;

    // Inside the VM this is always eth0, and that is the delivered path.  The
    // override is what lets the whole fleet protocol be exercised on the host
    // over `lo` — which delivers what it transmits, so five processes on one
    // interface see each other exactly as five VMs on one bus do.  Finding a
    // handshake bug there takes seconds; finding it through QEMU takes minutes.
    c.iface = param_str("iface", "VCOMM_IFACE", g_iface, sizeof(g_iface),
                        Traits<Ethernet>::INTERFACE);

    return c;
}

// The port every agent in the fleet shares.  It is the CHANNEL: Communicator
// broadcasts to its own port, so binding the same number is what makes two
// agents peers.  Per-component ports would be the other option and the Protocol
// supports it — tests/test_protocol.cpp exercises exactly that — but Stage 1
// wants everyone on one bus, which is what a radio cell is.
const Vehicle_Protocol::Port FLEET_PORT = 1024;

const char * component_name(unsigned int id)
{
    static const char * names[] = {"sensor", "fuser", "ecu"};
    return (id < sizeof(names) / sizeof(names[0])) ? names[id] : "aux";
}

// -----------------------------------------------------------------------------
// Time.  CLOCK_MONOTONIC, because CLOCK_REALTIME inside a freshly booted VM
// with no RTC discipline is not something to measure intervals against.
// -----------------------------------------------------------------------------
unsigned long now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long>(ts.tv_sec) * 1000UL +
           static_cast<unsigned long>(ts.tv_nsec / 1000000L);
}

// -----------------------------------------------------------------------------
// Deferred log.
//
// The guide is explicit that console output during the measured interval
// changes the timing it is trying to measure — and on a serial console under
// TCG that is not a small effect.  So per-frame lines are kept in memory and
// printed once the traffic is over.  The evidence survives; the interference
// does not.
// -----------------------------------------------------------------------------
class Log
{
public:
    Log() : _n(0), _dropped(0) {}

    __attribute__((format(printf, 2, 3))) void add(const char * fmt, ...)
    {
        if (_n >= MAX_LINES) {
            _dropped++;
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(_line[_n], LINE, fmt, ap);
        va_end(ap);
        _n++;
    }

    void flush() const
    {
        for (unsigned int i = 0; i < _n; i++)
            std::printf("%s\n", _line[i]);
        if (_dropped)
            std::printf("  (%u further log lines not kept)\n", _dropped);
        std::fflush(stdout);
    }

private:
    static const unsigned int MAX_LINES = 400;
    static const unsigned int LINE = 96;
    char _line[MAX_LINES][LINE];
    unsigned int _n;
    unsigned int _dropped;
};

// -----------------------------------------------------------------------------
// The pipe barrier that starts a vehicle's components together.
//
// Without it the requester would have to guess when its own siblings had
// finished attaching.  The guide forbids a fixed sleep as the readiness
// mechanism, and it is right to: under TCG the spread between three forks is
// not something to hard-code.  Remote vehicles announce themselves in band
// (KIND_READY); the local ones are settled here, by the kernel, for free.
//
// Every read/write loops on EINTR because SIGIO fires on this path too.
// -----------------------------------------------------------------------------
bool write_all(int fd, const char * buf, unsigned int n)
{
    while (n) {
        ssize_t w = ::write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        buf += w;
        n -= static_cast<unsigned int>(w);
    }
    return true;
}

bool read_all(int fd, char * buf, unsigned int n)
{
    while (n) {
        ssize_t r = ::read(fd, buf, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false; // a component died before reaching the barrier
        buf += r;
        n -= static_cast<unsigned int>(r);
    }
    return true;
}

// -----------------------------------------------------------------------------
// Are we the guest, or somebody's workstation?
//
// This gate exists because the alternative is unacceptable: run_vehicle() ends
// by powering the machine off, and this same static binary is runnable on the
// host — where a careless `sudo ./build/student-app` would shut down the
// developer's laptop.  The starter boots with `so2.vm_id=<n>` on the kernel
// command line and nothing else on this bench does, so that string is the
// signature of being inside the VM.  Absent it, the poweroff is skipped and
// said so.
// -----------------------------------------------------------------------------
bool inside_starter_vm()
{
    std::FILE * f = std::fopen("/proc/cmdline", "r");
    if (!f)
        return false;

    char line[1024];
    const char * got = std::fgets(line, sizeof(line), f);
    std::fclose(f);

    return got && std::strstr(line, "so2.vm_id=") != 0;
}

// -----------------------------------------------------------------------------
bool send_payload(Vehicle_Communicator & comm, unsigned char kind,
                  unsigned int vm_id, unsigned int comp_id, unsigned short seq,
                  unsigned char flags)
{
    fleet::Payload p;
    fleet::encode(p, kind, static_cast<unsigned char>(vm_id),
                  static_cast<unsigned char>(comp_id), seq, flags);
    Message m(&p, sizeof(p));
    return comm.send(&m);
}

// -----------------------------------------------------------------------------
// One component: one process, one Engine, one stack, one verdict.
// -----------------------------------------------------------------------------
int run_component(unsigned int vm_id, unsigned int comp_id, const Config & cfg,
                  int up_fd, int down_fd)
{
    const char * name = component_name(comp_id);

    const bool is_requester = (vm_id == 1 && comp_id == 0);
    const bool is_responder = (vm_id != 1 && comp_id == 0);

    // Only remote vehicles announce.  A component on VM 1 has no reason to:
    // the requester is its sibling and the barrier already proved it is up.
    // Keeping them quiet also keeps the measured interval free of READY frames.
    const bool announces = (vm_id != 1);

    const unsigned int total = cfg.requests();
    const unsigned char counted_kind =
        (vm_id == 1) ? fleet::KIND_RESPONSE : fleet::KIND_REQUEST;
    const unsigned int expected = (vm_id == 1) ? (cfg.vms - 1) * total : total;
    const unsigned int expected_peers = (cfg.vms - 1) * cfg.components;

    // -------------------------------------------------------------------------
    // The stack.  Each layer receives the one below it already built and never
    // constructs it itself — which is what lets tests/test_protocol.cpp swap
    // the Engine for a loopback and Stage 2 swap it for shared memory.
    // -------------------------------------------------------------------------
    Vehicle_NIC nic(cfg.iface);
    if (!nic.valid()) {
        std::fprintf(stderr,
                     "[vm %u comp %u] FATAL: the NIC did not come up. A raw "
                     "socket needs CAP_NET_RAW on '%s' — inside the VM you are "
                     "root, on the host you are not.\n",
                     vm_id, comp_id, cfg.iface);
        std::printf("RESULT vm=%u comp=%u name=%s counted=0 expected=%u FAIL-NIC\n",
                    vm_id, comp_id, name, expected);
        std::fflush(stdout);
        return 1;
    }

    Vehicle_Protocol protocol(&nic);
    Vehicle_Communicator comm(
        &protocol, Vehicle_Protocol::Address(nic.address(), FLEET_PORT));

    // Attached: tell the vehicle, then wait until the whole vehicle is.
    {
        char slot;
        if (!write_all(up_fd, "1", 1) || !read_all(down_fd, &slot, 1)) {
            std::fprintf(stderr,
                         "[vm %u comp %u] FATAL: the vehicle barrier broke\n",
                         vm_id, comp_id);
            std::printf("RESULT vm=%u comp=%u name=%s counted=0 expected=%u "
                        "FAIL-BARRIER\n",
                        vm_id, comp_id, name, expected);
            std::fflush(stdout);
            return 1;
        }
    }

    char mac[18];
    std::printf("READY vm=%u comp=%u name=%s mac=%s port=%u role=%s\n", vm_id,
                comp_id, name, nic.address().to_string(mac),
                static_cast<unsigned int>(FLEET_PORT),
                is_requester ? "requester" : (is_responder ? "responder"
                                                           : "listener"));
    std::fflush(stdout);

    // -------------------------------------------------------------------------
    // The loop.  One shape for every role: timers fire, frames arrive, the
    // termination condition is checked.  receive() is given a short ceiling so
    // the timers stay responsive — and that ceiling is the whole reason
    // Communicator gained a timed receive: a component that hangs reports
    // nothing, and a fleet test that cannot report is not gradeable.
    // -------------------------------------------------------------------------
    Log log;
    bool seen_peer[MAX_VMS][MAX_COMPONENTS];
    std::memset(seen_peer, 0, sizeof(seen_peer));

    const unsigned long t0 = now_ms();
    unsigned long next_ready = t0;
    unsigned long next_request = 0;
    unsigned long last_rx = t0;

    unsigned int requests_sent = 0;
    unsigned int responses_sent = 0;
    unsigned int counted = 0;
    unsigned int ready_peers = 0;
    unsigned int foreign = 0;

    bool announcing = announces;
    bool started = false; // the requester has heard from everybody
    bool live = false;    // the fleet's traffic has begun
    bool ready_failed = false;

    for (;;) {
        unsigned long now = now_ms();
        if (now - t0 > cfg.hard_deadline_ms)
            break;

        if (announcing && now >= next_ready) {
            send_payload(comm, fleet::KIND_READY, vm_id, comp_id, 0, 0);
            next_ready = now + cfg.ready_interval_ms;
        }

        if (is_requester && !started) {
            if (ready_peers >= expected_peers) {
                started = true;
                live = true;
                next_request = now;
                std::printf("START vm=%u comp=%u peers=%u after_ms=%lu\n", vm_id,
                            comp_id, ready_peers, now - t0);
                std::fflush(stdout);
            } else if (now - t0 > cfg.ready_timeout_ms) {
                ready_failed = true;
                break;
            }
        }

        if (is_requester && started && requests_sent < total &&
            now >= next_request) {
            const unsigned short seq = static_cast<unsigned short>(requests_sent);
            const unsigned char flags =
                (requests_sent < cfg.warmup) ? fleet::FLAG_WARMUP : 0;
            if (send_payload(comm, fleet::KIND_REQUEST, vm_id, comp_id, seq,
                             flags)) {
                log.add("TX vm=%u comp=%u kind=REQ seq=%u%s", vm_id, comp_id,
                        seq, flags ? " warmup" : "");
                requests_sent++;
            } else {
                log.add("TXFAIL vm=%u comp=%u kind=REQ seq=%u", vm_id, comp_id,
                        seq);
            }
            next_request += cfg.interval_ms;
        }

        Message in;
        if (comm.receive(&in, 10)) {
            if (in.size() >= sizeof(fleet::Payload)) {
                fleet::Payload p;
                std::memcpy(&p, in.data(), sizeof(p));

                if (!fleet::valid(p)) {
                    foreign++;
                } else if (p.vm_id == vm_id && p.comp_id == comp_id) {
                    // Our own broadcast, echoed by the bus.  Filtering on the
                    // source MAC would not do: the components of one vehicle
                    // share eth0's address, so identity lives in the payload.
                } else {
                    last_rx = now_ms();
                    const unsigned short seq = fleet::seq_of(p);

                    switch (p.kind) {
                    case fleet::KIND_READY:
                        if (is_requester && !started && p.vm_id != vm_id &&
                            p.vm_id < MAX_VMS && p.comp_id < MAX_COMPONENTS &&
                            !seen_peer[p.vm_id][p.comp_id]) {
                            seen_peer[p.vm_id][p.comp_id] = true;
                            ready_peers++;
                        }
                        break;

                    case fleet::KIND_REQUEST:
                        announcing = false;
                        live = true;
                        if (counted_kind == fleet::KIND_REQUEST) {
                            counted++;
                            log.add("RX vm=%u comp=%u kind=REQ from=%u.%u seq=%u"
                                    " bytes=%u",
                                    vm_id, comp_id, p.vm_id, p.comp_id, seq,
                                    in.size());
                        }
                        if (is_responder) {
                            if (send_payload(comm, fleet::KIND_RESPONSE, vm_id,
                                             comp_id, seq, p.flags))
                                responses_sent++;
                            else
                                log.add("TXFAIL vm=%u comp=%u kind=RESP seq=%u",
                                        vm_id, comp_id, seq);
                        }
                        break;

                    case fleet::KIND_RESPONSE:
                        announcing = false;
                        live = true;
                        if (counted_kind == fleet::KIND_RESPONSE) {
                            counted++;
                            log.add("RX vm=%u comp=%u kind=RESP from=%u.%u "
                                    "seq=%u bytes=%u",
                                    vm_id, comp_id, p.vm_id, p.comp_id, seq,
                                    in.size());
                        }
                        break;

                    default:
                        foreign++;
                        break;
                    }
                }
            }
        }

        now = now_ms();
        const bool sent_everything = !is_requester || requests_sent >= total;

        if (counted >= expected && sent_everything)
            break;
        if (live && sent_everything && now - last_rx > cfg.idle_timeout_ms)
            break;
    }

    // -------------------------------------------------------------------------
    // Report.  Console silence is over.
    // -------------------------------------------------------------------------
    log.flush();

    const Ethernet::Statistics & st = nic.statistics();
    std::printf("STATS vm=%u comp=%u tx_packets=%u tx_bytes=%u rx_packets=%u "
                "rx_bytes=%u rx_dropped=%u foreign=%u\n",
                vm_id, comp_id, st.tx_packets, st.tx_bytes, st.rx_packets,
                st.rx_bytes, st.rx_dropped, foreign);

    if (is_requester)
        std::printf("SENT vm=%u comp=%u requests=%u of=%u\n", vm_id, comp_id,
                    requests_sent, total);
    if (is_responder)
        std::printf("SENT vm=%u comp=%u responses=%u\n", vm_id, comp_id,
                    responses_sent);

    if (ready_failed)
        std::printf("READY-TIMEOUT vm=%u comp=%u peers=%u of=%u after_ms=%lu\n",
                    vm_id, comp_id, ready_peers, expected_peers,
                    now_ms() - t0);

    const bool ok = !ready_failed && counted == expected &&
                    (!is_requester || requests_sent == total);

    std::printf("RESULT vm=%u comp=%u name=%s kind=%s counted=%u expected=%u "
                "%s\n",
                vm_id, comp_id, name, fleet::kind_name(counted_kind), counted,
                expected, ok ? "OK" : "FAIL");
    std::fflush(stdout);

    return ok ? 0 : 1;
}

// -----------------------------------------------------------------------------
// The vehicle: fork the components, wait, aggregate, report.
// -----------------------------------------------------------------------------
int run_vehicle(unsigned int vm_id, const Config & cfg)
{
    int up[2], down[2];
    if (::pipe(up) < 0 || ::pipe(down) < 0) {
        std::fprintf(stderr, "[vm %u] FATAL: pipe: %s\n", vm_id,
                     std::strerror(errno));
        return 1;
    }

    pid_t child[MAX_COMPONENTS];
    unsigned int forked = 0;

    for (unsigned int c = 0; c < cfg.components; c++) {
        pid_t pid = ::fork();
        if (pid < 0) {
            std::fprintf(stderr, "[vm %u] FATAL: fork: %s\n", vm_id,
                         std::strerror(errno));
            break;
        }
        if (pid == 0) {
            ::close(up[0]);
            ::close(down[1]);
            // THE STACK IS BUILT HERE, after the fork, never before it.
            int rc = run_component(vm_id, c, cfg, up[1], down[0]);
            ::close(up[1]);
            ::close(down[0]);
            std::_Exit(rc);
        }
        child[forked++] = pid;
    }

    ::close(up[1]);
    ::close(down[0]);

    // Wait for every component to report itself attached, then release them all
    // at once.
    bool barrier_ok = true;
    for (unsigned int c = 0; c < forked; c++) {
        char slot;
        if (!read_all(up[0], &slot, 1)) {
            barrier_ok = false;
            break;
        }
    }
    if (barrier_ok) {
        for (unsigned int c = 0; c < forked; c++)
            if (!write_all(down[1], "1", 1))
                barrier_ok = false;
    }
    ::close(up[0]);
    ::close(down[1]);

    if (!barrier_ok)
        std::fprintf(stderr,
                     "[vm %u] a component failed before the barrier; the "
                     "verdicts below say which\n",
                     vm_id);

    unsigned int ok = 0;
    for (unsigned int c = 0; c < forked; c++) {
        int status = 0;
        while (::waitpid(child[c], &status, 0) < 0 && errno == EINTR)
            ;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            ok++;
    }

    const bool all = (forked == cfg.components) && (ok == cfg.components);
    std::printf("RESULT vm=%u components=%u ok=%u %s\n", vm_id, cfg.components,
                ok, all ? "OK" : "FAIL");
    std::fflush(stdout);

    return all ? 0 : 1;
}

} // namespace

int main()
{
    const Config cfg = config();
    const unsigned int vm_id = clamp(param_int("vm_id", "SO2_VM_ID", 0), 0,
                                     MAX_VMS - 1);

    std::printf("[vm %u] libvcomm — Stage 1 — vehicle with %u components, "
                "ethertype 0x%04x, %u requests (%u warm-up)\n",
                vm_id, cfg.components,
                static_cast<unsigned int>(Traits<Ethernet>::PROTOCOL_NUMBER),
                cfg.requests(), cfg.warmup);
    std::fflush(stdout);

    if (vm_id < 1) {
        std::fprintf(stderr,
                     "[vm ?] FATAL: SO2_VM_ID is unset or 0. The starter's "
                     "/init sets it from so2.vm_id on the kernel command "
                     "line.\n");
        std::printf("RESULT vm=0 components=0 ok=0 FAIL-NO-VM-ID\n");
        std::fflush(stdout);
        return 1;
    }

    const int rc = run_vehicle(vm_id, cfg);

    std::printf("DONE vm=%u rc=%d\n", vm_id, rc);
    std::fflush(stdout);

    // Powering the vehicle off is what makes the fleet finish when the WORK
    // finishes instead of always burning the whole timeout.  The timeout in
    // run-fleet.sh stays as the ceiling for when this does not happen.
    if (cfg.poweroff && inside_starter_vm()) {
        ::sync();
        struct timespec pause = {0, 200 * 1000 * 1000L}; // let the UART drain
        while (::nanosleep(&pause, &pause) == -1 && errno == EINTR)
            ;
        ::reboot(RB_POWER_OFF);
    } else if (cfg.poweroff) {
        std::printf("[vm %u] not inside the starter VM — poweroff skipped\n",
                    vm_id);
        std::fflush(stdout);
    }

    return rc;
}
