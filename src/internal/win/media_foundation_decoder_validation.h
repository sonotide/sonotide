#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>

#include "sonotide/audio_format.h"

namespace sonotide::detail::win {

class decoder_cancellation_epoch {
public:
    [[nodiscard]] std::uint64_t request() noexcept {
        return epoch_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    }

    [[nodiscard]] std::uint64_t snapshot() const noexcept {
        return epoch_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool changed_since(const std::uint64_t snapshot) const noexcept {
        return epoch_.load(std::memory_order_acquire) != snapshot;
    }

private:
    std::atomic<std::uint64_t> epoch_{0U};
};

inline bool is_valid_decoder_output_format(const audio_format& format) noexcept {
    if (!format.interleaved || format.sample_rate == 0U || format.channel_count == 0U) {
        return false;
    }

    bool sample_layout_valid = false;
    switch (format.sample) {
    case sample_type::float32:
        sample_layout_valid = format.bits_per_sample == 32U &&
            (format.valid_bits_per_sample == 0U || format.valid_bits_per_sample == 32U);
        break;
    case sample_type::pcm_i16:
        sample_layout_valid = format.bits_per_sample == 16U &&
            (format.valid_bits_per_sample == 0U || format.valid_bits_per_sample == 16U);
        break;
    case sample_type::pcm_i24_in_32:
        sample_layout_valid = format.bits_per_sample == 32U &&
            format.valid_bits_per_sample == 24U;
        break;
    case sample_type::pcm_i32:
        sample_layout_valid = format.bits_per_sample == 32U &&
            (format.valid_bits_per_sample == 0U || format.valid_bits_per_sample == 32U);
        break;
    case sample_type::unknown:
        return false;
    }
    if (!sample_layout_valid) {
        return false;
    }

    constexpr std::uint64_t float_bytes = sizeof(float);
    const std::uint64_t average_bytes_per_second =
        static_cast<std::uint64_t>(format.sample_rate) * format.channel_count * float_bytes;
    return average_bytes_per_second <= (std::numeric_limits<std::uint32_t>::max)();
}

inline std::optional<std::size_t> checked_sample_count(
    const std::size_t frame_count,
    const std::size_t channel_count,
    const std::size_t maximum_count = (std::numeric_limits<std::size_t>::max)()) noexcept {
    if (channel_count == 0U ||
        frame_count > maximum_count / channel_count) {
        return std::nullopt;
    }
    return frame_count * channel_count;
}

inline std::optional<std::int64_t> milliseconds_to_100ns(
    const std::int64_t position_ms) noexcept {
    constexpr std::int64_t ticks_per_millisecond = 10000;
    if (position_ms < 0 ||
        position_ms > (std::numeric_limits<std::int64_t>::max)() / ticks_per_millisecond) {
        return std::nullopt;
    }
    return position_ms * ticks_per_millisecond;
}

inline std::optional<std::int64_t> advance_timestamp_100ns(
    const std::int64_t current_timestamp_100ns,
    const std::uint32_t frame_count,
    const std::uint32_t sample_rate) noexcept {
    if (current_timestamp_100ns < 0 || sample_rate == 0U) {
        return std::nullopt;
    }

    constexpr std::int64_t ticks_per_second = 10000000;
    const std::int64_t increment =
        static_cast<std::int64_t>(frame_count) * ticks_per_second /
        static_cast<std::int64_t>(sample_rate);
    if (current_timestamp_100ns >
        (std::numeric_limits<std::int64_t>::max)() - increment) {
        return std::nullopt;
    }
    return current_timestamp_100ns + increment;
}

}  // namespace sonotide::detail::win
