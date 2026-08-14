#include "sonotide/playback_session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "internal/dsp/equalizer_chain.h"
#include "internal/equalizer_layout_utils.h"
#include "internal/playback/decoded_audio_queue.h"
#include "internal/playback/playback_decoder.h"
#include "internal/runtime_backend.h"

#if defined(_WIN32)
#include "internal/win/com_scope.h"
#endif

namespace sonotide {
namespace {

// Преобразует громкость транспорта из пользовательского диапазона 0..100 в линейный коэффициент.
float volume_percent_to_linear(const int volume_percent) {
    return static_cast<float>((std::clamp)(volume_percent, 0, 100)) / 100.0F;
}

constexpr float kMinEqualizerGainDb = -12.0F;
constexpr float kMaxEqualizerGainDb = 12.0F;
constexpr std::uint32_t kMaximumDecoderShutdownTimeoutMs = 5000U;

float clamp_equalizer_gain_db(const float gain_db) {
    return (std::clamp)(gain_db, kMinEqualizerGainDb, kMaxEqualizerGainDb);
}

float clamp_equalizer_q_value(const float q_value) {
    const equalizer_q_limits q_limits = supported_equalizer_q_limits();
    return (std::clamp)(q_value, q_limits.min_q_value, q_limits.max_q_value);
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
        auto* samples = reinterpret_cast<float*>(destination.data());
        for (std::size_t index = 0; index < sample_count; ++index) {
            samples[index] = std::isfinite(source_samples[index]) ? source_samples[index] : 0.0F;
        }
        return;
    }

    if (format.sample == sample_type::pcm_i16) {
        auto* samples = reinterpret_cast<std::int16_t*>(destination.data());
        for (std::size_t index = 0; index < sample_count; ++index) {
            const float finite_value = std::isfinite(source_samples[index]) ? source_samples[index] : 0.0F;
            const float value = (std::clamp)(finite_value, -1.0F, 1.0F);
            samples[index] = static_cast<std::int16_t>(value * 32767.0F);
        }
        return;
    }

    if (format.sample == sample_type::pcm_i24_in_32 || format.sample == sample_type::pcm_i32) {
        auto* samples = reinterpret_cast<std::int32_t*>(destination.data());
        const float scale =
            format.sample == sample_type::pcm_i24_in_32 ? 8388607.0F : 2147483647.0F;
        for (std::size_t index = 0; index < sample_count; ++index) {
            const float finite_value = std::isfinite(source_samples[index]) ? source_samples[index] : 0.0F;
            const float value = (std::clamp)(finite_value, -1.0F, 1.0F);
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

}  // namespace

namespace {
struct decoded_pipeline_payload {
    decoded_pipeline_payload(
        const std::size_t queue_slots,
        const std::size_t samples_per_slot,
        audio_format stream_format,
        const std::uint32_t stream_frame_count)
        : queue(queue_slots, samples_per_slot),
          render_scratch(samples_per_slot),
          format(stream_format),
          frame_count(stream_frame_count) {
        equalizer.configure(
            static_cast<float>(format.sample_rate),
            static_cast<std::size_t>(format.channel_count));
        equalizer.prepare(frame_count);
        equalizer.reset();
    }

    detail::playback::decoded_audio_queue queue;
    std::vector<float> render_scratch;
    detail::dsp::equalizer_chain equalizer;
    std::uint64_t applied_equalizer_control_version = (std::numeric_limits<std::uint64_t>::max)();
    audio_format format{};
    std::uint32_t frame_count = 0;
};

struct decoded_pipeline {
    void rebuild(
        const std::size_t samples_per_slot,
        const audio_format& format,
        const std::uint32_t frame_count) {
        payload = std::make_unique<decoded_pipeline_payload>(
            4U, samples_per_slot, format, frame_count);
    }

    std::unique_ptr<decoded_pipeline_payload> payload;
};
static_assert(std::atomic<decoded_pipeline*>::is_always_lock_free);

class render_hazard_guard {
public:
    explicit render_hazard_guard(std::atomic<decoded_pipeline*>& hazard) noexcept
        : hazard_(hazard) {}
    ~render_hazard_guard() { hazard_.store(nullptr, std::memory_order_seq_cst); }
    render_hazard_guard(const render_hazard_guard&) = delete;
    render_hazard_guard& operator=(const render_hazard_guard&) = delete;
private:
    std::atomic<decoded_pipeline*>& hazard_;
};
}  // namespace

class playback_session::implementation {
public:
    // Создаёт полностью инициализированную сессию и сразу запускает поток рендеринга.
    static result<playback_session> create(
        std::shared_ptr<detail::runtime_backend> backend,
        const playback_session_config& config) {
        if (config.decoder_shutdown_timeout_ms == 0U ||
            config.decoder_shutdown_timeout_ms > kMaximumDecoderShutdownTimeoutMs) {
            return result<playback_session>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::create",
                "Decoder shutdown timeout must be between 1 and 5000 milliseconds."));
        }
        if (config.initial_equalizer_state.has_value()) {
            const auto& initial = *config.initial_equalizer_state;
            const bool invalid = initial.bands.size() > supported_equalizer_band_count_limits().max_band_count ||
                !std::isfinite(initial.output_gain_db) ||
                std::any_of(initial.bands.begin(), initial.bands.end(), [](const equalizer_band& band) {
                    return !std::isfinite(band.center_frequency_hz) ||
                           !std::isfinite(band.gain_db) || !std::isfinite(band.q_value);
                }) || std::any_of(
                    initial.last_nonflat_band_gains_db.begin(),
                    initial.last_nonflat_band_gains_db.end(),
                    [](const float value) { return !std::isfinite(value); });
            if (invalid) {
                return result<playback_session>::failure(make_error(
                    error_category::configuration,
                    error_code::invalid_argument,
                    "playback_session::create",
                    "Initial equalizer state must contain only finite values and supported band count."));
            }
        }
        // Сначала создаём объект, чтобы при ошибке открытия можно было безопасно вернуть структурированную ошибку.
        auto instance = std::shared_ptr<implementation>(
            new implementation(std::move(backend), config));
        // Поток рендеринга должен быть открыт до того, как публичный объект будет возвращён.
        auto open_result = instance->open_render_stream();
        if (!open_result) {
            return result<playback_session>::failure(open_result.error());
        }

        // Восстановление и decode живут вне audio callback. Ошибка создания
        // системного потока остаётся в result-контракте публичной фабрики.
        try {
            instance->recovery_thread_ = std::thread([owner = instance.get()]() {
                owner->recovery_loop();
            });
            // The decode worker may outlive logical close() when an external
            // synchronous Media Foundation handler does not return. Shared
            // ownership keeps every object it can touch alive in that case.
            instance->decode_thread_ = std::thread([owner = instance]() {
                owner->decode_worker_entry();
            });
        } catch (const std::exception& exception) {
            try {
                (void)instance->close();
            } catch (...) {
                // Preserve the original startup diagnostic.
            }
            return result<playback_session>::failure(make_error(
                error_category::initialization,
                error_code::initialization_failed,
                "playback_session::create",
                std::string("Failed to start playback worker threads: ") + exception.what()));
        } catch (...) {
            try {
                (void)instance->close();
            } catch (...) {
                // Preserve the original startup diagnostic.
            }
            return result<playback_session>::failure(make_error(
                error_category::initialization,
                error_code::initialization_failed,
                "playback_session::create",
                "Failed to start playback worker threads."));
        }

        return result<playback_session>::success(
            playback_session(std::move(instance)));
    }

