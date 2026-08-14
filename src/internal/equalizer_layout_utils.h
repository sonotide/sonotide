#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "sonotide/equalizer.h"

namespace sonotide::detail {

inline std::vector<equalizer_band> normalize_equalizer_bands(
    const std::span<const equalizer_band> bands) {
    const equalizer_band_count_limits band_count_limits = supported_equalizer_band_count_limits();
    const equalizer_frequency_limits frequency_limits = supported_equalizer_frequency_limits();
    const equalizer_q_limits q_limits = supported_equalizer_q_limits();

    std::vector<equalizer_band> normalized_bands;
    normalized_bands.reserve((std::min)(bands.size(), band_count_limits.max_band_count));

    for (const equalizer_band& band : bands) {
        if (normalized_bands.size() == band_count_limits.max_band_count) {
            break;
        }

        const float finite_frequency_hz = std::isfinite(band.center_frequency_hz)
            ? band.center_frequency_hz
            : frequency_limits.min_frequency_hz;
        const float finite_gain_db = std::isfinite(band.gain_db) ? band.gain_db : 0.0F;
        const float finite_q_value = std::isfinite(band.q_value)
            ? band.q_value
            : default_equalizer_q_value;
        normalized_bands.push_back(equalizer_band{
            .center_frequency_hz = (std::clamp)(
                finite_frequency_hz,
                frequency_limits.min_frequency_hz,
                frequency_limits.max_frequency_hz),
            .gain_db = (std::clamp)(finite_gain_db, -12.0F, 12.0F),
            .q_value = (std::clamp)(finite_q_value, q_limits.min_q_value, q_limits.max_q_value),
        });
    }

    std::sort(
        normalized_bands.begin(),
        normalized_bands.end(),
        [](const equalizer_band& left, const equalizer_band& right) {
            return left.center_frequency_hz < right.center_frequency_hz;
        });

    for (std::size_t index = 1; index < normalized_bands.size(); ++index) {
        normalized_bands[index].center_frequency_hz = (std::max)(
            normalized_bands[index].center_frequency_hz,
            normalized_bands[index - 1U].center_frequency_hz + frequency_limits.min_band_spacing_hz);
    }

    if (!normalized_bands.empty()) {
        normalized_bands.back().center_frequency_hz = (std::min)(
            normalized_bands.back().center_frequency_hz,
            frequency_limits.max_frequency_hz);
    }

    for (std::size_t index = normalized_bands.size(); index > 1U; --index) {
        normalized_bands[index - 2U].center_frequency_hz = (std::min)(
            normalized_bands[index - 2U].center_frequency_hz,
            normalized_bands[index - 1U].center_frequency_hz - frequency_limits.min_band_spacing_hz);
        normalized_bands[index - 2U].center_frequency_hz = (std::max)(
            normalized_bands[index - 2U].center_frequency_hz,
            frequency_limits.min_frequency_hz);
    }

    return normalized_bands;
}

}  // namespace sonotide::detail
