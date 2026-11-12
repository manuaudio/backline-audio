// Unit tests: the worker's SPSC ring and OSC codec. SPDX-License-Identifier: MIT
#include <cstdio>
#include <thread>

#include "osc.h"
#include "spsc.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_spsc_basic()
{
    Spsc<int> q(4);
    int v;
    CHECK(!q.pop(v));
    for (int i = 0; i < 4; i++)
        CHECK(q.push(i));
    CHECK(!q.push(99)); // full: the caller counts the drop
    for (int i = 0; i < 4; i++)
        CHECK(q.pop(v) && v == i);
    CHECK(!q.pop(v));
}

static void test_spsc_threads()
{
    Spsc<unsigned> q(256);
    const unsigned n = 2000000;
    std::thread prod([&] {
        for (unsigned i = 0; i < n;)
            if (q.push(i))
                i++;
    });
    unsigned expect = 0, v;
    bool order = true;
    while (expect < n)
        if (q.pop(v))
            order &= v == expect++;
    prod.join();
    CHECK(order);
}

static void test_osc_roundtrip()
{
    auto b = osc::Writer("/backline/param/set").s("Global Volume").f(0.5f).bytes();
    CHECK(b.size() % 4 == 0);
    osc::Message m;
    CHECK(osc::parse(b.data(), b.size(), m));
    CHECK(m.address == "/backline/param/set" && m.args.size() == 2);
    double v = 0;
    CHECK(std::get<std::string>(m.args[0]) == "Global Volume");
    CHECK(osc::as_double(m.args[1], v) && v == 0.5);

    auto r = osc::Writer("/backline/param").i(-7).s("A/B").d(0.25).d(0).d(1).bytes();
    CHECK(osc::parse(r.data(), r.size(), m) && m.args.size() == 5 && std::get<int32_t>(m.args[0]) == -7);
    CHECK(osc::as_double(m.args[2], v) && v == 0.25);
}

static void test_osc_malformed()
{
    osc::Message m;
    const uint8_t no_slash[] = {'x', 0, 0, 0};
    CHECK(!osc::parse(no_slash, sizeof(no_slash), m));
    const uint8_t unterminated[] = {'/', 'a', 'b', 'c'};
    CHECK(!osc::parse(unterminated, sizeof(unterminated), m));
    auto b = osc::Writer("/x").i(1).bytes();
    CHECK(!osc::parse(b.data(), b.size() - 2, m)); // truncated argument
    CHECK(!osc::parse(b.data(), 0, m));
}

int main()
{
    test_spsc_basic();
    test_spsc_threads();
    test_osc_roundtrip();
    test_osc_malformed();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        puts("test_rt: all passed");
    return failures != 0;
}