    // Сохраняет внутреннюю реализацию и конфигурацию сессии, затем инициализирует снимок воспроизведения.
    implementation(
        std::shared_ptr<detail::runtime_backend> backend,
        playback_session_config config)
        : backend_(std::move(backend)),
          config_(std::move(config)),
          callback_(*this),
          decoder_(detail::playback::make_decoder()) {
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
    ~implementation() noexcept {
        try {
            (void)close();
        } catch (...) {
            // Explicit close() is the diagnostic path. Destruction must not
            // terminate the process if a system synchronization primitive fails.
        }
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

        std::scoped_lock command_lock(decoder_command_mutex_);
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return closed_session_result("playback_session::load");
            }
            const std::uint64_t command_epoch = decoder_->request_cancel();
            requested_source_uri_ = std::move(source_uri);
            ++requested_source_generation_;
            requested_decoder_epoch_ = command_epoch;
            requested_generation_atomic_.store(requested_source_generation_, std::memory_order_release);
            pending_seek_ms_.reset();
            requested_seek_sequence_atomic_.store(requested_seek_sequence_, std::memory_order_release);
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
            decode_condition_.notify_all();
        }
        return result<void>::success();
    }

    // Запрашивает запуск транспорта; реальная работа с декодером остаётся за рабочим потоком.
    result<void> play() {
        std::scoped_lock lock(mutex_);
        if (closed_) {
            return closed_session_result("playback_session::play");
        }
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
            ++requested_seek_sequence_;
            requested_seek_sequence_atomic_.store(requested_seek_sequence_, std::memory_order_release);
            reached_end_of_stream_ = false;
        }

