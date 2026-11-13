// offload-proof: the smallest possible Backline DSP-offload loop.
//
// The client is the console: every period (64 frames at 48 kHz) it sends one
// stereo block to the server, then waits for the processed block to come back
// before the period's deadline. The server is a DSP node: it runs a CLAP chain
// (offload/rack.h) on each block as it arrives and sends it straight back, in
// lockstep with the client (the client's period is the only clock; the server
// has none). The client can run the same chain locally on the same input and
// compare, which proves the remote audio is the plugin's output, bit for bit.
//
//   offload-proof server --transport udp|l2 [--if IF] [--port P] --plugin SPEC.. [--cpu N] [--spin]
//   offload-proof client --transport udp|l2 [--if IF] [--peer HOST|MAC] [--port P]
//                        [--seconds S] [--budget-us U] [--cpu N] [--spin] [--verify SPEC..]
//
// Transports: udp = UDP/IPv4 (loopback or any link); l2 = raw Ethernet frames
// (AF_PACKET, EtherType 0x88B5, IEEE local experimental), one frame per block,
// no IP, no checksum offload, PACKET_QDISC_BYPASS. --spin: busy-poll the socket
// (MSG_DONTWAIT) instead of blocking in recv.
// SPDX-License-Identifier: MIT
#include "rack.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cmath>
#include <getopt.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

using namespace blo;

static constexpr uint16_t kEtherType = 0x88B5;
static constexpr uint32_t kMagic = 0x314F4C42; // "BLO1"
static constexpr uint32_t kFrames = 64, kCh = 2;

struct __attribute__((packed)) Hdr {
    uint32_t magic;
    uint32_t seq;
    uint16_t frames, channels;
    uint32_t flags;      // 1 = server error (block is dry)
    uint64_t t_send_ns;  // client clock, echoed
    uint32_t server_us;  // server processing time, filled by the server
    uint32_t reserved;   // keeps the samples 16-byte aligned
};
struct __attribute__((packed)) Pkt {
    Hdr h;
    float s[kCh][kFrames]; // planar
};
static_assert(sizeof(Pkt) == 32 + 512);

static int64_t now_ns()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

struct Link {
    int fd = -1;
    bool l2 = false, spin = false;
    // l2
    int ifindex = 0;
    uint8_t mymac[6]{}, peermac[6]{};
    bool peer_known = false;
    // udp
    sockaddr_in peer{};
    bool udp_peer_known = false;

    bool open_l2(const char *ifname)
    {
        l2 = true;
        fd = socket(AF_PACKET, SOCK_DGRAM, htons(kEtherType));
        if (fd < 0) { perror("socket AF_PACKET"); return false; }
        ifindex = if_nametoindex(ifname);
        if (!ifindex) { perror(ifname); return false; }
        ifreq ifr{};
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
        if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) memcpy(mymac, ifr.ifr_hwaddr.sa_data, 6);
        sockaddr_ll a{};
        a.sll_family = AF_PACKET;
        a.sll_protocol = htons(kEtherType);
        a.sll_ifindex = ifindex;
        if (bind(fd, (sockaddr *)&a, sizeof a) < 0) { perror("bind l2"); return false; }
        int one = 1;
        setsockopt(fd, SOL_PACKET, PACKET_QDISC_BYPASS, &one, sizeof one);
        return true;
    }
    bool open_udp(int port, const char *peer_host)
    {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(peer_host ? 0 : port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (sockaddr *)&a, sizeof a) < 0) { perror("bind udp"); return false; }
        if (peer_host) {
            peer.sin_family = AF_INET;
            peer.sin_port = htons(port);
            inet_pton(AF_INET, peer_host, &peer.sin_addr);
            udp_peer_known = true;
        }
        int tos = 46 << 2; // DSCP EF
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
        return true;
    }
    bool send(const Pkt &p)
    {
        if (l2) {
            sockaddr_ll a{};
            a.sll_family = AF_PACKET;
            a.sll_protocol = htons(kEtherType);
            a.sll_ifindex = ifindex;
            a.sll_halen = 6;
            memcpy(a.sll_addr, peer_known ? peermac : (const uint8_t *)"\xff\xff\xff\xff\xff\xff", 6);
            return sendto(fd, &p, sizeof p, 0, (sockaddr *)&a, sizeof a) == (ssize_t)sizeof p;
        }
        return sendto(fd, &p, sizeof p, 0, (sockaddr *)&peer, sizeof peer) == (ssize_t)sizeof p;
    }
    // Receive one packet; deadline_ns 0 = forever. Returns false on timeout.
    bool recv(Pkt &p, int64_t deadline_ns)
    {
        for (;;) {
            sockaddr_storage from{};
            socklen_t fl = sizeof from;
            int flags = spin || deadline_ns ? MSG_DONTWAIT : 0;
            if (!spin && deadline_ns) {
                // block with a timeout (poll-free: SO_RCVTIMEO set per call is a syscall too;
                // use a short blocking wait via recv timeout)
                int64_t left = deadline_ns - now_ns();
                if (left <= 0) return false;
                timeval tv{(time_t)(left / 1000000000), (suseconds_t)((left % 1000000000) / 1000)};
                if (!tv.tv_sec && !tv.tv_usec) tv.tv_usec = 1;
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                flags = 0;
            }
            ssize_t n = recvfrom(fd, &p, sizeof p, flags, (sockaddr *)&from, &fl);
            if (n == (ssize_t)sizeof p && p.h.magic == kMagic) {
                if (l2 && !peer_known) {
                    memcpy(peermac, ((sockaddr_ll *)&from)->sll_addr, 6);
                    peer_known = true;
                }
                if (!l2 && !udp_peer_known) {
                    peer = *(sockaddr_in *)&from;
                    udp_peer_known = true;
                }
                return true;
            }
            if (deadline_ns && now_ns() >= deadline_ns) return false;
            if (spin) __builtin_ia32_pause();
        }
    }
};

