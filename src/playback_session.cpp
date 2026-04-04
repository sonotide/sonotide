#include "sonotide/playback_session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "internal/dsp/equalizer_chain.h"
#include "internal/runtime_backend.h"

#if defined(_WIN32)
#include "internal/win/media_foundation_decoder.h"
#endif

namespace sonotide {
namespace {

// Преобразует громкость транспорта из пользовательского диапазона 0..100 в линейный коэффициент.
float volume_percent_to_linear(const int volume_percent) {
    return static_cast<float>((std::clamp)(volume_percent, 0, 100)) / 100.0F;
}

constexpr float kMinEqualizerGainDb = -12.0F;
constexpr float kMaxEqualizerGainDb = 12.0F;

float clamp_equalizer_gain_db(const float gain_db) {
    return (std::clamp)(gain_db, kMinEqualizerGainDb, kMaxEqualizerGainDb);
}

// Сравнивает два согласованных аудиоформата, чтобы декодер переоткрывался только при необходимости.
bool formats_match(const audio_format& left, const audio_format& right) {
    return left.sample == right.sample &&
           left.sample_rate == right.sample_rate &&
           left.channel_count == right.channel_count &&
           left.bits_per_sample == right.bits_per_sample &&
           left.valid_bits_per_sample == right.valid_bits_per_sample &&
           left.channel_mask == right.channel_mask &&
           left.interleaved == right.interleaved;
}

// Сравнивает текущие band gains с эталонной кривой пресета.
bool band_gains_match(
    const std::span<const equalizer_band> bands,
    const std::span<const float> gains_db) {
    if (bands.size() != gains_db.size()) {
        return false;
    }

    for (std::size_t index = 0; index < bands.size(); ++index) {
        if (std::fabs(bands[index].gain_db - gains_db[index]) > 0.01F) {
            return false;
        }
    }

    return true;
}

// Интерполирует reference gain-кривую пресета в текущие пользовательские частоты полос.
std::vector<float> project_preset_gains_to_bands(
    const equalizer_preset& preset,
    const std::span<const equalizer_band> target_bands) {
    std::vector<float> projected_gains_db;
    projected_gains_db.reserve(target_bands.size());

    if (target_bands.empty()) {
        return projected_gains_db;
    }
    if (preset.gains_db.empty()) {
        projected_gains_db.assign(target_bands.size(), 0.0F);
        return projected_gains_db;
    }

    const std::vector<equalizer_band> preset_reference_bands =
        make_default_equalizer_bands(preset.gains_db.size());
    if (preset_reference_bands.empty()) {
        projected_gains_db.assign(target_bands.size(), 0.0F);
        return projected_gains_db;
    }

    for (const equalizer_band& band : target_bands) {
        if (band.center_frequency_hz <= preset_reference_bands.front().center_frequency_hz) {
            projected_gains_db.push_back(preset.gains_db.front());
            continue;
        }
        if (band.center_frequency_hz >= preset_reference_bands.back().center_frequency_hz) {
            projected_gains_db.push_back(preset.gains_db.back());
            continue;
        }

        const auto upper_iterator = std::upper_bound(
            preset_reference_bands.begin(),
            preset_reference_bands.end(),
            band.center_frequency_hz,
            [](const float frequency_hz, const equalizer_band& reference_band) {
                return frequency_hz < reference_band.center_frequency_hz;
            });
        const std::size_t upper_index = static_cast<std::size_t>(
            std::distance(preset_reference_bands.begin(), upper_iterator));
        const std::size_t lower_index = upper_index - 1U;

        const float lower_frequency_hz = preset_reference_bands[lower_index].center_frequency_hz;
        const float upper_frequency_hz = preset_reference_bands[upper_index].center_frequency_hz;
        const float lower_gain_db = preset.gains_db[lower_index];
        const float upper_gain_db = preset.gains_db[upper_index];
        const float lower_log_frequency = std::log(lower_frequency_hz);
        const float upper_log_frequency = std::log(upper_frequency_hz);
        const float target_log_frequency = std::log(band.center_frequency_hz);
        const float interpolation =
            (target_log_frequency - lower_log_frequency) /
            (upper_log_frequency - lower_log_frequency);

        projected_gains_db.push_back(
            lower_gain_db + (upper_gain_db - lower_gain_db) * interpolation);
    }

    return projected_gains_db;
}

// Нормализует пользовательскую раскладку полос: сортирует, ограничивает диапазон и выдерживает минимальный зазор.
std::vector<equalizer_band> normalize_equalizer_bands(std::span<const equalizer_band> bands) {
    std::vector<equalizer_band> normalized_bands(bands.begin(), bands.end());
    const equalizer_frequency_limits frequency_limits = supported_equalizer_frequency_limits();

    std::sort(
        normalized_bands.begin(),
        normalized_bands.end(),
        [](const equalizer_band& left, const equalizer_band& right) {
            return left.center_frequency_hz < right.center_frequency_hz;
        });

    for (equalizer_band& band : normalized_bands) {
        band.center_frequency_hz = (std::clamp)(
            band.center_frequency_hz,
            frequency_limits.min_frequency_hz,
            frequency_limits.max_frequency_hz);
        band.gain_db = clamp_equalizer_gain_db(band.gain_db);
    }

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

// Заполняет весь render buffer нулями, когда данных источника ещё нет.
void write_silence(audio_buffer_view buffer) {
    std::fill(buffer.bytes.begin(), buffer.bytes.end(), std::byte{0});
}

// Преобразует декодированный float PCM в согласованный выходной формат до возврата из обратного вызова рендеринга.
void convert_float_to_buffer(
    const float* source_samples,
    const std::uint32_t frame_count,
    const audio_format& format,
    std::span<std::byte> destination) {
    const std::size_t sample_count = static_cast<std::size_t>(frame_count) * format.channel_count;
    if (format.sample == sample_type::float32) {
        std::memcpy(destination.data(), source_samples, sample_count * sizeof(float));
        return;
    }

    if (format.sample == sample_type::pcm_i16) {
        auto* samples = reinterpret_cast<std::int16_t*>(destination.data());
        for (std::size_t index = 0; index < sample_count; ++index) {
            const float value = (std::clamp)(source_samples[index], -1.0F, 1.0F);
            samples[index] = static_cast<std::int16_t>(value * 32767.0F);
        }
        return;
    }

    if (format.sample == sample_type::pcm_i24_in_32 || format.sample == sample_type::pcm_i32) {
        auto* samples = reinterpret_cast<std::int32_t*>(destination.data());
        const float scale =
            format.sample == sample_type::pcm_i24_in_32 ? 8388607.0F : 2147483647.0F;
        for (std::size_t index = 0; index < sample_count; ++index) {
            const float value = (std::clamp)(source_samples[index], -1.0F, 1.0F);
            samples[index] = static_cast<std::int32_t>(value * scale);
        }
        return;
    }

    write_silence(audio_buffer_view{destination, frame_count, format});
}

// Собирает проектный объект ошибки, который используют методы playback_session.
error make_error(
    const error_category category,
    const error_code code,
    std::string operation,
    std::string message) {
    error failure;
    failure.category = category;
    failure.code = code;
    failure.operation = std::move(operation);
    failure.message = std::move(message);
    return failure;
}

#if !defined(_WIN32)
// Небольшой запасной блок для сборок не под Windows, чтобы код продолжал собираться.
struct fallback_decoded_audio_block {
    // Сырые декодированные PCM-сэмплы или эквивалент тишины.
    std::vector<float> samples;
    // Текущая позиция воспроизведения в миллисекундах.
    std::int64_t position_ms = 0;
    // Полная длительность источника в миллисекундах.
    std::int64_t duration_ms = 0;
    // Сигнализирует, что декодер дошёл до конца источника.
    bool end_of_stream = false;
};
#endif

#if defined(_WIN32)
// В сборках под Windows используется реальный блок декодера Media Foundation.
using decoded_audio_block_result = result<detail::win::decoded_audio_block>;
#else
// Сборки не под Windows используют запасной блок только для совместимости при компиляции.
using decoded_audio_block_result = result<fallback_decoded_audio_block>;
#endif

}  // namespace

class playback_session::implementation {
public:
    // Создаёт полностью инициализированную сессию и сразу запускает поток рендеринга.
    static result<playback_session> create(
        std::shared_ptr<detail::runtime_backend> backend,
        const playback_session_config& config) {
        // Сначала создаём объект, чтобы при ошибке открытия можно было безопасно вернуть структурированную ошибку.
        auto instance = std::unique_ptr<implementation>(
            new implementation(std::move(backend), config));
        // Поток рендеринга должен быть открыт до того, как публичный объект будет возвращён.
        auto open_result = instance->open_render_stream();
        if (!open_result) {
            return result<playback_session>::failure(open_result.error());
        }

        // Восстановление ждёт в отдельном потоке, чтобы обратный вызов рендеринга оставался лёгким.
        instance->recovery_thread_ = std::thread([owner = instance.get()]() {
            owner->recovery_loop();
        });

        return result<playback_session>::success(
            playback_session(std::move(instance)));
    }

