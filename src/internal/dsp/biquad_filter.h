#pragma once

#include <cstddef>
#include <vector>

namespace sonotide::detail::dsp {

/// Нормализованные коэффициенты biquad-фильтра в схеме Direct Form II Transposed.
struct biquad_coefficients {
    /// Коэффициент прямой связи для текущего входного отсчёта.
    float b0 = 1.0F;
    /// Коэффициент прямой связи для предыдущего входного отсчёта.
    float b1 = 0.0F;
    /// Коэффициент прямой связи для входного отсчёта ещё на шаг раньше.
    float b2 = 0.0F;
    /// Коэффициент обратной связи для предыдущего выходного отсчёта.
    float a1 = 0.0F;
    /// Коэффициент обратной связи для выходного отсчёта ещё на шаг раньше.
    float a2 = 0.0F;
};

/// Biquad-секция с отдельным состоянием для каждого канала.
class biquad_filter {
public:
    /// Конфигурирует фильтр под число каналов и набор коэффициентов.
    void configure(std::size_t channel_count, biquad_coefficients coefficients);
    /// Обновляет коэффициенты без очистки внутренней памяти каналов.
    void set_coefficients(biquad_coefficients coefficients);
    /// Очищает все задержанные буферы по каналам.
    void reset();
    /// Обрабатывает перемежающийся PCM-буфер на месте.
    void process(float* interleaved_samples, std::size_t frame_count, std::size_t channel_count);

private:
    /// Активные коэффициенты фильтра.
    biquad_coefficients coefficients_{};
    /// Первый элемент задержки для каждого канала.
    std::vector<float> z1_;
    /// Второй элемент задержки для каждого канала.
    std::vector<float> z2_;
};

/// Строит peaking EQ-секцию для запрошенной полосы и усиления.
[[nodiscard]] biquad_coefficients make_peaking_coefficients(
    float sample_rate,
    float center_frequency_hz,
    float q_value,
    float gain_db);

}  // пространство имён sonotide::detail::dsp
