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
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "internal/state_machine.h"
#include "internal/win/com_scope.h"
#include "internal/win/device_utils.h"
#include "internal/win/hresult_utils.h"
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
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<std::uint64_t>((now.QuadPart * 10000000LL) / frequency.QuadPart);
}

/// Переводит миллисекундную длительность в формат `REFERENCE_TIME`.
REFERENCE_TIME to_reference_time(const std::chrono::milliseconds duration) {
    return static_cast<REFERENCE_TIME>(duration.count()) * 10000;
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

/// Проверяет, относится ли HRESULT к потере WASAPI-устройства.
bool is_device_lost_hresult(const HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED ||
           hr == AUDCLNT_E_RESOURCES_INVALIDATED;
}

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
class wasapi_stream_handle_base : public stream_handle {
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
        close();
    }

    /// Останавливает worker thread и переводит поток в stopped, если это допустимо.
    result<void> stop() override {
        std::unique_lock lock(mutex_);
        if (closed_) {
            return result<void>::failure(
                make_invalid_state_error("stream::stop", "Stream is already closed."));
        }

        if (status_.state == stream_state::prepared || status_.state == stream_state::stopped) {
            return result<void>::success();
        }

        if (status_.state != stream_state::running && status_.state != stream_state::faulted) {
            return result<void>::failure(
                make_invalid_state_error("stream::stop", "Stream is not running."));
        }

        if (status_.state == stream_state::running) {
            auto transition = state_machine_.transition(stream_transition::stop);
            if (!transition) {
                return result<void>::failure(transition.error());
            }
        }

        /// Флагизирует необходимость остановки worker thread.
        stop_requested_ = true;
        /// Локальная копия event handle-а, используемая после выхода из lock.
        HANDLE stop_event = stop_event_;
        lock.unlock();

        if (stop_event != nullptr) {
            SetEvent(stop_event);
        }

        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        lock.lock();
        cleanup_stop_event_locked();
        if (!closed_ && status_.state != stream_state::faulted) {
            status_.state = stream_state::stopped;
        }
        return result<void>::success();
    }

    /// Возвращает поток в prepared и очищает negotiated runtime state.
    result<void> reset() override {
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
        status_.device_lost = false;
        return result<void>::success();
    }

    /// Полностью закрывает поток и освобождает связанные event-ресурсы.
    result<void> close() override {
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return result<void>::success();
            }
        }

        auto stopped = stop();
        if (!stopped && stopped.error().code != error_code::invalid_state) {
            return stopped;
        }

        std::scoped_lock lock(mutex_);
        closed_ = true;
        (void)state_machine_.transition(stream_transition::close);
        status_.state = stream_state::closed;
        status_.negotiated_format.reset();
        cleanup_stop_event_locked();
        return result<void>::success();
    }

    /// Возвращает потокобезопасный снимок текущего состояния потока.
    [[nodiscard]] stream_status status() const override {
        std::scoped_lock lock(mutex_);
        return status_;
    }

