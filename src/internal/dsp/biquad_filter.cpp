#include "internal/dsp/biquad_filter.h"

#include <algorithm>
#include <cmath>

namespace sonotide::detail::dsp {
namespace {

/// Константа pi, используемая при генерации коэффициентов.
constexpr float kPi = 3.14159265358979323846F;

}  // namespace

/// Конфигурирует секцию под запрошенное число каналов и набор коэффициентов.
void biquad_filter::configure(
    const std::size_t channel_count,
    const biquad_coefficients coefficients) {
    /// Коэффициенты хранятся по значению, а линии задержки пересоздаются по числу каналов.
    coefficients_ = coefficients;
    if (z1_.size() != channel_count || z2_.size() != channel_count) {
        /// Пересоздание очищает состояние при смене раскладки каналов.
        z1_.assign(channel_count, 0.0F);
        z2_.assign(channel_count, 0.0F);
    }
}

/// Меняет активные коэффициенты без очистки буферов задержки.
void biquad_filter::set_coefficients(const biquad_coefficients coefficients) {
    /// Это позволяет плавно менять коэффициенты без слышимых щелчков.
    coefficients_ = coefficients;
}

/// Очищает линии задержки по каждому каналу.
void biquad_filter::reset() {
    std::fill(z1_.begin(), z1_.end(), 0.0F);
    std::fill(z2_.begin(), z2_.end(), 0.0F);
}

/// Обрабатывает перемежающийся аудиосигнал на месте через секцию Direct Form II Transposed.
void biquad_filter::process(
    float* interleaved_samples,
    const std::size_t frame_count,
    const std::size_t channel_count) {
    if (interleaved_samples == nullptr || channel_count == 0 ||
        z1_.size() != channel_count || z2_.size() != channel_count) {
        /// Некорректный или не сконфигурированный вход намеренно игнорируется, чтобы упростить точки вызова.
        return;
    }

    for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        /// Каждый кадр обрабатывается по каналам, чтобы сохранить перемежающуюся раскладку.
        float* frame = interleaved_samples + frame_index * channel_count;
        for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
            /// Direct Form II Transposed держит состояние компактным и стабильным при обновлениях.
            const float input = frame[channel_index];
            const float output = coefficients_.b0 * input + z1_[channel_index];
            z1_[channel_index] =
                coefficients_.b1 * input - coefficients_.a1 * output + z2_[channel_index];
            z2_[channel_index] = coefficients_.b2 * input - coefficients_.a2 * output;
            frame[channel_index] = output;
        }
    }
}

/// Создаёт набор коэффициентов peaking EQ с безопасным ограничением центральной частоты.
biquad_coefficients make_peaking_coefficients(
    const float sample_rate,
    const float center_frequency_hz,
    const float q_value,
    const float gain_db) {
    if (sample_rate <= 0.0F || center_frequency_hz <= 0.0F || q_value <= 0.0F) {
        /// Некорректные входные параметры откатываются к нейтральной секции без обработки.
        return {};
    }

    /// Верхние полосы должны оставаться ниже частоты Найквиста, иначе фильтр становится численно хрупким.
    const float max_supported_frequency_hz = (sample_rate * 0.5F) * 0.9F;
    const float safe_center_frequency_hz =
        (std::clamp)(center_frequency_hz, 10.0F, max_supported_frequency_hz);
    /// Стандартная формула peaking EQ хорошо работает для фиксированной раскладки полос Sonotide.
    const float a = std::pow(10.0F, gain_db / 40.0F);
    const float omega = 2.0F * kPi * (safe_center_frequency_hz / sample_rate);
    const float sin_omega = std::sin(omega);
    const float cos_omega = std::cos(omega);
    const float alpha = sin_omega / (2.0F * q_value);

    const float b0 = 1.0F + alpha * a;
    const float b1 = -2.0F * cos_omega;
    const float b2 = 1.0F - alpha * a;
    const float a0 = 1.0F + alpha / a;
    const float a1 = -2.0F * cos_omega;
    const float a2 = 1.0F - alpha / a;

    return biquad_coefficients{
        .b0 = b0 / a0,
        .b1 = b1 / a0,
        .b2 = b2 / a0,
        .a1 = a1 / a0,
        .a2 = a2 / a0,
    };
}

}  // пространство имён sonotide::detail::dsp