    // Сохраняет внутреннюю реализацию и конфигурацию сессии, затем инициализирует снимок воспроизведения.
    implementation(
        std::shared_ptr<detail::runtime_backend> backend,
        playback_session_config config)
        : backend_(std::move(backend)),
          config_(std::move(config)),
          callback_(*this) {
        // Сохраняем запрошенное предпочтительное устройство, чтобы снимок отражал намерение вызывающего.
        state_.preferred_output_device_id =
            config_.render.device.selection_mode == device_selector::mode::explicit_id
                ? config_.render.device.device_id
                : "";
        // Применяем стартовую громкость пользователя до первого вызова обратного вызова рендеринга.
        state_.volume_percent = (std::clamp)(config_.initial_volume_percent, 0, 100);
        // Заполняем снимок эквалайзера, чтобы публичное состояние было осмысленным ещё до старта воспроизведения.
        populate_default_equalizer_state_locked();
        if (config_.initial_equalizer_state.has_value()) {
            (void)apply_equalizer_state(*config_.initial_equalizer_state);
        }
    }

    // Гарантирует выполнение логики завершения даже если вызывающий не закрыл сессию явно.
    ~implementation() {
        close();
    }

    // Показывает, владеет ли сессия ещё живым состоянием runtime.
    bool is_open() const noexcept {
        std::scoped_lock lock(mutex_);
        return !closed_;
    }

    // Загружает новый URI источника и помечает сессию как требующую переинициализации декодера.
    result<void> load(std::string source_uri) {
        if (source_uri.empty()) {
            return result<void>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::load",
                "Source URI must not be empty."));
        }

