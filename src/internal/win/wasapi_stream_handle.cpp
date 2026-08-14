#include "internal/win/wasapi_stream_handle.h"

#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <avrt.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "internal/state_machine.h"
#include "internal/win/com_scope.h"
#include "internal/win/device_utils.h"
#include "internal/win/hresult_utils.h"
#include "internal/win/wasapi_stream_validation.h"
#include "internal/win/wave_format_utils.h"

namespace sonotide::detail::win {
namespace {

using Microsoft::WRL::ComPtr;

/// Возвращает текущее значение QPC, приведённое к 100-нс единицам.
std::uint64_t current_qpc_100ns() {
    /// Текущее значение high-resolution счётчика.
    LARGE_INTEGER now;
    /// Частота high-resolution счётчика.
    LARGE_INTEGER frequency;
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency)) {
        return 0;
    }
    return scale_qpc_to_100ns(now.QuadPart, frequency.QuadPart).value_or(0);
}

/// Нормализует `format_request` в публичный `audio_format` для status snapshot-а.
audio_format requested_format_from_request(const format_request& request) {
    /// Формат, отражающий пользовательские предпочтения до negotiation.
    audio_format format;
    format.sample = request.preferred_sample.value_or(sample_type::unknown);
    format.sample_rate = request.preferred_sample_rate.value_or(0);
    format.channel_count = request.preferred_channel_count.value_or(0);
    format.interleaved = request.interleaved;
    return format;
}

/// Собирает типовую ошибку о недопустимом состоянии потока.
error make_invalid_state_error(const char* operation, const char* message) {
    /// Объект ошибки, возвращаемый пользователю.
    error failure;
    failure.category = error_category::stream;
    failure.code = error_code::invalid_state;
    failure.operation = operation;
    failure.message = message;
    return failure;
}

/// Собирает типовую ошибку о намеренно не реализованной возможности.
error make_not_implemented_error(const char* operation, const char* message) {
    /// Объект ошибки, возвращаемый пользователю.
    error failure;
    failure.category = error_category::configuration;
    failure.code = error_code::not_implemented;
    failure.operation = operation;
    failure.message = message;
    return failure;
}

/// Собирает ошибку валидации публичной конфигурации до создания worker-а.
error make_invalid_argument_error(const char* operation, const char* message) {
    error failure;
    failure.category = error_category::configuration;
    failure.code = error_code::invalid_argument;
    failure.operation = operation;
    failure.message = message;
    return failure;
}

/// Валидирует общие ограничения текущего shared-mode WASAPI backend-а.
result<void> validate_stream_request(
    const format_request& format,
    const stream_timing& timing,
    const share_mode mode,
    const callback_mode callback,
    const char* operation) {
    if (!milliseconds_to_reference_time(timing.target_latency)) {
        return result<void>::failure(make_invalid_argument_error(
            operation, "Target latency must be positive and representable in 100-ns units."));
    }
    if (!is_known_share_mode(mode)) {
        return result<void>::failure(make_invalid_argument_error(
            operation, "Share mode contains an unknown enum value."));
    }
    if (mode == share_mode::exclusive) {
        return result<void>::failure(make_not_implemented_error(
            operation, "Exclusive WASAPI mode is not implemented."));
    }
    if (!is_valid_callback_mode(callback)) {
        return result<void>::failure(make_invalid_argument_error(
            operation, "Callback mode contains an unknown enum value."));
    }
    if (timing.engine_period.has_value()) {
        return result<void>::failure(make_not_implemented_error(
            operation, "Custom WASAPI engine periods are not implemented."));
    }
    if (!format.interleaved) {
        return result<void>::failure(make_not_implemented_error(
            operation, "The WASAPI backend currently supports interleaved audio only."));
    }

    const int preferred_hint_count =
        static_cast<int>(format.preferred_sample.has_value()) +
        static_cast<int>(format.preferred_sample_rate.has_value()) +
        static_cast<int>(format.preferred_channel_count.has_value());
    if (preferred_hint_count != 0 && preferred_hint_count != 3) {
        return result<void>::failure(make_invalid_argument_error(
            operation,
            "Preferred sample type, sample rate, and channel count must be provided together."));
    }
    if (format.preferred_sample == sample_type::unknown) {
        return result<void>::failure(make_invalid_argument_error(
            operation, "Preferred sample type cannot be unknown."));
    }
    if (format.preferred_sample_rate.has_value() && *format.preferred_sample_rate == 0) {
        return result<void>::failure(make_invalid_argument_error(
            operation, "Preferred sample rate must be greater than zero."));
    }
    if (format.preferred_channel_count.has_value() && *format.preferred_channel_count == 0) {
        return result<void>::failure(make_invalid_argument_error(
            operation, "Preferred channel count must be greater than zero."));
    }
    return result<void>::success();
}

/// Проверяет, относится ли HRESULT к потере WASAPI-устройства.
bool is_device_lost_hresult(const HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED ||
           hr == AUDCLNT_E_RESOURCES_INVALIDATED;
}

/// Формирует стабильную ошибку, когда пользовательский callback нарушает noexcept-границу worker-а.
error make_callback_exception_error(const char* operation, const char* message) {
    error failure;
    failure.category = error_category::callback;
    failure.code = error_code::callback_failed;
    failure.operation = operation;
    failure.message = message;
    return failure;
}

/// Узкая RAII-обёртка над Win32 HANDLE, предотвращающая утечки на ранних выходах.
class unique_handle {
public:
    unique_handle() = default;
    explicit unique_handle(HANDLE handle) noexcept : handle_(handle) {}

