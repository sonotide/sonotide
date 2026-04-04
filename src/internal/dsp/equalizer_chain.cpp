#include "internal/dsp/equalizer_chain.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sonotide::detail::dsp {
namespace {

/// Коэффициент Q подобран так, чтобы peaking EQ-кривая была музыкально полезной.
constexpr float kEqualizerQ = 1.414F;
/// Короткий переход для пользовательских изменений параметров.
constexpr std::size_t kRampMilliseconds = 24;
/// Размер контрольного блока, который используется для обновления коэффициентов небольшими порциями.
constexpr std::size_t kControlBlockFrames = 64;

/// Преобразует децибелы в линейную амплитуду.
float db_to_linear(const float gain_db) {
    return std::pow(10.0F, gain_db / 20.0F);
}

/// Преобразует длину перехода из миллисекунд в отсчёты для текущей частоты дискретизации.
std::size_t ramp_samples_for_rate(const float sample_rate) {
    return sample_rate > 0.0F
        ? static_cast<std::size_t>((sample_rate * static_cast<float>(kRampMilliseconds)) / 1000.0F)
        : 0U;
}

std::array<float, equalizer_max_band_count> default_band_frequencies_hz() {
    std::array<float, equalizer_max_band_count> frequencies{};
    const std::vector<equalizer_band> bands = make_default_equalizer_bands(equalizer_max_band_count);
    for (std::size_t index = 0; index < bands.size(); ++index) {
        frequencies[index] = bands[index].center_frequency_hz;
    }

    return frequencies;
}

}  // namespace

/// Конфигурирует EQ-цепочку под конкретный формат вывода.
void equalizer_chain::configure(const float sample_rate, const std::size_t channel_count) {
    /// Сначала сохраняем новые параметры выполнения, чтобы последующие методы установки использовали их сразу.
    sample_rate_ = sample_rate;
    channel_count_ = channel_count;
    const std::array<float, equalizer_max_band_count> default_frequencies_hz =
        default_band_frequencies_hz();

    for (std::size_t band_index = 0; band_index < filters_.size(); ++band_index) {
        if (target_band_frequencies_hz_[band_index] <= 0.0F) {
            target_band_frequencies_hz_[band_index] = default_frequencies_hz[band_index];
        }
        if (current_band_frequencies_hz_[band_index] <= 0.0F) {
            current_band_frequencies_hz_[band_index] = target_band_frequencies_hz_[band_index];
        }

        /// Каждая полоса на старте конфигурируется нейтральным набором коэффициентов.
        filters_[band_index].configure(
            channel_count_,
            make_peaking_coefficients(
                sample_rate_,
                target_band_frequencies_hz_[band_index],
                kEqualizerQ,
                current_band_gains_db_[band_index]));
        /// Сглаживатели сбрасываются к текущей цели, чтобы configure() не вносил скачок.
        band_gain_smoothers_[band_index].reset(target_band_gains_db_[band_index]);
        band_frequency_smoothers_[band_index].reset(target_band_frequencies_hz_[band_index]);
    }

    /// Остальные сглаживатели тоже синхронизируются с текущим состоянием.
    wet_mix_smoother_.reset(enabled_ ? 1.0F : 0.0F);
    preamp_smoother_.reset(headroom_compensation_db_);
    output_gain_smoother_.reset(output_gain_db_);
    volume_smoother_.reset(1.0F);
    current_band_gains_db_ = target_band_gains_db_;
}

/// Очищает историю фильтров, сохраняя целевые настройки.
void equalizer_chain::reset() {
    for (biquad_filter& filter : filters_) {
        filter.reset();
    }
}

/// Включает или выключает обработанный путь EQ с переходом без щелчков.
void equalizer_chain::set_enabled(const bool enabled) {
    enabled_ = enabled;
    /// Коэффициент смешивания растягивается отдельно от значений полос, чтобы обход оставался плавным.
    wet_mix_smoother_.set_target(enabled ? 1.0F : 0.0F, ramp_samples_for_rate(sample_rate_));
}

/// Обновляет желаемую раскладку полос EQ.
void equalizer_chain::set_bands(const std::span<const equalizer_band> bands) {
    const std::size_t ramp_samples = ramp_samples_for_rate(sample_rate_);
    active_band_count_ = (std::min)(bands.size(), static_cast<std::size_t>(equalizer_max_band_count));
    const std::span<const equalizer_band> active_bands = bands.first(active_band_count_);

    for (std::size_t band_index = 0; band_index < active_band_count_; ++band_index) {
        target_band_frequencies_hz_[band_index] = active_bands[band_index].center_frequency_hz;
        target_band_gains_db_[band_index] = active_bands[band_index].gain_db;
        /// Каждая полоса движется к своей цели с одинаковой длиной перехода.
        band_gain_smoothers_[band_index].set_target(target_band_gains_db_[band_index], ramp_samples);
        band_frequency_smoothers_[band_index].set_target(
            target_band_frequencies_hz_[band_index],
            ramp_samples);
    }
    for (std::size_t band_index = active_band_count_; band_index < filters_.size(); ++band_index) {
        target_band_gains_db_[band_index] = 0.0F;
        band_gain_smoothers_[band_index].reset(0.0F);
        band_frequency_smoothers_[band_index].reset(target_band_frequencies_hz_[band_index]);
    }

    headroom_compensation_db_ =
        headroom_controller_.compute_target_preamp_db(active_bands, sample_rate_);

    preamp_smoother_.set_target(headroom_compensation_db_, ramp_samples);
}

