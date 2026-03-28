#pragma once

#include <cstdint>
#include <optional>

namespace sonotide {

enum class sample_type {
    unknown,
    pcm_i16,
    pcm_i24_in_32,
    pcm_i32,
    float32,
};

struct audio_format {
    sample_type sample = sample_type::unknown;
    std::uint32_t sample_rate = 0;
    std::uint16_t channel_count = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint16_t valid_bits_per_sample = 0;
    std::uint32_t channel_mask = 0;
    bool interleaved = true;
};

struct format_request {
    std::optional<sample_type> preferred_sample;
    std::optional<std::uint32_t> preferred_sample_rate;
    std::optional<std::uint16_t> preferred_channel_count;
    bool interleaved = true;
    bool allow_mix_format_fallback = true;
};

}  // namespace sonotide