        playback_intent_playing_ = true;
        state_.status = decoder_ready_ ? playback_status::playing : playback_status::loading;
        state_.error_message.clear();
        decode_condition_.notify_all();
        return result<void>::success();
    }

    // Запрашивает паузу транспорта, сохраняя загруженный источник и снимок таймлайна.
    result<void> pause() {
        std::scoped_lock lock(mutex_);
        if (closed_) {
            return closed_session_result("playback_session::pause");
        }
        playback_intent_playing_ = false;
        state_.status = requested_source_uri_.empty() ? playback_status::idle : playback_status::paused;
        state_.error_message.clear();
        decode_condition_.notify_all();
        return result<void>::success();
    }

    // Планирует seek декодера на следующем тике обратного вызова рендеринга.
    result<void> seek_to(std::int64_t position_ms) {
        std::scoped_lock command_lock(decoder_command_mutex_);
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return closed_session_result("playback_session::seek_to");
            }
            if (requested_source_uri_.empty()) {
                return result<void>::failure(make_error(
                    error_category::stream,
                    error_code::invalid_state,
                    "playback_session::seek_to",
                    "Cannot seek before a source has been loaded."));
            }
            const std::uint64_t command_epoch = decoder_->request_cancel();
            pending_seek_ms_ = (std::max)(position_ms, static_cast<std::int64_t>(0));
            ++requested_seek_sequence_;
            requested_decoder_epoch_ = command_epoch;
            requested_seek_sequence_atomic_.store(requested_seek_sequence_, std::memory_order_release);
            reached_end_of_stream_ = false;
            state_.position_ms = *pending_seek_ms_;
            state_.status = playback_intent_playing_ ? playback_status::loading : playback_status::paused;
            state_.error_message.clear();
            decode_condition_.notify_all();
        }
        return result<void>::success();
    }

    // Сохраняет пользовательскую громкость в процентах, а слой DSP преобразует её позже.
    result<void> set_volume_percent(int volume_percent) {
        std::scoped_lock lock(mutex_);
        if (closed_) return closed_session_result("playback_session::set_volume_percent");
        state_.volume_percent = (std::clamp)(volume_percent, 0, 100);
        ++equalizer_control_version_;
        return result<void>::success();
    }

    result<void> set_equalizer_enabled(const bool enabled) {
        std::scoped_lock lock(mutex_);
        if (closed_) return closed_session_result("playback_session::set_equalizer_enabled");
        equalizer_state_.enabled = enabled;
        recompute_equalizer_metadata_locked(current_sample_rate_or_default_locked());
        return result<void>::success();
    }

    result<void> select_equalizer_preset(const equalizer_preset_id preset_id) {
        std::scoped_lock lock(mutex_);
        if (closed_) return closed_session_result("playback_session::select_equalizer_preset");
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
        if (!std::isfinite(gain_db)) {
            return invalid_finite_value_result("playback_session::set_equalizer_band_gain");
        }
        std::scoped_lock lock(mutex_);
        if (closed_) return closed_session_result("playback_session::set_equalizer_band_gain");
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

    result<void> set_equalizer_band_q(const std::size_t band_index, const float q_value) {
        if (!std::isfinite(q_value)) {
            return invalid_finite_value_result("playback_session::set_equalizer_band_q");
        }
        std::scoped_lock lock(mutex_);
        if (closed_) return closed_session_result("playback_session::set_equalizer_band_q");
        if (band_index >= equalizer_state_.bands.size()) {
            return result<void>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::set_equalizer_band_q",
                "Equalizer band index is outside of the active band range."));
        }

        equalizer_state_.bands[band_index].q_value = clamp_equalizer_q_value(q_value);
        recompute_equalizer_metadata_locked(current_sample_rate_or_default_locked());
        return result<void>::success();
    }

    result<void> add_equalizer_band(const float center_frequency_hz, const float gain_db) {
        if (!std::isfinite(center_frequency_hz) || !std::isfinite(gain_db)) {
            return invalid_finite_value_result("playback_session::add_equalizer_band");
        }
        std::scoped_lock lock(mutex_);
        if (closed_) return closed_session_result("playback_session::add_equalizer_band");
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
                .q_value = default_equalizer_q_value,
            });
        update_last_nonflat_state_locked();
        recalculate_active_preset_locked();
        recompute_equalizer_metadata_locked(current_sample_rate_or_default_locked());
        return result<void>::success();
    }

    result<void> remove_equalizer_band(const std::size_t band_index) {
        std::scoped_lock lock(mutex_);
        if (closed_) return closed_session_result("playback_session::remove_equalizer_band");
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
        if (!std::isfinite(center_frequency_hz)) {
            return invalid_finite_value_result("playback_session::set_equalizer_band_frequency");
        }
        std::scoped_lock lock(mutex_);
        if (closed_) return closed_session_result("playback_session::set_equalizer_band_frequency");
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
        if (!std::isfinite(output_gain_db)) {
            return invalid_finite_value_result("playback_session::set_equalizer_output_gain");
        }
        std::scoped_lock lock(mutex_);
        if (closed_) return closed_session_result("playback_session::set_equalizer_output_gain");
        equalizer_state_.output_gain_db = clamp_equalizer_gain_db(output_gain_db);
        ++equalizer_control_version_;
        return result<void>::success();
    }

    result<void> apply_equalizer_state(const sonotide::equalizer_state& state) {
        if (!std::isfinite(state.output_gain_db) || std::any_of(
                state.bands.begin(), state.bands.end(), [](const equalizer_band& band) {
                    return !std::isfinite(band.center_frequency_hz) ||
                           !std::isfinite(band.gain_db) || !std::isfinite(band.q_value);
                }) || std::any_of(
                    state.last_nonflat_band_gains_db.begin(),
                    state.last_nonflat_band_gains_db.end(),
                    [](const float value) { return !std::isfinite(value); })) {
            return invalid_finite_value_result("playback_session::apply_equalizer_state");
        }
        std::scoped_lock lock(mutex_);
        if (closed_) return closed_session_result("playback_session::apply_equalizer_state");
        if (state.bands.size() > supported_equalizer_band_count_limits().max_band_count) {
            return result<void>::failure(make_error(
                error_category::configuration,
                error_code::invalid_argument,
                "playback_session::apply_equalizer_state",
                "Equalizer state exceeds the maximum supported number of bands."));
        }

        equalizer_state_.enabled = state.enabled;
        equalizer_state_.bands = detail::normalize_equalizer_bands(state.bands);
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
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return closed_session_result<std::vector<device_info>>(
                    "playback_session::list_output_devices");
            }
        }
        auto devices_result = backend_->enumerate_devices(device_direction::render);
        std::scoped_lock lock(mutex_);
        if (closed_) {
            return closed_session_result<std::vector<device_info>>(
                "playback_session::list_output_devices");
        }
        return devices_result;
    }

    result<void> select_output_device(std::string device_id) {
        std::scoped_lock command_lock(decoder_command_mutex_);
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return closed_session_result("playback_session::select_output_device");
            }
            const std::uint64_t command_epoch = decoder_->request_cancel();
            config_.render.device = device_id.empty()
                ? device_selector::system_default(device_direction::render)
                : device_selector::explicit_id(device_direction::render, std::move(device_id));
            state_.preferred_output_device_id = config_.render.device.selection_mode ==
                    device_selector::mode::explicit_id
                ? config_.render.device.device_id
                : "";
            pending_seek_ms_ = state_.position_ms;
            ++requested_seek_sequence_;
            requested_decoder_epoch_ = command_epoch;
            requested_seek_sequence_atomic_.store(requested_seek_sequence_, std::memory_order_release);
            decoder_ready_ = false;
            reached_end_of_stream_ = false;
            equalizer_state_.status = equalizer_status::loading;
            equalizer_state_.error_message.clear();
            if (!requested_source_uri_.empty()) {
                state_.status = playback_intent_playing_ ? playback_status::loading : playback_status::paused;
            }
            decode_condition_.notify_all();
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

    result<sonotide::equalizer_response_curve> preview_equalizer_response(
        const sonotide::equalizer_preview_state& preview_state,
        const std::span<const float> frequencies_hz) const {
        if (!std::isfinite(preview_state.output_gain_db) ||
            std::any_of(preview_state.bands.begin(), preview_state.bands.end(), [](const equalizer_band& band) {
                return !std::isfinite(band.center_frequency_hz) ||
                       !std::isfinite(band.gain_db) || !std::isfinite(band.q_value);
            }) || std::any_of(frequencies_hz.begin(), frequencies_hz.end(), [](const float value) {
                return !std::isfinite(value);
            })) {
            return closed_session_result<sonotide::equalizer_response_curve>(
                "playback_session::preview_equalizer_response",
                error_category::configuration,
                error_code::invalid_argument,
                "Equalizer values and sampled frequencies must be finite.");
        }
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return closed_session_result<sonotide::equalizer_response_curve>(
                    "playback_session::preview_equalizer_response");
            }
        }
        const std::optional<float> sample_rate_hz = resolve_equalizer_sample_rate_locked();
        if (!sample_rate_hz.has_value()) {
            return result<sonotide::equalizer_response_curve>::failure(make_error(
                error_category::stream,
                error_code::invalid_state,
                "playback_session::preview_equalizer_response",
                "Equalizer preview requires a known render sample rate. Start playback or wait for a negotiated format before requesting a session-level preview."));
        }

        sonotide::equalizer_state preview_state_snapshot;
        preview_state_snapshot.enabled = preview_state.enabled;
        preview_state_snapshot.bands = detail::normalize_equalizer_bands(preview_state.bands);
        preview_state_snapshot.output_gain_db = clamp_equalizer_gain_db(preview_state.output_gain_db);
        return sonotide::sample_equalizer_response(
            preview_state_snapshot,
            *sample_rate_hz,
            frequencies_hz);
    }

    result<sonotide::equalizer_response_curve> sample_equalizer_response(
        const std::span<const float> frequencies_hz) const {
        if (std::any_of(frequencies_hz.begin(), frequencies_hz.end(), [](const float value) {
                return !std::isfinite(value);
            })) {
            return closed_session_result<sonotide::equalizer_response_curve>(
                "playback_session::sample_equalizer_response",
                error_category::configuration,
                error_code::invalid_argument,
                "Sampled frequencies must be finite.");
        }
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return closed_session_result<sonotide::equalizer_response_curve>(
                    "playback_session::sample_equalizer_response");
            }
        }
        const std::optional<float> sample_rate_hz = resolve_equalizer_sample_rate_locked();
        if (!sample_rate_hz.has_value()) {
            return result<sonotide::equalizer_response_curve>::failure(make_error(
                error_category::stream,
                error_code::invalid_state,
                "playback_session::sample_equalizer_response",
                "Equalizer response sampling requires a known render sample rate. Use the helper that accepts an explicit sample rate before playback is configured."));
        }

        sonotide::equalizer_state equalizer_state_snapshot;
        {
            std::scoped_lock lock(mutex_);
            equalizer_state_snapshot = equalizer_state_;
        }
        return sonotide::sample_equalizer_response(equalizer_state_snapshot, *sample_rate_hz, frequencies_hz);
    }

    std::optional<sonotide::equalizer_frequency_range> equalizer_band_frequency_range(
        const std::size_t band_index) const {
        std::scoped_lock lock(mutex_);
        return sonotide::equalizer_band_editable_frequency_range(equalizer_state_.bands, band_index);
    }

    result<void> close() {
        std::scoped_lock command_lock(decoder_command_mutex_);
        std::uint32_t decoder_shutdown_timeout_ms = 0;
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return result<void>::success();
            }
            closed_ = true;
            shutting_down_ = true;
            recovery_requested_ = false;
            decoder_shutdown_timeout_ms = config_.decoder_shutdown_timeout_ms;
            if (decoder_) {
                (void)decoder_->request_cancel();
            }
            recovery_condition_.notify_all();
            decode_condition_.notify_all();
        }

        bool decode_shutdown_timed_out = false;
        if (decode_thread_.joinable()) {
            bool decode_worker_finished = false;
            {
                std::unique_lock lock(mutex_);
                decode_worker_finished = decode_worker_finished_condition_.wait_for(
                    lock,
                    std::chrono::milliseconds(decoder_shutdown_timeout_ms),
                    [this]() { return decode_worker_finished_; });
            }
            if (decode_worker_finished) {
                decode_thread_.join();
            } else {
                decode_shutdown_timed_out = true;
                // std::thread has no timed join. The worker owns this
                // implementation and releases its decoder on its own COM
                // apartment if the external synchronous operation returns.
                decode_thread_.detach();
            }
        }

        if (recovery_thread_.joinable()) {
            recovery_thread_.join();
        }

        std::scoped_lock stream_lock(stream_operation_mutex_);
        decoded_pipeline_.store(nullptr, std::memory_order_seq_cst);
        auto close_result = render_stream_.close();
        while (render_hazard_.load(std::memory_order_seq_cst) != nullptr) {
            std::this_thread::yield();
        }
        if (decode_shutdown_timed_out && close_result) {
            error failure;
            failure.category = error_category::stream;
            failure.code = error_code::operation_timed_out;
            failure.operation = "playback_session::close";
            failure.message =
                "The playback session was closed, but decoder cleanup was deferred because "
                "a synchronous Media Foundation operation did not finish before the configured timeout.";
            failure.recoverable = true;
            return result<void>::failure(std::move(failure));
        }
        return close_result;
    }

    result<void> on_render(audio_buffer_view buffer, stream_timestamp) {
        write_silence(buffer);
        negotiated_sample_rate_atomic_.store(
            static_cast<float>(buffer.format.sample_rate), std::memory_order_release);

        std::array<equalizer_band, equalizer_max_band_count> equalizer_bands{};
        std::size_t active_equalizer_band_count = 0;
        int volume_percent = 100;
        bool should_play = false;
        bool equalizer_enabled = false;
        float equalizer_output_gain_db = 0.0F;
        float equalizer_headroom_db = 0.0F;
        std::uint64_t equalizer_control_version = 0;
        decoded_pipeline* pipeline = nullptr;

        std::unique_lock state_lock(mutex_, std::try_to_lock);
        if (!state_lock.owns_lock() || shutting_down_) {
            return result<void>::success();
        }
        const bool render_changed = !requested_render_format_.has_value() ||
            !formats_match(*requested_render_format_, buffer.format) ||
            requested_frame_count_ != buffer.frame_count;
        state_.negotiated_format = buffer.format;
        last_known_sample_rate_ = static_cast<float>(buffer.format.sample_rate);
        state_.device_lost = false;
        requested_render_format_ = buffer.format;
        requested_frame_count_ = buffer.frame_count;
        volume_percent = state_.volume_percent;
        should_play = playback_intent_playing_ && !requested_source_uri_.empty();
        equalizer_enabled = equalizer_state_.enabled;
        equalizer_output_gain_db = equalizer_state_.output_gain_db;
        equalizer_headroom_db = equalizer_state_.headroom_compensation_db;
        equalizer_control_version = equalizer_control_version_;
        active_equalizer_band_count = (std::min)(equalizer_state_.bands.size(), equalizer_bands.size());
        std::copy_n(equalizer_state_.bands.begin(), active_equalizer_band_count, equalizer_bands.begin());
        render_hazard_guard hazard_guard(render_hazard_);
        do {
            pipeline = decoded_pipeline_.load(std::memory_order_seq_cst);
            render_hazard_.store(pipeline, std::memory_order_seq_cst);
            if (decoded_pipeline_.load(std::memory_order_seq_cst) == pipeline) {
                break;
            }
            render_hazard_.store(nullptr, std::memory_order_seq_cst);
        } while (true);
        state_lock.unlock();
        if (render_changed) {
            decode_condition_.notify_all();
        }

        if (!should_play || !pipeline ||
            pipeline->payload->frame_count != buffer.frame_count ||
            !formats_match(pipeline->payload->format, buffer.format)) {
            return result<void>::success();
        }

        const std::size_t sample_count = static_cast<std::size_t>(buffer.frame_count) *
            static_cast<std::size_t>(buffer.format.channel_count);

        detail::playback::decoded_metadata metadata;
        if (!pipeline->payload->queue.try_pop(
                std::span<float>(pipeline->payload->render_scratch.data(), sample_count), metadata)) {
            return result<void>::success();
        }
        decode_condition_.notify_one();

        if (metadata.generation != requested_generation_atomic_.load(std::memory_order_acquire) ||
            metadata.seek_sequence != requested_seek_sequence_atomic_.load(std::memory_order_acquire)) {
            return result<void>::success();
        }

        if (pipeline->payload->applied_equalizer_control_version != equalizer_control_version) {
            pipeline->payload->equalizer.set_bands_precomputed(
                std::span<const equalizer_band>(equalizer_bands.data(), active_equalizer_band_count),
                equalizer_headroom_db);
            pipeline->payload->equalizer.set_enabled(equalizer_enabled);
            pipeline->payload->equalizer.set_output_gain_db(equalizer_output_gain_db);
            pipeline->payload->equalizer.set_volume_linear(volume_percent_to_linear(volume_percent));
            pipeline->payload->applied_equalizer_control_version = equalizer_control_version;
        }
        pipeline->payload->equalizer.process(pipeline->payload->render_scratch.data(), buffer.frame_count);

        convert_float_to_buffer(
            pipeline->payload->render_scratch.data(),
            buffer.frame_count,
            buffer.format,
            buffer.bytes);

        std::unique_lock completion_lock(mutex_, std::try_to_lock);
        if (completion_lock.owns_lock() && !shutting_down_ &&
            metadata.generation == requested_source_generation_ &&
            metadata.seek_sequence == requested_seek_sequence_) {
            state_.position_ms = metadata.position_ms;
            state_.duration_ms = metadata.duration_ms;
            last_known_sample_rate_ = pipeline->payload->equalizer.sample_rate();
            equalizer_state_.headroom_compensation_db =
                pipeline->payload->equalizer.headroom_compensation_db();
            state_.status = playback_status::playing;
            state_.error_message.clear();
            if (metadata.end_of_stream) {
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
        if (shutting_down_) {
            return;
        }
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
    static result<void> invalid_finite_value_result(std::string operation) {
        return result<void>::failure(make_error(
            error_category::configuration,
            error_code::invalid_argument,
            std::move(operation),
            "Equalizer values must be finite."));
    }

    template <typename T = void>
    static result<T> closed_session_result(
        std::string operation,
        const error_category category = error_category::stream,
        const error_code code = error_code::invalid_state,
        std::string message = "Playback session is closed.") {
        return result<T>::failure(make_error(
            category,
            code,
            std::move(operation),
            std::move(message)));
    }

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
        render_stream_config render_config;
        {
            std::scoped_lock lock(mutex_);
            if (shutting_down_) {
                return result<void>::failure(make_error(
                    error_category::stream,
                    error_code::invalid_state,
                    "playback_session::open_render_stream",
                    "Playback session is closing."));
            }
            render_config = config_.render;
        }

        auto handle_result = backend_->open_render_stream(render_config, callback_);
        if (!handle_result) {
            return result<void>::failure(handle_result.error());
        }

        render_stream_ = detail::make_render_stream(std::move(handle_result.value()));
        auto start_result = render_stream_.start();
        if (!start_result) {
            return start_result;
        }
        refresh_active_output_device_state(render_config.device);
        return result<void>::success();
    }

    result<void> reopen_render_stream() {
        std::scoped_lock stream_lock(stream_operation_mutex_);
        {
            std::scoped_lock lock(mutex_);
            if (shutting_down_) {
                return result<void>::failure(make_error(
                    error_category::stream,
                    error_code::invalid_state,
                    "playback_session::reopen_render_stream",
                    "Playback session is closing."));
            }
        }
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
                if (shutting_down_) {
                    return;
                }
                playback_intent_playing_ = false;
                state_.status = playback_status::error;
                state_.error_message = reopen_result.error().message;
                equalizer_state_.status = equalizer_status::error;
                equalizer_state_.error_message = reopen_result.error().message;
            }
        }
    }

    void decode_loop() {
        std::uint64_t active_generation = 0;
        std::uint64_t active_seek_sequence = 0;
        audio_format active_format{};
        std::uint32_t active_frame_count = 0;
        bool end_of_stream = false;
        bool request_failed = false;
        std::uint64_t failed_generation = 0;
        std::uint64_t failed_seek_sequence = 0;
        audio_format failed_format{};
        std::uint32_t failed_frame_count = 0;

        while (true) {
            std::string source_uri;
            audio_format format{};
            std::uint32_t frame_count = 0;
            std::uint64_t generation = 0;
            std::uint64_t seek_sequence = 0;
            std::uint64_t decoder_epoch = 0;
            std::optional<std::int64_t> seek_position;
            decoded_pipeline* pipeline = nullptr;

            {
                std::unique_lock lock(mutex_);
                decode_condition_.wait(lock, [&]() {
                    const bool configured = !requested_source_uri_.empty() &&
                        requested_render_format_.has_value() && requested_frame_count_ > 0;
                    const auto* current_pipeline = decoded_pipeline_.load(std::memory_order_seq_cst);
                    const bool failed_request_changed = !request_failed ||
                        requested_source_generation_ != failed_generation ||
                        requested_seek_sequence_ != failed_seek_sequence ||
                        !formats_match(*requested_render_format_, failed_format) ||
                        requested_frame_count_ != failed_frame_count;
                    return shutting_down_ || (configured && failed_request_changed && (
                        requested_source_generation_ != active_generation ||
                        requested_seek_sequence_ != active_seek_sequence ||
                        !formats_match(*requested_render_format_, active_format) ||
                        requested_frame_count_ != active_frame_count ||
                        (!end_of_stream && current_pipeline && !current_pipeline->payload->queue.full())));
                });
                if (shutting_down_) {
                    return;
                }
                source_uri = requested_source_uri_;
                format = *requested_render_format_;
                frame_count = requested_frame_count_;
                generation = requested_source_generation_;
                seek_sequence = requested_seek_sequence_;
                decoder_epoch = requested_decoder_epoch_;
                seek_position = pending_seek_ms_;
                pipeline = decoded_pipeline_.load(std::memory_order_seq_cst);
            }

            const bool needs_open = generation != active_generation ||
                !formats_match(format, active_format) || frame_count != active_frame_count;
            if (needs_open) {
                {
                    std::scoped_lock lock(mutex_);
                    if (!decode_command_is_current_locked(
                            generation, seek_sequence, decoder_epoch)) {
                        continue;
                    }
                }
                decoder_->close();
                auto open_result = decoder_->open(source_uri, format, decoder_epoch);
                if (!open_result) {
                    std::scoped_lock lock(mutex_);
                    if (!decode_command_is_current_locked(
                            generation, seek_sequence, decoder_epoch)) {
                        active_generation = 0;
                        continue;
                    }
                    if (!shutting_down_) {
                        handle_source_error_locked(open_result.error());
                    }
                    active_generation = generation;
                    end_of_stream = true;
                    request_failed = true;
                    failed_generation = generation;
                    failed_seek_sequence = seek_sequence;
                    failed_format = format;
                    failed_frame_count = frame_count;
                    continue;
                }
                {
                    std::scoped_lock lock(mutex_);
                    if (!decode_command_is_current_locked(
                            generation, seek_sequence, decoder_epoch)) {
                        active_generation = 0;
                        continue;
                    }
                }
                if (format.channel_count == 0U || frame_count >
                        (std::numeric_limits<std::size_t>::max)() /
                            static_cast<std::size_t>(format.channel_count)) {
                    throw std::length_error("Decoded PCM slot size exceeds addressable memory.");
                }
                const std::size_t samples_per_slot = static_cast<std::size_t>(frame_count) *
                    static_cast<std::size_t>(format.channel_count);
                pipeline = prepare_pipeline_slot(samples_per_slot, format, frame_count);
                if (!publish_decoded_pipeline_if_current(
                        pipeline, generation, seek_sequence, decoder_epoch)) {
                    active_generation = 0;
                    continue;
                }
                active_generation = generation;
                active_format = format;
                active_frame_count = frame_count;
                active_seek_sequence = 0;
                end_of_stream = false;
                request_failed = false;
                {
                    std::scoped_lock lock(mutex_);
                    if (decode_command_is_current_locked(
                            generation, seek_sequence, decoder_epoch)) {
                        decoder_ready_ = true;
                        state_.duration_ms = decoder_->duration_ms();
                    }
                }
            }

            if (seek_sequence != active_seek_sequence) {
                auto seek_result = decoder_->seek_to(
                    seek_position.value_or(0), decoder_epoch);
                if (!seek_result) {
                    std::scoped_lock lock(mutex_);
                    if (!decode_command_is_current_locked(
                            generation, seek_sequence, decoder_epoch)) {
                        active_generation = 0;
                        continue;
                    }
                    if (!shutting_down_) {
                        handle_source_error_locked(seek_result.error());
                    }
                    request_failed = true;
                    failed_generation = generation;
                    failed_seek_sequence = seek_sequence;
                    failed_format = format;
                    failed_frame_count = frame_count;
                    continue;
                }
                {
                    std::scoped_lock lock(mutex_);
                    if (!decode_command_is_current_locked(
                            generation, seek_sequence, decoder_epoch)) {
                        active_generation = 0;
                        continue;
                    }
                }
                const std::size_t samples_per_slot = static_cast<std::size_t>(frame_count) *
                    static_cast<std::size_t>(format.channel_count);
                pipeline = prepare_pipeline_slot(samples_per_slot, format, frame_count);
                if (!publish_decoded_pipeline_if_current(
                        pipeline, generation, seek_sequence, decoder_epoch)) {
                    active_generation = 0;
                    continue;
                }
                active_seek_sequence = seek_sequence;
                end_of_stream = false;
                request_failed = false;
                std::scoped_lock lock(mutex_);
                if (decode_command_is_current_locked(
                        generation, seek_sequence, decoder_epoch)) {
                    pending_seek_ms_.reset();
                }
            }

            auto decoded_result = decoder_->read_frames(frame_count, decoder_epoch);
            if (!decoded_result) {
                std::scoped_lock lock(mutex_);
                if (shutting_down_) {
                    return;
                }
                if (!decode_command_is_current_locked(
                        generation, seek_sequence, decoder_epoch)) {
                    active_generation = 0;
                    continue;
                }
                handle_source_error_locked(decoded_result.error());
                end_of_stream = true;
                request_failed = true;
                failed_generation = generation;
                failed_seek_sequence = seek_sequence;
                failed_format = format;
                failed_frame_count = frame_count;
                continue;
            }

            auto block = std::move(decoded_result.value());
            const detail::playback::decoded_metadata metadata{
                .generation = generation,
                .seek_sequence = seek_sequence,
                .position_ms = block.position_ms,
                .duration_ms = block.duration_ms,
                .end_of_stream = block.end_of_stream,
            };
            {
                std::scoped_lock lock(mutex_);
                if (!decode_command_is_current_locked(
                        generation, seek_sequence, decoder_epoch)) {
                    active_generation = 0;
                    continue;
                }
                if (pipeline->payload->queue.try_push(block.samples, metadata)) {
                    end_of_stream = block.end_of_stream;
                }
            }
        }
    }

    void decode_worker_entry() noexcept {
        bool can_decode = true;
#if defined(_WIN32)
        detail::win::com_scope com;
        auto com_result = com.initialize_multithreaded();
        if (!com_result) {
            publish_decode_worker_exception(com_result.error().message);
            can_decode = false;
        }
#endif
        if (can_decode) {
            try {
                decode_loop();
            } catch (const std::exception& exception) {
                publish_decode_worker_exception(exception.what());
            } catch (...) {
                publish_decode_worker_exception("Unknown decode worker failure.");
            }
        }
        // Release Media Foundation objects on the same COM-initialized worker
        // before its apartment is uninitialized. External close after join is
        // intentionally idempotent.
        if (decoder_) {
            decoder_->close();
        }
        {
            std::scoped_lock lock(mutex_);
            decode_worker_finished_ = true;
        }
        decode_worker_finished_condition_.notify_all();
    }

    [[nodiscard]] bool decode_command_is_current_locked(
        const std::uint64_t generation,
        const std::uint64_t seek_sequence,
        const std::uint64_t decoder_epoch) const noexcept {
        return !shutting_down_ &&
            generation == requested_source_generation_ &&
            seek_sequence == requested_seek_sequence_ &&
            decoder_epoch == requested_decoder_epoch_;
    }

    void publish_decode_worker_exception(const std::string& message) noexcept {
        try {
            std::scoped_lock lock(mutex_);
            if (shutting_down_) {
                return;
            }
            playback_intent_playing_ = false;
            decoder_ready_ = false;
            state_.status = playback_status::error;
            state_.error_message = message;
            equalizer_state_.status = equalizer_status::error;
            equalizer_state_.error_message = message;
        } catch (...) {
            // The worker boundary must never terminate the process while reporting an OOM.
        }
    }

    decoded_pipeline* prepare_pipeline_slot(
        const std::size_t samples_per_slot,
        const audio_format& format,
        const std::uint32_t frame_count) {
        decoded_pipeline* published = decoded_pipeline_.load(std::memory_order_seq_cst);
        decoded_pipeline* hazard = render_hazard_.load(std::memory_order_seq_cst);
        for (auto& slot : pipeline_slots_) {
            if (&slot != published && &slot != hazard) {
                // With seq_cst publication/hazard operations, either this scan observes a
                // validated callback hazard, or that callback's second current-pointer load
                // observes the intervening publication and retries without touching payload.
                slot.rebuild(samples_per_slot, format, frame_count);
                return &slot;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool publish_decoded_pipeline_if_current(
        decoded_pipeline* pipeline,
        const std::uint64_t generation,
        const std::uint64_t seek_sequence,
        const std::uint64_t decoder_epoch) {
        if (!pipeline) {
            throw std::runtime_error("No free decoded pipeline slot is available.");
        }
        std::scoped_lock lock(mutex_);
        if (!decode_command_is_current_locked(generation, seek_sequence, decoder_epoch)) {
            return false;
        }
        decoded_pipeline_.store(pipeline, std::memory_order_seq_cst);
        last_known_sample_rate_ = static_cast<float>(pipeline->payload->format.sample_rate);
        recompute_equalizer_metadata_locked(last_known_sample_rate_);
        return true;
    }

    void refresh_active_output_device_state(const device_selector& selector) {
        auto active_device_result = backend_->default_device(
            device_direction::render, selector.role);
        if (selector.selection_mode == device_selector::mode::explicit_id) {
            auto devices_result = backend_->enumerate_devices(device_direction::render);
            if (devices_result) {
                const std::string selected_device_id = selector.device_id;
                const auto device_iterator = std::find_if(
                    devices_result.value().begin(),
                    devices_result.value().end(),
                    [&selected_device_id](const device_info& device) {
                        return device.id == selected_device_id;
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
        ++equalizer_control_version_;
    }

    [[nodiscard]] float current_sample_rate_or_default_locked() const {
        return last_known_sample_rate_ > 0.0F ? last_known_sample_rate_ : 48000.0F;
    }

    [[nodiscard]] std::optional<float> resolve_equalizer_sample_rate_locked() const {
        const float callback_sample_rate = negotiated_sample_rate_atomic_.load(std::memory_order_acquire);
        if (callback_sample_rate > 0.0F) {
            return callback_sample_rate;
        }
        std::scoped_lock lock(mutex_);
        if (state_.negotiated_format.has_value() && state_.negotiated_format->sample_rate > 0U) {
            return static_cast<float>(state_.negotiated_format->sample_rate);
        }
        if (last_known_sample_rate_ > 0.0F) {
            return last_known_sample_rate_;
        }

        return std::nullopt;
    }

    void handle_source_error_locked(const error& source_error) {
        playback_intent_playing_ = false;
        decoder_ready_ = false;
        reached_end_of_stream_ = false;
        state_.status = playback_status::error;
        state_.error_message = source_error.message;
    }

    std::shared_ptr<detail::runtime_backend> backend_;
    playback_session_config config_;
    render_callback_adapter callback_;
    render_stream render_stream_;
    mutable std::mutex mutex_;
    std::mutex stream_operation_mutex_;
    std::mutex decoder_command_mutex_;
    std::condition_variable recovery_condition_;
    std::condition_variable decode_condition_;
    std::condition_variable decode_worker_finished_condition_;
    std::thread recovery_thread_;
    std::thread decode_thread_;
    playback_state state_{};
    sonotide::equalizer_state equalizer_state_{};
    detail::dsp::output_headroom_controller headroom_controller_;
    std::vector<equalizer_preset> available_equalizer_presets_ = detail::dsp::builtin_equalizer_presets();
    float last_known_sample_rate_ = 0.0F;
    std::atomic<float> negotiated_sample_rate_atomic_{0.0F};
    bool playback_intent_playing_ = false;
    std::uint64_t equalizer_control_version_ = 0;
    bool decoder_ready_ = false;
    bool recovery_requested_ = false;
    bool shutting_down_ = false;
    bool closed_ = false;
    bool reached_end_of_stream_ = false;
    bool decode_worker_finished_ = false;
    std::uint64_t requested_source_generation_ = 0;
    std::atomic<std::uint64_t> requested_generation_atomic_{0};
    std::uint64_t requested_seek_sequence_ = 0;
    std::uint64_t requested_decoder_epoch_ = 0;
    std::atomic<std::uint64_t> requested_seek_sequence_atomic_{0};
    std::optional<std::int64_t> pending_seek_ms_;
    std::string requested_source_uri_;
    std::optional<audio_format> requested_render_format_;
    std::uint32_t requested_frame_count_ = 0;
    std::array<decoded_pipeline, 3> pipeline_slots_;
    std::atomic<decoded_pipeline*> decoded_pipeline_{nullptr};
    std::atomic<decoded_pipeline*> render_hazard_{nullptr};
    std::unique_ptr<detail::playback::decoder> decoder_;
};

result<playback_session> playback_session::create(
    std::shared_ptr<detail::runtime_backend> backend,
    const playback_session_config& config) {
    return implementation::create(std::move(backend), config);
}

playback_session::playback_session(std::shared_ptr<implementation> implementation) noexcept
    : implementation_(std::move(implementation)) {}

playback_session::~playback_session() {
    auto implementation = implementation_.exchange(nullptr);
    if (implementation) {
        try {
            (void)implementation->close();
        } catch (...) {
            // Destructors cannot surface cleanup exceptions.
        }
    }
}

playback_session::playback_session(playback_session&& other) noexcept
    : implementation_(other.implementation_.exchange(nullptr)) {}

playback_session& playback_session::operator=(playback_session&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    auto replacement = other.implementation_.exchange(nullptr);
    auto previous = implementation_.exchange(std::move(replacement));
    if (previous) {
        try {
            (void)previous->close();
        } catch (...) {
            // noexcept move assignment cannot surface cleanup exceptions.
        }
    }
    return *this;
}

bool playback_session::is_open() const noexcept {
    auto implementation = implementation_.load();
    return implementation && implementation->is_open();
}

result<void> playback_session::load(std::string source_uri) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::load",
            "Playback session is not open."));
    }
    return implementation->load(std::move(source_uri));
}

result<void> playback_session::play() {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::play",
            "Playback session is not open."));
    }
    return implementation->play();
}

