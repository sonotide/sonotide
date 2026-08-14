#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "sonotide/stream_config.h"

namespace sonotide::detail::win {

inline constexpr std::uint64_t qpc_ticks_per_second = 10'000'000ULL;
inline constexpr std::int64_t reference_ticks_per_millisecond = 10'000LL;

/// Возвращает 128-битное произведение как две 64-битные половины без compiler extensions.
struct uint128_parts {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
};

[[nodiscard]] inline uint128_parts multiply_u64(
    const std::uint64_t left,
    const std::uint64_t right) noexcept {
    constexpr std::uint64_t mask = 0xFFFF'FFFFULL;
    const std::uint64_t left_low = left & mask;
    const std::uint64_t left_high = left >> 32U;
    const std::uint64_t right_low = right & mask;
    const std::uint64_t right_high = right >> 32U;

    const std::uint64_t low_low = left_low * right_low;
    const std::uint64_t low_high = left_low * right_high;
    const std::uint64_t high_low = left_high * right_low;
    const std::uint64_t high_high = left_high * right_high;

    const std::uint64_t carry =
        (low_low >> 32U) + (low_high & mask) + (high_low & mask);
    return {
        high_high + (low_high >> 32U) + (high_low >> 32U) + (carry >> 32U),
        (carry << 32U) | (low_low & mask),
    };
}

[[nodiscard]] inline bool less_or_equal(
    const uint128_parts left,
    const uint128_parts right) noexcept {
    return left.high < right.high ||
           (left.high == right.high && left.low <= right.low);
}

/// Вычисляет floor(remainder * multiplier / divisor), не создавая переполнение.
/// Предусловие remainder < divisor; результат поэтому строго меньше multiplier.
[[nodiscard]] inline std::uint64_t scale_remainder(
    const std::uint64_t remainder,
    const std::uint64_t multiplier,
    const std::uint64_t divisor) noexcept {
    const auto product = multiply_u64(remainder, multiplier);
    std::uint64_t lower = 0;
    std::uint64_t upper = multiplier;
    while (lower < upper) {
        const std::uint64_t candidate = lower + (upper - lower + 1U) / 2U;
        if (less_or_equal(multiply_u64(candidate, divisor), product)) {
            lower = candidate;
        } else {
            upper = candidate - 1U;
        }
    }
    return lower;
}

/// Безопасно переводит QPC counter/frequency в 100-нс единицы.
[[nodiscard]] inline std::optional<std::uint64_t> scale_qpc_to_100ns(
    const std::int64_t counter,
    const std::int64_t frequency) noexcept {
    if (counter < 0 || frequency <= 0) {
        return std::nullopt;
    }

    const auto unsigned_counter = static_cast<std::uint64_t>(counter);
    const auto unsigned_frequency = static_cast<std::uint64_t>(frequency);
    const std::uint64_t quotient = unsigned_counter / unsigned_frequency;
    const std::uint64_t remainder = unsigned_counter % unsigned_frequency;
    if (quotient > (std::numeric_limits<std::uint64_t>::max)() / qpc_ticks_per_second) {
        return std::nullopt;
    }

    const std::uint64_t whole = quotient * qpc_ticks_per_second;
    const std::uint64_t fractional =
        scale_remainder(remainder, qpc_ticks_per_second, unsigned_frequency);
    if (whole > (std::numeric_limits<std::uint64_t>::max)() - fractional) {
        return std::nullopt;
    }
    return whole + fractional;
}

[[nodiscard]] inline std::optional<std::int64_t> milliseconds_to_reference_time(
    const std::chrono::milliseconds duration) noexcept {
    const auto count = duration.count();
    if (count <= 0 ||
        count > (std::numeric_limits<std::int64_t>::max)() /
            reference_ticks_per_millisecond) {
        return std::nullopt;
    }
    return count * reference_ticks_per_millisecond;
}

[[nodiscard]] inline std::optional<std::size_t> checked_audio_byte_count(
    const std::uint64_t frames,
    const std::uint64_t block_align) noexcept {
    if (block_align == 0 ||
        frames > (std::numeric_limits<std::size_t>::max)() / block_align) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(frames * block_align);
}

[[nodiscard]] inline bool is_valid_callback_mode(const callback_mode mode) noexcept {
    return mode == callback_mode::event_driven;
}

[[nodiscard]] inline bool is_known_share_mode(const share_mode mode) noexcept {
    return mode == share_mode::shared || mode == share_mode::exclusive;
}

}  // namespace sonotide::detail::win