/// Сохраняет дополнительный пользовательский gain, который стоит после EQ-обработки.
void equalizer_chain::set_output_gain_db(const float output_gain_db) {
    output_gain_db_ = output_gain_db;
    output_gain_smoother_.set_target(output_gain_db_, ramp_samples_for_rate(sample_rate_));
}

/// Сохраняет общий множитель громкости воспроизведения.
void equalizer_chain::set_volume_linear(const float volume_linear) {
    volume_smoother_.set_target(volume_linear, ramp_samples_for_rate(sample_rate_));
}

/// Обрабатывает перемежающийся float PCM-буфер на месте.
void equalizer_chain::process(float* interleaved_samples, const std::size_t frame_count) {
    if (interleaved_samples == nullptr || frame_count == 0 || channel_count_ == 0) {
        /// Пустой или не сконфигурированный вход игнорируется, чтобы упростить точку вызова.
        return;
    }

    /// Копия исходного сигнала нужна, потому что обработанный путь смешивается с оригинальными отсчётами.
    std::vector<float> dry_copy(
        interleaved_samples,
        interleaved_samples + frame_count * channel_count_);

    for (std::size_t frame_offset = 0; frame_offset < frame_count; frame_offset += kControlBlockFrames) {
        /// Коэффициенты обновляются порциями размера контрольного блока, а не на каждом отсчёте.
        const std::size_t control_frames = (std::min)(kControlBlockFrames, frame_count - frame_offset);
        update_filter_coefficients(control_frames);

        /// Каждый контрольный блок работает на соответствующем поддиапазоне буфера, обрабатываемого на месте.
        float* processed_block = interleaved_samples + frame_offset * channel_count_;
        const float* dry_block = dry_copy.data() + frame_offset * channel_count_;

        for (std::size_t band_index = 0; band_index < active_band_count_; ++band_index) {
            /// Все полосы проходят последовательно через один и тот же блок, поэтому кривая накапливается.
            filters_[band_index].process(processed_block, control_frames, channel_count_);
        }

        /// Все связанные с усилением значения сглаживаются только один раз на контрольный блок.
        const float wet_mix = wet_mix_smoother_.advance(control_frames);
        const float dry_mix = 1.0F - wet_mix;
        const float preamp_linear = db_to_linear(preamp_smoother_.advance(control_frames));
        const float output_gain_linear = db_to_linear(output_gain_smoother_.advance(control_frames));
        const float volume_linear = volume_smoother_.advance(control_frames);

        for (std::size_t sample_index = 0; sample_index < control_frames * channel_count_; ++sample_index) {
            /// Обработанный путь получает предусиление и выходной gain, а исходный путь остаётся нетронутым.
            const float wet_sample =
                processed_block[sample_index] * preamp_linear * output_gain_linear;
            const float mixed_sample = dry_block[sample_index] * dry_mix + wet_sample * wet_mix;
            /// Финальное ограничение — это лишь последний запасной барьер против случайных перегрузок.
            processed_block[sample_index] = (std::clamp)(mixed_sample * volume_linear, -0.995F, 0.995F);
        }
    }
}

/// Возвращает текущую целевую раскладку полос в виде снимка.
std::vector<equalizer_band> equalizer_chain::target_bands() const {
    std::vector<equalizer_band> bands;
    bands.reserve(active_band_count_);
    for (std::size_t band_index = 0; band_index < active_band_count_; ++band_index) {
        bands.push_back(equalizer_band{
            .center_frequency_hz = target_band_frequencies_hz_[band_index],
            .gain_db = target_band_gains_db_[band_index],
        });
    }

    return bands;
}

/// Возвращает текущую оценку автоматического запаса по уровню.
float equalizer_chain::headroom_compensation_db() const {
    return headroom_compensation_db_;
}

/// Возвращает, включён ли обработанный путь EQ.
bool equalizer_chain::enabled() const {
    return enabled_;
}

/// Возвращает сконфигурированную частоту дискретизации.
float equalizer_chain::sample_rate() const {
    return sample_rate_;
}

/// Возвращает сконфигурированное число каналов.
std::size_t equalizer_chain::channel_count() const {
    return channel_count_;
}

