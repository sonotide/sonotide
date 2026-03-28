#pragma once

#include <cstddef>

namespace sonotide::detail::dsp {

/// Линейно интерполирует скалярный параметр на фиксированном числе отсчётов.
class parameter_smoother {
public:
    /// Создаёт сглаживатель в состоянии по умолчанию с нулевыми значениями.
    parameter_smoother() = default;

    /// Сбрасывает текущую и целевую точки к одному и тому же значению.
    void reset(float value);
    /// Назначает новое целевое значение с опциональной длиной перехода.
    void set_target(float target_value, std::size_t ramp_samples);

    /// Продвигает сглаживание и возвращает текущее значение после шага.
    [[nodiscard]] float advance(std::size_t consumed_samples);
    /// Возвращает текущее интерполированное значение.
    [[nodiscard]] float current_value() const;
    /// Возвращает ожидающее целевое значение.
    [[nodiscard]] float target_value() const;

private:
    /// Текущее выходное значение интерполяции.
    float current_value_ = 0.0F;
    /// Финальное значение, к которому движется интерполяция.
    float target_value_ = 0.0F;
    /// Оставшееся число отсчётов до достижения целевого значения.
    std::size_t remaining_samples_ = 0;
};

}  // namespace sonotide::detail::dsp
