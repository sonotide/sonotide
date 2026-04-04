#include "sonotide/equalizer.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sonotide {
namespace {

constexpr std::array<float, equalizer_max_band_count> kReferenceBandFrequenciesHz{
    60.0F,
    170.0F,
    310.0F,
    600.0F,
    1000.0F,
    3000.0F,
    6000.0F,
    12000.0F,
    14000.0F,
    16000.0F,
};

float interpolate_default_band_frequency(
    const std::size_t band_count,
    const std::size_t band_index) {
    if (band_count == 1U) {
        return kReferenceBandFrequenciesHz[kReferenceBandFrequenciesHz.size() / 2U];
    }

    const float reference_position =
        static_cast<float>(band_index) *
        static_cast<float>(kReferenceBandFrequenciesHz.size() - 1U) /
        static_cast<float>(band_count - 1U);
    const std::size_t lower_index = static_cast<std::size_t>(reference_position);
    const std::size_t upper_index = (std::min)(
        lower_index + 1U,
        kReferenceBandFrequenciesHz.size() - 1U);
    if (lower_index == upper_index) {
        return kReferenceBandFrequenciesHz[lower_index];
    }

    const float lower_frequency_hz = kReferenceBandFrequenciesHz[lower_index];
    const float upper_frequency_hz = kReferenceBandFrequenciesHz[upper_index];
    const float interpolation = reference_position - static_cast<float>(lower_index);

    const float lower_log_frequency = std::log(lower_frequency_hz);
    const float upper_log_frequency = std::log(upper_frequency_hz);
    return std::exp(
        lower_log_frequency +
        (upper_log_frequency - lower_log_frequency) * interpolation);
}

}  // namespace

equalizer_band_count_limits supported_equalizer_band_count_limits() noexcept {
    return {};
}

equalizer_frequency_limits supported_equalizer_frequency_limits() noexcept {
    return {};
}

std::vector<equalizer_band> make_default_equalizer_bands(std::size_t band_count) {
    const equalizer_band_count_limits band_count_limits = supported_equalizer_band_count_limits();
    band_count = (std::clamp)(
        band_count,
        band_count_limits.min_band_count,
        band_count_limits.max_band_count);

    std::vector<equalizer_band> bands;
    bands.reserve(band_count);
    for (std::size_t band_index = 0; band_index < band_count; ++band_index) {
        bands.push_back(equalizer_band{
            .center_frequency_hz = interpolate_default_band_frequency(band_count, band_index),
            .gain_db = 0.0F,
        });
    }

    return bands;
}

std::optional<equalizer_frequency_range> equalizer_band_editable_frequency_range(
    const std::span<const equalizer_band> bands,
    const std::size_t band_index) noexcept {
    if (band_index >= bands.size()) {
        return std::nullopt;
    }

    const equalizer_frequency_limits frequency_limits = supported_equalizer_frequency_limits();
    float min_frequency_hz = frequency_limits.min_frequency_hz;
    float max_frequency_hz = frequency_limits.max_frequency_hz;

    if (band_index > 0U) {
        min_frequency_hz = (std::max)(
            min_frequency_hz,
            bands[band_index - 1U].center_frequency_hz + frequency_limits.min_band_spacing_hz);
    }
    if (band_index + 1U < bands.size()) {
        max_frequency_hz = (std::min)(
            max_frequency_hz,
            bands[band_index + 1U].center_frequency_hz - frequency_limits.min_band_spacing_hz);
    }

    if (min_frequency_hz > max_frequency_hz) {
        return std::nullopt;
    }

    return equalizer_frequency_range{
        .min_frequency_hz = min_frequency_hz,
        .max_frequency_hz = max_frequency_hz,
    };
}

}  // namespace sonotide