protected:
    /// Создаёт stop event для внешней остановки worker thread.
    [[nodiscard]] HANDLE create_stop_event() {
        return CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    /// Закрывает stop event внутри уже захваченной критической секции.
    void cleanup_stop_event_locked() {
        if (stop_event_ != nullptr) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
    }

    /// Фиксирует итог startup-пути и будит ожидающий вызывающий поток.
    void set_startup_result_locked(std::optional<error> failure = std::nullopt) {
        startup_error_ = std::move(failure);
        startup_completed_ = true;
        startup_cv_.notify_one();
    }

    /// Помечает поток как faulted и пересылает ошибку пользовательскому callback-у.
    void record_runtime_error(error failure) {
        {
            std::scoped_lock lock(mutex_);
            if (is_device_lost_hresult(static_cast<HRESULT>(failure.native_code.value_or(0)))) {
                status_.device_lost = true;
            }
            (void)state_machine_.transition(stream_transition::fault);
            status_.state = stream_state::faulted;
        }
        callback_.on_stream_error(failure);
    }

    /// Сериализует lifecycle-операции и доступ к status.
    mutable std::mutex mutex_;
    /// Сигнализирует завершение startup-пути worker thread.
    std::condition_variable startup_cv_;
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
    /// Рабочий поток, обслуживающий цикл WASAPI.
    std::thread worker_thread_;
    /// Событие остановки worker thread.
    HANDLE stop_event_ = nullptr;
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
    HANDLE audio_event = nullptr;
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
    const HANDLE audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (audio_event == nullptr) {
        error failure;
        failure.category = error_category::platform;
        failure.code = error_code::platform_failure;
        failure.operation = "CreateEventW";
        failure.message = "Failed to allocate stream event handle.";
        return result<client_init_result>::failure(std::move(failure));
    }

    hr = audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        stream_flags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        to_reference_time(target_latency),
        0,
        format_result.value().wave_format.get(),
        nullptr);
    if (FAILED(hr)) {
        CloseHandle(audio_event);
        return result<client_init_result>::failure(map_hresult(
            "IAudioClient::Initialize",
            hr,
            error_category::initialization,
            error_code::stream_open_failed));
    }

    hr = audio_client->SetEventHandle(audio_event);
    if (FAILED(hr)) {
        CloseHandle(audio_event);
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
        CloseHandle(audio_event);
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
    result_value.audio_event = audio_event;
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

        if (config_.mode != share_mode::shared) {
            return result<void>::failure(make_not_implemented_error(
                "render_stream::start",
                "Exclusive mode is intentionally deferred until shared-mode coverage is stable."));
        }

        auto transition = state_machine_.transition(stream_transition::start);
        if (!transition) {
            return result<void>::failure(transition.error());
        }

        /// Сбрасывает startup synchronization перед новым запуском.
        stop_requested_ = false;
        startup_completed_ = false;
        startup_error_.reset();
        stop_event_ = create_stop_event();
        if (stop_event_ == nullptr) {
            return result<void>::failure(make_invalid_state_error(
                "render_stream::start",
                "Failed to allocate stop event."));
        }

        /// Поднимает worker thread и ждёт его сигнала о завершении startup.
        worker_thread_ = std::thread([this]() { worker_main(); });
        startup_cv_.wait(lock, [this]() { return startup_completed_; });

        if (startup_error_) {
            /// Ошибка старта требует join и перевода потока в faulted.
            auto failure = *startup_error_;
            lock.unlock();
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
            lock.lock();
            (void)state_machine_.transition(stream_transition::fault);
            status_.state = stream_state::faulted;
            cleanup_stop_event_locked();
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
            std::scoped_lock lock(mutex_);
            set_startup_result_locked(com_result.error());
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
            std::scoped_lock lock(mutex_);
            set_startup_result_locked(init_result.error());
            return;
        }

        /// Render service, выдающий writable endpoint buffer.
        ComPtr<IAudioRenderClient> render_client;
        HRESULT hr = init_result.value().audio_client->GetService(
            __uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(render_client.GetAddressOf()));
        if (FAILED(hr)) {
            std::scoped_lock lock(mutex_);
            set_startup_result_locked(map_hresult(
                "IAudioClient::GetService(IAudioRenderClient)",
                hr,
                error_category::stream,
                error_code::stream_open_failed));
            CloseHandle(init_result.value().audio_event);
            return;
        }

        /// Начальный priming buffer перед запуском audio client-а.
        BYTE* initial_buffer = nullptr;
        hr = render_client->GetBuffer(init_result.value().buffer_frame_count, &initial_buffer);
        if (FAILED(hr)) {
            std::scoped_lock lock(mutex_);
            set_startup_result_locked(map_hresult(
                "IAudioRenderClient::GetBuffer",
                hr,
                error_category::stream,
                error_code::stream_open_failed));
            CloseHandle(init_result.value().audio_event);
            return;
        }

        /// Публичное представление стартового буфера для пользовательского callback-а.
        audio_buffer_view initial_view{
            std::span<std::byte>(
                reinterpret_cast<std::byte*>(initial_buffer),
                init_result.value().buffer_frame_count * init_result.value().format.block_align),
            init_result.value().buffer_frame_count,
            init_result.value().format.public_format,
        };
        /// Первый callback заполняет priming buffer до старта потока.
        auto initial_callback = callback_.on_render(
            initial_view,
            stream_timestamp{0, current_qpc_100ns()});
        if (!initial_callback) {
            render_client->ReleaseBuffer(
                init_result.value().buffer_frame_count,
                AUDCLNT_BUFFERFLAGS_SILENT);
            std::scoped_lock lock(mutex_);
            set_startup_result_locked(initial_callback.error());
            CloseHandle(init_result.value().audio_event);
            return;
        }

        render_client->ReleaseBuffer(init_result.value().buffer_frame_count, 0);

        /// Запускает consumption endpoint buffer со стороны WASAPI engine.
        hr = init_result.value().audio_client->Start();
        if (FAILED(hr)) {
            std::scoped_lock lock(mutex_);
            set_startup_result_locked(map_hresult(
                "IAudioClient::Start",
                hr,
                error_category::stream,
                error_code::stream_start_failed));
            CloseHandle(init_result.value().audio_event);
            return;
        }

        {
            std::scoped_lock lock(mutex_);
            /// Публикует negotiated format только после успешного старта.
            status_.negotiated_format = init_result.value().format.public_format;
            status_.state = stream_state::running;
            set_startup_result_locked();
        }

        /// События ожидания: ручная остановка и очередной сигнал от WASAPI.
        const HANDLE wait_handles[2] = {stop_event_, init_result.value().audio_event};
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

            /// Публичное представление render buffer для callback-а.
            audio_buffer_view view{
                std::span<std::byte>(
                    reinterpret_cast<std::byte*>(buffer),
                    frames_available * init_result.value().format.block_align),
                frames_available,
                init_result.value().format.public_format,
            };
            /// Пользовательский callback пишет PCM непосредственно в endpoint buffer.
            auto callback_result = callback_.on_render(
                view,
                stream_timestamp{frames_written, current_qpc_100ns()});
            if (!callback_result) {
                render_client->ReleaseBuffer(frames_available, AUDCLNT_BUFFERFLAGS_SILENT);
                record_runtime_error(callback_result.error());
                break;
            }

            hr = render_client->ReleaseBuffer(frames_available, 0);
            if (FAILED(hr)) {
                record_runtime_error(map_hresult(
                    "IAudioRenderClient::ReleaseBuffer",
                    hr,
                    error_category::stream,
                    error_code::stream_stop_failed,
                    is_device_lost_hresult(hr)));
                break;
            }

            /// Обновляет статистику только после успешного release буфера.
            std::scoped_lock lock(mutex_);
            frames_written += frames_available;
            status_.statistics.callback_count += 1;
            status_.statistics.frames_processed += frames_available;
        }

        /// Корректно завершает audio client и освобождает event handle.
        init_result.value().audio_client->Stop();
        CloseHandle(init_result.value().audio_event);

        std::scoped_lock lock(mutex_);
        if (!closed_ && status_.state != stream_state::faulted) {
            status_.state = stream_state::stopped;
        }
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

        if (config_.mode != share_mode::shared) {
            return result<void>::failure(make_not_implemented_error(
                "capture_stream::start",
                "Exclusive mode is intentionally deferred until shared-mode coverage is stable."));
        }

        auto transition = state_machine_.transition(stream_transition::start);
        if (!transition) {
            return result<void>::failure(transition.error());
        }

        /// Сбрасывает startup synchronization перед новым запуском.
        stop_requested_ = false;
        startup_completed_ = false;
        startup_error_.reset();
        stop_event_ = create_stop_event();
        if (stop_event_ == nullptr) {
            return result<void>::failure(make_invalid_state_error(
                "capture_stream::start",
                "Failed to allocate stop event."));
        }

        /// Поднимает worker thread и ждёт его сигнала о завершении startup.
        worker_thread_ = std::thread([this]() { worker_main(); });
        startup_cv_.wait(lock, [this]() { return startup_completed_; });

        if (startup_error_) {
            /// Ошибка старта требует join и перевода потока в faulted.
            auto failure = *startup_error_;
            lock.unlock();
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
            lock.lock();
            (void)state_machine_.transition(stream_transition::fault);
            status_.state = stream_state::faulted;
            cleanup_stop_event_locked();
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
            std::scoped_lock lock(mutex_);
            set_startup_result_locked(com_result.error());
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
            std::scoped_lock lock(mutex_);
            set_startup_result_locked(init_result.error());
            return;
        }

        /// Capture service, через который считываются packet-ы с endpoint-а.
        ComPtr<IAudioCaptureClient> capture_client;
        HRESULT hr = init_result.value().audio_client->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(capture_client.GetAddressOf()));
        if (FAILED(hr)) {
            std::scoped_lock lock(mutex_);
            set_startup_result_locked(map_hresult(
                "IAudioClient::GetService(IAudioCaptureClient)",
                hr,
                error_category::stream,
                error_code::stream_open_failed));
            CloseHandle(init_result.value().audio_event);
            return;
        }

        hr = init_result.value().audio_client->Start();
        if (FAILED(hr)) {
            std::scoped_lock lock(mutex_);
            set_startup_result_locked(map_hresult(
                "IAudioClient::Start",
                hr,
                error_category::stream,
                error_code::stream_start_failed));
            CloseHandle(init_result.value().audio_event);
            return;
        }

        {
            std::scoped_lock lock(mutex_);
            /// Публикует negotiated format только после успешного старта клиента.
            status_.negotiated_format = init_result.value().format.public_format;
            status_.state = stream_state::running;
            set_startup_result_locked();
        }

        /// События ожидания: ручная остановка и сигнал о готовом capture packet-е.
        const HANDLE wait_handles[2] = {stop_event_, init_result.value().audio_event};
        /// Буфер тишины для packet-ов, помеченных WASAPI как silent.
        std::vector<std::byte> silent_buffer;
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

                /// Нормализованное представление байтов packet-а для callback-а.
                std::span<const std::byte> bytes;
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U) {
                    silent_buffer.assign(frames * init_result.value().format.block_align, std::byte{0});
                    bytes = std::span<const std::byte>(silent_buffer.data(), silent_buffer.size());
                } else {
                    bytes = std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(data),
                        frames * init_result.value().format.block_align);
                }

                /// Пользовательский callback получает пакет как неизменяемый audio buffer.
                auto callback_result = callback_.on_capture(
                    const_audio_buffer_view{
                        bytes,
                        frames,
                        init_result.value().format.public_format,
                    },
                    stream_timestamp{device_position, qpc_position});
                capture_client->ReleaseBuffer(frames);
                if (!callback_result) {
                    record_runtime_error(callback_result.error());
                    packet_frames = 0;
                    break;
                }

                {
                    /// Обновляет внутреннюю статистику по факту успешной доставки packet-а.
                    std::scoped_lock lock(mutex_);
                    status_.statistics.callback_count += 1;
                    status_.statistics.frames_processed += frames;
                    if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0U) {
                        status_.statistics.discontinuity_count += 1;
                    }
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
        CloseHandle(init_result.value().audio_event);

        std::scoped_lock lock(mutex_);
        if (!closed_ && status_.state != stream_state::faulted) {
            status_.state = stream_state::stopped;
        }
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