result<void> playback_session::pause() {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::pause",
            "Playback session is not open."));
    }
    return implementation->pause();
}

result<void> playback_session::seek_to(const std::int64_t position_ms) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::seek_to",
            "Playback session is not open."));
    }
    return implementation->seek_to(position_ms);
}

result<void> playback_session::set_volume_percent(const int volume_percent) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::set_volume_percent",
            "Playback session is not open."));
    }
    return implementation->set_volume_percent(volume_percent);
}

result<void> playback_session::set_equalizer_enabled(const bool enabled) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::set_equalizer_enabled",
            "Playback session is not open."));
    }
    return implementation->set_equalizer_enabled(enabled);
}

result<void> playback_session::select_equalizer_preset(const equalizer_preset_id preset_id) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::select_equalizer_preset",
            "Playback session is not open."));
    }
    return implementation->select_equalizer_preset(preset_id);
}

result<void> playback_session::set_equalizer_band_gain(
    const std::size_t band_index,
    const float gain_db) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::set_equalizer_band_gain",
            "Playback session is not open."));
    }
    return implementation->set_equalizer_band_gain(band_index, gain_db);
}

result<void> playback_session::set_equalizer_band_q(
    const std::size_t band_index,
    const float q_value) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::set_equalizer_band_q",
            "Playback session is not open."));
    }
    return implementation->set_equalizer_band_q(band_index, q_value);
}