    ~unique_handle() {
        reset();
    }

    unique_handle(unique_handle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

/// Гарантирует ReleaseBuffer render-буфера даже при исключении callback-а.
class render_buffer_scope {
public:
    render_buffer_scope(IAudioRenderClient& client, UINT32 frames) noexcept
        : client_(client), frames_(frames) {}

    ~render_buffer_scope() {
        if (active_) {
            (void)client_.ReleaseBuffer(frames_, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    [[nodiscard]] HRESULT release(const DWORD flags) noexcept {
        active_ = false;
        return client_.ReleaseBuffer(frames_, flags);
    }

private:
    IAudioRenderClient& client_;
    UINT32 frames_ = 0;
    bool active_ = true;
};

/// Гарантирует ReleaseBuffer capture-пакета даже при исключении callback-а.
class capture_buffer_scope {
public:
    capture_buffer_scope(IAudioCaptureClient& client, UINT32 frames) noexcept
        : client_(client), frames_(frames) {}

    ~capture_buffer_scope() {
        if (active_) {
            (void)client_.ReleaseBuffer(frames_);
        }
    }

    [[nodiscard]] HRESULT release() noexcept {
        active_ = false;
        return client_.ReleaseBuffer(frames_);
    }

private:
    IAudioCaptureClient& client_;
    UINT32 frames_ = 0;
    bool active_ = true;
};

/// RAII-объект для временного включения MMCSS-приоритета на рабочем потоке.
class mmcss_scope {
public:
    /// Поднимает приоритет потока только если это разрешено runtime options.
    explicit mmcss_scope(const bool enabled) {
        if (!enabled) {
            return;
        }

        /// Индекс task profile, возвращаемый MMCSS.
        DWORD index = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    }

    /// Возвращает поток к обычному приоритету при уничтожении scope.
    ~mmcss_scope() {
        if (handle_ != nullptr) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }

private:
    /// MMCSS-дескриптор текущего рабочего потока.
    HANDLE handle_ = nullptr;
};

template <typename callback_type, typename config_type>
class wasapi_stream_handle_base
    : public stream_handle,
      public std::enable_shared_from_this<wasapi_stream_handle_base<callback_type, config_type>> {
public:
    /// Сохраняет конфигурацию и переводит state machine в prepared.
    wasapi_stream_handle_base(config_type config, callback_type& callback, runtime_options options)
        : config_(std::move(config)),
          callback_(callback),
          options_(options) {
        (void)state_machine_.transition(stream_transition::prepare);
        status_.state = stream_state::prepared;
    }

    /// Закрывает поток при уничтожении базового handle.
    ~wasapi_stream_handle_base() override {
        (void)close();
    }

    /// Останавливает worker thread и переводит поток в stopped, если это допустимо.
    result<void> stop() override {
        std::unique_lock lock(mutex_);
        if (closed_) {
            return result<void>::failure(
                make_invalid_state_error("stream::stop", "Stream is already closed."));
        }
        if (!worker_active_ &&
            (status_.state == stream_state::prepared || status_.state == stream_state::stopped)) {
            return result<void>::success();
        }

        if (!worker_active_ && status_.state != stream_state::faulted) {
            return result<void>::failure(
                make_invalid_state_error("stream::stop", "Stream is not running."));
        }

        if (state_machine_.state() == stream_state::running) {
            auto transition = state_machine_.transition(stream_transition::stop);
            if (!transition) {
                return result<void>::failure(transition.error());
            }
        }

        /// Сигнал выставляется под lock, чтобы worker не мог закрыть тот же HANDLE одновременно.
        stop_requested_ = true;
        if (stop_event_) {
            (void)SetEvent(stop_event_.get());
        }

        /// Lifecycle-вызов из callback-а не может ждать сам себя. Self-ownership worker-а
        /// удерживает handle живым до фактического выхода из worker_entry(). Такой вызов
        /// только запрашивает остановку: внешний поток должен повторить stop/close.
        if (is_worker_thread_locked()) {
            return result<void>::failure(make_invalid_state_error(
                "stream::stop",
                "Stop was requested from the audio callback and cannot complete synchronously; "
                "retry from a non-callback thread."));
        }

        worker_cv_.wait(lock, [this]() { return !worker_active_; });
        if (!closed_ && status_.state != stream_state::faulted) {
            status_.state = stream_state::stopped;
        }
        return result<void>::success();
    }

    /// Возвращает поток в prepared и очищает negotiated runtime state.
    result<void> reset() override {
        {
            std::scoped_lock lock(mutex_);
            if (is_worker_thread_locked()) {
                return result<void>::failure(make_invalid_state_error(
                    "stream::reset",
                    "A stream cannot be reset from its own audio callback."));
            }
        }

        auto stopped = stop();
        if (!stopped && stopped.error().code != error_code::invalid_state) {
            return stopped;
        }

        std::scoped_lock lock(mutex_);
        if (closed_) {
            return result<void>::failure(
                make_invalid_state_error("stream::reset", "Stream is already closed."));
        }
        auto transition = state_machine_.transition(stream_transition::reset);
        if (!transition) {
            return result<void>::failure(transition.error());
        }

        status_.state = stream_state::prepared;
        status_.negotiated_format.reset();
        status_.statistics = {};
        callback_count_.store(0, std::memory_order_relaxed);
        frames_processed_.store(0, std::memory_order_relaxed);
        discontinuity_count_.store(0, std::memory_order_relaxed);
        status_.device_lost = false;
        return result<void>::success();
    }

    /// Полностью закрывает поток и освобождает связанные event-ресурсы.
    result<void> close() override {
        std::unique_lock lock(mutex_);
        if (closed_) {
            return result<void>::success();
        }

        /// Self-close не может гарантировать окончание текущего виртуального callback-а.
        /// Сигнализируем worker, но не публикуем success/closed и не разрешаем wrapper-у
        /// отсоединиться от handle: внешний поток обязан повторить close после callback-а.
        stop_requested_ = true;
        if (stop_event_) {
            (void)SetEvent(stop_event_.get());
        }
        if (is_worker_thread_locked()) {
            return result<void>::failure(make_invalid_state_error(
                "stream::close",
                "Close was requested from the audio callback and cannot complete synchronously; "
                "retry from a non-callback thread before destroying the callback."));
        }

        /// Закрытое состояние публикуется до ожидания, запрещая параллельный restart.
        closed_ = true;
        (void)state_machine_.transition(stream_transition::close);
        status_.state = stream_state::closed;
        status_.negotiated_format.reset();
        worker_cv_.wait(lock, [this]() { return !worker_active_; });
        return result<void>::success();
    }

    /// Возвращает потокобезопасный снимок текущего состояния потока.
    [[nodiscard]] stream_status status() const override {
        std::scoped_lock lock(mutex_);
        auto snapshot = status_;
        snapshot.statistics.callback_count = callback_count_.load(std::memory_order_relaxed);
        snapshot.statistics.frames_processed = frames_processed_.load(std::memory_order_relaxed);
        snapshot.statistics.discontinuity_count =
            discontinuity_count_.load(std::memory_order_relaxed);
        return snapshot;
    }

protected:
    /// Подготавливает запуск и атомарно привязывает self-owned detached worker.
    template <typename worker_function>
    [[nodiscard]] result<void> launch_worker(worker_function&& worker) {
        auto self = this->weak_from_this().lock();
        if (!self) {
            return result<void>::failure(make_invalid_state_error(
                "stream::start", "Stream handle is not managed by shared ownership."));
        }

        stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop_event_) {
            return result<void>::failure(make_invalid_state_error(
                "stream::start", "Failed to allocate stop event."));
        }

        stop_requested_ = false;
        startup_completed_ = false;
        startup_error_.reset();
        worker_active_ = true;

        try {
            std::thread worker_thread(
                [self = std::move(self), worker = std::forward<worker_function>(worker)]() mutable noexcept {
                    self->worker_entry(worker);
                });
            worker_id_ = worker_thread.get_id();
            worker_thread.detach();
        } catch (const std::exception& exception) {
            worker_active_ = false;
            worker_id_ = {};
            stop_event_.reset();
            return result<void>::failure(make_callback_exception_error(
                "std::thread", exception.what()));
        } catch (...) {
            worker_active_ = false;
            worker_id_ = {};
            stop_event_.reset();
            return result<void>::failure(make_callback_exception_error(
                "std::thread", "Failed to create the audio worker thread."));
        }

        return result<void>::success();
    }

    template <typename worker_function>
    void worker_entry(worker_function& worker) noexcept {
        try {
            worker();
        } catch (const std::exception& exception) {
            try {
                publish_unhandled_worker_exception(exception.what());
            } catch (...) {
                publish_emergency_worker_failure();
            }
        } catch (...) {
            try {
                publish_unhandled_worker_exception("Unknown exception escaped the audio worker.");
            } catch (...) {
                publish_emergency_worker_failure();
            }
        }

        std::scoped_lock lock(mutex_);
        if (!startup_completed_) {
            startup_error_ = make_callback_exception_error(
                "stream::worker", "Audio worker exited before completing startup.");
            startup_completed_ = true;
            startup_cv_.notify_all();
        }
        worker_active_ = false;
        worker_id_ = {};
        stop_event_.reset();
        if (!closed_ && status_.state != stream_state::faulted) {
            status_.state = stream_state::stopped;
        }
        worker_cv_.notify_all();
    }

    /// Фиксирует итог startup-пути и будит ожидающий вызывающий поток.
    void set_startup_result_locked(std::optional<error> failure = std::nullopt) {
        startup_error_ = std::move(failure);
        startup_completed_ = true;
        startup_cv_.notify_all();
    }

    /// Помечает поток как faulted и пересылает ошибку пользовательскому callback-у.
    void record_runtime_error(error failure) noexcept {
        bool notify_callback = false;
        {
            std::scoped_lock lock(mutex_);
            if (is_device_lost_hresult(static_cast<HRESULT>(failure.native_code.value_or(0)))) {
                status_.device_lost = true;
            }
            if (!closed_) {
                (void)state_machine_.transition(stream_transition::fault);
                status_.state = stream_state::faulted;
                notify_callback = true;
            }
        }
        if (!notify_callback) {
            return;
        }
        try {
            callback_.on_stream_error(failure);
        } catch (...) {
            /// Ошибка диагностического callback-а не должна пересечь worker/COM boundary.
        }
    }

    [[nodiscard]] HANDLE stop_event_locked() const noexcept {
        return stop_event_.get();
    }

    void publish_startup_failure(error failure) {
        std::scoped_lock lock(mutex_);
        if (!closed_ && state_machine_.state() == stream_state::running) {
            (void)state_machine_.transition(stream_transition::fault);
            status_.state = stream_state::faulted;
        }
        set_startup_result_locked(std::move(failure));
    }

    [[nodiscard]] bool cancel_startup_if_requested(const char* operation) {
        std::scoped_lock lock(mutex_);
        if (!stop_requested_ && !closed_) {
            return false;
        }
        set_startup_result_locked(make_invalid_state_error(
            operation, "Stream startup was cancelled by a concurrent stop or close."));
        return true;
    }

    void publish_unhandled_worker_exception(const char* message) {
        auto failure = make_callback_exception_error("stream::worker", message);
        bool startup_pending = false;
        {
            std::scoped_lock lock(mutex_);
            startup_pending = !startup_completed_;
        }
        if (startup_pending) {
            publish_startup_failure(std::move(failure));
        } else {
            record_runtime_error(std::move(failure));
        }
    }

    void publish_emergency_worker_failure() noexcept {
        std::scoped_lock lock(mutex_);
        if (!closed_) {
            (void)state_machine_.transition(stream_transition::fault);
            status_.state = stream_state::faulted;
        }
        startup_completed_ = true;
        startup_cv_.notify_all();
    }

    [[nodiscard]] bool is_worker_thread_locked() const noexcept {
        return worker_active_ && worker_id_ == std::this_thread::get_id();
    }

    /// Сериализует lifecycle-операции и доступ к status.
    mutable std::mutex mutex_;
    /// Сигнализирует завершение startup-пути worker thread.
    std::condition_variable startup_cv_;
    /// Сигнализирует окончательный выход detached worker-а и освобождение native resources.
    std::condition_variable worker_cv_;
    /// Контролирует допустимые lifecycle transition-ы.
    detail::stream_state_machine state_machine_;
    /// Конфигурация конкретного render/capture потока.
    config_type config_;
    /// Пользовательский callback потока.
    callback_type& callback_;
    /// Общие runtime options backend-а.
    runtime_options options_;
    /// Публичный status snapshot потока.
    stream_status status_;
    /// Relaxed atomics достаточны: независимая телеметрия не синхронизирует lifecycle.
    std::atomic<std::uint64_t> callback_count_{0};
    std::atomic<std::uint64_t> frames_processed_{0};
    std::atomic<std::uint64_t> discontinuity_count_{0};
    /// Событие остановки worker thread.
    unique_handle stop_event_;
    /// Идентификатор worker-а, необходимый для безопасных lifecycle-вызовов из callback-а.
    std::thread::id worker_id_;
    /// Worker держит self-reference до завершения и публикует этот флаг последним.
    bool worker_active_ = false;
    /// Признак запрошенной остановки потока.
    bool stop_requested_ = false;
    /// Признак завершения startup-пути worker thread.
    bool startup_completed_ = false;
    /// Ошибка, произошедшая на startup, если она была.
    std::optional<error> startup_error_;
    /// Признак полного закрытия handle.
    bool closed_ = false;
};

/// Результат общей инициализации WASAPI client-а для render/capture path-ов.
struct client_init_result {
    /// Аудиоклиент WASAPI выбранного endpoint-а.
    ComPtr<IAudioClient> audio_client;
    /// Согласованный формат и сопутствующие wave-format метаданные.
    negotiated_format format;
    /// Event handle, которым WASAPI будит рабочий поток.
    unique_handle audio_event;
    /// Размер endpoint buffer в кадрах.
    UINT32 buffer_frame_count = 0;
};

/// Поднимает общий event-driven WASAPI client и возвращает всё нужное для worker loop.
result<client_init_result> initialize_audio_client(
    const device_selector& selector,
    const format_request& request,
    const std::chrono::milliseconds target_latency,
    const DWORD stream_flags) {
    /// COM enumerator для разрешения device selector.
    auto enumerator_result = create_device_enumerator();
    if (!enumerator_result) {
        return result<client_init_result>::failure(enumerator_result.error());
    }

    /// Конкретное устройство, выбранное пользователем или системой по умолчанию.
    auto device_result = resolve_device(*enumerator_result.value().Get(), selector);
    if (!device_result) {
        return result<client_init_result>::failure(device_result.error());
    }

    /// Основной WASAPI client выбранного endpoint-а.
    ComPtr<IAudioClient> audio_client;
    HRESULT hr = device_result.value().device->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(audio_client.GetAddressOf()));
    if (FAILED(hr)) {
        return result<client_init_result>::failure(map_hresult(
            "IMMDevice::Activate(IAudioClient)",
            hr,
            error_category::initialization,
            error_code::stream_open_failed));
    }

    /// Формат, согласованный между запросом и shared-mode endpoint-ом.
    auto format_result = negotiate_shared_mode_format(*audio_client.Get(), request);
    if (!format_result) {
        return result<client_init_result>::failure(format_result.error());
    }

    /// Event handle, который будет использоваться в event-driven worker loop.
    unique_handle audio_event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!audio_event) {
        error failure;
        failure.category = error_category::platform;
        failure.code = error_code::platform_failure;
        failure.operation = "CreateEventW";
        failure.message = "Failed to allocate stream event handle.";
        return result<client_init_result>::failure(std::move(failure));
    }

    const auto buffer_duration = milliseconds_to_reference_time(target_latency);
    if (!buffer_duration) {
        return result<client_init_result>::failure(make_invalid_argument_error(
            "IAudioClient::Initialize",
            "Target latency is not representable in 100-ns units."));
    }

    hr = audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        stream_flags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        static_cast<REFERENCE_TIME>(*buffer_duration),
        0,
        format_result.value().wave_format.get(),
        nullptr);
    if (FAILED(hr)) {
        return result<client_init_result>::failure(map_hresult(
            "IAudioClient::Initialize",
            hr,
            error_category::initialization,
            error_code::stream_open_failed));
    }

    hr = audio_client->SetEventHandle(audio_event.get());
    if (FAILED(hr)) {
        return result<client_init_result>::failure(map_hresult(
            "IAudioClient::SetEventHandle",
            hr,
            error_category::initialization,
            error_code::stream_open_failed));
    }

    /// Размер endpoint buffer после успешной инициализации audio client-а.
    UINT32 buffer_frame_count = 0;
    hr = audio_client->GetBufferSize(&buffer_frame_count);
    if (FAILED(hr)) {
        return result<client_init_result>::failure(map_hresult(
            "IAudioClient::GetBufferSize",
            hr,
            error_category::initialization,
            error_code::stream_open_failed));
    }

    /// Возвращаемый набор нативных ресурсов и negotiated format.
    client_init_result result_value;
    result_value.audio_client = std::move(audio_client);
    result_value.format = std::move(format_result.value());
    result_value.audio_event = std::move(audio_event);
    result_value.buffer_frame_count = buffer_frame_count;
    return result<client_init_result>::success(std::move(result_value));
}

class wasapi_render_stream_handle final
    : public wasapi_stream_handle_base<render_callback, render_stream_config> {
public:
    /// Фиксирует requested format для последующего stream status snapshot-а.
    wasapi_render_stream_handle(
        render_stream_config config,
        render_callback& callback,
        runtime_options options)
        : wasapi_stream_handle_base(std::move(config), callback, options) {
        status_.requested_format = requested_format_from_request(config_.format);
    }

    /// Стартует render worker thread и дожидается окончания startup-пути.
    result<void> start() override {
        std::unique_lock lock(mutex_);
        if (closed_) {
            return result<void>::failure(
                make_invalid_state_error("render_stream::start", "Stream is already closed."));
        }
        if (worker_active_) {
            return result<void>::failure(make_invalid_state_error(
                "render_stream::start",
                "The previous audio worker has not finished stopping yet."));
        }

        auto validation = validate_stream_request(
            config_.format,
            config_.timing,
            config_.mode,
            config_.callback,
            "render_stream::start");
        if (!validation) {
            return validation;
        }

        auto transition = state_machine_.transition(stream_transition::start);
        if (!transition) {
            return result<void>::failure(transition.error());
        }

        auto launched = launch_worker([this]() { worker_main(); });
        if (!launched) {
            (void)state_machine_.transition(stream_transition::fault);
            status_.state = stream_state::faulted;
            return launched;
        }

        /// Detached worker владеет handle через shared_ptr; start ждёт только публикацию startup.
        startup_cv_.wait(lock, [this]() { return startup_completed_; });

        if (startup_error_) {
            auto failure = *startup_error_;
            worker_cv_.wait(lock, [this]() { return !worker_active_; });
            return result<void>::failure(std::move(failure));
        }

        status_.state = stream_state::running;
        return result<void>::success();
    }

private:
    /// Основной render loop: инициализация WASAPI, priming buffer и callback-рендеринг.
    void worker_main() {
        /// COM apartment worker thread-а.
        com_scope com;
        auto com_result = com.initialize_multithreaded();
        if (!com_result) {
            publish_startup_failure(com_result.error());
            return;
        }

        /// Временное повышение приоритета audio worker thread-а.
        mmcss_scope mmcss(options_.enable_mmcss);

        /// Набор ресурсов, необходимых для render path.
        auto init_result = initialize_audio_client(
            config_.device,
            config_.format,
            config_.timing.target_latency,
            AUDCLNT_STREAMFLAGS_NOPERSIST);
        if (!init_result) {
            publish_startup_failure(init_result.error());
            return;
        }

        /// Render service, выдающий writable endpoint buffer.
        ComPtr<IAudioRenderClient> render_client;
        HRESULT hr = init_result.value().audio_client->GetService(
            __uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(render_client.GetAddressOf()));
        if (FAILED(hr)) {
            publish_startup_failure(map_hresult(
                "IAudioClient::GetService(IAudioRenderClient)",
                hr,
                error_category::stream,
                error_code::stream_open_failed));
            return;
        }

        if (config_.prefill_with_silence) {
            /// Priming действительно публикует тишину и не вызывает пользовательский callback.
            BYTE* initial_buffer = nullptr;
            hr = render_client->GetBuffer(init_result.value().buffer_frame_count, &initial_buffer);
            if (FAILED(hr)) {
                publish_startup_failure(map_hresult(
                    "IAudioRenderClient::GetBuffer",
                    hr,
                    error_category::stream,
                    error_code::stream_open_failed));
                return;
            }
            render_buffer_scope initial_buffer_scope(
                *render_client.Get(), init_result.value().buffer_frame_count);
            hr = initial_buffer_scope.release(AUDCLNT_BUFFERFLAGS_SILENT);
            if (FAILED(hr)) {
                publish_startup_failure(map_hresult(
                    "IAudioRenderClient::ReleaseBuffer",
                    hr,
                    error_category::stream,
                    error_code::stream_start_failed,
                    is_device_lost_hresult(hr)));
                return;
            }
        }

        if (cancel_startup_if_requested("render_stream::start")) {
            return;
        }

        /// Запускает consumption endpoint buffer со стороны WASAPI engine.
        hr = init_result.value().audio_client->Start();
        if (FAILED(hr)) {
            publish_startup_failure(map_hresult(
                "IAudioClient::Start",
                hr,
                error_category::stream,
                error_code::stream_start_failed));
            return;
        }

        bool cancelled_after_start = false;
        {
            std::scoped_lock lock(mutex_);
            if (stop_requested_ || closed_) {
                cancelled_after_start = true;
                set_startup_result_locked(make_invalid_state_error(
                    "render_stream::start",
                    "Stream startup was cancelled by a concurrent stop or close."));
            } else {
                /// Публикует negotiated format только после успешного старта.
                status_.negotiated_format = init_result.value().format.public_format;
                status_.state = stream_state::running;
                set_startup_result_locked();
            }
        }
        if (cancelled_after_start) {
            (void)init_result.value().audio_client->Stop();
            return;
        }

        /// События ожидания: ручная остановка и очередной сигнал от WASAPI.
        HANDLE stop_event = nullptr;
        {
            std::scoped_lock lock(mutex_);
            stop_event = stop_event_locked();
        }
        const HANDLE wait_handles[2] = {stop_event, init_result.value().audio_event.get()};
        /// Количество кадров, уже переданных render callback-у.
        std::uint64_t frames_written = 0;
        while (true) {
            /// Ожидает либо stop signal, либо готовность очередной порции endpoint buffer.
            const DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
            if (wait_result == WAIT_OBJECT_0) {
                break;
            }
            if (wait_result != WAIT_OBJECT_0 + 1) {
                record_runtime_error(make_invalid_state_error(
                    "WaitForMultipleObjects",
                    "Unexpected render wait result."));
                break;
            }

            UINT32 padding = 0;
            hr = init_result.value().audio_client->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                record_runtime_error(map_hresult(
                    "IAudioClient::GetCurrentPadding",
                    hr,
                    error_category::stream,
                    error_code::stream_stop_failed,
                    is_device_lost_hresult(hr)));
                break;
            }
            if (padding > init_result.value().buffer_frame_count) {
                record_runtime_error(make_invalid_state_error(
                    "IAudioClient::GetCurrentPadding",
                    "WASAPI reported padding larger than the endpoint buffer."));
                break;
            }
            /// Свободное место в endpoint buffer, доступное для записи.
            const UINT32 frames_available = init_result.value().buffer_frame_count - padding;
            if (frames_available == 0) {
                continue;
            }

            /// Writable-указатель на свободную часть endpoint buffer.
            BYTE* buffer = nullptr;
            hr = render_client->GetBuffer(frames_available, &buffer);
            if (FAILED(hr)) {
                record_runtime_error(map_hresult(
                    "IAudioRenderClient::GetBuffer",
                    hr,
                    error_category::stream,
                    error_code::stream_stop_failed,
                    is_device_lost_hresult(hr)));
                break;
            }
            render_buffer_scope buffer_scope(*render_client.Get(), frames_available);
            const auto byte_count = checked_audio_byte_count(
                frames_available, init_result.value().format.block_align);
            if (!byte_count) {
                (void)buffer_scope.release(AUDCLNT_BUFFERFLAGS_SILENT);
                record_runtime_error(make_invalid_state_error(
                    "render_stream::worker",
                    "Render buffer byte count exceeds the addressable range."));
                break;
            }

            /// Публичное представление render buffer для callback-а.
            audio_buffer_view view{
                std::span<std::byte>(
                    reinterpret_cast<std::byte*>(buffer),
                    *byte_count),
                frames_available,
                init_result.value().format.public_format,
            };
            /// Пользовательский callback пишет PCM непосредственно в endpoint buffer.
            result<void> callback_result = result<void>::success();
            try {
                callback_result = callback_.on_render(
                    view,
                    stream_timestamp{frames_written, current_qpc_100ns()});
            } catch (const std::exception& exception) {
                callback_result = result<void>::failure(make_callback_exception_error(
                    "render_callback::on_render", exception.what()));
            } catch (...) {
                callback_result = result<void>::failure(make_callback_exception_error(
                    "render_callback::on_render", "Render callback threw an unknown exception."));
            }
            if (!callback_result) {
                (void)buffer_scope.release(AUDCLNT_BUFFERFLAGS_SILENT);
                record_runtime_error(callback_result.error());
                break;
            }

            hr = buffer_scope.release(0);
            if (FAILED(hr)) {
                record_runtime_error(map_hresult(
                    "IAudioRenderClient::ReleaseBuffer",
                    hr,
                    error_category::stream,
                    error_code::stream_stop_failed,
                    is_device_lost_hresult(hr)));
                break;
            }

            frames_written += frames_available;
            callback_count_.fetch_add(1, std::memory_order_relaxed);
            frames_processed_.fetch_add(frames_available, std::memory_order_relaxed);
        }

        /// Корректно завершает audio client и освобождает event handle.
        init_result.value().audio_client->Stop();
        /// audio_event освобождается RAII после Stop и уничтожения service interface-ов.
    }
};