        std::scoped_lock lock(mutex_);
        // Счётчики поколений позволяют обратному вызову рендеринга отличать устаревшие запросы на загрузку.
        requested_source_uri_ = std::move(source_uri);
        ++requested_source_generation_;
        // Отложенный seek очищается, потому что новый источник должен стартовать с начала.
        pending_seek_ms_.reset();
        state_.source_uri = requested_source_uri_;
        state_.status = config_.auto_play_on_load ? playback_status::loading : playback_status::paused;
        state_.error_message.clear();
        state_.position_ms = 0;
        state_.duration_ms = 0;
        state_.device_lost = false;
        state_.completion_token = 0;
        playback_intent_playing_ = config_.auto_play_on_load;
        decoder_ready_ = false;
        reached_end_of_stream_ = false;
        equalizer_state_.error_message.clear();
        return result<void>::success();
    }

    // Запрашивает запуск транспорта; реальная работа с декодером остаётся за рабочим потоком.
    result<void> play() {
        std::scoped_lock lock(mutex_);
        if (requested_source_uri_.empty()) {
            state_.status = playback_status::error;
            state_.error_message = "Playback session has no loaded source.";
            return result<void>::failure(make_error(
                error_category::stream,
                error_code::invalid_state,
                "playback_session::play",
                state_.error_message));
        }

        if (reached_end_of_stream_) {
            pending_seek_ms_ = 0;
            reached_end_of_stream_ = false;
        }

        playback_intent_playing_ = true;
        state_.status = decoder_ready_ ? playback_status::playing : playback_status::loading;
        state_.error_message.clear();
        return result<void>::success();
    }

    // Запрашивает паузу транспорта, сохраняя загруженный источник и снимок таймлайна.
    result<void> pause() {
        std::scoped_lock lock(mutex_);
        playback_intent_playing_ = false;
        state_.status = requested_source_uri_.empty() ? playback_status::idle : playback_status::paused;
        state_.error_message.clear();
        return result<void>::success();
    }

    // Планирует seek декодера на следующем тике обратного вызова рендеринга.
    result<void> seek_to(std::int64_t position_ms) {
        std::scoped_lock lock(mutex_);
        if (requested_source_uri_.empty()) {
            return result<void>::failure(make_error(
                error_category::stream,
                error_code::invalid_state,
                "playback_session::seek_to",
                "Cannot seek before a source has been loaded."));
        }

        pending_seek_ms_ = (std::max)(position_ms, static_cast<std::int64_t>(0));
        reached_end_of_stream_ = false;
        state_.position_ms = *pending_seek_ms_;
        state_.status = playback_intent_playing_ ? playback_status::loading : playback_status::paused;
        state_.error_message.clear();
        return result<void>::success();
    }

    // Сохраняет пользовательскую громкость в процентах, а слой DSP преобразует её позже.
    result<void> set_volume_percent(int volume_percent) {
        std::scoped_lock lock(mutex_);
        state_.volume_percent = (std::clamp)(volume_percent, 0, 100);
        return result<void>::success();
    }

    result<void> set_equalizer_enabled(const bool enabled) {
        std::scoped_lock lock(mutex_);
        equalizer_state_.enabled = enabled;
        recompute_equalizer_metadata_locked(current_sample_rate_or_default_locked());
        return result<void>::success();
    }

