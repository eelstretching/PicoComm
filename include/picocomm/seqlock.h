// A sequence lock: one writer (core 1) publishes a small value that a reader (core 0) can copy
// every frame without ever blocking or taking a lock. The reader retries in the rare case it
// overlapped a write. Portable C++17 atomics, so it also runs in the host tests.

#pragma once

#include <atomic>
#include <cstdint>

namespace picocomm {

template <typename T>
class SeqLock {
   public:
    // Writer side. Only one writer at a time.
    void write(const T &v) {
        uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);  // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        value_ = v;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_relaxed);
    }

    // Reader side. Returns false if nothing has been written yet.
    bool read(T &out) const {
        while (true) {
            uint32_t s0 = seq_.load(std::memory_order_acquire);
            if (s0 & 1) {
                continue;  // writer is mid-update; it finishes within a few instructions
            }
            out = value_;
            std::atomic_thread_fence(std::memory_order_acquire);
            uint32_t s1 = seq_.load(std::memory_order_relaxed);
            if (s0 == s1) {
                return s0 != 0;
            }
        }
    }

   private:
    std::atomic<uint32_t> seq_{0};
    T value_{};
};

}  // namespace picocomm