class wasapi_capture_stream_handle final
    : public wasapi_stream_handle_base<capture_callback, capture_stream_config> {
public:
    /// Сохраняет requested format и запоминает, работает ли поток в loopback-режиме.
    wasapi_capture_stream_handle(
        capture_stream_config config,
        capture_callback& callback,
        runtime_options options,
        bool loopback)
        : wasapi_stream_handle_base(std::move(config), callback, options),
          loopback_(loopback) {
        status_.requested_format = requested_format_from_request(config_.format);
    }

    /// Стартует capture worker thread и дожидается окончания startup-пути.
    result<void> start() override {
        std::unique_lock lock(mutex_);
        if (closed_) {
            return result<void>::failure(
                make_invalid_state_error("capture_stream::start", "Stream is already closed."));
        }
        if (worker_active_) {
            return result<void>::failure(make_invalid_state_error(
                "capture_stream::start",
                "The previous audio worker has not finished stopping yet."));
        }

        auto validation = validate_stream_request(
            config_.format,
            config_.timing,
            config_.mode,
            config_.callback,
            "capture_stream::start");
        if (!validation) {
            return validation;
        }

        auto transition = state_machine_.transition(stream_transition::start);
        if (!transition) {
            return result<void>::failure(transition.error());
        }

        auto launched = launch_worker([this]() { worker_main(); });
        if (!launched) {
            (void)state_machine_.transition(stream_transition::fault);
            status_.state = stream_state::faulted;
            return launched;
        }

        /// Detached worker владеет handle через shared_ptr; start ждёт только публикацию startup.
        startup_cv_.wait(lock, [this]() { return startup_completed_; });

        if (startup_error_) {
            auto failure = *startup_error_;
            worker_cv_.wait(lock, [this]() { return !worker_active_; });
            return result<void>::failure(std::move(failure));
        }

        status_.state = stream_state::running;
        return result<void>::success();
    }

private:
    /// Основной capture loop для микрофона или системного loopback-захвата.
    void worker_main() {
        /// COM apartment worker thread-а.
        com_scope com;
        auto com_result = com.initialize_multithreaded();
        if (!com_result) {
            publish_startup_failure(com_result.error());
            return;
        }

        /// Временное повышение приоритета worker thread-а.
        mmcss_scope mmcss(options_.enable_mmcss);
        /// Дополнительные WASAPI-флаги, требуемые loopback-режиму.
        const DWORD stream_flags = loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        /// Набор ресурсов, необходимых для capture path.
        auto init_result = initialize_audio_client(
            config_.device,
            config_.format,
            config_.timing.target_latency,
            stream_flags);
        if (!init_result) {
            publish_startup_failure(init_result.error());
            return;
        }

        /// Capture service, через который считываются packet-ы с endpoint-а.
        ComPtr<IAudioCaptureClient> capture_client;
        HRESULT hr = init_result.value().audio_client->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(capture_client.GetAddressOf()));
        if (FAILED(hr)) {
            publish_startup_failure(map_hresult(
                "IAudioClient::GetService(IAudioCaptureClient)",
                hr,
                error_category::stream,
                error_code::stream_open_failed));
            return;
        }

        /// Silence storage is allocated before Start, never from the realtime packet loop.
        std::vector<std::byte> silent_buffer(
            static_cast<std::size_t>(init_result.value().buffer_frame_count) *
                init_result.value().format.block_align,
            std::byte{0});

        if (cancel_startup_if_requested("capture_stream::start")) {
            return;
        }

        hr = init_result.value().audio_client->Start();
        if (FAILED(hr)) {
            publish_startup_failure(map_hresult(
                "IAudioClient::Start",
                hr,
                error_category::stream,
                error_code::stream_start_failed));
            return;
        }

        bool cancelled_after_start = false;
        {
            std::scoped_lock lock(mutex_);
            if (stop_requested_ || closed_) {
                cancelled_after_start = true;
                set_startup_result_locked(make_invalid_state_error(
                    "capture_stream::start",
                    "Stream startup was cancelled by a concurrent stop or close."));
            } else {
                /// Публикует negotiated format только после успешного старта клиента.
                status_.negotiated_format = init_result.value().format.public_format;
                status_.state = stream_state::running;
                set_startup_result_locked();
            }
        }
        if (cancelled_after_start) {
            (void)init_result.value().audio_client->Stop();
            return;
        }

        /// События ожидания: ручная остановка и сигнал о готовом capture packet-е.
        HANDLE stop_event = nullptr;
        {
            std::scoped_lock lock(mutex_);
            stop_event = stop_event_locked();
        }
        const HANDLE wait_handles[2] = {stop_event, init_result.value().audio_event.get()};
        while (true) {
            const DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
            if (wait_result == WAIT_OBJECT_0) {
                break;
            }
            if (wait_result != WAIT_OBJECT_0 + 1) {
                record_runtime_error(make_invalid_state_error(
                    "WaitForMultipleObjects",
                    "Unexpected capture wait result."));
                break;
            }

            /// Число кадров, доступных в следующем packet-е.
            UINT32 packet_frames = 0;
            hr = capture_client->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) {
                record_runtime_error(map_hresult(
                    "IAudioCaptureClient::GetNextPacketSize",
                    hr,
                    error_category::stream,
                    error_code::stream_stop_failed,
                    is_device_lost_hresult(hr)));
                break;
            }

            while (packet_frames > 0) {
                /// Указатель на байты текущего capture packet-а.
                BYTE* data = nullptr;
                /// Число кадров в текущем packet-е.
                UINT32 frames = 0;
                /// Флаги packet-а, включая silence и discontinuity.
                DWORD flags = 0;
                /// Позиция устройства для packet-а.
                UINT64 device_position = 0;
                /// Временная метка QPC для packet-а.
                UINT64 qpc_position = 0;
                hr = capture_client->GetBuffer(
                    &data,
                    &frames,
                    &flags,
                    &device_position,
                    &qpc_position);
                if (FAILED(hr)) {
                    record_runtime_error(map_hresult(
                        "IAudioCaptureClient::GetBuffer",
                        hr,
                        error_category::stream,
                        error_code::stream_stop_failed,
                        is_device_lost_hresult(hr)));
                    packet_frames = 0;
                    break;
                }
                capture_buffer_scope buffer_scope(*capture_client.Get(), frames);

                /// Нормализованное представление байтов packet-а для callback-а.
                std::span<const std::byte> bytes;
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U ||
                    (config_.deliver_silence_on_glitch &&
                     (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0U)) {
                    const auto byte_count = checked_audio_byte_count(
                        frames, init_result.value().format.block_align);
                    if (!byte_count || *byte_count > silent_buffer.size()) {
                        (void)buffer_scope.release();
                        record_runtime_error(make_invalid_state_error(
                            "capture_stream::worker",
                            "Capture packet exceeded the negotiated endpoint buffer capacity."));
                        packet_frames = 0;
                        break;
                    }
                    bytes = std::span<const std::byte>(silent_buffer.data(), *byte_count);
                } else {
                    const auto byte_count = checked_audio_byte_count(
                        frames, init_result.value().format.block_align);
                    if (!byte_count) {
                        (void)buffer_scope.release();
                        record_runtime_error(make_invalid_state_error(
                            "capture_stream::worker",
                            "Capture buffer byte count exceeds the addressable range."));
                        packet_frames = 0;
                        break;
                    }
                    bytes = std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(data),
                        *byte_count);
                }

                /// Пользовательский callback получает пакет как неизменяемый audio buffer.
                result<void> callback_result = result<void>::success();
                try {
                    callback_result = callback_.on_capture(
                        const_audio_buffer_view{
                            bytes,
                            frames,
                            init_result.value().format.public_format,
                        },
                        stream_timestamp{device_position, qpc_position});
                } catch (const std::exception& exception) {
                    callback_result = result<void>::failure(make_callback_exception_error(
                        "capture_callback::on_capture", exception.what()));
                } catch (...) {
                    callback_result = result<void>::failure(make_callback_exception_error(
                        "capture_callback::on_capture", "Capture callback threw an unknown exception."));
                }
                hr = buffer_scope.release();
                if (FAILED(hr)) {
                    record_runtime_error(map_hresult(
                        "IAudioCaptureClient::ReleaseBuffer",
                        hr,
                        error_category::stream,
                        error_code::stream_stop_failed,
                        is_device_lost_hresult(hr)));
                    packet_frames = 0;
                    break;
                }
                if (!callback_result) {
                    record_runtime_error(callback_result.error());
                    packet_frames = 0;
                    break;
                }

                callback_count_.fetch_add(1, std::memory_order_relaxed);
                frames_processed_.fetch_add(frames, std::memory_order_relaxed);
                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0U) {
                    discontinuity_count_.fetch_add(1, std::memory_order_relaxed);
                }

                hr = capture_client->GetNextPacketSize(&packet_frames);
                if (FAILED(hr)) {
                    record_runtime_error(map_hresult(
                        "IAudioCaptureClient::GetNextPacketSize",
                        hr,
                        error_category::stream,
                        error_code::stream_stop_failed,
                        is_device_lost_hresult(hr)));
                    packet_frames = 0;
                }
            }
        }

        /// Завершает audio client и освобождает event handle перед выходом worker thread-а.
        init_result.value().audio_client->Stop();
        /// audio_event освобождается RAII после Stop и уничтожения service interface-ов.
    }

    /// Признак того, что поток работает в loopback-режиме.
    bool loopback_ = false;
};

}  // namespace