    result<void> select_equalizer_preset(const equalizer_preset_id preset_id) {
        std::scoped_lock lock(mutex_);
        if (preset_id == equalizer_preset_id::custom) {
            return result<void>::success();
        }

        const auto preset_iterator = std::find_if(
            available_equalizer_presets_.begin(),
            available_equalizer_presets_.end(),
            [preset_id](const equalizer_preset& preset) {
                return preset.id == preset_id;
            });
        if (preset_iterator == available_equalizer_presets_.end()) {
            return result<void>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::select_equalizer_preset",
                "Unknown equalizer preset."));
        }

        const std::vector<float> projected_gains_db = project_preset_gains_to_bands(
            *preset_iterator,
            equalizer_state_.bands);
        for (std::size_t index = 0; index < equalizer_state_.bands.size(); ++index) {
            equalizer_state_.bands[index].gain_db = projected_gains_db[index];
        }
        equalizer_state_.active_preset_id = preset_id;
        update_last_nonflat_state_locked();
        recompute_equalizer_metadata_locked(current_sample_rate_or_default_locked());
        return result<void>::success();
    }

    result<void> set_equalizer_band_gain(const std::size_t band_index, const float gain_db) {
        std::scoped_lock lock(mutex_);
        if (band_index >= equalizer_state_.bands.size()) {
            return result<void>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::set_equalizer_band_gain",
                "Equalizer band index is outside of the active band range."));
        }

        equalizer_state_.bands[band_index].gain_db = clamp_equalizer_gain_db(gain_db);
        recalculate_active_preset_locked();
        update_last_nonflat_state_locked();
        recompute_equalizer_metadata_locked(current_sample_rate_or_default_locked());
        return result<void>::success();
    }

    result<void> add_equalizer_band(const float center_frequency_hz, const float gain_db) {
        std::scoped_lock lock(mutex_);
        if (equalizer_state_.bands.size() >= supported_equalizer_band_count_limits().max_band_count) {
            return result<void>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::add_equalizer_band",
                "Equalizer already uses the maximum supported number of bands."));
        }

        const equalizer_frequency_limits frequency_limits = supported_equalizer_frequency_limits();
        const float clamped_frequency_hz = (std::clamp)(
            center_frequency_hz,
            frequency_limits.min_frequency_hz,
            frequency_limits.max_frequency_hz);
        const auto insert_iterator = std::lower_bound(
            equalizer_state_.bands.begin(),
            equalizer_state_.bands.end(),
            clamped_frequency_hz,
            [](const equalizer_band& band, const float frequency_hz) {
                return band.center_frequency_hz < frequency_hz;
            });
        const std::size_t insert_index = static_cast<std::size_t>(
            std::distance(equalizer_state_.bands.begin(), insert_iterator));

        float min_frequency_hz = frequency_limits.min_frequency_hz;
        float max_frequency_hz = frequency_limits.max_frequency_hz;
        if (insert_index > 0U) {
            min_frequency_hz = (std::max)(
                min_frequency_hz,
                equalizer_state_.bands[insert_index - 1U].center_frequency_hz +
                    frequency_limits.min_band_spacing_hz);
        }
        if (insert_index < equalizer_state_.bands.size()) {
            max_frequency_hz = (std::min)(
                max_frequency_hz,
                equalizer_state_.bands[insert_index].center_frequency_hz -
                    frequency_limits.min_band_spacing_hz);
        }
        if (min_frequency_hz > max_frequency_hz) {
            return result<void>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::add_equalizer_band",
                "No valid frequency slot is available for another equalizer band."));
        }

        equalizer_state_.bands.insert(
            insert_iterator,
            equalizer_band{
                .center_frequency_hz = (std::clamp)(clamped_frequency_hz, min_frequency_hz, max_frequency_hz),
                .gain_db = clamp_equalizer_gain_db(gain_db),
            });
        update_last_nonflat_state_locked();
        recalculate_active_preset_locked();
        recompute_equalizer_metadata_locked(current_sample_rate_or_default_locked());
        return result<void>::success();
    }

    result<void> remove_equalizer_band(const std::size_t band_index) {
        std::scoped_lock lock(mutex_);
        if (band_index >= equalizer_state_.bands.size()) {
            return result<void>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::remove_equalizer_band",
                "Equalizer band index is outside of the active band range."));
        }

        equalizer_state_.bands.erase(equalizer_state_.bands.begin() + static_cast<std::ptrdiff_t>(band_index));
        update_last_nonflat_state_locked();
        recalculate_active_preset_locked();
        recompute_equalizer_metadata_locked(current_sample_rate_or_default_locked());
        return result<void>::success();
    }

    result<void> set_equalizer_band_frequency(
        const std::size_t band_index,
        const float center_frequency_hz) {
        std::scoped_lock lock(mutex_);
        if (band_index >= equalizer_state_.bands.size()) {
            return result<void>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::set_equalizer_band_frequency",
                "Equalizer band index is outside of the active band range."));
        }

        const auto editable_range = sonotide::equalizer_band_editable_frequency_range(
            equalizer_state_.bands,
            band_index);
        if (!editable_range.has_value()) {
            return result<void>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::set_equalizer_band_frequency",
                "The requested band cannot be moved because its editable range is unavailable."));
        }

        equalizer_state_.bands[band_index].center_frequency_hz = (std::clamp)(
            center_frequency_hz,
            editable_range->min_frequency_hz,
            editable_range->max_frequency_hz);
        recalculate_active_preset_locked();
        update_last_nonflat_state_locked();
        recompute_equalizer_metadata_locked(current_sample_rate_or_default_locked());
        return result<void>::success();
    }

    result<void> reset_equalizer() {
        auto preset_result = select_equalizer_preset(equalizer_preset_id::flat);
        if (!preset_result) {
            return preset_result;
        }

        std::scoped_lock lock(mutex_);
        equalizer_state_.output_gain_db = 0.0F;
        recompute_equalizer_metadata_locked(current_sample_rate_or_default_locked());
        return result<void>::success();
    }

    result<void> set_equalizer_output_gain(const float output_gain_db) {
        std::scoped_lock lock(mutex_);
        equalizer_state_.output_gain_db = clamp_equalizer_gain_db(output_gain_db);
        return result<void>::success();
    }

    result<void> apply_equalizer_state(const sonotide::equalizer_state& state) {
        std::scoped_lock lock(mutex_);
        if (state.bands.size() > supported_equalizer_band_count_limits().max_band_count) {
            return result<void>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::apply_equalizer_state",
                "Equalizer state exceeds the maximum supported number of bands."));
        }

        equalizer_state_.enabled = state.enabled;
        equalizer_state_.bands = normalize_equalizer_bands(state.bands);
        equalizer_state_.last_nonflat_band_gains_db.assign(equalizer_state_.bands.size(), 0.0F);
        for (std::size_t index = 0; index < equalizer_state_.bands.size(); ++index) {
            if (index < state.last_nonflat_band_gains_db.size()) {
                equalizer_state_.last_nonflat_band_gains_db[index] =
                    clamp_equalizer_gain_db(state.last_nonflat_band_gains_db[index]);
            }
        }
        equalizer_state_.output_gain_db = clamp_equalizer_gain_db(state.output_gain_db);
        recalculate_active_preset_locked();
        update_last_nonflat_state_locked();
        recompute_equalizer_metadata_locked(current_sample_rate_or_default_locked());
        return result<void>::success();
    }

    result<std::vector<device_info>> list_output_devices() const {
        return backend_->enumerate_devices(device_direction::render);
    }

    result<void> select_output_device(std::string device_id) {
        {
            std::scoped_lock lock(mutex_);
            config_.render.device = device_id.empty()
                ? device_selector::system_default(device_direction::render)
                : device_selector::explicit_id(device_direction::render, std::move(device_id));
            state_.preferred_output_device_id = config_.render.device.selection_mode ==
                    device_selector::mode::explicit_id
                ? config_.render.device.device_id
                : "";
            pending_seek_ms_ = state_.position_ms;
            decoder_ready_ = false;
            reached_end_of_stream_ = false;
            equalizer_state_.status = equalizer_status::loading;
            equalizer_state_.error_message.clear();
            if (!requested_source_uri_.empty()) {
                state_.status = playback_intent_playing_ ? playback_status::loading : playback_status::paused;
            }
        }

        auto reopen_result = reopen_render_stream();
        if (!reopen_result) {
            return reopen_result;
        }
        return result<void>::success();
    }

    playback_state state() const {
        std::scoped_lock lock(mutex_);
        return state_;
    }

    sonotide::equalizer_state equalizer_state() const {
        std::scoped_lock lock(mutex_);
        return equalizer_state_;
    }

    std::optional<sonotide::equalizer_frequency_range> equalizer_band_frequency_range(
        const std::size_t band_index) const {
        std::scoped_lock lock(mutex_);
        return sonotide::equalizer_band_editable_frequency_range(equalizer_state_.bands, band_index);
    }

    result<void> close() {
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return result<void>::success();
            }
            closed_ = true;
            shutting_down_ = true;
            recovery_requested_ = false;
            recovery_condition_.notify_all();
        }

        if (recovery_thread_.joinable()) {
            recovery_thread_.join();
        }

        auto close_result = render_stream_.close();
