#include "internal/dsp/equalizer_response_sampler.h"

#include <cmath>
#include <complex>

namespace sonotide::detail::dsp {
namespace {

/// Математическая константа pi, используемая при вычислении АЧХ.
constexpr float kPi = 3.14159265358979323846F;

}  // namespace

float evaluate_frequency_response_db(
    const biquad_coefficients& coefficients,
    const float normalized_frequency) {
    if (!std::isfinite(normalized_frequency) ||
        !std::isfinite(coefficients.b0) || !std::isfinite(coefficients.b1) ||
        !std::isfinite(coefficients.b2) || !std::isfinite(coefficients.a1) ||
        !std::isfinite(coefficients.a2)) {
        return 0.0F;
    }
    const std::complex<float> z1 = std::exp(
        std::complex<float>(0.0F, -2.0F * kPi * normalized_frequency));
    const std::complex<float> z2 = z1 * z1;
    const std::complex<float> numerator =
        coefficients.b0 + coefficients.b1 * z1 + coefficients.b2 * z2;
    const std::complex<float> denominator = 1.0F + coefficients.a1 * z1 + coefficients.a2 * z2;
    const float denominator_magnitude = std::abs(denominator);
    if (!std::isfinite(denominator_magnitude) || denominator_magnitude == 0.0F) {
        return 0.0F;
    }
    const float magnitude = std::abs(numerator / denominator);
    if (!std::isfinite(magnitude)) {
        return 0.0F;
    }
    if (magnitude <= 0.0F) {
        return -120.0F;
    }

    return 20.0F * std::log10(magnitude);
}

float sample_equalizer_band_response_db(
    const std::span<const equalizer_band> bands,
    const float sample_rate_hz,
    const float frequency_hz) {
    if (!std::isfinite(sample_rate_hz) || !std::isfinite(frequency_hz) ||
        sample_rate_hz <= 0.0F || frequency_hz <= 0.0F || bands.empty()) {
        return 0.0F;
    }

    float response_db = 0.0F;
    for (const equalizer_band& band : bands) {
        const biquad_coefficients coefficients = make_peaking_coefficients(
            sample_rate_hz,
            band.center_frequency_hz,
            band.q_value,
            band.gain_db);
        response_db += evaluate_frequency_response_db(coefficients, frequency_hz / sample_rate_hz);
    }

    return response_db;
}

}  // namespace sonotide::detail::dsp
