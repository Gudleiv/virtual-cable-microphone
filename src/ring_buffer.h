#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace vcmic {

// Lock-free single-producer / single-consumer ring of deinterleaved stereo
// float32 frames (spec 4.5): one capture thread writes, the render thread
// reads, and nothing between them locks or allocates.
//
// Reset() is the only method that allocates and it must be called before the
// threads start. Capacity is rounded up to a power of two so that wrapping is a
// mask rather than a division.
class StereoRing {
public:
    void Reset(std::size_t capacity_frames) {
        capacity_ = RoundUpPow2(capacity_frames < 2 ? 2 : capacity_frames);
        mask_ = capacity_ - 1;
        left_.assign(capacity_, 0.0f);
        right_.assign(capacity_, 0.0f);
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
    }

    std::size_t capacity() const noexcept { return capacity_; }

    // Safe from either side: both indices are read atomically.
    std::size_t Readable() const noexcept {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_acquire);
        return w - r;
    }

    // Producer side.
    std::size_t Write(const float* left, const float* right, std::size_t frames) noexcept {
        if (capacity_ == 0) {
            return 0;
        }
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        const std::size_t count = (std::min)(frames, capacity_ - (w - r));
        if (count == 0) {
            return 0;
        }

        const std::size_t start = w & mask_;
        const std::size_t first = (std::min)(count, capacity_ - start);
        std::memcpy(left_.data() + start, left, first * sizeof(float));
        std::memcpy(right_.data() + start, right, first * sizeof(float));
        if (count > first) {
            std::memcpy(left_.data(), left + first, (count - first) * sizeof(float));
            std::memcpy(right_.data(), right + first, (count - first) * sizeof(float));
        }

        write_.store(w + count, std::memory_order_release);
        return count;
    }

    // Consumer side.
    std::size_t Read(float* left, float* right, std::size_t frames) noexcept {
        if (capacity_ == 0) {
            return 0;
        }
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t count = (std::min)(frames, w - r);
        if (count == 0) {
            return 0;
        }

        const std::size_t start = r & mask_;
        const std::size_t first = (std::min)(count, capacity_ - start);
        std::memcpy(left, left_.data() + start, first * sizeof(float));
        std::memcpy(right, right_.data() + start, first * sizeof(float));
        if (count > first) {
            std::memcpy(left + first, left_.data(), (count - first) * sizeof(float));
            std::memcpy(right + first, right_.data(), (count - first) * sizeof(float));
        }

        read_.store(r + count, std::memory_order_release);
        return count;
    }

    // Consumer side. Drops the oldest frames; used to pull the fill level back
    // down after a stall, which is the emergency path of spec 4.6.
    std::size_t Discard(std::size_t frames) noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t count = (std::min)(frames, w - r);
        if (count != 0) {
            read_.store(r + count, std::memory_order_release);
        }
        return count;
    }

private:
    static std::size_t RoundUpPow2(std::size_t value) {
        std::size_t result = 1;
        while (result < value) {
            result <<= 1;
        }
        return result;
    }

    std::vector<float> left_;
    std::vector<float> right_;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;

    // Separate cache lines: the producer spins on one, the consumer on the other.
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

}  // namespace vcmic