#if defined(_WIN32)
        decoder_.close();
#endif
        return close_result;
    }

    result<void> on_render(audio_buffer_view buffer, stream_timestamp) {
        std::string source_uri;
        std::uint64_t generation = 0;
        std::optional<std::int64_t> seek_ms;
        int volume_percent = 100;
        bool should_play = false;
        bool should_open_decoder = false;
        bool reached_end_of_stream = false;
        bool equalizer_enabled = false;
        float equalizer_output_gain_db = 0.0F;
        std::vector<equalizer_band> equalizer_bands;

        {
            std::scoped_lock lock(mutex_);
            state_.negotiated_format = buffer.format;
            state_.device_lost = false;
            source_uri = requested_source_uri_;
            generation = requested_source_generation_;
            volume_percent = state_.volume_percent;
            should_play = playback_intent_playing_;
            reached_end_of_stream = reached_end_of_stream_;
            equalizer_enabled = equalizer_state_.enabled;
            equalizer_output_gain_db = equalizer_state_.output_gain_db;
            equalizer_bands = equalizer_state_.bands;
            if (!source_uri.empty()) {
                should_open_decoder =
                    !decoder_ready_ ||
                    active_source_generation_ != generation ||
#if defined(_WIN32)
                    !formats_match(decoder_.output_format(), buffer.format);
#else
                    true;
#endif
            }

            if (pending_seek_ms_.has_value()) {
                seek_ms = pending_seek_ms_;
                pending_seek_ms_.reset();
            }
        }

        if (source_uri.empty()) {
            write_silence(buffer);
            std::scoped_lock lock(mutex_);
            state_.status = playback_status::idle;
            state_.error_message.clear();
            return result<void>::success();
        }

        if (!equalizer_configured_ || !formats_match(equalizer_format_, buffer.format)) {
            configure_equalizer_for_format(buffer.format);
        }

        if (should_open_decoder) {
            auto open_result = open_decoder(source_uri, buffer.format);
            if (!open_result) {
                handle_source_error(open_result.error());
                write_silence(buffer);
                return result<void>::success();
            }

            std::scoped_lock lock(mutex_);
            active_source_generation_ = generation;
            decoder_ready_ = true;
            state_.duration_ms = decoder_duration_ms();
            state_.status = should_play ? playback_status::loading : playback_status::paused;
        }

        if (seek_ms.has_value()) {
            auto seek_result = seek_decoder(*seek_ms);
            if (!seek_result) {
                handle_source_error(seek_result.error());
                write_silence(buffer);
                return result<void>::success();
            }

            equalizer_chain_.reset();
        }

        if (!should_play) {
            write_silence(buffer);
            std::scoped_lock lock(mutex_);
            state_.status = reached_end_of_stream ? playback_status::idle : playback_status::paused;
            state_.error_message.clear();
            return result<void>::success();
        }

        auto decoded_result = read_decoder_frames(buffer.frame_count);
        if (!decoded_result) {
            handle_source_error(decoded_result.error());
            write_silence(buffer);
            return result<void>::success();
        }

        apply_equalizer_runtime_targets(
            equalizer_enabled,
            equalizer_bands,
            equalizer_output_gain_db,
            volume_percent);
        equalizer_chain_.process(decoded_result.value().samples.data(), buffer.frame_count);

        convert_float_to_buffer(
            decoded_result.value().samples.data(),
            buffer.frame_count,
            buffer.format,
            buffer.bytes);

        {
            std::scoped_lock lock(mutex_);
            state_.position_ms = decoded_result.value().position_ms;
            state_.duration_ms = decoded_result.value().duration_ms;
            state_.status = playback_status::playing;
            state_.error_message.clear();
            if (decoded_result.value().end_of_stream) {
                playback_intent_playing_ = false;
                reached_end_of_stream_ = true;
                state_.status = playback_status::idle;
                state_.position_ms = state_.duration_ms;
                state_.completion_token += 1;
            }
        }

        return result<void>::success();
    }

    void on_stream_error(const error& stream_error) {
        std::scoped_lock lock(mutex_);
        state_.device_lost = true;
        state_.active_output_device_id.clear();
        state_.active_output_device_name.clear();
        state_.active_output_device_is_default = false;
        state_.error_message = stream_error.message;
        state_.status = playback_status::loading;
        decoder_ready_ = false;
        reached_end_of_stream_ = false;
        equalizer_state_.status = equalizer_status::audio_engine_unavailable;
        equalizer_state_.error_message = stream_error.message;
        if (config_.render.auto_recover_device_loss) {
            recovery_requested_ = true;
            recovery_condition_.notify_all();
        } else {
            playback_intent_playing_ = false;
            state_.status = playback_status::error;
            equalizer_state_.status = equalizer_status::error;
        }
    }