/// Открывает render stream только для render device selector-ов.
result<std::shared_ptr<stream_handle>> open_render_stream(
    const render_stream_config& config,
    render_callback& callback,
    runtime_options options) {
    if (config.device.direction != device_direction::render) {
        return result<std::shared_ptr<stream_handle>>::failure(make_invalid_state_error(
            "open_render_stream",
            "Render streams must target a render device selector."));
    }

    return result<std::shared_ptr<stream_handle>>::success(
        std::make_shared<wasapi_render_stream_handle>(config, callback, options));
}

/// Открывает обычный capture stream только для capture device selector-ов.
result<std::shared_ptr<stream_handle>> open_capture_stream(
    const capture_stream_config& config,
    capture_callback& callback,
    runtime_options options) {
    if (config.device.direction != device_direction::capture) {
        return result<std::shared_ptr<stream_handle>>::failure(make_invalid_state_error(
            "open_capture_stream",
            "Capture streams must target a capture device selector."));
    }

    return result<std::shared_ptr<stream_handle>>::success(
        std::make_shared<wasapi_capture_stream_handle>(config, callback, options, false));
}

/// Открывает loopback capture stream поверх render endpoint-а.
result<std::shared_ptr<stream_handle>> open_loopback_stream(
    const loopback_stream_config& config,
    capture_callback& callback,
    runtime_options options) {
    if (config.device.direction != device_direction::render) {
        return result<std::shared_ptr<stream_handle>>::failure(make_invalid_state_error(
            "open_loopback_stream",
            "Loopback streams must target a render device selector."));
    }

    /// Внутренняя capture-конфигурация, соответствующая публичному loopback API.
    capture_stream_config capture_config;
    capture_config.device = config.device;
    capture_config.mode = config.mode;
    capture_config.callback = config.callback;
    capture_config.format = config.format;
    capture_config.timing = config.timing;
    capture_config.auto_recover_device_loss = config.auto_recover_device_loss;

    return result<std::shared_ptr<stream_handle>>::success(
        std::make_shared<wasapi_capture_stream_handle>(capture_config, callback, options, true));
}

}  // namespace sonotide::detail::win