result<void> playback_session::add_equalizer_band(
    const float center_frequency_hz,
    const float gain_db) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::add_equalizer_band",
            "Playback session is not open."));
    }
    return implementation->add_equalizer_band(center_frequency_hz, gain_db);
}

result<void> playback_session::remove_equalizer_band(const std::size_t band_index) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::remove_equalizer_band",
            "Playback session is not open."));
    }
    return implementation->remove_equalizer_band(band_index);
}

result<void> playback_session::set_equalizer_band_frequency(
    const std::size_t band_index,
    const float center_frequency_hz) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::set_equalizer_band_frequency",
            "Playback session is not open."));
    }
    return implementation->set_equalizer_band_frequency(band_index, center_frequency_hz);
}

result<void> playback_session::reset_equalizer() {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::reset_equalizer",
            "Playback session is not open."));
    }
    return implementation->reset_equalizer();
}

result<void> playback_session::set_equalizer_output_gain(const float output_gain_db) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::set_equalizer_output_gain",
            "Playback session is not open."));
    }
    return implementation->set_equalizer_output_gain(output_gain_db);
}

result<void> playback_session::apply_equalizer_state(const sonotide::equalizer_state& state) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::apply_equalizer_state",
            "Playback session is not open."));
    }
    return implementation->apply_equalizer_state(state);
}