static void pin(int cpu)
{
    if (cpu < 0) return;
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

static double pct(std::vector<int32_t> v, double p)
{
    if (v.empty()) return 0;
    size_t k = std::min(v.size() - 1, (size_t)(p * (v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "offload-proof server|client ...\n"); return 2; }
    bool server = !strcmp(argv[1], "server");
    std::string transport = "udp", ifname, peerhost = "127.0.0.1";
    int port = 47100, cpu = -1;
    bool spin = false;
    double seconds = 30, budget_us = 1333.3;
    std::vector<Spec> chain, verify;
    static const option opts[] = {{"transport", 1, 0, 't'}, {"if", 1, 0, 'i'}, {"peer", 1, 0, 'P'},
                                  {"port", 1, 0, 'p'}, {"plugin", 1, 0, 'g'}, {"verify", 1, 0, 'v'},
                                  {"seconds", 1, 0, 's'}, {"budget-us", 1, 0, 'b'}, {"cpu", 1, 0, 'c'},
                                  {"spin", 0, 0, 'S'}, {}};
    int c;
    optind = 2;
    while ((c = getopt_long(argc, argv, "", opts, nullptr)) != -1) {
        switch (c) {
        case 't': transport = optarg; break;
        case 'i': ifname = optarg; break;
        case 'P': peerhost = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'g': chain.push_back(parse_spec(optarg)); break;
        case 'v': verify.push_back(parse_spec(optarg)); break;
        case 's': seconds = atof(optarg); break;
        case 'b': budget_us = atof(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'S': spin = true; break;
        default: return 2;
        }
    }
    pin(cpu);
    Link link;
    link.spin = spin;
    bool ok = transport == "l2" ? link.open_l2(ifname.c_str()) : link.open_udp(port, server ? nullptr : peerhost.c_str());
    if (!ok) return 1;

    if (server) {
        Rack rack(48000, kFrames);
        for (auto &s : chain) {
            std::string err;
            if (!rack.add(s, err)) { fprintf(stderr, "server: %s: %s\n", s.path.c_str(), err.c_str()); return 1; }
        }
        // lock what is loaded (best effort: without Pro Audio mode RLIMIT_MEMLOCK is 8 MB and
        // this fails; MCL_FUTURE before dlopen would make the plugins fail to map)
        mlockall(MCL_CURRENT);
        fprintf(stderr, "server: %zu plugins, transport %s, %s\n", rack.plugins.size(), transport.c_str(), spin ? "busy-poll" : "blocking");
        Pkt in, out;
        int64_t steady = 0;
        for (;;) {
            if (!link.recv(in, 0)) continue;
            int64_t a = now_ns();
            out.h = in.h;
            out.h.flags = rack.process(in.s[0], in.s[1], out.s[0], out.s[1], steady) ? 0 : 1;
            steady += kFrames;
            out.h.server_us = (uint32_t)((now_ns() - a) / 1000);
            link.send(out);
        }
    }

    // client: the console side
    std::unique_ptr<Rack> ref;
    if (!verify.empty()) {
        ref = std::make_unique<Rack>(48000, kFrames);
        for (auto &s : verify) {
            std::string err;
            if (!ref->add(s, err)) { fprintf(stderr, "client verify: %s\n", err.c_str()); return 1; }
        }
    }
    const int64_t P = 1000000000LL * kFrames / 48000;
    const int64_t budget = (int64_t)(budget_us * 1000);
    size_t cap = (size_t)(seconds * 1e9 / P) + 16;
    std::vector<int32_t> rtt, srv, wake;
    rtt.reserve(cap); srv.reserve(cap); wake.reserve(cap);
    uint64_t late = 0, lost = 0, stale = 0, errors = 0;
    double maxdiff = 0;
    float refL[kFrames], refR[kFrames];
    Pkt out, in;
    out.h.magic = kMagic;
    out.h.frames = kFrames;
    out.h.channels = kCh;
    out.h.flags = 0;
    srand(7);
    // warm-up: 500 blocks, not measured (first-block allocations in plugins, ARP/MAC learning)
    int64_t start = now_ns() + 20000000;
    uint64_t warm = 500, total = warm + (uint64_t)(seconds * 1e9 / P);
    for (uint64_t k = 0; k < total; k++) {
        int64_t target = start + (int64_t)k * P;
        if (spin) { while (now_ns() < target) __builtin_ia32_pause(); }
        else {
            timespec ts{(time_t)(target / 1000000000), (long)(target % 1000000000)};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        }
        int64_t woke = now_ns();
        for (uint32_t i = 0; i < kFrames; i++) {
            out.s[0][i] = 0.125f * (2.f * rand() / RAND_MAX - 1.f);
            out.s[1][i] = 0.125f * (2.f * rand() / RAND_MAX - 1.f);
        }
        out.h.seq = (uint32_t)k;
        out.h.t_send_ns = (uint64_t)now_ns();
        link.send(out);
        // wait for this block, until the end of the period (the latest the console can mix it in)
        int64_t deadline = target + P;
        bool got = false;
        while (link.recv(in, deadline)) {
            if (in.h.seq != (uint32_t)k) { stale++; continue; } // a late reply from an earlier period
            got = true;
            break;
        }
        int64_t t = now_ns();
        // the reference runs after the reply is in, so it never inflates the measured round trip
        if (ref) ref->process(out.s[0], out.s[1], refL, refR, (int64_t)k * kFrames);
        if (k < warm) continue;
        wake.push_back((int32_t)((woke - target) / 1000));
        if (!got) { lost++; continue; }
        int64_t r = t - (int64_t)in.h.t_send_ns;
        rtt.push_back((int32_t)(r / 100)); // 0.1 us units
        srv.push_back((int32_t)in.h.server_us);
        if (in.h.flags) errors++;
        if (r > budget) late++;
        if (ref)
            for (uint32_t i = 0; i < kFrames; i++)
                maxdiff = std::max({maxdiff, (double)fabsf(refL[i] - in.s[0][i]), (double)fabsf(refR[i] - in.s[1][i])});
    }
    uint64_t measured = total - warm;
    printf("{\"transport\": \"%s\", \"wait\": \"%s\", \"blocks\": %llu, \"frames\": %u, \"rate\": 48000, \"period_us\": %.1f, "
           "\"budget_us\": %.0f, \"rtt_min_us\": %.1f, \"rtt_p50_us\": %.1f, \"rtt_p99_us\": %.1f, \"rtt_p999_us\": %.1f, "
           "\"rtt_max_us\": %.1f, \"jitter_p999_minus_p50_us\": %.1f, \"server_p50_us\": %.0f, \"server_max_us\": %.0f, "
           "\"over_budget\": %llu, \"missed_period\": %llu, \"stale\": %llu, \"server_errors\": %llu, "
           "\"client_wake_p99_us\": %.0f, \"client_wake_max_us\": %.0f, \"verify\": %s, \"max_abs_diff_vs_local\": %g}\n",
           transport.c_str(), spin ? "busy-poll" : "blocking", (unsigned long long)measured, kFrames, P / 1000.0, budget_us,
           pct(rtt, 0) / 10, pct(rtt, .5) / 10, pct(rtt, .99) / 10, pct(rtt, .999) / 10, pct(rtt, 1) / 10,
           (pct(rtt, .999) - pct(rtt, .5)) / 10, pct(srv, .5), pct(srv, 1), (unsigned long long)late,
           (unsigned long long)lost, (unsigned long long)stale, (unsigned long long)errors, pct(wake, .99), pct(wake, 1),
           ref ? "true" : "false", maxdiff);
    fflush(stdout);
    _exit(0);
}
