// dspbench: how many CLAP plugin instances fit in a 64-frame period on this
// box. Each thread runs one chain (a Rack) on a periodic absolute timer, the
// way an engine thread is woken by the device each period, and counts the
// periods whose work ends after the next period starts (a deadline miss = an
// xrun on a real device). Proof code for docs/offload.
//
//   dspbench --list SPEC                       print a plugin's parameters
//   dspbench --cost SPEC [--seconds S]         back-to-back cost of one instance
//   dspbench --plugin SPEC [--plugin SPEC..] --n N --threads T [--seconds S]
//            [--frames 64] [--rate 48000] [--cpus 2,3,4] [--fifo PRIO] [--spin]
//
// SPEC = PATH.clap:ID[:name=value;name=value]
// SPDX-License-Identifier: MIT
#include "rack.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <time.h>
#include <unistd.h>

using namespace blo;

static int64_t now_ns()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static double pct(std::vector<int32_t> v, double p)
{
    if (v.empty()) return 0;
    size_t k = std::min(v.size() - 1, (size_t)(p * (v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

struct Result {
    uint64_t periods = 0, misses = 0, late_wakes = 0;
    std::vector<int32_t> work_us, wake_us;
    int64_t worst_finish_ns = 0; // finish time relative to own period start
    bool fifo = false;
};

int main(int argc, char **argv)
{
    std::vector<Spec> chain;
    int n = 1, threads = 1, fifo = 0;
    bool spin = false; // busy-wait for the period instead of sleeping (an isolated core's engine thread)
    double seconds = 10;
    uint32_t frames = 64, rate = 48000;
    std::vector<int> cpus;
    std::string list, cost;
    static const option opts[] = {{"plugin", 1, 0, 'p'}, {"n", 1, 0, 'n'}, {"threads", 1, 0, 't'},
                                  {"seconds", 1, 0, 's'}, {"frames", 1, 0, 'f'}, {"rate", 1, 0, 'r'},
                                  {"cpus", 1, 0, 'c'}, {"fifo", 1, 0, 'F'}, {"list", 1, 0, 'l'},
                                  {"cost", 1, 0, 'C'}, {"spin", 0, 0, 'X'}, {}};
    int c;
    while ((c = getopt_long(argc, argv, "", opts, nullptr)) != -1) {
        switch (c) {
        case 'p': chain.push_back(parse_spec(optarg)); break;
        case 'n': n = atoi(optarg); break;
        case 't': threads = atoi(optarg); break;
        case 's': seconds = atof(optarg); break;
        case 'f': frames = atoi(optarg); break;
        case 'r': rate = atoi(optarg); break;
        case 'F': fifo = atoi(optarg); break;
        case 'l': list = optarg; break;
        case 'C': cost = optarg; break;
        case 'X': spin = true; break;
        case 'c': {
            char *s = optarg;
            while (*s) { cpus.push_back(strtol(s, &s, 10)); if (*s == ',') s++; }
            break;
        }
        default: return 2;
        }
    }
    if (!list.empty()) { Rack::list_params(parse_spec(list)); return 0; }

    // white noise at -18 dBFS: plugins that skip silence must not get it
    std::vector<float> nl(frames), nr(frames), ol(frames), orr(frames);
    srand(1);
    for (uint32_t i = 0; i < frames; i++) {
        nl[i] = 0.125f * (2.f * rand() / RAND_MAX - 1.f);
        nr[i] = 0.125f * (2.f * rand() / RAND_MAX - 1.f);
    }

    if (!cost.empty()) {
        Rack r(rate, frames);
        std::string err;
        if (!r.add(parse_spec(cost), err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        for (int i = 0; i < 2000; i++) r.process(nl.data(), nr.data(), ol.data(), orr.data(), (int64_t)i * frames);
        std::vector<int32_t> t;
        int64_t end = now_ns() + (int64_t)(seconds * 1e9), steady = 2000LL * frames;
        while (now_ns() < end) {
            int64_t a = now_ns();
            r.process(nl.data(), nr.data(), ol.data(), orr.data(), steady += frames);
            t.push_back((int32_t)((now_ns() - a) / 10)); // units of 10 ns
        }
        double sum = 0;
        for (auto v : t) sum += v;
        double period_us = 1e6 * frames / rate;
        printf("{\"plugin\": \"%s\", \"frames\": %u, \"rate\": %u, \"blocks\": %zu, \"mean_us\": %.2f, \"p50_us\": %.2f, \"p99_us\": %.2f, \"p999_us\": %.2f, \"max_us\": %.2f, \"pct_of_period\": %.2f}\n",
               r.plugins[0].name.c_str(), frames, rate, t.size(), sum / t.size() / 100, pct(t, .5) / 100,
               pct(t, .99) / 100, pct(t, .999) / 100, *std::max_element(t.begin(), t.end()) / 100.0,
               100.0 * sum / t.size() / 100 / period_us);
        return 0;
    }

    if (chain.empty()) { fprintf(stderr, "no --plugin\n"); return 2; }
    const int64_t P = (int64_t)1000000000 * frames / rate;
    std::vector<std::unique_ptr<Rack>> racks;
    for (int t = 0; t < threads; t++) {
        auto r = std::make_unique<Rack>(rate, frames);
        for (int k = 0; k < n; k++)
            for (auto &s : chain) {
                std::string err;
                if (!r->add(s, err)) { fprintf(stderr, "load %s: %s\n", s.path.c_str(), err.c_str()); return 1; }
            }
        racks.push_back(std::move(r));
    }
    std::vector<Result> res(threads);
    std::atomic<int> ready{0};
    std::atomic<int64_t> t0{0};
    std::vector<std::thread> th;
    for (int t = 0; t < threads; t++) {
        th.emplace_back([&, t] {
            Result &R = res[t];
            if (!cpus.empty()) {
                cpu_set_t set;
                CPU_ZERO(&set);
                CPU_SET(cpus[t % cpus.size()], &set);
                pthread_setaffinity_np(pthread_self(), sizeof set, &set);
            }
            if (fifo) {
                sched_param sp{fifo};
                R.fifo = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
            }
            Rack &r = *racks[t];
            size_t cap = (size_t)(seconds * 1e9 / P) + 16;
            R.work_us.reserve(cap);
            R.wake_us.reserve(cap);
            // warm up outside the measurement (first blocks allocate in some plugins)
            for (int i = 0; i < 200; i++) r.process(nl.data(), nr.data(), ol.data(), orr.data(), (int64_t)i * frames);
            ready++;
            while (!t0.load()) std::this_thread::yield();
            int64_t start = t0.load(), steady = 200LL * frames;
            for (uint64_t k = 0;; k++) {
                int64_t target = start + (int64_t)k * P;
                if (target - start > (int64_t)(seconds * 1e9)) break;
                timespec ts{(time_t)(target / 1000000000), (long)(target % 1000000000)};
                if (spin) { while (now_ns() < target) __builtin_ia32_pause(); }
                else clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
                int64_t woke = now_ns();
                r.process(nl.data(), nr.data(), ol.data(), orr.data(), steady += frames);
                int64_t done = now_ns();
                R.periods++;
                R.work_us.push_back((int32_t)((done - woke) / 1000));
                R.wake_us.push_back((int32_t)((woke - target) / 1000));
                if (done - target > R.worst_finish_ns) R.worst_finish_ns = done - target;
                if (done > target + P) {
                    R.misses++;
                    // a real device would have xrun; skip the periods we overran
                    // and resumes at the next whole period
                    uint64_t cur = (uint64_t)((done - start) / P);
                    if (cur > k) k = cur;
                }
            }
        });
    }
    while (ready.load() < threads) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    t0 = now_ns() + 50000000;
    for (auto &x : th) x.join();

    uint64_t periods = 0, misses = 0;
    std::vector<int32_t> work, wake;
    int64_t worst = 0;
    bool anyfifo = false;
    for (auto &R : res) {
        periods += R.periods;
        misses += R.misses;
        work.insert(work.end(), R.work_us.begin(), R.work_us.end());
        wake.insert(wake.end(), R.wake_us.begin(), R.wake_us.end());
        worst = std::max(worst, R.worst_finish_ns);
        anyfifo |= R.fifo;
    }
    printf("{\"instances_per_thread\": %zu, \"threads\": %d, \"total_instances\": %zu, \"frames\": %u, \"rate\": %u, "
           "\"period_us\": %.1f, \"seconds\": %.0f, \"periods\": %llu, \"misses\": %llu, \"miss_per_min\": %.2f, "
           "\"work_p50_us\": %.0f, \"work_p99_us\": %.0f, \"work_p999_us\": %.0f, \"work_max_us\": %d, "
           "\"wake_p50_us\": %.0f, \"wake_p99_us\": %.0f, \"wake_max_us\": %d, \"worst_finish_us\": %.0f, "
           "\"load_p50_pct\": %.1f, \"sched_fifo\": %s, \"wait\": \"%s\"}\n",
           racks[0]->plugins.size(), threads, racks[0]->plugins.size() * threads, frames, rate, P / 1000.0, seconds,
           (unsigned long long)periods, (unsigned long long)misses, misses / (seconds / 60.0) / threads,
           pct(work, .5), pct(work, .99), pct(work, .999), *std::max_element(work.begin(), work.end()),
           pct(wake, .5), pct(wake, .99), *std::max_element(wake.begin(), wake.end()), worst / 1000.0,
           100.0 * pct(work, .5) * 1000 / P, anyfifo ? "true" : "false", spin ? "spin" : "sleep");
    fflush(stdout);
    _exit(0); // skip plugin teardown at exit (some plugins are slow to unload); measurement is done
}
