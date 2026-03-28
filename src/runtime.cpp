#include "sonotide/runtime.h"
#include "sonotide/playback_session.h"

#include <memory>
#include <utility>

#include "internal/runtime_backend.h"

namespace sonotide {

// Привязывает публичный объект среды выполнения к конкретной внутренней реализации, которая владеет платформенными ресурсами.
runtime::runtime(std::shared_ptr<detail::runtime_backend> backend) noexcept
    : backend_(std::move(backend)) {}

// Создаёт экземпляр среды выполнения после инициализации платформенного слоя.
result<runtime> runtime::create(runtime_options options) {
    // Создание внутренней реализации вынесено отдельно, чтобы публичный runtime оставался тонкой фасадной обёрткой.
    auto backend_result = detail::make_runtime_backend(options);
    if (!backend_result) {
        return result<runtime>::failure(backend_result.error());
    }

    return result<runtime>::success(runtime(std::move(backend_result.value())));
}

// Передаёт перечисление устройств во внутреннюю реализацию и сохраняет результат в обёртке `result`.
result<std::vector<device_info>> runtime::enumerate_devices(const device_direction direction) const {
    return backend_->enumerate_devices(direction);
}

// Возвращает устройство по умолчанию для указанного направления и роли.
result<device_info> runtime::default_device(
    const device_direction direction,
    const device_role role) const {
    return backend_->default_device(direction, role);
}

// Открывает поток рендеринга и упаковывает объект внутренней реализации в публичный RAII-тип.
result<render_stream> runtime::open_render_stream(
    const render_stream_config& config,
    render_callback& callback) {
    // Создание дескриптора остаётся во внутренней реализации, чтобы этот слой только собирал публичные объекты.
    auto handle_result = backend_->open_render_stream(config, callback);
    if (!handle_result) {
        return result<render_stream>::failure(handle_result.error());
    }

    return result<render_stream>::success(detail::make_render_stream(std::move(handle_result.value())));
}

// Открывает поток захвата и упаковывает объект внутренней реализации в публичный RAII-тип.
result<capture_stream> runtime::open_capture_stream(
    const capture_stream_config& config,
    capture_callback& callback) {
    // У захвата тот же паттерн владения, что и у рендеринга: сначала дескриптор внутренней реализации, затем обёртка.
    auto handle_result = backend_->open_capture_stream(config, callback);
    if (!handle_result) {
        return result<capture_stream>::failure(handle_result.error());
    }

    return result<capture_stream>::success(detail::make_capture_stream(std::move(handle_result.value())));
}

// Открывает поток loopback-захвата, который слушает выходное устройство рендеринга.
result<loopback_capture_stream> runtime::open_loopback_stream(
    const loopback_stream_config& config,
    capture_callback& callback) {
    auto handle_result = backend_->open_loopback_stream(config, callback);
    if (!handle_result) {
        return result<loopback_capture_stream>::failure(handle_result.error());
    }

    return result<loopback_capture_stream>::success(
        detail::make_loopback_capture_stream(std::move(handle_result.value())));
}

// Собирает высокоуровневую сессию воспроизведения поверх той же внутренней реализации и времени жизни среды выполнения.
result<playback_session> runtime::open_playback_session(const playback_session_config& config) {
    return playback_session::create(backend_, config);
}

}  // namespace sonotide
