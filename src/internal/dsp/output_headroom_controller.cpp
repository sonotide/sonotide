#include "internal/dsp/output_headroom_controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

#include "internal/dsp/biquad_filter.h"

namespace sonotide::detail::dsp {
namespace {

/// Математическая константа pi, используемая при вычислении АЧХ.
constexpr float kPi = 3.14159265358979323846F;
/// Тот же коэффициент Q, что и в EQ chain, чтобы оценка запаса по уровню совпадала с рабочим путём.
constexpr float kEqualizerQ = 1.414F;
/// Фиксированные центральные частоты, зеркально повторяющие EQ chain.
constexpr std::array<float, equalizer_band_count> kBandFrequenciesHz{
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

/// Вычисляет частотную характеристику одного biquad в децибелах.
float evaluate_frequency_response_db(
    const biquad_coefficients& coefficients,
    const float normalized_frequency) {
    /// АЧХ вычисляется на единичной окружности, потому что нас интересует только модуль.
    const std::complex<float> z1 = std::exp(
        std::complex<float>(0.0F, -2.0F * kPi * normalized_frequency));
    const std::complex<float> z2 = z1 * z1;
    const std::complex<float> numerator =
        coefficients.b0 + coefficients.b1 * z1 + coefficients.b2 * z2;
    const std::complex<float> denominator = 1.0F + coefficients.a1 * z1 + coefficients.a2 * z2;
    const float magnitude = std::abs(numerator / denominator);
    if (magnitude <= 0.0F) {
        return -120.0F;
    }

    return 20.0F * std::log10(magnitude);
}

}  // namespace

/// Вычисляет консервативное значение предусиления, уменьшающее риск клиппинга.
float output_headroom_controller::compute_target_preamp_db(
    const std::array<float, equalizer_band_count>& band_gains_db,
    const float sample_rate) const {
    if (sample_rate <= 0.0F) {
        /// Без валидной частоты дискретизации безопасно оценить отклик полос невозможно.
        return 0.0F;
    }

    /// АЧХ берётся в ограниченном числе точек, чтобы оценка оставалась дешёвой.
    float max_response_db = 0.0F;
    constexpr int frequency_points = 96;

    for (int point_index = 0; point_index < frequency_points; ++point_index) {
        /// Логарифмически распределённые частоты покрывают слышимый диапазон равномернее, чем линейные.
        const float ratio = static_cast<float>(point_index) /
            static_cast<float>(frequency_points - 1);
        const float frequency_hz =
            (std::min)(20.0F * std::pow(1000.0F, ratio), sample_rate * 0.45F);
        float response_db = 0.0F;

        for (std::size_t band_index = 0; band_index < band_gains_db.size(); ++band_index) {
            /// Каждая EQ-полоса вносит вклад в общую оценку АЧХ.
            const biquad_coefficients coefficients = make_peaking_coefficients(
                sample_rate,
                kBandFrequenciesHz[band_index],
                kEqualizerQ,
                band_gains_db[band_index]);
            response_db += evaluate_frequency_response_db(coefficients, frequency_hz / sample_rate);
        }

        max_response_db = (std::max)(max_response_db, response_db);
    }

    /// Поверх оценённого пикового отклика оставляется небольшой запас безопасности.
    return max_response_db > 0.0F ? -(max_response_db + 1.0F) : 0.0F;
}

}  // пространство имён sonotide::detail::dsp