private:
    class render_callback_adapter final : public render_callback {
    public:
        explicit render_callback_adapter(implementation& owner)
            : owner_(owner) {}

        result<void> on_render(audio_buffer_view buffer, stream_timestamp timestamp) override {
            return owner_.on_render(buffer, timestamp);
        }

        void on_stream_error(const error& stream_error) override {
            owner_.on_stream_error(stream_error);
        }

    private:
        implementation& owner_;
    };

    result<void> open_render_stream() {
        auto handle_result = backend_->open_render_stream(config_.render, callback_);
        if (!handle_result) {
            return result<void>::failure(handle_result.error());
        }

        render_stream_ = detail::make_render_stream(std::move(handle_result.value()));
        auto start_result = render_stream_.start();
        if (!start_result) {
            return start_result;
        }
        refresh_active_output_device_state();
        return result<void>::success();
    }

    result<void> reopen_render_stream() {
        auto close_result = render_stream_.close();
        if (!close_result && close_result.error().code != error_code::invalid_state) {
            return close_result;
        }
        return open_render_stream();
    }

    void recovery_loop() {
        while (true) {
            {
                std::unique_lock lock(mutex_);
                recovery_condition_.wait(lock, [this]() {
                    return shutting_down_ || recovery_requested_;
                });
                if (shutting_down_) {
                    return;
                }
                recovery_requested_ = false;
            }

            auto reopen_result = reopen_render_stream();
            if (!reopen_result) {
                std::scoped_lock lock(mutex_);
                playback_intent_playing_ = false;
                state_.status = playback_status::error;
                state_.error_message = reopen_result.error().message;
                equalizer_state_.status = equalizer_status::error;
                equalizer_state_.error_message = reopen_result.error().message;
            }
        }
    }

    void refresh_active_output_device_state() {
        auto active_device_result = backend_->default_device(device_direction::render, config_.render.device.role);
        if (config_.render.device.selection_mode == device_selector::mode::explicit_id) {
            auto devices_result = backend_->enumerate_devices(device_direction::render);
            if (devices_result) {
                const auto device_iterator = std::find_if(
                    devices_result.value().begin(),
                    devices_result.value().end(),
                    [this](const device_info& device) {
                        return device.id == config_.render.device.device_id;
                    });
                if (device_iterator != devices_result.value().end()) {
                    std::scoped_lock lock(mutex_);
                    state_.active_output_device_id = device_iterator->id;
                    state_.active_output_device_name = device_iterator->friendly_name;
                    state_.active_output_device_is_default =
                        device_iterator->is_default_console ||
                        device_iterator->is_default_multimedia ||
                        device_iterator->is_default_communications;
                    state_.device_lost = false;
                    return;
                }
            }
        }

        if (!active_device_result) {
            return;
        }

        std::scoped_lock lock(mutex_);
        state_.active_output_device_id = active_device_result.value().id;
        state_.active_output_device_name = active_device_result.value().friendly_name;
        state_.active_output_device_is_default =
            active_device_result.value().is_default_console ||
            active_device_result.value().is_default_multimedia ||
            active_device_result.value().is_default_communications;
        state_.device_lost = false;
    }

    void populate_default_equalizer_state_locked() {
        equalizer_state_.available_presets = available_equalizer_presets_;
        equalizer_state_.status = equalizer_status::loading;
        equalizer_state_.bands = make_default_equalizer_bands(equalizer_max_band_count);
        equalizer_state_.last_nonflat_band_gains_db.assign(equalizer_state_.bands.size(), 0.0F);
        equalizer_state_.active_preset_id = equalizer_preset_id::flat;
        equalizer_state_.output_gain_db = 0.0F;
        equalizer_state_.headroom_compensation_db = 0.0F;
        equalizer_state_.error_message.clear();
    }

    void recalculate_active_preset_locked() {
        for (const equalizer_preset& preset : available_equalizer_presets_) {
            if (preset.id == equalizer_preset_id::custom) {
                continue;
            }

            const std::vector<float> projected_gains_db = project_preset_gains_to_bands(
                preset,
                equalizer_state_.bands);
            if (band_gains_match(equalizer_state_.bands, projected_gains_db)) {
                equalizer_state_.active_preset_id = preset.id;
                return;
            }
        }

        equalizer_state_.active_preset_id = equalizer_preset_id::custom;
    }

    void update_last_nonflat_state_locked() {
        if (equalizer_state_.last_nonflat_band_gains_db.size() != equalizer_state_.bands.size()) {
            equalizer_state_.last_nonflat_band_gains_db.assign(equalizer_state_.bands.size(), 0.0F);
        }

        bool is_flat = true;
        for (const equalizer_band& band : equalizer_state_.bands) {
            if (std::fabs(band.gain_db) > 0.01F) {
                is_flat = false;
                break;
            }
        }

        if (is_flat) {
            return;
        }

        for (std::size_t index = 0; index < equalizer_state_.bands.size(); ++index) {
            equalizer_state_.last_nonflat_band_gains_db[index] = equalizer_state_.bands[index].gain_db;
        }
    }

    void recompute_equalizer_metadata_locked(const float sample_rate) {
        equalizer_state_.headroom_compensation_db =
            headroom_controller_.compute_target_preamp_db(equalizer_state_.bands, sample_rate);
        if (equalizer_state_.status != equalizer_status::audio_engine_unavailable &&
            equalizer_state_.status != equalizer_status::unsupported_audio_path) {
            equalizer_state_.status = equalizer_status::ready;
            equalizer_state_.error_message.clear();
        }
    }

    [[nodiscard]] float current_sample_rate_or_default_locked() const {
        return last_known_sample_rate_ > 0.0F ? last_known_sample_rate_ : 48000.0F;
    }

    void configure_equalizer_for_format(const audio_format& format) {
        if (format.sample_rate == 0 || format.channel_count == 0) {
            std::scoped_lock lock(mutex_);
            equalizer_state_.status = equalizer_status::unsupported_audio_path;
            equalizer_state_.error_message =
                "Equalizer requires a negotiated render format with valid sample rate and channel count.";
            equalizer_configured_ = false;
            return;
        }

        equalizer_chain_.configure(
            static_cast<float>(format.sample_rate),
            static_cast<std::size_t>(format.channel_count));
        equalizer_chain_.reset();
        equalizer_format_ = format;
        equalizer_configured_ = true;

        std::scoped_lock lock(mutex_);
        last_known_sample_rate_ = static_cast<float>(format.sample_rate);
        recompute_equalizer_metadata_locked(last_known_sample_rate_);
    }

    void apply_equalizer_runtime_targets(
        const bool enabled,
        const std::span<const equalizer_band> bands,
        const float output_gain_db,
        const int volume_percent) {
        if (!equalizer_configured_) {
            return;
        }

        equalizer_chain_.set_bands(bands);
        equalizer_chain_.set_enabled(enabled);
        equalizer_chain_.set_output_gain_db(output_gain_db);
        equalizer_chain_.set_volume_linear(volume_percent_to_linear(volume_percent));

        std::scoped_lock lock(mutex_);
        last_known_sample_rate_ = equalizer_chain_.sample_rate();
        equalizer_state_.headroom_compensation_db = equalizer_chain_.headroom_compensation_db();
        if (equalizer_state_.status != equalizer_status::audio_engine_unavailable &&
            equalizer_state_.status != equalizer_status::unsupported_audio_path) {
            equalizer_state_.status = equalizer_status::ready;
            equalizer_state_.error_message.clear();
        }
    }

    void handle_source_error(const error& source_error) {
        std::scoped_lock lock(mutex_);
        playback_intent_playing_ = false;
        decoder_ready_ = false;
        reached_end_of_stream_ = false;
        state_.status = playback_status::error;
        state_.error_message = source_error.message;
    }

    std::int64_t decoder_duration_ms() const {
#if defined(_WIN32)
        return decoder_.duration_ms();
#else
        return 0;
#endif
    }

    result<void> open_decoder(const std::string& source_uri, const audio_format& output_format) {
#if defined(_WIN32)
        return decoder_.open(source_uri, output_format);
#else
        (void)source_uri;
        (void)output_format;
        return result<void>::failure(make_error(
            error_category::platform,
            error_code::unsupported_platform,
            "playback_session::open_decoder",
            "Playback session decode is available only on Windows."));
#endif
    }

    result<void> seek_decoder(const std::int64_t position_ms) {
#if defined(_WIN32)
        auto result_value = decoder_.seek_to(position_ms);
        if (result_value) {
            std::scoped_lock lock(mutex_);
            state_.position_ms = position_ms;
            state_.status = playback_intent_playing_ ? playback_status::loading : playback_status::paused;
            state_.error_message.clear();
        }
        return result_value;
#else
        (void)position_ms;
        return result<void>::failure(make_error(
            error_category::platform,
            error_code::unsupported_platform,
            "playback_session::seek_decoder",
            "Playback session decode is available only on Windows."));
#endif
    }

    decoded_audio_block_result read_decoder_frames(const std::uint32_t frame_count) {
#if defined(_WIN32)
        return decoder_.read_frames(frame_count);
#else
        (void)frame_count;
        return decoded_audio_block_result::failure(make_error(
            error_category::platform,
            error_code::unsupported_platform,
            "playback_session::read_decoder_frames",
            "Playback session decode is available only on Windows."));
#endif
    }

    std::shared_ptr<detail::runtime_backend> backend_;
    playback_session_config config_;
    render_callback_adapter callback_;
    render_stream render_stream_;
    mutable std::mutex mutex_;
    std::condition_variable recovery_condition_;
    std::thread recovery_thread_;
    playback_state state_{};
    sonotide::equalizer_state equalizer_state_{};
    detail::dsp::equalizer_chain equalizer_chain_;
    detail::dsp::output_headroom_controller headroom_controller_;
    std::vector<equalizer_preset> available_equalizer_presets_ = detail::dsp::builtin_equalizer_presets();
    audio_format equalizer_format_{};
    float last_known_sample_rate_ = 0.0F;
    bool playback_intent_playing_ = false;
    bool decoder_ready_ = false;
    bool equalizer_configured_ = false;
    bool recovery_requested_ = false;
    bool shutting_down_ = false;
    bool closed_ = false;
    bool reached_end_of_stream_ = false;
    std::uint64_t requested_source_generation_ = 0;
    std::uint64_t active_source_generation_ = 0;
    std::optional<std::int64_t> pending_seek_ms_;
    std::string requested_source_uri_;
