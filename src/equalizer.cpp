#include "sonotide/equalizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "internal/equalizer_layout_utils.h"
#include "internal/dsp/equalizer_response_sampler.h"
#include "internal/dsp/output_headroom_controller.h"

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

float clamp_equalizer_gain_db(const float gain_db) {
    if (!std::isfinite(gain_db)) {
        return 0.0F;
    }
    return (std::clamp)(gain_db, -12.0F, 12.0F);
}

bool is_finite_band(const equalizer_band& band) noexcept {
    return std::isfinite(band.center_frequency_hz) &&
           std::isfinite(band.gain_db) &&
           std::isfinite(band.q_value);
}

result<equalizer_response_curve> invalid_response_argument(std::string message) {
    error failure;
    failure.category = error_category::configuration;
    failure.code = error_code::invalid_argument;
    failure.operation = "sample_equalizer_response";
    failure.message = std::move(message);
    return result<equalizer_response_curve>::failure(std::move(failure));
}

}  // namespace

equalizer_band_count_limits supported_equalizer_band_count_limits() noexcept {
    return {};
}

equalizer_frequency_limits supported_equalizer_frequency_limits() noexcept {
    return {};
}

equalizer_q_limits supported_equalizer_q_limits() noexcept {
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
            .q_value = default_equalizer_q_value,
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
    if (!std::all_of(bands.begin(), bands.end(), is_finite_band)) {
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

result<equalizer_response_curve> sample_equalizer_response(
    const equalizer_state& state,
    const float sample_rate_hz,
    const std::span<const float> frequencies_hz) {
    if (!std::isfinite(sample_rate_hz) || sample_rate_hz <= 0.0F) {
        return invalid_response_argument("Sample rate must be finite and greater than zero.");
    }
    if (frequencies_hz.empty()) {
        return invalid_response_argument(
            "At least one frequency point is required to sample the equalizer response.");
    }
    if (!std::isfinite(state.output_gain_db)) {
        return invalid_response_argument("Equalizer output gain must be finite.");
    }
    if (!std::all_of(state.bands.begin(), state.bands.end(), is_finite_band)) {
        return invalid_response_argument(
            "Equalizer band frequency, gain, and Q values must be finite.");
    }

    const float nyquist_frequency_hz = sample_rate_hz * 0.5F;
    for (const float frequency_hz : frequencies_hz) {
        if (!std::isfinite(frequency_hz) || frequency_hz <= 0.0F ||
            frequency_hz > nyquist_frequency_hz) {
            return invalid_response_argument(
                "Requested response frequency must be finite, greater than zero, and not exceed Nyquist.");
        }
    }

    equalizer_response_curve response_curve;
    response_curve.sample_rate_hz = sample_rate_hz;
    response_curve.enabled = state.enabled;
    response_curve.applied_output_gain_db = state.enabled ? clamp_equalizer_gain_db(state.output_gain_db) : 0.0F;
    response_curve.points.reserve(frequencies_hz.size());

    const std::vector<equalizer_band> sanitized_bands = detail::normalize_equalizer_bands(state.bands);
    if (state.enabled) {
        detail::dsp::output_headroom_controller headroom_controller;
        response_curve.applied_headroom_compensation_db =
            headroom_controller.compute_target_preamp_db(sanitized_bands, sample_rate_hz);
    }

    for (const float frequency_hz : frequencies_hz) {
        float response_db = 0.0F;
        if (state.enabled) {
            response_db = detail::dsp::sample_equalizer_band_response_db(
                sanitized_bands,
                sample_rate_hz,
                frequency_hz);
            response_db += response_curve.applied_headroom_compensation_db;
            response_db += response_curve.applied_output_gain_db;
        }

        response_curve.points.push_back(equalizer_response_point{
            .frequency_hz = frequency_hz,
            .response_db = response_db,
        });
    }

    return result<equalizer_response_curve>::success(std::move(response_curve));
}

}  // namespace sonotide
