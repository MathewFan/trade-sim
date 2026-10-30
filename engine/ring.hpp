#pragma once
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

// Single-Producer/Single-Consumer ring buffer (fixed size; N must be a power of two)
template<typename T, size_t N>
struct SpscRing {
    static_assert(N >= 2 && (N & (N-1)) == 0, "N must be power of two");
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};
    T buf[N];

    bool push(const T& v) {
        size_t h = head.load(std::memory_order_relaxed);
        size_t n = (h + 1) & (N - 1);
        if (n == tail.load(std::memory_order_acquire)) return false; // full
        buf[h] = v;
        head.store(n, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {
        size_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false; // empty
        out = std::move(buf[t]);
        buf[t] = T{};
        tail.store((t + 1) & (N - 1), std::memory_order_release);
        return true;
    }
    bool empty() const {
        return tail.load(std::memory_order_acquire) == head.load(std::memory_order_acquire);
    }
};
