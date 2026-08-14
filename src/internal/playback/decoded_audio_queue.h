#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sonotide::detail::playback {

struct decoded_metadata {
    std::uint64_t generation = 0;
    std::uint64_t seek_sequence = 0;
    std::int64_t position_ms = 0;
    std::int64_t duration_ms = 0;
    bool end_of_stream = false;
};

class decoded_audio_queue {
public:
    decoded_audio_queue(std::size_t slot_count, std::size_t samples_per_slot)
        : slots_(slot_count + 1U) {
        for (slot& value : slots_) {
            value.samples.resize(samples_per_slot);
        }
    }

    bool try_push(std::span<const float> samples, decoded_metadata metadata) noexcept {
        const std::size_t write = write_.load(std::memory_order_relaxed);
        const std::size_t next = increment(write);
        if (next == read_.load(std::memory_order_acquire) ||
            samples.size() > slots_[write].samples.size()) {
            return false;
        }
        slot& destination = slots_[write];
        std::copy(samples.begin(), samples.end(), destination.samples.begin());
        destination.sample_count = samples.size();
        destination.metadata = metadata;
        write_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(std::span<float> destination, decoded_metadata& metadata) noexcept {
        const std::size_t read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) {
            return false;
        }
        slot& source = slots_[read];
        const std::size_t count = (std::min)(destination.size(), source.sample_count);
        std::copy_n(source.samples.data(), count, destination.data());
        if (count < destination.size()) {
            std::fill(destination.begin() + static_cast<std::ptrdiff_t>(count), destination.end(), 0.0F);
        }
        metadata = source.metadata;
        read_.store(increment(read), std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool full() const noexcept {
        return increment(write_.load(std::memory_order_acquire)) ==
               read_.load(std::memory_order_acquire);
    }

private:
    struct slot {
        std::vector<float> samples;
        std::size_t sample_count = 0;
        decoded_metadata metadata{};
    };

    [[nodiscard]] std::size_t increment(std::size_t value) const noexcept {
        return (value + 1U) % slots_.size();
    }

    std::vector<slot> slots_;
    std::atomic<std::size_t> read_{0U};
    std::atomic<std::size_t> write_{0U};
};

}  // namespace sonotide::detail::playback
