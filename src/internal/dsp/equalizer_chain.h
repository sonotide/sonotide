#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "sonotide/equalizer.h"

#include "internal/dsp/biquad_filter.h"
#include "internal/dsp/output_headroom_controller.h"
#include "internal/dsp/parameter_smoother.h"

namespace sonotide::detail::dsp {

/// Полный пайплайн EQ для воспроизведения, собранный из полосовых фильтров, сглаживания и логики предусиления.
class equalizer_chain {
public:
    /// Подготавливает цепочку для конкретной частоты дискретизации и раскладки каналов.
    void configure(float sample_rate, std::size_t channel_count);
    /// Сбрасывает внутреннюю память фильтров.
    void reset();
    /// Включает или выключает обработанный путь EQ с коротким переходом.
    void set_enabled(bool enabled);
    /// Применяет все 10 значений усиления полос за один вызов.
    void set_band_gains(const std::array<float, equalizer_band_count>& band_gains_db);
    /// Устанавливает выходное усиление после EQ в децибелах.
    void set_output_gain_db(float output_gain_db);
    /// Устанавливает линейный множитель громкости воспроизведения перед финальным ограничением.
    void set_volume_linear(float volume_linear);
    /// Обрабатывает перемежающийся float PCM-буфер на месте.
    void process(float* interleaved_samples, std::size_t frame_count);

    /// Возвращает целевые значения усиления для сконфигурированных EQ-полос.
    [[nodiscard]] std::array<float, equalizer_band_count> target_band_gains_db() const;
    /// Возвращает текущую автоматическую компенсацию запаса по уровню в децибелах.
    [[nodiscard]] float headroom_compensation_db() const;
    /// Сообщает, включён ли сейчас обработанный путь EQ.
    [[nodiscard]] bool enabled() const;
    /// Возвращает сконфигурированную частоту дискретизации.
    [[nodiscard]] float sample_rate() const;
    /// Возвращает сконфигурированное число каналов.
    [[nodiscard]] std::size_t channel_count() const;
    /// Возвращает пользовательское выходное усиление EQ в децибелах.
    [[nodiscard]] float output_gain_db() const;
    /// Возвращает фиксированные центральные частоты, используемые EQ-полосами.
    [[nodiscard]] static const std::array<float, equalizer_band_count>& band_frequencies_hz();

private:
    /// Обновляет внутренние коэффициенты фильтров небольшими контрольными блоками.
    void update_filter_coefficients(std::size_t control_block_frames);

    /// Текущая частота дискретизации, которая управляет цепочкой.
    float sample_rate_ = 0.0F;
    /// Текущее число перемежающихся каналов, сконфигурированное для цепочки.
    std::size_t channel_count_ = 0;
    /// Одна секция фильтра на каждую EQ-полосу.
    std::array<biquad_filter, equalizer_band_count> filters_;
    /// Плавно изменяемые целевые значения для каждой EQ-полосы.
    std::array<parameter_smoother, equalizer_band_count> band_smoothers_;
    /// Плавный коэффициент смешивания обработанного и исходного сигнала для переключения без щелчков.
    parameter_smoother wet_mix_smoother_;
    /// Плавная компенсация предусиления для автоматического запаса по уровню.
    parameter_smoother preamp_smoother_;
    /// Плавный пользовательский выходной gain.
    parameter_smoother output_gain_smoother_;
    /// Плавный множитель громкости воспроизведения.
    parameter_smoother volume_smoother_;
    /// Последние значения усиления полос, реально применённые в текущем контрольном блоке.
    std::array<float, equalizer_band_count> current_band_gains_db_{};
    /// Запрошенные значения усиления полос, к которым цепочка должна сходиться.
    std::array<float, equalizer_band_count> target_band_gains_db_{};
    /// Вспомогательный объект для оценки безопасной компенсации предусиления.
    output_headroom_controller headroom_controller_;
    /// Пользовательский gain после EQ-обработки.
    float output_gain_db_ = 0.0F;
    /// Автоматическая компенсация, вычисленная из текущей EQ-кривой.
    float headroom_compensation_db_ = 0.0F;
    /// Включён ли обработанный путь EQ.
    bool enabled_ = false;
};

/// Возвращает встроенные пресеты эквалайзера, используемые Sonotide.
[[nodiscard]] std::vector<equalizer_preset> builtin_equalizer_presets();

}  // пространство имён sonotide::detail::dsp