#if defined(_WIN32)
    detail::win::media_foundation_decoder decoder_;
#endif
};

result<playback_session> playback_session::create(
    std::shared_ptr<detail::runtime_backend> backend,
    const playback_session_config& config) {
    return implementation::create(std::move(backend), config);
}

playback_session::playback_session(std::unique_ptr<implementation> implementation) noexcept
    : implementation_(std::move(implementation)) {}

playback_session::~playback_session() = default;

playback_session::playback_session(playback_session&&) noexcept = default;

playback_session& playback_session::operator=(playback_session&&) noexcept = default;

bool playback_session::is_open() const noexcept {
    return implementation_ != nullptr && implementation_->is_open();
}

result<void> playback_session::load(std::string source_uri) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::load",
            "Playback session is not open."));
    }
    return implementation_->load(std::move(source_uri));
}

result<void> playback_session::play() {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::play",
            "Playback session is not open."));
    }
    return implementation_->play();
}

result<void> playback_session::pause() {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::pause",
            "Playback session is not open."));
    }
    return implementation_->pause();
}

result<void> playback_session::seek_to(const std::int64_t position_ms) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::seek_to",
            "Playback session is not open."));
    }
    return implementation_->seek_to(position_ms);
}

result<void> playback_session::set_volume_percent(const int volume_percent) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::set_volume_percent",
            "Playback session is not open."));
    }
    return implementation_->set_volume_percent(volume_percent);
}