result<std::vector<device_info>> playback_session::list_output_devices() const {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<std::vector<device_info>>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::list_output_devices",
            "Playback session is not open."));
    }
    return implementation->list_output_devices();
}

result<void> playback_session::select_output_device(std::string device_id) {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<void>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::select_output_device",
            "Playback session is not open."));
    }
    return implementation->select_output_device(std::move(device_id));
}

playback_state playback_session::state() const {
    auto implementation = implementation_.load();
    if (!implementation) {
        return {};
    }
    return implementation->state();
}

sonotide::equalizer_state playback_session::equalizer_state() const {
    auto implementation = implementation_.load();
    if (!implementation) {
        return {};
    }
    return implementation->equalizer_state();
}

result<sonotide::equalizer_response_curve> playback_session::sample_equalizer_response(
    const std::span<const float> frequencies_hz) const {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<sonotide::equalizer_response_curve>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::sample_equalizer_response",
            "Playback session is not open."));
    }
    return implementation->sample_equalizer_response(frequencies_hz);
}

result<sonotide::equalizer_response_curve> playback_session::preview_equalizer_response(
    const equalizer_preview_state& preview_state,
    const std::span<const float> frequencies_hz) const {
    auto implementation = implementation_.load();
    if (!implementation) {
        return result<sonotide::equalizer_response_curve>::failure(make_error(
            error_category::stream,
            error_code::invalid_state,
            "playback_session::preview_equalizer_response",
            "Playback session is not open."));
    }
    return implementation->preview_equalizer_response(preview_state, frequencies_hz);
}

std::optional<sonotide::equalizer_frequency_range> playback_session::equalizer_band_frequency_range(
    const std::size_t band_index) const {
    auto implementation = implementation_.load();
    if (!implementation) {
        return std::nullopt;
    }
    return implementation->equalizer_band_frequency_range(band_index);
}

result<void> playback_session::close() {
    auto implementation = implementation_.exchange(nullptr);
    if (!implementation) {
        return result<void>::success();
    }
    return implementation->close();
}

}  // namespace sonotide