/// Возвращает пользовательское усиление после EQ.
float equalizer_chain::output_gain_db() const {
    return output_gain_db_;
}

/// Возвращает текущее число активных полос.
std::size_t equalizer_chain::active_band_count() const {
    return active_band_count_;
}

/// Продвигает сглаживатели коэффициентов и применяет новые коэффициенты ко всем полосам.
void equalizer_chain::update_filter_coefficients(const std::size_t control_block_frames) {
    for (std::size_t band_index = 0; band_index < active_band_count_; ++band_index) {
        /// Текущее усиление полосы хранится отдельно, чтобы секция фильтра могла эволюционировать плавно.
        current_band_gains_db_[band_index] =
            band_gain_smoothers_[band_index].advance(control_block_frames);
        current_band_frequencies_hz_[band_index] =
            band_frequency_smoothers_[band_index].advance(control_block_frames);
        filters_[band_index].set_coefficients(
            make_peaking_coefficients(
                sample_rate_,
                current_band_frequencies_hz_[band_index],
                kEqualizerQ,
                current_band_gains_db_[band_index]));
    }
}

/// Возвращает встроенные пресеты эквалайзера, которыми инициализируется публичная модель эквалайзера.
std::vector<equalizer_preset> builtin_equalizer_presets() {
    /// Список пресетов намеренно задан явно, чтобы документация и интерфейс могли опираться на стабильный порядок.
    return std::vector<equalizer_preset>{
        equalizer_preset{equalizer_preset_id::flat, "Flat", {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}},
        equalizer_preset{equalizer_preset_id::bass_boost, "Bass Boost", {5.0F, 4.5F, 3.5F, 2.0F, 1.0F, 0.0F, -1.0F, -1.5F, -2.0F, -2.5F}},
        equalizer_preset{equalizer_preset_id::treble_boost, "Treble Boost", {-2.0F, -1.5F, -1.0F, -0.5F, 0.0F, 1.0F, 2.0F, 3.5F, 4.5F, 5.0F}},
        equalizer_preset{equalizer_preset_id::vocal, "Vocal", {-2.0F, -1.5F, -0.5F, 1.0F, 2.5F, 3.5F, 3.0F, 1.5F, 0.5F, -1.0F}},
        equalizer_preset{equalizer_preset_id::pop, "Pop", {-1.0F, 1.5F, 3.0F, 3.5F, 2.0F, -0.5F, -1.0F, 1.0F, 2.0F, 2.5F}},
        equalizer_preset{equalizer_preset_id::rock, "Rock", {4.0F, 3.0F, 1.0F, -1.0F, -1.5F, 0.5F, 2.0F, 3.0F, 3.5F, 4.0F}},
        equalizer_preset{equalizer_preset_id::electronic, "Electronic", {4.0F, 3.5F, 2.0F, 0.5F, -1.0F, -0.5F, 1.5F, 3.0F, 3.5F, 3.0F}},
        equalizer_preset{equalizer_preset_id::hip_hop, "Hip-Hop", {5.0F, 4.5F, 2.5F, 1.0F, -0.5F, -1.0F, 0.5F, 1.5F, 2.0F, 1.0F}},
        equalizer_preset{equalizer_preset_id::jazz, "Jazz", {1.0F, 1.5F, 1.0F, 0.5F, 1.5F, 2.5F, 2.0F, 1.5F, 1.0F, 1.0F}},
        equalizer_preset{equalizer_preset_id::classical, "Classical", {0.0F, 0.0F, 0.5F, 1.5F, 2.0F, 1.5F, 0.5F, 0.0F, 0.5F, 1.0F}},
        equalizer_preset{equalizer_preset_id::acoustic, "Acoustic", {-0.5F, 0.5F, 1.5F, 2.5F, 2.0F, 1.0F, 0.5F, 1.0F, 1.5F, 1.0F}},
        equalizer_preset{equalizer_preset_id::dance, "Dance", {4.5F, 4.0F, 2.5F, 0.5F, -0.5F, 1.0F, 2.5F, 3.5F, 4.0F, 3.0F}},
        equalizer_preset{equalizer_preset_id::piano, "Piano", {-1.0F, -0.5F, 0.5F, 1.5F, 2.5F, 3.0F, 2.0F, 1.0F, 0.5F, 0.0F}},
        equalizer_preset{equalizer_preset_id::spoken_podcast, "Spoken / Podcast", {-4.0F, -3.0F, -1.0F, 1.0F, 3.0F, 4.5F, 4.0F, 2.5F, 0.0F, -1.0F}},
        equalizer_preset{equalizer_preset_id::loudness, "Loudness", {3.0F, 2.5F, 2.0F, 1.0F, 0.0F, 0.5F, 1.5F, 2.5F, 3.0F, 3.0F}},
        equalizer_preset{equalizer_preset_id::custom, "Custom", {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}},
    };
}

}  // пространство имён sonotide::detail::dsp