result<void> playback_session::set_equalizer_enabled(const bool enabled) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::set_equalizer_enabled",
            "Playback session is not open."));
    }
    return implementation_->set_equalizer_enabled(enabled);
}

result<void> playback_session::select_equalizer_preset(const equalizer_preset_id preset_id) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::select_equalizer_preset",
            "Playback session is not open."));
    }
    return implementation_->select_equalizer_preset(preset_id);
}

result<void> playback_session::set_equalizer_band_gain(
    const std::size_t band_index,
    const float gain_db) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::set_equalizer_band_gain",
            "Playback session is not open."));
    }
    return implementation_->set_equalizer_band_gain(band_index, gain_db);
}

result<void> playback_session::add_equalizer_band(
    const float center_frequency_hz,
    const float gain_db) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::add_equalizer_band",
            "Playback session is not open."));
    }
    return implementation_->add_equalizer_band(center_frequency_hz, gain_db);
}

result<void> playback_session::remove_equalizer_band(const std::size_t band_index) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::remove_equalizer_band",
            "Playback session is not open."));
    }
    return implementation_->remove_equalizer_band(band_index);
}

result<void> playback_session::set_equalizer_band_frequency(
    const std::size_t band_index,
    const float center_frequency_hz) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::set_equalizer_band_frequency",
            "Playback session is not open."));
    }
    return implementation_->set_equalizer_band_frequency(band_index, center_frequency_hz);
}

result<void> playback_session::reset_equalizer() {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::reset_equalizer",
            "Playback session is not open."));
    }
    return implementation_->reset_equalizer();
}

result<void> playback_session::set_equalizer_output_gain(const float output_gain_db) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::set_equalizer_output_gain",
            "Playback session is not open."));
    }
    return implementation_->set_equalizer_output_gain(output_gain_db);
}

result<void> playback_session::apply_equalizer_state(const sonotide::equalizer_state& state) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::apply_equalizer_state",
            "Playback session is not open."));
    }
    return implementation_->apply_equalizer_state(state);
}

result<std::vector<device_info>> playback_session::list_output_devices() const {
    if (!implementation_) {
        return result<std::vector<device_info>>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::list_output_devices",
            "Playback session is not open."));
    }
    return implementation_->list_output_devices();
}

result<void> playback_session::select_output_device(std::string device_id) {
    if (!implementation_) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::select_output_device",
            "Playback session is not open."));
    }
    return implementation_->select_output_device(std::move(device_id));
}

playback_state playback_session::state() const {
    if (!implementation_) {
        return {};
    }
    return implementation_->state();
}

sonotide::equalizer_state playback_session::equalizer_state() const {
    if (!implementation_) {
        return {};
    }
    return implementation_->equalizer_state();
}

std::optional<sonotide::equalizer_frequency_range> playback_session::equalizer_band_frequency_range(
    const std::size_t band_index) const {
    if (!implementation_) {
        return std::nullopt;
    }
    return implementation_->equalizer_band_frequency_range(band_index);
}

result<void> playback_session::close() {
    if (!implementation_) {
        return result<void>::success();
    }
    auto close_result = implementation_->close();
    implementation_.reset();
    return close_result;
}

}  // namespace sonotide
