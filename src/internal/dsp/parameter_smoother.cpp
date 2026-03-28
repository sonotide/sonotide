#include "internal/dsp/parameter_smoother.h"

namespace sonotide::detail::dsp {

/// Сбрасывает сглаживатель в одно стабильное значение без ожидающего перехода.
void parameter_smoother::reset(const float value) {
    /// Сброс обеих точек делает следующий advance() детерминированным.
    current_value_ = value;
    target_value_ = value;
    remaining_samples_ = 0;
}

/// Запоминает новое целевое значение и при необходимости растягивает переход на отсчёты.
void parameter_smoother::set_target(const float target_value, const std::size_t ramp_samples) {
    target_value_ = target_value;
    if (ramp_samples == 0) {
        /// Переход нулевой длины ведёт себя как мгновенный скачок параметра.
        current_value_ = target_value_;
        remaining_samples_ = 0;
        return;
    }

    /// Текущее значение остаётся прежним, а advance() будет вести его к новой цели.
    remaining_samples_ = ramp_samples;
}

/// Продвигает интерполяцию на запрошенное число отсчётов.
float parameter_smoother::advance(const std::size_t consumed_samples) {
    if (remaining_samples_ == 0) {
        return current_value_;
    }

    if (consumed_samples >= remaining_samples_) {
        /// Когда шаг доходит до конца перехода, точно фиксируем целевое значение.
        current_value_ = target_value_;
        remaining_samples_ = 0;
        return current_value_;
    }

    /// Линейной интерполяции достаточно, потому что сглаживатель нужен только для переходов уровня интерфейса.
    const float delta = target_value_ - current_value_;
    current_value_ += delta * (
        static_cast<float>(consumed_samples) /
        static_cast<float>(remaining_samples_));
    remaining_samples_ -= consumed_samples;
    return current_value_;
}

/// Возвращает текущее интерполированное значение.
float parameter_smoother::current_value() const {
    return current_value_;
}

/// Возвращает ожидающее целевое значение.
float parameter_smoother::target_value() const {
    return target_value_;
}

}  // namespace sonotide::detail::dsp
