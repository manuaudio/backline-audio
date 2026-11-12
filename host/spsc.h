// Single-producer single-consumer ring: wait-free, no allocation after
// construction, safe between the main thread and the realtime thread.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

template <typename T>
class Spsc {
public:
    explicit Spsc(size_t capacity_pow2) : buf_(capacity_pow2), mask_(capacity_pow2 - 1) {}

    bool push(const T &v)
    {
        size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) > mask_)
            return false; // full: the caller counts the drop
        buf_[h & mask_] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pop(T &out)
    {
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire))
            return false;
        out = buf_[t & mask_];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

private:
    std::vector<T> buf_;
    size_t mask_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};
